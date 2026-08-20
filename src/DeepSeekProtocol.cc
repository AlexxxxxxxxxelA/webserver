#include "DeepSeekProtocol.h"

#include <stdexcept>

namespace
{

/*
 * Provider usage 是请求完成后的观测值，与 ContextBuilder 请求前的启发式 Token 估算
 * 来源不同。字段缺失按 0 只表示“Provider 没有提供”，不证明真实消耗一定为 0。
 */
uint64_t optionalUsageValue(const nlohmann::json &object, const char *key)
{
    if (!object.contains(key) || object[key].is_null())
    {
        return 0;
    }
    if (!object[key].is_number_integer() || object[key].get<long long>() < 0)
    {
        throw std::runtime_error("invalid DeepSeek usage value");
    }
    const uint64_t value = object[key].get<uint64_t>();
    // 单次请求不可能合理消耗十亿 Token；上限也保证最多 6 次聚合不会 uint64 回绕。
    if (value > 1000000000ULL)
    {
        throw std::runtime_error("DeepSeek usage value is unreasonably large");
    }
    return value;
}

} // namespace

nlohmann::json buildDeepSeekChatRequest(
    const std::string &model,
    const std::vector<nlohmann::json> &messages,
    const nlohmann::json &tools,
    bool thinkingEnabled)
{
    nlohmann::json payload;
    payload["model"] = model;
    payload["stream"] = false;
    payload["messages"] = messages;

    // 显式发送开关，不依赖 Provider 默认值，便于配置回退和 Enabled/Disabled 测试。
    payload["thinking"] = {
        {"type", thinkingEnabled ? "enabled" : "disabled"}
    };
    if (!tools.empty())
    {
        payload["tools"] = tools;
        payload["tool_choice"] = "auto";
    }
    return payload;
}

nlohmann::json buildDeepSeekStreamingRequest(
    const std::string &model,
    const std::vector<nlohmann::json> &messages,
    const nlohmann::json &tools,
    bool thinkingEnabled)
{
    nlohmann::json payload = buildDeepSeekChatRequest(
        model, messages, tools, thinkingEnabled);
    payload["stream"] = true;
    payload["stream_options"] = {{"include_usage", true}};
    return payload;
}

DeepSeekSseParser::DeepSeekSseParser(
    const AgentModelClient::TextDeltaCallback &onDelta,
    const AgentModelClient::ThinkingStateCallback &onThinking)
    : onDelta_(onDelta)
    , onThinking_(onThinking)
    , dataBytes_(0)
    , done_(false)
    , failed_(false)
    , thinkingStarted_(false)
    , thinkingCompleted_(false)
    , answerOrToolStarted_(false)
{
}

bool DeepSeekSseParser::feed(const char *data, size_t length, std::string *error)
{
    if (failed_ || done_)
    {
        if (done_ && !failed_)
        {
            failed_ = true;
            errorMessage_ = "data received after DeepSeek [DONE]";
        }
        if (error)
        {
            *error = errorMessage_;
        }
        return false;
    }
    pendingLine_.append(data, length);
    const size_t kMaxPendingLineBytes = 1024 * 1024;
    if (pendingLine_.size() > kMaxPendingLineBytes)
    {
        failed_ = true;
        errorMessage_ = "DeepSeek SSE line exceeded 1 MiB";
        if (error) *error = errorMessage_;
        return false;
    }

    size_t newline = pendingLine_.find('\n');
    while (newline != std::string::npos)
    {
        std::string line = pendingLine_.substr(0, newline);
        pendingLine_.erase(0, newline + 1);
        if (!line.empty() && line[line.size() - 1] == '\r')
        {
            line.erase(line.size() - 1);
        }
        if (!processLine(line, error))
        {
            return false;
        }
        newline = pendingLine_.find('\n');
    }
    return true;
}

