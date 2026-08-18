#pragma once

#include <string>
#include <unordered_map>

/**
 * 一条已经解析完成的 HTTP 请求。
 *
 * TcpConnection 和 Buffer 只认识字节流，不知道这些字节代表 GET、路径或 Header。
 * HttpContext 负责从字节流中解析协议，并把结果写入本类。业务回调最终只需要
 * 读取 HttpRequest，而不必再操作原始 Buffer。
 *
 * 示例请求：
 *     GET /health?verbose=1 HTTP/1.1\r\n
 *     Host: localhost\r\n
 *     Connection: keep-alive\r\n
 *     \r\n
 *
 * 对应字段：method_=kGet、path_="/health"、query_="verbose=1"。
 *
 * 【基础知识：HTTP 请求由什么组成？】
 * 1. 请求行：Method + Request-Target + HTTP-Version；
 * 2. Header：描述主机、连接方式、Body 长度和内容类型等元数据；
 * 3. 空行：表示 Header 结束；
 * 4. 可选 Body：POST 等请求常用它携带 JSON 或表单数据。
 *
 * 【设计概念：数据对象与解析器分离】
 * HttpRequest 只保存结果，不负责从 Buffer 解析。这属于“单一职责原则”：
 * HttpContext 负责协议解析，HttpRequest 负责表示数据，HttpServer 负责调度业务。
 * 如果全部写进 TcpConnection，网络层、协议层和业务层会紧密耦合，难以测试和复用。
 *
 * 【常见面试问题】HTTP 和 TCP 是什么关系？
 * TCP 负责可靠传输字节；HTTP 规定这些字节的应用层格式和语义。
 * 可以把 TCP 理解为运输通道，把 HTTP 理解为通道里货物的包装规则。
 */
class HttpRequest
{
public:
    enum Method
    {
        kInvalid,     // 方法名本身不符合 HTTP token 语法，应返回 400
        kGet,         // 当前学习版支持 GET
        kPost,        // 当前学习版支持 POST
        kUnsupported  // 方法语法合法但尚未实现，例如 PUT，返回 501
    };

    enum Version
    {
        kUnknown, // 还没有解析到版本或版本非法
        kHttp10,  // HTTP/1.0 默认短连接
        kHttp11   // HTTP/1.1 默认 Keep-Alive
    };

    HttpRequest();

    // 校验并记录 Method；返回 false 表示 Method 的字符格式本身非法。
    bool setMethod(const std::string &method);
    void setVersion(Version version) { version_ = version; }
    void setPath(const std::string &path) { path_ = path; }
    void setQuery(const std::string &query) { query_ = query; }
    void setBody(const std::string &body) { body_ = body; }
    /**
     * Header 名称大小写不敏感，因此写入前统一转换为小写。
     * 当前实现拒绝同名重复 Header，特别是避免多个 Content-Length 导致
     * 不同节点对请求边界产生不同理解。成功插入返回 true。
     *
     * 【常见面试问题】HTTP Header 名称区分大小写吗？
     * 不区分。例如 Content-Length、content-length、CONTENT-LENGTH 表示同一字段。
     * 但 Header value 是否区分大小写取决于具体字段，不能统一全部转小写。
     */
    bool addHeader(const std::string &field, const std::string &value);

    Method method() const { return method_; }
    Version version() const { return version_; }
    const std::string &path() const { return path_; }
    const std::string &query() const { return query_; }
    const std::string &body() const { return body_; }
    bool hasHeader(const std::string &field) const;
    std::string getHeader(const std::string &field) const;

    // reset 时与一个空对象交换，统一清空字符串、Header 和枚举状态。
    void swap(HttpRequest &that);

private:
    Method method_;
    Version version_;
    std::string path_;  // 问号之前的路径，例如 /health
    std::string query_; // 问号之后的查询串，不包含 '?'
    std::string body_;  // 按 Content-Length 精确读取的请求体
    // key 保存为小写，使 Host、host、HOST 查询结果一致。
    std::unordered_map<std::string, std::string> headers_;
};
