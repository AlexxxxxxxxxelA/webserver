#include <string>

#include <TcpServer.h>
#include <Logger.h>
#include <sys/stat.h>
#include <signal.h>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include "AgentDemo.h"
#include "AsyncLogging.h"
#include "LFU.h"
#include "memoryPool.h"
#include "HttpServer.h"
// 日志文件滚动大小为1MB (1*1024*1024字节)
static const off_t kRollSize = 1*1024*1024;
class AgentServer
{
public:
    AgentServer(EventLoop *loop, const InetAddress &addr, const std::string &name,
                AgentDemoService &service)
        : service_(service)
        , server_(loop, addr, name)
    {
        server_.setConnectionCallback(
            std::bind(&AgentDemoService::onConnection, &service_, std::placeholders::_1));

        server_.setMessageCallback(
            std::bind(&AgentDemoService::onMessage, &service_, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

        server_.setThreadNum(3);
    }

    void start()
    {
        server_.start();
    }

private:
    // server_ 声明在最后，因此最先析构，停止回调后外部 service 才会销毁。
    AgentDemoService &service_;
    TcpServer server_;
};

/**
 * 纯 TCP QPS 测试服务。
 *
 * 协议：每收到一行 "/ping\n" 返回一行 "pong\n"。pending_ 保存尚未遇到换行的
 * 半包，避免把一次 read 当成一条完整消息。它只测试 Reactor/TCP/Buffer/发送链路，
 * 不包含 HTTP、JSON、DeepSeek 和日志业务，因此压测结果不能代表 Agent 吞吐量。
 */
class QpsServer
{
public:
    QpsServer(EventLoop *loop, const InetAddress &address, int threadCount)
        : server_(loop, address, "QpsServer", TcpServer::kReusePort)
    {
        server_.setThreadNum(threadCount);
        server_.setConnectionCallback(
            std::bind(&QpsServer::onConnection, this, std::placeholders::_1));
        server_.setMessageCallback(
            std::bind(&QpsServer::onMessage, this, std::placeholders::_1,
                      std::placeholders::_2, std::placeholders::_3));
    }

    void start() { server_.start(); }

private:
    struct ConnectionState
    {
        std::mutex mutex;
        std::string pending;
    };

    void onConnection(const TcpConnectionPtr &connection)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connection->connected())
        {
            states_[connection->name()] = std::make_shared<ConnectionState>();
        }
        else
        {
            states_.erase(connection->name());
        }
    }

    void onMessage(const TcpConnectionPtr &connection, Buffer *buffer, Timestamp)
    {
        std::shared_ptr<ConnectionState> state;
        {
            // 全局锁只用于查 map；不同连接随后使用各自 mutex 并行解析。
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = states_.find(connection->name());
            if (it == states_.end())
            {
                return;
            }
            state = it->second;
        }

        std::string response;
        bool inputTooLarge = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            std::string &pending = state->pending;
            pending += buffer->retrieveAllAsString();

            size_t lineBegin = 0;
            size_t newline = pending.find('\n', lineBegin);
            while (newline != std::string::npos)
            {
                size_t lineEnd = newline;
                if (lineEnd > lineBegin && pending[lineEnd - 1] == '\r')
                {
                    --lineEnd;
                }
                if (lineEnd - lineBegin == 5 &&
                    pending.compare(lineBegin, 5, "/ping") == 0)
                {
                    response += "pong\n";
                }
                else
                {
                    response += "error\n";
                }
                lineBegin = newline + 1;
                newline = pending.find('\n', lineBegin);
            }
            if (lineBegin > 0)
            {
                // 一次性删除全部完整行，避免每行 erase 造成 O(n^2) 内存搬移。
                pending.erase(0, lineBegin);
            }

            const size_t kMaxPendingLine = 64 * 1024;
            if (pending.size() > kMaxPendingLine)
            {
                pending.clear();
                inputTooLarge = true;
            }
        }

        if (!response.empty())
        {
            connection->send(response);
        }
        if (inputTooLarge)
        {
            connection->send("error\n");
            connection->shutdown();
        }
    }

    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<ConnectionState>> states_;
    // 最后声明，使 TcpServer 最先析构并停止回调。
    TcpServer server_;
};

