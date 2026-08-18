#pragma once

#include <string>
#include <unordered_map>

class Buffer;

/**
 * HTTP 响应的结构化表示。
 *
 * 业务层只设置状态码、Header 和 Body，appendToBuffer() 再统一序列化成：
 *     HTTP/1.1 200 OK\r\n
 *     Connection: keep-alive\r\n
 *     Content-Length: 16\r\n
 *     Content-Type: application/json\r\n
 *     \r\n
 *     {"status":"ok"}
 *
 * Content-Length 按 body_.size() 计算的是字节数，中文 UTF-8 内容也能正确处理。
 *
 * 【基础知识：为什么响应也需要 Content-Length？】
 * Keep-Alive 连接不会在每条响应后关闭 Socket，所以客户端不能靠“连接关闭”判断
 * Body 结束位置。Content-Length 告诉客户端读取多少字节后，本响应结束，后续字节
 * 可能属于同一连接上的下一条响应。
 *
 * 【常见面试问题】HTTP 状态码由哪几部分组成？
 * 状态行示例为 HTTP/1.1 404 Not Found：
 * - HTTP/1.1：协议版本；
 * - 404：机器容易判断的数字状态码；
 * - Not Found：便于人阅读的原因短语。
 * 常见类别：2xx 成功、4xx 客户端请求问题、5xx 服务端处理问题。
 */
class HttpResponse
{
public:
    explicit HttpResponse(bool closeConnection);

    void setStatusCode(int code) { statusCode_ = code; }
    void setStatusMessage(const std::string &message) { statusMessage_ = message; }
    void setCloseConnection(bool close) { closeConnection_ = close; }
    void setContentType(const std::string &contentType) { addHeader("Content-Type", contentType); }
    void setBody(const std::string &body) { body_ = body; }
    // Connection 和 Content-Length 由框架统一生成，业务层不能覆盖它们。
    void addHeader(const std::string &key, const std::string &value);

    bool closeConnection() const { return closeConnection_; }
    // 将结构化响应序列化到 Buffer，之后交给 TcpConnection::send() 非阻塞发送。
    void appendToBuffer(Buffer *output) const;

private:
    int statusCode_;
    std::string statusMessage_;
    bool closeConnection_;
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;
};
