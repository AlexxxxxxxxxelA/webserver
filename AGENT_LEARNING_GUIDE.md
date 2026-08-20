# AI Agent 总体概念与源码学习导读

## 0. 这份文档怎么用

这份文档面向第一次系统学习 Agent 的读者，目标不是让你记住框架名，而是建立一张能
解释当前 C++ 项目的心智地图。

建议学习方法：

```text
先读概念和天气示例
-> 再读 AgentRuntimeTest 的 Fake Model 测试
-> 再进入 AgentRuntime 主循环
-> 最后阅读网络、SQLite 和 SSE 接入
```

不要从 1000 多行的 `AgentDemo.cc` 第一行一直读到最后。那会同时遇到配置、libcurl、
工具、Session、SQLite、线程和网络生命周期，很容易把不同层次混在一起。

## 1. 一张总图

```text
Client
  |
  | HTTP JSON / HTTP SSE / TCP line
  v
AgentDemoService
  |-- Session::inFlight            同一会话串行
  |-- BoundedThreadPool            隔离阻塞业务
  |
  +-> ConversationStore            SQLite 完整成功历史
  +-> ContextBuilder               选择本次历史上下文
  +-> AgentRuntime                 有界 Agent Loop
        |-- AgentModelClient
        |     +-> DeepSeekClient    HTTP/TLS/libcurl
        |           +-> DeepSeekProtocol JSON/SSE 协议
        |
        +-- AgentToolRegistry
              +-> CalculatorTool
              +-> TimeTool
              +-> WeatherTool
  |
  +-> AgentResult / AgentEvent
        |-- 普通 JSON Response
        +-- SSE 生命周期和文本增量
```

可以先把项目分成五层：

| 层次 | 负责什么 | 主要文件 |
|---|---|---|
| 网络层 | 收请求、发响应、TCP 生命周期 | `HttpServer`、`TcpConnection` |
| Service 层 | Session、线程池、存储、传输适配 | `AgentDemo` |
| Runtime 层 | 模型与工具之间的执行循环 | `AgentRuntime` |
| Provider 层 | DeepSeek HTTP 和协议 | `DeepSeekProtocol`、`DeepSeekClient` |
| Context 层 | SQLite 历史、Token 预算、摘要 | `ConversationContext` |

## 2. LLM 与 Agent

### 2.1 LLM 是什么

在这个项目中，可以把 LLM 简化理解为：

```text
输入 messages
-> 模型生成下一条 assistant 输出
```

一次：

```cpp
modelClient.complete(messages, tools, timeoutMs)
```

只是一次 Model Call。

模型可能生成：

```text
自然语言答案
```

也可能生成：

```text
tool_calls
```

但模型不会进入你的服务器进程执行 `queryWeather()`，也不会直接读 SQLite。

### 2.2 Agent 是什么

Agent 是围绕 LLM 建立的应用系统：

```text
上下文
+ 模型调用
+ 工具执行
+ 多步循环
+ 次数预算
+ deadline
+ cancel
+ 持久化
+ 可观测性
```

所以：

```text
调用一次 DeepSeek != 完整 Agent
```

当前项目的核心 Agent 价值在于：模型提出下一步动作，服务器在有明确边界的状态机里
执行动作，并决定何时停止。

## 3. 用天气问题走完整条链路

用户发送：

```text
北京天气怎么样？
```

### 第一步：构造上下文

Service 从 SQLite 加载该 Session 的成功 Turn：

```text
ConversationStore::load
-> ContextBuilder::build
-> 最近完整 Turn + 可选摘要
```

Runtime 再加入：

```text
system
历史 user/assistant
当前 user：北京天气怎么样？
tools definitions
```

### 第二步：第一次模型调用

DeepSeek 不直接返回天气，而是返回：

```json
{
  "tool_calls": [
    {
      "id": "call-1",
      "type": "function",
      "function": {
        "name": "weather",
        "arguments": "{\"location\":\"Beijing\"}"
      }
    }
  ]
}
```

这里最容易误解的一点：

```text
模型只是提出“请调用 weather”
工具此时还没有执行
```

### 第三步：服务器执行工具

Runtime 检查：

```text
工具是否注册
工具名是否合法
tool_call_id 是否重复
arguments 是否为 JSON object
字段类型和长度
剩余 deadline
cancel 状态
```

然后 ToolRegistry 才执行 C++ `WeatherTool`。

### 第四步：把观察结果交回模型

服务器构造：

```json
{
  "role": "tool",
  "tool_call_id": "call-1",
  "content": "{\"ok\":true,\"result\":\"Beijing: Sunny +25C\"}"
}
```

