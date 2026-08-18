# 基于 epoll 的 C++ 网络服务器框架学习路线

## 快速导航

- [文档目标](#1-文档目标)
- [当前项目概览](#2-当前项目概览)
- [学习原则](#3-学习原则)
- [阶段零：环境与运行基线](#4-阶段零准备环境和建立运行基线)
- [阶段一：C++11 基础](#5-阶段一补齐必要的-c11-基础)
- [阶段二：Linux Socket](#6-阶段二学习-linux-socket-与非阻塞-io)
- [阶段三：epoll、Channel 和 Poller](#7-阶段三理解-epollchannel-和-poller)
- [阶段四：EventLoop 与 Reactor](#8-阶段四理解-eventloop-与-reactor)
- [阶段五：主从 Reactor 和线程池](#9-阶段五理解主从-reactor-和线程池)
- [阶段六：TcpConnection 生命周期](#10-阶段六理解-tcpconnection-生命周期)
- [阶段七：Buffer 和非阻塞收发](#11-阶段七理解-buffer-和非阻塞收发)
- [阶段八：HTTP/1.1 基础](#12-阶段八学习-http11-基础)
- [阶段九：HTTP 状态机](#13-阶段九理解-http-状态机实现)
- [阶段十：HttpServer 和响应](#14-阶段十理解-httpserver-和响应发送)
- [阶段十一：异步日志](#15-阶段十一理解异步日志)
- [阶段十二：Agent Demo](#16-阶段十二理解-agent-demo)
- [阶段十三：把 Agent 接入 HTTP](#17-阶段十三把-agent-接入-http)
- [十二周计划](#19-建议的十二周计划)
- [调试工具](#20-调试与观察工具)
- [面试讲解模板](#22-面试讲解模板)
- [最终自测清单](#24-最终自测清单)

### 时间有限时的最短主线

如果暂时没有时间完成所有内容，优先按下面顺序学习：

```text
阶段零：先把项目运行起来
-> 阶段二：Socket 和非阻塞 IO
-> 阶段三：epoll、Channel、Poller
-> 阶段四：EventLoop 和 eventfd
-> 阶段五：主从 Reactor
-> 阶段六：TcpConnection 生命周期
-> 阶段七：Buffer 和部分写
-> 阶段八至十：HTTP 报文、状态机和 HttpServer
```

C++ 基础可以在看不懂具体语法时回到阶段一补充；日志、Agent、LFU、内存池和定时器可以放在主线之后。

### 学习前的安全事项

仓库中的 DeepSeek API Key 曾经以明文形式保存在配置文件并进入 Git。学习和运行前应在 DeepSeek 平台吊销旧 Key、生成新 Key，并避免把新 Key 再次提交到仓库。即使删除当前文件中的 Key，旧提交历史仍可能保留它。

## 1. 文档目标

这份路线以当前仓库为学习材料，目标不是让你一次记住所有代码，而是逐步回答下面几个核心问题：

1. Linux 如何用 Socket 建立 TCP 连接？
2. 为什么服务器需要非阻塞 IO 和 epoll？
3. Reactor、EventLoop、Channel、Poller 分别负责什么？
4. 主从 Reactor 和 One Loop per Thread 是如何实现的？
5. TCP 为什么需要 Buffer，半包和粘包如何处理？
6. HTTP/1.1 如何在 TCP 字节流上恢复请求边界？
7. HTTP 请求如何经过网络层、协议层、业务层，再返回响应？
8. Agent 为什么不能在 IO 线程中同步等待外部 API？

最终希望你能够：

- 不看代码画出项目架构图和线程模型图。
- 顺着代码讲清楚新连接、读事件、发送和关闭的完整调用链。
- 解释 HTTP 状态机如何处理半包、粘包和 Keep-Alive。
- 独立增加一个简单 HTTP 路由。
- 知道当前实现的边界和后续合理优化方向。
- 面试时能够说明设计原因，而不只是背诵类名。

---

## 2. 当前项目概览

当前程序同时运行两个应用层服务：

| 端口 | 协议 | 用途 |
|---|---|---|
| `18080` | TCP 按行文本协议 | 原有 Agent Demo，支持 `/health`、`/clear`、`/quit` |
| `18081` | 基础 HTTP/1.1 | 当前提供 `GET /health` |

整体分层如下：

```text
业务层
├── AgentDemoService
└── onHttpRequest
        |
应用层协议
├── TCP 按行文本协议
└── HttpServer / HttpContext / HttpRequest / HttpResponse
        |
TCP 连接层
├── TcpServer
├── Acceptor
├── TcpConnection
└── Buffer
        |
事件驱动层
├── EventLoop
├── Channel
├── Poller / EPollPoller
└── eventfd 跨线程唤醒
        |
操作系统
├── socket / bind / listen / accept4
├── readv / write
├── epoll
└── pthread
```

当前线程模型：

```text
                         主线程
                           |
                       baseLoop
                           |
              +------------+------------+
              |                         |
       Agent Acceptor              HTTP Acceptor
         18080                       18081
              |                         |
      Agent IO 线程池             HTTP IO 线程池
       3 个 subLoop                2 个 subLoop
```

注意：当前两个 `TcpServer` 共用一个 baseLoop，但各自拥有一个 `EventLoopThreadPool`。

---

## 3. 学习原则

### 3.1 每一阶段使用四步法

每学一个模块，都按以下顺序进行：

1. **先看接口**：阅读头文件，明确类负责什么、不负责什么。
2. **再追主流程**：只看一条正常调用链，不立刻研究所有异常分支。
3. **动手实验**：运行服务，通过日志、`curl`、`nc` 观察真实行为。
4. **最后复述**：关闭代码，用自己的话讲一遍设计和调用链。

### 3.2 不要一开始逐行阅读全部源码

第一遍先抓住主干：

```text
main
-> TcpServer
-> Acceptor
-> EventLoop
-> EPollPoller
-> Channel
-> TcpConnection
-> Buffer
```

等主干明确后，再读线程池、日志、HTTP、Agent 和边界处理。

### 3.3 每阶段都有完成标准

如果还不能完成阶段末尾的检查项，就先不要进入下一阶段。学习项目最重要的是形成稳定的心智模型，而不是快速看完文件。

---

## 4. 阶段零：准备环境和建立运行基线

### 学习目标

- 会在 WSL 中配置、编译和运行项目。
- 能分别访问 TCP Agent 和 HTTP 服务。
- 知道源码、构建产物和日志的位置。

### 需要掌握的基础知识

- Linux 基础命令：`pwd`、`ls`、`ps`、`ss`。
- 编译和链接的区别。
- CMake 的配置阶段和构建阶段。
- 进程、线程、端口和文件描述符的基本概念。

### 运行命令

```bash
cd /mnt/f/webserver/webserver
cmake -S . -B build
cmake --build build --parallel
cd bin
./main
```

新开一个 WSL 终端测试 TCP Agent：

```bash
nc 127.0.0.1 18080
```

输入：

```text
/health
/clear
/quit
```

测试 HTTP：

```bash
curl -v http://127.0.0.1:18081/health
```

观察监听端口：

```bash
ss -ltnp | grep -E ':18080|:18081'
```

### 需要理解的问题

1. `cmake -S . -B build` 做了什么？
2. `cmake --build build` 做了什么？
3. `./main` 是一个进程，内部为什么可以有多个线程？
4. `127.0.0.1` 表示什么？
5. 端口 `18080` 和 `18081` 为什么可以由同一个进程监听？

### 完成标准

- [ ] 能独立编译并启动程序。
- [ ] 能使用 `nc` 与 TCP Agent 交互。
- [ ] 能使用 `curl -v` 查看完整 HTTP 请求和响应。
- [ ] 能使用 `ss` 确认端口监听状态。

---

## 5. 阶段一：补齐必要的 C++11 基础

### 学习目标

看懂项目使用的主要 C++ 语言特性，不要求先掌握模板元编程或复杂泛型技术。

### 必学内容

#### 5.1 RAII

RAII 的核心思想是：资源生命周期绑定对象生命周期。

项目示例：

- `Socket` 析构时关闭 fd。
- `std::unique_ptr<Socket>` 管理 Socket 对象。
- `std::lock_guard<std::mutex>` 离开作用域时自动解锁。

需要回答：为什么不在所有错误分支中手动 `close()` 和 `unlock()`？

#### 5.2 智能指针

重点理解：

- `unique_ptr`：独占所有权。
- `shared_ptr`：共享所有权，通过引用计数延长生命周期。
- `weak_ptr`：观察对象，但不增加引用计数。

项目示例：

- `TcpServer` 独占 `Acceptor`，使用 `unique_ptr`。
- `TcpServer::connections_` 共享持有连接，使用 `shared_ptr<TcpConnection>`。
- `Channel::tie()` 使用 `weak_ptr`，避免事件回调期间对象被销毁。

常见面试问题：

1. `shared_ptr` 是否绝对线程安全？
2. `weak_ptr` 为什么能避免循环引用？
3. `enable_shared_from_this` 有什么作用？
4. 为什么不能对一个栈对象调用 `shared_from_this()`？

#### 5.3 `std::function`、`std::bind` 和 lambda

项目大量使用回调：

```cpp
server_.setMessageCallback(
    std::bind(&HttpServer::onMessage, this,
              std::placeholders::_1,
              std::placeholders::_2,
              std::placeholders::_3));
```

需要理解：

- 回调函数是什么。
- 成员函数为什么需要绑定对象 `this`。
- 占位符 `_1`、`_2`、`_3` 表示什么。
- lambda 按值捕获和按引用捕获的生命周期差异。

#### 5.4 移动语义

重点看：

- `std::move`
- `unique_ptr` 为什么不能复制但可以移动。
- Buffer 或回调对象移动能减少什么成本。

#### 5.5 原子变量与互斥锁

重点理解：

- `std::atomic_bool` 适合简单状态。
- `std::mutex` 保护多个操作组成的临界区。
- 原子变量不能自动保证整个容器线程安全。

项目示例：

- `EventLoop::quit_`
- `TcpServer::started_`
- `HttpServer::contextsMutex_`

### 推荐阅读文件

```text
include/noncopyable.h
include/Callbacks.h
include/TcpConnection.h
include/Channel.h
include/HttpServer.h
src/TcpConnection.cc
```

### 动手练习

1. 写一个拥有 `unique_ptr<int>` 的小类，观察它为什么不能复制。
2. 写一个 lambda，分别按值和按引用捕获局部字符串，解释异步执行时哪个更安全。
3. 找出项目中三个使用 `shared_from_this()` 的位置，说明为什么要延长对象生命周期。

### 完成标准

- [ ] 能解释三种智能指针的所有权区别。
- [ ] 能看懂项目里的 `std::bind` 和回调注册。
- [ ] 能解释跨线程任务为什么不能随意捕获裸 `this` 或 `string::c_str()`。
- [ ] 能解释 `lock_guard` 保护的临界区在哪里结束。

---

## 6. 阶段二：学习 Linux Socket 与非阻塞 IO

### 学习目标

理解服务器从创建监听 Socket 到接收客户端连接的系统调用流程。

### 基础调用链

```text
socket
-> setsockopt
-> bind
-> listen
-> accept4
-> read/write
-> shutdown/close
```

### 推荐阅读顺序

```text
include/InetAddress.h
src/InetAddress.cc
include/Socket.h
src/Socket.cc
include/Acceptor.h
src/Acceptor.cc
```

### 重点概念

#### 6.1 文件描述符

Linux 把 Socket 也表示为整数 fd。fd 是进程文件描述符表中的索引，不是 Socket 对象本身。

#### 6.2 监听 Socket 与连接 Socket

- 监听 Socket：只负责接收新连接请求。
- 连接 Socket：由 `accept4()` 返回，负责与某个具体客户端通信。

#### 6.3 阻塞与非阻塞

阻塞 Socket 在暂时无法完成操作时让线程睡眠；非阻塞 Socket 立即返回 `EAGAIN/EWOULDBLOCK`。

Reactor 为什么必须配合非阻塞 IO：一个 EventLoop 线程需要管理多个连接，不能因为某一个连接暂时没有数据而阻塞整个线程。

#### 6.4 常见 Socket 选项

- `SO_REUSEADDR`：服务器重启时更容易重新绑定地址。
- `SO_REUSEPORT`：允许多个 Socket 绑定同一地址端口，当前由配置决定是否启用。
- `TCP_NODELAY`：关闭 Nagle 算法，减少小包延迟。
- `SO_KEEPALIVE`：TCP 层探测失效连接，与 HTTP Keep-Alive 不是同一概念。

#### 6.5 `shutdown` 与 `close`

- `shutdown(SHUT_WR)`：半关闭写端，仍可接收数据。
- `close(fd)`：释放当前进程持有的 fd。

### 常见面试问题

1. `listen()` 的 backlog 参数是什么？
2. `accept()` 返回的新 fd 和监听 fd 有什么区别？
3. 非阻塞 `read()` 返回 `-1` 是否一定表示错误？
4. `EAGAIN` 和 `EWOULDBLOCK` 表示什么？
5. 为什么使用 `accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC)`？
6. TCP KeepAlive 与 HTTP Keep-Alive 有什么区别？
7. SIGPIPE 为什么可能终止服务器？当前项目如何处理？

### 动手练习

1. 在 `Acceptor::handleRead()` 设置断点，连接一次 `nc`，观察 `connfd`。
2. 使用 `ss -tnp` 观察连接建立前后 Socket 状态。
3. 客户端连接后立即关闭，观察服务器连接建立和断开日志。

### 完成标准

- [ ] 能画出 `socket -> bind -> listen -> accept` 流程。
- [ ] 能区分监听 Socket 和连接 Socket。
- [ ] 能解释非阻塞 IO 为什么是 Reactor 的必要条件。
- [ ] 能解释项目为什么忽略 SIGPIPE。

---

## 7. 阶段三：理解 epoll、Channel 和 Poller

### 学习目标

理解 IO 多路复用不是替你读写数据，而是通知哪些 fd 当前已经就绪。

### 推荐阅读顺序

```text
include/Poller.h
src/Poller.cc
include/EPollPoller.h
src/EPollPoller.cc
include/Channel.h
src/Channel.cc
src/DefaultPoller.cc
```

### 核心关系

```text
EventLoop
   |
   | 调用 poll()
   v
Poller / EPollPoller
   |
   | epoll_wait 返回就绪 fd
   v
Channel
   |
   | 根据 revents 调用回调
   v
TcpConnection / Acceptor
```

### 重点概念

#### 7.1 epoll 的三个主要接口

```text
epoll_create1：创建 epoll 实例
epoll_ctl：添加、修改或删除关注的 fd
epoll_wait：等待就绪事件
```

#### 7.2 Channel 是什么

Channel 不拥有 fd，也不负责关闭 fd。它只是把下面几项绑定在一起：

```text
fd
+ 感兴趣的事件 events
+ 实际发生的事件 revents
+ 读/写/关闭/错误回调
```

#### 7.3 Poller 是什么

`Poller` 是 IO 多路复用的抽象接口，`EPollPoller` 是 Linux epoll 实现。

这个设计体现多态：EventLoop 只依赖 `Poller` 接口，不直接依赖某个具体实现。

#### 7.4 LT 与 ET

- LT（Level Triggered）：只要条件仍满足，就会继续通知。
- ET（Edge Triggered）：只在状态变化时通知，通常必须一次读到 `EAGAIN`。

当前项目主要按 LT 思路实现，学习初期不要急于切换 ET。

### 常见面试问题

1. `select`、`poll`、`epoll` 有什么区别？
2. epoll 是同步 IO 还是异步 IO？
3. epoll 通知可读后，数据是谁读取的？
4. LT 和 ET 有什么区别？
5. Channel 是否拥有 Socket fd？为什么？
6. `events_` 与 `revents_` 有什么区别？
7. 为什么不能同时重复监听 EPOLLOUT？

### 动手练习

1. 在 `EPollPoller::poll()` 打印本次返回事件数量。
2. 在 `Channel::handleEventWithGuard()` 观察读事件和写事件的分发。
3. 列出一个连接建立后，在 epoll 中注册了哪些 fd。

### 完成标准

- [ ] 能解释 epoll 只负责“通知就绪”，不负责读取数据。
- [ ] 能说清 Channel 的四类回调。
- [ ] 能说清 EventLoop、Poller、Channel 三者关系。
- [ ] 能解释当前项目为什么选择 LT 更容易保证正确性。

---

## 8. 阶段四：理解 EventLoop 与 Reactor

### 学习目标

掌握项目最核心的事件循环，理解 IO 事件和任务队列如何在同一个线程中被处理。

### 推荐阅读顺序

```text
include/EventLoop.h
src/EventLoop.cc
include/Channel.h
src/Channel.cc
```

### EventLoop 主循环

```text
while (!quit)
    |
    +-> poller.poll() 等待 IO
    |
    +-> 遍历 activeChannels
    |      |
    |      +-> channel.handleEvent()
    |
    +-> doPendingFunctors() 执行跨线程任务
```

### Reactor 是什么

Reactor 是一种事件驱动设计模式：

1. 注册 fd 和感兴趣事件。
2. IO 多路复用器等待事件。
3. 事件就绪后分发到对应处理器。
4. 处理器执行非阻塞读写或快速业务回调。

### `runInLoop` 与 `queueInLoop`

- 当前就在目标 EventLoop 线程：`runInLoop` 可直接执行。
- 当前不在目标线程：任务放入 `pendingFunctors_`，再唤醒目标 EventLoop。

### eventfd 的作用

subLoop 可能阻塞在 `epoll_wait()`。其他线程只把任务加入队列还不够，因为 subLoop 不知道队列变了。

唤醒流程：

```text
其他线程 queueInLoop
-> pendingFunctors_ 加入任务
-> write(eventfd)
-> eventfd 变为可读
-> epoll_wait 返回
-> EventLoop::handleRead 消费 eventfd
-> doPendingFunctors 执行任务
```

### 常见面试问题

1. Reactor 与 Proactor 有什么区别？
2. EventLoop 为什么通常与线程一一绑定？
3. `queueInLoop` 为什么需要 mutex？
4. 为什么把任务加入队列后还需要 eventfd？
5. eventfd 与普通 pipe 相比有什么特点？
6. 回调中执行耗时任务会有什么后果？
7. 为什么 EventLoop 回调应该尽量短？

### 动手练习

1. 从其他线程向 EventLoop 投递一个打印任务。
2. 在 `wakeup()`、`handleRead()`、`doPendingFunctors()` 加断点，观察完整过程。
3. 思考如果没有 eventfd，任务最迟何时才会执行。

### 完成标准

- [ ] 能画出 EventLoop 主循环。
- [ ] 能讲清跨线程任务投递和 eventfd 唤醒过程。
- [ ] 能解释为什么不能在 EventLoop 回调中执行长时间阻塞任务。
- [ ] 能解释 one loop per thread 的基本含义。

---

## 9. 阶段五：理解主从 Reactor 和线程池

### 学习目标

理解 baseLoop 与 subLoop 的职责划分，以及新连接如何分配给 IO 线程。

### 推荐阅读顺序

```text
include/Thread.h
src/Thread.cc
include/EventLoopThread.h
src/EventLoopThread.cc
include/EventLoopThreadPool.h
src/EventLoopThreadPool.cc
include/TcpServer.h
src/TcpServer.cc
```

### 线程模型

```text
baseLoop 线程
   |
   +-> 监听 listenfd
   +-> accept 新连接
   +-> 轮询选择 subLoop
               |
               +-> subLoop0 管理一组 TcpConnection
               +-> subLoop1 管理一组 TcpConnection
               +-> subLoop2 管理一组 TcpConnection
```

### One Loop per Thread

每个 EventLoop 固定属于一个线程，每条 TcpConnection 固定属于一个 EventLoop。

主要优点：

- 同一连接的 Channel 和 Buffer 通常只在一个 IO 线程中操作。
- 减少锁竞争。
- 跨线程操作统一通过任务队列投递。
- 连接的线程归属清晰。

### 新连接分配链路

```text
Acceptor::handleRead
-> accept4
-> TcpServer::newConnection
-> EventLoopThreadPool::getNextLoop
-> 创建 TcpConnection
-> subLoop->runInLoop(connectEstablished)
-> Channel::enableReading
```

### 常见面试问题

1. 主从 Reactor 中主 Reactor 和从 Reactor 分别做什么？
2. 为什么不让 baseLoop 处理所有连接 IO？
3. 新连接如何选择 subLoop？当前是负载均衡吗？
4. 一个连接能否在多个 EventLoop 中同时处理？
5. One Loop per Thread 为什么可以减少锁？
6. IO 线程池和业务线程池有什么区别？

### 动手练习

1. 将 Agent 的线程数暂时设为 1、2、3，观察线程创建日志。
2. 连续建立多个 `nc` 连接，观察连接名称和所属 loop。
3. 画出当前同时运行 AgentServer 和 HttpServer 时的线程图。

### 完成标准

- [ ] 能讲清 baseLoop 与 subLoop 的职责。
- [ ] 能从 `newConnection()` 追到 `connectEstablished()`。
- [ ] 能解释 IO 线程池不等于业务线程池。
- [ ] 能解释轮询分配的优点和局限。

---

## 10. 阶段六：理解 TcpConnection 生命周期

### 学习目标

掌握连接建立、读数据、发送、关闭和对象销毁的完整链路。

### 推荐阅读顺序

```text
include/TcpConnection.h
src/TcpConnection.cc
include/TcpServer.h
src/TcpServer.cc
include/Channel.h
src/Channel.cc
```

### 状态机

```text
kConnecting
-> kConnected
-> kDisconnecting
-> kDisconnected
```

### 建立链路

```text
TcpServer::newConnection
-> make_shared<TcpConnection>
-> connections_ 保存 shared_ptr
-> connectEstablished
-> Channel::tie(shared_from_this())
-> enableReading
-> connectionCallback
```

### 读事件链路

```text
epoll_wait
-> Channel::handleEvent
-> TcpConnection::handleRead
-> inputBuffer_.readFd
-> messageCallback
-> HttpServer::onMessage 或 AgentDemoService::onMessage
```

### 关闭链路

```text
read 返回 0
-> handleClose
-> closeCallback
-> TcpServer::removeConnection
-> 从 connections_ 删除
-> connectDestroyed
-> Channel::remove
-> Socket 析构 close(fd)
```

### 生命周期重点

`Channel::tie()` 保存 `weak_ptr`。事件处理开始时尝试提升为 `shared_ptr`，确保回调执行期间 TcpConnection 不会突然析构。

跨线程 `send()` 需要：

- 按值保存待发送字符串。
- 捕获 `shared_ptr<TcpConnection>`。
- 在连接所属 EventLoop 执行真正写操作。

### 常见面试问题

1. 为什么 TcpConnection 继承 `enable_shared_from_this`？
2. Channel 为什么使用 `weak_ptr` 而不是 `shared_ptr`？
3. 跨线程发送为什么不能捕获 `buf.c_str()`？
4. 为什么连接删除要在 baseLoop 和 subLoop 之间分别投递任务？
5. `read()` 返回 0 代表什么？
6. 为什么发送完成前不能直接销毁连接？

### 动手练习

1. 为同一连接的构造、建立、关闭、析构日志记录顺序。
2. 客户端发送 `/quit`，观察半关闭和最终断开。
3. 客户端建立后直接退出，观察 `read == 0` 路径。

### 完成标准

- [ ] 能讲清连接状态变化。
- [ ] 能解释 `tie()` 解决的问题。
- [ ] 能解释跨线程 send 的正确生命周期管理。
- [ ] 能追踪连接最终在哪里从 `connections_` 删除。

---

## 11. 阶段七：理解 Buffer 和非阻塞收发

### 学习目标

理解 TCP 字节流、动态 Buffer、部分写和 EPOLLOUT。

### 推荐阅读顺序

```text
include/Buffer.h
src/Buffer.cc
src/TcpConnection.cc
```

### Buffer 布局

```text
| prependable | readable | writable |
              ^          ^
         readerIndex  writerIndex
```

### 读取过程

`Buffer::readFd()` 使用 `readv`：

```text
iovec[0] -> Buffer 当前可写区域
iovec[1] -> 栈上额外 64 KiB 空间
```

如果数据较少，直接写进 Buffer；如果数据超过可写区域，多余部分先进入额外栈空间，再追加到 Buffer。

### 发送过程

```text
TcpConnection::send
-> sendInLoop
-> 尝试直接 write
-> 全部写完：结束
-> 只写一部分：剩余数据进入 outputBuffer
-> enableWriting 关注 EPOLLOUT
-> handleWrite 继续发送
-> 缓冲区清空后 disableWriting
```

### 为什么会部分写

非阻塞 Socket 的内核发送缓冲区可能暂时没有足够空间。`write()` 返回成功只表示写入了一部分字节，不保证整条消息已经发送。

### 常见面试问题

1. TCP 为什么会有半包和粘包？
2. `readv` 与 `read` 有什么区别？
3. 为什么不能一直监听 EPOLLOUT？
4. 部分写如何处理？
5. outputBuffer 为什么可能无限增长？
6. 高水位回调有什么用途？
7. Buffer 为什么优先复用前部空间，而不是每次扩容？

### 动手练习

1. 手动画出 Buffer 执行 `retrieve()` 前后的下标变化。
2. 构造超过初始大小的数据，观察 Buffer 扩容。
3. 找出 `sendInLoop()` 中直接写与缓冲写的分界条件。

### 完成标准

- [ ] 能画出 Buffer 三段布局。
- [ ] 能解释 `readv` 的两个 iovec。
- [ ] 能解释部分写和 EPOLLOUT 的配合。
- [ ] 能解释 TCP 字节流为什么需要应用层协议解析。

---

## 12. 阶段八：学习 HTTP/1.1 基础

### 学习目标

先理解协议文本，再阅读项目 HTTP 代码。

### HTTP 请求示例

```http
GET /health?verbose=1 HTTP/1.1
Host: 127.0.0.1:18081
Connection: keep-alive

```

### HTTP POST 示例

```http
POST /agent/run HTTP/1.1
Host: 127.0.0.1:18081
Content-Type: application/json
Content-Length: 19

{"message":"hello"}
```

### HTTP 响应示例

```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
Content-Length: 16
Connection: keep-alive

{"status":"ok"}
```

### 必学概念

- Method：GET、POST 等。
- Request Target：path 和 query。
- HTTP Version：HTTP/1.0、HTTP/1.1。
- Header 名称大小写不敏感。
- CRLF：每行使用 `\r\n` 结束。
- 空行：Header 与 Body 的分界。
- `Content-Length`：Body 的字节长度。
- `Host`：HTTP/1.1 必需字段。
- Keep-Alive：一个 TCP 连接承载多条 HTTP 请求。
- Pipeline：前一个响应返回前连续发送多条请求。

### 常见状态码

| 状态码 | 含义 | 当前项目使用场景 |
|---|---|---|
| 200 | OK | `GET /health` |
| 400 | Bad Request | 请求语法错误 |
| 404 | Not Found | 路径不存在 |
| 405 | Method Not Allowed | `/health` 路径存在但方法不允许 |
| 413 | Payload Too Large | 请求超过大小限制 |
| 501 | Not Implemented | 方法或 Transfer-Encoding 未实现 |

### HTTP/1.0 与 HTTP/1.1 连接差异

- HTTP/1.0 默认关闭，显式 `Connection: keep-alive` 才保留。
- HTTP/1.1 默认保留，显式 `Connection: close` 才关闭。

### 常见面试问题

1. HTTP 与 TCP 的关系是什么？
2. HTTP 是否是无状态协议？Keep-Alive 是否改变无状态性？
3. `Content-Length` 是字符数还是字节数？
4. GET 和 POST 的本质区别是什么？
5. HTTP/1.1 为什么必须有 Host？
6. Keep-Alive 和 TCP KeepAlive 是否相同？
7. Chunked 与 Content-Length 有什么区别？
8. 400、404、405、501 有什么区别？

### 动手练习

使用 `nc` 手写 HTTP：

```bash
nc 127.0.0.1 18081
```

输入以下内容，最后再输入一个空行：

```http
GET /health HTTP/1.1
Host: localhost
Connection: close

```

### 完成标准

- [ ] 能手写一条合法 HTTP/1.1 请求。
- [ ] 能解释请求行、Header、空行和 Body。
- [ ] 能解释 Content-Length 的作用。
- [ ] 能说明 HTTP/1.0 和 HTTP/1.1 的连接默认值。

---

## 13. 阶段九：理解 HTTP 状态机实现

### 学习目标

掌握本项目如何把 TCP Buffer 转换成结构化 HttpRequest。

### 推荐阅读顺序

```text
include/HttpRequest.h
src/HttpRequest.cc
include/HttpContext.h
src/HttpContext.cc
include/Buffer.h 中 findCRLF/retrieveUntil
```

### 状态转移

```text
kExpectRequestLine
         |
         v
  kExpectHeaders
     |        |
无 Body      Content-Length > 0
     |        |
     |        v
     |  kExpectBody
     |        |
     +--------+
         |
         v
      kGotAll
```

### `parseRequest()` 三类结果

1. `kComplete`：请求已经完整，可以进入业务层。
2. `kIncomplete`：当前数据不足，等待下一次读事件。
3. 错误结果：格式非法、超限或使用未支持能力。

一定要理解：`kIncomplete` 不是错误。TCP 分段是正常现象。

### 半包处理

假设两次收到：

```text
第一次：GET /heal
第二次：th HTTP/1.1\r\nHost: localhost\r\n\r\n
```

第一次找不到 CRLF，返回 `kIncomplete`，不消费 Buffer。第二次数据追加后，再从原位置找到完整请求行。

### 粘包和 Pipeline 处理

假设一次收到：

```text
GET /health HTTP/1.1 ...\r\n\r\nGET /missing HTTP/1.1 ...\r\n\r\n
```

第一条请求处理完成后：

```text
context.reset()
-> Buffer 不清空
-> while 循环继续
-> 解析第二条请求
```

### 协议边界检查

当前实现包含：

- 请求行上限 8 KiB。
- Header 上限 32 KiB。
- Body 上限 1 MiB。
- HTTP/1.1 Host 校验。
- Content-Length 必须为纯数字。
- 重复 Header 拒绝。
- Transfer-Encoding 明确返回 501。
- Header 名称 token 校验。

### 常见面试问题

1. 为什么使用状态机解析 HTTP？
2. 为什么不能收到数据后直接 `retrieveAllAsString()`？
3. 半包时哪些数据被保留？状态保存在什么地方？
4. 为什么每条连接需要自己的 HttpContext？
5. 请求完成后为什么 reset Context，但不清空 Buffer？
6. 为什么重复 Content-Length 有安全风险？
7. 如何防止恶意客户端无限发送 Header？

### 动手实验

#### 请求行半包

```bash
{ printf 'GET /heal'; sleep 1; printf 'th HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n'; } \
  | nc -w 3 127.0.0.1 18081
```

#### Pipeline

```bash
printf 'GET /health HTTP/1.1\r\nHost: localhost\r\n\r\nGET /missing HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' \
  | nc -w 3 127.0.0.1 18081
```

#### 非法重复 Content-Length

```bash
printf 'POST /health HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nContent-Length: 0\r\n\r\nhello' \
  | nc -w 3 127.0.0.1 18081
```

预期返回 `400 Bad Request`。

### 完成标准

- [ ] 能手画状态转移图。
- [ ] 能解释 `kIncomplete` 为什么不是错误。
- [ ] 能解释半包和 Pipeline 的 Buffer 变化。
- [ ] 能从 `findCRLF()` 追到 `HttpRequest` 字段写入。

---

## 14. 阶段十：理解 HttpServer 和响应发送

### 学习目标

理解 HTTP 协议层如何接入现有 TcpServer，以及响应如何返回客户端。

### 推荐阅读顺序

```text
include/HttpResponse.h
src/HttpResponse.cc
include/HttpServer.h
src/HttpServer.cc
src/main.cc 中 onHttpRequest
```

### 完整请求链路

```text
客户端 curl
-> Socket 可读
-> epoll_wait 返回
-> Channel::handleEvent
-> TcpConnection::handleRead
-> Buffer::readFd
-> HttpServer::onMessage
-> HttpContext::parseRequest
-> HttpServer::onRequest
-> main.cc::onHttpRequest
-> HttpResponse::appendToBuffer
-> TcpConnection::send
-> 客户端收到响应
```

### 为什么 `contexts_` 需要锁

每条连接自己的 HttpContext 只由所属 EventLoop 处理；但 `contexts_` 是所有 HTTP IO 线程共享的容器，不同线程可能同时建立、查找和删除连接，因此映射需要 mutex。

锁只保护映射操作，解析过程不持锁。这是较小的锁粒度。

### 响应序列化

```text
状态行
-> Connection
-> Content-Length
-> 业务 Header
-> 空行
-> Body
```

### C++ 生命周期重点

`HttpServer` 中 `TcpServer server_` 声明在最后，因此析构时最先销毁。这样可以先停止网络回调，再销毁回调会访问的 `contexts_` 和 mutex。

C++ 成员按声明顺序的逆序析构，不按初始化列表顺序析构。

### 常见面试问题

1. 为什么 HttpServer 使用组合而不是继承 TcpServer？
2. 为什么 HttpServer 不直接调用 epoll？
3. 为什么同一连接的 HttpContext 可以不加锁？
4. 为什么共享 contexts_ 仍然需要锁？
5. 响应的 Content-Length 如何计算？
6. 为什么业务层不能覆盖 Connection 和 Content-Length？
7. 为什么错误请求通常响应后关闭连接？
8. C++ 成员析构顺序为什么会影响回调安全？

### 动手练习：增加路由

在 `onHttpRequest()` 中增加：

```text
GET /hello
```

返回：

```json
{"message":"hello"}
```

测试：

```bash
curl -v http://127.0.0.1:18081/hello
```

再增加只允许 POST 的 `/echo`，原样返回 Body。这个练习可以验证你是否真正理解 Method、Body 和路由。

### 完成标准

- [ ] 能完整讲述 curl 到响应返回的调用链。
- [ ] 能解释 HttpServer 与 TcpServer 的分层关系。
- [ ] 能独立增加一个 GET 路由。
- [ ] 能解释 Connection 和 Content-Length 为什么由框架生成。

---

## 15. 阶段十一：理解异步日志

### 学习目标

理解为什么网络线程不应频繁同步写磁盘，以及双缓冲日志如何降低前端阻塞。

### 推荐阅读顺序

```text
include/Logger.h
src/Logger.cc
include/LogStream.h
log/LogStream.cc
include/AsyncLogging.h
log/AsyncLogging.cc
include/LogFile.h
log/LogFile.cc
log/FileUtil.cc
```

### 数据流

```text
业务线程 LOG_INFO
-> Logger / LogStream
-> AsyncLogging::append
-> currentBuffer_
-> 后台日志线程
-> LogFile
-> FileUtil
-> 磁盘文件
```

### 双缓冲思想

前端写当前 Buffer；Buffer 满或定时刷新后，后台线程交换 Buffer 并批量落盘。这样前端主要执行内存复制，而不是每条日志都等待磁盘 IO。

### 常见面试问题

1. 同步日志和异步日志有什么区别？
2. 为什么异步日志需要后台线程？
3. 双缓冲减少了什么开销？
4. 日志线程停止时为什么必须 join？
5. 如果进程崩溃，异步日志是否一定完整？
6. 为什么高频事件不应该使用 INFO 级别？

### 完成标准

- [ ] 能画出日志从前端到磁盘的流程。
- [ ] 能解释 `stop -> notify -> flush -> join`。
- [ ] 能解释异步日志的性能优势和日志丢失风险。

---

## 16. 阶段十二：理解 Agent Demo

### 学习目标

理解 Agent 是现有网络框架上的业务，不要把它和 epoll/Reactor 混为一层。

### 推荐阅读顺序

```text
include/AgentDemo.h
src/AgentDemo.cc
config/agent_demo.conf
src/main.cc 中 AgentServer
```

### 当前 TCP Agent 流程

```text
客户端 nc
-> TcpConnection 收到一行
-> AgentDemoService::onMessage
-> 按 '\n' 拆分命令
-> 保存/读取连接级会话历史
-> 请求 DeepSeek 规划工具
-> calculator 或 time
-> 必要时再次请求 DeepSeek
-> TcpConnection::send 返回答案
```

### 当前上下文特点

- 以连接名称作为会话 key。
- 同一 TCP 长连接支持多轮对话。
- 连接断开后删除历史。
- 进程重启后历史丢失。
- 这不是持久化记忆。

### 当前外部 API 调用的问题

目前通过外部 `curl` 子进程同步调用 DeepSeek。同步等待发生在 IO 线程时，会阻塞该 subLoop 上的其他连接。

理想结构：

```text
IO 线程解析请求
-> 投递业务线程池
-> 业务线程使用 libcurl 请求 DeepSeek
-> queueInLoop 回到连接所属 IO 线程
-> TcpConnection::send
```

### 常见面试问题

1. Agent 业务运行在哪个线程？
2. 为什么同步调用 DeepSeek 会阻塞 Reactor？
3. IO 线程池和业务线程池有什么区别？
4. 多轮上下文当前如何隔离？
5. TCP 连接级会话有什么局限？
6. 为什么 HTTP 版本更适合使用 session_id？
7. 为什么出站 DeepSeek HTTPS 推荐使用 libcurl？

### 完成标准

- [ ] 能区分网络层、HTTP 层和 Agent 业务层。
- [ ] 能讲清当前 Agent 一次请求的执行过程。
- [ ] 能解释阻塞 DeepSeek 调用对 EventLoop 的影响。
- [ ] 能画出未来业务线程池方案。

---

## 17. 阶段十三：把 Agent 接入 HTTP

这一阶段是后续开发路线，目前项目尚未完成。

### 目标接口

```http
POST /agent/run HTTP/1.1
Host: 127.0.0.1:18081
Content-Type: application/json
Content-Length: ...

{"session_id":"demo-1","message":"计算 12 * 15"}
```

响应：

```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
Content-Length: ...

{"session_id":"demo-1","answer":"180"}
```

### 推荐实现顺序

1. 先增加一个不调用 DeepSeek 的 `POST /echo`。
2. 引入 JSON 库并安全解析 Body。
3. 增加 `POST /agent/run` 路由。
4. 暂时复用现有 Agent 逻辑，验证功能链路。
5. 增加独立业务线程池。
6. 使用 libcurl 替换 `fork + exec curl`。
7. 使用 `session_id` 管理 HTTP 会话。
8. 增加请求超时、并发限制和响应大小限制。

### 为什么先做 `/echo`

`/echo` 只验证 HTTP POST 和 Body，不涉及 JSON、线程池和 DeepSeek。每次只增加一个变量，出现问题时更容易定位。

### 为什么 HTTP 会话使用 session_id

HTTP 客户端不保证一直复用同一 TCP 连接。如果把会话绑定到 TcpConnection，客户端重连后上下文就丢失。`session_id` 把应用会话与底层连接解耦。

### 完成标准

- [ ] POST Body 能正确解析。
- [ ] `/echo` 能正确处理半包 Body。
- [ ] Agent 请求不会阻塞 EventLoop。
- [ ] 同一 session_id 能保持多轮上下文。
- [ ] DeepSeek 请求失败时返回明确 HTTP 错误。

---

## 18. 暂时不必深入的模块

以下模块可以在主线掌握后再学习，不要让它们阻塞 Reactor 和 HTTP 主线：

### TimerQueue

当前未接入 EventLoop。后续可用于：

- HTTP 空闲连接超时。
- Session 过期清理。
- 请求超时。
- 周期性统计。

### 内存池

当前完成初始化但未真正接入网络对象。先理解固定大小槽位和自由链表，不要为了展示功能强行替换所有 `new`。

### LFU

当前主业务未使用。可以作为数据结构练习，重点理解频率链表、最小频率和淘汰策略。

### sendFile

适合后续静态文件服务器。当前 Agent 和 `/health` 都不需要它。

学习优先级：

```text
Reactor/TCP > Buffer > HTTP > Agent 线程隔离 > Timer/LFU/MemoryPool
```

---

## 19. 建议的十二周计划

这只是参考。时间不足时可以延长，不需要为了赶进度跳过完成标准。

| 周次 | 学习内容 | 最终输出 |
|---|---|---|
| 第 1 周 | WSL、CMake、运行基线、C++ RAII | 独立编译运行，解释资源生命周期 |
| 第 2 周 | 智能指针、回调、lambda、线程同步 | 解释 `shared_from_this` 和回调绑定 |
| 第 3 周 | Linux Socket、非阻塞 IO | 画出 listen/accept/read/write 流程 |
| 第 4 周 | epoll、Poller、Channel | 解释 fd 就绪到回调分发 |
| 第 5 周 | EventLoop、eventfd、任务队列 | 画出 Reactor 主循环和唤醒流程 |
| 第 6 周 | EventLoopThreadPool、主从 Reactor | 画出线程模型和新连接分配流程 |
| 第 7 周 | TcpConnection 生命周期 | 讲清连接建立、收发、关闭、销毁 |
| 第 8 周 | Buffer、readv、部分写、EPOLLOUT | 讲清半包粘包和非阻塞发送 |
| 第 9 周 | HTTP 报文基础 | 手写 GET/POST 请求和响应 |
| 第 10 周 | HttpContext 状态机 | 完成半包、Pipeline 实验 |
| 第 11 周 | HttpServer、HttpResponse、路由 | 独立增加 `/hello` 和 `/echo` |
| 第 12 周 | Agent 链路和后续线程池设计 | 画出 HTTP Agent 目标架构 |

每周建议安排：

```text
30% 基础知识
40% 阅读和调试当前代码
20% 动手实验
10% 总结与口述
```

---

## 20. 调试与观察工具

### 查看线程

```bash
ps -T -p $(pgrep -n main)
```

### 查看监听端口和连接

```bash
ss -ltnp
ss -tnp | grep -E ':18080|:18081'
```

### 查看系统调用

```bash
strace -f -e trace=network,epoll_wait,eventfd ./main
```

### 使用 GDB

```bash
gdb ./main
```

推荐断点：

```text
Acceptor::handleRead
TcpServer::newConnection
EventLoop::loop
EPollPoller::poll
Channel::handleEventWithGuard
TcpConnection::handleRead
TcpConnection::sendInLoop
HttpServer::onMessage
HttpContext::parseRequest
```

### 使用 curl 查看协议

```bash
curl -v http://127.0.0.1:18081/health
curl --http1.0 -v http://127.0.0.1:18081/health
curl -v -X POST http://127.0.0.1:18081/health
```

### 使用 nc 手写协议

`nc` 不替你生成 HTTP 格式，适合观察服务器如何处理原始字节、半包和非法请求。

---

## 21. 推荐做的最小测试

当前仓库还没有正式自动化测试。学习过程中至少手工验证：

### TCP Agent

- [ ] 建立连接收到欢迎信息。
- [ ] `/health` 返回状态。
- [ ] `/clear` 清空会话。
- [ ] `/quit` 返回 Bye 并关闭连接。

### HTTP 正常请求

- [ ] `GET /health` 返回 200。
- [ ] 未知路径返回 404。
- [ ] `POST /health` 返回 405。
- [ ] 不支持的方法返回 501。
- [ ] `Connection: close` 后连接关闭。
- [ ] HTTP/1.0 默认关闭。

### HTTP 边界请求

- [ ] 请求行拆成两次发送仍能解析。
- [ ] Body 拆成两次发送仍能解析。
- [ ] 一次发送两条请求能返回两条响应。
- [ ] 缺少 HTTP/1.1 Host 返回 400。
- [ ] 重复 Content-Length 返回 400。
- [ ] Chunked 请求返回 501。
- [ ] Body 超限返回 413。

---

## 22. 面试讲解模板

### 22.1 一分钟项目介绍

> 这是一个基于 C++11 和 Linux epoll 实现的多线程网络服务器学习项目。底层采用主从 Reactor 和 One Loop per Thread 模型，baseLoop 负责监听和接收连接，subLoop 负责已连接 Socket 的 IO 事件。项目封装了 EventLoop、Channel、Poller、TcpServer、TcpConnection 和动态 Buffer，并通过 eventfd 实现跨线程任务唤醒。在网络层上实现了基础 HTTP/1.1 状态机，支持请求行、Header、Content-Length Body、半包、连续请求和 Keep-Alive，同时保留了一个支持长连接、多轮上下文和简单工具调用的 Agent Demo。

### 22.2 新连接如何处理

> listenfd 的可读事件由 baseLoop 监听。epoll 返回后，Channel 调用 Acceptor::handleRead，通过 accept4 得到非阻塞 connfd。TcpServer 使用 EventLoopThreadPool 轮询选择 subLoop，创建 TcpConnection，再把 connectEstablished 投递到目标 subLoop，最后由该连接的 Channel 注册 EPOLLIN。此后连接 IO 固定在所属 subLoop 线程处理。

### 22.3 HTTP 半包如何处理

> TCP 没有消息边界，因此一次读取不保证得到完整 HTTP 请求。每条 HTTP 连接都有独立 HttpContext，状态机依次解析请求行、Header 和 Body。找不到 CRLF 或 Body 字节不足时返回 Incomplete，不消费未完成数据，等待下一次 EPOLLIN。请求完整后 reset Context，但保留 Buffer 中可能属于下一条请求的字节，因此也能处理粘包和 Pipeline。

### 22.4 非阻塞发送如何处理

> TcpConnection 先尝试直接 write。如果只写出一部分，剩余数据追加到 outputBuffer，并让 Channel 关注 EPOLLOUT。内核发送缓冲区重新可写时，handleWrite 继续发送，数据清空后取消 EPOLLOUT，避免可写事件持续触发造成忙循环。

### 22.5 当前项目的不足

> 当前 HTTP 是学习版 HTTP/1.1 子集，暂不支持 chunked、TLS、HTTP/2 和完整超时管理。Agent 请求目前还是同步调用外部 curl，可能阻塞 IO 线程。合理的下一步是增加独立业务线程池和 libcurl，将外部 API 调用移出 EventLoop，再通过 queueInLoop 回到连接所属线程发送响应。

---

## 23. 当前实现边界

学习时要能够主动说明下面这些边界：

- HTTP 服务端是基础 HTTP/1.1 子集，不是完整生产级实现。
- 暂不支持 Chunked Request Body。
- 暂不支持 HTTPS 服务端、HTTP/2、WebSocket。
- 暂未接入 HTTP 请求读取超时和空闲连接超时。
- HTTP 当前只有 `/health`，Agent 尚未接入 HTTP。
- Agent 的外部 DeepSeek 调用仍可能阻塞 IO 线程。
- 当前会话是连接级内存上下文，不是持久化长期记忆。
- TimerQueue、LFU 和内存池尚未成为网络主链路的必要组成部分。

知道边界不是项目缺点。能够解释“当前做了什么、为什么这样取舍、下一步如何演进”，比声称实现了所有功能更重要。

---

## 24. 最终自测清单

完成整条路线后，尝试不看源码回答：

- [ ] epoll 为什么比每连接一个阻塞线程更适合大量连接？
- [ ] EventLoop、Channel、Poller 分别是什么？
- [ ] eventfd 为什么能唤醒 EventLoop？
- [ ] 主从 Reactor 与 One Loop per Thread 如何配合？
- [ ] TcpConnection 为什么使用 shared_ptr 和 weak_ptr？
- [ ] TCP 半包和粘包是什么？
- [ ] Buffer 的 readerIndex 和 writerIndex 如何变化？
- [ ] 非阻塞 write 只发送一部分时如何处理？
- [ ] HTTP 请求状态机有哪些状态？
- [ ] Content-Length 如何确定 Body 边界？
- [ ] Keep-Alive 下如何区分连续响应？
- [ ] 为什么每条连接需要独立 HttpContext？
- [ ] 为什么 contexts_ 需要锁，但单个 Context 通常不需要锁？
- [ ] 从 curl 到 HTTP 响应返回经历了哪些函数？
- [ ] 为什么 DeepSeek 调用不应该阻塞 EventLoop？
- [ ] IO 线程池和业务线程池有什么区别？
- [ ] 当前项目还存在哪些边界和后续方向？

如果这些问题都能结合当前代码回答，而不是只背定义，就已经真正掌握了这个项目的核心。
