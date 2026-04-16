#pragma once

#include <vector>
#include <unordered_map>

#include "noncopyable.h"
#include "Timestamp.h"

class Channel;
class EventLoop;

// muduo库中多路事件分发器的核心IO复用模块
class Poller
{
public:
    using ChannelList = std::vector<Channel *>;
    /*
    ChannelList 是活跃 Channel 列表。
    Poller::poll() 等到事件后，会把本轮有事件发生的 Channel* 放进这个 vector 里，然后交给 EventLoop 处理。
    */

    Poller(EventLoop *loop);
    virtual ~Poller() = default;

    // 给所有IO复用保留统一的接口
    virtual Timestamp poll(int timeoutMs, ChannelList *activeChannels) = 0;
    virtual void updateChannel(Channel *channel) = 0;//用于更新某个 Channel 关心的事件。
    virtual void removeChannel(Channel *channel) = 0;//它用于把一个 Channel 从 Poller 中移除。比如连接关闭后，对应的 fd 不应该再被 epoll 监听，就需要调用 removeChannel()。

    // 判断参数channel是否在当前的Poller当中
    bool hasChannel(Channel *channel) const;

    // EventLoop可以通过该接口获取默认的IO复用的具体实现
    static Poller *newDefaultPoller(EventLoop *loop);

protected:
    // map的key:sockfd value:sockfd所属的channel通道类型
    /*
    它维护的是：
    fd → Channel* 的映射。
    */
    using ChannelMap = std::unordered_map<int, Channel *>;
    ChannelMap channels_;

private:
    //一个 Poller 只属于一个 EventLoop
    EventLoop *ownerLoop_; // 定义Poller所属的事件循环EventLoop
};