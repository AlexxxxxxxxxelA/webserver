#pragma once

#include <chrono>
#include <mutex>
#include <memory>
#include <functional>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>

#include "Buffer.h"
#include "Callbacks.h"
#include "Timestamp.h"
#include "BoundedThreadPool.h"
#include "AgentRuntime.h"
#include "ConversationContext.h"

// 必须在程序启动任何后台线程前调用，完成 libcurl 全局初始化。
void initializeAgentRuntime();

struct AgentDemoConfig
{
    uint16_t port;
    std::string deepseekApiKey;
    std::string deepseekApiUrl;
    std::string deepseekModel;
    bool deepseekThinkingEnabled;
    std::string weatherApiBaseUrl;
    std::string conversationDatabasePath;
    std::string configPath;
};

class AgentDemoService
{
public:
    /**
     * Agent 核心返回结构，不直接绑定 TCP 文本或 HTTP JSON 格式。
     * TCP 层可以把它格式化成人类可读文本，HTTP 层可以映射成状态码和 JSON。
     * 这体现“业务结果”和“传输协议”分离。
     */
    struct AgentResult
    {
        enum Error
        {
            kNone,
            kNotConfigured,
            kUpstreamTimeout,
            kUpstreamAuthentication,
            kUpstreamPaymentRequired,
            kUpstreamRateLimited,
            kUpstreamRejectedRequest,
            kUpstreamUnavailable,
            kUpstreamError,
            kInvalidUpstreamResponse,
            kRunDeadlineExceeded,
            kCancelled,
            kExecutionLimit,
            kInternalError
        };

        AgentResult()
            : error(kNone), queueWaitMs(0), totalLatencyMs(0), historyLoadMs(0),
              historySaveMs(0), contextEstimatedTokens(0), contextRecentTurns(0),
              summaryUsed(false) {}
        bool ok() const { return error == kNone; }

        Error error;
        std::string runId;
        std::string answer;
        std::string toolName;
        std::string toolResult;
        // 完整保存一次 Run 的全部工具执行；toolName/toolResult 兼容旧的单工具响应。
        std::vector<AgentToolExecution> toolExecutions;
        std::string errorMessage;
        long queueWaitMs;
        long totalLatencyMs;
        AgentRunMetrics metrics;
        long historyLoadMs;
        long historySaveMs;
        size_t contextEstimatedTokens;
        size_t contextRecentTurns;
        bool summaryUsed;
    };

    enum SubmitStatus
    {
        kAccepted,       // 任务已进入业务线程池，稍后通过 Completion 返回
        kSessionBusy,    // 同一 session 已有一个进行中的请求
        kQueueFull,      // 系统过载，业务队列或 session 容量暂不可用
        kInvalidRequest  // session_id/message 不符合长度或字符限制
    };

    // Completion 运行在业务 worker 线程，调用方不能假设它在 EventLoop 线程。
    using Completion = std::function<void(AgentResult)>;
    using SessionCreateCompletion =
        std::function<void(bool, const ConversationSessionInfo &, const std::string &)>;
    using SessionListCompletion =
        std::function<void(bool, const std::vector<ConversationSessionInfo> &,
                           const std::string &)>;

    AgentDemoService();
    ~AgentDemoService();

    void onConnection(const TcpConnectionPtr &conn);
    void onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp receiveTime);
    SubmitStatus submit(const std::string &sessionId,
                        const std::string &message,
                        Completion completion);
    // eventCallback 与 Completion 一样，会同步运行在业务 worker/Provider callback 调用栈
    // 中，必须快速且线程安全；真正的 Socket Write 由 HttpStreamResponder 投递回连接
    // 所属 EventLoop。
    SubmitStatus submitStreaming(
        const std::string &sessionId,
        const std::string &message,
        const AgentEventCallback &eventCallback,
        const AgentModelClient::CancelCheck &cancelled,
        Completion completion);
    SubmitStatus clearSessionAsync(const std::string &sessionId,
                                   std::function<void(bool)> completion);
    SubmitStatus createHttpSessionAsync(const std::string &title,
                                        SessionCreateCompletion completion);
    SubmitStatus listHttpSessionsAsync(size_t limit,
                                      SessionListCompletion completion);
    bool isConfigured() const;
    void stop();

private:
    struct Session
    {
        /*
         * 这是进程内“并发控制对象”，不是聊天历史本身。SQLite 保存 sessionId 下的
         * 多个 Turn；每次 submit 创建一个 Run，inFlight 保证同一 Session 的 Run 不交叉。
         */
        Session() : inFlight(false), deleteWhenIdle(false), lastAccess(0) {}
        std::mutex mutex; // 只保护 inFlight；完整历史存储在 SQLite
        bool inFlight;    // 保证同一对话一次只执行一个完整 Agent turn
        bool deleteWhenIdle; // TCP 断开时置位，由业务 worker 删除持久化临时会话
        // 由 AgentDemoService::mutex_ 保护，用于容量满时淘汰最久未使用会话。
        size_t lastAccess;
    };

    AgentResult runTurn(const std::shared_ptr<Session> &session,
                        const std::string &sessionId,
                        const std::string &runId,
                        const std::string &message,
                        const std::chrono::steady_clock::time_point &deadline,
                        const AgentEventCallback &eventCallback,
                        const AgentModelClient::CancelCheck &cancelled,
                        bool streaming);
    SubmitStatus submitInternal(
        const std::string &sessionId,
        const std::string &message,
        const AgentEventCallback &eventCallback,
        const AgentModelClient::CancelCheck &cancelled,
        Completion completion,
        bool streaming);
    AgentResult makeAgentResult(const AgentRunResult &runtimeResult);
    std::string formatChatReply(const std::string &answer,
                                const std::string &toolName,
                                const std::string &toolResult) const;
    std::shared_ptr<Session> getOrCreateSession(const std::string &sessionId);
    void eraseSession(const std::string &sessionId);
    void deleteSessionWhenIdle(const std::string &sessionId);
    void eraseRejectedEmptySession(const std::string &sessionId,
                                   const std::shared_ptr<Session> &session);

    // TCP 按行协议的半包缓存：连接名 -> 尚未遇到 '\n' 的字节。
    std::unordered_map<std::string, std::string> pendingRequests_;
    // HTTP/TCP 共享 Agent 核心，但使用 http:/tcp: 前缀隔离会话命名空间。
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
    // 只保护两个全局 map 和访问序号；绝不能持有它等待 DeepSeek。
    mutable std::mutex mutex_;
    size_t accessSequence_;
    /*
     * 线程职责：
     * - IO/EventLoop：解析请求、快速校验、切换 inFlight、非阻塞入队；
     * - business worker：SQLite、AgentRuntime、同步 libcurl 和工具执行；
     * - 连接 EventLoop：真正 Socket Write；日志后台线程负责最终落盘。
     * AgentRuntime 本身不创建线程。
     */
    std::shared_ptr<AgentRuntime> runtime_;
    std::shared_ptr<ConversationStore> conversationStore_;
    ContextBuilder contextBuilder_;
    BoundedThreadPool businessPool_;
};

const AgentDemoConfig &getAgentDemoConfig();
