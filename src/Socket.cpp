#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <iostream>

#include "mywebserver/Socket.h"
//#include <Logger.h>
#include "mywebserver/InetAddress.h"

Socket::~Socket()
{
    ::close(sockfd_);
}

//绑定本地地址
void Socket::bindAddress(const InetAddress &localaddr)
{
    if (0 != ::bind(sockfd_, (sockaddr *)localaddr.getSockAddr(), sizeof(sockaddr_in)))
    {
        //LOG_FATAL<<"bind sockfd:"<<sockfd_ <<"fail";
        std::cout << "bind sockfd: " << sockfd_ << " fail\n";
    }
}
//把这个 socket 从普通 socket 变成 监听 socket
//也就是说，这个 fd 开始用于接收客户端连接请求。
void Socket::listen()
{
    if (0 != ::listen(sockfd_, 1024))
    {
        //LOG_FATAL<<"bind sockfd:"<<sockfd_ <<"fail";
        std::cout << "listen sockfd: " << sockfd_ << " fail\n";
    }
}

// peeraddr 是外部传进来的对象
// 这里把客户端地址拷贝到 peeraddr 里保存下来
//从监听 socket sockfd_ 上取出一个已经建立好的新连接，返回一个新的连接 fd：connfd
int Socket::accept(InetAddress *peeraddr)
{
    /**
     * 1. accept函数的参数不合法
     * 2. 对返回的connfd没有设置非阻塞
     * Reactor模型 one loop per thread
     * poller + non-blocking IO
     **/
    sockaddr_in addr;
    socklen_t len = sizeof(addr);
    ::memset(&addr, 0, sizeof(addr));
    //accept4() 可以在接收连接的同时设置 fd 标志：
    int connfd = ::accept4(sockfd_, (sockaddr *)&addr, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (connfd >= 0)
    {
        peeraddr->setSockAddr(addr);
        //std::cout <<peeraddr->toIpPort() << std::endl;
    }
    return connfd;
}

/*
半关闭有什么用
比如 HTTP 短连接、协议结束通知时：
我这边已经没有数据要发了
但还想继续接收对方剩余的数据
这时可以关闭写端，但保留读端。
*/
void Socket::shutdownWrite()
{
    if (::shutdown(sockfd_, SHUT_WR) < 0)
    {
        //LOG_ERROR<<"shutdownWrite error";
        std::cout << "shutdownWrite error\n";
    }
}

//////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////

void Socket::setTcpNoDelay(bool on)
{
    // TCP_NODELAY 用于禁用 Nagle 算法。
    // Nagle 算法用于减少网络上传输的小数据包数量。
    // 将 TCP_NODELAY 设置为 1 可以禁用该算法，允许小数据包立即发送。
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
}

void Socket::setReuseAddr(bool on)
{
    // SO_REUSEADDR 允许一个套接字强制绑定到一个已被其他套接字使用的端口。
    // 这对于需要重启并绑定到相同端口的服务器应用程序非常有用。
    
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
}

//这个选项常用于多进程或多线程服务，让多个 acceptor 共同监听同一个端口，内核负责在它们之间分配连接。
void Socket::setReusePort(bool on)
{
    // SO_REUSEPORT 允许同一主机上的多个套接字绑定到相同的端口号。
    // 这对于在多个线程或进程之间负载均衡传入连接非常有用。
   
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
}

void Socket::setKeepAlive(bool on)
{
    // SO_KEEPALIVE 启用在已连接的套接字上定期传输消息。
    // 如果另一端没有响应，则认为连接已断开并关闭。
    // 这对于检测网络中失效的对等方非常有用。
/*
开启 TCP keepalive。开启后，内核会在连接长时间空闲时发送探测包，用来判断对端是否还活着。

这对长连接服务比较有用，比如客户端异常断电、网络断开时，服务端可能无法立刻感知，keepalive 可以帮助最终发现死连接。
*/

    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval));
}
