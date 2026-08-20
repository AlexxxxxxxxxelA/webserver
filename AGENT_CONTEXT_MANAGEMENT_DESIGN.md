# Agent 上下文持久化与预算管理设计

本阶段的存储选型、事务/摘要取舍和复审修复集中记录在
`AGENT_ENGINEERING_DECISIONS.md` 的 ADR-018 至 ADR-023。

## 1. 本阶段解决的问题

旧实现：

```text
Session -> vector<ChatMessage> -> 最多 120 条 / 128 KiB -> 重启丢失
```

新实现：

```text
内存 Session -> 只保存 inFlight、deleteWhenIdle、lastAccess
SQLite ConversationStore -> 保存成功 Turn、工具轨迹和摘要
ContextBuilder -> 按历史 Token Budget 选择摘要和最近完整 Turn
```

数据库持久化解决重启、删除和内存常驻；ContextBuilder 才负责本次向模型发送哪些历史。
把 vector 原样放入数据库不等于上下文优化。

## 2. 依赖

```bash
sudo apt-get install libsqlite3-dev
```

本机实际下载约 911 KiB、安装后约 3.4 MiB，WSL 位于 `D:\WSL\Ubuntu`。SQLite 是
进程内库，不会启动额外数据库服务。

CMake：

```cmake
find_package(SQLite3 REQUIRED)
target_link_libraries(webserver_core PUBLIC SQLite::SQLite3)
```

## 3. 模块职责

位置：

```text
include/ConversationContext.h
src/ConversationContext.cc
```

### ConversationStore

定义 `load`、`saveTurn`、`saveSummary` 和 `deleteSession`。AgentDemoService 不直接依赖
`sqlite3*`，后续替换其他存储实现不需要修改 AgentRuntime。

### SQLiteConversationStore

负责 Schema、WAL、外键、busy timeout、事务、按 Session 加载和级联删除。

### TokenEstimator

第一版确定性估算：

```text
ASCII：约 4 字符一个 Token
非 ASCII：每个 UTF-8 code point 约一个 Token
每条消息增加 role/结构开销
```

它不是 DeepSeek 精确 tokenizer。已有 `usage.prompt_tokens` 可用于后续校准。

### ContextBuilder

```text
历史 Token Budget：8000
摘要内容预算：1200
最近原文窗口：8 个完整 Turn
```

## 4. 数据模型

### sessions

```text
session_id, title, created_at, updated_at, next_sequence, summary_version
```

旧数据库启动时会检查 `PRAGMA table_info(sessions)`，缺少 title 时自动执行兼容迁移。聊天
目录按 updated_at 排序，并通过 LEFT JOIN turns 统计 Turn 数。

### turns

一个成功 Turn 一行：

```text
session_id, sequence, turn_id/run_id
user_message, assistant_message
tool_executions_json, estimated_tokens, created_at
```

长期模型历史目前只重建 user 和 final assistant；工具轨迹持久化用于审计和后续演进，
不会伪造历史 Provider `tool_call_id`。

### conversation_summaries

```text
session_id, version, covered_until_sequence, content, created_at
```

摘要是派生数据，原始 Turn 不会删除，因此可以重新生成、调试和评测。

## 5. 完整 Turn 与事务

当前 Turn：

```text
User -> Tool Calls/Results -> Final Assistant
```

只有成功形成最终答案才保存。`saveTurn()`：

```text
BEGIN IMMEDIATE
-> 创建 Session
-> 读取 next_sequence
-> 插入完整 Turn
-> next_sequence + 1
-> COMMIT
```

任何步骤失败都会 ROLLBACK。测试以重复 `turn_id` 触发 UNIQUE 约束，确认失败 Turn 不存在、
sequence 不跳号。

## 6. SQLite 并发和线程边界

```text
PRAGMA journal_mode=WAL
PRAGMA foreign_keys=ON
sqlite3_busy_timeout=3000
SQLITE_OPEN_FULLMUTEX
```

每个 Store 操作使用独立 Connection，不跨 worker 共享 handle 或 statement。数据库操作
允许阻塞，但只能运行在 BoundedThreadPool worker，不能运行在 EventLoop。`/agent/clear`
同样是异步业务任务。

## 7. 内存 Session 为什么仍保留

内存 Session 保存：

```text
inFlight, lastAccess, deleteWhenIdle
```

同 Session 的 `load -> model/tool -> save` 必须完整串行，否则两个请求会读取相同旧历史，
再按完成速度乱序写入。当前只支持一个服务进程访问数据库；多进程需要 DB lease/version。

## 8. 上下文构造

模型历史：

```text
Earlier conversation excerpts（可选）
-> 最近完整 Turn
```

Runtime 再加入 System Prompt 和当前 User Message。ContextBuilder 从最近 Turn 向前选择；
完整 Turn 放不下就停止，不会拆开 user/assistant。

摘要包含用户原文，因此使用 assistant 角色并标记 `untrusted reference data`，不能提升成
system 权限。

## 9. Token Budget 的边界

