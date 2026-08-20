# DeepSeek Thinking 的 Run 内暂存设计

## 1. 目标

开启 DeepSeek Thinking，同时保持以下安全边界：

```text
reasoning_content 只存在于当前 Agent Run 内存
Tool Call 后原样回传 DeepSeek
不写 SQLite
不写 agent_trace 或普通日志
不返回 HTTP JSON
不通过 SSE 展示具体内容
SSE 只展示 thinking started/completed 状态
```

配置默认开启：

```ini
deepseek_thinking_enabled=true
```

环境变量：

```bash
DEEPSEEK_THINKING_ENABLED=true
```

需要快速回退时可设置为 `false`，请求会显式发送：

```json
{"thinking":{"type":"disabled"}}
```

## 2. 官方协议要求

参考：<https://api-docs.deepseek.com/zh-cn/guides/thinking_mode>

思考模式响应中：

```text
reasoning_content 与 content 同级
```

如果 Assistant 进行了 Tool Call，后续请求必须完整回传该 Assistant Message 的：

```text
content
reasoning_content
tool_calls
```

再追加对应：

```text
role=tool
tool_call_id
tool result
```

遗漏 reasoning 会导致 Provider 400。因此它不是可有可无的 UI 文本，而是当前 Tool
Calling Run 的 Provider 协议状态。

## 3. 为什么按 Assistant Message 保存

错误方案：

```cpp
std::string lastReasoning;
```

多步 Run：

```text
Model Call 1 -> R1 -> Weather
Model Call 2 -> R2 -> Calculator
Model Call 3 -> R3 -> Final Answer
```

如果只保存 `lastReasoning`，R2 会覆盖 R1，第三次请求无法完整重放前面的消息链。

正确结构：

```text
user
assistant(reasoning=R1, weather tool_call)
tool(weather result)
assistant(reasoning=R2, calculator tool_call)
tool(calculator result)
```

每个 reasoning 属于对应 Assistant Message，不属于一个全局“最近思考”。

## 4. 内存生命周期

`AgentRuntime::runInternal()` 中的：

```cpp
std::vector<nlohmann::json> messages;
```

是一次 Run 的局部变量。Tool Call 响应通过：

```text
AgentModelResponse.reasoningContent
-> makeAssistantToolCallMessage
-> messages
-> 下一次 Provider Request
```

Run 成功、失败或取消后，局部 messages 和所有 reasoning 自动析构。

最终答案轮次的 reasoning 不需要再次调用 Provider，因此只存在于最后一个
`AgentModelResponse`，随后一起释放。

## 5. 为什么不跨 Turn 持久化

当前 SQLite Turn 保存：

```text
user message
final assistant answer
tool execution trace
```

不保存 reasoning。原因：

```text
思考是 Provider 特定的临时执行状态
可能包含错误草稿或被否定路径
可能泄露 System/工具内部信息
体积和 Token 成本较大
不适合作为下一轮普通聊天历史
切换 Provider 时兼容性差
```

DeepSeek 官方示例在“完整重放原始 Tool Calling 消息链”时，要求后续 User Turn 继续携带
这些 Assistant Tool Messages 的 reasoning。本项目选择另一种有损上下文策略：跨 Turn
只重建 user/final assistant，不重放旧 Tool Call 子轮。因此下一 Turn 不会发送一条
“有旧 tool_calls 但缺 reasoning”的不完整消息，而是基于压缩后的对话结果开始新 Run。

这个方案保持请求结构自洽，但主动放弃 Provider 旧工具子轮的隐藏推理连续性；如果未来
要做“官方示例式完整原始消息重放”，就必须将 assistant tool_calls、tool results 和对应
reasoning 一起加密持久化，而不能只增加一列 reasoning。

## 6. 非流式解析

`parseDeepSeekChatResponse()` 解析：

```json
{
  "role":"assistant",
  "reasoning_content":"...",
  "content":null,
  "tool_calls":[...]
}
```

校验：

```text
字段允许缺失/null/string
存在且非 null 时必须为 string
单次模型调用最多 1 MiB
Thinking 开启且返回 Tool Calls 时必须存在 reasoning_content 字段
```

`hasReasoningContent` 区分“字段缺失”和“字段存在但为空字符串”。

## 7. 流式解析

DeepSeek SSE 可能分片：

```text
delta.reasoning_content = "需要"
delta.reasoning_content = "查询天气"
delta.tool_calls = ...
```

`DeepSeekSseParser` 分别累计：

```text
reasoningContent
content
toolCalls
```