bool DeepSeekSseParser::processLine(const std::string &line, std::string *error)
{
    if (done_)
    {
        failed_ = true;
        errorMessage_ = "SSE line received after DeepSeek [DONE]";
        if (error) *error = errorMessage_;
        return false;
    }
    if (line.empty())
    {
        return processEvent(error);
    }
    if (line[0] == ':')
    {
        return true; // SSE comment/heartbeat
    }
    if (line.compare(0, 5, "data:") == 0)
    {
        size_t begin = 5;
        if (begin < line.size() && line[begin] == ' ')
        {
            ++begin;
        }
        const std::string value = line.substr(begin);
        const size_t kMaxEventBytes = 1024 * 1024;
        if (dataBytes_ + value.size() + 1 > kMaxEventBytes)
        {
            failed_ = true;
            errorMessage_ = "DeepSeek SSE event exceeded 1 MiB";
            if (error) *error = errorMessage_;
            return false;
        }
        dataBytes_ += value.size() + 1;
        dataLines_.push_back(value);
    }
    // id/event/retry 字段不是 DeepSeek 数据载荷，安全忽略。
    return true;
}

bool DeepSeekSseParser::processEvent(std::string *error)
{
    if (dataLines_.empty())
    {
        return true;
    }
    std::string data;
    for (size_t i = 0; i < dataLines_.size(); ++i)
    {
        if (i > 0) data += '\n';
        data += dataLines_[i];
    }
    dataLines_.clear();
    dataBytes_ = 0;

    if (data == "[DONE]")
    {
        done_ = true;
        return true;
    }
    try
    {
        return processJsonEvent(nlohmann::json::parse(data), error);
    }
    catch (const std::exception &)
    {
        failed_ = true;
        errorMessage_ = "failed to parse DeepSeek SSE JSON";
        if (error) *error = errorMessage_;
        return false;
    }
}