这里的 Tool Result 可以理解为 Agent 从环境得到的一次 observation。

### 第五步：第二次模型调用

DeepSeek 根据问题和工具结果生成：

```text
北京天气晴朗，温度约 25 摄氏度。
```

### 第六步：保存和响应

成功 Run 形成一个持久化 Turn：

```text
user：北京天气怎么样？
assistant：北京天气晴朗，温度约 25 摄氏度。
tool trace：weather 成功
```

普通接口一次返回 JSON；SSE 接口在过程中持续发送生命周期事件和 `assistant.delta`。

## 4. Message 与 Role

模型不是接收一整段随意拼接的文本，而是接收有角色的消息序列。

| Role | 谁产生 | 作用 |
|---|---|---|
| `system` | 应用程序 | 定义行为、安全规则和回答方式 |
| `user` | 用户 | 当前问题或历史问题 |
| `assistant` | 模型 | 自然语言输出或 tool_calls |
| `tool` | 服务器 | 工具执行后的环境结果 |

典型 Tool Calling 消息：

```json
[
  {"role":"system","content":"你是一个助手..."},
  {"role":"user","content":"北京天气？"},
  {
    "role":"assistant",
    "content":null,
    "tool_calls":[{"id":"call-1","function":{"name":"weather","arguments":"..."}}]
  },
  {"role":"tool","tool_call_id":"call-1","content":"..."},
  {"role":"assistant","content":"北京天气晴朗。"}
]
```

Role 不是用户登录身份。`role=user` 不表示这条消息通过了身份认证；Session 权限仍需
应用层鉴权，当前本地 Demo 尚未实现多用户鉴权。

## 5. Tool Definition、Tool Call 与 Tool Result

### Tool Definition

工具定义是服务器告诉模型的说明书：

```text
name
description
JSON Schema
```

例如 Weather Schema 说明 `location` 必须是字符串。

### Tool Call

Tool Call 是模型某一次实际提出的调用请求：

```text
id = call-1
name = weather
argumentsJson = {"location":"Beijing"}
```

### Tool Result

Tool Result 是服务器执行后的观察结果，通过同一个 `tool_call_id` 回传模型。

### 为什么 Schema 之后还要 C++ 校验

模型输出始终是不可信输入。Schema 可以提高模型生成正确参数的概率，但不能替代：

```text
长度限制
类型检查
未知字段拒绝
权限检查
deadline
业务规则
```

可以把 Schema 理解为“给模型看的接口文档”，把 C++ 校验理解为“服务器真正的安全门”。

## 6. tool_call_id、run_id、turn_id 与 session_id

这些 ID 解决不同问题：

| ID | 标识什么 | 生命周期 |
|---|---|---|
| `session_id` | 一段多轮对话 | 多个 Turn |
| `run_id` | 一次 Agent 执行尝试 | 一次请求 |
| `turn_id` | 一个成功持久化对话单元 | 成功历史 |
| `tool_call_id` | 一次具体工具调用 | 当前 Run 内 |

当前成功 Run 使用 `run_id` 作为 `turn_id`，只是为了关联日志和数据库，不表示两个概念
完全相同。失败 Run 有 `run_id`，但不会生成 Turn。

`tool_call_id` 也不是写工具幂等键。同名工具可以调用多次，每次都应有不同 ID。

## 7. Session、Run 与 Turn

### Session

一段多轮对话的命名空间：

```text
Session demo
  Turn 1
  Turn 2
  Turn 3
```

### Run

一次请求触发的一次执行尝试：

```text
load context
-> model
-> tool
-> model
-> save
```

Run 可能：

```text
成功
超时
取消
模型错误
预算超限
```

### Turn

只有成功 Run 才形成 Turn：

```text
一条 user 输入
-> 一条最终 assistant 回答
```

当前 SQLite 每个成功 Turn 保存一行，同时保存工具执行轨迹。

### inFlight 为什么需要

如果同 Session 的 A、B 两个请求同时读取旧历史 H：

```text
A 读取 H
B 读取 H
B 先完成并保存
A 后完成并保存
```

历史顺序就不再等于请求顺序。因此 `inFlight` 保护的是对话语义，不只是防止 vector 或
map 写坏。

## 8. History、Context、Memory 与 RAG

### History

SQLite 中保存的完整成功 Turn。

### Context

本次实际发送给模型的信息。数据库可以有 100 个 Turn，但本次可能只选摘要和最近 8 个。

完整请求上下文还包含：

```text
system
当前 user
tools definitions
本轮临时 tool messages
```

### Memory

长期提取并复用的事实，例如：

```text
用户偏好中文注释
用户项目使用 C++11
```

