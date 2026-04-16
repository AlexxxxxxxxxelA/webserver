#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>

#include "mywebserver/AgentDemo.h"
#include "mywebserver/Buffer.h"
#include "mywebserver/EventLoop.h"
#include "mywebserver/InetAddress.h"
#include "mywebserver/TcpServer.h"

namespace
{

int parseIntArg(const char *arg, const char *prefix, int defaultValue)
{
    std::string value(arg);
    std::string key(prefix);
    if (value.find(key) != 0)
    {
        return defaultValue;
    }
    return std::atoi(value.substr(key.size()).c_str());
}

void onQpsMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp)
{
    std::string data = buf->retrieveAllAsString();
    size_t replies = static_cast<size_t>(std::count(data.begin(), data.end(), '\n'));
    if (replies == 0 && !data.empty())
    {
        replies = 1;
    }

    std::string response;
    response.reserve(replies * 5);
    for (size_t i = 0; i < replies; ++i)
    {
        response += "pong\n";
    }
    if (!response.empty())
    {
        conn->send(response);
    }
}

int runQpsServer(int port, int threads)
{
    InetAddress listenAddr(static_cast<uint16_t>(port), "0.0.0.0");
    EventLoop loop;
    TcpServer server(&loop, listenAddr, "QpsServer", TcpServer::kReusePort);

    server.setConnectionCallback([](const TcpConnectionPtr &) {});
    server.setMessageCallback(onQpsMessage);
    server.setThreadNum(threads);
    server.start();

    std::cout << "QpsServer listening on " << listenAddr.toIpPort()
              << ", threads=" << threads << std::endl;
    loop.loop();
    return 0;
}

int runAgentDemo()
{
    const AgentDemoConfig &config = getAgentDemoConfig();
    InetAddress listenAddr(config.port, "0.0.0.0");
    EventLoop loop;
    TcpServer server(&loop, listenAddr, "AgentDemoServer", TcpServer::kReusePort);
    AgentDemoService service;

    server.setConnectionCallback(
        std::bind(&AgentDemoService::onConnection, &service, std::placeholders::_1));
    server.setMessageCallback(
        std::bind(&AgentDemoService::onMessage,
                  &service,
                  std::placeholders::_1,
                  std::placeholders::_2,
                  std::placeholders::_3));
    server.setThreadNum(3);
    server.start();

    std::cout << "AgentDemoServer listening on " << listenAddr.toIpPort() << std::endl;
    loop.loop();
    return 0;
}

}  // namespace

int main(int argc, char *argv[])
{
    int qpsPort = 18081;
    int threads = 3;
    bool qpsMode = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg(argv[i]);
        if (arg == "--qps")
        {
            qpsMode = true;
        }
        else if (arg.find("--port=") == 0)
        {
            qpsPort = parseIntArg(argv[i], "--port=", qpsPort);
        }
        else if (arg.find("--threads=") == 0)
        {
            threads = parseIntArg(argv[i], "--threads=", threads);
        }
    }

    if (qpsMode)
    {
        return runQpsServer(qpsPort, threads);
    }

    return runAgentDemo();
}
/*
requests=1000000
completed=1000000
errors=0
connections=100

100 个客户端连接
每个连接平均发送 10000 个请求
总计 1000000 个请求

pipeline=1000

表示每个连接不是“发一个请求，等一个响应”，而是一次批量发送 1000 个请求，然后批量读取 1000 个响应。

duration=1.302s

表示完成这 100 万个请求总共用了 1.302 秒。

qps=767975.47

这个纯 TCP ping/pong 服务在本机长连接 + pipeline 模式下，粗测约 76.8 万 QPS。

*/

/*
./build/mywebserver
nc 127.0.0.1 18080
*/