#include "AgentRuntime.h"
#include "DeepSeekProtocol.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

#define CHECK_TRUE(condition)                                                        \
    do                                                                               \
    {                                                                                \
        if (!(condition))                                                            \
        {                                                                            \
            throw std::runtime_error(std::string("check failed: ") + #condition);   \
        }                                                                            \
    } while (false)

AgentToolCall makeToolCall(const std::string &id, const std::string &name,
                           const std::string &arguments)
{
    AgentToolCall call;
    call.toolCallId = id;
    call.name = name;
    call.argumentsJson = arguments;
    return call;
}

AgentModelResponse finalAnswer(const std::string &content)
{
    AgentModelResponse response;
    response.content = content;
    return response;
}

void setUsage(AgentModelResponse *response, uint64_t prompt, uint64_t completion,
              uint64_t cacheHit, uint64_t cacheMiss)
{
    response->usage.promptTokens = prompt;
    response->usage.completionTokens = completion;
    response->usage.totalTokens = prompt + completion;
    response->usage.promptCacheHitTokens = cacheHit;
    response->usage.promptCacheMissTokens = cacheMiss;
}

AgentModelResponse toolRequest(const std::vector<AgentToolCall> &calls)
{
    AgentModelResponse response;
    response.toolCalls = calls;
    return response;
}

AgentModelResponse thinkingToolRequest(const std::vector<AgentToolCall> &calls,
                                       const std::string &reasoning)
{
    AgentModelResponse response = toolRequest(calls);
    response.hasReasoningContent = true;
    response.reasoningContent = reasoning;
    return response;
}

/**
 * FakeModelClient 按顺序返回预先准备的响应，同时保存 Runtime 发送给模型的消息。
 * 这样测试关注的是确定的协议状态转换，不需要真实网络，也不会因为模型随机性偶发失败。
 */
class FakeModelClient : public AgentModelClient
{
public:
    explicit FakeModelClient(const std::vector<AgentModelResponse> &responses)
        : responses_(responses), next_(0) {}

    bool isConfigured() const override { return true; }

    AgentModelResponse complete(const std::vector<nlohmann::json> &messages,
                                const nlohmann::json &tools,
                                long timeoutMs) const override
    {
        CHECK_TRUE(timeoutMs > 0);
        observedMessages_.push_back(messages);
        observedTools_.push_back(tools);
        if (next_ >= responses_.size())
        {
            AgentModelResponse response;
            response.error = AgentModelResponse::kUpstreamError;
            response.errorMessage = "fake response script exhausted";
            return response;
        }
        return responses_[next_++];
    }

    mutable std::vector<std::vector<nlohmann::json>> observedMessages_;
    mutable std::vector<nlohmann::json> observedTools_;

private:
    std::vector<AgentModelResponse> responses_;
    mutable size_t next_;
};

class ThrowingAfterFirstModelClient : public AgentModelClient
{
public:
    ThrowingAfterFirstModelClient() : calls_(0) {}

    bool isConfigured() const override { return true; }

    AgentModelResponse complete(const std::vector<nlohmann::json> &,
                                const nlohmann::json &, long) const override
    {
        ++calls_;
        if (calls_ == 1)
        {
            AgentModelResponse response = toolRequest(std::vector<AgentToolCall>(
                1, makeToolCall("before-throw", "echo_value", "{\"value\":\"kept\"}")));
            setUsage(&response, 9, 1, 3, 6);
            return response;
        }
        throw std::runtime_error("PRIVATE_MODEL_EXCEPTION");
    }

private:
    mutable size_t calls_;
};

class EchoTool : public AgentTool
{
public:
    std::string name() const override { return "echo_value"; }
    std::string description() const override { return "Echo a validated string value."; }
    bool isReadOnly() const override { return true; }

    nlohmann::json inputSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"value", {{"type", "string"}}}
            }},
            {"required", nlohmann::json::array({"value"})},
            {"additionalProperties", false}
        };
    }

    AgentToolResult execute(const nlohmann::json &arguments,
                            const AgentToolContext &) const override
    {
        AgentToolResult result;
        if (arguments.size() != 1 || !arguments.contains("value") ||
            !arguments["value"].is_string())
        {
            result.errorMessage = "echo_value requires only a string value";
            return result;
        }
        result.success = true;
        result.output = arguments["value"].get<std::string>();
        return result;
    }
};

