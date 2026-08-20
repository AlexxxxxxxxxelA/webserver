# Agent 可观测性与错误分类设计

本阶段的问题演进、错误映射和复审修复集中记录在
`AGENT_ENGINEERING_DECISIONS.md` 的 ADR-008 至 ADR-011。

## 1. 为什么 Tool Calling 之后先做可观测性

Tool Calling 引入了新的不确定性：

```text
一次用户请求
-> 可能调用模型一次
-> 也可能调用模型多次
-> 可能执行零个、一个或多个工具
-> 每个阶段都可能超时或失败
```

如果只有一个最终 HTTP 状态，无法回答：

```text
请求慢在业务队列、模型还是工具？
模型一共调用了几次？
哪个步骤失败？
DeepSeek 返回了 401、429 还是 503？
一次 Run 使用了多少 Token？
Prompt Cache 命中了多少 Token？
```

因此本阶段先建立 Run 级观测模型，再做 SSE。后续 Streaming 的每个事件可以直接复用
同一个 `run_id` 和阶段结构，而不必重新设计数据模型。

初学者应先区分：

| 概念 | 当前项目中的实现 |
|---|---|
| Log | `agent_trace {JSON}` 完成日志 |
| Trace-like steps | model/tool execution 明细和 SSE 生命周期事件 |
| Run Metrics | 单次 Run 的 latency、调用次数和 Token usage |
| Distributed Trace | 未实现，没有 Span、父子关系、Trace Context、Exporter |
| Aggregated Metrics | 未实现，没有 Prometheus QPS、p95/p99 时间序列 |

## 2. 数据流

```text
AgentDemoService::submit
-> 创建 run_id、enqueuedAt、deadline
-> 业务队列
-> AgentRuntime::runUntil
   -> AgentModelClient::complete
      -> AgentModelExecution + TokenUsage
   -> AgentToolRegistry::execute
      -> AgentToolExecution + latency
   -> 聚合 AgentRunMetrics
-> AgentDemoService 补 queueWaitMs、totalLatencyMs
-> logAgentRun 输出安全 JSON 日志
-> HTTP/TCP 返回 run_id
```

数据只计算一次，再由日志和协议层读取，避免 HTTP、TCP 和日志各自重复计时产生不同结果。

## 3. Run ID

格式：

```text
run-<system_clock 微秒十六进制>-<进程内原子序号>
```

用途：

```text
客户端响应
<-> HTTP/TCP 请求
<-> agent_trace 日志
```

它不是：

```text
用户身份
Session ID
鉴权 Token
分布式全局唯一 ID
写操作幂等键
```

Run ID 只用于观测关联。未来写工具必须使用由业务方提供并持久化的幂等键，不能因为
Run ID 看起来唯一就复用它承担幂等语义。

## 4. 为什么使用两种时钟

### system_clock

只用于生成可读 Run ID 前缀。它对应现实世界时间，但可能因为 NTP 校时发生跳变。

### steady_clock

用于：

```text
queue_wait_ms
model_latency_ms
tool_latency_ms
total_latency_ms
统一 deadline
```

`steady_clock` 保证单调递增。耗时计算不能使用系统墙钟，否则系统时间向后调整时可能
出现负延迟。

## 5. 指标结构

### AgentModelExecution

每次模型调用记录：

```text
sequence
success
error category
provider HTTP status
latency_ms
TokenUsage
```

不记录：

```text
完整 Prompt
用户消息
模型完整回答
reasoning_content
API Key
Authorization Header
```

### AgentToolExecution

每次工具调用在业务结果中包含：

```text
tool_call_id
tool name
success
result/error
latency_ms
```

HTTP 成功响应原本就返回工具结果，所以继续保留；`agent_trace` 日志只记录工具名、状态
和耗时，不记录 arguments 或 output。

### AgentRunMetrics

聚合：

```text
modelLatencyMs
toolLatencyMs
所有 ModelExecution
所有模型调用累计 TokenUsage
```

Service 再补：

```text
queueWaitMs
totalLatencyMs
```

这些时长不要求相加严格等于总耗时，因为总耗时还包括：

