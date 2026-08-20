# Agent Tool Calling Runtime 设计与思考说明

本阶段的问题演进、候选方案、复审缺陷和修复证据集中记录在
`AGENT_ENGINEERING_DECISIONS.md` 的 ADR-003 至 ADR-007。

## 1. 本阶段目标

本阶段不是继续增加工具数量，而是把原来的自定义 Planner：

```text
Prompt 要求模型返回 {"tool":"weather","input":"Beijing"}
-> C++ if/else 执行一个工具
-> 再请求一次模型
```

升级为 DeepSeek 原生 Tool Calls：

```text
messages + tools
-> 模型返回 tool_calls
-> C++ 校验并执行工具
-> role=tool + tool_call_id 回传结果
-> 模型继续选择工具或生成最终答案
```

参考协议：

- DeepSeek Tool Calls：<https://api-docs.deepseek.com/zh-cn/guides/tool_calls>
- DeepSeek Chat Completions：<https://api-docs.deepseek.com/api/create-chat-completion>

## 2. 为什么删除自定义 Planner JSON

旧 Planner 依赖自然语言要求模型输出固定 JSON，存在以下问题：

1. 模型可能返回 Markdown 代码块，而不是纯 JSON。
2. `tool` 和 `input` 是项目自定义字段，不能表达一次响应中的多个工具调用。
3. 工具结果只能重新拼成普通 User Prompt，缺少标准关联 ID。
4. 每轮固定为 Planner、一个工具、Final Model，不能根据环境反馈继续执行。
5. 工具增加后，`runTurn()` 中的 `if/else` 会持续膨胀。
6. Planner 格式错误曾被降级成普通成功回答，会掩盖协议问题。

原生协议提供：

```text
tool_calls[].id
tool_calls[].function.name
tool_calls[].function.arguments
role=tool
tool_call_id
```

其中 `tool_call_id` 很重要。一次模型响应可以请求多个工具，工具结果必须精确关联到
原请求；只使用工具名无法区分同名工具的两次调用。

## 3. 模块职责

### AgentModelClient

位置：`include/AgentRuntime.h`

接口输入：

```text
标准 messages
标准 tools definitions
本次调用剩余 timeout
```

接口输出：

```text
自然语言 content
零个或多个 AgentToolCall
结构化错误
```

Runtime 不知道 API Key、URL、libcurl 和 DeepSeek Response JSON。生产环境使用
`DeepSeekClient`，测试使用 `FakeModelClient`。

这个依赖反转解决了一个关键测试问题：如果 `AgentRuntime` 内部直接调用全局
`deepSeekClient()`，每条自动化测试都要访问公网，而且结果会受到模型随机性影响。

### AgentTool

每个工具统一提供：

```text
name()
description()
inputSchema()
execute(arguments, context)
```

`inputSchema()` 是给模型阅读的接口说明；`execute()` 中的 C++ 校验才是真正安全边界。
不能因为模型看过 Schema 就信任它生成的参数。

### AgentToolRegistry

Registry 同时使用：

```text
vector        保存稳定注册顺序
unordered_map 按名称快速查找
```

稳定顺序便于阅读请求、制作快照和比较 Prompt；map 负责执行时的工具分发。

Registry 的执行边界：

```text
未知工具拒绝
arguments 最大 16 KiB
arguments 必须是 JSON object
工具结果最大 16 KiB
工具异常转换成失败结果
执行前检查 Run deadline
```

### AgentRuntime

Runtime 是协议无关、同步执行的 Agent 状态机。它不拥有线程和 Session：

```text
AgentDemoService worker
-> AgentRuntime::run
-> AgentModelClient::complete
-> AgentToolRegistry::execute
```

“同步 Runtime”不会阻塞 Reactor，因为整个 `run()` 已经运行在
`BoundedThreadPool` worker 中。线程隔离和函数同步/异步是两个不同问题。

### AgentDemoService

Service 继续负责：

```text
TCP 按行协议
HTTP/TCP Session
同 Session inFlight
有界业务线程池
历史保存
传输层 AgentResult
```

它不再负责解析 Planner JSON 或判断具体工具名。

## 4. 完整状态流转

```text
CREATED
-> 构造 system/history/user messages
-> CALL_MODEL
   -> content 且没有 tool_calls：COMPLETED
   -> tool_calls：VALIDATE_BUDGET
      -> 预算不足：BUDGET_EXCEEDED
      -> 预算允许：APPEND_ASSISTANT_TOOL_CALLS
         -> VALIDATE_AND_EXECUTE_TOOLS
         -> APPEND_TOOL_RESULTS
         -> CALL_MODEL
   -> 模型错误：FAILED
```

