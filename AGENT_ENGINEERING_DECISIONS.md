# Agent 工程决策、问题修复与复盘记录

## 1. 文档目的

这份文档记录项目演进中的工程判断，不记录逐字逐句的私人思维过程，而是记录可以复查、
讨论和验证的决策依据：

```text
当时的问题是什么？
有哪些候选方案？
最终选择了什么？
为什么没有选择其他方案？
代码复审发现过什么缺陷？
缺陷如何修复？
用什么测试证明？
当前还保留什么边界？
```

它与其他文档的分工：

| 文档 | 作用 |
|---|---|
| `AGENT_LEARNING_GUIDE.md` | 建立初学者概念地图 |
| `AGENT_TOOL_CALLING_DESIGN.md` | Tool Calling 当前设计 |
| `AGENT_OBSERVABILITY_DESIGN.md` | Trace、Metrics 和错误分类当前设计 |
| `AGENT_SSE_STREAMING_DESIGN.md` | SSE 当前设计 |
| `AGENT_CONTEXT_MANAGEMENT_DESIGN.md` | SQLite 上下文当前设计 |
| 本文档 | 为什么演进成现在这样，以及修过哪些问题 |

## 2. 如何阅读工程决策

不要把“最终方案”理解成永远正确。工程决策总是在约束下做出的：

```text
当前是 C++11 学习项目
单机、单进程、本地单用户
已有 epoll/Reactor 和有界业务线程池
目标是秋招展示和可学习性
需要避免过早引入大量基础设施
```

如果约束改变，例如变成公网多用户、多实例或高并发 Streaming 服务，部分决策必须重新
评估。好的设计不是“永不修改”，而是明确当前成立条件和未来触发重构的信号。

---

## ADR-001：选择性迁移旧项目，而不是整仓合并

### 问题

旧目录 `mywebserver` 功能更多，但混有：

```text
fork/exec curl
阻塞 Reactor 的风险
真实密钥痕迹
实验性 Timer/静态文件/sendfile
生命周期和协议边界不完整
```

### 候选方案

1. 直接合并整个旧目录。
2. 完全放弃旧目录。
3. 只迁移有价值的思想，并按当前架构重新实现。

### 决策

选择方案 3，只迁移：

```text
Weather Tool
TCP QPS 模式
target-based CMake
```

暂不迁移 TimerQueue、静态文件和 sendfile。

### 原因

整仓合并会把已知风险重新带入主项目；完全放弃又会浪费可复用的功能思路。选择性重写
能够保留学习价值，同时让新功能服从当前线程、资源和测试边界。

### 验证

`MYWEBSERVER_MIGRATION_NOTES.md` 记录迁移范围；Weather Mock、QPS 半包和全新 CMake
构建均有验证。

### 当前边界

静态文件和 sendfile 只有在项目真的托管磁盘文件时才有价值；TimerQueue 应在实现 TTL、
idle timeout 等真实需求时再接入。

---

## ADR-002：Weather 使用进程内 libcurl，不使用子进程

### 问题

旧 Weather 使用：

```text
fork -> exec curl
```

这会增加子进程、参数传递、回收和 Shell 安全复杂度。

### 候选方案

1. 继续 fork/exec 系统 curl。
2. 自己实现 HTTP/TLS Client。
3. 使用已有 libcurl，并放入业务线程池。

### 决策

选择方案 3。

### 原因

libcurl 已解决 HTTPS、DNS、证书、超时和代理；自研 TLS Client 与 Agent 主线无关。同步
libcurl 虽会阻塞当前 worker，但不会阻塞 EventLoop，符合现有线程隔离模型。

### 同时加入的边界

```text
location <= 128 字节
connect timeout = 3 秒
total timeout <= 8 秒且服从 Run 剩余时间
response <= 16 KiB
curl_easy_escape URL 编码
SSE 断连取消
```

### 复审修复

- Weather 错误最初可能把 curl/DNS/TLS 细节送入 Tool Result，后改成稳定错误文案。
- libcurl C 回调中的 `std::string::append` 可能抛异常，后加入 C ABI 内部 `try/catch`。

### 验证

HTTP Mock Weather 成功与失败路径均由集成测试覆盖，失败正文不会进入模型、响应或日志。

---

## ADR-003：从自定义 Planner JSON 升级为原生 Tool Calls

### 问题

旧流程要求模型返回：

```json
{"tool":"weather","input":"Beijing"}
```

