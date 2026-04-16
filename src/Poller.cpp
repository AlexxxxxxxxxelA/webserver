#include "mywebserver/Poller.h"
#include "mywebserver/Channel.h"

Poller::Poller(EventLoop *loop)
    : ownerLoop_(loop)
{
}

bool Poller::hasChannel(Channel *channel) const
{
    /*
    它判断某个 Channel 是否已经在当前 Poller 里。
    逻辑是：
    先用 channel->fd() 去 channels_ 里查找
    如果找到了，再判断 map 里保存的 Channel* 是否就是传入的这个 channel
    它不仅判断 fd 存不存在，还判断对应的对象是不是同一个。
    */
    auto it = channels_.find(channel->fd());
    return it != channels_.end() && it->second == channel;
}