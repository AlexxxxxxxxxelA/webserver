#include "AgentDemo.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "Logger.h"
#include "TcpConnection.h"

namespace
{

std::string trim(const std::string &input)
{
    size_t begin = 0;
    while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])))
    {
        ++begin;
    }

    size_t end = input.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])))
    {
        --end;
    }

    return input.substr(begin, end - begin);
}

bool fileExists(const std::string &path)
{
    std::ifstream input(path.c_str());
    return input.good();
}

AgentDemoConfig loadAgentDemoConfig()
{
    AgentDemoConfig config;
    config.port = 18080;
    config.deepseekApiUrl = "https://api.deepseek.com/chat/completions";
    config.deepseekModel = "deepseek-chat";

    const char *candidatePaths[] = {
        "agent_demo.conf",
        "./agent_demo.conf",
        "../config/agent_demo.conf",
        "config/agent_demo.conf"
    };

    std::string path;
    for (size_t i = 0; i < sizeof(candidatePaths) / sizeof(candidatePaths[0]); ++i)
    {
        if (fileExists(candidatePaths[i]))
        {
            path = candidatePaths[i];
            break;
        }
    }

    config.configPath = path.empty() ? "not found" : path;

    if (!path.empty())
    {
        std::ifstream input(path.c_str());
        std::string line;
        while (std::getline(input, line))
        {
            line = trim(line);
            if (line.empty() || line[0] == '#')
            {
                continue;
            }

            size_t equals = line.find('=');
            if (equals == std::string::npos)
            {
                continue;
            }

            std::string key = trim(line.substr(0, equals));
            std::string value = trim(line.substr(equals + 1));
            if (key == "port")
            {
                config.port = static_cast<uint16_t>(std::atoi(value.c_str()));
            }
            else if (key == "deepseek_api_key")
            {
                config.deepseekApiKey = value;
            }
            else if (key == "deepseek_api_url")
            {
                config.deepseekApiUrl = value;
            }
            else if (key == "deepseek_model")
            {
                config.deepseekModel = value;
            }
        }
    }

    // 环境变量适合容器、CI 和本地测试，可覆盖文件配置且不会进入 Git。
    const char *apiKey = std::getenv("DEEPSEEK_API_KEY");
    const char *apiUrl = std::getenv("DEEPSEEK_API_URL");
    const char *model = std::getenv("DEEPSEEK_MODEL");
    if (apiKey != NULL && apiKey[0] != '\0')
    {
        config.deepseekApiKey = apiKey;
    }
    if (apiUrl != NULL && apiUrl[0] != '\0')
    {
        config.deepseekApiUrl = apiUrl;
    }
    if (model != NULL && model[0] != '\0')
    {
        config.deepseekModel = model;
    }

    return config;
}

