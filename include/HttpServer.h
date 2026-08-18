#pragma once

#include <functional>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "HttpContext.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "TcpServer.h"

/**
 * 建立在 TcpServer 之上的 HTTP 协议层。
 *
 * 分层关系：
 *     epoll/EventLoop -> TcpServer/TcpConnection -> HttpServer -> 业务回调
 *
 * TcpServer 负责 accept、线程分发和字节收发；HttpServer 把字节解析为
 * HttpRequest，再把业务生成的 HttpResponse 序列化后交回 TcpConnection。
 * HttpServer 不直接操作 epoll，因此现有 Reactor 网络层无需为 HTTP 重写。
 *
 * 【基础知识：网络分层】
 * 本项目可粗略分成：
 * - IO 多路复用层：epoll/Poller；
 * - 事件分发层：EventLoop/Channel；
 * - TCP 连接层：Acceptor/TcpServer/TcpConnection/Buffer；
 * - HTTP 协议层：HttpServer/HttpContext/HttpRequest/HttpResponse；
 * - 业务层：main.cc 中的 onHttpRequest，后续也可以是 AgentService。
 * 上层依赖下层提供能力，下层不应该反过来知道具体业务。
 *
 * 【常见面试问题】为什么不把 HTTP 解析直接写进 TcpConnection？
 * TcpConnection 应该是通用 TCP 抽象，它既能承载 HTTP，也能承载当前 18080 端口
 * 的按行文本 Agent 协议。若写死 HTTP，TcpConnection 就无法复用于其他应用协议。
 *
 * 【常见面试问题】什么是回调函数？
 * HttpServer 不知道每个项目有哪些路由，因此保存一个 std::function。请求解析完成
 * 后调用该函数，把“什么时候调用”交给框架，把“具体做什么”交给业务层。这就是
 * 控制反转的一种简单形式。
 */
class HttpServer
{
public:
    // 业务层读取 request，并通过 response 指针填写响应内容。
    using HttpCallback = std::function<void(const HttpRequest &, HttpResponse *)>;
    /**
     * responder 是异步业务的“完成通知”。业务线程不能保存栈上的 HttpResponse*，
     * 而是新建一个 HttpResponse，按值交给 responder，由框架负责连接检查和发送。
     */
    using AsyncHttpResponder = std::function<void(HttpResponse)>;
    /**
     * 异步回调只处理需要后台执行的路由。返回 true 表示已经接管该请求，并会在
     * 将来调用 responder；返回 false 表示交给普通 HttpCallback 同步处理。
     *
     * 当前学习版对异步路由采用“一连接一请求，响应后关闭”的明确限制。这样不必
     * 在第一版就实现复杂的 HTTP Pipeline 异步响应排序。
     *
     * 【常见面试问题】为什么异步 HTTP 需要响应排序？
     * 同一连接若先收到慢 Agent、再收到快 health，后者可能先完成。但 HTTP/1.1
     * Pipeline 要求响应顺序与请求顺序一致。完整实现需要 request-id 和响应队列；
     * 当前明确关闭异步连接，是控制学习复杂度的取舍。
     */
    using AsyncHttpCallback =
        std::function<bool(const HttpRequest &, const AsyncHttpResponder &)>;

    HttpServer(EventLoop *loop,
               const InetAddress &listenAddress,
               const std::string &name);

    void setHttpCallback(const HttpCallback &callback) { httpCallback_ = callback; }
    void setAsyncHttpCallback(const AsyncHttpCallback &callback) { asyncHttpCallback_ = callback; }
    void setThreadNum(int numThreads) { server_.setThreadNum(numThreads); }
    void start() { server_.start(); }

private:
    // TcpServer 的连接回调：为每条新连接创建独立解析上下文，断开时删除。
    void onConnection(const TcpConnectionPtr &connection);
    // TcpServer 的消息回调：驱动状态机，可能一次解析零条、一条或多条请求。
    void onMessage(const TcpConnectionPtr &connection, Buffer *buffer, Timestamp receiveTime);
    // 调用业务回调并发送一条完整 HTTP Response。
    // 返回 true 表示请求已异步接管，当前连接不能继续解析后续 Pipeline 请求。
    bool onRequest(const TcpConnectionPtr &connection, const HttpRequest &request);
    void sendError(const TcpConnectionPtr &connection, int statusCode,
                   const std::string &statusMessage, const std::string &body);
    // 根据 HTTP 版本和 Connection Header 判断本次响应后是否关闭连接。
    bool shouldClose(const HttpRequest &request) const;

    HttpCallback httpCallback_; // 例如 main.cc 中的 /health 处理函数
    AsyncHttpCallback asyncHttpCallback_;
    // 多个 subLoop 线程可能同时建立/关闭 HTTP 连接，因此映射本身需要加锁。
    std::mutex contextsMutex_;
    // key 使用 TcpConnection 唯一名称；value 持有该连接当前解析进度。
    std::unordered_map<std::string, std::shared_ptr<HttpContext>> contexts_;
    // 异步请求执行期间继续监听 EPOLLIN 以便读到 EOF，但丢弃后续请求字节。
    std::unordered_set<std::string> deferredConnections_;
    /**
     * 声明在最后，使其最先析构并停止网络回调。
     *
     * 【C++ 基础知识：成员析构顺序】
     * 成员按“声明顺序的逆序”析构，而不是按构造函数初始化列表的书写顺序。
     * TcpServer 的回调会访问 contexts_ 和 contextsMutex_，所以必须先停止 TcpServer，
     * 再销毁这些被回调访问的成员，否则可能发生 use-after-free。
     */
    TcpServer server_;
};
