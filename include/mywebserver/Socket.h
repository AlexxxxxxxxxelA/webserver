#pragma once

#include "mywebserver/noncopyable.h"


/*
Socket 是对 socket 文件描述符的 C++ 封装。它内部保存一个 sockfd_，对外提供 bindAddress()、listen()、accept()、shutdownWrite() 和一些 socket 选项设置函数。它的析构函数会自动 close(sockfd_)，避免文件描述符泄漏。
*/
class InetAddress;

// 封装socket fd
class Socket : noncopyable
{
public:
    explicit Socket(int sockfd)
        : sockfd_(sockfd)
    {
    }
    ~Socket();

    int fd() const { return sockfd_; }
    void bindAddress(const InetAddress &localaddr);//绑定本地地址
    void listen();
    int accept(InetAddress *peeraddr);

    void shutdownWrite();

    void setTcpNoDelay(bool on);
    void setReuseAddr(bool on);
    void setReusePort(bool on);
    void setKeepAlive(bool on);

private:
    const int sockfd_;
};
