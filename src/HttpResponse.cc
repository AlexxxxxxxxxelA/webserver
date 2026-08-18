#include <HttpResponse.h>

#include <Buffer.h>

#include <algorithm>
#include <cctype>
#include <stdio.h>

HttpResponse::HttpResponse(bool closeConnection)
    : statusCode_(200)
    , statusMessage_("OK")
    , closeConnection_(closeConnection)
{
}

void HttpResponse::addHeader(const std::string &key, const std::string &value)
{
    /*
     * Connection 与 Content-Length 会在 appendToBuffer() 中统一生成。
     * 禁止业务层再次添加这两个 Header，避免响应中出现两个互相冲突的长度
     * 或连接策略，导致客户端无法判断响应边界。
     *
     * 【设计原则】协议关键字段由框架控制，普通业务字段开放给上层。
     * 路由可以设置 Content-Type、Allow 等 Header，但不应破坏连接和消息边界。
     */
    std::string lower(key);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lower != "content-length" && lower != "connection")
    {
        headers_[key] = value;
    }
}

void HttpResponse::appendToBuffer(Buffer *output) const
{
    /*
     * HTTP 响应序列化顺序：
     *   状态行 -> 框架 Header -> 业务 Header -> 空行 -> Body
     * 所有协议行都必须用 CRLF，而不是普通的 '\n'。
     *
     * 【常见面试问题】为什么不能直接 send 多次拼出响应？
     * 可以多次 send，但每次都可能发生部分写，并增加系统调用与状态管理复杂度。
     * 当前先在 Buffer 中序列化成完整响应，再交给 TcpConnection；TcpConnection 会
     * 尝试直接写，未写完的部分进入 outputBuffer 并等待 EPOLLOUT，职责更清晰。
     */
    char buffer[64];

    // 1. 状态行，例如 HTTP/1.1 200 OK\r\n
    snprintf(buffer, sizeof buffer, "HTTP/1.1 %d ", statusCode_);
    output->append(std::string(buffer));
    output->append(statusMessage_);
    output->append("\r\n", 2);

    if (closeConnection_)
    {
        // 告知客户端本响应发送完后服务器将关闭写端。
        output->append("Connection: close\r\n", 19);
    }
    else
    {
        // HTTP/1.1 默认长连接，这里显式输出便于观察和学习。
        output->append("Connection: keep-alive\r\n", 24);
    }

    // 2. Body 的字节长度决定了 Keep-Alive 下这一条响应在哪里结束。
    snprintf(buffer, sizeof buffer, "Content-Length: %zu\r\n", body_.size());
    output->append(std::string(buffer));
    // 3. 输出 Content-Type、Allow 等业务 Header。
    for (const auto &header : headers_)
    {
        output->append(header.first);
        output->append(": ", 2);
        output->append(header.second);
        output->append("\r\n", 2);
    }
    // 4. 空行表示 Header 结束，之后的所有字节都属于 Body。
    output->append("\r\n", 2);
    output->append(body_);
}