std::shared_ptr<AgentToolRegistry> makeRegistry()
{
    std::shared_ptr<AgentToolRegistry> registry(new AgentToolRegistry());
    registry->registerTool(std::shared_ptr<AgentTool>(new EchoTool()));
    return registry;
}

void testDirectAnswer()
{
    std::shared_ptr<FakeModelClient> model(
        new FakeModelClient(std::vector<AgentModelResponse>(1, finalAnswer("直接回答"))));
    AgentRuntime runtime(model, makeRegistry());

    AgentRunResult result = runtime.run(std::vector<AgentConversationMessage>(), "你好");

    CHECK_TRUE(result.ok());
    CHECK_TRUE(result.answer == "直接回答");
    CHECK_TRUE(result.modelCalls == 1);
    CHECK_TRUE(result.toolExecutions.empty());
    CHECK_TRUE(model->observedMessages_.size() == 1);
    CHECK_TRUE(model->observedMessages_[0].size() == 2);
    CHECK_TRUE(model->observedMessages_[0][0]["role"] == "system");
    CHECK_TRUE(model->observedMessages_[0][1]["role"] == "user");
    CHECK_TRUE(model->observedTools_[0][0]["function"]["name"] == "echo_value");
}

void testSingleToolRoundTrip()
{
    std::vector<AgentModelResponse> responses;
    AgentModelResponse first = thinkingToolRequest(std::vector<AgentToolCall>(
        1, makeToolCall("call-1", "echo_value", "{\"value\":\"abc\"}")),
        "PRIVATE_REASONING_SENTINEL_R1");
    setUsage(&first, 10, 2, 4, 6);
    responses.push_back(first);
    AgentModelResponse second = finalAnswer("工具返回 abc");
    setUsage(&second, 20, 5, 8, 12);
    responses.push_back(second);
    std::shared_ptr<FakeModelClient> model(new FakeModelClient(responses));
    AgentRuntime runtime(model, makeRegistry());

    AgentRunResult result = runtime.run(std::vector<AgentConversationMessage>(), "回显 abc");

    CHECK_TRUE(result.ok());
    CHECK_TRUE(result.modelCalls == 2);
    CHECK_TRUE(result.toolExecutions.size() == 1);
    CHECK_TRUE(result.toolExecutions[0].success);
    CHECK_TRUE(result.toolExecutions[0].output == "abc");
    CHECK_TRUE(result.toolExecutions[0].latencyMs >= 0);
    CHECK_TRUE(result.metrics.modelExecutions.size() == 2);
    CHECK_TRUE(result.metrics.modelLatencyMs >= 0);
    CHECK_TRUE(result.metrics.toolLatencyMs >= 0);
    CHECK_TRUE(result.metrics.usage.promptTokens == 30);
    CHECK_TRUE(result.metrics.usage.completionTokens == 7);
    CHECK_TRUE(result.metrics.usage.totalTokens == 37);
    CHECK_TRUE(result.metrics.usage.promptCacheHitTokens == 12);
    CHECK_TRUE(result.metrics.usage.promptCacheMissTokens == 18);
    CHECK_TRUE(model->observedMessages_[1].size() == 4);
    CHECK_TRUE(model->observedMessages_[1][2]["role"] == "assistant");
    CHECK_TRUE(model->observedMessages_[1][2]["tool_calls"][0]["id"] == "call-1");
    CHECK_TRUE(model->observedMessages_[1][2]["reasoning_content"] ==
               "PRIVATE_REASONING_SENTINEL_R1");
    CHECK_TRUE(model->observedMessages_[1][3]["role"] == "tool");
    CHECK_TRUE(model->observedMessages_[1][3]["tool_call_id"] == "call-1");
    const nlohmann::json toolContent = nlohmann::json::parse(
        model->observedMessages_[1][3]["content"].get<std::string>());
    CHECK_TRUE(toolContent["ok"] == true);
    CHECK_TRUE(toolContent["result"] == "abc");
}