模型可能返回 Markdown、非法 JSON、错误字段；协议不能自然表达同轮多个工具和标准结果
关联，也只能执行固定的一次 Planner、一次 Tool、一次 Final Model。

### 候选方案

1. 继续增强 Prompt 和容错 Parser。
2. 使用 DeepSeek 原生 `tools/tool_calls/tool_call_id/role=tool`。
3. 直接引入完整 Agent 框架。

### 决策

选择方案 2，并自研小型 `AgentRuntime`。

### 原因

原生协议减少自定义格式歧义；自研 Runtime 能与现有 C++ Server 深度结合，也便于讲清
状态机、生命周期和资源边界。直接引入大型框架会隐藏本项目最有学习价值的执行过程。

### 实现结果

```text
AgentModelClient
AgentTool
AgentToolRegistry
AgentRuntime
```

支持同轮多个 Tool Calls、连续多轮工具调用和失败 observation。

### 复审修复

- 增加空或重复 `tool_call_id` 拒绝，并扩展为整个 Run 内不可复用。
- 校验 Provider `role`、`type=function` 和 `finish_reason`。
- 工具执行前确认还保留一次模型调用预算，避免动作完成后无法生成最终回答。
- 工具名限制为 `[A-Za-z0-9_-]{1,64}`，避免日志和协议污染。

### 验证

Fake Model 单元测试覆盖直接回答、单工具、多工具、多步骤、非法参数、未知工具、重复 ID、
预算终止和 Provider 错误。

---

## ADR-004：Tool Schema 只帮助生成，C++ 校验才是安全边界

### 问题

模型看到 JSON Schema 后，是否可以直接执行它返回的参数？

### 候选方案

1. 信任模型或 Provider strict schema。
2. 应用侧重新解析和验证全部参数。

### 决策

选择方案 2。

### 原因

模型输出是不可信输入。Schema 不能处理用户权限、目录/网络白名单、业务长度、剩余
deadline，也不能保证 Provider 永远正确执行 strict 模式。

### 当前验证

```text
工具是否注册
arguments <= 16 KiB
必须是 JSON object
必填字段和类型
未知字段拒绝
结果 <= 16 KiB 且可序列化为 UTF-8 JSON
工具异常转换为稳定失败结果
```

### 当前边界

只允许只读工具。写工具必须先补审批、持久化幂等键、执行记录和恢复语义。

---

## ADR-005：使用有界 Agent Loop，而不是无限自主执行

### 问题

模型可以持续要求调用工具。如果没有终止条件，会产生无限循环、成本失控和线程长期占用。

### 决策

同时限制：

```text
maxModelCalls = 6
maxToolCalls = 8
absolute Run deadline = 60 秒
```

### 为什么三个限制都需要

| 限制 | 防止什么 |
|---|---|
| Model Calls | 模型反复决策但不结束 |
| Tool Calls | 一轮或累计执行过多工具 |
| Deadline | 每次都没超时但总时间不断累加 |

### 复审修复

- Deadline 从任务入队前开始，业务队列等待也消耗预算。
- 区分 Run Deadline 与 Provider Timeout，前者不归因给 DeepSeek。
- Cancel 明确为协作式信号，不宣称可以强杀任意 C++ 工具。

### 当前边界

只有工具主动使用 `AgentToolContext`，运行中的阻塞 I/O 才能及时服从 deadline/cancel。

---

## ADR-006：最初关闭 DeepSeek Thinking（已由 ADR-025 替代）

### 问题

DeepSeek Thinking 模式使用 Tool Calls 时，后续请求需要回传 `reasoning_content`。项目当时
没有对应消息模型和安全策略。

### 候选方案

1. 忽略字段并依赖 Provider 默认值。
2. 保存和回传完整 reasoning content。
3. 显式关闭 Thinking。

### 决策

选择方案 3：

```json
{"thinking":{"type":"disabled"}}
```

### 原因

当前目标是建立标准 Tool Calling Runtime，不是展示或持久化思维链。隐式依赖默认值会
导致 Provider 升级后协议突然失效；保存 reasoning 又会增加隐私、存储和消息兼容问题。

### 当时的重新评估条件

只有当项目设计好 `reasoning_content` 的回传、日志禁止和存储边界后再开启。当前这些
条件已经满足，因此由 ADR-025 替代，但本条保留用于说明当时关闭是正确的阶段性选择。

---

