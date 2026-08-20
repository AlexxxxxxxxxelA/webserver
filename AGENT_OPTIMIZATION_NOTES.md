# Agent 异步化优化：问题分析、方案取舍与面试思考

## 1. 文档目的

这份文档记录本项目从“TCP IO 线程中同步调用外部 curl 命令”演进到“有界业务线程池 + libcurl + HTTP Agent API”的完整思考过程。

文档重点不是只描述最终代码，而是回答：

1. 原来的代码有什么问题？
2. 问题在什么场景下触发？
3. 如果不修改，会发生什么？
4. 为什么选择当前方案？
5. 还有哪些可选方案？为什么当前学习项目暂时不选？
6. 面试时可能如何追问？

当前实现目标是一个范围明确的学习版 Agent 服务，不追求一次实现生产级所有能力。

---

## 2. 优化前后的架构对比

### 2.1 优化前

```text
客户端
  |
  v
subLoop IO 线程
  |
  +-> TcpConnection::handleRead
  +-> AgentDemoService::onMessage
  +-> fork
  +-> exec curl
  +-> waitpid 等待 DeepSeek
  +-> 得到结果后 send
```

关键问题：网络 IO 和耗时业务运行在同一个线程。

### 2.2 优化后

```text
客户端
  |
  v
EventLoopThreadPool（IO 线程）
  |
  +-> 读取 Socket
  +-> 解析 HTTP
  +-> 校验 JSON
  +-> trySubmit（立即返回）
          |
          v
BoundedThreadPool（业务线程）
  |
  +-> 调用 libcurl
  +-> 等待 DeepSeek HTTPS
  +-> 执行 calculator/time
  +-> 更新 session 历史
  +-> 调用异步 responder
          |
          v
TcpConnection::send
  |
  +-> queueInLoop 回到连接所属 IO 线程
  +-> 非阻塞发送 HTTP Response
  +-> 数据发送完后回收连接
```

职责划分：

| 组件 | 职责 |
|---|---|
| `EventLoopThreadPool` | epoll、Socket、TCP 连接和快速协议处理 |
| `BoundedThreadPool` | DeepSeek、工具执行等阻塞业务 |
| `HttpServer` | HTTP 请求解析、同步/异步路由衔接、响应发送 |
| `AgentDemoService` | session、多轮上下文、模型规划和工具编排 |
| libcurl | DNS、TCP 主动连接、TLS、HTTPS 请求和响应接收 |

---

## 3. 为什么先实现 `POST /echo`

### 原问题

HTTP 层虽然已经解析 `Content-Length` 和 Body，但此前只有 `GET /health`，没有业务路由真正使用 POST Body。

如果直接开始实现 `/agent/run`，一次请求会同时经过：

```text
HTTP Body 解析
+ JSON 解析
+ session
+ 线程池
+ libcurl
+ DeepSeek
+ 工具调用
+ 异步响应
```

任何环节出错，定位成本都很高。

### 当前修改

先增加：

```http
POST /echo
```

服务端原样返回 Body。

### 为什么这样改

这是工程中的“分层验证”和“控制变量”：

- `/echo` 成功，说明 POST、Content-Length、Body、HttpResponse 没问题。
- `/echo` 失败，问题一定在 HTTP 层，不需要怀疑 Agent 或 DeepSeek。
- `/echo` 稳定后，后续只关注新增变量。

### 如果跳过

可能出现“DeepSeek 没有收到正确 message”，但根因其实是 Body 半包或 JSON 提取错误。多个变量一起变化会让排错非常困难。

### 面试问题

**问：为什么不直接实现最终功能？**

答：复杂链路应该按边界拆分验证。我先使用 `/echo` 验证 HTTP POST 与 Body，再引入线程池和外部 API，这样每一步只有少量新增变量，出现故障时可以快速定位到具体层。

---

## 4. IO 线程为什么不能同步调用 DeepSeek

### 原实现

`AgentDemoService::onMessage()` 是 `TcpConnection` 的消息回调，因此运行在连接所属的 subLoop 线程。

原调用链：