void testMultipleCallsAndSteps()
{
    std::vector<AgentModelResponse> responses;
    std::vector<AgentToolCall> firstCalls;
    firstCalls.push_back(makeToolCall("call-a", "echo_value", "{\"value\":\"A\"}"));
    firstCalls.push_back(makeToolCall("call-b", "echo_value", "{\"value\":\"B\"}"));
    responses.push_back(thinkingToolRequest(firstCalls, "reasoning-one"));
    responses.push_back(thinkingToolRequest(std::vector<AgentToolCall>(
        1, makeToolCall("call-c", "echo_value", "{\"value\":\"C\"}")),
        "reasoning-two"));
    responses.push_back(finalAnswer("ABC"));
    std::shared_ptr<FakeModelClient> model(new FakeModelClient(responses));
    AgentRuntime runtime(model, makeRegistry());

    AgentRunResult result = runtime.run(std::vector<AgentConversationMessage>(), "依次处理");

    CHECK_TRUE(result.ok());
    CHECK_TRUE(result.modelCalls == 3);
    CHECK_TRUE(result.toolExecutions.size() == 3);
    CHECK_TRUE(result.toolExecutions[0].output == "A");
    CHECK_TRUE(result.toolExecutions[1].output == "B");
    CHECK_TRUE(result.toolExecutions[2].output == "C");
    // 第三次请求应包含 system、user、两轮 assistant tool_calls 和三个 tool result。
    CHECK_TRUE(model->observedMessages_[2].size() == 7);
    CHECK_TRUE(model->observedMessages_[2][2]["reasoning_content"] == "reasoning-one");
    CHECK_TRUE(model->observedMessages_[2][5]["reasoning_content"] == "reasoning-two");
}

void testInvalidArgumentsBecomeToolFeedback()
{
    std::vector<AgentModelResponse> responses;
    responses.push_back(toolRequest(std::vector<AgentToolCall>(
        1, makeToolCall("bad-json", "echo_value", "{"))));
    responses.push_back(finalAnswer("参数不合法"));
    std::shared_ptr<FakeModelClient> model(new FakeModelClient(responses));
    AgentRuntime runtime(model, makeRegistry());

    AgentRunResult result = runtime.run(std::vector<AgentConversationMessage>(), "测试错误参数");

    CHECK_TRUE(result.ok());
    CHECK_TRUE(result.toolExecutions.size() == 1);
    CHECK_TRUE(!result.toolExecutions[0].success);
    CHECK_TRUE(result.toolExecutions[0].output == "tool arguments are not valid JSON");
    const nlohmann::json toolContent = nlohmann::json::parse(
        model->observedMessages_[1].back()["content"].get<std::string>());
    CHECK_TRUE(toolContent["ok"] == false);
}

void testUnknownToolIsNeverExecuted()
{
    std::vector<AgentModelResponse> responses;
    responses.push_back(toolRequest(std::vector<AgentToolCall>(
        1, makeToolCall("unknown-1", "delete_everything", "{}"))));
    responses.push_back(finalAnswer("该工具不可用"));
    std::shared_ptr<FakeModelClient> model(new FakeModelClient(responses));
    AgentRuntime runtime(model, makeRegistry());

    AgentRunResult result = runtime.run(std::vector<AgentConversationMessage>(), "执行未知工具");

    CHECK_TRUE(result.ok());
    CHECK_TRUE(result.toolExecutions.size() == 1);
    CHECK_TRUE(!result.toolExecutions[0].success);
    CHECK_TRUE(result.toolExecutions[0].output == "requested tool is not registered");
}