## ADR-007：先建立 Fake Model 测试，再继续扩展 Agent

### 问题

真实模型具有网络依赖、费用、随机性和 Provider 波动，无法稳定验证状态机。

### 候选方案

1. 每次测试调用真实 DeepSeek。
2. 只做手工测试。
3. 通过 `AgentModelClient` 注入 Fake Model，并保留真实 Mock HTTP 集成测试。

### 决策

选择方案 3。

### 原因

Fake Model 精确控制每一步响应，适合验证 Tool Call 消息顺序、预算和错误；本地 Mock
Provider 则验证真实 libcurl、JSON、HTTP、SSE 和 Server 生命周期。两类测试互补。

### 测试经验

多个集成测试固定占用 `18080/18081/19090`，不能并行执行。CTest 使用 `RUN_SERIAL`；
曾经并行启动两个独立构建目录导致端口竞争和误连旧服务，后改为严格串行，并在测试中
检查新子进程是否仍存活。

---

## ADR-008：Run ID 只用于观测，不承担鉴权或幂等

### 问题

需要把客户端响应、模型步骤、工具步骤和日志关联起来。

### 决策

生成：

```text
run-<时间前缀>-<进程内原子序号>
```

### 为什么不复用它做幂等键

Run ID 由服务器为每次执行产生。客户端重试时会获得新 Run ID，无法识别两个请求是否
是同一业务操作。写工具幂等键必须由业务方提供并持久化。

### 当前边界

Run ID 不是用户身份、Session ID、分布式全局 ID 或 Trace Context。

---

## ADR-009：计时使用 steady_clock，标识使用 system_clock

### 问题

系统时间可能因 NTP 或人工校时向前/向后跳变。

### 决策

```text
system_clock：生成可读 Run ID 前缀
steady_clock：queue/model/tool/total latency 和 deadline
```

### 原因

耗时要求单调，不能因墙钟回拨得到负延迟；标识前缀则需要与现实时间大致对应。

---

## ADR-010：Provider 错误分类后再映射 HTTP，不直接透传

### 问题

所有错误返回 502 会失去诊断意义；直接透传 Provider 状态和 Body 又会泄露内部信息或
误导客户端。

### 决策

建立稳定内部类别，再映射下游：

```text
DeepSeek 401 -> 本服务 503
DeepSeek 402 -> 本服务 503
DeepSeek 429 -> 本服务 429 + Retry-After
DeepSeek 500/503 -> 本服务 503 + Retry-After
Provider timeout -> 504
Run deadline -> 504，但使用不同错误码
```

### 为什么上游 401 不是下游 401

DeepSeek 401 表示服务器配置的 API Key 错误，不是调用本服务的客户端身份失败。返回 401
会让客户端去修改错误的凭据。

### 复审修复

- HTTP status 为 0 时先看 CURLcode，避免 DNS/TLS/连接超时误分类成未知 HTTP 状态。
- Provider Body、curl、DNS、TLS 和 `exception::what()` 不进入客户端或 Tool Result。

---

## ADR-011：可观测性是旁路能力，不能阻止业务完成

### 问题

Agent 多步骤后，仅有最终状态无法定位慢点和失败点；但日志本身也可能分配失败或抛异常。

### 决策

聚合一次 Run 的：

```text
queue/model/tool/total latency
model/tool steps
Token usage
Provider status
```

并输出安全 `agent_trace` 完成日志。

### 取舍

当前不是完整 OpenTelemetry 或 Prometheus，只做单 Run 观测。详细日志不保存 Prompt、用户
消息、工具参数/结果和 API Key。

### 复审修复

- `localtime()` 改为 `localtime_r()`，消除多线程共享静态 `tm` 数据竞争。
- 日志 JSON 序列化失败不能吞掉 Completion。
- Logger 析构隔离 output sink 异常，避免 `noexcept` 析构触发 `terminate`。
- Model Client 第二步抛异常时保留第一步模型、工具和 Token metrics。
- SQLite 保存失败也保留已完成的模型与工具 metrics。

---

## ADR-012：实现真正的上游与下游 Streaming，而不是伪 SSE

### 问题

非流式请求必须等待 Planner/Tool/Final Answer 全部完成，首段文本延迟高。只把最终答案
包装成一个 SSE Event 不会改善首段延迟。

### 候选方案