std::string toLower(std::string input)
{
    std::transform(input.begin(), input.end(), input.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return input;
}

class CurlGlobal
{
public:
    CurlGlobal()
    {
        /*
         * libcurl 有进程级共享状态。最容易保证线程安全的方式是：在任何日志线程、
         * worker 和 EventLoop 线程启动前初始化，在所有使用者退出后再 cleanup。
         * 局部 static 还保证 C++11 下首次初始化只执行一次。
         */
        CURLcode code = ::curl_global_init(CURL_GLOBAL_DEFAULT);
        if (code != CURLE_OK)
        {
            throw std::runtime_error("curl_global_init failed");
        }
    }

    ~CurlGlobal()
    {
        ::curl_global_cleanup();
    }
};

CurlGlobal &curlGlobal()
{
    static CurlGlobal global;
    return global;
}

struct CurlResponse
{
    CurlResponse() : tooLarge(false) {}
    std::string body;
    bool tooLarge;
};

struct CurlHandleDeleter
{
    // 把 C API 的 cleanup 函数包装成 unique_ptr deleter，实现 RAII。
    void operator()(CURL *curl) const
    {
        if (curl != NULL)
        {
            ::curl_easy_cleanup(curl);
        }
    }
};

struct CurlHeadersDeleter
{
    void operator()(curl_slist *headers) const
    {
        if (headers != NULL)
        {
            ::curl_slist_free_all(headers);
        }
    }
};

size_t writeCurlResponse(char *data, size_t size, size_t count, void *userData)
{
    /*
     * libcurl 每收到一段响应数据就调用本函数，分块大小由 libcurl 决定，不等于
     * 完整 HTTP Body。必须不断 append；返回值小于 bytes 会让 libcurl 中止传输。
     * 这里借此实现 1 MiB 硬上限，避免上游异常响应耗尽服务器内存。
     */
    const size_t bytes = size * count;
    const size_t kMaxResponseBytes = 1024 * 1024;
    CurlResponse *response = static_cast<CurlResponse *>(userData);
    if (response->body.size() + bytes > kMaxResponseBytes)
    {
        response->tooLarge = true;
        return 0;
    }
    response->body.append(data, bytes);
    return bytes;
}

class Calculator
{
public:
    explicit Calculator(const std::string &expression)
        : expression_(expression)
        , position_(0)
    {
    }

    double evaluate()
    {
        double value = parseExpression();
        skipSpaces();
        if (position_ != expression_.size())
        {
            throw std::runtime_error("invalid expression");
        }
        return value;
    }

private:
    double parseExpression()
    {
        double value = parseTerm();
        while (true)
        {
            skipSpaces();
            if (match('+'))
            {
                value += parseTerm();
            }
            else if (match('-'))
            {
                value -= parseTerm();
            }
            else
            {
                break;
            }
        }
        return value;
    }

    double parseTerm()
    {
        double value = parseFactor();
        while (true)
        {
            skipSpaces();
            if (match('*'))
            {
                value *= parseFactor();
            }
            else if (match('/'))
            {
                double rhs = parseFactor();
                if (rhs == 0.0)
                {
                    throw std::runtime_error("division by zero");
                }
                value /= rhs;
            }
            else
            {
                break;
            }
        }
        return value;
    }

    double parseFactor()
    {
        skipSpaces();
        if (match('('))
        {
            double value = parseExpression();
            skipSpaces();
            if (!match(')'))
            {
                throw std::runtime_error("missing ')'");
            }
            return value;
        }

        if (match('-'))
        {
            return -parseFactor();
        }

        return parseNumber();
    }

    double parseNumber()
    {
        skipSpaces();
        size_t start = position_;
        bool hasDot = false;
        while (position_ < expression_.size())
        {
            char ch = expression_[position_];
            if (std::isdigit(static_cast<unsigned char>(ch)))
            {
                ++position_;
                continue;
            }
            if (ch == '.' && !hasDot)
            {
                hasDot = true;
                ++position_;
                continue;
            }
            break;
        }

        if (start == position_)
        {
            throw std::runtime_error("expected number");
        }

        return std::strtod(expression_.substr(start, position_ - start).c_str(), NULL);
    }

    void skipSpaces()
    {
        while (position_ < expression_.size() &&
               std::isspace(static_cast<unsigned char>(expression_[position_])))
        {
            ++position_;
        }
    }

    bool match(char ch)
    {
        if (position_ < expression_.size() && expression_[position_] == ch)
        {
            ++position_;
            return true;
        }
        return false;
    }

    std::string expression_;
    size_t position_;
};

std::string formatDouble(double value)
{
    std::ostringstream oss;
    oss.precision(15);
    oss << value;
    return oss.str();
}

std::string currentTimeString()
{
    time_t now = ::time(NULL);
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    char buffer[64] = {0};
    ::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tmNow);
    return buffer;
}

std::string normalizeMultiline(const std::string &text)
{
    std::string normalized;
    normalized.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\r')
        {
            continue;
        }
        normalized.push_back(text[i]);
    }
    return normalized;
}

class DeepSeekClient
{
public:
    bool isConfigured() const
    {
        return !getAgentDemoConfig().deepseekApiKey.empty() &&
               getAgentDemoConfig().deepseekApiKey != "YOUR_DEEPSEEK_API_KEY";
    }

