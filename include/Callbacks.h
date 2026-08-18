#pragma once
/*
一些回调函数
*/
#include <memory>
#include <functional>

class Buffer;
class TcpConnection;
class Timestamp;

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
using ConnectionCallback = std::function<void(const TcpConnectionPtr &)>;

using CloseCallback = std::function<void(const TcpConnectionPtr &)>;
using WriteCompleteCallback = std::function<void(const TcpConnectionPtr &)>;
using HighWaterMarkCallback = std::function<void(const TcpConnectionPtr &, size_t)>;

using MessageCallback = std::function<void(const TcpConnectionPtr &,
                                           Buffer *,
                                           Timestamp)>;
/*
消息回调将来被触发时，会拿到 3 个参数：
当前这条连接 TcpConnectionPtr
读缓冲区 Buffer*
收到消息的时间 Timestamp
*/