1. 最终答案完成后包装成 SSE。
2. DeepSeek `stream=true`，解析上游 SSE，再生成项目自己的下游 SSE。
3. 立即重写为 libcurl multi + epoll。

### 决策

选择方案 2；第一版仍由业务 worker 执行阻塞式流式 `curl_easy_perform()`。

### 原因

方案 2 能先验证协议、状态机、背压和取消，同时复用现有线程池。直接进入 libcurl multi
会把 Provider 事件循环、Socket/TLS 和 Agent 状态机同时复杂化，不利于学习和测试。

### 当前代价

真流式改善 TTFT，但一个长流仍占用一个业务 worker，因此不自动提高并发吞吐。

---

## ADR-013：Provider SSE 与客户端 SSE 分层转换，不透明转发

### 问题

直接转发 DeepSeek SSE 会让客户端依赖 Provider 私有 JSON，并且无法统一表达 Tool、Run、
Metrics 和错误生命周期。

### 决策

```text
DeepSeek SSE -> DeepSeekSseParser -> AgentEvent -> HttpStreamResponder -> Client SSE
```

### 原因

客户端只依赖本项目事件协议；未来替换模型供应商时，前端不必改变。

### 复审修复

- 明确 libcurl 已处理 TCP/TLS/HTTP framing，Parser 只处理 Response Body 中的 SSE。
- SSE 单 Event 所有 `data:` 行累计限制 1 MiB，不能用大量短行绕过单行上限。
- `[DONE]` 改为严格终态，之后数据直接判协议错误。
- Tool Call 的 ID、Name、Arguments 按 index 聚合分片后再执行。
- 所有 libcurl C ABI 回调捕获 C++ 异常，不能让异常越过 C 调用栈。

---

## ADR-014：SSE 使用独立 HttpStreamResponder，不复用固定长度 HttpResponse

### 问题

普通 `HttpResponse` 假设完整 Body 已知并生成 `Content-Length`；SSE 长度未知，需要持续
发送和显式终止。

### 决策

新增 Streaming 状态机：

```text
Pending -> Open -> Finished
Pending/Open -> Disconnected
```

Header 使用 `Transfer-Encoding: chunked`，最终发送 `0\r\n\r\n`。

### 为什么不在 HttpResponse 加大量开关

固定响应和 Streaming 的提交时机、错误语义、关闭方式差异很大。独立 Responder 能让
业务层不能误设 Content-Length/Chunk，也让 Header 提交前后错误边界更清楚。

### 复审修复

- Header、Event、Terminal 全部通过连接 EventLoop FIFO 排队。
- Terminal 和 Force Close 放在同一个 Task，避免关闭越过最后 Chunk。
- `run.completed` 不重复携带可能很大的完整 Answer，只给出 `answer_sequence/bytes`。
- 不宣称网络 exactly-once，只保证服务器侧最多提交一次并保持顺序。

---

## ADR-015：慢客户端采用有界缓存后取消，不阻塞 EventLoop

### 问题

Provider 生成速度可能高于客户端读取速度，无界输出会持续增长内存。

### 候选方案

1. 无界缓存。
2. 在 EventLoop 中阻塞等待客户端。
3. 统计真实待发送字节，超过高水位后取消该 Run。
4. 实现 low-water 暂停/恢复 Provider。

### 决策

第一版选择方案 3：

```text
pending output <= 1 MiB
single Run payload <= 4 MiB
single downstream data <= 64 KiB
```

### 原因

方案 1 不安全；方案 2 会阻塞整个 IO Loop；方案 4 更完整但需要 Provider 可暂停机制和
low-water 回调。第一版先选择明确、可测试的取消策略。

### 复审修复

只看 `outputBuffer` 会漏掉尚在跨线程 EventLoop Task 中的数据，因此新增原子
`pendingOutputBytes`，在接受 send 时记账，在实际 write/关闭时结算。

---

## ADR-016：区分 TCP FIN 与 RST/HUP

### 问题

客户端 `shutdown(SHUT_WR)` 发送 FIN，只表示不再写请求，仍可能继续读取 SSE。如果收到
FIN 就完整关闭，合法半关闭客户端会丢失响应。

### 决策

Streaming 连接收到 FIN 后关闭读取方向、保留写方向；RST、HUP、非暂时 Read/Write Error
才完整关闭并触发 Agent Cancel。

### 复审修复

