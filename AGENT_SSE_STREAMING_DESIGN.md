# Agent SSE Streaming 设计与阅读导读

本阶段的候选方案、背压/取消取舍和复审问题集中记录在
`AGENT_ENGINEERING_DECISIONS.md` 的 ADR-012 至 ADR-017。

## 1. 本阶段实现了什么

保留原有非流式接口：

```text
POST /agent/run
-> application/json
-> 等完整 Agent Run 完成后一次返回
```

新增真正的流式接口：

```text
POST /agent/run/stream
-> text/event-stream
-> HTTP/1.1 Chunked Transfer-Encoding
-> Agent 生命周期事件
-> DeepSeek assistant 文本增量
```

调用示例：

```bash
curl -N \
  -H "Content-Type: application/json" \
  -d '{"session_id":"demo","message":"北京天气怎么样？"}' \
  http://127.0.0.1:18081/agent/run/stream
```

`curl -N` 禁用客户端输出缓存，否则服务器已经发送 delta，终端也可能暂时不显示。

## 2. 为什么这是真流式，不是“伪 SSE”

伪流式实现通常是：

```text
完整等待 DeepSeek Response
-> 得到完整答案
-> 把完整答案包装成一个 SSE event
```

当前实现是：

```text
DeepSeek stream=true
-> libcurl write callback 收到部分 Provider SSE 字节
-> DeepSeekSseParser 恢复完整 Provider Event
-> 解析 choices[].delta.content
-> 立即发 assistant.delta 到下游 SSE
-> Provider 尚未完成时客户端已经看到首段文本
```

集成测试让 Mock Provider 在第一个 delta 后等待 250 ms，并断言客户端提前收到
`assistant.delta`，因此不是最终完成后一次性发送。

## 3. 上游和下游不是同一条 SSE

### 上游 DeepSeek

```text
TCP/TLS -> HTTP Response -> Provider SSE -> DeepSeek JSON
```

但 libcurl 已经处理 TCP、TLS 和 HTTP framing，所以 `DeepSeekSseParser::feed()` 实际收到：

```text
HTTP Response Body 的任意字节分片
-> 恢复 Provider SSE Event
-> 解析 data 中的 JSON
```

Parser 不会看到或解析 HTTP Chunk Header。

### 下游客户端

```text
AgentEvent
-> HttpStreamResponder 生成项目自己的 SSE event/data
-> HttpStreamState 包装 HTTP Chunk
-> TcpConnection 写 TCP
```

所以项目不是透明转发 DeepSeek 原始 SSE，而是把 Provider 数据转换成统一的 Agent 事件。

## 4. 四层消息边界

流式链路中存在四层不同边界：

```text
TCP 字节流
-> HTTP Chunked
-> SSE Event
-> data 中的 JSON
```

### TCP

TCP 不保留 `write()` 边界。一次 libcurl callback 可能收到半行，也可能收到多个事件。

### HTTP Chunked

格式：

```text
<十六进制 payload 字节数>\r\n
<payload>\r\n
```

终止符：

```text
0\r\n\r\n
```

### SSE

当前一个事件格式：

```text
event: assistant.delta\n
data: {"text":"北京"}\n
\n
```

空行是 SSE 事件边界。

### JSON

`data:` 使用单行 JSON。字符串换行由 JSON 转义，不会破坏 SSE 空行边界。

关键结论：

```text
TCP read 边界 != HTTP chunk 边界
HTTP chunk 边界 != SSE event 边界
SSE event 边界 != Provider delta 边界
Provider delta 边界 != 模型 token 边界
```

不能把任何一层的一次回调直接当成上一层完整消息。

一个 delta 可能包含半句话或多个 Token，不能用 delta 数量计算 Token 使用量。

## 5. 下游 HTTP Streaming Responder

位置：

```text
include/HttpServer.h::HttpStreamResponder
src/HttpServer.cc::HttpStreamState
```

业务层只能使用：

```text
start()
sendEvent(type, json)
finish()
reject(response)
cancelled()
```

