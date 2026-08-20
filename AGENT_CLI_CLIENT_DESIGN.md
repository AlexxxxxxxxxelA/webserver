# HTTP/SSE 交互式 CLI 客户端

## 1. 目标

原有命令行使用方式有两个极端：

```text
nc：自然语言输入简单，但只使用原始 TCP Agent
curl：能使用 HTTP/SSE，但每次需要手写 JSON 并阅读原始事件
```

新增：

```text
tools/chat_client.py
```

使用者只输入自然语言，客户端自动完成：

```text
Session ID
JSON Request
POST /agent/run/stream
HTTP Chunked 解码
SSE 增量解析
Thinking/Tool 状态渲染
Assistant Delta 输出
Run Completed/Metrics
/agent/clear
```

Agent 能力仍全部位于 C++ Server。Python 只是薄终端 UI，不执行 Tool Calling、不保存
Conversation Turn，也不接触 DeepSeek API Key。

## 2. Windows 使用

### 双击启动

项目根目录：

```text
start_agent.bat
stop_agent.bat
```

`start_agent.bat` 会打开两个窗口：

```text
C++ Agent Server：CMake 构建日志和 Server 日志，持续运行
启动脚本窗口：HTTP/SSE CLI，显示聊天框菜单
```

CLI 退出不会自动停止 Server。这样再次双击启动时，脚本检测到 `/health` 已可用，会复用
具有 `service=cpp-webserver-agent` 标识的现有 Server，只重新打开 CLI。需要释放端口时
双击 `stop_agent.bat`。

脚本内部文件：

```text
tools/run_server.cmd  Windows 独立 Server 窗口包装
tools/run_server.sh   WSL 配置、构建和 exec bin/main
tools/stop_server.sh  按 /proc/<pid>/exe 精确匹配本项目 Server
```

可执行无副作用检查：

```powershell
cmd /c start_agent.bat --check
cmd /c stop_agent.bat --check
```

脚本会自动根据自身位置计算项目根目录，不依赖固定 `F:` 路径；移动整个仓库后仍可使用，
前提是该路径可通过 WSL `wslpath` 访问。

### 启动服务器

PowerShell 窗口一：

```powershell
wsl.exe bash -lc "cd '/mnt/f/webserver/webserver' && ./bin/main"
```

### 启动 CLI

PowerShell 窗口二：

```powershell
python "F:\webserver\webserver\tools\chat_client.py"
```

如果 Windows 没有 `python` 命令，也可以使用 WSL Python：

```powershell
wsl.exe python3 "/mnt/f/webserver/webserver/tools/chat_client.py"
```

启动后：

```text
C++ Agent HTTP/SSE CLI
Server:  http://127.0.0.1:18081

请选择聊天框：
  [0] 新建聊天

历史聊天（最近更新优先）：
  [1] C++ Reactor 学习 [上次使用]
      ID=chat-...  Turn=5  更新=2026-08-19 20:30

选择编号（直接回车继续上次 [1]）:

> 北京天气怎么样？
```

## 3. 输出示例

```text
[Run] run-...
[思考中 #1]
[思考完成 #1]
[调用工具] weather
[工具成功] weather (320 ms)
[思考中 #2]
[思考完成 #2]
[模型输出 #2]
北京天气晴朗。
[完成] 最终答案序号 #2
```

CLI 不显示具体 `reasoning_content`，只显示思考状态；这是服务器协议的安全边界。

## 4. 命令

| 命令 | 作用 |
|---|---|
| `/help` | 显示帮助 |
| `/health` | 请求 `GET /health` |
| `/session` | 显示当前 HTTP Session ID |
| `/list` | 重新打开历史聊天选择菜单 |
| `/use <id>` | 切换到指定 Session |
| `/new` | 创建并保存新的 Session ID |
| `/clear` | 调用 `POST /agent/clear` 删除当前历史 |
| `/metrics` | 切换完成后的 Metrics 显示 |
| `/quit` | 退出 |

其他非空输入都作为自然语言发送。

## 5. Session 持久化

默认 Session 文件：

```text
Windows：%USERPROFILE%\.webserver_agent_cli_session
WSL/Linux：~/.webserver_agent_cli_session
```

文件只保存随机 Session ID，不保存聊天内容、API Key 或 reasoning。聊天历史由 C++ Server
的 SQLite ConversationStore 保存。

Session ID 仍是一种本地会话能力标识：知道它的人可以继续或清除对应会话。WSL/Linux
文件使用 `0600` 权限；Windows 使用当前用户目录继承的 ACL。写入使用同目录随机临时
文件、flush/fsync 和 `os.replace`，避免固定 `.tmp` 文件的并发覆盖。多个 CLI 同时修改
同一个 Session 文件仍采用 last-writer-wins，不提供跨进程锁。

因此：

```text
关闭 CLI
-> 下次重新运行
-> 读取相同 Session ID
-> 继续 HTTP 持久化会话
```

历史聊天列表的权威数据在 Server SQLite，而不是 Session 文件。Session 文件只保存“上次
使用哪个聊天框”，用于菜单默认选项。Server 提供：

```text
POST /agent/sessions  新建聊天框并生成随机 Session ID
GET  /agent/sessions  按最后更新时间列出最近 50 个聊天框
```

聊天标题默认是“新聊天”，第一次成功发送消息后使用该消息前 36 个字符更新标题，不额外
调用模型。列表同时显示 Turn 数和最后更新时间。

新会话：

```powershell
python tools\chat_client.py --new-session
```

指定 Session：

```powershell
python tools\chat_client.py --session my-demo
```

不保存 Session ID：

```powershell
python tools\chat_client.py --no-save-session --new-session
```