当前没有独立的长期事实 Memory。SQLite 历史和确定性摘要不能直接等同于 Memory。

### RAG

根据当前问题从外部知识库检索相关文档：

```text
用户问题
-> 检索项目文档
-> 取 Top-K 片段
-> 加入模型上下文
```

当前尚未实现 RAG。Conversation Summary 不是 RAG，因为它没有按当前 Query 检索外部资料。

## 9. Token、字符与字节

Token 是模型 tokenizer 的单位，不等于：

```text
字符
Unicode code point
UTF-8 字节
单词
```

例如中文字符通常占多个 UTF-8 字节，但 Token 数由模型 tokenizer 决定。

本项目有两个 Token 来源：

| 名称 | 什么时候得到 | 用途 |
|---|---|---|
| `TokenEstimator` | 请求前 | 按预算裁剪历史 |
| Provider `usage` | 请求后 | 观测实际消耗和缓存命中 |

`context_estimated_tokens` 只覆盖历史部分；`usage.prompt_tokens` 还包括 system、当前 user、
工具定义等。因此两者不应完全相等。

## 10. Agent Loop

核心循环：

```text
CALL_MODEL
  |
  +-> final content -> SUCCESS
  |
  +-> tool_calls
        -> 检查预算和调用身份
        -> 服务器串行执行工具
        -> 追加 role=tool observations
        -> CALL_MODEL
```

模型是决策建议者，Runtime 是执行控制者。

当前同一批多个工具串行执行，不是并行 fan-out。这样状态、事件顺序和资源预算更容易
理解。后续只有在工具彼此独立并有收益时才考虑并行。

工具失败通常不直接终止 Run，而是把：

```json
{"ok":false,"error":"..."}
```

作为 observation 交回模型，让模型修正参数或向用户解释失败。

## 11. 四种控制机制

| 机制 | 解决什么问题 |
|---|---|
| `maxModelCalls` | 模型一直要求继续调用 |
| `maxToolCalls` | 一次 Run 执行工具过多 |
| absolute deadline | 排队、模型和工具总时间不断叠加 |
| cooperative cancel | SSE 客户端断开后停止不必要工作 |

时间线：

```text
enqueue -> queue wait -> SQLite/context -> model -> tool -> model -> save
   \_____________________ 同一个 Run deadline __________________/
```

单次调用时间：

```text
Provider timeout = min(Provider 单次上限, Run 剩余时间)
Tool timeout     = min(Tool 单次上限, Run 剩余时间)
```

### Cancel 不是杀线程

CancelCheck 是协作式信号：

```text
客户端断开
-> cancelled() 返回 true
-> libcurl progress callback 返回非零
-> curl_easy_perform 中止
```

Runtime 不能安全地强杀任意 C++ 函数。新工具如果长时间阻塞，必须主动使用
`AgentToolContext` 的剩余时间和取消检查。

## 12. Provider Adapter

Provider 接入分三层理解：

| 层次 | 职责 |
|---|---|
| `AgentModelClient` | Runtime 依赖的模型调用接口 |
| `DeepSeekProtocol` | JSON 请求构造、响应校验、SSE Parser |
| `DeepSeekClient` | API Key、HTTP/TLS、libcurl、timeout |

`AgentRuntime` 不知道 DeepSeek URL 和 API Key，所以 FakeModelClient 可以在不联网的情况下
精确测试 Agent Loop。

不过当前内部 messages/tools 仍采用 Chat Completions/Tool Calls JSON。它隔离了具体 HTTP
供应商细节，但不是无需转换就能接入所有模型协议的完全通用 IR。

## 13. SQLite Transaction 与 inFlight

这两者解决不同范围的问题。

### inFlight

保证：

```text
同 Session 的 load -> model/tool -> save 不交叉
```

### SQLite Transaction

保证一次 `saveTurn()` 内部：

```text
sequence 分配
Turn 插入
next_sequence 更新
```

要么一起提交，要么一起回滚。

Transaction 不覆盖模型网络调用。如果把一次长达几十秒的 DeepSeek 调用放进 SQLite 写
事务，会长期占用写锁，是错误设计。

## 14. Streaming 的上下游两条链路

### 上游 Provider

网络线上：

```text
TCP/TLS -> HTTP Response -> Provider SSE -> DeepSeek JSON delta
```

代码可见边界：

```text
libcurl 已处理 TCP/TLS/HTTP framing
-> write callback 收到 Response Body 分片
-> DeepSeekSseParser 恢复 Provider SSE Event
-> JSON Parser
-> AgentRuntime assistant.delta
```

`DeepSeekSseParser` 不解析 HTTP Chunk Header。