    bool chat(const std::string &systemPrompt,
              const std::string &userPrompt,
               std::string *content,
               std::string *rawOutput,
               std::string *error,
               bool *timedOut) const
    {
        /*
         * 这是同步阻塞函数，但它只会由 BoundedThreadPool worker 调用。
         * “同步函数”并不等于“一定阻塞 Reactor”，关键看它运行在哪个线程。
         */
        *timedOut = false;
        const AgentDemoConfig &config = getAgentDemoConfig();
        if (config.deepseekApiKey.empty() || config.deepseekApiKey == "YOUR_DEEPSEEK_API_KEY")
        {
            *error = "deepseek_api_key is not configured";
            return false;
        }

        // 先构造 JSON；若非法 UTF-8 导致 dump 抛异常，此时还没有 curl 资源需要清理。
        nlohmann::json payloadJson;
        payloadJson["model"] = config.deepseekModel;
        payloadJson["stream"] = false;
        payloadJson["messages"] = nlohmann::json::array({
            {{"role", "system"}, {"content", systemPrompt}},
            {{"role", "user"}, {"content", userPrompt}}
        });
        const std::string payload = payloadJson.dump();

        std::unique_ptr<CURL, CurlHandleDeleter> curl(::curl_easy_init());
        if (!curl)
        {
            *error = "curl_easy_init failed";
            return false;
        }

        std::unique_ptr<curl_slist, CurlHeadersDeleter> headers(
            ::curl_slist_append(NULL, "Content-Type: application/json"));
        if (!headers)
        {
            *error = "failed to allocate HTTP headers";
            return false;
        }
        const std::string authorization =
            std::string("Authorization: Bearer ") + config.deepseekApiKey;
        struct curl_slist *newHeaders = ::curl_slist_append(headers.get(), authorization.c_str());
        if (newHeaders == NULL)
        {
            *error = "failed to allocate authorization header";
            return false;
        }
        // curl_slist_append 成功时仍返回链表头；release/reset 转移给同一 RAII 对象。
        headers.release();
        headers.reset(newHeaders);

        CurlResponse response;
        char curlError[CURL_ERROR_SIZE] = {0};
        ::curl_easy_setopt(curl.get(), CURLOPT_URL, config.deepseekApiUrl.c_str());
        ::curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
        ::curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
        ::curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, payload.data());
        ::curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                           static_cast<curl_off_t>(payload.size()));
        ::curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeCurlResponse);
        ::curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
        ::curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 5000L);
        ::curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 30000L);
        ::curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        ::curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, curlError);

        // worker 会在这里等待 DNS/TCP/TLS/HTTP，但 IO loop 已经返回 epoll_wait。
        CURLcode code = ::curl_easy_perform(curl.get());
        long httpStatus = 0;
        ::curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &httpStatus);

        *rawOutput = response.body;
        if (code != CURLE_OK)
        {
            *timedOut = code == CURLE_OPERATION_TIMEDOUT;
            if (response.tooLarge)
            {
                *error = "DeepSeek response exceeded 1 MiB";
            }
            else
            {
                *error = curlError[0] != '\0' ? curlError : ::curl_easy_strerror(code);
            }
            return false;
        }
        if (httpStatus < 200 || httpStatus >= 300)
        {
            std::ostringstream oss;
            oss << "DeepSeek returned HTTP " << httpStatus;
            *error = oss.str();
            return false;
        }

        try
        {
            const nlohmann::json responseJson = nlohmann::json::parse(response.body);
            *content = responseJson.at("choices").at(0).at("message").at("content").get<std::string>();
        }
        catch (const std::exception &)
        {
            *error = "failed to parse DeepSeek response content";
            return false;
        }
        return true;
    }
};

DeepSeekClient &deepSeekClient()
{
    static DeepSeekClient client;
    return client;
}

} // namespace

void initializeAgentRuntime()
{
    // main 在启动异步日志前调用；AgentDemoService 构造中再次调用也是幂等的。
    (void)curlGlobal();
}