bool parsePositiveIntOption(const std::string &argument, const std::string &prefix,
                            int minValue, int maxValue, int *value)
{
    if (argument.find(prefix) != 0)
    {
        return false;
    }
    const std::string text = argument.substr(prefix.size());
    char *end = NULL;
    errno = 0;
    long parsed = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' ||
        parsed < minValue || parsed > maxValue)
    {
        throw std::invalid_argument(std::string("invalid option: ") + argument);
    }
    *value = static_cast<int>(parsed);
    return true;
}

int runQpsServer(int port, int threadCount)
{
    EventLoop loop;
    InetAddress address(static_cast<uint16_t>(port));
    QpsServer server(&loop, address, threadCount);
    server.start();
    std::cout << "QPS server: nc 127.0.0.1 " << port
              << "  (send /ping per line), IO threads=" << threadCount << std::endl;
    loop.loop();
    return 0;
}

std::string lowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

HttpResponse jsonError(int statusCode, const std::string &statusMessage,
                       const std::string &message)
{
    // 使用 JSON 库生成响应，自动处理引号、换行和 Unicode 转义。
    HttpResponse response(true);
    response.setStatusCode(statusCode);
    response.setStatusMessage(statusMessage);
    response.setContentType("application/json; charset=utf-8");
    nlohmann::json body;
    body["error"] = message;
    response.setBody(body.dump() + "\n");
    return response;
}

HttpResponse agentJsonError(int statusCode, const std::string &statusMessage,
                            const std::string &code, const std::string &message,
                            const AgentDemoService::AgentResult &result)
{
    /*
     * 对外只返回稳定错误码和安全文案，不暴露 curl 的 DNS、TLS、代理或本机路径细节。
     * 内部 Provider 状态和阶段信息已进入 agent_trace，可使用 run_id 关联排查。
     */
    HttpResponse response(true);
    response.setStatusCode(statusCode);
    response.setStatusMessage(statusMessage);
    response.setContentType("application/json; charset=utf-8");
    nlohmann::json body;
    body["code"] = code;
    body["error"] = message;
    body["run_id"] = result.runId;
    response.setBody(body.dump() + "\n");
    return response;
}

nlohmann::json agentMetricsJson(const AgentDemoService::AgentResult &result)
{
    nlohmann::json metrics;
    metrics["queue_wait_ms"] = result.queueWaitMs;
    metrics["total_latency_ms"] = result.totalLatencyMs;
    metrics["model_latency_ms"] = result.metrics.modelLatencyMs;
    metrics["tool_latency_ms"] = result.metrics.toolLatencyMs;
    metrics["model_calls"] = result.metrics.modelExecutions.size();
    metrics["tool_calls"] = result.toolExecutions.size();
    metrics["history_load_ms"] = result.historyLoadMs;
    metrics["history_save_ms"] = result.historySaveMs;
    metrics["context_estimated_tokens"] = result.contextEstimatedTokens;
    metrics["context_recent_turns"] = result.contextRecentTurns;
    metrics["summary_used"] = result.summaryUsed;
    metrics["usage"] = {
        {"prompt_tokens", result.metrics.usage.promptTokens},
        {"completion_tokens", result.metrics.usage.completionTokens},
        {"total_tokens", result.metrics.usage.totalTokens},
        {"prompt_cache_hit_tokens", result.metrics.usage.promptCacheHitTokens},
        {"prompt_cache_miss_tokens", result.metrics.usage.promptCacheMissTokens},
        {"reasoning_tokens", result.metrics.usage.reasoningTokens}
    };
    return metrics;
}

std::string agentStreamErrorCode(AgentDemoService::AgentResult::Error error)
{
    switch (error)
    {
    case AgentDemoService::AgentResult::kNotConfigured: return "AGENT_NOT_CONFIGURED";
    case AgentDemoService::AgentResult::kUpstreamTimeout: return "UPSTREAM_TIMEOUT";
    case AgentDemoService::AgentResult::kUpstreamAuthentication: return "UPSTREAM_AUTHENTICATION";
    case AgentDemoService::AgentResult::kUpstreamPaymentRequired: return "UPSTREAM_PAYMENT_REQUIRED";
    case AgentDemoService::AgentResult::kUpstreamRateLimited: return "UPSTREAM_RATE_LIMITED";
    case AgentDemoService::AgentResult::kUpstreamRejectedRequest: return "UPSTREAM_REJECTED_REQUEST";
    case AgentDemoService::AgentResult::kUpstreamUnavailable: return "UPSTREAM_UNAVAILABLE";
    case AgentDemoService::AgentResult::kInvalidUpstreamResponse: return "INVALID_UPSTREAM_RESPONSE";
    case AgentDemoService::AgentResult::kRunDeadlineExceeded: return "AGENT_RUN_DEADLINE_EXCEEDED";
    case AgentDemoService::AgentResult::kCancelled: return "CANCELLED";
    case AgentDemoService::AgentResult::kExecutionLimit: return "AGENT_EXECUTION_LIMIT";
    case AgentDemoService::AgentResult::kInternalError: return "INTERNAL_ERROR";
    case AgentDemoService::AgentResult::kUpstreamError: return "UPSTREAM_ERROR";
    case AgentDemoService::AgentResult::kNone: return "NONE";
    }
    return "UNKNOWN_ERROR";
}