业务层不能取得 Socket，也不能直接拼 Chunk Header。

### Header

```http
HTTP/1.1 200 OK
Content-Type: text/event-stream; charset=utf-8
Cache-Control: no-cache, no-transform
X-Accel-Buffering: no
Transfer-Encoding: chunked
Connection: close
```

不能同时出现：

```text
Content-Length
Transfer-Encoding: chunked
```

所以普通 `HttpResponse` 仍负责固定长度响应，Streaming 使用独立序列化路径；
`HttpResponse::addHeader()` 也拒绝业务层设置 `Transfer-Encoding`。

## 6. Stream 状态机

```text
kPending
-> kOpen
-> kFinished

kPending/kOpen
-> kDisconnected
```

语义：

| 状态 | 含义 |
|---|---|
| Pending | 还没有提交 HTTP 200 Header，可以 reject 普通 JSON |
| Open | Header 已发送，只能继续 SSE event 或 finish |
| Finished | terminal 已提交，不允许迟到事件 |
| Disconnected | 客户端断开或背压超限，上层必须取消 |

`sendEvent()`、`finish()`、`reject()` 和 `disconnect()` 由同一 mutex 串行化。

网络层只能保证：

```text
Header/Event/Terminal 最多提交一次且保持 FIFO
```

不能保证客户端一定处理了事件。TCP 没有业务级 ACK，所以这里不能宣称网络交付
`exactly-once`。

## 7. Header、事件与关闭如何保持顺序

普通 `TcpConnection::send()` 在 IO 线程可能立即 write；Streaming 需要所有写动作统一
排队，因此新增：

```text
sendQueued(data)
sendAndForceClose(terminal)
```

顺序：

```text
sendQueued(Header)
sendQueued(Event 1 Chunk)
sendQueued(Event 2 Chunk)
sendAndForceClose("0\r\n\r\n")
```

这些任务进入同一个连接所属 EventLoop 的 FIFO pending functors。terminal 数据和
`forceCloseAfterWrite` 又在同一个任务中执行，关闭不会越过最后一个 Chunk。

## 8. 背压与待发送字节

只检查 `outputBuffer_.readableBytes()` 不够，因为 worker 调用 `send()` 后，数据可能还在
EventLoop 任务队列中，没有进入 outputBuffer。

因此 `TcpConnection` 新增原子：

```text
pendingOutputBytes_
```

记账：

```text
接受 send/sendQueued 时加
直接 write 成功时减
EPOLLOUT write 成功时减
写错误未入 Buffer 的部分减
异常关闭时清理 outputBuffer 对应部分
```

SSE 每次接受新事件前检查：

```text
连接待发送字节 <= 1 MiB
单个 Run 累计事件正文 <= 4 MiB
单个 JSON data <= 64 KiB
```

超过限制：

```text
标记 Disconnected
停止继续生成
排空已接受输出后关闭
```

第一版采用“慢客户端直接取消”，没有实现 low-water 后暂停/恢复 Provider。这比无界缓存
安全，也比让 EventLoop 阻塞等待简单。

## 9. Provider SSE Parser

位置：

```text
include/DeepSeekProtocol.h::DeepSeekSseParser
src/DeepSeekProtocol.cc
```

Parser 保存：

```text
pendingLine_
dataLines_
dataBytes_
content accumulator
tool calls by index
finishReason
usage
DONE 状态
```

### 为什么先按行再解析 JSON

libcurl callback 可能在 JSON 中间、CRLF 中间或中文 UTF-8 中间分片。只有遇到 SSE 空行
才能确认完整 `data`，之后再调用 JSON Parser。

### Event 上限

不仅限制单行，也限制一个事件全部 `data:` 行累计最多 1 MiB。否则 Provider 可以发送
大量短行但不发送空行，绕过单行限制并持续占用内存。

### `[DONE]`

```text
data: [DONE]

```

是严格终态。之后再收到任何 SSE 行都会判为协议错误，不能继续污染 content、usage 或
tool calls。