伪代码：

```cpp
for (size_t step = 0; step < maxModelCalls; ++step)
{
    ModelResponse response = model.complete(messages, tools, remainingTime);

    if (response.hasNoToolCalls())
    {
        return response.finalAnswer();
    }

    checkToolBudget(response.toolCalls);
    messages.push_back(response.asAssistantMessage());

    for (const ToolCall &call : response.toolCalls)
    {
        ToolResult result = registry.execute(call, deadline);
        messages.push_back(result.asToolMessage(call.id));
    }
}

return budgetExceeded();
```

## 5. 为什么工具失败不直接终止 Agent

模型生成非法参数、请求未知工具或工具业务失败时，Runtime 会构造：

```json
{
  "ok": false,
  "error": "tool arguments are not valid JSON"
}
```

并通过对应的 `tool_call_id` 作为 `role=tool` 消息回给模型。

原因是工具错误不一定不可恢复：

```text
location 类型错误 -> 模型可修正参数
天气服务失败 -> 模型可向用户解释暂时不可用
未知工具 -> 模型可改用已注册工具或直接回答
```

只有模型供应商错误、统一 deadline、模型调用预算等 Runtime 级错误才直接终止 Run。

## 6. 为什么仍然需要应用侧参数校验

工具 Schema 主要帮助模型正确生成参数，不构成权限边界。

当前内置工具检查：

```text
calculator：只允许 expression 字符串，1-4096 字节，结果必须是有限数
time：不允许任何参数
weather：只允许 location 字符串，location 最终限制 128 字节
```

所有工具拒绝未知字段。例如：

```json
{
  "location": "Beijing",
  "admin": true
}
```

不能因为 `location` 合法就忽略 `admin`。未来涉及权限的工具中，静默忽略未知字段
可能将模型拼错的参数变成意外操作。

当前没有开启 DeepSeek Beta `strict` 模式，避免生产 URL 和协议依赖 Beta 功能。即使
未来开启 strict，应用侧长度、权限、deadline 和业务校验仍应保留。

当前 DeepSeek Thinking 默认通过配置开启：

```json
deepseek_thinking_enabled=true
```

请求仍显式发送 enabled/disabled，不依赖供应商默认值。`reasoning_content` 只在当前
Agent Run 的对应 Assistant Tool Call Message 中暂存并原样回传，不进入 SQLite、日志、
HTTP 或具体 SSE 文本；详见 `AGENT_THINKING_DESIGN.md`。

## 7. 执行预算

默认预算定义在 `AgentRunOptions`：

```text
maxModelCalls = 6
maxToolCalls  = 8
timeoutMs     = 60000
```

三种预算解决不同问题：

| 预算 | 防止的问题 |
|---|---|
| maxModelCalls | 模型持续要求工具、无法生成最终答案 |
| maxToolCalls | 一次响应请求大量工具，或累计工具过多 |
| timeoutMs | 模型和工具单次都未超时，但总耗时不断叠加 |

DeepSeek 单次调用仍最多 30 秒；Weather 单次最多 8 秒，但两者都会取自身上限和 Run
剩余时间的较小值。因此后续步骤不能重新获得一份完整超时预算。

模型/工具调用次数超出预算由 HTTP 映射为 `502 Bad Gateway`；统一 deadline 从请求入队前
开始计算，超时映射为 `504 Gateway Timeout`。因此业务队列等待也会消耗这 60 秒，
不会在排队结束后重新获得一份完整预算。

当前 Registry 只接受 `isReadOnly() == true` 的工具。写工具即使在最后一次模型调用前
执行，也可能遇到后续模型超时和客户端重试，必须先实现审批、幂等键、执行记录和恢复
语义。本阶段的 calculator、time、weather 都是只读工具。

## 8. 消息历史如何处理

当前 SQLite ConversationStore 长期保留：

```text
user 原始问题
assistant 最终答案
```

当前 Run 内部临时保留：

```text
assistant tool_calls
tool results
```

数据库同时保存工具执行轨迹；下一个 Turn 的模型上下文目前只重建 user/final assistant，
不会伪造历史 Provider tool_call_id。ContextBuilder 按 Token Budget 选择历史，详见
`AGENT_CONTEXT_MANAGEMENT_DESIGN.md`。

## 9. HTTP 兼容策略

旧单工具字段继续保留：