bool DeepSeekSseParser::processJsonEvent(const nlohmann::json &event,
                                         std::string *error)
{
    if (event.contains("usage") && !event["usage"].is_null())
    {
        const nlohmann::json &usage = event["usage"];
        if (!usage.is_object())
        {
            throw std::runtime_error("DeepSeek stream usage must be an object");
        }
        response_.usage.promptTokens = optionalUsageValue(usage, "prompt_tokens");
        response_.usage.completionTokens = optionalUsageValue(usage, "completion_tokens");
        response_.usage.totalTokens = optionalUsageValue(usage, "total_tokens");
        response_.usage.promptCacheHitTokens =
            optionalUsageValue(usage, "prompt_cache_hit_tokens");
        response_.usage.promptCacheMissTokens =
            optionalUsageValue(usage, "prompt_cache_miss_tokens");
        if (usage.contains("completion_tokens_details") &&
            !usage["completion_tokens_details"].is_null())
        {
            if (!usage["completion_tokens_details"].is_object())
            {
                throw std::runtime_error("stream completion token details must be an object");
            }
            response_.usage.reasoningTokens = optionalUsageValue(
                usage["completion_tokens_details"], "reasoning_tokens");
        }
    }

    if (!event.contains("choices") || !event["choices"].is_array() ||
        event["choices"].empty())
    {
        return true; // include_usage 的最后一块允许 choices=[]
    }
    const nlohmann::json &choice = event["choices"][0];
    if (choice.contains("finish_reason") && choice["finish_reason"].is_string())
    {
        finishReason_ = choice["finish_reason"].get<std::string>();
    }
    if (!choice.contains("delta") || !choice["delta"].is_object())
    {
        return true;
    }
    const nlohmann::json &delta = choice["delta"];
    if (delta.contains("reasoning_content") &&
        !delta["reasoning_content"].is_null())
    {
        if (!delta["reasoning_content"].is_string())
        {
            throw std::runtime_error("streamed reasoning_content must be a string");
        }
        const std::string reasoning = delta["reasoning_content"].get<std::string>();
        if (!reasoning.empty() && (thinkingCompleted_ || answerOrToolStarted_))
        {
            throw std::runtime_error(
                "reasoning_content arrived after thinking was completed");
        }
        if (response_.reasoningContent.size() + reasoning.size() > 1024 * 1024)
        {
            throw std::runtime_error("DeepSeek streamed reasoning exceeded 1 MiB");
        }
        response_.hasReasoningContent = true;
        response_.reasoningContent += reasoning;
        if (!reasoning.empty() && !thinkingStarted_)
        {
            thinkingStarted_ = true;
            if (onThinking_ && !onThinking_(true))
            {
                failed_ = true;
                errorMessage_ = "agent stream was cancelled";
                response_.error = AgentModelResponse::kCancelled;
                response_.errorMessage = errorMessage_;
                if (error) *error = errorMessage_;
                return false;
            }
        }
    }
    if (delta.contains("content") && delta["content"].is_string())
    {
        // 一个 delta 可能包含半句话或多个模型 Token；它只是 Provider 的传输增量。
        const std::string text = delta["content"].get<std::string>();
        if (!text.empty() && !completeThinking(error))
        {
            return false;
        }
        if (!text.empty()) answerOrToolStarted_ = true;
        if (response_.content.size() + text.size() > 1024 * 1024)
        {
            throw std::runtime_error("DeepSeek streamed content exceeded 1 MiB");
        }
        response_.content += text;
        if (!text.empty() && onDelta_ && !onDelta_(text))
        {
            failed_ = true;
            errorMessage_ = "agent stream was cancelled";
            response_.error = AgentModelResponse::kCancelled;
            response_.errorMessage = errorMessage_;
            if (error) *error = errorMessage_;
            return false;
        }
    }
    if (delta.contains("tool_calls") && delta["tool_calls"].is_array())
    {
        if (!delta["tool_calls"].empty() && !completeThinking(error))
        {
            return false;
        }
        if (!delta["tool_calls"].empty()) answerOrToolStarted_ = true;
        for (size_t i = 0; i < delta["tool_calls"].size(); ++i)
        {
            const nlohmann::json &fragment = delta["tool_calls"][i];
            const size_t index = fragment.at("index").get<size_t>();
            if (index >= 128)
            {
                throw std::runtime_error("too many streamed tool calls");
            }
            if (response_.toolCalls.size() <= index)
            {
                response_.toolCalls.resize(index + 1);
            }
            AgentToolCall &call = response_.toolCalls[index];
            if (fragment.contains("id") && fragment["id"].is_string())
            {
                call.toolCallId += fragment["id"].get<std::string>();
            }
            if (fragment.contains("type") && fragment["type"].is_string() &&
                fragment["type"].get<std::string>() != "function")
            {
                throw std::runtime_error("unsupported streamed tool call type");
            }
            if (fragment.contains("function") && fragment["function"].is_object())
            {
                const nlohmann::json &function = fragment["function"];
                if (function.contains("name") && function["name"].is_string())
                {
                    call.name += function["name"].get<std::string>();
                }
                if (function.contains("arguments") && function["arguments"].is_string())
                {
                    call.argumentsJson += function["arguments"].get<std::string>();
                }
            }
            if (call.argumentsJson.size() > 16 * 1024 || call.name.size() > 64 ||
                call.toolCallId.size() > 256)
            {
                throw std::runtime_error("streamed tool call exceeded limits");
            }
        }
    }
    if (!finishReason_.empty() && !completeThinking(error))
    {
        return false;
    }
    return true;
}

bool DeepSeekSseParser::completeThinking(std::string *error)
{
    if (!thinkingStarted_ || thinkingCompleted_)
    {
        return true;
    }
    thinkingCompleted_ = true;
    if (onThinking_ && !onThinking_(false))
    {
        failed_ = true;
        errorMessage_ = "agent stream was cancelled";
        response_.error = AgentModelResponse::kCancelled;
        response_.errorMessage = errorMessage_;
        if (error) *error = errorMessage_;
        return false;
    }
    return true;
}

