# C++ WebServer Agent

基于 C++11、epoll/Reactor、HTTP/1.1、DeepSeek Tool Calls、SSE、SQLite Context 和
Thinking 构建的学习项目。

## Windows 快速启动

### 双击启动（推荐）

在资源管理器打开：

```text
F:\webserver\webserver
```

双击：

```text
start_agent.bat
```

它会自动：

```text
检查 WSL、curl 和 Windows Python
-> 将 Windows 项目路径转换成 WSL 路径
-> 在独立窗口配置/构建/启动 C++ Server
-> 等待 /health 成功
-> 打开 HTTP/SSE CLI 聊天框选择菜单
```

脚本只会复用 `/health` 同时返回：

```json
{"service":"cpp-webserver-agent","status":"ok"}
```

的本项目服务，不会只因为 18081 上任意 HTTP 服务返回 2xx 就发送聊天内容。

退出 CLI 后，Server 窗口仍继续运行，方便下次快速打开。停止时双击：

```text
stop_agent.bat
```

停止脚本只匹配本项目 `bin/main` 的真实可执行路径，不会宽泛结束其他名为 `main` 的程序。

### 手工启动

第一个 PowerShell 窗口启动 C++ Server：

```powershell
wsl.exe bash -lc "cd '/mnt/f/webserver/webserver' && cmake --build build --parallel && ./bin/main"
```

第二个 PowerShell 窗口启动自然语言 HTTP/SSE CLI：

```powershell
python "F:\webserver\webserver\tools\chat_client.py"
```

然后直接输入：

```text
请选择聊天框：
  [0] 新建聊天
  [1] C++ Reactor 学习

选择编号: 0

> 北京天气怎么样？
```

CLI 会自动处理 JSON、HTTP Chunked、SSE、Session、Thinking/Tool 状态和增量回答。

常用命令：

```text
/help
/health
/session
/list
/new
/clear
/metrics
/quit
```

完整说明见：

- `AGENT_CLI_CLIENT_DESIGN.md`
- `AGENT_LEARNING_GUIDE.md`
- `LEARNING_ROADMAP.md`

## 测试

```powershell
wsl.exe bash -lc "cd /mnt/f/webserver/webserver && ctest --test-dir build --output-on-failure"
```
