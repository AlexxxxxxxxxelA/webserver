#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "AgentRuntime.h"

/**
 * 【Session、Run、Turn 三个生命周期】
 *
 * Session：一段多轮对话的命名空间和顺序边界，例如 http:demo。
 * Run：一次 Agent 执行尝试，内部可有多次模型调用和工具调用，也可能失败或取消。
 * Turn：成功持久化的“一条 user 输入 -> 一条最终 assistant 回答”。
 *
 * 一个 Session 包含多个 Turn；失败或取消的 Run 不产生 Turn。当前成功时使用 runId
 * 作为 turnId 便于关联日志，但 Run 和 Turn 的语义、生命周期并不相同。
 */

struct ConversationTurn
{
    ConversationTurn() : sequence(0), estimatedTokens(0) {}

    int64_t sequence;
    std::string turnId;
    std::string userMessage;
    std::string assistantMessage;
    std::vector<AgentToolExecution> toolExecutions;
    size_t estimatedTokens;
};

struct ConversationSummary
{
    ConversationSummary() : version(0), coveredUntilSequence(0) {}

    int64_t version;
    int64_t coveredUntilSequence;
    std::string content;
};

struct ConversationSnapshot
{
    ConversationSummary summary;
    // 只包含 summary 覆盖范围之后的原始 Turn，并按 sequence 升序排列。
    std::vector<ConversationTurn> turns;
};

struct ConversationSessionInfo
{
    ConversationSessionInfo()
        : createdAtMs(0), updatedAtMs(0), turnCount(0) {}

    std::string sessionId;
    std::string title;
    int64_t createdAtMs;
    int64_t updatedAtMs;
    size_t turnCount;
};

/**
 * 完整历史存储接口。AgentDemoService 只依赖该接口，不依赖 sqlite3 C API。
 *
 * Store 的方法允许阻塞，只能由业务 worker 或离线测试调用，不能在 EventLoop 中执行。
 *
 * 四个容易混淆的概念：
 * - History/Store：SQLite 保存的完整成功 Turn；
 * - Context：本次真正选择送给模型的历史子集；
 * - Memory：提取并长期复用的用户事实/偏好，本项目尚未实现；
 * - RAG：按当前问题从外部知识库检索资料，本项目尚未实现。
 * 滚动摘要只是历史 Context 压缩，不是长期 Memory，也不是 RAG。
 */
class ConversationStore
{
public:
    virtual ~ConversationStore() {}

    virtual ConversationSnapshot load(const std::string &sessionId,
                                      size_t maxRecentTurns) const = 0;
    virtual void createSession(const std::string &sessionId,
                               const std::string &title) = 0;
    virtual std::vector<ConversationSessionInfo> listSessions(
        const std::string &sessionPrefix, size_t limit) const = 0;
    virtual int64_t saveTurn(const std::string &sessionId,
                             const ConversationTurn &turn) = 0;
    virtual void saveSummary(const std::string &sessionId,
                             const ConversationSummary &summary) = 0;
    virtual void deleteSession(const std::string &sessionId) = 0;
};

/**
 * SQLite 单机实现。每个操作打开独立 Connection，避免多个 worker 共享 sqlite3 handle
 * 或 prepared statement。WAL 允许读与短写事务更好地并发；一次成功 Turn 在一个
 * BEGIN IMMEDIATE 事务内写入，绝不留下半个 Turn。
 *
 * SQLite 事务只保证 saveTurn 内部的 sequence 分配与 Turn 写入原子；整个
 * load -> model/tool -> save 的业务串行性由 AgentDemoService::Session::inFlight 保证。
 * WAL 改善读写并发，但不代表 SQLite 可以无限并发写。
 */
class SQLiteConversationStore : public ConversationStore
{
public:
    explicit SQLiteConversationStore(const std::string &databasePath);

    ConversationSnapshot load(const std::string &sessionId,
                              size_t maxRecentTurns) const override;
    void createSession(const std::string &sessionId,
                       const std::string &title) override;
    std::vector<ConversationSessionInfo> listSessions(
        const std::string &sessionPrefix, size_t limit) const override;
    int64_t saveTurn(const std::string &sessionId,
                     const ConversationTurn &turn) override;
    void saveSummary(const std::string &sessionId,
                     const ConversationSummary &summary) override;
    void deleteSession(const std::string &sessionId) override;

    const std::string &databasePath() const { return databasePath_; }

private:
    void initializeSchema();

    std::string databasePath_;
};

/**
 * 第一版 Token 估算器不追求与 DeepSeek tokenizer 完全一致，而是保守、确定、可测试：
 * ASCII 约 4 字符一个 Token，非 ASCII UTF-8 code point 按一个 Token，再加消息结构开销。
 * 实际误差可使用已有 usage.prompt_tokens 持续校准。
 *
 * Token 是模型 tokenizer 的单位，不等于字符、Unicode code point 或 UTF-8 字节。
 * 本估算值用于请求前裁剪历史；Provider usage 是请求完成后的实际报告值，并且还包含
 * system、当前 user、工具定义等本估算器没有覆盖的内容，两者不应要求完全相等。
 */
class TokenEstimator
{
public:
    size_t estimateText(const std::string &text) const;
    size_t estimateMessage(const AgentConversationMessage &message) const;
    size_t estimateTurn(const ConversationTurn &turn) const;
};

struct ContextBuildResult
{
    ContextBuildResult()
        : estimatedTokens(0), recentTurns(0), summaryUsed(false) {}

    std::vector<AgentConversationMessage> history;
    size_t estimatedTokens;
    size_t recentTurns;
    bool summaryUsed;
};

/**
 * ContextBuilder 只构造“历史部分”。完整 Provider 请求还会由 AgentRuntime 加入 system、
 * 当前 user、tools，以及本 Run 中新产生的 assistant tool_calls 和 tool results。
 */
class ContextBuilder
{
public:
    ContextBuilder(size_t historyTokenBudget = 8000,
                   size_t summaryTokenBudget = 1200,
                   size_t recentTurnsToKeep = 8);

    ContextBuildResult build(const ConversationSnapshot &snapshot) const;

    /**
     * 将已经离开 recent window 的完整 Turn 做确定性摘录。摘要只是一份派生数据，
     * coveredUntilSequence 表示它覆盖的原始范围；数据库中的原始 Turn 不会被删除。
     * 这是确定性历史摘录，不会判断长期事实，也不会按当前问题检索，因此不是语义
     * Memory 提取或 RAG。
     */
    ConversationSummary extendSummary(
        const ConversationSummary &existing,
        const std::vector<ConversationTurn> &turns) const;

    size_t recentTurnsToKeep() const { return recentTurnsToKeep_; }

private:
    std::string truncateToTokenBudget(const std::string &text,
                                      size_t tokenBudget) const;

    size_t historyTokenBudget_;
    size_t summaryTokenBudget_;
    size_t recentTurnsToKeep_;
    TokenEstimator estimator_;
};