AgentModelResponse DeepSeekSseParser::finish()
{
    if (failed_)
    {
        /*
         * 即使 Provider 在 reasoning 之后返回损坏 JSON/超限数据，也要闭合 UI 状态。
         * Completed 表示“本次思考阶段结束”，不表示 Model Call 成功；随后仍会发送
         * model.completed(ok=false) 和 error。
         */
        if (thinkingStarted_ && !thinkingCompleted_)
        {
            std::string ignored;
            completeThinking(&ignored);
        }
        if (response_.error == AgentModelResponse::kNone)
        {
            response_.error = AgentModelResponse::kInvalidResponse;
            response_.errorMessage = errorMessage_;
        }
        return response_;
    }
    std::string thinkingError;
    if (!completeThinking(&thinkingError))
    {
        return response_;
    }
    if (!pendingLine_.empty() || !dataLines_.empty() || !done_)
    {
        response_.error = AgentModelResponse::kInvalidResponse;
        response_.errorMessage = "DeepSeek SSE stream ended before [DONE]";
        return response_;
    }
    if (finishReason_ == "tool_calls")
    {
        if (response_.toolCalls.empty())
        {
            response_.error = AgentModelResponse::kInvalidResponse;
            response_.errorMessage = "stream ended with tool_calls but no calls";
        }
        for (size_t i = 0; i < response_.toolCalls.size(); ++i)
        {
            if (response_.toolCalls[i].toolCallId.empty() ||
                response_.toolCalls[i].name.empty())
            {
                response_.error = AgentModelResponse::kInvalidResponse;
                response_.errorMessage = "streamed tool call is incomplete";
            }
        }
    }
    else if (finishReason_ == "stop")
    {
        if (response_.content.empty() || !response_.toolCalls.empty())
        {
            response_.error = AgentModelResponse::kInvalidResponse;
            response_.errorMessage = "streamed final answer is incomplete";
        }
    }
    else if (finishReason_ == "length")
    {
        response_.error = AgentModelResponse::kInvalidResponse;
        response_.errorMessage = "DeepSeek streamed response was truncated";
    }
    else if (finishReason_ == "content_filter")
    {
        response_.error = AgentModelResponse::kRejectedRequest;
        response_.errorMessage = "DeepSeek streamed response was filtered";
    }
    else if (finishReason_ == "insufficient_system_resource")
    {
        response_.error = AgentModelResponse::kUnavailable;
        response_.errorMessage = "DeepSeek stream lacked system resources";
    }
    else
    {
        response_.error = AgentModelResponse::kInvalidResponse;
        response_.errorMessage = "DeepSeek SSE stream has no valid finish reason";
    }
    return response_;
}