void testExecutionBudgetStopsLoop()
{
    std::vector<AgentModelResponse> responses;
    responses.push_back(toolRequest(std::vector<AgentToolCall>(
        1, makeToolCall("loop-1", "echo_value", "{\"value\":\"1\"}"))));
    responses.push_back(toolRequest(std::vector<AgentToolCall>(
        1, makeToolCall("loop-2", "echo_value", "{\"value\":\"2\"}"))));
    std::shared_ptr<FakeModelClient> model(new FakeModelClient(responses));
    AgentRunOptions options;
    options.maxModelCalls = 2;
    AgentRuntime runtime(model, makeRegistry(), options);

    AgentRunResult result = runtime.run(std::vector<AgentConversationMessage>(), "循环调用");

    CHECK_TRUE(!result.ok());
    CHECK_TRUE(result.error == AgentRunResult::kBudgetExceeded);
    CHECK_TRUE(result.modelCalls == 2);
    // 第二次调用后已经无法把工具结果再交给模型，因此第二个工具不能产生副作用。
    CHECK_TRUE(result.toolExecutions.size() == 1);
    CHECK_TRUE(result.toolExecutions[0].output == "1");
}

void testDuplicateToolCallIdIsRejected()
{
    std::vector<AgentModelResponse> responses;
    responses.push_back(toolRequest(std::vector<AgentToolCall>(
        1, makeToolCall("same-id", "echo_value", "{\"value\":\"A\"}"))));
    responses.push_back(toolRequest(std::vector<AgentToolCall>(
        1, makeToolCall("same-id", "echo_value", "{\"value\":\"B\"}"))));
    std::shared_ptr<FakeModelClient> model(new FakeModelClient(responses));
    AgentRuntime runtime(model, makeRegistry());

    AgentRunResult result = runtime.run(std::vector<AgentConversationMessage>(), "重复 ID");

    CHECK_TRUE(!result.ok());
    CHECK_TRUE(result.error == AgentRunResult::kInvalidModelResponse);
    CHECK_TRUE(result.toolExecutions.size() == 1);
    CHECK_TRUE(result.toolExecutions[0].output == "A");
}

