#include <HttpServer.h>

#include <Buffer.h>
#include <Logger.h>
#include <TcpConnection.h>

#include <algorithm>
#include <cctype>

namespace
{

// Connection Header 的 token 不区分大小写，先统一转为小写再判断。
std::string toLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

} // namespace

HttpServer::HttpServer(EventLoop *loop,
                       const InetAddress &listenAddress,
                       const std::string &name)
    : server_(loop, listenAddress, name)
{
    /*
     * HttpServer 本身不创建 Socket，它组合一个已有的 TcpServer：
     * - 连接建立/断开时调用 onConnection；
     * - TcpConnection 读到字节时调用 onMessage。
     * 这样 HTTP 是独立的应用层协议，底层 Reactor 不需要知道 HTTP 的存在。
     *
     * 【常见面试问题】组合和继承如何选择？
     * HttpServer 不是一种 TcpServer，而是“使用一个 TcpServer 提供 HTTP 服务”，
     * 因此这里采用组合。组合通常比继承耦合更低，也更容易替换内部实现。
     */
    server_.setConnectionCallback(
        std::bind(&HttpServer::onConnection, this, std::placeholders::_1));
    server_.setMessageCallback(
        std::bind(&HttpServer::onMessage, this, std::placeholders::_1,
                  std::placeholders::_2, std::placeholders::_3));
}

void HttpServer::onConnection(const TcpConnectionPtr &connection)
{
    /*
     * 一条 TCP 连接只能由它所属的 EventLoop 线程处理消息，但多个连接可能分布
     * 在不同 subLoop。因此每条连接有独立 HttpContext，contexts_ 映射的增删
     * 需要 mutex 保护。锁只保护映射，不包围真正的 HTTP 解析。
     *
     * 【常见面试问题】既然 one loop per thread，为什么还要加锁？
     * 单条连接只在所属 loop 线程处理，所以该连接自己的 HttpContext 无需加锁；
     * 但 contexts_ 是所有连接共享的容器，不同 subLoop 会同时插入、查找和删除，
     * 因此共享容器仍需互斥保护。这体现了“只给真正共享的数据加锁”。
     */
    std::lock_guard<std::mutex> lock(contextsMutex_);
    if (connection->connected())
    {
        contexts_[connection->name()] = std::make_shared<HttpContext>();
        LOG_INFO << "HTTP connection UP: " << connection->peerAddress().toIpPort().c_str();
    }
    else
    {
        contexts_.erase(connection->name());
        LOG_INFO << "HTTP connection DOWN: " << connection->peerAddress().toIpPort().c_str();
    }
}

void HttpServer::onMessage(const TcpConnectionPtr &connection, Buffer *buffer, Timestamp receiveTime)
{
    (void)receiveTime;

    // shutdown() 后连接进入 kDisconnecting；丢弃对端后续数据，避免 Buffer 堆积。
    if (!connection->connected())
    {
        buffer->retrieveAll();
        return;
    }

    std::shared_ptr<HttpContext> context;
    {
        // 只在查找 shared_ptr 时持锁，解析过程不持有全局锁。
        std::lock_guard<std::mutex> lock(contextsMutex_);
        auto it = contexts_.find(connection->name());
        if (it == contexts_.end())
        {
            return;
        }
        context = it->second;
    }

    while (connection->connected())
    {
        /*
         * 一次 onMessage 可能出现：
         * - 半条请求：parseRequest 返回 kIncomplete，等待下次 EPOLLIN；
         * - 一条请求：处理后 Buffer 为空，返回；
         * - 多条请求粘在一起：reset 后循环继续解析 Buffer 中下一条。
         *
         * 【Reactor 概念】
         * Reactor 的核心是“事件到来后分发回调”：epoll 告诉 EventLoop 哪些 fd 就绪，
         * Channel 调用 TcpConnection::handleRead，最后进入本函数。HTTP 解析不是 epoll
         * 的职责，而是读回调中的应用层工作。
         */
        HttpContext::ParseResult result = context->parseRequest(buffer);
        if (result == HttpContext::kIncomplete)
        {
            return;
        }
        if (result == HttpContext::kBadRequest)
        {
            sendError(connection, 400, "Bad Request", "Bad Request\n");
            return;
        }
        if (result == HttpContext::kRequestTooLarge)
        {
            sendError(connection, 413, "Payload Too Large", "Payload Too Large\n");
            return;
        }
        if (result == HttpContext::kNotImplemented)
        {
            sendError(connection, 501, "Not Implemented", "Transfer-Encoding is not supported\n");
            return;
        }

        // 只有 kComplete 才能把 request 交给业务层，错误请求不会进入路由。
        onRequest(connection, context->request());
        const bool close = shouldClose(context->request());

        // request 交给业务层使用完以后再 reset，不能提前清空。
        context->reset();
        if (close || buffer->readableBytes() == 0)
        {
            return;
        }
    }
}