const AgentDemoConfig &getAgentDemoConfig()
{
    static AgentDemoConfig config = loadAgentDemoConfig();
    return config;
}

AgentDemoService::AgentDemoService()
    : accessSequence_(0)
    , businessPool_(4, 64)
{
    // main() 会在所有线程前初始化；这里保留幂等调用，避免类被其他程序单独使用时遗漏。
    (void)curlGlobal();
    businessPool_.start();
}

AgentDemoService::~AgentDemoService()
{
    stop();
}

void AgentDemoService::stop()
{
    // 必须在网络 server/其 EventLoop 析构前 join，避免 completion 访问失效 loop_。
    businessPool_.stop();
}

void AgentDemoService::onConnection(const TcpConnectionPtr &conn)
{
    if (conn->connected())
    {
        LOG_INFO << "Agent connection UP :" << conn->peerAddress().toIpPort().c_str();
        std::ostringstream welcome;
        welcome << "Agent Demo connected.\n"
                << "Config: " << getAgentDemoConfig().configPath << "\n"
                << "DeepSeek configured: " << (isConfigured() ? "yes" : "no") << "\n"
                << "Type one question per line. Commands: /health /clear /quit\n> ";
        conn->send(welcome.str());
        return;
    }

    LOG_INFO << "Agent connection DOWN :" << conn->peerAddress().toIpPort().c_str();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingRequests_.erase(conn->name());
    }
    eraseSession(std::string("tcp:") + conn->name());
}

void AgentDemoService::onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp receiveTime)
{
    (void)receiveTime;
    std::string chunk = buf->retrieveAllAsString();
    std::vector<std::string> lines;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string &pending = pendingRequests_[conn->name()];
        pending.append(chunk);
        size_t newline = pending.find('\n');
        while (newline != std::string::npos)
        {
            std::string line = pending.substr(0, newline);
            if (!line.empty() && line[line.size() - 1] == '\r')
            {
                line.erase(line.size() - 1);
            }
            lines.push_back(line);
            pending.erase(0, newline + 1);
            newline = pending.find('\n');
        }

        if (pending.size() > 1024 * 1024)
        {
            pending.clear();
            conn->send("Error: input too large\n> ");
            conn->shutdown();
            return;
        }
    }

    if (lines.empty())
    {
        return;
    }

    for (size_t i = 0; i < lines.size(); ++i)
    {
        std::string message = trim(lines[i]);
        if (message.empty())
        {
            conn->send("> ");
            continue;
        }

        if (message == "/quit" || message == "/exit")
        {
            conn->send("Bye.\n");
            conn->shutdown();
            return;
        }

        if (message == "/health")
        {
            std::ostringstream oss;
            oss << "status=ok"
                << ", deepseek_configured=" << (isConfigured() ? "yes" : "no")
                << ", config=" << getAgentDemoConfig().configPath << "\n> ";
            conn->send(oss.str());
            continue;
        }

        if (message == "/clear")
        {
            if (clearSession(std::string("tcp:") + conn->name()))
            {
                conn->send("Conversation cleared.\n> ");
            }
            else
            {
                conn->send("Conversation is busy. Try again later.\n> ");
            }
            continue;
        }

        std::weak_ptr<TcpConnection> weakConnection(conn);
        /*
         * 后台任务可能晚于客户端断开完成。若强持有 shared_ptr，连接会被任务强行
         * 延寿；使用 weak_ptr，completion 执行时再 lock，连接不存在就丢弃结果。
         */
        SubmitStatus status = submit(
            std::string("tcp:") + conn->name(), message,
            [this, weakConnection](AgentResult result) {
                TcpConnectionPtr connection = weakConnection.lock();
                if (!connection || !connection->connected())
                {
                    return;
                }
                if (!result.ok())
                {
                    connection->send(std::string("Agent error: ") + result.errorMessage + "\n\n> ");
                    return;
                }
                connection->send(formatChatReply(result.answer, result.toolName,
                                                 result.toolResult) + "\n\n> ");
            });
        if (status == kSessionBusy)
        {
            conn->send("Agent is already processing this conversation.\n> ");
        }
        else if (status == kQueueFull)
        {
            conn->send("Agent is busy. Try again later.\n> ");
        }
        else if (status == kInvalidRequest)
        {
            conn->send("Message is empty or too large.\n> ");
        }
    }
}