void testDeepSeekProtocol()
{
    const std::vector<nlohmann::json> messages(
        1, nlohmann::json({{"role", "user"}, {"content", "hello"}}));
    const nlohmann::json request = buildDeepSeekChatRequest(
        "deepseek-v4-flash", messages, makeRegistry()->definitions(), true);
    CHECK_TRUE(request["thinking"]["type"] == "enabled");
    const nlohmann::json disabledRequest = buildDeepSeekChatRequest(
        "deepseek-v4-flash", messages, makeRegistry()->definitions(), false);
    CHECK_TRUE(disabledRequest["thinking"]["type"] == "disabled");
    CHECK_TRUE(request["tool_choice"] == "auto");

    const std::string toolResponse = nlohmann::json({
        {"choices", nlohmann::json::array({{
            {"finish_reason", "tool_calls"},
            {"message", {
                {"role", "assistant"},
                {"reasoning_content", "provider reasoning"},
                {"content", nullptr},
                {"tool_calls", nlohmann::json::array({{
                    {"id", "provider-1"},
                    {"type", "function"},
                    {"function", {
                        {"name", "echo_value"},
                        {"arguments", "{\"value\":\"ok\"}"}
                    }}
                }})}
            }}
        }})}
    }).dump();
    const AgentModelResponse parsedTool = parseDeepSeekChatResponse(toolResponse);
    CHECK_TRUE(parsedTool.ok());
    CHECK_TRUE(parsedTool.toolCalls.size() == 1);
    CHECK_TRUE(parsedTool.toolCalls[0].toolCallId == "provider-1");
    CHECK_TRUE(parsedTool.hasReasoningContent);
    CHECK_TRUE(parsedTool.reasoningContent == "provider reasoning");

    const std::string emptyReasoningResponse = nlohmann::json({
        {"choices", nlohmann::json::array({{
            {"finish_reason", "tool_calls"},
            {"message", {
                {"role", "assistant"},
                {"reasoning_content", ""},
                {"content", nullptr},
                {"tool_calls", nlohmann::json::array({{
                    {"id", "empty-reasoning"},
                    {"type", "function"},
                    {"function", {{"name", "echo_value"}, {"arguments", "{}"}}}
                }})}
            }}
        }})}
    }).dump();
    AgentModelResponse parsedEmptyReasoning =
        parseDeepSeekChatResponse(emptyReasoningResponse);
    CHECK_TRUE(parsedEmptyReasoning.ok());
    CHECK_TRUE(parsedEmptyReasoning.hasReasoningContent);
    CHECK_TRUE(parsedEmptyReasoning.reasoningContent.empty());

    const std::string nullReasoningResponse = nlohmann::json({
        {"choices", nlohmann::json::array({{
            {"finish_reason", "stop"},
            {"message", {
                {"role", "assistant"},
                {"reasoning_content", nullptr},
                {"content", "done"}
            }}
        }})}
    }).dump();
    CHECK_TRUE(!parseDeepSeekChatResponse(nullReasoningResponse).hasReasoningContent);

    const std::string oversizedReasoningResponse = nlohmann::json({
        {"choices", nlohmann::json::array({{
            {"finish_reason", "stop"},
            {"message", {
                {"role", "assistant"},
                {"reasoning_content", std::string(1024 * 1024 + 1, 'r')},
                {"content", "done"}
            }}
        }})}
    }).dump();
    CHECK_TRUE(!parseDeepSeekChatResponse(oversizedReasoningResponse).ok());

    const std::string finalResponse = nlohmann::json({
        {"choices", nlohmann::json::array({{
            {"finish_reason", "stop"},
            {"message", {{"role", "assistant"}, {"content", "done"}}}
        }})},
        {"usage", {
            {"prompt_tokens", 12},
            {"completion_tokens", 3},
            {"total_tokens", 15},
            {"prompt_cache_hit_tokens", 7},
            {"prompt_cache_miss_tokens", 5},
            {"completion_tokens_details", {{"reasoning_tokens", 0}}}
        }}
    }).dump();
    const AgentModelResponse parsedFinal = parseDeepSeekChatResponse(finalResponse);
    CHECK_TRUE(parsedFinal.ok());
    CHECK_TRUE(parsedFinal.content == "done");
    CHECK_TRUE(parsedFinal.usage.promptTokens == 12);
    CHECK_TRUE(parsedFinal.usage.totalTokens == 15);
    CHECK_TRUE(parsedFinal.usage.promptCacheHitTokens == 7);

    const std::string truncatedResponse = nlohmann::json({
        {"choices", nlohmann::json::array({{
            {"finish_reason", "length"},
            {"message", {{"role", "assistant"}, {"content", "partial"}}}
        }})}
    }).dump();
    const AgentModelResponse parsedTruncated = parseDeepSeekChatResponse(truncatedResponse);
    CHECK_TRUE(!parsedTruncated.ok());
    CHECK_TRUE(parsedTruncated.error == AgentModelResponse::kInvalidResponse);

    const std::string wrongTypeResponse = nlohmann::json({
        {"choices", nlohmann::json::array({{
            {"finish_reason", "tool_calls"},
            {"message", {
                {"role", "assistant"},
                {"content", nullptr},
                {"tool_calls", nlohmann::json::array({{
                    {"id", "bad-type"},
                    {"type", "custom"},
                    {"function", {{"name", "echo_value"}, {"arguments", "{}"}}}
                }})}
            }}
        }})}
    }).dump();
    CHECK_TRUE(!parseDeepSeekChatResponse(wrongTypeResponse).ok());

    const std::string malformedUsage = nlohmann::json({
        {"choices", nlohmann::json::array({{
            {"finish_reason", "stop"},
            {"message", {{"role", "assistant"}, {"content", "done"}}}
        }})},
        {"usage", {{"completion_tokens_details", "not-an-object"}}}
    }).dump();
    CHECK_TRUE(!parseDeepSeekChatResponse(malformedUsage).ok());

    const std::string malformedReasoning = nlohmann::json({
        {"choices", nlohmann::json::array({{
            {"finish_reason", "stop"},
            {"message", {
                {"role", "assistant"},
                {"reasoning_content", nlohmann::json::object()},
                {"content", "done"}
            }}
        }})}
    }).dump();
    CHECK_TRUE(!parseDeepSeekChatResponse(malformedReasoning).ok());
}