## 10. 流式 Tool Calls

Provider 可能把函数调用拆成：

```text
id: "stream-weather-" + "1"
name: "wea" + "ther"
arguments: "{\"loc" + "ation\":\"Beijing\"}"
```

Parser 按 `tool_calls[].index` 聚合：

```text
id
function.name
function.arguments
```

只有确认：

```text
finish_reason == tool_calls
[DONE] 已到达
id/name 完整
参数大小未超限
```

才把 Tool Call 交给 Runtime。参数在 Registry 中仍会重新做 JSON 和业务校验。

工具参数不会作为下游 SSE 事件发送，避免暴露模型生成的敏感或错误参数。

## 11. Agent 生命周期事件

当前事件顺序示例：

```text
run.started
model.started
model.completed
tool.started
tool.completed
model.started
assistant.delta
assistant.delta
model.completed
run.completed
```

### run.started

```json
{"run_id":"run-...","queue_wait_ms":0}
```

### model.started

```json
{"run_id":"run-...","sequence":1}
```

### model.completed

```json
{
  "run_id":"run-...",
  "sequence":1,
  "ok":true,
  "error":"none",
  "latency_ms":24,
  "tokens":12
}
```

### tool.started/tool.completed

只包含：

```text
tool_call_id
tool name
success
latency
```

不包含工具 arguments 和 output。

### assistant.delta

```json
{"run_id":"run-...","sequence":2,"text":"北京"}
```

工具调用轮次也可能输出“正在查询”之类的说明文本，所以每个 delta 携带模型调用
`sequence`。客户端只拼接 `run.completed.answer_sequence` 对应的 delta 得到最终答案；
其他 sequence 可作为过程文本单独展示，不能混入最终答案。

### run.completed

```json
{
  "run_id":"run-...",
  "answer_bytes":24,
  "answer_sequence":2,
  "metrics":{...}
}
```

终态不重复携带完整 answer。原因是答案可能接近 1 MiB，把它再次塞进一个事件会超过
64 KiB 单事件限制，也浪费网络带宽。正文唯一来源是 `assistant.delta`。

## 12. Pre-commit 与 Post-commit 错误

### Header 发送前

仍能返回正常 HTTP 状态：

```text
400 JSON 错误
405 Method Not Allowed
409 Session Busy
415 Content-Type 错误
503 Queue Full
```

使用：

```text
stream.reject(HttpResponse)
```

### Header 发送后

HTTP 状态已经是 200，不能改成 429/500。发送：

```text
event: error
data: {"run_id":"...","code":"...","message":"..."}
```

然后发送 terminal chunk 并关闭。

## 13. 取消传播

链路：

```text
TCP RST/HUP/write error
-> HttpStreamState::disconnect
-> HttpStreamResponder::cancelled
-> Agent Runtime CancelCheck
-> DeepSeek/Weather CURLOPT_XFERINFOFUNCTION
-> callback 返回非零
-> curl_easy_perform 终止
-> Session inFlight 复位
```

C ABI callback 中不能抛 C++ 异常，因此 write/progress callback 均使用 `try/catch`，异常
转换为取消或 write abort。

### TCP FIN、RST 与完整断开

客户端 `shutdown(SHUT_WR)` 发送 FIN，只表示不再发送请求，但仍可以读取 SSE。
Streaming 连接会关闭 EPOLLIN、继续写响应。FIN 不能被可靠区分为“只半关闭”还是普通
`close()` 的第一阶段，所以此时不会立即取消；真正 RST、HUP 或后续 write error 才完整
回收并通知 Agent 取消。测试使用 `SO_LINGER(1, 0)` 主动制造 RST 覆盖及时取消路径。

## 14. UTF-8

网络可以在一个中文字符的三字节 UTF-8 编码中间分片。

服务端不直接把任意 libcurl 原始块当文本，而是：

```text
恢复完整 SSE event
-> 解析完整 JSON
-> JSON String 得到完整 delta
-> 下游重新 JSON 编码
```