早期 Read Error 只记录 `handleError()` 而没有 `handleClose()`，可能让断开 Run 占满业务
worker；后改为错误路径幂等关闭。测试用 `SO_LINGER(1,0)` 制造 RST 验证取消和 inFlight
释放。

---

## ADR-017：使用 sequence 区分过程文本和最终答案文本

### 问题

模型在 Tool Call 轮次可能先输出“正在查询天气”，随后最终模型再输出答案。如果客户端
拼接所有 `assistant.delta`，过程文本会污染最终答案。

### 候选方案

1. 丢弃 Tool Call 轮次的所有 content。
2. 保留所有 delta，并给每次模型调用增加 sequence。

### 决策

选择方案 2。`assistant.delta` 带模型调用序号，`run.completed.answer_sequence` 指明最终
答案对应序号。

### 原因

过程文本对 UI 有价值，不应静默丢弃；序号让客户端可以分别展示过程和最终回答。

### 验证

SSE 集成测试让 Tool Call 轮次主动输出说明文本，并断言最终答案只拼接最终 sequence。

---

## ADR-018：上下文先使用 SQLite，不引入 Redis/MySQL

### 问题

内存 `vector<ChatMessage>` 重启丢失、常驻内存，并且消息数/字节数不能代表模型 Token。

### 候选方案

1. 继续只用内存。
2. SQLite。
3. Redis/MySQL/PostgreSQL。

### 决策

选择 SQLite + `ConversationStore` 抽象。

### 原因

当前是单机单进程项目。SQLite 提供事务、索引、WAL 和崩溃恢复，不需要额外服务；Store
接口又保留未来替换空间。直接部署外部数据库会增加运维和连接池，但不能自动改善上下文。

### 当前边界

只支持单服务进程。多进程需要数据库级 Lease/Version，公网多用户还需要 owner_id 和
鉴权。

---

## ADR-019：按完整 Turn 保存和裁剪，而不是按单条 Message

### 问题

按单条消息裁剪可能留下 Assistant 或 Tool Result，却删除对应 User/Tool Call，破坏协议
和语义完整性。

### 决策

一个成功 Run 保存为一个 Turn：

```text
User + Final Assistant + Tool Execution Trace
```

ContextBuilder 从新到旧按完整 Turn 选择，放不下就停止。

### 取舍

长期上下文目前只重建 User/Final Assistant，不重放旧 Provider Tool Messages，因为不能
伪造过去的 `tool_call_id` 协议状态；工具轨迹单独保存供审计和未来演进。

---

## ADR-020：SQLite 事务只包保存，不包模型调用

### 问题

需要保证 Turn 和 Sequence 原子，同时不能在几十秒模型调用期间持有数据库写锁。

### 决策

```text
Session::inFlight：保护 load -> model/tool -> save 的单进程业务顺序
BEGIN IMMEDIATE：只保护 saveTurn 的 sequence + insert + update
```

### 复审修复

- 用重复 `turn_id` 触发 UNIQUE 错误，验证事务回滚后 sequence 不跳号。
- SQLite Query 返回 BUSY/CORRUPT 时不能静默当空历史，必须抛错。
- 保存失败时保留已完成的模型、工具和 Token metrics。

---

## ADR-021：先做确定性摘要 Baseline，不额外调用模型摘要

### 问题

历史无限增加会扩大 Token 成本，但立即增加模型摘要会让每次压缩多一次网络、费用和
不确定性。

### 候选方案

1. 不摘要，只保留最近 N Turn。
2. 确定性摘录旧 Turn。
3. 模型生成结构化摘要。

### 决策

第一版选择方案 2：保留最近 8 个完整 Turn，较早 Turn 生成带 coverage 的有界摘录。

### 原因

确定性方案可测试、无额外 Token 成本，并建立 Summary Version/Coverage 数据模型。它是
Baseline，不宣称高保真。

### 复审修复

- 摘要不能使用 System Role，避免把用户历史提升权限。
- 摘要达到上限时优先保留较新的尾部，并保证 UTF-8 边界。
- 摘要包装前缀和消息结构也要占预算，不能先把内容填满再发现整个摘要放不下。
- 不信任数据库缓存 `estimated_tokens`，构造上下文时根据真实文本重新估算。
- 加载全部未摘要 Turn，避免只取最新 64 条后错误推进 coverage、永久跳过更早历史。

### 当前边界

确定性摘录可能遗漏长消息后半段。只有 Evaluation 证明需要时，再升级模型结构化摘要。

---

