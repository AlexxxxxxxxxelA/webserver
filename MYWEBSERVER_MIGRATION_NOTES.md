# 从 mywebserver 迁移到当前 webserver 的功能说明

## 1. 迁移原则

`F:\webserver\mywebserver` 是同一项目的旧实验副本，功能面更宽，但部分实现存在
HTTP 边界、阻塞 Reactor、fork/curl、生命周期和密钥管理问题。因此本次没有整仓
合并，只提取值得保留的想法，并按当前 `webserver` 的安全边界重新实现。

本次迁移：

```text
Weather 工具
TCP QPS 模式和 benchmark
target-based CMake
```

暂不迁移：

```text
TimerQueue 接入
静态文件服务
sendfile 状态机
```

## 2. Weather 工具

旧实现使用 `fork + execvp("curl")`，新实现复用当前进程内 libcurl，并运行在有界
业务线程池中，因此不会阻塞 EventLoop，也不会创建额外子进程。

资源边界：

```text
location 最大 128 字节
连接超时 3 秒
总超时 8 秒
响应最大 16 KiB
```

默认地址：

```text
https://wttr.in
```

可通过配置或环境变量覆盖，方便本地 mock：

```text
weather_api_base_url=http://127.0.0.1:19090/weather
WEATHER_API_BASE_URL=http://127.0.0.1:19090/weather
```

调用链：

```text
DeepSeek 原生 tool_calls 请求 weather({"location":"Beijing"})
-> AgentToolRegistry 校验并执行 WeatherTool
-> curl_easy_escape 编码 location
-> libcurl GET wttr.in
-> role=tool + tool_call_id 回传结果
-> DeepSeek 继续生成最终回答
```

## 3. QPS 模式

启动：

```bash
./bin/main --qps --port=18082 --threads=3
```

协议：

```text
请求：/ping\n
响应：pong\n
非法行响应：error\n
```

压测：

```bash
python3 tools/bench_tcp.py \
  --host 127.0.0.1 \
  --port 18082 \
  --connections 100 \
  --requests 100000 \
  --pipeline 100
```

QPS 模式只测试：

```text
epoll
TcpServer/TcpConnection
Buffer
TCP 长连接
pipeline 收发
```

它不测试 HTTP、JSON、DeepSeek 或 Agent，不能把该 QPS 当成 Agent 吞吐量。

与旧实现的重要区别：当前实现为每条连接保留未完成行。例如 TCP 将 `/ping\n`
拆成 `/pi` 和 `ng\n` 两次到达时，只会在完整行到达后返回一次 pong。

## 4. CMake 迁移

当前构建改为：

```text
webserver_core 静态库
-> main 可执行程序
```

使用：

```cmake
target_include_directories
target_link_libraries
Threads::Threads
CURL::libcurl
nlohmann_json::nlohmann_json
SQLite::SQLite3
```

并显式列出源文件，不再使用 `file(GLOB)`。这样新增源码时必须明确加入 target，
配置更可预测，也避免 `CurrentThread.cc` 被两个动态库重复编译。

## 5. 为什么暂不迁移其他能力

### TimerQueue

当前 HTTP、Agent 和 QPS 都不依赖定时器，所以暂不接入不影响功能。后续实现 Session
TTL、连接 idle timeout 和慢请求超时时，再修复取消定时器、时钟源和生命周期后一并接入。

### 静态文件服务

作用是直接返回 HTML、CSS、JavaScript、图片等磁盘文件，使服务器可以托管简单网站。
当前项目目标是网络框架和 Agent API，没有网页资源，因此不是近期必需功能。

### sendfile

`sendfile` 是 Linux 的零拷贝文件发送接口，可以减少“内核文件缓存 -> 用户 Buffer
-> Socket”的拷贝，适合大静态文件。它不优化普通 JSON/Agent Response；只有增加
静态文件服务后才值得实现完整的 EOF、EAGAIN、部分写和文件 fd 生命周期状态机。

## 6. 验证记录

Weather 本地 mock：

```json
{
  "answer":"天气晴朗，温度约 25 摄氏度。",
  "session_id":"weather-test",
  "tool":"weather",
  "tool_result":"Beijing: Sunny +25C"
}
```

QPS 半包测试：

```text
分两次发送 /pi + ng\n -> pong
发送 /bad\n -> error
```

小规模 benchmark：

```text
requests=1000
completed=1000
errors=0
connections=10
pipeline=20
```
