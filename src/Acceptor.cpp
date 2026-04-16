#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <unistd.h>
#include "mywebserver/Acceptor.h"
#include "mywebserver/InetAddress.h"

static int createNonblocking()
{
    int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    //创建一个 TCP 监听 socket，并且一开始就设置成非阻塞 + close-on-exec。
    //sockfd：一个新创建的 socket 对象在当前进程里的文件描述符编号（fd）
    //内核创建了一个 socket 对象，并在当前进程的 fd 表里加了一项，让这个整数指向它。
    //所以这个 sockfd 本质上就是：当前进程访问这个 socket 内核对象的索引 / 句柄
    /*
    AF_INET：表示 IPv4。
    SOCK_STREAM：表示 TCP 字节流 socket。
    SOCK_NONBLOCK：表示这个 socket 是非阻塞的。
    SOCK_CLOEXEC：如果以后这个进程执行 exec 系列函数，这个 fd 会自动关闭，避免 fd 泄漏到新程序里。
    */
    if (sockfd < 0)
    {
        std::cout << "listen socket create error: " << errno << '\n';
    }
    return sockfd;
}

Acceptor::Acceptor(EventLoop *loop, const InetAddress &listenAddr, bool reuseport)
    : loop_(loop)//这里传进来的就是 TcpServer 构造时给它的 mainLoop。
    , acceptSocket_(createNonblocking())//创建监听 socket
    , acceptChannel_(loop, acceptSocket_.fd())//给这个 socket 创建对应的 Channel,这里传进去要专注的fd
    , listening_(false)//初始化未监听状态
{
    acceptSocket_.setReuseAddr(true);//地址快速复用
    acceptSocket_.setReusePort(true);//端口复用
    acceptSocket_.bindAddress(listenAddr);//这一步就是把监听 socket 绑定到：127.0.0.1:8080
    //

    // TcpServer::start() => Acceptor.listen() 如果有新用户连接 要执行一个回调(accept => connfd => 打包成Channel => 唤醒subloop)
    // baseloop监听到有事件发生 => acceptChannel_(listenfd) => 执行该回调函数
    acceptChannel_.setReadCallback(//以后如果监听 socket 对应的 Channel 发生了“读事件”，就调用 Acceptor::handleRead()。
        std::bind(&Acceptor::handleRead, this));//把成员函数 Acceptor::handleRead 和当前对象 this 绑定在一起，包装成一个“以后可以直接调用的函数对象”。
}

Acceptor::~Acceptor()
{
    acceptChannel_.disableAll();    // 把从Poller中感兴趣的事件删除掉
    acceptChannel_.remove();        // 调用EventLoop->removeChannel => Poller->removeChannel 把Poller的ChannelMap对应的部分删除
}

void Acceptor::listen()//真正开始监听
{
    listening_ = true;
    acceptSocket_.listen();         // listen
    acceptChannel_.enableReading(); // acceptChannel_注册至Poller !重要
}

// listenfd有事件发生了，就是有新用户连接了
void Acceptor::handleRead()
{
    InetAddress peerAddr;
    int connfd = acceptSocket_.accept(&peerAddr);//这一步才是真正把新客户端连接从内核 accept 队列里取出来。
    if (connfd >= 0)
    {
        if (NewConnectionCallback_)
        {
            NewConnectionCallback_(connfd, peerAddr); // 轮询找到subLoop 唤醒并分发当前的新客户端的Channel
        }
        else
        {
            ::close(connfd);
        }
    }
    else//如果是 EMFILE，说明当前进程 fd 用满了
    {
        std::cout << "accept error: " << errno << '\n';
        if (errno == EMFILE)
        {
            std::cout << "sockfd reached limit\n";
        }
    }
}
