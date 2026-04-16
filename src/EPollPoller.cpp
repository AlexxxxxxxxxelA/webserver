#include <errno.h>
#include <unistd.h>
#include <string.h>

#include "mywebserver/EPollPoller.h"
#include "mywebserver/Channel.h"

const int kNew = -1;    // 某个channel还没添加至Poller          // channel的成员index_初始化为-1
const int kAdded = 1;   // 某个channel已经添加至Poller
const int kDeleted = 2; // 某个channel已经从Poller删除

EPollPoller::EPollPoller(EventLoop *loop)
    : Poller(loop)
    , epollfd_(::epoll_create1(EPOLL_CLOEXEC)) 
    , events_(kInitEventListSize) // vector<epoll_event>(16)
{
    if (epollfd_ < 0)
    {
        std::cout << "epoll_create error: " << errno << '\n';
    }
}

EPollPoller::~EPollPoller()
{
    ::close(epollfd_);
}

Timestamp EPollPoller::poll(int timeoutMs, ChannelList *activeChannels)
{
    /*
    没有事件时，当前 loop 线程阻塞在这里
    有事件时，epoll_wait 返回
    返回值 numEvents 表示这次有多少个 fd 活跃了
    */
    int numEvents = ::epoll_wait(epollfd_, &*events_.begin(), static_cast<int>(events_.size()), timeoutMs);
    int saveErrno = errno;
    Timestamp now(Timestamp::now());

    if (numEvents > 0)
    {
        fillActiveChannels(numEvents, activeChannels);
        if (numEvents == events_.size()) // 扩容操作
        {
            events_.resize(events_.size() * 2);
        }
    }
    else if (numEvents == 0)
    {
        // timeout is normal for an idle event loop.
    }
    else
    {
        if (saveErrno != EINTR)
        {
            errno = saveErrno;
            std::cout << "EPollPoller::poll() error: " << errno << '\n';
        }
    }
    return now;
}

// channel update remove => EventLoop updateChannel removeChannel => Poller updateChannel removeChannel
/*
根据 Channel 当前在不在 epoll 里，以及它当前关心的事件是否为空，决定是 ADD、MOD 还是 DEL。
*/
void EPollPoller::updateChannel(Channel *channel)
{
    /*
    kNew = -1：这个 channel 还没加进 poller
    kAdded = 1：已经在 poller 里
    kDeleted = 2：之前加过，但现在从 epoll 里删掉了
    */
    const int index = channel->index();
    if (index == kNew || index == kDeleted)
    {
        if (index == kNew)
        {
            int fd = channel->fd();
            channels_[fd] = channel;
        }
        else // index == kDeleted
        {
        }
        channel->set_index(kAdded);
        update(EPOLL_CTL_ADD, channel);
    }
    else // channel已经在Poller中注册过了
    {
        int fd = channel->fd();
        if (channel->isNoneEvent())
        {
            update(EPOLL_CTL_DEL, channel);
            channel->set_index(kDeleted);
        }
        else
        {
            update(EPOLL_CTL_MOD, channel);
        }
    }
}

// 从Poller中删除channel
void EPollPoller::removeChannel(Channel *channel)
{
    int fd = channel->fd();
    channels_.erase(fd);

    int index = channel->index();
    if (index == kAdded)
    {
        update(EPOLL_CTL_DEL, channel);
    }
    channel->set_index(kNew);
}

// 填写活跃的连接
/*
epoll_wait() 只知道“有事件了”
EPollPoller 通过 data.ptr 直接拿回对应的 Channel*
再把“这次实际发生了什么事件”写进 Channel::revents_
然后交给 EventLoop
*/
void EPollPoller::fillActiveChannels(int numEvents, ChannelList *activeChannels) const
{
    for (int i = 0; i < numEvents; ++i)
    {
        Channel *channel = static_cast<Channel *>(events_[i].data.ptr);
        channel->set_revents(events_[i].events);
        activeChannels->push_back(channel); // EventLoop就拿到了它的Poller给它返回的所有发生事件的channel列表了
    }
}

// 更新channel通道 其实就是调用epoll_ctl add/mod/del
void EPollPoller::update(int operation, Channel *channel)
{
    epoll_event event;
    ::memset(&event, 0, sizeof(event));

    int fd = channel->fd();

    event.events = channel->events();
    event.data.fd = fd;
    event.data.ptr = channel;

    if (::epoll_ctl(epollfd_, operation, fd, &event) < 0)//通过 Channel.events_ 注册给 epoll 感兴趣事件
    {
        if (operation == EPOLL_CTL_DEL)
        {
            std::cout << "epoll_ctl del error: " << errno << '\n';
        }
        else
        {
            std::cout << "epoll_ctl add/mod error: " << errno << '\n';
        }
    }
}