AgentDemoService::SubmitStatus AgentDemoService::submit(
    const std::string &sessionId, const std::string &message, Completion completion)
{
    /*
     * submit 运行在 IO 线程，只允许做快速校验、session 状态切换和非阻塞入队。
     * 不能在这里等待 session mutex 很久、等待队列空位或调用 DeepSeek。
     */
    if (sessionId.empty() || sessionId.size() > 128 ||
        message.empty() || message.size() > 16 * 1024 || !completion)
    {
        return kInvalidRequest;
    }
    const bool tcpSession = sessionId.compare(0, 4, "tcp:") == 0;
    for (size_t i = 0; i < sessionId.size(); ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(sessionId[i]);
        if (!std::isalnum(ch) && ch != '-' && ch != '_' && ch != '.' && ch != ':' &&
            !(tcpSession && ch == '#'))
        {
            return kInvalidRequest;
        }
    }

    std::shared_ptr<Session> session = getOrCreateSession(sessionId);
    if (!session)
    {
        return kQueueFull;
    }
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->inFlight)
        {
            return kSessionBusy;
        }
        session->inFlight = true;
    }

    /*
     * 【为什么用 inFlight，而不是只依赖 mutex 避免数据竞争？】
     * mutex 只能保证 vector 不被同时写坏。若两个请求都先复制同一份旧历史，再分别
     * 等待模型，最终历史顺序仍由完成快慢决定，这是“业务语义竞态”。busy 时返回
     * 409，可以保证一个 session 的完整 read -> model -> append turn 串行。
     */

    bool accepted = false;
    try
    {
        accepted = businessPool_.trySubmit([this, session, message, completion]() {
            // 以下代码运行在业务 worker，而不是连接所属 EventLoop。
            AgentResult result;
            try
            {
                result = runTurn(session, message);
            }
            catch (const std::exception &ex)
            {
                result.error = AgentResult::kInternalError;
                result.errorMessage = ex.what();
            }
            catch (...)
            {
                result.error = AgentResult::kInternalError;
                result.errorMessage = "unknown agent error";
            }

            {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->inFlight = false;
            }
            // 必须先复位 busy，再通知上层；否则 completion 后立即重试仍会得到 409。
            completion(result);
        });
    }
    catch (const std::exception &)
    {
        accepted = false;
    }

    if (!accepted)
    {
        // 入队失败也必须复位，否则该 session 会永久卡在 busy。
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->inFlight = false;
        }
        eraseRejectedEmptySession(sessionId, session);
        return kQueueFull;
    }
    return kAccepted;
}

bool AgentDemoService::clearSession(const std::string &sessionId)
{
    std::shared_ptr<Session> session = getOrCreateSession(sessionId);
    if (!session)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->inFlight)
    {
        return false;
    }
    session->history.clear();
    return true;
}

bool AgentDemoService::isConfigured() const
{
    return deepSeekClient().isConfigured();
}

std::shared_ptr<AgentDemoService::Session>
AgentDemoService::getOrCreateSession(const std::string &sessionId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end())
    {
        it->second->lastAccess = ++accessSequence_;
        return it->second;
    }

    // HTTP session 不随 TCP 断开自动删除，因此必须限制总数量，避免内存无限增长。
    const size_t kMaxSessions = 1024;
    if (sessions_.size() >= kMaxSessions)
    {
        /*
         * 没有接入 TimerQueue TTL 前，先使用容量触发淘汰。只淘汰：
         * 1. 不在执行；2. map 是唯一持有者。否则可能删除一个 worker 正在使用的会话。
         * lastAccess 是递增序号，值越小表示越久没有访问。
         */
        auto oldest = sessions_.end();
        for (auto candidate = sessions_.begin(); candidate != sessions_.end(); ++candidate)
        {
            std::lock_guard<std::mutex> sessionLock(candidate->second->mutex);
            if (!candidate->second->inFlight && candidate->second.use_count() == 1 &&
                (oldest == sessions_.end() ||
                 candidate->second->lastAccess < oldest->second->lastAccess))
            {
                oldest = candidate;
            }
        }
        if (oldest == sessions_.end())
        {
            return std::shared_ptr<Session>();
        }
        sessions_.erase(oldest);
    }

    std::shared_ptr<Session> &session = sessions_[sessionId];
    if (!session)
    {
        session.reset(new Session());
        session->lastAccess = ++accessSequence_;
    }
    return session;
}