std::string agentStreamErrorMessage(AgentDemoService::AgentResult::Error error)
{
    switch (error)
    {
    case AgentDemoService::AgentResult::kNotConfigured: return "agent model is not configured";
    case AgentDemoService::AgentResult::kUpstreamTimeout: return "agent upstream request timed out";
    case AgentDemoService::AgentResult::kUpstreamAuthentication: return "agent upstream authentication failed";
    case AgentDemoService::AgentResult::kUpstreamPaymentRequired: return "agent upstream account is unavailable";
    case AgentDemoService::AgentResult::kUpstreamRateLimited: return "agent upstream rate limit exceeded";
    case AgentDemoService::AgentResult::kUpstreamRejectedRequest: return "agent upstream rejected the request";
    case AgentDemoService::AgentResult::kUpstreamUnavailable: return "agent upstream is temporarily unavailable";
    case AgentDemoService::AgentResult::kInvalidUpstreamResponse: return "agent upstream returned an invalid response";
    case AgentDemoService::AgentResult::kRunDeadlineExceeded: return "agent run deadline was exceeded";
    case AgentDemoService::AgentResult::kCancelled: return "agent stream was cancelled";
    case AgentDemoService::AgentResult::kExecutionLimit: return "agent execution budget was exceeded";
    case AgentDemoService::AgentResult::kInternalError: return "internal agent error";
    case AgentDemoService::AgentResult::kUpstreamError: return "agent upstream request failed";
    case AgentDemoService::AgentResult::kNone: return "";
    }
    return "agent request failed";
}

bool onStreamingHttpRequest(AgentDemoService &service,
                            const HttpRequest &request,
                            const HttpStreamResponder &stream)
{
    if (request.path() != "/agent/run/stream")
    {
        return false;
    }

    const auto reject = [&stream](HttpResponse response) {
        stream.reject(response);
    };
    if (request.method() != HttpRequest::kPost)
    {
        HttpResponse response = jsonError(405, "Method Not Allowed", "method not allowed");
        response.addHeader("Allow", "POST");
        reject(response);
        return true;
    }

    std::string contentType = lowerCopy(request.getHeader("Content-Type"));
    const size_t semicolon = contentType.find(';');
    std::string mediaType = contentType.substr(0, semicolon);
    mediaType.erase(0, mediaType.find_first_not_of(" \t"));
    while (!mediaType.empty() && std::isspace(static_cast<unsigned char>(mediaType.back())))
    {
        mediaType.pop_back();
    }
    if (mediaType != "application/json")
    {
        reject(jsonError(415, "Unsupported Media Type",
                         "Content-Type must be application/json"));
        return true;
    }

    std::string sessionId;
    std::string message;
    try
    {
        const nlohmann::json body = nlohmann::json::parse(request.body());
        if (!body.is_object() || !body.contains("session_id") ||
            !body.contains("message") || !body["session_id"].is_string() ||
            !body["message"].is_string())
        {
            throw std::invalid_argument("required string field is missing");
        }
        sessionId = body["session_id"].get<std::string>();
        message = body["message"].get<std::string>();
    }
    catch (const std::exception &)
    {
        reject(jsonError(400, "Bad Request", "JSON requires string session_id and message"));
        return true;
    }

    AgentDemoService::SubmitStatus status = service.submitStreaming(
        std::string("http:") + sessionId, message,
        [stream](const AgentEvent &event) {
            return stream.sendEvent(event.type, event.data.dump());
        },
        [stream]() { return stream.cancelled(); },
        [stream](AgentDemoService::AgentResult result) {
            if (stream.cancelled())
            {
                return;
            }
            if (!result.ok())
            {
                nlohmann::json error = {
                    {"run_id", result.runId},
                    {"code", agentStreamErrorCode(result.error)},
                    {"message", agentStreamErrorMessage(result.error)}
                };
                stream.sendEvent("error", error.dump());
                stream.finish();
                return;
            }
            nlohmann::json completed = {
                {"run_id", result.runId},
                // 正文已通过 assistant.delta 发送，终态不重复携带大答案。
                {"answer_bytes", result.answer.size()},
                {"answer_sequence", result.metrics.modelExecutions.size()},
                {"metrics", agentMetricsJson(result)}
            };
            if (!stream.sendEvent("run.completed", completed.dump()))
            {
                return;
            }
            stream.finish();
        });

    if (status == AgentDemoService::kAccepted)
    {
        stream.start();
        return true;
    }
    if (status == AgentDemoService::kSessionBusy)
    {
        reject(jsonError(409, "Conflict", "this session is already processing a request"));
    }
    else if (status == AgentDemoService::kQueueFull)
    {
        HttpResponse response = jsonError(503, "Service Unavailable", "agent task queue is full");
        response.addHeader("Retry-After", "1");
        reject(response);
    }
    else
    {
        reject(jsonError(400, "Bad Request", "invalid session_id or message"));
    }
    return true;
}

