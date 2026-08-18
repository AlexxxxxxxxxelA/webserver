#pragma once

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

// 必须在程序启动任何后台线程前调用，完成 libcurl 全局初始化。
void initializeAgentRuntime();

struct AgentDemoConfig
{
    uint16_t port;
    std::string deepseekApiKey;
    std::string deepseekApiUrl;
    std::string deepseekModel;
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
            kUpstreamError,
            kInternalError
        };

        AgentResult() : error(kNone) {}
        bool ok() const { return error == kNone; }

        Error error;
        std::string answer;
        std::string toolName;
        std::string toolResult;
        std::string errorMessage;
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

    AgentDemoService();
    ~AgentDemoService();

    void onConnection(const TcpConnectionPtr &conn);
    void onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp receiveTime);
    SubmitStatus submit(const std::string &sessionId,
                        const std::string &message,
                        Completion completion);
    bool clearSession(const std::string &sessionId);
    bool isConfigured() const;
    void stop();

private:
    struct ChatMessage
    {
        std::string role;
        std::string content;
    };

    struct Session
    {
        Session() : inFlight(false), lastAccess(0) {}
        std::mutex mutex; // 保护 inFlight 和 history
        bool inFlight;    // 保证同一对话一次只执行一个完整 Agent turn
        // 由 AgentDemoService::mutex_ 保护，用于容量满时淘汰最久未使用会话。
        size_t lastAccess;
        std::vector<ChatMessage> history;
    };

    AgentResult runTurn(const std::shared_ptr<Session> &session,
                        const std::string &message);
    std::string buildConversationContext(const std::vector<ChatMessage> &history) const;
    std::string formatChatReply(const std::string &answer,
                                const std::string &toolName,
                                const std::string &toolResult) const;
    void appendHistory(const std::shared_ptr<Session> &session,
                       const std::string &userMessage,
                       const std::string &assistantMessage);
    std::shared_ptr<Session> getOrCreateSession(const std::string &sessionId);
    void eraseSession(const std::string &sessionId);
    void eraseRejectedEmptySession(const std::string &sessionId,
                                   const std::shared_ptr<Session> &session);

    // TCP 按行协议的半包缓存：连接名 -> 尚未遇到 '\n' 的字节。
    std::unordered_map<std::string, std::string> pendingRequests_;
    // HTTP/TCP 共享 Agent 核心，但使用 http:/tcp: 前缀隔离会话命名空间。
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
    // 只保护两个全局 map 和访问序号；绝不能持有它等待 DeepSeek。
    mutable std::mutex mutex_;
    size_t accessSequence_;
    BoundedThreadPool businessPool_;
};

const AgentDemoConfig &getAgentDemoConfig();