## ADR-022：HTTP Session 持久化，TCP Session 连接结束后删除

### 问题

HTTP 请求天然跨连接，需要客户端 Session ID；TCP Agent 的 Session 与一条长连接绑定，
断开后客户端无法继续使用旧连接名。

### 决策

```text
HTTP：http:<client-session-id>，跨重启保留
TCP：tcp:<startup-micros>:<pid>:<connection-name>，断开后异步删除
```

### 复审修复

TCP 连接序号重启后从 `#1` 开始。若异常退出留下 `tcp:#1`，新用户可能读到旧历史，因此
加入启动时间和 PID 的进程命名空间。TCP `/clear` 的校验也同步允许连接名中的 `#`。

### 当前边界

断开时业务队列已满可能遗留孤儿 TCP 数据，但新进程命名空间保证不会被后续用户加载。

---

## ADR-023：数据库持久化不等于 Memory 或 RAG

### 问题

容易把“历史放进 SQLite”描述为长期记忆，把摘要描述为 RAG。

### 决策

文档和代码明确区分：

```text
History/Store：完整成功 Turn
Context：本次送给模型的历史子集
Summary：历史压缩派生数据
Memory：长期提取事实，未实现
RAG：按 Query 检索外部知识，未实现
```

### 原因

准确命名可以避免简历和面试中过度声称，也能指导后续模块边界：RAG 应成为知识检索工具，
而不是与 ConversationStore 混成一张表和一段 Prompt。

---

## ADR-024：保留明确的安全与部署边界

### 当前明确不声称

```text
生产级公网多用户服务
多进程共享 Session
物理安全删除
完整 OpenTelemetry/Prometheus
高并发非阻塞 Provider Client
长期事实 Memory
RAG
写工具 Exactly Once
```

### 原因

学习项目最重要的不是声称功能最多，而是能准确说明：

```text
当前做了什么
为什么这样取舍
测试证明了什么
剩余风险在哪里
什么条件下继续演进
```

---

## ADR-025：开启 Thinking，但 reasoning 仅在当前 Run 暂存

### 问题

希望利用 DeepSeek Thinking 提高复杂问题和工具选择质量，但不希望保存、展示或泄露具体
思维链。

### 候选方案

1. 继续关闭 Thinking。
2. 开启并把 reasoning 返回前端、写日志和 SQLite。
3. 开启；每条 Assistant Tool Message 在当前 Run 内暂存 reasoning，Run 结束释放；SSE
   只显示状态。

### 决策

选择方案 3，并保留 `deepseek_thinking_enabled` 配置开关，默认开启。

### 原因

方案 1 放弃模型能力；方案 2 扩大隐私、存储和 Provider 耦合。方案 3 满足 DeepSeek
Tool Calls 协议，又把 reasoning 限制在最短必要生命周期。

### 关键实现

```text
AgentModelResponse.reasoningContent
-> makeAssistantToolCallMessage(reasoning + tool_calls)
-> 当前 runInternal messages
-> 后续 Provider Request
-> Run 结束自动释放
```

多步 Run 中 R1、R2 分别保存在各自 Assistant Message，不能使用会覆盖的 lastReasoning。

### 不允许进入

```text
AgentRunResult
SQLite ConversationStore
HTTP JSON
SSE reasoning text
agent_trace / 普通日志
Tool Result
```

### SSE 取舍

只发送：

```text
assistant.thinking.started
assistant.thinking.completed
```

客户端知道状态，但看不到内容。

### 复审与测试

- 非流式 Tool Call 验证 R1 原样进入下一请求。
- 多步调用验证 R1/R2 不互相覆盖。
- SSE Reasoning 跨事件分片并聚合。
- 哨兵验证 reasoning 不进入客户端、日志和 SQLite。
- Reasoning 中途 RST 验证取消和 inFlight 释放。
- Enabled/Disabled 两种 Request 均有测试。

### 跨 Turn 取舍

DeepSeek 官方要求在完整重放旧 Tool Calling 消息链时继续携带对应 reasoning。本项目跨
Turn 只重建 user/final assistant，不重放旧 tool_calls/tool results，所以新 Run 不会提交
缺 reasoning 的旧工具消息；代价是放弃旧工具子轮的隐藏推理连续性，而不是完整 Provider
Transcript 恢复。

### 当前边界

没有暴露 `reasoning_effort`，使用 Provider 默认 high；单次 Model Call Reasoning 上限
1 MiB；非流式客户端断开仍不能及时取消。