```text
epoll_wait
-> Channel::handleEvent
-> TcpConnection::handleRead
-> AgentDemoService::onMessage
-> DeepSeekClient::chat
-> waitpid 等待 curl
```

### 如果不修改会发生什么

假设 HTTP/DeepSeek 请求需要 5 秒：

- 当前 subLoop 在 5 秒内不能再次执行 `epoll_wait()`。
- 该 subLoop 管理的其他连接无法读取数据。
- 已经准备好的发送事件无法及时处理。
- 新投递到该 loop 的任务无法执行。
- 少量慢请求即可拖慢大量无关连接。

重要理解：One Loop per Thread 不是 One Connection per Thread。一个 subLoop 通常管理很多连接，因此阻塞一个 loop 会影响一组连接。

### 当前修改

增加独立的：

```text
BoundedThreadPool
```

IO 线程只执行：

```text
轻量校验
-> trySubmit
-> 立即返回 EventLoop
```

### 实测结果

本地 mock DeepSeek 故意等待约 2 秒：

```text
AGENT_MS=2037
HEALTH_MS=22
```

Agent 等待期间，`GET /health` 仍约 22 ms 返回，说明 Reactor 没有被外部 API 阻塞。

### 面试问题

**问：既然使用 epoll，为什么服务器仍然可能被阻塞？**

答：epoll 只能让 Socket 就绪等待不阻塞线程，但业务回调本身仍可能执行阻塞函数。如果在 EventLoop 回调中调用同步 HTTPS、磁盘 IO 或 `sleep`，线程仍然会阻塞。因此非阻塞网络框架还要求 IO 回调足够短，耗时业务应转移到独立线程池。

---

## 5. 为什么业务线程池必须“有界”

### 无界队列的问题

假设 DeepSeek 每秒只能完成 4 个请求，但客户端每秒提交 100 个请求。

如果任务队列无上限：

```text
到达速度 > 处理速度
-> 队列持续增长
-> 闭包、message、session 等持续占用内存
-> 等待时间越来越长
-> 最终 OOM 或系统抖动
```

无界队列只是把“处理不过来”隐藏成“内存一直增长”，并没有提高系统真实吞吐量。

### 为什么 `trySubmit()` 不能等待

如果业务队列满时，IO 线程阻塞等待队列空位：

```text
IO 线程等待业务线程池
-> EventLoop 再次被阻塞
-> 又回到了优化前的问题
```

因此 `trySubmit()` 必须立即返回：

```cpp
bool trySubmit(Task task);
```

返回 false 时，HTTP 层快速响应：

```http
HTTP/1.1 503 Service Unavailable
Retry-After: 1
```

### 当前参数

```text
worker 数量：4
最大排队任务：64
```

这些是学习版保守默认值，不代表经过压测后的生产最优参数。

### 停止策略

`stop()`：

1. 停止接收新任务。
2. 唤醒所有 worker。
3. 处理完已进入队列的任务。
4. join 所有线程。

优点是已接受任务不会静默丢失；缺点是外部 API 很慢时，停机等待可能较长。生产服务还会增加取消、停机 deadline 或丢弃未开始任务策略。

### 面试问题

**问：线程池队列为什么不能无限大？**

答：当流量超过处理能力时，无界队列会持续占用内存并让请求延迟无限增长。容量上限属于背压机制，让服务在过载时快速失败，保护整体可用性。

**问：队列满为什么返回 503，而不是继续等待？**

答：继续等待会阻塞 IO 线程或者让用户等待不可预测的时间。503 表示服务暂时没有处理能力，客户端可以根据 Retry-After 稍后重试。

---

## 6. 为什么用 libcurl 替换 `fork + exec curl`

### 原实现

每次 DeepSeek 请求：

```text
mkstemp 创建临时文件
-> 写 JSON payload
-> pipe
-> fork
-> 子进程构造 argv
-> execvp curl
-> 父进程 read pipe
-> waitpid
-> 删除临时文件
```

### 问题一：多线程进程中的 fork 风险

服务器启动后已经存在日志线程和多个 Reactor 线程。

多线程进程调用 `fork()` 后，子进程只保留调用 fork 的线程，其他线程不会复制执行，但它们当时持有的 C/C++ 运行库内部锁状态可能被复制。