bool onAsyncHttpRequest(AgentDemoService &service,
                        const HttpRequest &request,
                        const HttpServer::AsyncHttpResponder &responder)
{
    /*
     * 本函数仍运行在 HTTP IO 线程，只做路由、Content-Type、JSON 和长度校验。
     * service.submit() 之后的 DeepSeek/工具流程在业务线程执行。
     */
    if (request.path() == "/agent/sessions")
    {
        if (request.method() == HttpRequest::kGet)
        {
            AgentDemoService::SubmitStatus status = service.listHttpSessionsAsync(
                50,
                [responder](bool loaded,
                            const std::vector<ConversationSessionInfo> &sessions,
                            const std::string &) {
                    if (!loaded)
                    {
                        responder(jsonError(500, "Internal Server Error",
                                            "failed to list chat sessions"));
                        return;
                    }
                    nlohmann::json body;
                    body["sessions"] = nlohmann::json::array();
                    for (size_t i = 0; i < sessions.size(); ++i)
                    {
                        body["sessions"].push_back({
                            {"session_id", sessions[i].sessionId},
                            {"title", sessions[i].title},
                            {"created_at_ms", sessions[i].createdAtMs},
                            {"updated_at_ms", sessions[i].updatedAtMs},
                            {"turn_count", sessions[i].turnCount}
                        });
                    }
                    HttpResponse response(true);
                    response.setStatusCode(200);
                    response.setStatusMessage("OK");
                    response.setContentType("application/json; charset=utf-8");
                    response.setBody(body.dump() + "\n");
                    responder(response);
                });
            if (status == AgentDemoService::kQueueFull)
            {
                HttpResponse response = jsonError(
                    503, "Service Unavailable", "agent task queue is full");
                response.addHeader("Retry-After", "1");
                responder(response);
            }
            return true;
        }

        if (request.method() == HttpRequest::kPost)
        {
            std::string contentType = lowerCopy(request.getHeader("Content-Type"));
            const size_t semicolon = contentType.find(';');
            std::string mediaType = contentType.substr(0, semicolon);
            while (!mediaType.empty() &&
                   std::isspace(static_cast<unsigned char>(mediaType.back())))
            {
                mediaType.pop_back();
            }
            size_t first = 0;
            while (first < mediaType.size() &&
                   std::isspace(static_cast<unsigned char>(mediaType[first])))
            {
                ++first;
            }
            mediaType.erase(0, first);
            if (mediaType != "application/json")
            {
                responder(jsonError(415, "Unsupported Media Type",
                                    "Content-Type must be application/json"));
                return true;
            }

            std::string title;
            try
            {
                const nlohmann::json body = nlohmann::json::parse(request.body());
                if (!body.is_object() ||
                    (body.contains("title") && !body["title"].is_string()))
                {
                    throw std::invalid_argument("title must be a string");
                }
                if (body.contains("title")) title = body["title"].get<std::string>();
            }
            catch (const std::exception &)
            {
                responder(jsonError(400, "Bad Request",
                                    "JSON title must be a string"));
                return true;
            }

            AgentDemoService::SubmitStatus status = service.createHttpSessionAsync(
                title,
                [responder](bool created, const ConversationSessionInfo &session,
                            const std::string &) {
                    if (!created)
                    {
                        responder(jsonError(500, "Internal Server Error",
                                            "failed to create chat session"));
                        return;
                    }
                    nlohmann::json body = {
                        {"session_id", session.sessionId}, {"title", session.title}
                    };
                    HttpResponse response(true);
                    response.setStatusCode(201);
                    response.setStatusMessage("Created");
                    response.setContentType("application/json; charset=utf-8");
                    response.setBody(body.dump() + "\n");
                    responder(response);
                });
            if (status == AgentDemoService::kInvalidRequest)
            {
                responder(jsonError(400, "Bad Request", "title is invalid or too large"));
            }
            else if (status == AgentDemoService::kQueueFull)
            {
                HttpResponse response = jsonError(
                    503, "Service Unavailable", "agent task queue is full");
                response.addHeader("Retry-After", "1");
                responder(response);
            }
            return true;
        }

        HttpResponse response = jsonError(405, "Method Not Allowed", "method not allowed");
        response.addHeader("Allow", "GET, POST");
        responder(response);
        return true;
    }

    if (request.path() != "/agent/run" && request.path() != "/agent/clear")
    {
        return false;
    }

    if (request.method() != HttpRequest::kPost)
    {
        HttpResponse response = jsonError(405, "Method Not Allowed", "method not allowed");
        response.addHeader("Allow", "POST");
        responder(response);
        return true;
    }

    std::string contentType = lowerCopy(request.getHeader("Content-Type"));
    const size_t semicolon = contentType.find(';');
    std::string mediaType = contentType.substr(0, semicolon);
    while (!mediaType.empty() && std::isspace(static_cast<unsigned char>(mediaType.back())))
    {
        mediaType.pop_back();
    }
    size_t first = 0;
    while (first < mediaType.size() &&
           std::isspace(static_cast<unsigned char>(mediaType[first])))
    {
        ++first;
    }
    mediaType.erase(0, first);
    if (mediaType != "application/json")
    {
        responder(jsonError(415, "Unsupported Media Type", "Content-Type must be application/json"));
        return true;
    }

    std::string sessionId;
    std::string message;
    try
    {
        // 正式 parser 会拒绝非法 JSON，并正确还原 \uXXXX 等 Unicode 转义。
        const nlohmann::json body = nlohmann::json::parse(request.body());
        if (!body.is_object() || !body.contains("session_id") ||
            !body["session_id"].is_string() ||
            (request.path() == "/agent/run" &&
             (!body.contains("message") || !body["message"].is_string())))
        {
            throw std::invalid_argument("required string field is missing");
        }
        sessionId = body["session_id"].get<std::string>();
        if (request.path() == "/agent/run")
        {
            message = body["message"].get<std::string>();
        }
    }
    catch (const std::exception &)
    {
        responder(jsonError(400, "Bad Request", request.path() == "/agent/run"
            ? "JSON requires string session_id and message"
            : "JSON requires string session_id"));
        return true;
    }

    if (request.path() == "/agent/clear")
    {
        AgentDemoService::SubmitStatus clearStatus = service.clearSessionAsync(
            std::string("http:") + sessionId,
            [sessionId, responder](bool cleared) {
                if (!cleared)
                {
                    responder(jsonError(500, "Internal Server Error",
                                        "failed to clear conversation"));
                    return;
                }
                HttpResponse response(true);
                response.setStatusCode(200);
                response.setStatusMessage("OK");
                response.setContentType("application/json; charset=utf-8");
                nlohmann::json body = {
                    {"session_id", sessionId}, {"cleared", true}
                };
                response.setBody(body.dump() + "\n");
                responder(response);
            });
        if (clearStatus == AgentDemoService::kSessionBusy)
        {
            responder(jsonError(409, "Conflict",
                                "this session is already processing a request"));
        }
        else if (clearStatus == AgentDemoService::kQueueFull)
        {
            HttpResponse response = jsonError(503, "Service Unavailable",
                                              "agent task queue is full");
            response.addHeader("Retry-After", "1");
            responder(response);
        }
        else if (clearStatus == AgentDemoService::kInvalidRequest)
        {
            responder(jsonError(400, "Bad Request", "invalid session_id"));
        }
        return true;
    }

    AgentDemoService::SubmitStatus status = service.submit(
        std::string("http:") + sessionId, message,
        [sessionId, responder](AgentDemoService::AgentResult result) {
            // 这个 lambda 在 worker 线程执行；responder 内部负责连接生命周期。
            if (!result.ok())
            {
                HttpResponse response(true);
                if (result.error == AgentDemoService::AgentResult::kNotConfigured)
                {
                    response = agentJsonError(503, "Service Unavailable",
                        "AGENT_NOT_CONFIGURED", "agent model is not configured", result);
                }
                else if (result.error == AgentDemoService::AgentResult::kUpstreamTimeout)
                {
                    response = agentJsonError(504, "Gateway Timeout",
                        "UPSTREAM_TIMEOUT", "agent upstream request timed out", result);
                }
                else if (result.error == AgentDemoService::AgentResult::kUpstreamAuthentication)
                {
                    response = agentJsonError(503, "Service Unavailable",
                        "UPSTREAM_AUTHENTICATION", "agent upstream authentication failed", result);
                }
                else if (result.error == AgentDemoService::AgentResult::kUpstreamPaymentRequired)
                {
                    response = agentJsonError(503, "Service Unavailable",
                        "UPSTREAM_PAYMENT_REQUIRED", "agent upstream account is unavailable", result);
                }
                else if (result.error == AgentDemoService::AgentResult::kUpstreamRateLimited)
                {
                    response = agentJsonError(429, "Too Many Requests",
                        "UPSTREAM_RATE_LIMITED", "agent upstream rate limit exceeded", result);
                    response.addHeader("Retry-After", "1");
                }
                else if (result.error == AgentDemoService::AgentResult::kUpstreamRejectedRequest)
                {
                    response = agentJsonError(502, "Bad Gateway",
                        "UPSTREAM_REJECTED_REQUEST", "agent upstream rejected the request", result);
                }
                else if (result.error == AgentDemoService::AgentResult::kUpstreamUnavailable)
                {
                    response = agentJsonError(503, "Service Unavailable",
                        "UPSTREAM_UNAVAILABLE", "agent upstream is temporarily unavailable", result);
                    response.addHeader("Retry-After", "1");
                }
                else if (result.error == AgentDemoService::AgentResult::kUpstreamError)
                {
                    response = agentJsonError(502, "Bad Gateway",
                        "UPSTREAM_ERROR", "agent upstream request failed", result);
                }
                else if (result.error == AgentDemoService::AgentResult::kInvalidUpstreamResponse)
                {
                    response = agentJsonError(502, "Bad Gateway",
                        "INVALID_UPSTREAM_RESPONSE", "agent upstream returned an invalid response", result);
                }
                else if (result.error == AgentDemoService::AgentResult::kRunDeadlineExceeded)
                {
                    response = agentJsonError(504, "Gateway Timeout",
                        "AGENT_RUN_DEADLINE_EXCEEDED", "agent run deadline was exceeded", result);
                }
                else if (result.error == AgentDemoService::AgentResult::kExecutionLimit)
                {
                    response = agentJsonError(502, "Bad Gateway",
                        "AGENT_EXECUTION_LIMIT", "agent execution budget was exceeded", result);
                }
                else
                {
                    response = agentJsonError(500, "Internal Server Error",
                        "INTERNAL_ERROR", "internal agent error", result);
                }
                responder(response);
                return;
            }

            HttpResponse response(true);
            response.setStatusCode(200);
            response.setStatusMessage("OK");
            response.setContentType("application/json; charset=utf-8");
            nlohmann::json body;
            body["session_id"] = sessionId;
            body["run_id"] = result.runId;
            body["answer"] = result.answer;
            body["metrics"] = agentMetricsJson(result);
            if (!result.toolName.empty() && result.toolName != "none")
            {
                body["tool"] = result.toolName;
                body["tool_result"] = result.toolResult;
            }
            if (!result.toolExecutions.empty())
            {
                body["tool_calls"] = nlohmann::json::array();
                for (size_t i = 0; i < result.toolExecutions.size(); ++i)
                {
                    const AgentToolExecution &execution = result.toolExecutions[i];
                    body["tool_calls"].push_back({
                        {"id", execution.toolCallId},
                        {"name", execution.toolName},
                        {"ok", execution.success},
                        {"latency_ms", execution.latencyMs},
                        {"result", execution.output}
                    });
                }
            }
            response.setBody(body.dump() + "\n");
            responder(response);
        });

    if (status == AgentDemoService::kAccepted)
    {
        return true;
    }
    if (status == AgentDemoService::kSessionBusy)
    {
        responder(jsonError(409, "Conflict", "this session is already processing a request"));
    }
    else if (status == AgentDemoService::kQueueFull)
    {
        // Retry-After 告诉客户端这是暂时过载，而不是请求永久无效。
        HttpResponse response = jsonError(503, "Service Unavailable", "agent task queue is full");
        response.addHeader("Retry-After", "1");
        responder(response);
    }
    else
    {
        responder(jsonError(400, "Bad Request", "invalid session_id or message"));
    }
    return true;
}