```text
JSON 构造与解析
Session 历史复制
Runtime 状态切换
线程调度
日志前业务收尾
```

## 6. Token Usage

从 DeepSeek Response 的 `usage` 解析：

```text
prompt_tokens
completion_tokens
total_tokens
prompt_cache_hit_tokens
prompt_cache_miss_tokens
completion_tokens_details.reasoning_tokens
```

Runtime 对每次模型调用做加法聚合。例如：

```text
第一次模型调用/工具选择轮次：12 tokens
第二次 Final Answer：23 tokens
Run total_tokens：35
```

当前 Thinking 默认开启，`reasoning_tokens` 会正常聚合；只记录 Token 数量，不记录具体
`reasoning_content`。可通过配置关闭 Thinking，此时该字段通常为 0。

Usage 字段缺失时按 0 处理，因为本地 Mock 或兼容 Provider 不一定提供完整统计；字段存在
但为负数或错误类型时，整个 Provider Response 被视为无效，不能生成虚假成本指标。

## 7. DeepSeek 错误分类

参考：<https://api-docs.deepseek.com/zh-cn/quick_start/error_codes>

| DeepSeek HTTP | 内部类别 | 本服务 HTTP | Retry-After |
|---|---|---:|---|
| 400/422 | rejected_request | 502 | 否 |
| 401 | authentication | 503 | 否 |
| 402 | payment_required | 503 | 否 |
| 429 | rate_limited | 429 | 1 秒 |
| 500/503 | unavailable | 503 | 1 秒 |
| curl timeout | timeout | 504 | 否 |
| 其他网络错误 | upstream_error | 502 | 否 |
| 响应格式错误 | invalid_response | 502 | 否 |

为什么上游 401 不直接返回下游 401：

```text
DeepSeek 401 表示服务器配置的 API Key 错误
客户端没有修改这个 Key 的能力
若返回客户端 401，会让客户端误以为自己的身份认证失败
```

因此映射为 503，表示服务器当前无法提供 Agent 能力。

## 8. 对外错误与内部错误分离

内部 `errorMessage` 可能包含：

```text
DNS 解析失败
TLS 验证错误
代理配置
本机网络信息
libcurl 错误文本
```

HTTP/TCP 不直接返回这些内容，而是返回稳定安全文案。HTTP 示例：

```json
{
  "code": "UPSTREAM_RATE_LIMITED",
  "error": "agent upstream rate limit exceeded",
  "run_id": "run-..."
}
```

排障人员使用 `run_id` 查询内部 `agent_trace`，客户端不需要看到基础设施细节。

## 9. 结构化日志

日志以可搜索前缀开头：

```text
agent_trace {JSON}
```

示例：

```json
{
  "event": "agent.run.completed",
  "run_id": "run-...",
  "ok": true,
  "error": "none",
  "queue_wait_ms": 0,
  "total_latency_ms": 18,
  "model_latency_ms": 15,
  "tool_latency_ms": 1,
  "model_calls": 2,
  "tool_calls": 1,
  "usage": {
    "total_tokens": 35
  },
  "model_steps": [
    {"sequence": 1, "ok": true, "provider_status": 200, "latency_ms": 13},
    {"sequence": 2, "ok": true, "provider_status": 200, "latency_ms": 2}
  ],
  "tool_steps": [
    {"name": "weather", "ok": true, "latency_ms": 1}
  ]
}
```

日志 JSON 序列化被 `try/catch` 包围；`Logger` 析构也会隔离 output sink 抛出的异常。
可观测性是旁路能力，日志内存分配失败不能阻止业务 completion，否则会出现“Agent
已完成但客户端永远等不到响应”。

工具失败也只向模型和客户端提供稳定错误，例如：

```text
weather service request failed
tool execution raised an exception
requested tool is not registered
```

curl、DNS、TLS 和 `exception::what()` 的内部细节不会进入 Tool Result，防止模型在最终
答案中复述基础设施信息。

## 10. HTTP 成功响应

新增：

