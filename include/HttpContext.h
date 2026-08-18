#pragma once

#include <stddef.h>

#include "HttpRequest.h"

class Buffer;

/**
 * 单条 TCP 连接上的 HTTP 解析上下文。
 *
 * 为什么每条连接都需要自己的 HttpContext？
 * TCP 是字节流，一次 readv() 不保证正好读到一个完整 HTTP 请求。例如第一次
 * 可能只收到 "GET /heal"，第二次才收到剩余内容。解析器必须记住第一次解析到
 * 哪个阶段，下一次 EPOLLIN 到来时才能继续，而不能从头丢弃已有数据。
 *
 * 一个 Keep-Alive 连接还可能连续承载多条请求。完成一条请求后调用 reset()，
 * 剩余字节仍留在 Buffer 中，HttpServer 会继续解析下一条请求。
 *
 * 【基础知识：什么是有限状态机？】
 * 状态机把一个复杂过程拆成有限个阶段，每个阶段只处理自己关心的数据，并根据
 * 输入切换到下一状态。本类的状态转移如下：
 *
 *     kExpectRequestLine
 *              |
 *              v
 *       kExpectHeaders
 *          |       |
 *  无 Body |       | Content-Length > 0
 *          |       v
 *          |  kExpectBody
 *          |       |
 *          +-------+
 *              |
 *              v
 *           kGotAll
 *
 * 状态机的好处是：即使请求分多次到达，也能从上次状态继续，而不是每次重新解析。
 *
 * 【常见面试问题】半包和粘包分别是什么？
 * 半包：一次读取只获得一条应用层消息的一部分；
 * 粘包：一次读取获得了多条应用层消息，或者一条完整消息加下一条的一部分。
 * 本项目用“Buffer 保存未消费字节 + HttpContext 保存解析状态”同时解决两者。
 *
 * 【常见面试问题】为什么不能假设一次 EPOLLIN 就是一条 HTTP 请求？
 * epoll 只通知“Socket 当前可读”，不会告诉应用层有几条完整请求。可读字节数和
 * HTTP 请求边界没有一一对应关系，因此收到 EPOLLIN 后仍必须通过协议状态机判断。
 */
class HttpContext
{
public:
    enum ParseResult
    {
        kComplete,          // 当前请求已经完整，可以交给业务层
        kIncomplete,        // 当前字节不足，不是错误，等待下次读事件
        kBadRequest,        // 请求格式非法，对应 400
        kRequestTooLarge,   // 请求行、Header 或 Body 超过学习版上限
        kNotImplemented     // 协议合法但当前不支持，例如 chunked Body
    };

    HttpContext();

    // 尽可能消费 Buffer；只有确认某一段完整后才移动 readerIndex。
    ParseResult parseRequest(Buffer *buffer);
    const HttpRequest &request() const { return request_; }
    void reset();

private:
    enum ParseState
    {
        kExpectRequestLine, // 等待 Method Target Version\r\n
        kExpectHeaders,     // 逐行等待 Header，空行表示 Header 结束
        kExpectBody,        // 已知 Content-Length，等待指定字节数
        kGotAll             // 请求全部解析完成
    };

    bool processRequestLine(const char *begin, const char *end);
    ParseResult processHeaders(Buffer *buffer);

    /**
     * 防止客户端无限发送但始终不结束一行或请求，导致 Buffer 无限制增长。
     *
     * 【安全概念：慢速请求攻击】
     * 客户端可以非常缓慢地发送一个永远不结束的请求，长期占用连接和内存。
     * 大小限制只能限制内存，完整生产服务器通常还会配合定时器设置 Header/Body
     * 读取超时。本项目暂未接入定时器，所以先实现大小边界。
     */
    static const size_t kMaxRequestLineSize = 8 * 1024;
    static const size_t kMaxHeaderSize = 32 * 1024;
    static const size_t kMaxBodySize = 1024 * 1024;

    ParseState state_;
    HttpRequest request_;
    size_t contentLength_; // 从 Content-Length 解析出的 Body 字节数
    size_t headerBytes_;   // 当前请求累计解析的 Header 字节数
};
