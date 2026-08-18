#include <HttpRequest.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace
{

// HTTP Header 名称大小写不敏感。统一存为小写后，查询时不需要逐项比较。
std::string toLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

} // namespace

HttpRequest::HttpRequest()
    : method_(kInvalid)
    , version_(kUnknown)
{
}

bool HttpRequest::setMethod(const std::string &method)
{
    /*
     * Method 在协议上是 token，不能包含空格、控制字符或分隔符。
     * 这里先判断“语法是否合法”，再判断“本项目是否实现了它”：
     *   - 非法字符：kInvalid，解析器返回 400 Bad Request；
     *   - 合法但不是 GET/POST：kUnsupported，业务层返回 501。
     * 这样不会把 PUT 这种合法方法错误地当成一条格式损坏的请求。
     *
     * 【状态码区别】
     * 400 Bad Request：请求语法本身损坏，服务器无法理解；
     * 405 Method Not Allowed：资源存在，但不允许当前方法；
     * 501 Not Implemented：服务器整体尚未实现该方法或协议能力。
     * 当前 main.cc 中 POST /health 返回 405，PUT 等未实现方法返回 501。
     */
    if (method.empty() || !std::all_of(method.begin(), method.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '!' || ch == '#' || ch == '$' || ch == '%' ||
                   ch == '&' || ch == '\'' || ch == '*' || ch == '+' || ch == '-' ||
                   ch == '.' || ch == '^' || ch == '_' || ch == '`' || ch == '|' || ch == '~';
        }))
    {
        method_ = kInvalid;
        return false;
    }

    if (method == "GET")
    {
        method_ = kGet;
    }
    else if (method == "POST")
    {
        method_ = kPost;
    }
    else
    {
        method_ = kUnsupported;
    }
    return true;
}

bool HttpRequest::addHeader(const std::string &field, const std::string &value)
{
    /*
     * unordered_map::emplace 在 key 已存在时返回 false。
     * 学习版选择拒绝所有重复 Header，规则比完整 RFC 更保守，但能避免：
     *   Content-Length: 5
     *   Content-Length: 0
     * 这类请求让不同 HTTP 节点对 Body 边界产生歧义。
     *
     * 【安全概念：HTTP Request Smuggling】
     * 如果代理按第一个 Content-Length 判断边界，而后端按第二个判断边界，双方会
     * 对“下一条请求从哪里开始”产生不同理解，攻击者可能把隐藏请求带入后端。
     * 学习版直接拒绝重复 Header，是简单且保守的处理方式。
     */
    return headers_.emplace(toLower(field), value).second;
}

bool HttpRequest::hasHeader(const std::string &field) const
{
    return headers_.find(toLower(field)) != headers_.end();
}

std::string HttpRequest::getHeader(const std::string &field) const
{
    auto it = headers_.find(toLower(field));
    return it == headers_.end() ? std::string() : it->second;
}

void HttpRequest::swap(HttpRequest &that)
{
    // 与默认构造的空请求交换，比逐个 clear/reset 更不容易遗漏字段。
    std::swap(method_, that.method_);
    std::swap(version_, that.version_);
    path_.swap(that.path_);
    query_.swap(that.query_);
    body_.swap(that.body_);
    headers_.swap(that.headers_);
}
