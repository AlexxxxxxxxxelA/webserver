# mywebserver

`mywebserver` 是一个基于 C++17 实现的轻量级网络服务器项目。项目核心采用 Reactor 风格的事件驱动模型，封装了 `EventLoop`、`Channel`、`Poller`、`TcpServer`、`TcpConnection`、缓冲区、定时器和线程池等基础组件，可用于学习和实践 Linux 下的高并发 TCP 服务开发。

当前项目除了基础 TCP 网络库，还包含一个 `AgentDemoServer` 示例服务：客户端通过 TCP 长连接发送一行文本，服务端将请求交给 DeepSeek Chat API，并在需要时调用本地工具完成简单计算或时间查询，再把结果返回给客户端。

## 功能特性

- 基于 `epoll` 的事件循环，支持非阻塞 I/O。
- 封装 TCP 服务端、连接管理、读写缓冲区和回调机制。
- 支持多线程事件循环池，可通过多个 worker loop 分担连接处理。
- 内置定时器队列，支持按时间调度回调任务。
- 提供 Agent Demo 服务，演示网络服务与大模型 API 的结合。
- Agent Demo 支持简单会话上下文、健康检查、清空上下文和退出命令。
- 内置 `calculator` 与 `time` 两个本地工具，展示 Agent 工具调用流程。
- 提供纯 TCP ping/pong QPS 测试模式和 Python 压测脚本。

## 项目结构

```text
.
├── CMakeLists.txt
├── README.md
├── agent_demo.conf
├── include/mywebserver/
│   ├── EventLoop.h
│   ├── TcpServer.h
│   ├── TcpConnection.h
│   ├── Buffer.h
│   ├── TimerQueue.h
│   └── AgentDemo.h
├── src/
│   ├── EventLoop.cpp
│   ├── EPollPoller.cpp
│   ├── TcpServer.cpp
│   ├── TcpConnection.cpp
│   ├── AgentDemo.cpp
│   └── main.cpp
├── tests/
└── tools/
    └── bench_tcp.py
```

## 核心模块

### Reactor 网络层

`EventLoop` 是事件循环的核心，负责等待 I/O 事件、分发回调和执行跨线程投递的任务。`Channel` 封装文件描述符及其关心的读写事件，`Poller`/`EPollPoller` 负责和 Linux `epoll` 交互。

### TCP 服务封装

`TcpServer` 负责监听端口、接收新连接，并把连接分配给事件循环线程。`TcpConnection` 管理单个 TCP 连接的生命周期，提供连接回调、消息回调、写完成回调和关闭回调等接口。

### Buffer

`Buffer` 封装网络读写缓冲区，负责处理 TCP 字节流中的数据暂存、读取和追加，降低业务层直接操作裸内存的复杂度。

### 线程与定时器

`EventLoopThread` 和 `EventLoopThreadPool` 用于构建多线程 Reactor 模型。`Timer` 与 `TimerQueue` 提供定时任务能力，可扩展心跳、超时关闭、周期任务等逻辑。

### AgentDemo

`AgentDemoService` 是一个基于 TCP 的 Agent 示例。服务端收到用户输入后，会先请求 DeepSeek 判断是否需要工具调用。如果需要，会调用本地工具，例如表达式计算器或当前时间工具，再把工具结果交给模型生成最终回复。

支持的客户端命令：

```text
/health  检查服务状态
/clear   清空当前连接的会话上下文
/quit    关闭当前连接
/exit    关闭当前连接
```

## 构建

需要 CMake 3.16+ 和支持 C++17 的编译器。

```bash
cmake -S . -B build
cmake --build build
```

## 运行 Agent Demo

编辑 `agent_demo.conf`：

```text
port=18080
deepseek_api_key=你的 DeepSeek API Key
deepseek_api_url=https://api.deepseek.com/chat/completions
deepseek_model=deepseek-chat
```

启动服务：

```bash
./build/mywebserver
```

使用 `nc` 连接：

```bash
nc 127.0.0.1 18080
```

输入一行问题并回车，例如：

```text
现在几点？
计算 12 * (8 + 3)
/health
```

## 运行 QPS 测试服务

项目还提供一个纯 TCP ping/pong 模式，用于测试基础网络层吞吐能力。

启动服务：

```bash
./build/mywebserver --qps --port=18081 --threads=3
```

运行压测脚本：

```bash
python3 tools/bench_tcp.py --host 127.0.0.1 --port 18081 --connections 100 --requests 1000000 --pipeline 1000
```

输出示例：

```text
requests=1000000
completed=1000000
errors=0
connections=100
pipeline=1000
duration=1.302s
qps=767975.47
```

压测结果会受到机器性能、编译模式、系统参数和运行环境影响，上面的数据仅作为本机测试示例。

## 测试

```bash
ctest --test-dir build
```

如果需要重新生成构建目录：

```bash
rm -rf build
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

## 注意事项

- `AgentDemoService` 当前通过 `curl` 子进程请求 DeepSeek API，因此运行 Agent Demo 时需要系统已安装 `curl`。
- `agent_demo.conf` 中不要提交真实 API Key。建议使用自己的本地配置，或后续改造为从环境变量读取密钥。
- 当前 JSON 解析逻辑偏轻量，适合 demo 和学习场景；如果用于生产环境，建议替换为成熟 JSON 库。
- 当前项目主要面向 Linux 环境，依赖 `epoll`、`fork`、`pipe`、`execvp` 等系统调用。

## 后续可扩展方向

- 支持 HTTP 协议解析和静态文件服务。
- 增加连接 idle timeout、限流和更完整的错误处理。
- 使用成熟 HTTP 客户端与 JSON 库替换 demo 中的轻量实现。
- 将 API Key 改为环境变量或独立未跟踪配置文件。
- 增加更完整的单元测试和压力测试场景。
