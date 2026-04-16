#pragma once

/**
 * 用户使用muduo编写服务器程序
 **/

#include <functional>
#include <string>
#include <memory>
#include <atomic>
#include <unordered_map>

#include "EventLoop.h"
#include "Acceptor.h"
#include "InetAddress.h"
#include "noncopyable.h"
#include "EventLoopThreadPool.h"
#include "Callbacks.h"
#include "TcpConnection.h"
#include "Buffer.h"

// 对外的服务器编程使用的类
class TcpServer
{
public:
    using ThreadInitCallback = std::function<void(EventLoop *)>;

    enum Option
    {
        kNoReusePort,//不允许重用本地端口
        kReusePort,//允许重用本地端口
    };

    TcpServer(EventLoop *loop,
              const InetAddress &listenAddr,
              const std::string &nameArg,
              Option option = kNoReusePort);
    ~TcpServer();

    void setThreadInitCallback(const ThreadInitCallback &cb) { threadInitCallback_ = cb; }
    void setConnectionCallback(const ConnectionCallback &cb) { connectionCallback_ = cb; }
    void setMessageCallback(const MessageCallback &cb) { messageCallback_ = cb; }//把传进来的message先保存在 TcpServer::messageCallback_ 这个成员里
    void setWriteCompleteCallback(const WriteCompleteCallback &cb) { writeCompleteCallback_ = cb; }

    // 设置底层subloop的个数
    void setThreadNum(int numThreads);
    /**
     * 如果没有监听, 就启动服务器(监听).
     * 多次调用没有副作用.
     * 线程安全.
     */
    void start();

private:
    void newConnection(int sockfd, const InetAddress &peerAddr);
    void removeConnection(const TcpConnectionPtr &conn);
    void removeConnectionInLoop(const TcpConnectionPtr &conn);

    using ConnectionMap = std::unordered_map<std::string, TcpConnectionPtr>;

    EventLoop *loop_; // baseloop 用户自定义的loop

    const std::string ipPort_;//保存监听地址字符串，比如 127.0.0.1:8080
    const std::string name_;//服务器名字，比如 EchoServer


    /*
    acceptor_ 用 unique_ptr 很合理
    因为它明显是 TcpServer 独占拥有 的对象：
    在构造函数里 new Acceptor(...)
    只由 TcpServer 持有
    只在 TcpServer 里调用，比如 acceptor_->setNewConnectionCallback(...)、acceptor_.get() 去 listen()
    所以它的语义就是：
    这个 Acceptor 只属于这个 TcpServer，不会和别人共享。
    */
    std::unique_ptr<Acceptor> acceptor_; // 运行在mainloop 任务就是监听新连接事件，
    //当 listenfd 上有可读事件，就说明有新连接来了，Acceptor 就去 accept()。Acceptor 本身只负责“接”，接到后再通知 TcpServer。

    std::shared_ptr<EventLoopThreadPool> threadPool_; // one loop per thread，一个线程跑一个线程池

    ConnectionCallback connectionCallback_;       //有新连接时的回调
    MessageCallback messageCallback_;             // 有读写事件发生时的回调
    WriteCompleteCallback writeCompleteCallback_; // 消息发送完成后的回调

    ThreadInitCallback threadInitCallback_; // loop线程初始化的回调
    int numThreads_;//线程池中线程的数量。
    std::atomic_int started_;//原子变量，防止 start() 被重复启动
    int nextConnId_;//给新连接生成唯一编号
    ConnectionMap connections_; // 保存所有的连接
};