AgentModelResponse parseDeepSeekChatResponse(const std::string &body)
{
    AgentModelResponse result;
    try
    {
        const nlohmann::json responseJson = nlohmann::json::parse(body);
        const nlohmann::json &choice = responseJson.at("choices").at(0);
        const std::string finishReason = choice.at("finish_reason").get<std::string>();
        const nlohmann::json &message = choice.at("message");
        if (message.at("role").get<std::string>() != "assistant")
        {
            throw std::runtime_error("DeepSeek message role is not assistant");
        }

        if (message.contains("reasoning_content") &&
            !message["reasoning_content"].is_null())
        {
            if (!message["reasoning_content"].is_string())
            {
                throw std::runtime_error("reasoning_content must be a string");
            }
            result.reasoningContent =
                message["reasoning_content"].get<std::string>();
            if (result.reasoningContent.size() > 1024 * 1024)
            {
                throw std::runtime_error("reasoning_content exceeded 1 MiB");
            }
            result.hasReasoningContent = true;
        }

        if (message.contains("content") && message["content"].is_string())
        {
            result.content = message["content"].get<std::string>();
            if (result.content.size() > 1024 * 1024)
            {
                throw std::runtime_error("DeepSeek content exceeded 1 MiB");
            }
        }
        if (message.contains("tool_calls") && !message["tool_calls"].is_null())
        {
            if (!message["tool_calls"].is_array())
            {
                throw std::runtime_error("tool_calls must be an array");
            }
            if (message["tool_calls"].size() > 128)
            {
                throw std::runtime_error("too many DeepSeek tool calls");
            }
            for (size_t i = 0; i < message["tool_calls"].size(); ++i)
            {
                const nlohmann::json &toolCallJson = message["tool_calls"][i];
                if (toolCallJson.at("type").get<std::string>() != "function")
                {
                    throw std::runtime_error("unsupported DeepSeek tool call type");
                }
                AgentToolCall call;
                call.toolCallId = toolCallJson.at("id").get<std::string>();
                call.name = toolCallJson.at("function").at("name").get<std::string>();
                call.argumentsJson =
                    toolCallJson.at("function").at("arguments").get<std::string>();
                if (call.toolCallId.empty() || call.name.empty())
                {
                    throw std::runtime_error("tool call id and name must not be empty");
                }
                result.toolCalls.push_back(call);
            }
        }

        /*
         * finish_reason 是 Provider 对“为什么停止”的权威描述。不能在 length 时把
         * 截断文本当作完整答案，也不能在 stop 时悄悄执行一个结构不一致的工具调用。
         */
        if (finishReason == "tool_calls")
        {
            if (result.toolCalls.empty())
            {
                throw std::runtime_error("finish_reason=tool_calls without tool calls");
            }
        }
        else if (finishReason == "stop")
        {
            if (!result.toolCalls.empty() || result.content.empty())
            {
                throw std::runtime_error("finish_reason=stop has inconsistent message");
            }
        }
        else if (finishReason == "insufficient_system_resource")
        {
            result.error = AgentModelResponse::kUnavailable;
            result.errorMessage = "DeepSeek stopped because of insufficient system resources";
        }
        else if (finishReason == "length")
        {
            result.error = AgentModelResponse::kInvalidResponse;
            result.errorMessage = "DeepSeek response was truncated by the token limit";
        }
        else if (finishReason == "content_filter")
        {
            result.error = AgentModelResponse::kRejectedRequest;
            result.errorMessage = "DeepSeek response was blocked by content filtering";
        }
        else
        {
            throw std::runtime_error("unknown DeepSeek finish_reason");
        }

        if (responseJson.contains("usage") && !responseJson["usage"].is_null())
        {
            const nlohmann::json &usage = responseJson["usage"];
            if (!usage.is_object())
            {
                throw std::runtime_error("DeepSeek usage must be an object");
            }
            result.usage.promptTokens = optionalUsageValue(usage, "prompt_tokens");
            result.usage.completionTokens = optionalUsageValue(usage, "completion_tokens");
            result.usage.totalTokens = optionalUsageValue(usage, "total_tokens");
            result.usage.promptCacheHitTokens =
                optionalUsageValue(usage, "prompt_cache_hit_tokens");
            result.usage.promptCacheMissTokens =
                optionalUsageValue(usage, "prompt_cache_miss_tokens");
            if (usage.contains("completion_tokens_details") &&
                !usage["completion_tokens_details"].is_null())
            {
                if (!usage["completion_tokens_details"].is_object())
                {
                    throw std::runtime_error(
                        "DeepSeek completion_tokens_details must be an object");
                }
                result.usage.reasoningTokens = optionalUsageValue(
                    usage["completion_tokens_details"], "reasoning_tokens");
            }
        }
    }
    catch (const std::exception &)
    {
        result = AgentModelResponse();
        result.error = AgentModelResponse::kInvalidResponse;
        result.errorMessage = "failed to parse DeepSeek response message";
    }
    return result;
}

AgentModelResponse classifyDeepSeekHttpError(long httpStatus)
{
    AgentModelResponse result;
    result.providerStatusCode = static_cast<int>(httpStatus);
    switch (httpStatus)
    {
    case 400:
    case 422:
        result.error = AgentModelResponse::kRejectedRequest;
        result.errorMessage = "DeepSeek rejected the request";
        break;
    case 401:
        result.error = AgentModelResponse::kAuthentication;
        result.errorMessage = "DeepSeek authentication failed";
        break;
    case 402:
        result.error = AgentModelResponse::kPaymentRequired;
        result.errorMessage = "DeepSeek account balance is insufficient";
        break;
    case 429:
        result.error = AgentModelResponse::kRateLimited;
        result.errorMessage = "DeepSeek rate limit exceeded";
        break;
    case 500:
    case 503:
        result.error = AgentModelResponse::kUnavailable;
        result.errorMessage = "DeepSeek is temporarily unavailable";
        break;
    default:
        result.error = AgentModelResponse::kUpstreamError;
        result.errorMessage = "DeepSeek returned an unexpected HTTP status";
        break;
    }
    return result;
}
