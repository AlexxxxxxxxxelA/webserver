#include <HttpContext.h>

#include <Buffer.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <string>

namespace
{

// 去掉 Header value 首尾的可选空白，例如 "Host:   localhost  "。
std::string trim(const char *begin, const char *end)
{
    while (begin < end && std::isspace(static_cast<unsigned char>(*begin)))
    {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(*(end - 1))))
    {
        --end;
    }
    return std::string(begin, end);
}

bool parseContentLength(const std::string &value, size_t *length)
{
    /*
     * Content-Length 的语法必须是一个或多个十进制数字。
     * 先逐字符校验，再调用 strtoull 检测数值溢出，最后确认该值能够放进
     * 当前平台的 size_t。不能直接用 atoi，因为 atoi 无法可靠报告错误和溢出。
     */
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isdigit(ch);
        }))
    {
        return false;
    }

    char *end = nullptr;
    errno = 0;
    unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0' ||
        parsed > std::numeric_limits<size_t>::max())
    {
        return false;
    }
    *length = static_cast<size_t>(parsed);
    return true;
}

} // namespace

HttpContext::HttpContext()
    : state_(kExpectRequestLine)
    , contentLength_(0)
    , headerBytes_(0)
{
}

HttpContext::ParseResult HttpContext::parseRequest(Buffer *buffer)
{
    /*
     * 状态机持续向前推进，直到出现三种情况之一：
     * 1. 请求完整：返回 kComplete；
     * 2. 当前 TCP 字节不足：返回 kIncomplete，保留状态和未消费数据；
     * 3. 请求非法/超限：返回对应错误，由 HttpServer 生成错误响应。
     *
     * while 循环很重要：如果一次 readv() 已经收到请求行、Header 和 Body，
     * 就可以在同一次回调中连续跨过多个状态，不必人为等待下一次 EPOLLIN。
     *
     * 【常见面试问题】为什么解析函数要区分“不完整”和“错误”？
     * TCP 半包是正常现象，数据不完整时应等待后续字节；只有格式违反协议时才返回
     * 400。如果把半包当错误，网络稍有分段就会错误关闭正常客户端。
     */
    while (true)
    {
        if (state_ == kExpectRequestLine)
        {
            // 请求行必须以 CRLF 结束；没找到说明很可能只是 TCP 半包。
            const char *crlf = buffer->findCRLF();
            if (crlf == nullptr)
            {
                // 未结束的请求行也必须受大小限制，否则客户端可无限占用内存。
                return buffer->readableBytes() > kMaxRequestLineSize
                    ? kRequestTooLarge : kIncomplete;
            }

            // 此时 [peek(), crlf) 是一条完整请求行，可以安全解析和消费。
            if (static_cast<size_t>(crlf - buffer->peek()) > kMaxRequestLineSize ||
                !processRequestLine(buffer->peek(), crlf))
            {
                return kBadRequest;
            }
            // +2 同时越过 '\r' 和 '\n'，下一个可读字节就是第一条 Header。
            buffer->retrieveUntil(crlf + 2);
            state_ = kExpectHeaders;
        }
        else if (state_ == kExpectHeaders)
        {
            // processHeaders 可能一次处理多条 Header，也可能因半包暂停。
            ParseResult result = processHeaders(buffer);
            if (result != kComplete)
            {
                return result;
            }
        }
        else if (state_ == kExpectBody)
        {
            /*
             * Body 不使用分隔符，而是严格按 Content-Length 取字节。
             * 即使当前 Buffer 中已经有一部分 Body，也不能提前消费，否则需要
             * 额外保存已消费片段；这里保留全部数据，等长度满足后一次取出。
             */
            if (buffer->readableBytes() < contentLength_)
            {
                return kIncomplete;
            }
            request_.setBody(std::string(buffer->peek(), contentLength_));
            buffer->retrieve(contentLength_);
            state_ = kGotAll;
        }
        else
        {
            // kGotAll：request_ 中各字段已经稳定，可以交给业务层读取。
            return kComplete;
        }
    }
}

bool HttpContext::processRequestLine(const char *begin, const char *end)
{
    /*
     * 请求行格式：Method SP Request-Target SP HTTP-Version
     * 例如：      GET    /health?x=1     HTTP/1.1
     * begin/end 不包含结尾 CRLF。
     *
     * 【基础知识：URI 中 path 和 query 的区别】
     * /search?q=cpp 中，/search 是定位资源的 path，q=cpp 是查询参数 query。
     * '?' 只用于分隔，不属于 path，也不保存在 query_ 中。
     */
    const char *methodEnd = std::find(begin, end, ' ');
    if (methodEnd == end || !request_.setMethod(std::string(begin, methodEnd)))
    {
        return false;
    }

    const char *targetBegin = methodEnd + 1;
    const char *targetEnd = std::find(targetBegin, end, ' ');
    if (targetBegin == targetEnd || targetEnd == end || *targetBegin != '/')
    {
        return false;
    }

    const char *query = std::find(targetBegin, targetEnd, '?');
    // path 不包含查询串；如果存在 '?'，其后的原始内容单独存入 query_。
    request_.setPath(std::string(targetBegin, query));
    if (query != targetEnd)
    {
        request_.setQuery(std::string(query + 1, targetEnd));
    }

    std::string version(targetEnd + 1, end);
    if (version == "HTTP/1.1")
    {
        request_.setVersion(HttpRequest::kHttp11);
    }
    else if (version == "HTTP/1.0")
    {
        request_.setVersion(HttpRequest::kHttp10);
    }
    else
    {
        return false;
    }
    return true;
}