```json
{
  "run_id": "run-...",
  "metrics": {
    "queue_wait_ms": 0,
    "total_latency_ms": 18,
    "model_latency_ms": 15,
    "tool_latency_ms": 1,
    "model_calls": 2,
    "tool_calls": 1,
    "usage": {
      "prompt_tokens": 30,
      "completion_tokens": 5,
      "total_tokens": 35,
      "prompt_cache_hit_tokens": 12,
      "prompt_cache_miss_tokens": 18,
      "reasoning_tokens": 0
    }
  }
}
```

当前 Demo 直接返回指标便于学习和调试。生产系统通常只返回 `run_id`，详细成本和耗时
进入受权限保护的管理接口或观测平台，避免对普通用户暴露内部容量信息。

## 11. Logger 并发修复

原 `Logger::formatTime()` 使用 `localtime()`，它返回进程共享静态对象。多个 IO 和 Agent
worker 同时写日志时存在数据竞争。本阶段改为 `localtime_r()`，将结果写入线程自己的
栈对象；日志格式保持不变。

## 12. 自动化和集成测试

单元测试覆盖：

```text
Provider usage 解析
多次模型 usage 聚合
模型和工具步骤计数
401/402/400/422/429/500/503 分类
Provider status 保留
```

仓库中的 `tests/AgentHttpIntegrationTest.py` 由 CTest 自动启动本地 Mock
DeepSeek、Mock Weather 和真实服务器进程，覆盖：

```text
两次模型 + 一次 Weather 的 Token 聚合
run_id 返回
各阶段耗时字段
HTTP tool latency
429 -> HTTP 429 + Retry-After
稳定错误码
Provider 私有 Body 不进入响应或日志
Weather 失败的上游 Body 不进入 Tool Result、模型答案或日志
run_id 能在异步日志中找到
```

当前完整 `ctest` 会运行：

```text
agent_runtime_test
conversation_context_test
chat_client_test
agent_http_integration_test
agent_sse_integration_test
conversation_restart_integration_test
```

统一 Run deadline 与 Provider timeout 是两个不同错误。若队列等待已经耗尽 60 秒预算，
Runtime 不会调用 DeepSeek，而是返回：

```text
504 AGENT_RUN_DEADLINE_EXCEEDED
```

只有已经进入 Provider 调用并由 curl 报告 timeout，才返回 `UPSTREAM_TIMEOUT`。

## 13. 当前边界

本阶段仍然不是完整 Observability 平台：

```text
没有 Prometheus /metrics
没有全局 p50/p95/p99 聚合
没有 OpenTelemetry exporter
没有跨进程 Trace Context
没有请求取消
没有自动重试
没有模型费用金额换算
没有日志采样和脱敏配置中心
```

SSE 已在 `AGENT_SSE_STREAMING_DESIGN.md` 中实现并记录，Run 生命周期映射为：

```text
run.started
model.started/completed
tool.started/completed
assistant.delta
run.completed
error
```

## 14. 推荐阅读顺序

```text
1. include/AgentRuntime.h::TokenUsage / AgentModelExecution / AgentRunMetrics
2. src/DeepSeekProtocol.cc::parseDeepSeekChatResponse
3. src/DeepSeekProtocol.cc::classifyDeepSeekHttpError
4. src/AgentRuntime.cc::AgentRuntime::runUntil
5. include/AgentDemo.h::AgentResult
6. src/AgentDemo.cc::submit
7. src/AgentDemo.cc::makeAgentResult
8. src/AgentDemo.cc::logAgentRun
9. src/main.cc::agentJsonError / agentMetricsJson
10. tests/AgentRuntimeTest.cc
```

阅读时回答：

1. 为什么 Run ID 不能当幂等键？
2. 为什么延迟必须使用 `steady_clock`？
3. 为什么上游 401 映射成下游 503？
4. 为什么日志不能保存完整 Prompt 和工具参数？
5. 为什么模型阶段指标由 Runtime 聚合，而队列等待由 Service 补充？
6. 为什么日志序列化失败不能阻止 completion？
7. `model_latency_ms + tool_latency_ms` 为什么不一定等于 `total_latency_ms`？
8. 为什么 HTTP Demo 可以返回 metrics，而真实生产系统通常只返回 run_id？