### 下游客户端

```text
AgentEvent
-> HttpStreamResponder 编码 event/data/空行
-> HttpStreamState 包装 HTTP Chunk
-> TcpConnection 写 TCP 字节
-> Client
```

项目不是把 DeepSeek 原始 SSE 透明转发给客户端，而是生成自己的：

```text
run.started
model.started/completed
tool.started/completed
assistant.delta
run.completed
error
```

## 15. Token、Delta、SSE Event、HTTP Chunk、TCP Write

这些边界都不相等：

```text
模型 Token
!= Provider delta
!= Provider SSE event
!= 下游 SSE event
!= HTTP chunk
!= TCP write/read
```

一个 `assistant.delta` 可能包含多个 Token；不能用 delta 数量计算 Token。一次 TCP read 也
可能包含半个 Event 或多个 Event。

## 16. Trace、Log 与 Metrics

| 概念 | 当前项目对应物 |
|---|---|
| Log | `agent_trace {JSON}` 完成日志 |
| Trace-like steps | `modelExecutions`、`toolExecutions` |
| Run metrics | latency、调用次数、Token usage |
| Distributed Trace | 未实现 |
| Aggregated metrics | 未实现 QPS、p95、Prometheus endpoint |

`run_id` 用来关联 HTTP、SSE 和日志，但不是鉴权 Token，也不是写操作幂等键。

日志明确不记录：

```text
完整用户消息
完整 Prompt
API Key
Authorization Header
Tool arguments/output
reasoning content
```

## 17. 线程泳道

```text
Client
  |
  v
Connection EventLoop
  |  HTTP/TCP parse、快速校验、inFlight、trySubmit
  |---------------------------------------------->
Business Worker
  |  SQLite load/context
  |  AgentRuntime
  |  curl_easy_perform
  |  Tool execute
  |  SQLite save
  |
  | sendEvent()/Completion
  v
Connection EventLoop
  |  queueInLoop 后真正 Socket Write
  v
Client

Business Worker -> AsyncLogging -> Logging Thread -> Disk
```

需要记住：

```text
同步函数不等于阻塞 EventLoop
```

`curl_easy_perform()` 是同步阻塞函数，但它运行在业务 worker，所以不会占用 IO EventLoop。
代价是每个长 Agent Run 会占用一个 worker。

## 18. 一次普通请求和一次 SSE 请求

### 普通请求

```text
POST /agent/run
-> 等待完整 Run
-> 一次返回 JSON
-> forceCloseAfterWrite
```

### SSE 请求

```text
POST /agent/run/stream
-> 先提交 200 Streaming Header
-> 持续发送 AgentEvent
-> assistant.delta 提前到达
-> run.completed
-> HTTP terminal chunk
-> 完整关闭
```

SSE 改善首段文本延迟和用户体验，不会让模型生成更快，也不会自动提高并发量。

终端用户不需要手写这些 JSON/SSE。`tools/chat_client.py` 是一个薄客户端：用户只输入
自然语言，它自动维护 HTTP Session 并解析 SSE。详见 `AGENT_CLI_CLIENT_DESIGN.md`。

## 19. 错误发生在哪一层

| 错误 | 典型处理 |
|---|---|
| 请求 JSON 错误 | HTTP 400，Header 提交前返回 |
| Session Busy | HTTP 409 |
| 业务队列满 | HTTP 503 |
| DeepSeek 429 | 非流式 HTTP 429；SSE Header 后发送 error event |
| Provider timeout | 504 / SSE error |
| Run deadline | 504 / SSE error |
| 工具参数错误 | role=tool 的失败 observation，模型可修正 |
| SQLite 保存失败 | 保留已有 metrics，Run 返回内部错误 |
| 客户端 RST | CancelCheck 中止 Provider/Tool |

SSE Header 一旦发出 200，就不能再把 HTTP 状态改成 500，所以后续错误必须是 SSE
`event: error`。

## 20. 当前已经实现与未实现

已实现：

```text
DeepSeek 原生 Tool Calls
DeepSeek Thinking：reasoning 仅在当前 Run 暂存和回传
有界多步 Agent Loop
calculator/time/weather
Session inFlight
SQLite Turn 持久化
Token Budget 与滚动摘要
Run metrics 和安全日志
DeepSeek 真流式 SSE
下游 HTTP Chunked SSE
断连取消和背压
Fake Model 与进程级集成测试
```

未实现：

```text
RAG
长期事实 Memory
多用户鉴权和 owner_id
多进程 Session lease
精确 DeepSeek tokenizer
模型结构化摘要
Prometheus/OpenTelemetry
libcurl multi
写工具审批和业务幂等
多 Agent
```

