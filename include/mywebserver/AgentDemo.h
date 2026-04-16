#pragma once

#include <mutex>
#include <memory>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>

#include "Buffer.h"
#include "Callbacks.h"
#include "Timestamp.h"

class ThreadPool;

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
    AgentDemoService();
    ~AgentDemoService();

    void onConnection(const TcpConnectionPtr &conn);
    void onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp receiveTime);

private:
    struct ChatMessage
    {
        std::string role;
        std::string content;
    };

    std::string handleChatLine(const std::string &connectionName, const std::string &message);
    std::string buildConversationContext(const std::vector<ChatMessage> &history) const;
    std::string formatChatReply(const std::string &answer,
                                const std::string &toolName,
                                const std::string &toolResult) const;
    void appendHistory(const std::string &connectionName,
                       const std::string &userMessage,
                       const std::string &assistantMessage);

    std::unordered_map<std::string, std::string> pendingRequests_;
    std::unordered_map<std::string, std::vector<ChatMessage> > conversations_;
    mutable std::mutex mutex_;
    std::unique_ptr<ThreadPool> workerPool_;
};

std::string jsonEscape(const std::string &input);
bool jsonGetString(const std::string &json, const std::string &key, std::string *value);

const AgentDemoConfig &getAgentDemoConfig();
