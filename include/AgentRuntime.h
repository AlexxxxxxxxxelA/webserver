#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

/**
 * 【先理解：LLM 和 Agent 不是同一个东西】
 *
 * LLM（大语言模型）可以把它理解成“根据输入消息，预测下一条输出”的生成器。一次
 * HTTP 请求 DeepSeek，只是一次 Model Call。模型可以输出文字，也可以建议调用工具，
 * 但它自己不会执行服务器中的 C++ 函数。
 *
 * Agent 是 LLM 外面的一层应用系统：它保存上下文，让模型选择工具，由服务器校验并
 * 执行工具，再把结果送回模型；同时还负责循环终止、次数预算、deadline、取消和日志。
 * 本文件定义的 AgentRuntime 就是这层“可控的执行循环”。
 */

/**
 * 模型要求执行的一次函数调用。
 *
 * argumentsJson 保留模型返回的原始 JSON 字符串，而不是提前转换成某个 C++ 结构。
 * 原因是每个工具的参数 Schema 不同，统一解析层只能确认它是合法 JSON，具体字段仍应
 * 由对应工具验证。toolCallId 用于把 role=tool 的结果精确关联回这次调用。
 * toolCallId 标识“一次具体调用”，不是工具名、Run ID，也不是写操作的幂等键。
 * 同一轮模型可能两次调用同名工具，所以不能只按工具名关联结果。
 */
struct AgentToolCall
{
    std::string toolCallId;
    std::string name;
    std::string argumentsJson;
};

struct AgentModelResponse
{
    AgentModelResponse()
        : error(kNone), hasReasoningContent(false), providerStatusCode(0) {}

    enum Error
    {
        kNone,
        kNotConfigured,
        kTimeout,
        kAuthentication,
        kPaymentRequired,
        kRateLimited,
        kRejectedRequest,
        kUnavailable,
        kCancelled,
        kUpstreamError,
        kInvalidResponse
    };

    bool ok() const { return error == kNone; }

    Error error;
    /**
     * DeepSeek Thinking 的临时思考内容。它只在当前 Agent Run 的模型响应和局部 messages
     * 中存在：Tool Call 后必须原样回传 Provider，但不会进入 AgentRunResult、SQLite、
     * HTTP、SSE 文本或日志。hasReasoningContent 用于区分“字段缺失”和“空字符串”。
     */
    std::string reasoningContent;
    bool hasReasoningContent;
    std::string content;
    std::vector<AgentToolCall> toolCalls;
    std::string errorMessage;
    int providerStatusCode;

    struct TokenUsage
    {
        TokenUsage()
            : promptTokens(0), completionTokens(0), totalTokens(0),
              promptCacheHitTokens(0), promptCacheMissTokens(0), reasoningTokens(0) {}

        uint64_t promptTokens;
        uint64_t completionTokens;
        uint64_t totalTokens;
        uint64_t promptCacheHitTokens;
        uint64_t promptCacheMissTokens;
        uint64_t reasoningTokens;
    } usage;
};

const char *agentModelErrorName(AgentModelResponse::Error error);

/**
 * 模型供应商适配接口。AgentRuntime 只认识“消息 + 工具定义 -> 模型响应”，不认识
 * libcurl、DeepSeek URL 或 API Key。测试可以注入 FakeModelClient，生产环境则注入
 * DeepSeekClient，从而把不确定的外部网络与确定的 Agent 状态机分开测试。
 *
 * 这里隔离的是认证、HTTP/TLS、URL、响应解析和 Provider 错误。messages/tools 仍采用
 * Chat Completions/Tool Calls 的 JSON 形态，因此“适配接口”不等于已经拥有适用于
 * 所有模型协议的完全通用中间表示。
 */
class AgentModelClient
{
public:
    using TextDeltaCallback = std::function<bool(const std::string &)>;
    // true=开始思考，false=思考阶段结束；回调只传状态，不传 reasoning 文本。
    using ThinkingStateCallback = std::function<bool(bool)>;
    using CancelCheck = std::function<bool()>;

    virtual ~AgentModelClient() {}