在 exec 之前，子进程原则上只应调用 async-signal-safe 函数。原代码在 exec 前构造和修改 `std::vector`，可能触发内存分配器或运行库锁。如果该锁在 fork 时由另一个已消失的线程持有，子进程可能永久死锁。

### 问题二：额外进程与磁盘开销

每个请求都创建进程、临时文件和 pipe：

- 进程创建有开销。
- 临时文件增加磁盘 IO 和清理分支。
- 错误处理路径复杂。
- stdout 和 stderr 容易混合。

### 问题三：密钥暴露面

Bearer Token 作为 curl 进程参数时，可能被系统进程查看工具观察到。

libcurl 在当前进程内设置 Header，不需要出现在另一个进程的 argv 中。

### 当前修改

每个业务任务创建独立 easy handle：

```text
curl_easy_init
-> curl_easy_setopt
-> curl_easy_perform
-> curl_easy_getinfo
-> curl_easy_cleanup
```

每请求一个 easy handle，不跨线程共享。

### 当前边界

```text
连接超时：5 秒
总超时：30 秒
非流式 DeepSeek 完整响应：3 MiB；其中 reasoning/content 各自最多 1 MiB
CURLOPT_NOSIGNAL：开启
TLS 证书验证：保持 libcurl 默认开启
```

错误映射：

| 错误 | HTTP 状态 |
|---|---|
| DeepSeek 未配置 | 503 |
| 业务队列已满 | 503 |
| DeepSeek 超时 | 504 |
| DeepSeek 非 2xx | 502 |
| DeepSeek 响应结构错误 | 502 |
| 本地未预期错误 | 500 |

### 为什么不用自己实现 HTTPS Client

自研 DeepSeek HTTPS Client 还要处理：

- DNS
- 非阻塞 connect
- TLS 握手
- 证书链和主机名验证
- SNI
- `SSL_ERROR_WANT_READ/WRITE`
- HTTP Response 和 Chunked
- 超时和重定向

这会把学习重点从“epoll 服务端框架”转移到“重新实现 libcurl/OpenSSL”。当前选择自己实现入站 HTTP Server，使用成熟库完成出站 HTTPS Client，工程边界更合理。

### 面试问题

**问：使用 libcurl 后还是同步调用，为什么算优化？**

答：`curl_easy_perform()` 确实是阻塞的，但它运行在独立业务线程池，不在 EventLoop 中。优化点不是把所有调用变成异步 API，而是把阻塞边界隔离，保证网络 IO 线程不被阻塞。

---

## 7. libcurl 全局初始化的时机

### 问题

libcurl 有进程级全局状态：

```cpp
curl_global_init(CURL_GLOBAL_DEFAULT);
curl_global_cleanup();
```

如果等日志线程启动后再初始化，在不保证 global init 线程安全的旧版本或特殊构建中可能有风险。

### 当前修改

`main()` 在启动异步日志线程之前调用：

```cpp
initializeAgentRuntime();
```

顺序：

```text
libcurl 全局初始化
-> 日志线程
-> Agent 业务线程池
-> Reactor IO 线程
```

`AgentDemoService` 构造中仍有一次幂等访问，防止该类被其他程序单独使用时忘记初始化。

### 面试问题

**问：为什么全局初始化必须放在线程启动前？**

答：全局初始化可能创建或修改进程级共享状态。最简单可靠的生命周期方式是在任何并发线程出现前初始化，在所有使用线程退出后清理，避免初始化与并发访问交错。

---

## 8. 为什么 HTTP 异步响应不能捕获原请求引用

### 原同步接口生命周期

原回调：

```cpp
std::function<void(const HttpRequest &, HttpResponse *)>
```

其中：

- `HttpRequest&` 指向 `HttpContext` 内部对象。
- `HttpResponse*` 指向 `HttpServer::onRequest()` 的栈对象。
- 回调返回后，Response 被序列化。
- 随后 Context 被 reset。

### 如果直接交给工作线程

错误示例：

```cpp
pool.submit([&request, response]() {
    // 数秒后访问
});
```

工作线程真正运行时：