## 21. 推荐阅读顺序

### 第一阶段：只理解 Agent Loop

```text
1. include/AgentRuntime.h
2. tests/AgentRuntimeTest.cc::testSingleToolRoundTrip
3. src/AgentRuntime.cc::runInternal
4. AGENT_TOOL_CALLING_DESIGN.md
```

目标：能口述模型提出 Tool Call、服务器执行、Tool Result 回模型的闭环。

### 第二阶段：理解 Provider

```text
1. include/DeepSeekProtocol.h
2. src/DeepSeekProtocol.cc
3. src/AgentDemo.cc::DeepSeekClient
4. tests/AgentRuntimeTest.cc::testDeepSeekProtocol
5. AGENT_THINKING_DESIGN.md
```

目标：区分 Runtime 接口、JSON 协议和 libcurl 传输。

### 第三阶段：理解 Session 和 Context

```text
1. include/ConversationContext.h
2. tests/ConversationContextTest.cc
3. src/ConversationContext.cc
4. AGENT_CONTEXT_MANAGEMENT_DESIGN.md
```

目标：区分 Session/Run/Turn，以及 History/Context/Memory/RAG。

### 第四阶段：理解 Streaming

```text
1. include/HttpServer.h::HttpStreamResponder
2. include/DeepSeekProtocol.h::DeepSeekSseParser
3. tests/AgentSseIntegrationTest.py
4. AGENT_SSE_STREAMING_DESIGN.md
```

目标：画出上游 Provider SSE 和下游客户端 SSE 两条链路。

### 第五阶段：理解完整 Service

```text
1. include/AgentDemo.h
2. src/AgentDemo.cc::submitInternal
3. src/AgentDemo.cc::runTurn
4. src/main.cc Agent HTTP 路由
5. AGENT_OBSERVABILITY_DESIGN.md
```

目标：说明每一步在哪个线程执行，为什么不阻塞 EventLoop。

## 22. 建议亲手做的实验

### 实验一：直接回答

修改 Fake Model 直接返回 content，观察：

```text
modelCalls=1
toolExecutions=0
```

### 实验二：单工具闭环

在 `testSingleToolRoundTrip` 打印第二次模型请求 messages，找出：

```text
assistant.tool_calls
tool.tool_call_id
```

### 实验三：工具参数错误

让 Fake Model 返回非法 JSON arguments，观察工具没有执行，但错误作为 observation 回给模型。

### 实验四：预算超限

让 Fake Model 永远返回 tool_calls，观察 Runtime 在预算内停止，而不是无限循环。

### 实验五：上下文预算

在 `ConversationContextTest` 中不断增加 Turn，观察 ContextBuilder 总是保留最近完整 Turn。

### 实验六：重启恢复

运行 `conversation_restart_integration_test`，观察同一个 SQLite 文件跨进程恢复，再通过
`/agent/clear` 删除。

### 实验七：SSE 分片

阅读 `AgentSseIntegrationTest.py`，观察 Mock Provider 如何故意拆分 HTTP write 和中文
UTF-8，证明 callback/read 边界不等于 Event 边界。

### 实验八：客户端取消

观察测试如何使用 `SO_LINGER(1, 0)` 制造 RST，并验证同 Session 很快解除 `inFlight`。

## 23. 自测问题

尝试不看答案口述：

1. 为什么调用一次 DeepSeek 不一定叫 Agent？
2. 模型返回 `tool_calls` 后，工具是谁执行的？
3. Schema 为什么不能替代 C++ 参数校验？
4. `tool_call_id`、`run_id`、`turn_id`、`session_id` 分别是什么？
5. 为什么失败 Run 不应保存为成功 Turn？
6. `inFlight` 和 SQLite transaction 各自保护什么？
7. History、Context、Memory、RAG 有什么区别？
8. TokenEstimator 与 Provider usage 为什么不相等？
9. deadline、timeout、cancel、调用次数预算有什么区别？
10. 为什么 Runtime 不能强杀任意超时工具？
11. 为什么 Provider SSE 与客户端 SSE 不是同一条流？
12. 为什么 HTTP Chunk、SSE Event 和模型 Token 边界不同？
13. 为什么同步 libcurl 不阻塞 EventLoop，却会占用 business worker？
14. 为什么 SSE Header 发出后错误只能作为 event 发送？
15. `agent_trace` 为什么还不算完整 OpenTelemetry Trace？

如果能够结合具体类和数据流回答这些问题，就已经不只是“会调用一个大模型 API”，而是
真正理解了当前项目中 Agent Runtime 的基本工程结构。