    virtual bool isConfigured() const = 0;
    virtual AgentModelResponse complete(
        const std::vector<nlohmann::json> &messages,
        const nlohmann::json &tools,
        long timeoutMs) const = 0;
    /**
     * 流式模型接口。默认实现调用 complete() 并把完整答案作为一个 delta，便于 Fake
     * Client 和非流式 Provider 复用；DeepSeekClient 会覆盖为真正的增量 SSE 解析。
     * 默认 fallback 无法打断正在阻塞的 complete()；只有 Provider 把 CancelCheck 接入
     * 底层 I/O（本项目 DeepSeek 使用 libcurl progress callback）才能及时取消。
     */
    virtual AgentModelResponse completeStreaming(
        const std::vector<nlohmann::json> &messages,
        const nlohmann::json &tools,
        long timeoutMs,
        const TextDeltaCallback &onDelta,
        const ThinkingStateCallback &onThinking,
        const CancelCheck &cancelled) const;
};

struct AgentToolContext
{
    /*
     * 次数预算、deadline、timeout、cancel 是四个概念：
     * - maxModelCalls/maxToolCalls：限制循环执行多少次；
     * - deadline：整个 Run 的绝对截止时间，排队时间也算在内；
     * - timeout：某一次 Provider/Tool 最多等待多久，通常取自身上限与剩余时间较小值；
     * - cancel：外部协作式停止信号，例如 SSE 客户端断开。
     * Runtime 不会“杀掉线程”；长工具必须主动使用本 Context 限制 I/O 并检查取消。
     */
    std::chrono::steady_clock::time_point deadline;
    AgentModelClient::CancelCheck cancelCheck;

    long remainingMilliseconds() const;
    bool cancelled() const { return cancelCheck && cancelCheck(); }
};

struct AgentToolResult
{
    AgentToolResult() : success(false) {}

    bool success;
    std::string output;
    std::string errorMessage;
};

/**
 * 每个工具同时提供给两类调用者：
 * 1. definition() 给模型阅读，说明名称、用途和 JSON Schema；
 * 2. execute() 给服务器调用，执行前仍要在 C++ 中校验参数。
 *
 * 模型输出永远是不可信输入。即使供应商提供 strict schema，服务端也不能跳过长度、
 * 类型、权限和业务范围校验。
 *
 * inputSchema() 描述“模型应该怎样生成参数”；AgentToolCall::argumentsJson 是某一次
 * 调用实际生成的 JSON 文本。Schema 是说明书，C++ 校验才是真正执行边界。
 */
class AgentTool
{
public:
    virtual ~AgentTool() {}

    virtual std::string name() const = 0;
    virtual std::string description() const = 0;
    // 当前 Runtime 只允许无副作用工具；写工具需要审批、幂等键和恢复语义后再开放。
    virtual bool isReadOnly() const = 0;
    virtual nlohmann::json inputSchema() const = 0;
    virtual AgentToolResult execute(const nlohmann::json &arguments,
                                    const AgentToolContext &context) const = 0;
};

struct AgentToolExecution
{
    AgentToolExecution() : success(false), latencyMs(0) {}

    std::string toolCallId;
    std::string toolName;
    bool success;
    std::string output;
    long latencyMs;
};

struct AgentModelExecution
{
    AgentModelExecution()
        : sequence(0), success(false), error(AgentModelResponse::kNone),
          providerStatusCode(0), latencyMs(0) {}

    size_t sequence;
    bool success;
    AgentModelResponse::Error error;
    int providerStatusCode;
    long latencyMs;
    AgentModelResponse::TokenUsage usage;
};

struct AgentRunMetrics
{
    AgentRunMetrics() : modelLatencyMs(0), toolLatencyMs(0) {}

    long modelLatencyMs;
    long toolLatencyMs;
    AgentModelResponse::TokenUsage usage;
    std::vector<AgentModelExecution> modelExecutions;
};

/*
 * AgentModelExecution/AgentToolExecution 是一次 Run 的步骤明细，具有 trace-like 属性；
 * latency 和 usage 是该 Run 的聚合指标。当前它们用于 HTTP 返回和 agent_trace 日志，
 * 还不是 OpenTelemetry 分布式 Trace，也不是跨请求聚合的 Prometheus/QPS/p95 指标。
 */

/**
 * 工具注册表负责稳定地生成 DeepSeek tools 数组，并按名称分发调用。
 * vector 保存注册顺序，保证每次请求中的工具顺序稳定；unordered_map 只用于 O(1)
 * 查找。不能只用 unordered_map 迭代生成定义，否则顺序会随实现和 rehash 改变，给
 * Prompt 回归和请求快照测试带来无意义噪声。
 */
