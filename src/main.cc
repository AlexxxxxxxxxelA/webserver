#include <string>

#include <TcpServer.h>
#include <Logger.h>
#include <sys/stat.h>
#include <signal.h>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
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
    AgentServer(EventLoop *loop, const InetAddress &addr, const std::string &name,
                AgentDemoService &service)
        : service_(service)
        , server_(loop, addr, name)
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
    // server_ 声明在最后，因此最先析构，停止回调后外部 service 才会销毁。
    AgentDemoService &service_;
    TcpServer server_;
};

std::string lowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

HttpResponse jsonError(int statusCode, const std::string &statusMessage,
                       const std::string &message)
{
    // 使用 JSON 库生成响应，自动处理引号、换行和 Unicode 转义。
    HttpResponse response(true);
    response.setStatusCode(statusCode);
    response.setStatusMessage(statusMessage);
    response.setContentType("application/json; charset=utf-8");
    nlohmann::json body;
    body["error"] = message;
    response.setBody(body.dump() + "\n");
    return response;
}

bool onAsyncHttpRequest(AgentDemoService &service,
                        const HttpRequest &request,
                        const HttpServer::AsyncHttpResponder &responder)
{
    /*
     * 本函数仍运行在 HTTP IO 线程，只做路由、Content-Type、JSON 和长度校验。
     * service.submit() 之后的 DeepSeek/工具流程在业务线程执行。
     */
    if (request.path() != "/agent/run")
    {
        return false;
    }

    if (request.method() != HttpRequest::kPost)
    {
        HttpResponse response = jsonError(405, "Method Not Allowed", "method not allowed");
        response.addHeader("Allow", "POST");
        responder(response);
        return true;
    }

    std::string contentType = lowerCopy(request.getHeader("Content-Type"));
    const size_t semicolon = contentType.find(';');
    std::string mediaType = contentType.substr(0, semicolon);
    while (!mediaType.empty() && std::isspace(static_cast<unsigned char>(mediaType.back())))
    {
        mediaType.pop_back();
    }
    size_t first = 0;
    while (first < mediaType.size() &&
           std::isspace(static_cast<unsigned char>(mediaType[first])))
    {
        ++first;
    }
    mediaType.erase(0, first);
    if (mediaType != "application/json")
    {
        responder(jsonError(415, "Unsupported Media Type", "Content-Type must be application/json"));
        return true;
    }

    std::string sessionId;
    std::string message;
    try
    {
        // 正式 parser 会拒绝非法 JSON，并正确还原 \uXXXX 等 Unicode 转义。
        const nlohmann::json body = nlohmann::json::parse(request.body());
        if (!body.is_object() || !body.contains("session_id") ||
            !body.contains("message") || !body["session_id"].is_string() ||
            !body["message"].is_string())
        {
            throw std::invalid_argument("required string field is missing");
        }
        sessionId = body["session_id"].get<std::string>();
        message = body["message"].get<std::string>();
    }
    catch (const std::exception &)
    {
        responder(jsonError(400, "Bad Request", "JSON requires string session_id and message"));
        return true;
    }

    AgentDemoService::SubmitStatus status = service.submit(
        std::string("http:") + sessionId, message,
        [sessionId, responder](AgentDemoService::AgentResult result) {
            // 这个 lambda 在 worker 线程执行；responder 内部负责连接生命周期。
            if (!result.ok())
            {
                if (result.error == AgentDemoService::AgentResult::kNotConfigured)
                {
                    responder(jsonError(503, "Service Unavailable", result.errorMessage));
                }
                else if (result.error == AgentDemoService::AgentResult::kUpstreamTimeout)
                {
                    responder(jsonError(504, "Gateway Timeout", result.errorMessage));
                }
                else if (result.error == AgentDemoService::AgentResult::kUpstreamError)
                {
                    responder(jsonError(502, "Bad Gateway", result.errorMessage));
                }
                else
                {
                    responder(jsonError(500, "Internal Server Error", result.errorMessage));
                }
                return;
            }

            HttpResponse response(true);
            response.setStatusCode(200);
            response.setStatusMessage("OK");
            response.setContentType("application/json; charset=utf-8");
            nlohmann::json body;
            body["session_id"] = sessionId;
            body["answer"] = result.answer;
            if (!result.toolName.empty() && result.toolName != "none")
            {
                body["tool"] = result.toolName;
                body["tool_result"] = result.toolResult;
            }
            response.setBody(body.dump() + "\n");
            responder(response);
        });

    if (status == AgentDemoService::kAccepted)
    {
        return true;
    }
    if (status == AgentDemoService::kSessionBusy)
    {
        responder(jsonError(409, "Conflict", "this session is already processing a request"));
    }
    else if (status == AgentDemoService::kQueueFull)
    {
        // Retry-After 告诉客户端这是暂时过载，而不是请求永久无效。
        HttpResponse response = jsonError(503, "Service Unavailable", "agent task queue is full");
        response.addHeader("Retry-After", "1");
        responder(response);
    }
    else
    {
        responder(jsonError(400, "Bad Request", "invalid session_id or message"));
    }
    return true;
}

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

    if (request.method() == HttpRequest::kPost && request.path() == "/echo")
    {
        // /echo 不调用 Agent，只原样返回 Body，用于独立验证 POST 和 Content-Length。
        response->setStatusCode(200);
        response->setStatusMessage("OK");
        response->setContentType("text/plain; charset=utf-8");
        response->setBody(request.body());
        return;
    }

    if (request.path() == "/echo")
    {
        response->setStatusCode(405);
        response->setStatusMessage("Method Not Allowed");
        response->addHeader("Allow", "POST");
        response->setBody("{\"error\":\"method not allowed\"}\n");
        return;
    }

    // 正常情况下 /agent/run 已由异步回调接管；这里保留方法错误的同步兜底。
    if (request.path() == "/agent/run")
    {
        response->setStatusCode(405);
        response->setStatusMessage("Method Not Allowed");
        response->addHeader("Allow", "POST");
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
    (void)argc;
    // libcurl 要求全局初始化先于可能调用它的所有线程，因此必须放在日志线程之前。
    initializeAgentRuntime();

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
    AgentDemoService agentService;
    AgentServer server(&loop, addr, "AgentServer", agentService);

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
    httpServer.setAsyncHttpCallback(
        std::bind(onAsyncHttpRequest, std::ref(agentService),
                  std::placeholders::_1, std::placeholders::_2));
    server.start();
    httpServer.start();
    std::cout << "=============================================Start Agent Demo Server=============================================" << std::endl;
    std::cout << "Plain text long-connection chat on port " << port << std::endl;
    std::cout << "Connect with: nc 127.0.0.1 " << port << std::endl;
    std::cout << "Commands: /health /clear /quit" << std::endl;
    std::cout << "Config file: " << getAgentDemoConfig().configPath << std::endl;
    std::cout << "HTTP health check: curl http://127.0.0.1:18081/health" << std::endl;
    std::cout << "HTTP Agent API: POST http://127.0.0.1:18081/agent/run" << std::endl;
    loop.loop();
    // 必须先等待业务任务结束，再让 HttpServer/AgentServer 析构其 IO loop 和连接。
    agentService.stop();
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