void AgentDemoService::eraseSession(const std::string &sessionId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(sessionId);
}

void AgentDemoService::eraseRejectedEmptySession(
    const std::string &sessionId, const std::shared_ptr<Session> &session)
{
    // 锁顺序始终是全局 map 锁 -> 单 session 锁，与淘汰流程保持一致。
    std::lock_guard<std::mutex> mapLock(mutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end() || it->second != session)
    {
        return;
    }
    std::lock_guard<std::mutex> sessionLock(session->mutex);
    if (!session->inFlight && session->history.empty() && session.use_count() == 2)
    {
        sessions_.erase(it);
    }
}

AgentDemoService::AgentResult AgentDemoService::runTurn(
    const std::shared_ptr<Session> &session, const std::string &message)
{
    /*
     * 一个 Agent turn：复制历史 -> planner -> 可选工具 -> 可选第二次模型请求
     * -> 追加 user/assistant 历史。耗时步骤全部在业务线程执行。
     */
    AgentResult result;
    std::vector<ChatMessage> history;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        history = session->history;
    }

    std::string plannerOutput;
    std::string plannerError;
    std::string plannerContent;
    std::string conversationContext = buildConversationContext(history);

    std::string plannerPrompt =
        "You are an agent planner. Available tools are calculator(expression) and time(). "
        "You must return JSON only without markdown. "
        "If a tool is needed, return {\"tool\":\"calculator\",\"input\":\"1+2\"} or {\"tool\":\"time\"}. "
        "If no tool is needed, return {\"tool\":\"none\",\"answer\":\"...\"}.";

    std::string plannerUserPrompt = message;
    if (!conversationContext.empty())
    {
        plannerUserPrompt = std::string("Conversation history:\n") + conversationContext +
                            "\nCurrent user message:\n" + message;
    }

    bool timedOut = false;
    if (!deepSeekClient().chat(plannerPrompt, plannerUserPrompt, &plannerContent,
                               &plannerOutput, &plannerError, &timedOut))
    {
        result.error = timedOut ? AgentResult::kUpstreamTimeout :
            (isConfigured() ? AgentResult::kUpstreamError : AgentResult::kNotConfigured);
        result.errorMessage = plannerError;
        return result;
    }

    nlohmann::json plannerJson;
    try
    {
        plannerJson = nlohmann::json::parse(plannerContent);
    }
    catch (const std::exception &)
    {
        std::string fallback = normalizeMultiline(plannerContent);
        appendHistory(session, message, fallback);
        result.answer = fallback;
        result.toolName = "none";
        return result;
    }

    if (!plannerJson.is_object() || !plannerJson.contains("tool") ||
        !plannerJson["tool"].is_string())
    {
        std::string fallback = normalizeMultiline(plannerContent);
        appendHistory(session, message, fallback);
        result.answer = fallback;
        result.toolName = "none";
        return result;
    }

    std::string toolName = trim(plannerJson["tool"].get<std::string>());
    if (toolName == "none")
    {
        std::string answer;
        if (!plannerJson.contains("answer") || !plannerJson["answer"].is_string())
        {
            answer = plannerContent;
        }
        else
        {
            answer = plannerJson["answer"].get<std::string>();
        }
        answer = normalizeMultiline(answer);
        appendHistory(session, message, answer);
        result.answer = answer;
        result.toolName = "none";
        return result;
    }

    std::string toolResult;
    if (toolName == "calculator")
    {
        if (!plannerJson.contains("input") || !plannerJson["input"].is_string())
        {
            result.error = AgentResult::kInternalError;
            result.errorMessage = "calculator tool missing input";
            return result;
        }
        std::string expression = plannerJson["input"].get<std::string>();
        if (expression.size() > 4096)
        {
            result.error = AgentResult::kInternalError;
            result.errorMessage = "calculator input is too large";
            return result;
        }

        try
        {
            Calculator calculator(expression);
            toolResult = formatDouble(calculator.evaluate());
        }
        catch (const std::exception &ex)
        {
            toolResult = std::string("calculator error: ") + ex.what();
        }
    }
    else if (toolName == "time")
    {
        toolResult = currentTimeString();
    }
    else
    {
        result.error = AgentResult::kInternalError;
        result.errorMessage = std::string("unsupported tool: ") + toolName;
        return result;
    }

    std::string finalContent;
    std::string finalRaw;
    std::string finalError;
    std::ostringstream toolUserPrompt;
    toolUserPrompt << "User question: " << message << "\n"
                   << "Tool used: " << toolName << "\n"
                   << "Tool result: " << toolResult << "\n"
                   << "Please answer in concise Chinese.";

    timedOut = false;
    if (!deepSeekClient().chat(
            "You are a helpful assistant. Use the provided tool result to answer accurately.",
            conversationContext.empty()
                ? toolUserPrompt.str()
                : std::string("Conversation history:\n") + conversationContext + "\n" + toolUserPrompt.str(),
            &finalContent,
            &finalRaw,
            &finalError,
            &timedOut))
    {
        // 第二次模型整理失败时降级返回本地工具结果，保证工具本身的有效结果可用。
        finalContent = std::string("工具结果：") + toolResult;
    }
    finalContent = normalizeMultiline(finalContent);
    appendHistory(session, message, finalContent);
    result.answer = finalContent;
    result.toolName = toolName;
    result.toolResult = toolResult;
    return result;
}