class AgentToolRegistry
{
public:
    void registerTool(const std::shared_ptr<AgentTool> &tool);
    nlohmann::json definitions() const;
    AgentToolResult execute(const AgentToolCall &call,
                            const AgentToolContext &context) const;

private:
    std::vector<std::shared_ptr<AgentTool>> orderedTools_;
    std::unordered_map<std::string, std::shared_ptr<AgentTool>> toolsByName_;
};

/**
 * role 是模型消息协议中的语义，不是登录身份或权限认证：
 * - system：应用制定的高优先级行为规则；
 * - user：用户输入；
 * - assistant：模型输出；
 * - tool：服务器执行工具后的环境观察结果，需要关联 tool_call_id。
 *
 * 当前持久化历史只重建 user 和最终 assistant。一次 Run 内临时产生的 assistant
 * tool_calls 与 tool results 由 AgentRuntime 直接构造，不由 ContextBuilder 伪造。
 */
struct AgentConversationMessage
{
    std::string role;
    std::string content;
};

struct AgentRunResult
{
    AgentRunResult()
        : error(kNone), upstreamError(AgentModelResponse::kNone), modelCalls(0) {}

    enum Error
    {
        kNone,
        kNotConfigured,
        kUpstreamTimeout,
        kUpstreamError,
        kInvalidModelResponse,
        kCancelled,
        kDeadlineExceeded,
        kBudgetExceeded,
        kInternalError
    };

    bool ok() const { return error == kNone; }

    Error error;
    AgentModelResponse::Error upstreamError;
    std::string answer;
    std::string errorMessage;
    size_t modelCalls;
    std::vector<AgentToolExecution> toolExecutions;
    AgentRunMetrics metrics;
};

const char *agentRunErrorName(AgentRunResult::Error error);

struct AgentEvent
{
    std::string type;
    nlohmann::json data;
};

using AgentEventCallback = std::function<bool(const AgentEvent &)>;

struct AgentRunOptions
{
    AgentRunOptions();

    size_t maxModelCalls;
    size_t maxToolCalls;
    long timeoutMs;
};

/**
 * AgentRuntime 是一个有明确终止条件的 Tool Calling 状态机：
 *
 * model -> tool_calls -> validate/execute -> role=tool -> model
 *
 * 模型可以连续选择多个工具，但不能无限运行。maxModelCalls、maxToolCalls 和统一
 * deadline 共同构成执行预算。Runtime 不拥有 Session，也不创建线程，因此同一套
 * 状态机既能被服务器 worker 调用，也能在单元测试中同步执行。
 *
 * 注意：tool_calls 只是模型提出的动作建议，不表示工具已经执行。服务器才是执行者。
 * 当前同一批多个工具按 Provider 返回顺序串行执行；工具失败通常作为一次 observation
 * 回给模型继续判断，不会自动让整个 Run 失败。
 */
class AgentRuntime
{
public:
    AgentRuntime(const std::shared_ptr<AgentModelClient> &modelClient,
                 const std::shared_ptr<AgentToolRegistry> &toolRegistry,
                 const AgentRunOptions &options = AgentRunOptions());

    AgentRunResult run(const std::vector<AgentConversationMessage> &history,
                       const std::string &userMessage) const;
    AgentRunResult runUntil(
        const std::vector<AgentConversationMessage> &history,
        const std::string &userMessage,
        const std::chrono::steady_clock::time_point &deadline) const;
    AgentRunResult runStreamingUntil(
        const std::vector<AgentConversationMessage> &history,
        const std::string &userMessage,
        const std::chrono::steady_clock::time_point &deadline,
        const AgentEventCallback &onEvent,
        const AgentModelClient::CancelCheck &cancelled) const;
    std::chrono::steady_clock::time_point deadlineFromNow() const;
    bool isConfigured() const;

private:
    AgentRunResult runInternal(
        const std::vector<AgentConversationMessage> &history,
        const std::string &userMessage,
        const std::chrono::steady_clock::time_point &deadline,
        const AgentEventCallback &onEvent,
        const AgentModelClient::CancelCheck &cancelled,
        bool streaming) const;
    static bool emitEvent(const AgentEventCallback &callback,
                          const std::string &type,
                          const nlohmann::json &data);

    std::shared_ptr<AgentModelClient> modelClient_;
    std::shared_ptr<AgentToolRegistry> toolRegistry_;
    AgentRunOptions options_;
};