测试客户端同样使用增量 UTF-8 Decoder，不能对每个单字节调用一次普通 `decode()`。

## 15. 线程模型

当前上游仍是：

```text
阻塞 curl_easy_perform
运行在 BoundedThreadPool worker
```

它不会阻塞 EventLoop，但一个长流会长期占用一个业务 worker。当前只有 4 个 worker，
所以最多约 4 个 Agent Run 同时执行，其余进入有界队列。

第一版先验证协议、生命周期和背压；后续若要大量并发流，应升级为：

```text
libcurl multi
+ epoll
+ 非阻塞 Provider Client
```

## 16. 自动化测试

### AgentRuntimeTest

新增 Parser 单测：

```text
逐字节 feed 中文 delta
CRLF/LF
SSE comment
include_usage choices=[]
Tool Call id/name/arguments 跨事件分片
[DONE] 后数据严格拒绝
```

### AgentSseIntegrationTest.py

启动：

```text
真实 bin/main
Mock DeepSeek Chunked SSE
Mock Weather
```

覆盖：

```text
响应 Content-Type
Transfer-Encoding: chunked
不存在 Content-Length
Provider HTTP chunk 被拆成多次 socket write
Tool Call 分片聚合
[DONE] 后数据拒绝和单 Event 1 MiB 上限
生命周期事件顺序
首 delta 提前到达
超过 64 KiB 的完整答案仍有 run.completed
中文 UTF-8 增量解码
Token usage=35
普通 400 pre-commit 错误
客户端 RST 断开取消
同 Session inFlight 释放
```

运行：

```bash
ctest --test-dir build --output-on-failure
```

SSE 阶段直接相关的三项测试：

```text
agent_runtime_test
agent_http_integration_test
agent_sse_integration_test
```

完整仓库还会运行 `conversation_context_test`、`conversation_restart_integration_test` 和
`chat_client_test`，共六项 CTest。

## 17. 当前边界

当前明确未实现：

```text
SSE heartbeat
Last-Event-ID 断线重放
Run 持久化和重新订阅
低水位暂停/恢复 Provider
写停滞 Timer
libcurl multi
跨进程 Trace Context
反向代理部署测试
```

当前策略：

```text
一条 Streaming HTTP 连接只处理一个 Run
完成后发送 terminal chunk 并主动关闭
待发送超过 1 MiB就取消慢流
单 Run 事件正文最多 4 MiB
客户端断开不保存不完整对话
```

## 18. 推荐阅读顺序

```text
1. include/HttpServer.h::HttpStreamResponder
2. src/HttpServer.cc::HttpStreamState
3. include/TcpConnection.h::sendQueued/sendAndForceClose/pendingOutputBytes
4. src/TcpConnection.cc 对应实现
5. include/DeepSeekProtocol.h::DeepSeekSseParser
6. src/DeepSeekProtocol.cc::DeepSeekSseParser
7. include/AgentRuntime.h::AgentEvent
8. src/AgentRuntime.cc::runInternal
9. src/AgentDemo.cc::DeepSeekClient::completeStreaming
10. src/AgentDemo.cc::submitStreaming
11. src/main.cc::onStreamingHttpRequest
12. tests/AgentSseIntegrationTest.py
```

阅读时回答：

1. 为什么 Chunked、SSE 和 JSON 需要三套独立边界？
2. 为什么普通 HttpResponse 不能直接用于 Streaming？
3. 为什么 terminal chunk 和 `run.completed` 都需要？
4. 为什么 `pendingOutputBytes` 必须统计 EventLoop 队列中的消息？
5. 为什么最终事件不重复携带 answer？
6. 为什么 Tool Call 参数必须聚合完整后再执行？
7. 为什么 TCP FIN 不一定表示客户端不能继续读？
8. 为什么 C ABI callback 不能让 C++ 异常越过边界？
9. 为什么客户端断开后不保存半截回答？
10. 为什么 SSE 改善 TTFT，但不自动提高 Agent 吞吐量？