void testDeepSeekHttpErrorClassification()
{
    CHECK_TRUE(classifyDeepSeekHttpError(401).error ==
               AgentModelResponse::kAuthentication);
    CHECK_TRUE(classifyDeepSeekHttpError(402).error ==
               AgentModelResponse::kPaymentRequired);
    CHECK_TRUE(classifyDeepSeekHttpError(429).error ==
               AgentModelResponse::kRateLimited);
    CHECK_TRUE(classifyDeepSeekHttpError(400).error ==
               AgentModelResponse::kRejectedRequest);
    CHECK_TRUE(classifyDeepSeekHttpError(422).error ==
               AgentModelResponse::kRejectedRequest);
    CHECK_TRUE(classifyDeepSeekHttpError(500).error ==
               AgentModelResponse::kUnavailable);
    AgentModelResponse unavailable = classifyDeepSeekHttpError(503);
    CHECK_TRUE(unavailable.error == AgentModelResponse::kUnavailable);
    CHECK_TRUE(unavailable.providerStatusCode == 503);
    CHECK_TRUE(classifyDeepSeekHttpError(418).error ==
               AgentModelResponse::kUpstreamError);
}

void testDeepSeekSseParserWithArbitraryFragments()
{
    std::vector<std::string> deltas;
    std::vector<bool> thinkingStates;
    DeepSeekSseParser parser(
        [&deltas](const std::string &delta) {
            deltas.push_back(delta);
            return true;
        },
        [&thinkingStates](bool started) {
            thinkingStates.push_back(started);
            return true;
        });

    const std::string stream =
        ": heartbeat\r\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"reasoning_content\":\"PRIVATE_\"},\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"REASONING_SENTINEL\"},\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"北\"},\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"京\"},\"finish_reason\":\"stop\"}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":4,\"completion_tokens\":2,\"total_tokens\":6}}\n\n"
        "data: [DONE]\n\n";
    std::string error;
    for (size_t i = 0; i < stream.size(); ++i)
    {
        CHECK_TRUE(parser.feed(&stream[i], 1, &error));
    }
    AgentModelResponse response = parser.finish();
    CHECK_TRUE(response.ok());
    CHECK_TRUE(response.content == "北京");
    CHECK_TRUE(response.reasoningContent == "PRIVATE_REASONING_SENTINEL");
    CHECK_TRUE(response.hasReasoningContent);
    CHECK_TRUE(deltas.size() == 2);
    CHECK_TRUE(thinkingStates.size() == 2);
    CHECK_TRUE(thinkingStates[0] == true);
    CHECK_TRUE(thinkingStates[1] == false);
    CHECK_TRUE(response.usage.totalTokens == 6);

    // 避免 `Type variable(Type())` 被 C++ 解析成函数声明（most vexing parse）。
    AgentModelClient::TextDeltaCallback noDelta;
    DeepSeekSseParser toolParser(noDelta);
    const std::string toolStream =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call-\",\"type\":\"function\",\"function\":{\"name\":\"wea\",\"arguments\":\"{\\\"loc\"}}]},\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"1\",\"function\":{\"name\":\"ther\",\"arguments\":\"ation\\\":\\\"Beijing\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    CHECK_TRUE(toolParser.feed(toolStream.data(), toolStream.size(), &error));
    AgentModelResponse toolResponse = toolParser.finish();
    CHECK_TRUE(toolResponse.ok());
    CHECK_TRUE(toolResponse.toolCalls.size() == 1);
    CHECK_TRUE(toolResponse.toolCalls[0].toolCallId == "call-1");
    CHECK_TRUE(toolResponse.toolCalls[0].name == "weather");
    CHECK_TRUE(toolResponse.toolCalls[0].argumentsJson == "{\"location\":\"Beijing\"}");

    AgentModelClient::TextDeltaCallback noLateDelta;
    DeepSeekSseParser strictDoneParser(noLateDelta);
    const std::string afterDone = "data: [DONE]\n\ndata: {\"choices\":[]}\n\n";
    CHECK_TRUE(!strictDoneParser.feed(afterDone.data(), afterDone.size(), &error));
    CHECK_TRUE(!strictDoneParser.finish().ok());

    DeepSeekSseParser oversizedEventParser(noLateDelta);
    std::string oversizedEvent;
    const std::string dataLine = "data: " + std::string(4096, 'x') + "\n";
    for (size_t i = 0; i < 260; ++i)
    {
        oversizedEvent += dataLine;
    }
    CHECK_TRUE(!oversizedEventParser.feed(
        oversizedEvent.data(), oversizedEvent.size(), &error));
    CHECK_TRUE(!oversizedEventParser.finish().ok());

    std::vector<bool> failedThinkingStates;
    DeepSeekSseParser failedThinkingParser(
        noLateDelta,
        [&failedThinkingStates](bool started) {
            failedThinkingStates.push_back(started);
            return true;
        });
    const std::string failedThinkingStream =
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"started\"},\"finish_reason\":null}]}\n\n"
        "data: {not-json}\n\n";
    CHECK_TRUE(!failedThinkingParser.feed(
        failedThinkingStream.data(), failedThinkingStream.size(), &error));
    CHECK_TRUE(!failedThinkingParser.finish().ok());
    CHECK_TRUE(failedThinkingStates.size() == 2);
    CHECK_TRUE(failedThinkingStates[0] == true);
    CHECK_TRUE(failedThinkingStates[1] == false);
}