std::string AgentDemoService::buildConversationContext(const std::vector<ChatMessage> &history) const
{
    std::ostringstream oss;
    for (size_t i = 0; i < history.size(); ++i)
    {
        oss << history[i].role << ": " << history[i].content << "\n";
    }
    return oss.str();
}

std::string AgentDemoService::formatChatReply(const std::string &answer,
                                              const std::string &toolName,
                                              const std::string &toolResult) const
{
    std::ostringstream oss;
    oss << "Assistant:\n" << answer;
    if (toolName != "none" && !toolName.empty())
    {
        oss << "\n\n[tool] " << toolName;
        if (!toolResult.empty())
        {
            oss << "\n[result] " << toolResult;
        }
    }
    return oss.str();
}

void AgentDemoService::appendHistory(const std::shared_ptr<Session> &session,
                                     const std::string &userMessage,
                                     const std::string &assistantMessage)
{
    std::lock_guard<std::mutex> lock(session->mutex);
    std::vector<ChatMessage> &history = session->history;
    ChatMessage user;
    user.role = "user";
    user.content = userMessage;
    history.push_back(user);

    ChatMessage assistant;
    assistant.role = "assistant";
    assistant.content = assistantMessage;
    history.push_back(assistant);

    const size_t kMaxMessages = 120;
    if (history.size() > kMaxMessages)
    {
        history.erase(history.begin(), history.begin() + (history.size() - kMaxMessages));
    }

    // 除消息条数外再限制总字节数，避免少量超长消息长期占用大量内存。
    const size_t kMaxHistoryBytes = 128 * 1024;
    size_t bytes = 0;
    for (size_t i = 0; i < history.size(); ++i)
    {
        bytes += history[i].role.size() + history[i].content.size();
    }
    while (!history.empty() && bytes > kMaxHistoryBytes)
    {
        bytes -= history.front().role.size() + history.front().content.size();
        history.erase(history.begin());
    }
}