- `HttpResponse` 栈对象已经销毁。
- `HttpRequest` 可能已经被 reset。
- 访问会产生 use-after-free 或数据错乱。

### 当前修改

IO 线程在提交任务前复制：

```text
session_id
message
responder
```

业务线程不保存 `HttpRequest&` 或 `HttpResponse*`。

异步 responder 内部只保存：

- `weak_ptr<TcpConnection>`
- `shared_ptr<atomic_bool>` exactly-once 标记

### 为什么用 weak_ptr

DeepSeek 返回前客户端可能断开。如果闭包强持有连接：

- 连接可能被不必要地延长生命周期。
- 甚至可能活得比所属 EventLoop 更久。

weak_ptr 允许业务完成时检查连接是否仍存在：

```cpp
TcpConnectionPtr conn = weakConnection.lock();
if (!conn) return;
```

### exactly-once

异步业务存在错误调用 completion 两次的可能。共享原子标记保证只有第一次 responder 生效：

```text
第一次 exchange(false -> true)：发送
第二次 exchange(true -> true)：直接返回
```

### 面试问题

**问：跨线程闭包最常见的生命周期错误是什么？**

答：捕获栈引用、裸 this 或临时对象内部指针。异步任务执行时间晚于调用函数返回，因此应按值复制必要数据，并用 shared_ptr/weak_ptr 明确对象生命周期。

---

## 9. 为什么异步 `/agent/run` 暂时一连接一请求

### 问题：HTTP Pipeline 响应顺序

同一连接可能连续收到：

```text
请求 1：POST /agent/run，耗时 5 秒
请求 2：GET /health，耗时 1 ms
```

如果两个请求并行处理，health 可能先完成。但 HTTP/1.1 Pipeline 要求响应顺序与请求顺序一致。

### 完整方案

生产实现需要每连接维护：

```text
request id
待处理请求队列
已完成响应队列
next response id
in-flight 状态
连接关闭状态
```

这会显著增加当前学习阶段复杂度。

### 当前取舍

普通路由：

- `/health`
- `/echo`

继续支持同步 Keep-Alive 和 Pipeline。

异步路由：

- `/agent/run`

采用：

```text
一条连接只处理一个 Agent 请求
-> 忽略后续 Pipeline 字节
-> 响应声明 Connection: close
-> 响应完整发送后主动回收连接
```

这是明确的功能边界，不是假装支持异步 Pipeline。

### 为什么不能简单 stopRead + shutdownWrite

如果取消 EPOLLIN：

- 服务器无法继续读到客户端 EOF。
- `handleClose()` 不会执行。
- TcpServer 连接表和 HttpContext 可能一直保留。

因此当前仍保留读事件以观察断开，但异步执行期间不再解析后续 HTTP 请求。

为防止客户端永远不主动关闭，还增加：

```cpp
TcpConnection::forceCloseAfterWrite();
```

语义：

- 响应一次直接写完：立即回收连接。
- 响应发生部分写：等待 outputBuffer 清空后回收。
- 不会为了主动关闭而截断响应。

### 面试问题

**问：为什么 Agent API 返回后关闭连接，HTTP/1.1 不是默认 Keep-Alive 吗？**

答：这是当前学习版对异步响应排序的明确取舍。同步路由仍支持 Keep-Alive；慢 Agent 路由采用单请求连接，避免未实现响应排序时返回乱序结果。后续可以加入每连接 request-id 队列再恢复异步 Keep-Alive。

---

## 10. Session 为什么不能直接绑定 TCP 连接

### TCP Agent 的做法

原 TCP Agent 使用：

```text
TcpConnection name -> history
```

这对 TCP 长连接有效，因为同一客户端一直使用一条连接。

### HTTP 的问题

HTTP 客户端可能：

- 每次请求新建连接。
- 使用连接池中的不同连接。
- 经过代理或负载均衡。
- Keep-Alive 连接因空闲而关闭。

如果会话绑定 TcpConnection，重连后上下文就丢失。

### 当前修改

请求显式提供：

```json
{
  "session_id": "demo-1",
  "message": "继续刚才的问题"
}
```

服务端保存：

```text
http:demo-1 -> Session
```

TCP 使用：