void onHttpRequest(const HttpRequest &request, HttpResponse *response)
{
    /*
     * 这是 HTTP 协议层之上的最小业务路由示例。
     * HttpServer 已经保证 request 是一条完整请求；这里不需要再处理 TCP 半包、
     * Header 或 Content-Length，只需根据 Method + Path 决定返回内容。
     *
     * 【常见面试问题】什么是路由？
     * 路由就是把 Method + Path 映射到处理函数。例如：
     *     GET  /health   -> 健康检查
     *     POST /agent/run -> 异步 Agent 请求
     * 当前路由很少，直接使用 if 足够；只有路由数量增长后才值得抽象 Router 类。
     */
    response->setContentType("application/json; charset=utf-8");
    if (request.method() == HttpRequest::kUnsupported)
    {
        response->setStatusCode(501);
        response->setStatusMessage("Not Implemented");
        response->setBody("{\"error\":\"method not implemented\"}\n");
        return;
    }

    if (request.method() == HttpRequest::kGet && request.path() == "/health")
    {
        response->setStatusCode(200);
        response->setStatusMessage("OK");
        /*
         * service 是本地启动器的身份标记。只检查“18081 返回 2xx”可能误把其他服务
         * 当成本项目，并把聊天 Session 发给错误进程；固定标记让双击脚本可以验证身份。
         */
        response->setBody(
            "{\"service\":\"cpp-webserver-agent\",\"status\":\"ok\"}\n");
        return;
    }

    if (request.path() == "/health")
    {
        // 路径存在但方法不允许，405 比 404 更准确；Allow 告诉客户端可用方法。
        response->setStatusCode(405);
        response->setStatusMessage("Method Not Allowed");
        response->addHeader("Allow", "GET");
        response->setBody("{\"error\":\"method not allowed\"}\n");
        return;
    }

    if (request.method() == HttpRequest::kPost && request.path() == "/echo")
    {
        // /echo 不调用 Agent，只原样返回 Body，用于独立验证 POST 和 Content-Length。
        response->setStatusCode(200);
        response->setStatusMessage("OK");
        response->setContentType("text/plain; charset=utf-8");
        response->setBody(request.body());
        return;
    }

    if (request.path() == "/echo")
    {
        response->setStatusCode(405);
        response->setStatusMessage("Method Not Allowed");
        response->addHeader("Allow", "POST");
        response->setBody("{\"error\":\"method not allowed\"}\n");
        return;
    }

    // 正常情况下 /agent/run 已由异步回调接管；这里保留方法错误的同步兜底。
    if (request.path() == "/agent/run")
    {
        response->setStatusCode(405);
        response->setStatusMessage("Method Not Allowed");
        response->addHeader("Allow", "POST");
        response->setBody("{\"error\":\"method not allowed\"}\n");
        return;
    }

    response->setStatusCode(404);
    response->setStatusMessage("Not Found");
    response->setBody("{\"error\":\"not found\"}\n");
}