---

---

## ADR-026：使用 Python 标准库实现薄 HTTP/SSE CLI

### 问题

`nc` 可以自然语言聊天，但只走原始 TCP；`curl` 能走 HTTP/SSE，却需要用户手写 JSON 并
阅读原始事件。需要一个适合 Windows/WSL 日常使用的交互终端。

### 候选方案

1. 继续让用户手写 curl。
2. 在 C++ Server 中加入终端 UI。
3. 使用 Python `requests/httpx`。
4. 使用 Python 标准库实现薄客户端。
5. 直接开发浏览器前端。

### 决策

选择方案 4：`tools/chat_client.py`。

### 原因

```text
无需 pip install
Windows Python 和 WSL Python 都可运行
不污染 C++ Server 的协议和业务职责
便于学习 UTF-8/SSE 增量解析
开发和测试成本低于浏览器前端
```

### 职责边界

CLI 只负责：

```text
终端输入输出
Session ID 文件
HTTP Request
SSE Parser
Thinking/Tool/Delta 状态渲染
```

Tool Calling、Thinking、SQLite Context、预算、可观测性和 Provider 调用仍全部在 C++
Server。Python 不是第二套 Agent Runtime。

### Session 取舍

随机 HTTP Session ID 默认保存到用户目录的小文本文件。关闭 CLI 后重新打开可以继续
SQLite 会话；文件不保存聊天正文、API Key 或 reasoning。

### 当前边界

没有复杂 TUI、Markdown/颜色、历史补全、认证 Token 和断线重放。先解决“命令行自然
语言完整使用 HTTP/SSE”这一实际问题，再根据使用反馈决定是否做网页前端。

### 验证

`chat_client_test` 使用随机端口 Mock Server，覆盖 HTTP Chunked、任意分片 SSE、中文、
Thinking/Tool/Delta、Session 文件、Health、Clear 和错误终态。

---

## ADR-027：聊天框由 Session 标识，不由网络连接标识

### 问题

底层支持多连接，但用户需要关闭界面后重新选择历史聊天。如果把聊天框等同于 TCP
Connection，连接关闭后就失去身份，无法恢复。

### 决策

```text
聊天框 = server-generated session_id + SQLite history
一次消息 = 一次 HTTP/SSE Connection + Agent Run
```

Server 增加 `POST/GET /agent/sessions`；CLI 启动时提供“新建聊天、选择历史、继续上次”。

### 隔离语义

不同聊天框使用不同 Session ID，Context、Summary、inFlight 和 Turn 相互隔离；多个聊天
框同时发送时，网络层维护多条并发连接，业务层跨 Session 并行、同 Session 串行。

### 标题取舍

第一版不额外调用模型生成标题，而是在首次成功 Turn 后截取第一条消息前 36 个字符。
这样零额外 Token、确定且可测试。后续如果需要更自然标题，再加入异步模型重命名。

### 当前边界

本地单用户模式，列表返回全部 HTTP Session；没有 owner_id、认证、重命名、分页和指定
聊天框删除。多用户上线前必须先按 owner_id 过滤。

---

## ADR-028：Windows 启动器分离 Server 与 CLI 生命周期

### 问题

手工启动需要记住 WSL 路径、CMake 命令和 Python CLI 命令；如果把 Server 与 CLI 放在
同一个前台进程中，退出聊天界面也会误停后端。

### 决策

项目根目录提供：

```text
start_agent.bat
stop_agent.bat
```

启动器在独立窗口运行 C++ Server，等待 Health 后在当前窗口打开 CLI；CLI 退出后 Server
继续运行。停止器按 `/proc/<pid>/exe` 的规范化绝对路径匹配本项目 `bin/main`。

### 原因

```text
双击即可使用
Server/CLI 生命周期清晰
移动仓库后通过 wslpath 自动适配
不使用危险的 pkill main
构建错误保留在独立窗口供初学者查看
```

### 当前边界

依赖 WSL、Windows PowerShell 和 Python 3；没有注册 Windows Service 或开机自启。
Health 会校验固定 `service=cpp-webserver-agent` 身份，但没有版本/构建哈希，因此可能复用
同项目的旧版本实例。

停止脚本排除 `--qps` 实例并在每次发信号前重新校验 `/proc/<pid>/exe`，但 C++ 主程序尚未
实现基于 `EventLoop::quit()` 的优雅信号停机；当前 SIGINT/SIGTERM 是直接进程终止。