```text
tcp:<connection-name> -> Session
```

前缀避免 HTTP 客户端构造 session_id 与内部 TCP 连接名碰撞。

### 当前限制

- session_id 最大 128 字节（内部前缀也计入）。
- 只允许字母、数字、`-`、`_`、`.`、`:`。
- 总 session 数上限 1024。
- 容量满时淘汰最久未使用、且当前不在执行中的 session。
- 消息最大 16 KiB。
- 成功 Turn 持久化到 SQLite，内存 Session 只保存并发状态。
- ContextBuilder 使用 8000 Token 历史估算预算，并保留最近完整 Turn。
- 未摘要 Turn 超过 8 个时生成带 coverage 的确定性滚动摘要。

### 为什么还需要容量淘汰

HTTP session 不会随某条 TCP 连接断开自动删除。如果客户端不断生成新 ID：

```text
session map 无限增长
-> 历史占用无限增长
-> 内存耗尽
```

当前使用简单的访问序号淘汰最久未使用 session。更完整的方案是定时器 + TTL/LRU；由于本项目 TimerQueue 暂未接入，因此先采用容量触发淘汰。

---

## 11. 同一个 session 为什么不能并行处理

### 语义竞态

如果两个请求同时读取同一个历史 H：

```text
请求 A 读取 H
请求 B 读取 H
A 根据 H 回答
B 也根据 H 回答
B 先写历史
A 后写历史
```

虽然 mutex 可以避免容器数据竞争，但对话顺序仍然错误。这叫业务语义竞态。

### 可选方案

1. 同 session 请求排队。
2. worker 阻塞等待 session mutex。
3. busy 时立即拒绝。

### 当前选择

同 session 有请求执行时返回：

```http
HTTP/1.1 409 Conflict
```

原因：

- 实现简单明确。
- 不让多个 worker 线程只用于等待同一把 session 锁。
- 避免热门 session 耗尽线程池。
- 客户端可以等前一请求完成后重试。

不同 session 仍可并行。

### inFlight 为什么所有路径都必须复位

如果出现任何遗漏：

```text
任务异常
-> inFlight 没有改回 false
-> 该 session 永久返回 409
```

当前覆盖：

- 正常结果。
- DeepSeek 错误。
- `runTurn()` 抛异常。
- 未知异常。
- 队列拒绝。
- `trySubmit()` 内存分配异常。

### 实测

第一个 mock Agent 请求执行期间，第二个相同 session 请求：

```text
BUSY_STATUS=409
```

---

## 12. 为什么使用正式 JSON 库

### 原实现问题

原来的 `jsonGetString()` 本质上是在字符串中搜索：

```text
"key"
-> 找下一个冒号
-> 找字符串引号
```

它不真正理解 JSON 结构，因此可能：

- 接受尾随垃圾。
- 在嵌套对象中找到错误字段。
- 错误处理重复 key。
- 破坏 `\u4f60` 这样的 Unicode 转义。
- 无法可靠验证字段类型。

### 当前修改

使用 `nlohmann/json` 处理：

- `/agent/run` 请求 Body。
- HTTP JSON 响应。
- DeepSeek 请求 payload。
- DeepSeek `choices[0].message.content`。
- planner 返回的 tool JSON。

请求必须是对象，并且：

```text
session_id：string
message：string
```

否则返回 400。

### 实测

mock DeepSeek 通过标准 JSON Unicode 转义返回中文，最终客户端收到：

```json
{"answer":"模拟回答","session_id":"mock-session"}
```

### 面试问题

**问：手写 JSON parser 有什么问题？**

答：JSON 包含嵌套结构、转义、Unicode、数字和类型语义，字符串搜索无法正确覆盖边界。公开 HTTP API 应使用经过验证的 parser，避免非法请求被误接受或合法 Unicode 被破坏。

---

## 13. Agent 结果为什么要结构化

### 原接口

```cpp
std::string handleChatLine(...);
```

正常回答和错误都塞在一个字符串里：

```text
DeepSeek planner request failed: ...
```

HTTP 层无法判断这是正常回答还是上游失败，也无法选择合适状态码。

### 当前接口