AsyncLogging* g_asyncLog = NULL;
AsyncLogging * getAsyncLog(){
    return g_asyncLog;
}
 void asyncLog(const char* msg, int len)
{
    AsyncLogging* logging = getAsyncLog();
    if (logging)
    {
        logging->append(msg, len);
    }
}
int main(int argc,char *argv[]) {
    // 对端关闭连接后，写操作通过错误码处理，避免SIGPIPE终止整个进程。
    ::signal(SIGPIPE, SIG_IGN);

    bool qpsMode = false;
    int qpsPort = 18082;
    int qpsThreads = 3;
    try
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string argument(argv[i]);
            if (argument == "--qps")
            {
                qpsMode = true;
            }
            else if (!parsePositiveIntOption(argument, "--port=", 1, 65535, &qpsPort) &&
                     !parsePositiveIntOption(argument, "--threads=", 1, 128, &qpsThreads))
            {
                throw std::invalid_argument(std::string("unknown option: ") + argument);
            }
        }
    }
    catch (const std::exception &ex)
    {
        std::cerr << ex.what() << "\nUsage: " << argv[0]
                  << " [--qps] [--port=18082] [--threads=3]" << std::endl;
        return 1;
    }
    if (qpsMode)
    {
        return runQpsServer(qpsPort, qpsThreads);
    }
    if (argc > 1)
    {
        std::cerr << "--port and --threads are only valid together with --qps" << std::endl;
        return 1;
    }

    // libcurl 要求全局初始化先于可能调用它的所有线程，因此必须放在日志线程之前。
    initializeAgentRuntime();

    //第一步启动日志，双缓冲异步写入磁盘.
    //创建一个文件夹
    const std::string LogDir="logs";
    mkdir(LogDir.c_str(),0755);//创建 LogDir字符串所指定的目录，并赋予其权限：所有者可读、写、执行，同组用户和其他用户可读、执行。
    //使用std::stringstream 构建日志文件夹
    std::ostringstream LogfilePath;
    LogfilePath << LogDir << "/" << ::basename(argv[0]); // 完整的日志文件路径
    AsyncLogging log(LogfilePath.str(), kRollSize);//创建日志对象
    g_asyncLog = &log;
    Logger::setOutput(asyncLog); // 为Logger设置输出回调, 重新配接输出位置
    log.start(); // 开启日志后端线程
    ///////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////
    //第二步启动内存池和LFU缓存
     // 初始化内存池
    memoryPool::HashBucket::initMemoryPool();

    // 初始化缓存
    const int CAPACITY = 5;  
    Cache::KLfuCache<int, std::string> lfu(CAPACITY);
    ///////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////
    //第三步启动底层网络模块
    EventLoop loop;
    uint16_t port = getAgentDemoConfig().port;
    InetAddress addr(port);

    // 原有 TCP 文本 Agent 继续监听配置中的端口（默认 18080）。
    AgentDemoService agentService;
    AgentServer server(&loop, addr, "AgentServer", agentService);

    /*
     * HTTP 学习服务独立监听 18081，与 TCP Agent 共用同一个 base EventLoop，
     * 但 HttpServer 内部拥有自己的 TcpServer 和 IO 线程池。两个协议互不干扰，
     * 便于在学习 HTTP 时继续保留原有 Agent 演示。
     *
     * 【线程模型】
     * main 函数中的 loop 是 baseLoop，负责两个监听 Socket 的接入事件；每个 TcpServer
     * 又创建自己的 subLoop 线程处理已连接 Socket。这仍然符合主从 Reactor：
     * main Reactor 负责 accept，sub Reactor 负责连接上的读写事件。
     */
    InetAddress httpAddr(18081);
    HttpServer httpServer(&loop, httpAddr, "HttpServer");
    httpServer.setThreadNum(2);
    httpServer.setHttpCallback(onHttpRequest);
    httpServer.setStreamingHttpCallback(
        std::bind(onStreamingHttpRequest, std::ref(agentService),
                  std::placeholders::_1, std::placeholders::_2));
    httpServer.setAsyncHttpCallback(
        std::bind(onAsyncHttpRequest, std::ref(agentService),
                  std::placeholders::_1, std::placeholders::_2));
    server.start();
    httpServer.start();
    std::cout << "=============================================Start Agent Demo Server=============================================" << std::endl;
    std::cout << "Plain text long-connection chat on port " << port << std::endl;
    std::cout << "Connect with: nc 127.0.0.1 " << port << std::endl;
    std::cout << "Commands: /health /clear /quit" << std::endl;
    std::cout << "Config file: " << getAgentDemoConfig().configPath << std::endl;
    std::cout << "HTTP health check: curl http://127.0.0.1:18081/health" << std::endl;
    std::cout << "HTTP Agent API: POST http://127.0.0.1:18081/agent/run" << std::endl;
    std::cout << "HTTP Agent SSE: POST http://127.0.0.1:18081/agent/run/stream" << std::endl;
    loop.loop();
    // 必须先等待业务任务结束，再让 HttpServer/AgentServer 析构其 IO loop 和连接。
    agentService.stop();
    std::cout << "==============================================Stop Agent Demo Server==============================================" << std::endl;
}
/*
cd /mnt/f/webserver/webserver
cmake --build build
cd bin
./main
*/
/*
nc 127.0.0.1 18080
*/