HttpContext::ParseResult HttpContext::processHeaders(Buffer *buffer)
{
    /*
     * Header 区由若干 "Field-Name: value\r\n" 组成，最后以额外空行结束：
     *     Host: localhost\r\n
     *     Content-Length: 5\r\n
     *     \r\n
     * 每轮只消费已经找到 CRLF 的完整行，因此 Header 被 TCP 拆开也不会丢失。
     *
     * 【基础知识：为什么 Header 后有一个空行？】
     * Header 数量不是固定的，协议用连续的 CRLF（即一条空行）表示 Header 区结束。
     * 解析到空行后，后续字节是否为 Body 由 Content-Length 等 Header 决定。
     */
    while (true)
    {
        const char *crlf = buffer->findCRLF();
        if (crlf == nullptr)
        {
            return headerBytes_ + buffer->readableBytes() > kMaxHeaderSize
                ? kRequestTooLarge : kIncomplete;
        }

        const size_t lineSize = static_cast<size_t>(crlf - buffer->peek()) + 2;
        headerBytes_ += lineSize;
        if (headerBytes_ > kMaxHeaderSize)
        {
            return kRequestTooLarge;
        }

        if (crlf == buffer->peek())
        {
            // 当前行长度为 0，说明遇到 Header 与 Body 之间的空行。
            buffer->retrieve(2);

            // 第一版不实现 chunked Body，明确拒绝而不是错误地按无 Body 处理。
            /*
             * 【基础知识：Content-Length 和 chunked 的区别】
             * Content-Length 在发送 Body 前就给出总字节数；chunked 把 Body 拆成多个
             * 块，每块前面携带自己的长度，最后以 0 长度块结束。两者解析方式完全
             * 不同，所以未实现 chunked 时必须明确拒绝，不能继续按 Content-Length 解析。
             */
            if (request_.hasHeader("Transfer-Encoding"))
            {
                return kNotImplemented;
            }

            // HTTP/1.1 请求必须提供 Host；HTTP/1.0 没有这个强制要求。
            /*
             * 【常见面试问题】HTTP/1.1 为什么要求 Host？
             * 同一个 IP:Port 可以托管多个域名（虚拟主机），Host 告诉服务器客户端
             * 实际访问哪个站点。没有 Host，服务器可能无法选择正确的网站或路由。
             */
            if (request_.version() == HttpRequest::kHttp11 &&
                (!request_.hasHeader("Host") || request_.getHeader("Host").empty()))
            {
                return kBadRequest;
            }

            // 没有 Content-Length 时，当前学习版把请求视为没有 Body。
            /*
             * 【易错点】Content-Length 是字符数还是字节数？
             * 是字节数。UTF-8 中文通常每个字符占多个字节，因此必须使用 string::size()
             * 或 Buffer 字节长度，不能按“肉眼看到的字符个数”计算。
             */
            if (request_.hasHeader("Content-Length") &&
                !parseContentLength(request_.getHeader("Content-Length"), &contentLength_))
            {
                return kBadRequest;
            }
            if (contentLength_ > kMaxBodySize)
            {
                return kRequestTooLarge;
            }

            // Header 结束后，根据长度决定下一状态；长度为 0 时请求已经完整。
            state_ = contentLength_ == 0 ? kGotAll : kExpectBody;
            return kComplete;
        }

        const char *colon = std::find(buffer->peek(), crlf, ':');
        if (colon == crlf || colon == buffer->peek())
        {
            return kBadRequest;
        }

        // 标准不允许 Field-Name 与冒号之间存在空白，如 "Host : value"。
        if (std::isspace(static_cast<unsigned char>(*(colon - 1))))
        {
            return kBadRequest;
        }

        // Header 名称也必须是 token，不能包含空格、控制字符等非法字符。
        std::string field(buffer->peek(), colon);
        if (!std::all_of(field.begin(), field.end(), [](unsigned char ch) {
                return std::isalnum(ch) || ch == '!' || ch == '#' || ch == '$' || ch == '%' ||
                       ch == '&' || ch == '\'' || ch == '*' || ch == '+' || ch == '-' ||
                       ch == '.' || ch == '^' || ch == '_' || ch == '`' || ch == '|' || ch == '~';
            }))
        {
            return kBadRequest;
        }
        std::string value = trim(colon + 1, crlf);
        // addHeader 返回 false 表示出现重复 Header，学习版统一按 400 拒绝。
        if (!request_.addHeader(field, value))
        {
            return kBadRequest;
        }
        buffer->retrieveUntil(crlf + 2);
    }
}

void HttpContext::reset()
{
    /*
     * 一条请求处理完后回到初始状态。注意这里只清空解析上下文，不清空 Buffer：
     * Buffer 中可能已经带有 Keep-Alive 连接上的下一条请求，HttpServer 会继续
     * 调用 parseRequest() 解析它，这就是对 HTTP Pipeline/粘包的处理。
     *
     * 【基础知识：Keep-Alive 与 Pipeline】
     * Keep-Alive 表示一个 TCP 连接可以先后承载多条 HTTP 请求；Pipeline 更进一步，
     * 客户端可在前一个响应返回前连续发送多个请求。当前实现能按顺序解析和响应，
     * 但不做并行业务处理，因此响应顺序与请求顺序一致。
     */
    state_ = kExpectRequestLine;
    contentLength_ = 0;
    headerBytes_ = 0;
    HttpRequest fresh;
    request_.swap(fresh);
}