## 6. 一次性模式

适合脚本或快速验证：

```powershell
python tools\chat_client.py --once "北京天气怎么样？" --show-metrics
```

仍使用 HTTP/SSE，不是普通 `/agent/run`。

请求失败时进程返回非零状态码。

## 7. 自定义服务器

```powershell
python tools\chat_client.py --url http://127.0.0.1:18081
```

支持 HTTP 和 HTTPS URL。当前本地 C++ Server 是 HTTP；若未来通过反向代理提供 HTTPS，
CLI 可直接连接代理地址。

请求 timeout 默认 75 秒，略大于 Server 60 秒 Run Deadline：

```powershell
python tools\chat_client.py --timeout 75
```

## 8. SSE Parser

`http.client` 已处理 HTTP Chunked framing，但 Body 分片仍可能切在：

```text
UTF-8 中文字符中间
SSE field 中间
JSON 文本中间
一个或多个 SSE Event 之间
```

客户端使用：

```text
增量 UTF-8 Decoder
未完成 Event Buffer
空行 Event 边界
data 多行合并
JSON Parser
```

不能把一次 `response.read1()` 当成一个 SSE Event。

客户端未完成 SSE Buffer 上限为 1 MiB，避免异常服务器一直不发送 Event 空行而持续占用
内存。

## 9. 模型输出序号

Tool Call 轮次和最终答案轮次都可能产生 `assistant.delta`：

```text
sequence=1：正在查询天气
sequence=2：北京天气晴朗
```

CLI 会按序号显示每段模型输出。`run.completed.answer_sequence` 指明最终答案序号，不会把
所有序号静默拼成一个答案。

## 10. 取消

在流式请求中按 `Ctrl+C`：

```text
CLI 对当前 Socket 设置 `SO_LINGER(1,0)` 后关闭，主动发送 RST
-> C++ Server 感知断开
-> CancelCheck
-> 中止 DeepSeek/Weather
-> 释放 Session inFlight
```

如果设置 Linger 失败，会退化为普通关闭；实际取消速度仍取决于操作系统和 Provider I/O
callback 的执行频率。

## 11. 为什么使用 Python 标准库

候选方案：

1. `requests/httpx`。
2. 浏览器前端。
3. Python 标准库 `http.client`。

当前选择方案 3：

```text
无需 pip install
Windows Python 和 WSL Python 都可运行
可以直接观察 HTTP/SSE 解析过程
与 C++ Server 构建依赖完全解耦
```

它不是用 Python 重写 Agent；只承担终端输入输出和协议客户端职责。

## 12. 错误处理

### Header 提交前错误

Server 返回普通 JSON 4xx/5xx，CLI 解析 `error/code` 后显示。

### Header 提交后错误

Server 返回：

```text
event: error
data: {"code":"...","message":"..."}
```

CLI 显示一次错误，并把本次请求视为失败。

### 非完整 Stream

没有 `run.completed` 或 `error` 就 EOF，会报告协议错误，不会误当成功。

第一个终态不可逆：终态之后再出现 Delta、第二个 Error 或 Completed 会判为协议错误，
不能用后来的 Completed 覆盖已经发生的 Error。

## 13. 自动化测试

新增：

```text
tests/ChatClientTest.py
CTest: chat_client_test
```

使用随机本地端口的 Mock HTTP/SSE Server，覆盖：

```text
逐字节 UTF-8/SSE Parser
HTTP Chunked 响应
Thinking/Tool/Delta/Completed
中文回答拼接
Metrics 渲染
Session 文件恢复和轮换
/health
/agent/clear
SSE error 终态
```

`agent_sse_integration_test` 还会启动真实 C++ Server 和 Mock DeepSeek/Weather，再以
`chat_client.py --once` 子进程完成一次端到端聊天，验证 Thinking、Weather、中文 Delta、
Metrics 和 reasoning 不泄露。

启动器还执行过真实生命周期回归：Runner 启动 Server、Health 200、Stop Runner 精确
校验 `service=cpp-webserver-agent`、终止本项目 Agent PID，并确认不会误停同一 `bin/main`
的 `--qps` 实例；测试最后清理 QPS，18080/18081/18082/19090 无残留监听。

当前 C++ `main` 尚未把 SIGINT/SIGTERM 转成 `EventLoop::quit()`；停止脚本使用信号直接
终止进程，SQLite WAL 能恢复已提交事务，但不属于优雅停机。完整 Graceful Shutdown 是
后续独立任务，不在双击启动器范围内。

## 14. 当前边界

```text
没有命令历史和 readline 自动补全
没有 Markdown/颜色渲染
没有多行编辑器
没有重连并恢复正在运行的 SSE
没有身份认证 Token 参数
没有上传文件
没有重命名和按 ID 删除其他聊天框（当前只有进入后 /clear）
最多列出最近 50 个 HTTP 聊天框
```

当前目标是让命令行自然语言聊天完整使用 C++ HTTP/SSE Agent，而不是构建复杂 TUI。

当前是本地单用户模式：`GET /agent/sessions` 会列出数据库中的全部 HTTP 聊天框。随机
Session ID 不是鉴权凭据；如果未来开放到公网，必须先增加用户认证、`owner_id`，并让
create/list/run/clear 全部按 Owner 过滤。

## 15. 推荐阅读顺序

```text
1. tools/chat_client.py::SseParser
2. AgentHttpClient::stream_run
3. TerminalRenderer::handle
4. load_or_create_session/save_session
5. main 的交互命令循环
6. tests/ChatClientTest.py
7. src/main.cc::onStreamingHttpRequest
8. src/main.cc 的 /agent/sessions 路由
```