```json
{
  "tool": "weather",
  "tool_result": "Beijing: Sunny"
}
```

它们表示最后一次工具执行。新增完整数组：

```json
{
  "tool_calls": [
    {
      "id": "call-1",
      "name": "weather",
      "ok": true,
      "result": "Beijing: Sunny"
    }
  ]
}
```

这样旧调用方可以继续工作，新调用方可以观察多工具执行过程。

## 10. 自动化测试

测试文件：`tests/AgentRuntimeTest.cc`

运行：

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

当前 Tool Calling、Provider 和 SSE Parser 单元测试已扩展到 13 类确定性用例：

1. 不调用工具直接回答。
2. 单工具标准 round trip。
3. 一次多个工具，并继续下一轮工具调用。
4. 非法 arguments JSON 作为工具失败反馈模型。
5. 未注册工具绝不执行。
6. 模型调用预算终止循环。
7. 跨步骤复用 `tool_call_id` 会在重复工具执行前被拒绝。
8. DeepSeek Provider 请求和响应协议，包括 thinking、finish_reason 和 function type。
9. 模型超时映射为 Agent 上游超时。
10. DeepSeek 401/402/400/422/429/500/503 错误分类。
11. 第二次 Model Client 抛异常时保留第一次模型、工具和 Token metrics。
12. Run 在调用 Provider 前已经超过统一 deadline。
13. DeepSeek SSE 任意分片、Tool Call 参数拼接、`[DONE]` 和 Event 上限。

Run ID、阶段耗时、Token usage 和安全错误映射见 `AGENT_OBSERVABILITY_DESIGN.md`。

这些是 Runtime 单元测试，不访问公网。另有 `tests/AgentHttpIntegrationTest.py` 使用本地
Mock 启动真实服务器进程，验证 HTTP、Provider 错误、异步日志和 Run ID。单元测试和
集成测试不能互相替代。

## 11. 当前仍未解决的问题

本阶段有意不同时加入：

```text
SSE 已实现；仍未实现断线重放、heartbeat 和 libcurl multi Provider Client
普通非流式请求的客户端断连取消
429/5xx 重试
精确 tokenizer 和 Token 金额换算
多用户 Session 鉴权与多进程租约
Run checkpoint
RAG
MCP
多 Agent
```

原因是本阶段先建立标准、可测试的工具执行底座。后续 RAG 检索工具、诊断工具和 MCP
工具都可以注册到同一个 Registry，而不再修改 Agent 主循环。

## 12. 推荐阅读顺序

```text
1. include/AgentRuntime.h
   先只看数据结构和五个接口：ModelClient、AgentTool、Registry、Options、Runtime

2. tests/AgentRuntimeTest.cc::testSingleToolRoundTrip
   用 Fake 响应理解标准 tool_calls -> role=tool 闭环

3. src/AgentRuntime.cc::AgentRuntime::run
   阅读循环、终止条件和消息追加顺序

4. src/AgentRuntime.cc::AgentToolRegistry::execute
   阅读不可信参数、异常、结果大小和 deadline 边界

5. include/DeepSeekProtocol.h + src/DeepSeekProtocol.cc
   阅读 thinking、finish_reason、assistant role 和 function type 的 Provider 协议校验

6. src/AgentDemo.cc::DeepSeekClient::complete
   阅读 Provider 协议如何通过 libcurl 发送到真实 DeepSeek HTTP API

7. src/AgentDemo.cc::CalculatorTool/TimeTool/WeatherTool
   比较无参数工具、本地纯计算工具和外部 HTTP 工具的校验差异

8. src/AgentDemo.cc::AgentDemoService::runTurn
   最后看 Runtime 如何与 Session 和业务线程池组合

9. src/main.cc::onAsyncHttpRequest
   看多工具轨迹如何映射为 HTTP JSON
```

阅读时尝试回答：

1. 为什么 ModelClient 接收完整 messages，而不是 systemPrompt/userPrompt 两个字符串？
2. 为什么 Tool Schema 不能替代 C++ 校验？
3. 为什么工具失败作为 `role=tool` 返回，而未知工具不直接导致进程错误？
4. 为什么需要同时限制 model calls、tool calls 和总时间？
5. 为什么 Runtime 可以同步，却仍然不阻塞 EventLoop？
6. 为什么 assistant tool_calls 消息必须原样放回下一次模型请求？
7. `tool_call_id` 解决了什么关联问题？
8. 为什么测试 Agent Runtime 时不应调用真实模型？