```cpp
struct AgentResult
{
    Error error;
    std::string answer;
    std::string toolName;
    std::string toolResult;
    std::string errorMessage;
};
```

TCP 适配层将结果格式化成人类可读文本；HTTP 适配层生成 JSON 和状态码。

### 好处

- 业务逻辑不依赖 TCP 文本格式。
- TCP 与 HTTP 复用同一个 Agent 核心。
- HTTP 可以正确区分 500、502、503、504。
- 后续测试可以直接断言 error 类型和 answer。

### 面试问题

**问：为什么不直接返回字符串？**

答：字符串无法表达机器可判断的错误分类。结构化结果可以让不同协议适配层决定各自的表现形式，例如 TCP 文本提示或 HTTP 状态码和 JSON。

---

## 14. 配置和密钥处理

### 当前支持

配置文件：

```text
config/agent_demo.conf
```

该文件已被 `.gitignore` 忽略。

环境变量可覆盖：

```text
DEEPSEEK_API_KEY
DEEPSEEK_API_URL
DEEPSEEK_MODEL
```

### 为什么支持环境变量

- CI、容器和部署环境不需要写真实配置文件。
- 测试可以把 URL 指向本地 mock。
- 密钥不进入 Git。
- 不同环境使用相同二进制。

### 注意

已经进入历史的 Key 即使后来删除，也应该在平台侧吊销。Git 历史重写和 `.gitignore` 是减少暴露面，不能让已泄露 Key 自动失效。

---

## 15. HTTP API 使用方式

### Weather 工具

Planner 现在还可以选择：

```json
{"tool":"weather","input":"Beijing"}
```

天气请求复用现有业务线程池和 libcurl，默认访问 `https://wttr.in`。工具位置最多
128 字节，连接超时 3 秒、总超时 8 秒、响应上限 16 KiB。可使用
`WEATHER_API_BASE_URL` 指向本地 mock，避免测试依赖公网。

### 健康检查

```bash
curl -v http://127.0.0.1:18081/health
```

### Echo

```bash
curl -v -X POST \
  --data-binary 'hello' \
  http://127.0.0.1:18081/echo
```

### Agent

```bash
curl -v -X POST \
  -H 'Content-Type: application/json' \
  -d '{"session_id":"demo-1","message":"计算 12 * 15"}' \
  http://127.0.0.1:18081/agent/run
```

成功响应示例：

```json
{
  "session_id": "demo-1",
  "answer": "12 * 15 等于 180。",
  "tool": "calculator",
  "tool_result": "180"
}
```

### Content-Type

支持：

```text
application/json
application/json; charset=utf-8
```

其他媒体类型返回 415。

---

## 16. 当前依赖与构建

新增依赖：

```text
libcurl4-openssl-dev
nlohmann-json3-dev
pkg-config
```

WSL Ubuntu 安装：

```bash
sudo apt-get update
sudo apt-get install -y libcurl4-openssl-dev nlohmann-json3-dev pkg-config
```

构建：

```bash
cmake -S . -B build
cmake --build build --parallel
```

CMake 最低版本提升到 3.12，因为使用了：

```cmake
CURL::libcurl
```

imported target。

---

## 17. 已完成验证

### 编译

- WSL Ubuntu 24.04。
- GCC 13.3。
- C++11。
- 完整 CMake 构建成功。
- Agent/HTTP/线程池代码通过 `-Wall -Wextra -Wpedantic -fsyntax-only`。
- 只剩原 LFU 模块的 signed/unsigned 比较告警。

### HTTP 路由

- `POST /echo` 原样返回 Body。
- `GET /echo` 返回 405。
- `/agent/run` 字段缺失返回 400。
- 错误 Content-Type 返回 415。
- 同 session 并发返回 409。

### 异步性

mock DeepSeek 延迟约 2 秒：

```text
AGENT_MS=2037
HEALTH_MS=22
```

### Unicode

mock 使用标准 JSON Unicode 转义返回中文，最终响应中文正确。

### 连接回收

Agent 响应完成后检查 18081：

```text
ESTABLISHED_AFTER_RESPONSE=0
```

### 线程池

测试配置：1 worker + 1 queue slot。