void HttpServer::onRequest(const TcpConnectionPtr &connection, const HttpRequest &request)
{
    // 连接策略来源于请求版本和 Connection Header，业务回调不能改变它。
    const bool close = shouldClose(request);
    HttpResponse response(close);

    // 上层回调只关心路由和响应内容，不接触 TcpConnection 或解析状态机。
    if (httpCallback_)
    {
        httpCallback_(request, &response);
    }
    else
    {
        response.setStatusCode(404);
        response.setStatusMessage("Not Found");
        response.setContentType("text/plain; charset=utf-8");
        response.setBody("Not Found\n");
    }
    response.setCloseConnection(close);

    // 先序列化成连续字节，再复用 TcpConnection 已有的非阻塞发送机制。
    Buffer output;
    response.appendToBuffer(&output);
    connection->send(output.retrieveAllAsString());
    if (response.closeConnection())
    {
        // shutdown() 会等待 outputBuffer_ 发送完成后再关闭写端，不会截断响应。
        /*
         * 【基础知识：shutdown 和 close 的区别】
         * shutdown(SHUT_WR) 是半关闭：本端不再发送，但仍可接收对方剩余数据；
         * close 释放整个文件描述符。当前 TcpConnection::shutdown() 会先等输出缓冲区
         * 发送完成，再调用 shutdownWrite，因此 HTTP 响应不会在中途被截断。
         */
        connection->shutdown();
    }
}

void HttpServer::sendError(const TcpConnectionPtr &connection, int statusCode,
                           const std::string &statusMessage, const std::string &body)
{
    // 解析错误后无法可靠判断后续字节边界，所以发送错误响应后统一关闭连接。
    HttpResponse response(true);
    response.setStatusCode(statusCode);
    response.setStatusMessage(statusMessage);
    response.setContentType("text/plain; charset=utf-8");
    response.setBody(body);
    Buffer output;
    response.appendToBuffer(&output);
    connection->send(output.retrieveAllAsString());
    connection->shutdown();
}

bool HttpServer::shouldClose(const HttpRequest &request) const
{
    const std::string connection = toLower(request.getHeader("Connection"));

    /*
     * Connection 的值是逗号分隔 token 列表，不能只与整个字符串做相等比较：
     *     Connection: keep-alive, close
     * 仍然必须识别其中的 close。hasToken 同时忽略 token 两侧空格。
     *
     * 【常见面试问题】HTTP/1.0 与 HTTP/1.1 长连接默认行为有何不同？
     * HTTP/1.0 默认每个响应后关闭，只有 Connection: keep-alive 才保留；
     * HTTP/1.1 默认保留，只有 Connection: close 才关闭。
     */
    const auto hasToken = [&connection](const std::string &expected) {
        size_t begin = 0;
        while (begin < connection.size())
        {
            size_t end = connection.find(',', begin);
            if (end == std::string::npos)
            {
                end = connection.size();
            }
            size_t first = connection.find_first_not_of(" \t", begin);
            size_t last = connection.find_last_not_of(" \t", end - 1);
            if (first != std::string::npos && first < end && last != std::string::npos &&
                connection.substr(first, last - first + 1) == expected)
            {
                return true;
            }
            begin = end + 1;
        }
        return false;
    };
    if (request.version() == HttpRequest::kHttp10)
    {
        // HTTP/1.0 默认关闭，仅显式 keep-alive 才复用连接。
        return !hasToken("keep-alive");
    }
    // HTTP/1.1 默认 Keep-Alive，仅显式 close 才关闭。
    return hasToken("close");
}