8000 只覆盖历史摘要和最近 Turn，不包含：

```text
Runtime System Prompt
当前 User Message
Tool Definitions
Provider JSON 固定开销
输出预算
```

所以 `context_estimated_tokens` 是历史成本预算，不是完整 Provider Context 的硬上限。
数据库缓存的 estimated_tokens 不作为安全依据，加载时会根据真实文本重新估算。

## 10. 确定性滚动摘要

未摘要 Turn 超过 8 个时：

```text
旧摘要 + 离开 recent window 的完整 Turn 摘录
-> 新摘要版本
-> 推进 covered_until_sequence
```

每个 Turn 摘录 User 前约 80 Token、Assistant 前约 120 Token。达到 1200 Token 上限时
优先保留较新的摘要尾部，并在 UTF-8 code point 边界截断。

这是低成本确定性 baseline，可能遗漏长消息后半部分；它不是高保真语义摘要。如果后续
Evaluation 证明不足，再增加模型生成的结构化摘要。

## 11. HTTP 与 TCP Session

HTTP：

```text
http:<client-session-id>
```

跨连接和重启持久化。清除：

聊天框管理：

```text
POST /agent/sessions -> 创建随机 chat-... Session
GET  /agent/sessions -> 列出最近 HTTP Session
```

第一次成功消息会将默认“新聊天”更新为消息前 36 个字符的本地标题，不消耗模型 Token。

清除：

```http
POST /agent/clear
Content-Type: application/json

{"session_id":"demo"}
```

TCP Session 只属于当前连接，断开后由 worker 异步删除。其 ID 包含进程级命名空间：

```text
tcp:<startup-microseconds>:<pid>:<connection-name>
```

因为连接序号重启后会从 `#1` 开始；异常退出遗留记录不能被下次启动的新用户误加载。
TCP `/clear` 与普通提交使用相同 ID 校验规则，允许连接名中的 `#`；HTTP Session ID 不
允许该字符。

## 12. 路径和配置

默认：

```text
conversation_database_path=data/conversations.db
```

从 `bin/` 启动时默认调整为 `../data/conversations.db`。systemd/容器应使用绝对路径：

```bash
export CONVERSATION_DATABASE_PATH=/var/lib/webserver/conversations.db
```

`.gitignore` 已排除 `data/*.db`、`*.db-wal` 和 `*.db-shm`。

## 13. 可观测性

HTTP metrics 和 `agent_trace` 新增：

```text
history_load_ms
history_save_ms
context_estimated_tokens
context_recent_turns
summary_used
```

`history_load_ms` 当前实际包括 SQLite load、可选摘要更新和 ContextBuilder，可理解为上下文
准备耗时。保存失败时会保留已有模型、工具和 Token metrics，再返回内部错误。

## 14. 自动化测试

`conversation_context_test`：

```text
SQLite 重开恢复、Session 隔离、工具轨迹
事务失败回滚、sequence 不跳号
摘要 version/coverage、级联删除
Token Budget、完整 Turn 配对、UTF-8 估算
```

`conversation_restart_integration_test` 使用同一临时 DB 启动服务器三次：

```text
第一次保存
第二次确认恢复并 /agent/clear
第三次确认历史不再恢复
```

## 15. 安全和保留边界

当前是本地单用户 Demo，没有登录和 owner_id。知道另一个 session_id 的调用者可以继续或
清除其会话；不能部署成公网多用户服务并声称已有权限隔离。

当前还没有：

```text
TTL、单 Session/总 DB 大小上限
secure_delete、VACUUM/WAL checkpoint 擦除策略
文件权限主动设置、用户导出/删除审计
```

普通 DELETE 是逻辑删除，不保证磁盘页面立即物理擦除。本地学习库不应保存真实敏感对话。

## 16. 当前未实现

```text
精确 DeepSeek tokenizer
模型结构化摘要
长期用户事实 Memory
多进程 Session lease
多用户鉴权和 owner_id
Run checkpoint
Redis/PostgreSQL Store
RAG
```

## 17. 推荐阅读顺序

```text
1. include/ConversationContext.h
2. tests/ConversationContextTest.cc
3. SQLiteConversationStore::saveTurn/load
4. TokenEstimator
5. ContextBuilder::build/extendSummary
6. AgentDemoService::runTurn
7. clearSessionAsync/deleteSessionWhenIdle
8. main.cc 的 /agent/clear
9. tests/ConversationRestartIntegrationTest.py
```

阅读时回答：

1. 为什么数据库持久化不等于上下文优化？
2. 为什么 SQLite 不能在 EventLoop 中操作？
3. 为什么 Turn 与 sequence 必须在同一事务？
4. 为什么裁剪不能拆开 user/assistant？
5. 为什么摘要不能用 system 角色？
6. 为什么 estimated tokens 不是精确上限？
7. 为什么 HTTP 持久化而 TCP 断开后删除？
8. 为什么 TCP 需要进程级命名空间？
9. 为什么原始 Turn 不随摘要删除？
10. 为什么当前只支持单服务进程？