### 验证

`--check` 验证 Windows/WSL 路径与依赖；真实生命周期测试验证 Runner 启动、Health 200、
精确停止项目 PID 和端口释放。

## 3. 典型问题修复索引

| 问题 | 根因 | 修复 | 测试证据 |
|---|---|---|---|
| Planner JSON 不稳定 | 用 Prompt 模拟协议 | 原生 Tool Calls | Runtime Fake 测试 |
| Tool ID 重复 | 缺少 Run 级身份校验 | 整个 Run 拒绝复用 ID | Duplicate ID 单测 |
| 工具执行后无模型预算 | 预算检查太晚 | 工具前预留下一次 Model Call | Budget 单测 |
| Thinking Tool Calls 协议不完整 | 未回传 reasoning | 先关闭，现按 Assistant Message Run 内回传 | Thinking/Provider 测试 |
| 429/401 全变 502 | 错误未分类 | 稳定内部类别和 HTTP 映射 | Error Classification 单测 |
| curl 错误泄露 | 直接使用底层错误文本 | 对外稳定文案 | HTTP Mock 泄露测试 |
| 日志异常影响 Completion | 旁路异常未隔离 | JSON/Logger sink catch | 集成与复审 |
| 伪 SSE | 完整答案后才发送 | Provider 真流式解析 | TTFT 集成测试 |
| Tool 过程文本污染答案 | 所有 delta 混拼 | Model Sequence | SSE sequence 测试 |
| 大答案没有 completed | 终态重复完整 Answer | 终态只发 bytes/sequence | 70 KiB 测试 |
| Provider Event 可无限积累 | 只限制单行 | Event 总 data 1 MiB | Oversized Event 单测 |
| `[DONE]` 后仍接受数据 | 终态不严格 | DONE 后拒绝任何行 | Parser 单测 |
| 慢客户端无界内存 | 只看 outputBuffer | pendingOutputBytes + 高水位取消 | SSE 集成/边界测试 |
| RST 不取消 Run | Read Error 只记日志 | 幂等 handleClose | SO_LINGER RST 测试 |
| C ABI 异常越界 | 回调内 STL 可抛异常 | 所有 curl 回调 catch | 严格复审与回归 |
| 内存历史重启丢失 | Session 保存 vector | SQLite ConversationStore | 三次进程重启测试 |
| Turn 保存一半 | 非原子多步写入风险 | BEGIN IMMEDIATE 事务 | UNIQUE 回滚测试 |
| 摘要提升用户权限 | 摘要误用 System Role | Assistant + untrusted 标记 | Context 单测/复审 |
| 64 Turn 被摘要跳过 | 只加载最新 64 条 | 加载全部未摘要 Turn | Store/复审验证 |
| TCP 重启复用旧历史 | 连接名序号重置 | 启动时间 + PID namespace | TCP Clear/重启测试 |
| 测试误连旧服务 | 固定端口并行竞争 | RUN_SERIAL + 子进程存活检查 | 固定端口集成测试串行通过 |
| 命令行只能方便地使用 TCP | HTTP/SSE 需要手写 JSON | 零依赖交互式 CLI | chat_client_test |
| 关闭界面后难找回聊天 | 只有最后一个本地 Session 指针 | Server Session 目录 + CLI 菜单 | Store/CLI/重启测试 |
| Windows 启动步骤过多 | 需手工记 WSL/CMake/Python 命令 | 双击启动/精确停止脚本 | Health 生命周期回归 |

## 4. 推荐复盘方法

阅读一条决策时，尝试自己回答：

1. 如果不修，这个问题会表现成什么用户可见现象？
2. 它属于协议、并发、生命周期、安全、资源还是可观测性问题？
3. 修复应该放在哪一层，为什么不放在其他层？
4. 修复是否引入新的成本？
5. 测试验证的是功能存在，还是验证了错误边界？
6. 当前约束改变后，这条决策还成立吗？

例如“慢客户端背压”不是简单地加一个大小判断：

```text
只看 outputBuffer 为什么不够？
跨线程待执行 send 在哪里？
为什么不能阻塞 EventLoop？
为什么第一版选择取消，而不是暂停/恢复？
什么时候值得升级 low-water backpressure？
```

能够沿着这种问题链解释设计，才是真正理解工程取舍，而不是只记住最终代码。