约束：

```text
reasoningContent <= 1 MiB / Model Call
非流式完整 Provider Response Body <= 3 MiB
思考必须发生在普通 content/tool_calls 之前
Thinking 状态每个 Model Call 最多 started/completed 各一次
客户端取消会让 callback 返回 false 并中止 curl
```

## 8. SSE 状态，不展示内容

下游只发送：

```text
event: assistant.thinking.started
data: {"run_id":"...","sequence":1}

event: assistant.thinking.completed
data: {"run_id":"...","sequence":1}
```

不发送：

```text
reasoning text
reasoning delta
```

Started 在收到第一个非空 reasoning delta 时发送；Completed 在开始输出 content、开始
Tool Call、出现 finish reason 或 Stream 正常结束时发送。

Parser 损坏、内容超限或非标准非 2xx SSE 结束时也会先闭合 Completed，再由 Runtime 发送
`model.completed(ok=false)` 和 `error`。Completed 只表示思考阶段停止，不表示调用成功。

如果 Provider 没返回 reasoning，当前不会伪造 Thinking 状态。

## 9. 日志、HTTP 和数据库安全边界

允许观测：

```text
reasoning_tokens 数量
Thinking started/completed 状态
Model Call latency
```

禁止观测具体内容：

```text
AgentResult 没有 reasoningContent
HTTP JSON 没有 reasoningContent
SSE data 没有 reasoning text
agent_trace 没有 reasoningContent
SQLite Schema 没有 reasoningContent
Tool Result 没有 reasoningContent
```

## 10. 配置取舍

默认开启，原因是当前已具备：

```text
每条 Assistant Tool Message 的 reasoning 回传
非流式和流式解析
内容上限
取消
泄露测试
reasoning token metrics
配置回退
```

保留关闭开关，原因是：

```text
Thinking 会增加延迟和 Token 成本
Provider 协议可能调整
某些测试只关注 Context，不需要 Thinking
出现 Provider 兼容问题时需要快速降级
```

当前未增加 `reasoning_effort` 配置，使用 DeepSeek 默认 effort（官方当前默认 high）。只有
评测证明需要在质量、延迟和成本之间切换时，再增加 low/high/max。

## 11. 测试

### Runtime 单元测试

```text
R1 在 Tool Call 后进入第二次 Provider Request
多步 R1/R2 分别保存在对应 Assistant Message
Enabled/Disabled Request 构造
非流式 reasoning 字段解析和错误类型拒绝
```

### SSE Parser

```text
reasoning_content 跨 Event 分片
只发送 started/completed
reasoning 不进入 assistant.delta
reasoning 后再到达 content/tool_calls
reasoning_tokens 解析
```

### HTTP/SSE 集成

使用：

```text
PRIVATE_REASONING_SENTINEL_...
```

作为 Provider Reasoning 内容，断言它：

```text
会出现在后续 Provider Tool Call 请求中
不会出现在客户端 JSON/SSE
不会出现在 agent_trace
不会出现在 SQLite
```

取消测试在 reasoning 流中收到 `assistant.thinking.started` 后用 RST 断开，验证 Provider
中止和 Session inFlight 释放。

## 12. 当前边界

```text
不展示具体思维链
不跨 Turn 保存 reasoning
不记录 reasoning 内容
不支持 reasoning_effort 配置
普通非流式请求断开仍不会取消
Reasoning 最多 1 MiB / Model Call
非流式完整 Provider Response Body 最多 3 MiB
跨 Turn 使用压缩对话，不是原始 Provider Tool Message 的完整重放
```

## 13. 推荐阅读顺序

```text
1. include/AgentRuntime.h::AgentModelResponse
2. src/DeepSeekProtocol.cc::parseDeepSeekChatResponse
3. src/DeepSeekProtocol.cc::DeepSeekSseParser
4. src/AgentRuntime.cc::makeAssistantToolCallMessage
5. src/AgentRuntime.cc::runInternal
6. src/AgentDemo.cc::DeepSeekClient
7. tests/AgentRuntimeTest.cc
8. tests/AgentSseIntegrationTest.py
```

阅读时回答：

1. 为什么不能只保存 lastReasoning？
2. 为什么 Tool Call 后必须原样回传 reasoning？
3. 为什么最终回答的 reasoning 不需要持久化？
4. 为什么 SQLite 不增加 reasoning 字段？
5. 为什么 SSE 只显示状态，不显示内容？
6. `reasoning_tokens` 和 reasoning 文本有什么安全差异？