void testModelTimeoutMapping()
{
    AgentModelResponse timeout;
    timeout.error = AgentModelResponse::kTimeout;
    timeout.errorMessage = "mock timeout";
    std::shared_ptr<FakeModelClient> model(
        new FakeModelClient(std::vector<AgentModelResponse>(1, timeout)));
    AgentRuntime runtime(model, makeRegistry());

    AgentRunResult result = runtime.run(std::vector<AgentConversationMessage>(), "超时测试");

    CHECK_TRUE(!result.ok());
    CHECK_TRUE(result.error == AgentRunResult::kUpstreamTimeout);
    CHECK_TRUE(result.errorMessage == "mock timeout");
}

void testModelExceptionPreservesMetrics()
{
    std::shared_ptr<AgentModelClient> model(new ThrowingAfterFirstModelClient());
    AgentRuntime runtime(model, makeRegistry());

    AgentRunResult result = runtime.run(std::vector<AgentConversationMessage>(), "异常测试");

    CHECK_TRUE(!result.ok());
    CHECK_TRUE(result.error == AgentRunResult::kUpstreamError);
    CHECK_TRUE(result.modelCalls == 2);
    CHECK_TRUE(result.metrics.modelExecutions.size() == 2);
    CHECK_TRUE(result.metrics.usage.totalTokens == 10);
    CHECK_TRUE(result.toolExecutions.size() == 1);
    CHECK_TRUE(result.errorMessage == "model client raised an exception");
    CHECK_TRUE(result.errorMessage.find("PRIVATE_MODEL_EXCEPTION") == std::string::npos);
}

void testExpiredRunDeadline()
{
    std::shared_ptr<FakeModelClient> model(
        new FakeModelClient(std::vector<AgentModelResponse>(1, finalAnswer("不应调用"))));
    AgentRuntime runtime(model, makeRegistry());

    AgentRunResult result = runtime.runUntil(
        std::vector<AgentConversationMessage>(), "已过期",
        std::chrono::steady_clock::now() - std::chrono::milliseconds(1));

    CHECK_TRUE(!result.ok());
    CHECK_TRUE(result.error == AgentRunResult::kDeadlineExceeded);
    CHECK_TRUE(result.modelCalls == 0);
    CHECK_TRUE(model->observedMessages_.empty());
}

} // namespace

int main()
{
    try
    {
        testDirectAnswer();
        testSingleToolRoundTrip();
        testMultipleCallsAndSteps();
        testInvalidArgumentsBecomeToolFeedback();
        testUnknownToolIsNeverExecuted();
        testExecutionBudgetStopsLoop();
        testDuplicateToolCallIdIsRejected();
        testDeepSeekProtocol();
        testDeepSeekHttpErrorClassification();
        testDeepSeekSseParserWithArbitraryFragments();
        testModelTimeoutMapping();
        testModelExceptionPreservesMetrics();
        testExpiredRunDeadline();
    }
    catch (const std::exception &ex)
    {
        std::cerr << "AgentRuntimeTest failed: " << ex.what() << std::endl;
        return 1;
    }

    std::cout << "AgentRuntimeTest passed: 13 cases" << std::endl;
    return 0;
}