- 第一个任务运行。
- 第二个任务进入队列。
- 第三个任务立即拒绝。
- stop 后两个已接受任务均完成。

---

## 18. 当前仍然存在的边界

这些是明确知道但当前学习阶段暂不继续扩展的内容：

1. `/agent/run` 异步路由不支持同连接 Pipeline 和 Keep-Alive 复用。
2. session 只有容量淘汰，没有基于真实时间的 TTL。
3. 停机时业务线程池会排空队列，最坏等待时间可能较长。
4. DeepSeek 调用使用阻塞式 `curl_easy_perform()`，但已隔离到业务线程。
5. 已使用 DeepSeek 原生 tool_calls，并提供 `/agent/run/stream` SSE 流式返回；当前
   一个流仍会占用一个阻塞业务 worker，尚未升级 libcurl multi。
6. 尚未实现登录、鉴权和用户级 session 权限。
7. 已增加 AgentRuntime、HTTP/Provider、SSE、SQLite 和进程重启恢复测试；Session TTL、
   多用户权限和多进程租约仍未实现。
8. Tool Calling 有统一 60 秒 deadline；SSE 客户端断开可取消 DeepSeek/Weather，普通
   非流式请求断开仍不会取消已运行任务。

这些边界可以在面试中主动说明。主动说明取舍通常比声称所有功能都已完整实现更可信。

---

## 19. 后续合理演进顺序

如果继续开发，建议顺序：

1. 扩充线程池、HTTP 和 Session 自动化测试。
2. 为 SSE 增加 heartbeat、写停滞 timeout 和可选断线重放。
3. 为 429/500/503 增加受总 deadline 限制的有界重试。
4. 为 session 增加 TimerQueue TTL 清理。
5. 增加服务停止 API 和 shutdown deadline。
6. 实现异步 HTTP 请求序号和响应排序，恢复 Agent Keep-Alive。

暂时不建议优先做：

- RAG。
- 向量数据库。
- MCP。
- 多 Agent。
- 自研 TLS。

因为当前项目的核心学习价值仍是 Reactor、线程模型、协议状态机和异步生命周期。

---

## 20. 面试复述模板

### 为什么优化 Agent

> 原 Agent 在 TcpConnection 的消息回调中同步 fork/exec curl，请求 DeepSeek 时会阻塞连接所属的 EventLoop，导致同一个 subLoop 上的其他连接无法及时处理。我将阻塞业务迁移到独立有界线程池，IO 线程只做协议解析和非阻塞任务提交，队列满时快速返回 503，从而保护 Reactor。

### 为什么用 libcurl

> 原方案在多线程进程中 fork，并在 exec 前使用 C++ 容器，存在运行库锁继承导致死锁的风险；每次创建进程和临时文件开销也较大。我改为 libcurl，让成熟库处理 DNS、TLS 和 HTTPS，并设置连接超时、总超时及响应大小限制。libcurl easy 调用仍是阻塞的，但运行在业务线程池中，不阻塞 IO loop。

### 如何处理异步生命周期

> 原 HttpRequest 引用和 HttpResponse 指针都只在同步回调期间有效，不能传给后台线程。我在 IO 线程提取并复制 session_id 和 message，异步 responder 使用 weak_ptr 观察 TcpConnection，并用共享原子标记保证只响应一次。客户端提前断开时 weak_ptr 提升失败，结果会被安全丢弃。

### 如何保证 session 一致性

> 全局 mutex 只保护 session map，每个 session 有独立 mutex 和 inFlight 标记。同一个 session 同时只接受一个请求，重复请求返回 409；不同 session 可以并行。这样既避免同一历史被两个请求同时读取造成语义乱序，也不会让多个 worker 阻塞等待热门 session 的锁。

### 为什么 Agent 请求后关闭 HTTP 连接

> 完整支持异步 HTTP Pipeline 需要按连接维护请求序号和响应排序。当前学习版为控制复杂度，对异步 `/agent/run` 采用一连接一请求，响应完整发送后主动关闭；同步 `/health` 和 `/echo` 仍支持 Keep-Alive。这个限制是明确的设计取舍，而不是协议实现遗漏。
