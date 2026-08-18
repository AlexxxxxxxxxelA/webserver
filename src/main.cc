#include <string>

#include <TcpServer.h>
#include <Logger.h>
#include <sys/stat.h>
#include <signal.h>
#include <sstream>
#include "AgentDemo.h"
#include "AsyncLogging.h"
#include "LFU.h"
#include "memoryPool.h"
#include "HttpServer.h"
// 日志文件滚动大小为1MB (1*1024*1024字节)
static const off_t kRollSize = 1*1024*1024;
class AgentServer
{
public:
    AgentServer(EventLoop *loop, const InetAddress &addr, const std::string &name)
        : server_(loop, addr, name)
        , loop_(loop)
    {
        server_.setConnectionCallback(
            std::bind(&AgentDemoService::onConnection, &service_, std::placeholders::_1));

        server_.setMessageCallback(
            std::bind(&AgentDemoService::onMessage, &service_, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

        server_.setThreadNum(3);
    }

    void start()
    {
        server_.start();
    }

private:
    TcpServer server_;
    EventLoop *loop_;
    AgentDemoService service_;
};

void onHttpRequest(const HttpRequest &request, HttpResponse *response)
{
    /*
     * 这是 HTTP 协议层之上的最小业务路由示例。
     * HttpServer 已经保证 request 是一条完整请求；这里不需要再处理 TCP 半包、
     * Header 或 Content-Length，只需根据 Method + Path 决定返回内容。
     *
     * 【常见面试问题】什么是路由？
     * 路由就是把 Method + Path 映射到处理函数。例如：
     *     GET  /health   -> 健康检查
     *     POST /agent/run -> Agent 请求（后续实现）
     * 当前路由很少，直接使用 if 足够；只有路由数量增长后才值得抽象 Router 类。
     */
    response->setContentType("application/json; charset=utf-8");
    if (request.method() == HttpRequest::kUnsupported)
    {
        response->setStatusCode(501);
        response->setStatusMessage("Not Implemented");
        response->setBody("{\"error\":\"method not implemented\"}\n");
        return;
    }

    if (request.method() == HttpRequest::kGet && request.path() == "/health")
    {
        response->setStatusCode(200);
        response->setStatusMessage("OK");
        response->setBody("{\"status\":\"ok\"}\n");
        return;
    }

    if (request.path() == "/health")
    {
        // 路径存在但方法不允许，405 比 404 更准确；Allow 告诉客户端可用方法。
        response->setStatusCode(405);
        response->setStatusMessage("Method Not Allowed");
        response->addHeader("Allow", "GET");
        response->setBody("{\"error\":\"method not allowed\"}\n");
        return;
    }

    response->setStatusCode(404);
    response->setStatusMessage("Not Found");
    response->setBody("{\"error\":\"not found\"}\n");
}

AsyncLogging* g_asyncLog = NULL;
AsyncLogging * getAsyncLog(){
    return g_asyncLog;
}
 void asyncLog(const char* msg, int len)
{
    AsyncLogging* logging = getAsyncLog();
    if (logging)
    {
        logging->append(msg, len);
    }
}
int main(int argc,char *argv[]) {
    // 对端关闭连接后，写操作通过错误码处理，避免SIGPIPE终止整个进程。
    ::signal(SIGPIPE, SIG_IGN);

    //第一步启动日志，双缓冲异步写入磁盘.
    //创建一个文件夹
    const std::string LogDir="logs";
    mkdir(LogDir.c_str(),0755);//创建 LogDir字符串所指定的目录，并赋予其权限：所有者可读、写、执行，同组用户和其他用户可读、执行。
    //使用std::stringstream 构建日志文件夹
    std::ostringstream LogfilePath;
    LogfilePath << LogDir << "/" << ::basename(argv[0]); // 完整的日志文件路径
    AsyncLogging log(LogfilePath.str(), kRollSize);//创建日志对象
    g_asyncLog = &log;
    Logger::setOutput(asyncLog); // 为Logger设置输出回调, 重新配接输出位置
    log.start(); // 开启日志后端线程
    ///////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////
    //第二步启动内存池和LFU缓存
     // 初始化内存池
    memoryPool::HashBucket::initMemoryPool();

    // 初始化缓存
    const int CAPACITY = 5;  
    Cache::KLfuCache<int, std::string> lfu(CAPACITY);
    ///////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////
    //第三步启动底层网络模块
    EventLoop loop;
    uint16_t port = getAgentDemoConfig().port;
    InetAddress addr(port);

    // 原有 TCP 文本 Agent 继续监听配置中的端口（默认 18080）。
    AgentServer server(&loop, addr, "AgentServer");

    /*
     * HTTP 学习服务独立监听 18081，与 TCP Agent 共用同一个 base EventLoop，
     * 但 HttpServer 内部拥有自己的 TcpServer 和 IO 线程池。两个协议互不干扰，
     * 便于在学习 HTTP 时继续保留原有 Agent 演示。
     *
     * 【线程模型】
     * main 函数中的 loop 是 baseLoop，负责两个监听 Socket 的接入事件；每个 TcpServer
     * 又创建自己的 subLoop 线程处理已连接 Socket。这仍然符合主从 Reactor：
     * main Reactor 负责 accept，sub Reactor 负责连接上的读写事件。
     */
    InetAddress httpAddr(18081);
    HttpServer httpServer(&loop, httpAddr, "HttpServer");
    httpServer.setThreadNum(2);
    httpServer.setHttpCallback(onHttpRequest);
    server.start();
    httpServer.start();
    std::cout << "=============================================Start Agent Demo Server=============================================" << std::endl;
    std::cout << "Plain text long-connection chat on port " << port << std::endl;
    std::cout << "Connect with: nc 127.0.0.1 " << port << std::endl;
    std::cout << "Commands: /health /clear /quit" << std::endl;
    std::cout << "Config file: " << getAgentDemoConfig().configPath << std::endl;
    std::cout << "HTTP health check: curl http://127.0.0.1:18081/health" << std::endl;
    loop.loop();
    std::cout << "==============================================Stop Agent Demo Server==============================================" << std::endl;
}
/*
cd /mnt/f/webserver/webserver
cmake --build build
cd bin
./main
*/
/*
nc 127.0.0.1 18080
*/
