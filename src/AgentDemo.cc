#include "AgentDemo.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <unistd.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "Logger.h"
#include "TcpConnection.h"
#include "DeepSeekProtocol.h"

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

bool parseBooleanConfig(const std::string &value)
{
    std::string normalized = trim(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (normalized == "true" || normalized == "1" || normalized == "yes" ||
        normalized == "on")
    {
        return true;
    }
    if (normalized == "false" || normalized == "0" || normalized == "no" ||
        normalized == "off")
    {
        return false;
    }
    throw std::invalid_argument(
        "deepseek_thinking_enabled must be true/false, 1/0, yes/no, or on/off");
}

AgentDemoConfig loadAgentDemoConfig()
{
    AgentDemoConfig config;
    config.port = 18080;
    config.deepseekApiUrl = "https://api.deepseek.com/chat/completions";
    config.deepseekModel = "deepseek-v4-flash";
    config.deepseekThinkingEnabled = true;
    config.weatherApiBaseUrl = "https://wttr.in";
    config.conversationDatabasePath = "data/conversations.db";

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
            else if (key == "deepseek_thinking_enabled")
            {
                config.deepseekThinkingEnabled =
                    parseBooleanConfig(value);
            }
            else if (key == "weather_api_base_url")
            {
                config.weatherApiBaseUrl = value;
            }
            else if (key == "conversation_database_path")
            {
                config.conversationDatabasePath = value;
            }
        }
    }

    // 手工常从 bin/ 启动；此时配置位于 ../config，数据库应落到 ../data。
    if (config.conversationDatabasePath == "data/conversations.db" &&
        path.compare(0, 3, "../") == 0)
    {
        config.conversationDatabasePath = "../data/conversations.db";
    }

    // 环境变量适合容器、CI 和本地测试，可覆盖文件配置且不会进入 Git。
    const char *apiKey = std::getenv("DEEPSEEK_API_KEY");
    const char *apiUrl = std::getenv("DEEPSEEK_API_URL");
    const char *model = std::getenv("DEEPSEEK_MODEL");
    const char *thinkingEnabled = std::getenv("DEEPSEEK_THINKING_ENABLED");
    const char *weatherApiBaseUrl = std::getenv("WEATHER_API_BASE_URL");
    const char *conversationDatabasePath = std::getenv("CONVERSATION_DATABASE_PATH");
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
    if (thinkingEnabled != NULL && thinkingEnabled[0] != '\0')
    {
        config.deepseekThinkingEnabled =
            parseBooleanConfig(thinkingEnabled);
    }
    if (weatherApiBaseUrl != NULL && weatherApiBaseUrl[0] != '\0')
    {
        config.weatherApiBaseUrl = weatherApiBaseUrl;
    }
    if (conversationDatabasePath != NULL && conversationDatabasePath[0] != '\0')
    {
        config.conversationDatabasePath = conversationDatabasePath;
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

long elapsedMilliseconds(const std::chrono::steady_clock::time_point &begin,
                         const std::chrono::steady_clock::time_point &end)
{
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
}

std::string createAgentRunId()
{
    /*
     * Run ID 只用于日志和响应关联，不承担鉴权或幂等语义。系统时间提供跨进程可读前缀，
     * 原子序号解决同一微秒内并发创建的冲突；真正的写操作幂等键必须由调用方提供并
     * 持久化，不能复用这个临时观测 ID。
     */
    static std::atomic<unsigned long long> sequence(0);
    const long long micros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream oss;
    oss << "run-" << std::hex << micros << "-" << ++sequence;
    return oss.str();
}

const std::string &tcpSessionNamespace()
{
    /*
     * TcpServer 连接序号重启后从 #1 开始。为防止异常退出遗留的 tcp:#1 被下一次启动
     * 的新用户加载，每个进程使用不同持久化命名空间；正常断开仍会异步删除该记录。
     */
    static const std::string value = []() {
        std::ostringstream output;
        output << "tcp:" << std::hex
               << std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count()
               << ":" << static_cast<long long>(::getpid()) << ":";
        return output.str();
    }();
    return value;
}

std::string tcpSessionId(const TcpConnectionPtr &connection)
{
    return tcpSessionNamespace() + connection->name();
}

std::string createChatSessionId()
{
    // Session ID 不是鉴权凭据，但随机值可以降低本地碰撞和误枚举概率。
    std::random_device random;
    std::ostringstream output;
    output << "chat-" << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i)
    {
        output << std::setw(8) << static_cast<uint32_t>(random());
    }
    return output.str();
}

bool hasUnsafeTitleCodePoint(const std::string &title)
{
    for (size_t i = 0; i < title.size();)
    {
        const unsigned char first = static_cast<unsigned char>(title[i]);
        size_t width = 1;
        if ((first & 0xe0) == 0xc0) width = 2;
        else if ((first & 0xf0) == 0xe0) width = 3;
        else if ((first & 0xf8) == 0xf0) width = 4;
        if (i + width > title.size()) return true;
        uint32_t codePoint = width == 1 ? first : first & (0x7f >> width);
        for (size_t j = 1; j < width; ++j)
        {
            const unsigned char next = static_cast<unsigned char>(title[i + j]);
            if ((next & 0xc0) != 0x80) return true;
            codePoint = (codePoint << 6) | (next & 0x3f);
        }
        if (codePoint < 0x20 || (codePoint >= 0x7f && codePoint <= 0x9f) ||
            (codePoint >= 0x202a && codePoint <= 0x202e) ||
            (codePoint >= 0x2066 && codePoint <= 0x2069))
        {
            return true;
        }
        i += width;
    }
    return false;
}

const char *agentResultErrorName(AgentDemoService::AgentResult::Error error)
{
    switch (error)
    {
    case AgentDemoService::AgentResult::kNone: return "none";
    case AgentDemoService::AgentResult::kNotConfigured: return "not_configured";
    case AgentDemoService::AgentResult::kUpstreamTimeout: return "upstream_timeout";
    case AgentDemoService::AgentResult::kUpstreamAuthentication: return "upstream_authentication";
    case AgentDemoService::AgentResult::kUpstreamPaymentRequired: return "upstream_payment_required";
    case AgentDemoService::AgentResult::kUpstreamRateLimited: return "upstream_rate_limited";
    case AgentDemoService::AgentResult::kUpstreamRejectedRequest: return "upstream_rejected_request";
    case AgentDemoService::AgentResult::kUpstreamUnavailable: return "upstream_unavailable";
    case AgentDemoService::AgentResult::kUpstreamError: return "upstream_error";
    case AgentDemoService::AgentResult::kInvalidUpstreamResponse: return "invalid_upstream_response";
    case AgentDemoService::AgentResult::kRunDeadlineExceeded: return "run_deadline_exceeded";
    case AgentDemoService::AgentResult::kCancelled: return "cancelled";
    case AgentDemoService::AgentResult::kExecutionLimit: return "execution_limit";
    case AgentDemoService::AgentResult::kInternalError: return "internal_error";
    }
    return "unknown";
}

const char *agentResultPublicMessage(AgentDemoService::AgentResult::Error error)
{
    switch (error)
    {
    case AgentDemoService::AgentResult::kNotConfigured:
        return "agent model is not configured";
    case AgentDemoService::AgentResult::kUpstreamTimeout:
        return "agent upstream request timed out";
    case AgentDemoService::AgentResult::kUpstreamAuthentication:
        return "agent upstream authentication failed";
    case AgentDemoService::AgentResult::kUpstreamPaymentRequired:
        return "agent upstream account is unavailable";
    case AgentDemoService::AgentResult::kUpstreamRateLimited:
        return "agent upstream rate limit exceeded";
    case AgentDemoService::AgentResult::kUpstreamRejectedRequest:
        return "agent upstream rejected the request";
    case AgentDemoService::AgentResult::kUpstreamUnavailable:
        return "agent upstream is temporarily unavailable";
    case AgentDemoService::AgentResult::kUpstreamError:
        return "agent upstream request failed";
    case AgentDemoService::AgentResult::kInvalidUpstreamResponse:
        return "agent upstream returned an invalid response";
    case AgentDemoService::AgentResult::kRunDeadlineExceeded:
        return "agent run deadline was exceeded";
    case AgentDemoService::AgentResult::kCancelled:
        return "agent stream was cancelled";
    case AgentDemoService::AgentResult::kExecutionLimit:
        return "agent execution budget was exceeded";
    case AgentDemoService::AgentResult::kInternalError:
        return "internal agent error";
    case AgentDemoService::AgentResult::kNone:
        return "";
    }
    return "agent request failed";
}

void logAgentRun(const AgentDemoService::AgentResult &result)
{
    /*
     * 结构化日志只记录排障所需的元数据。禁止把用户消息、Session ID、Prompt、API Key、
     * tool arguments/output 写入这里，防止日志变成第二份隐私数据和凭据存储。
     * agent_trace 是一条按 run_id 关联的安全完成日志；虽然包含步骤明细，但当前没有
     * Span、父子关系、跨进程 Trace Context 或 Exporter，不是完整分布式 Trace。
     */
    nlohmann::json event;
    event["event"] = "agent.run.completed";
    event["run_id"] = result.runId;
    event["ok"] = result.ok();
    event["error"] = agentResultErrorName(result.error);
    event["queue_wait_ms"] = result.queueWaitMs;
    event["total_latency_ms"] = result.totalLatencyMs;
    event["model_latency_ms"] = result.metrics.modelLatencyMs;
    event["tool_latency_ms"] = result.metrics.toolLatencyMs;
    event["model_calls"] = result.metrics.modelExecutions.size();
    event["tool_calls"] = result.toolExecutions.size();
    event["history_load_ms"] = result.historyLoadMs;
    event["history_save_ms"] = result.historySaveMs;
    event["context_estimated_tokens"] = result.contextEstimatedTokens;
    event["context_recent_turns"] = result.contextRecentTurns;
    event["summary_used"] = result.summaryUsed;
    event["usage"] = {
        {"prompt_tokens", result.metrics.usage.promptTokens},
        {"completion_tokens", result.metrics.usage.completionTokens},
        {"total_tokens", result.metrics.usage.totalTokens},
        {"cache_hit_tokens", result.metrics.usage.promptCacheHitTokens},
        {"cache_miss_tokens", result.metrics.usage.promptCacheMissTokens},
        {"reasoning_tokens", result.metrics.usage.reasoningTokens}
    };
    event["model_steps"] = nlohmann::json::array();
    for (size_t i = 0; i < result.metrics.modelExecutions.size(); ++i)
    {
        const AgentModelExecution &step = result.metrics.modelExecutions[i];
        event["model_steps"].push_back({
            {"sequence", step.sequence},
            {"ok", step.success},
            {"error", agentModelErrorName(step.error)},
            {"provider_status", step.providerStatusCode},
            {"latency_ms", step.latencyMs},
            {"tokens", step.usage.totalTokens}
        });
    }
    event["tool_steps"] = nlohmann::json::array();
    for (size_t i = 0; i < result.toolExecutions.size(); ++i)
    {
        const AgentToolExecution &step = result.toolExecutions[i];
        event["tool_steps"].push_back({
            {"name", step.toolName},
            {"ok", step.success},
            {"latency_ms", step.latencyMs}
        });
    }
    LOG_INFO << "agent_trace " << event.dump();
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
    explicit CurlResponse(size_t limit = 1024 * 1024)
        : maxBytes(limit), tooLarge(false) {}
    std::string body;
    size_t maxBytes;
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
     * 这里借此实现调用方指定的硬上限：DeepSeek 非流式完整 Body 使用 3 MiB，Weather
     * 使用 16 KiB；DeepSeek Protocol 还分别限制 reasoning/content 为 1 MiB。
     */
    try
    {
        if (size != 0 && count > static_cast<size_t>(-1) / size)
        {
            return 0;
        }
        const size_t bytes = size * count;
        CurlResponse *response = static_cast<CurlResponse *>(userData);
        if (response->body.size() + bytes > response->maxBytes)
        {
            response->tooLarge = true;
            return 0;
        }
        response->body.append(data, bytes);
        return bytes;
    }
    catch (...)
    {
        // std::string::append 的分配异常不能越过 libcurl C ABI callback。
        return 0;
    }
}

/**
 * 使用 libcurl 查询天气。它运行在 Agent 业务 worker 中，因此同步等待不会阻塞
 * EventLoop。与旧 mywebserver 的 fork/exec curl 相比，这里不创建子进程，也不会
 * 把用户输入或命令参数交给 shell。
 */
bool queryWeather(const std::string &location, long remainingMs,
                   const AgentModelClient::CancelCheck &cancelled,
                   std::string *result, std::string *error, bool *timedOut)
{
    *timedOut = false;
    std::string normalizedLocation = trim(location);
    if (normalizedLocation.empty())
    {
        *error = "weather tool missing location";
        return false;
    }
    if (normalizedLocation.size() > 128)
    {
        *error = "weather location is too large";
        return false;
    }

    std::unique_ptr<CURL, CurlHandleDeleter> curl(::curl_easy_init());
    if (!curl)
    {
        *error = "curl_easy_init failed";
        return false;
    }

    // curl_easy_escape 按 URL path 规则编码空格、中文和保留字符。
    char *escapedRaw = ::curl_easy_escape(curl.get(), normalizedLocation.c_str(),
                                          static_cast<int>(normalizedLocation.size()));
    if (escapedRaw == NULL)
    {
        *error = "failed to encode weather location";
        return false;
    }
    std::unique_ptr<char, decltype(&::curl_free)> escaped(escapedRaw, &::curl_free);

    std::string baseUrl = getAgentDemoConfig().weatherApiBaseUrl;
    while (!baseUrl.empty() && baseUrl[baseUrl.size() - 1] == '/')
    {
        baseUrl.erase(baseUrl.size() - 1);
    }
    const std::string url = baseUrl + "/" + escaped.get() + "?format=3";

    CurlResponse response(16 * 1024);
    char curlError[CURL_ERROR_SIZE] = {0};
    ::curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    ::curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeCurlResponse);
    ::curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    ::curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    /*
     * 工具自身最多等待 8 秒，但它还必须服从整个 Agent Run 的统一 deadline。
     * 若前面的模型调用已经消耗大部分预算，Weather 不能重新获得完整 8 秒。
     */
    const long timeoutMs = std::max(1L, std::min(8000L, remainingMs));
    ::curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, timeoutMs);
    ::curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    ::curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, curlError);
    if (cancelled)
    {
        ::curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        ::curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION,
            +[](void *userData, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                const AgentModelClient::CancelCheck *check =
                    static_cast<const AgentModelClient::CancelCheck *>(userData);
                try
                {
                    return *check && (*check)() ? 1 : 0;
                }
                catch (...)
                {
                    return 1;
                }
            });
        ::curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &cancelled);
    }

    CURLcode code = ::curl_easy_perform(curl.get());
    long httpStatus = 0;
    ::curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &httpStatus);
    if (code != CURLE_OK)
    {
        *timedOut = code == CURLE_OPERATION_TIMEDOUT;
        if (response.tooLarge)
        {
            *error = "weather response exceeded 16 KiB";
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
        oss << "weather service returned HTTP " << httpStatus;
        *error = oss.str();
        return false;
    }

    *result = trim(response.body);
    if (result->empty())
    {
        *error = "weather service returned an empty response";
        return false;
    }
    return true;
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

bool containsOnly(const nlohmann::json &object,
                  const std::vector<std::string> &allowedKeys)
{
    for (auto it = object.begin(); it != object.end(); ++it)
    {
        if (std::find(allowedKeys.begin(), allowedKeys.end(), it.key()) == allowedKeys.end())
        {
            return false;
        }
    }
    return true;
}

class CalculatorTool : public AgentTool
{
public:
    std::string name() const override { return "calculator"; }
    bool isReadOnly() const override { return true; }

    std::string description() const override
    {
        return "Evaluate an arithmetic expression containing +, -, *, / and parentheses.";
    }

    nlohmann::json inputSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"expression", {
                    {"type", "string"},
                    {"description", "Arithmetic expression, for example (1+2)*3"}
                }}
            }},
            {"required", nlohmann::json::array({"expression"})},
            {"additionalProperties", false}
        };
    }

    AgentToolResult execute(const nlohmann::json &arguments,
                            const AgentToolContext &) const override
    {
        AgentToolResult result;
        if (!containsOnly(arguments, std::vector<std::string>(1, "expression")) ||
            !arguments.contains("expression") || !arguments["expression"].is_string())
        {
            result.errorMessage = "calculator requires only a string expression";
            return result;
        }

        const std::string expression = arguments["expression"].get<std::string>();
        if (expression.empty() || expression.size() > 4096)
        {
            result.errorMessage = "calculator expression must contain 1 to 4096 bytes";
            return result;
        }
        try
        {
            const double value = Calculator(expression).evaluate();
            if (!std::isfinite(value))
            {
                result.errorMessage = "calculator result is not finite";
                return result;
            }
            result.success = true;
            result.output = formatDouble(value);
        }
        catch (const std::exception &ex)
        {
            result.errorMessage = ex.what();
        }
        return result;
    }
};

class TimeTool : public AgentTool
{
public:
    std::string name() const override { return "time"; }
    bool isReadOnly() const override { return true; }

    std::string description() const override
    {
        return "Return the server local date and time.";
    }

    nlohmann::json inputSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", nlohmann::json::object()},
            {"additionalProperties", false}
        };
    }

    AgentToolResult execute(const nlohmann::json &arguments,
                            const AgentToolContext &) const override
    {
        AgentToolResult result;
        if (!arguments.empty())
        {
            result.errorMessage = "time does not accept arguments";
            return result;
        }
        result.success = true;
        result.output = currentTimeString();
        return result;
    }
};

class WeatherTool : public AgentTool
{
public:
    std::string name() const override { return "weather"; }
    bool isReadOnly() const override { return true; }

    std::string description() const override
    {
        return "Get the current weather for a city or location supplied by the user.";
    }

    nlohmann::json inputSchema() const override
    {
        return {
            {"type", "object"},
            {"properties", {
                {"location", {
                    {"type", "string"},
                    {"description", "City or location, for example Beijing"}
                }}
            }},
            {"required", nlohmann::json::array({"location"})},
            {"additionalProperties", false}
        };
    }

    AgentToolResult execute(const nlohmann::json &arguments,
                            const AgentToolContext &context) const override
    {
        AgentToolResult result;
        if (!containsOnly(arguments, std::vector<std::string>(1, "location")) ||
            !arguments.contains("location") || !arguments["location"].is_string())
        {
            result.errorMessage = "weather requires only a string location";
            return result;
        }

        bool timedOut = false;
        std::string internalError;
        if (!queryWeather(arguments["location"].get<std::string>(),
                          context.remainingMilliseconds(), context.cancelCheck,
                          &result.output,
                          &internalError, &timedOut))
        {
            // curl/DNS/TLS 细节不能进入模型上下文、HTTP、TCP 或工具执行轨迹。
            result.errorMessage = timedOut
                ? "weather service timed out" : "weather service request failed";
            return result;
        }
        result.success = true;
        return result;
    }
};

struct DeepSeekStreamContext
{
    DeepSeekStreamContext(const AgentModelClient::TextDeltaCallback &delta,
                          const AgentModelClient::ThinkingStateCallback &thinking,
                          const AgentModelClient::CancelCheck &cancel)
        : parser(delta, thinking), cancelCheck(cancel), parserStopped(false) {}

    DeepSeekSseParser parser;
    AgentModelClient::CancelCheck cancelCheck;
    bool parserStopped;
};

size_t writeDeepSeekStream(char *data, size_t size, size_t count, void *userData)
{
    if (size != 0 && count > static_cast<size_t>(-1) / size)
    {
        return 0;
    }
    const size_t bytes = size * count;
    DeepSeekStreamContext *context = static_cast<DeepSeekStreamContext *>(userData);
    try
    {
        std::string error;
        if (!context->parser.feed(data, bytes, &error))
        {
            context->parserStopped = true;
            return 0;
        }
        return bytes;
    }
    catch (...)
    {
        // 任何 C++ 异常都必须在 libcurl 的 C ABI callback 边界内终止。
        context->parserStopped = true;
        return 0;
    }
}

int checkDeepSeekStreamCancellation(void *userData, curl_off_t, curl_off_t,
                                    curl_off_t, curl_off_t)
{
    DeepSeekStreamContext *context = static_cast<DeepSeekStreamContext *>(userData);
    try
    {
        return context->cancelCheck && context->cancelCheck() ? 1 : 0;
    }
    catch (...)
    {
        return 1;
    }
}

class DeepSeekClient : public AgentModelClient
{
public:
    bool isConfigured() const override
    {
        return !getAgentDemoConfig().deepseekApiKey.empty() &&
               getAgentDemoConfig().deepseekApiKey != "YOUR_DEEPSEEK_API_KEY";
    }

    AgentModelResponse complete(
        const std::vector<nlohmann::json> &messages,
        const nlohmann::json &tools,
        long timeoutMs) const override
    {
        /*
         * 这是同步阻塞函数，但它只会由 BoundedThreadPool worker 调用。
         * “同步函数”并不等于“一定阻塞 Reactor”，关键看它运行在哪个线程。
         */
        AgentModelResponse result;
        const AgentDemoConfig &config = getAgentDemoConfig();
        if (config.deepseekApiKey.empty() || config.deepseekApiKey == "YOUR_DEEPSEEK_API_KEY")
        {
            result.error = AgentModelResponse::kNotConfigured;
            result.errorMessage = "deepseek_api_key is not configured";
            return result;
        }

        const nlohmann::json payloadJson =
            buildDeepSeekChatRequest(config.deepseekModel, messages, tools,
                                     config.deepseekThinkingEnabled);

        std::string payload;
        try
        {
            payload = payloadJson.dump();
        }
        catch (const std::exception &)
        {
            result.error = AgentModelResponse::kInvalidResponse;
            result.errorMessage = "failed to serialize DeepSeek request";
            return result;
        }

        std::unique_ptr<CURL, CurlHandleDeleter> curl(::curl_easy_init());
        if (!curl)
        {
            result.error = AgentModelResponse::kUpstreamError;
            result.errorMessage = "curl_easy_init failed";
            return result;
        }

        std::unique_ptr<curl_slist, CurlHeadersDeleter> headers(
            ::curl_slist_append(NULL, "Content-Type: application/json"));
        if (!headers)
        {
            result.error = AgentModelResponse::kUpstreamError;
            result.errorMessage = "failed to allocate HTTP headers";
            return result;
        }
        const std::string authorization =
            std::string("Authorization: Bearer ") + config.deepseekApiKey;
        struct curl_slist *newHeaders = ::curl_slist_append(headers.get(), authorization.c_str());
        if (newHeaders == NULL)
        {
            result.error = AgentModelResponse::kUpstreamError;
            result.errorMessage = "failed to allocate authorization header";
            return result;
        }
        // curl_slist_append 成功时仍返回链表头；release/reset 转移给同一 RAII 对象。
        headers.release();
        headers.reset(newHeaders);

        // Reasoning 和最终 Content 各自最多 1 MiB，外加 Tool Calls/Usage JSON 开销。
        CurlResponse response(3 * 1024 * 1024);
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
        ::curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS,
                           std::max(1L, std::min(30000L, timeoutMs)));
        ::curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        ::curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, curlError);

        // worker 会在这里等待 DNS/TCP/TLS/HTTP，但 IO loop 已经返回 epoll_wait。
        CURLcode code = ::curl_easy_perform(curl.get());
        long httpStatus = 0;
        ::curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &httpStatus);

        if (code != CURLE_OK)
        {
            result.providerStatusCode = static_cast<int>(httpStatus);
            result.error = code == CURLE_OPERATION_TIMEDOUT
                ? AgentModelResponse::kTimeout : AgentModelResponse::kUpstreamError;
            if (response.tooLarge)
            {
                result.errorMessage = "DeepSeek response exceeded 3 MiB";
            }
            else
            {
                result.errorMessage = curlError[0] != '\0'
                    ? curlError : ::curl_easy_strerror(code);
            }
            return result;
        }
        if (httpStatus != 0 && (httpStatus < 200 || httpStatus >= 300))
        {
            return classifyDeepSeekHttpError(httpStatus);
        }

        AgentModelResponse parsed = parseDeepSeekChatResponse(response.body);
        parsed.providerStatusCode = static_cast<int>(httpStatus);
        if (config.deepseekThinkingEnabled && parsed.ok() &&
            !parsed.toolCalls.empty() && !parsed.hasReasoningContent)
        {
            parsed.error = AgentModelResponse::kInvalidResponse;
            parsed.errorMessage =
                "DeepSeek thinking tool call omitted reasoning_content";
        }
        return parsed;
    }

    AgentModelResponse completeStreaming(
        const std::vector<nlohmann::json> &messages,
        const nlohmann::json &tools,
        long timeoutMs,
        const TextDeltaCallback &onDelta,
        const ThinkingStateCallback &onThinking,
        const CancelCheck &cancelled) const override
    {
        AgentModelResponse result;
        const AgentDemoConfig &config = getAgentDemoConfig();
        if (config.deepseekApiKey.empty() || config.deepseekApiKey == "YOUR_DEEPSEEK_API_KEY")
        {
            result.error = AgentModelResponse::kNotConfigured;
            result.errorMessage = "deepseek_api_key is not configured";
            return result;
        }
        if (cancelled && cancelled())
        {
            result.error = AgentModelResponse::kCancelled;
            result.errorMessage = "agent stream was cancelled";
            return result;
        }

        std::string payload;
        try
        {
            payload = buildDeepSeekStreamingRequest(
                config.deepseekModel, messages, tools,
                config.deepseekThinkingEnabled).dump();
        }
        catch (const std::exception &)
        {
            result.error = AgentModelResponse::kInvalidResponse;
            result.errorMessage = "failed to serialize DeepSeek streaming request";
            return result;
        }

        std::unique_ptr<CURL, CurlHandleDeleter> curl(::curl_easy_init());
        if (!curl)
        {
            result.error = AgentModelResponse::kUpstreamError;
            result.errorMessage = "curl_easy_init failed";
            return result;
        }
        std::unique_ptr<curl_slist, CurlHeadersDeleter> headers(
            ::curl_slist_append(NULL, "Content-Type: application/json"));
        if (!headers)
        {
            result.error = AgentModelResponse::kUpstreamError;
            result.errorMessage = "failed to allocate HTTP headers";
            return result;
        }
        const std::string authorization =
            std::string("Authorization: Bearer ") + config.deepseekApiKey;
        curl_slist *newHeaders = ::curl_slist_append(headers.get(), authorization.c_str());
        if (!newHeaders)
        {
            result.error = AgentModelResponse::kUpstreamError;
            result.errorMessage = "failed to allocate authorization header";
            return result;
        }
        headers.release();
        headers.reset(newHeaders);

        DeepSeekStreamContext context(onDelta, onThinking, cancelled);
        char curlError[CURL_ERROR_SIZE] = {0};
        ::curl_easy_setopt(curl.get(), CURLOPT_URL, config.deepseekApiUrl.c_str());
        ::curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
        ::curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
        ::curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, payload.data());
        ::curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                           static_cast<curl_off_t>(payload.size()));
        ::curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeDeepSeekStream);
        ::curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &context);
        ::curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 5000L);
        ::curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS,
                           std::max(1L, std::min(30000L, timeoutMs)));
        ::curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
        ::curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, curlError);
        ::curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        ::curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION,
                           checkDeepSeekStreamCancellation);
        ::curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &context);

        const CURLcode code = ::curl_easy_perform(curl.get());
        long httpStatus = 0;
        ::curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &httpStatus);

        /*
         * 先统一收尾 Parser，确保任何已经发出的 thinking.started 都有 completed。正常
         * DeepSeek 非 2xx 返回 JSON Error Body，Parser 结果随后会被 HTTP 错误类别覆盖；
         * 这也兼容“非标准上游先发 SSE reasoning、再以非 2xx 结束”的防御性场景。
         */
        AgentModelResponse parsed = context.parser.finish();
        parsed.providerStatusCode = static_cast<int>(httpStatus);
        if (httpStatus != 0 && (httpStatus < 200 || httpStatus >= 300))
        {
            return classifyDeepSeekHttpError(httpStatus);
        }
        if (config.deepseekThinkingEnabled && parsed.ok() &&
            !parsed.toolCalls.empty() && !parsed.hasReasoningContent)
        {
            parsed.error = AgentModelResponse::kInvalidResponse;
            parsed.errorMessage =
                "DeepSeek thinking tool call omitted reasoning_content";
        }
        if (code == CURLE_OK)
        {
            return parsed;
        }
        if ((cancelled && cancelled()) || parsed.error == AgentModelResponse::kCancelled)
        {
            parsed.error = AgentModelResponse::kCancelled;
            parsed.errorMessage = "agent stream was cancelled";
            return parsed;
        }
        if (context.parserStopped)
        {
            return parsed;
        }
        parsed.error = code == CURLE_OPERATION_TIMEDOUT
            ? AgentModelResponse::kTimeout : AgentModelResponse::kUpstreamError;
        parsed.errorMessage = code == CURLE_OPERATION_TIMEDOUT
            ? "DeepSeek streaming request timed out" : "DeepSeek streaming request failed";
        return parsed;
    }
};

std::shared_ptr<AgentRuntime> createAgentRuntime()
{
    std::shared_ptr<AgentToolRegistry> registry(new AgentToolRegistry());
    registry->registerTool(std::shared_ptr<AgentTool>(new CalculatorTool()));
    registry->registerTool(std::shared_ptr<AgentTool>(new TimeTool()));
    registry->registerTool(std::shared_ptr<AgentTool>(new WeatherTool()));

    std::shared_ptr<AgentModelClient> client(new DeepSeekClient());
    return std::shared_ptr<AgentRuntime>(new AgentRuntime(client, registry));
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
    , runtime_(createAgentRuntime())
    , conversationStore_(new SQLiteConversationStore(
          getAgentDemoConfig().conversationDatabasePath))
    , contextBuilder_(8000, 1200, 8)
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
    deleteSessionWhenIdle(tcpSessionId(conn));
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
            std::weak_ptr<TcpConnection> weakConnection(conn);
            SubmitStatus clearStatus = clearSessionAsync(
                tcpSessionId(conn),
                [weakConnection](bool cleared) {
                    TcpConnectionPtr connection = weakConnection.lock();
                    if (!connection || !connection->connected()) return;
                    connection->send(cleared
                        ? "Conversation cleared.\n> "
                        : "Failed to clear conversation.\n> ");
                });
            if (clearStatus == kSessionBusy)
            {
                conn->send("Conversation is busy. Try again later.\n> ");
            }
            else if (clearStatus != kAccepted)
            {
                conn->send("Agent is busy. Try again later.\n> ");
            }
            continue;
        }

        std::weak_ptr<TcpConnection> weakConnection(conn);
        /*
         * 后台任务可能晚于客户端断开完成。若强持有 shared_ptr，连接会被任务强行
         * 延寿；使用 weak_ptr，completion 执行时再 lock，连接不存在就丢弃结果。
         */
        SubmitStatus status = submit(
            tcpSessionId(conn), message,
            [this, weakConnection](AgentResult result) {
                TcpConnectionPtr connection = weakConnection.lock();
                if (!connection || !connection->connected())
                {
                    return;
                }
                if (!result.ok())
                {
                    connection->send(std::string("Agent error: ") +
                                     agentResultPublicMessage(result.error) +
                                     "\n[run_id] " + result.runId + "\n\n> ");
                    return;
                }
                connection->send(formatChatReply(result.answer, result.toolName,
                                                 result.toolResult) +
                                 "\n[run_id] " + result.runId + "\n\n> ");
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
    return submitInternal(sessionId, message, AgentEventCallback(),
                          AgentModelClient::CancelCheck(), completion, false);
}

AgentDemoService::SubmitStatus AgentDemoService::submitStreaming(
    const std::string &sessionId,
    const std::string &message,
    const AgentEventCallback &eventCallback,
    const AgentModelClient::CancelCheck &cancelled,
    Completion completion)
{
    if (!eventCallback || !cancelled)
    {
        return kInvalidRequest;
    }
    return submitInternal(sessionId, message, eventCallback, cancelled,
                          completion, true);
}

AgentDemoService::SubmitStatus AgentDemoService::submitInternal(
    const std::string &sessionId,
    const std::string &message,
    const AgentEventCallback &eventCallback,
    const AgentModelClient::CancelCheck &cancelled,
    Completion completion,
    bool streaming)
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
        const std::string runId = createAgentRunId();
        const std::chrono::steady_clock::time_point enqueuedAt =
            std::chrono::steady_clock::now();
        const std::chrono::steady_clock::time_point deadline = runtime_->deadlineFromNow();
        accepted = businessPool_.trySubmit(
            [this, session, sessionId, message, completion, deadline, runId, enqueuedAt,
             eventCallback, cancelled, streaming]() {
            // 以下代码运行在业务 worker，而不是连接所属 EventLoop。
            AgentResult result;
            const long queueWaitMs = elapsedMilliseconds(
                enqueuedAt, std::chrono::steady_clock::now());
            try
            {
                // deadline 在入队前创建，因此业务队列等待时间也计入 60 秒总预算。
                AgentEventCallback runEventCallback;
                if (streaming)
                {
                    runEventCallback = [eventCallback, runId](const AgentEvent &event) {
                        AgentEvent enriched = event;
                        enriched.data["run_id"] = runId;
                        return eventCallback(enriched);
                    };
                    AgentEvent started;
                    started.type = "run.started";
                    started.data = {
                        {"run_id", runId},
                        {"queue_wait_ms", queueWaitMs}
                    };
                    if (!eventCallback(started))
                    {
                        result.error = AgentResult::kCancelled;
                        result.errorMessage = "agent stream was cancelled";
                    }
                    else
                    {
                        result = runTurn(session, sessionId, runId, message, deadline,
                                         runEventCallback,
                                         cancelled, true);
                    }
                }
                else
                {
                    result = runTurn(session, sessionId, runId, message, deadline,
                                     AgentEventCallback(),
                                     AgentModelClient::CancelCheck(), false);
                }
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
            result.runId = runId;
            result.queueWaitMs = queueWaitMs;
            result.totalLatencyMs = elapsedMilliseconds(
                enqueuedAt, std::chrono::steady_clock::now());

            bool deleteAfterRun = false;
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                deleteAfterRun = session->deleteWhenIdle;
                if (!deleteAfterRun) session->inFlight = false;
            }
            if (deleteAfterRun)
            {
                try
                {
                    conversationStore_->deleteSession(sessionId);
                }
                catch (const std::exception &)
                {
                    LOG_ERROR << "failed to delete disconnected TCP conversation";
                }
                {
                    std::lock_guard<std::mutex> lock(session->mutex);
                    session->inFlight = false;
                }
                eraseSession(sessionId);
            }
            // 必须先复位 busy，再通知上层；否则 completion 后立即重试仍会得到 409。
            try
            {
                // 可观测性是旁路能力；即使 JSON 分配失败，也不能吞掉业务 completion。
                logAgentRun(result);
            }
            catch (...)
            {
                LOG_ERROR << "failed to serialize agent trace run_id=" << result.runId;
            }
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

AgentDemoService::SubmitStatus AgentDemoService::clearSessionAsync(
    const std::string &sessionId, std::function<void(bool)> completion)
{
    if (sessionId.empty() || sessionId.size() > 128 || !completion)
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
    if (!session) return kQueueFull;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->inFlight) return kSessionBusy;
        session->inFlight = true;
    }
    const bool accepted = businessPool_.trySubmit(
        [this, sessionId, session, completion]() {
            bool cleared = false;
            try
            {
                conversationStore_->deleteSession(sessionId);
                cleared = true;
            }
            catch (const std::exception &)
            {
                LOG_ERROR << "failed to clear persisted conversation";
            }
            {
                std::lock_guard<std::mutex> lock(session->mutex);
                session->inFlight = false;
            }
            completion(cleared);
        });
    if (!accepted)
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->inFlight = false;
        return kQueueFull;
    }
    return kAccepted;
}

AgentDemoService::SubmitStatus AgentDemoService::createHttpSessionAsync(
    const std::string &title, SessionCreateCompletion completion)
{
    const std::string normalizedTitle = trim(title);
    if (!completion || normalizedTitle.size() > 256)
    {
        return kInvalidRequest;
    }
    if (hasUnsafeTitleCodePoint(normalizedTitle)) return kInvalidRequest;
    const bool accepted = businessPool_.trySubmit(
        [this, normalizedTitle, completion]() {
        ConversationSessionInfo info;
        std::string error;
        bool created = false;
        try
        {
            info.sessionId = createChatSessionId();
            info.title = normalizedTitle.empty() ? "新聊天" : normalizedTitle;
            conversationStore_->createSession(
                std::string("http:") + info.sessionId, info.title);
            created = true;
        }
        catch (const std::exception &)
        {
            error = "failed to create chat session";
            LOG_ERROR << error;
        }
        completion(created, info, error);
    });
    return accepted ? kAccepted : kQueueFull;
}

AgentDemoService::SubmitStatus AgentDemoService::listHttpSessionsAsync(
    size_t limit, SessionListCompletion completion)
{
    if (!completion || limit == 0 || limit > 100)
    {
        return kInvalidRequest;
    }
    const bool accepted = businessPool_.trySubmit([this, limit, completion]() {
        std::vector<ConversationSessionInfo> sessions;
        std::string error;
        bool loaded = false;
        try
        {
            sessions = conversationStore_->listSessions("http:", limit);
            for (size_t i = 0; i < sessions.size(); ++i)
            {
                sessions[i].sessionId.erase(0, 5); // 隐藏 Store 内部 http: 命名空间。
            }
            loaded = true;
        }
        catch (const std::exception &)
        {
            error = "failed to list chat sessions";
            LOG_ERROR << error;
        }
        completion(loaded, sessions, error);
    });
    return accepted ? kAccepted : kQueueFull;
}

bool AgentDemoService::isConfigured() const
{
    return runtime_->isConfigured();
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

void AgentDemoService::deleteSessionWhenIdle(const std::string &sessionId)
{
    std::shared_ptr<Session> session;
    {
        // 保持项目统一锁顺序：全局 map mutex -> 单 Session mutex。
        std::lock_guard<std::mutex> mapLock(mutex_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) return;
        session = it->second;
        std::lock_guard<std::mutex> sessionLock(session->mutex);
        session->deleteWhenIdle = true;
        if (session->inFlight) return;
        session->inFlight = true;
    }

    const bool accepted = businessPool_.trySubmit([this, sessionId, session]() {
        try
        {
            conversationStore_->deleteSession(sessionId);
        }
        catch (const std::exception &)
        {
            LOG_ERROR << "failed to delete idle TCP conversation";
        }
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->inFlight = false;
        }
        eraseSession(sessionId);
    });
    if (!accepted)
    {
        // 队列过载时不能在 IO 线程退化为同步 SQLite；保留内存标记供后续任务收尾。
        std::lock_guard<std::mutex> lock(session->mutex);
        session->inFlight = false;
    }
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
    if (!session->inFlight && session.use_count() == 2)
    {
        sessions_.erase(it);
    }
}

AgentDemoService::AgentResult AgentDemoService::runTurn(
    const std::shared_ptr<Session> &session,
    const std::string &sessionId,
    const std::string &runId,
    const std::string &message,
    const std::chrono::steady_clock::time_point &deadline,
    const AgentEventCallback &eventCallback,
    const AgentModelClient::CancelCheck &cancelled,
    bool streaming)
{
    /*
     * Service 只负责取得 Session 快照并保存最终对话；Tool Calling 的消息状态机交给
     * AgentRuntime。这样 Session 锁不会覆盖任何模型或工具等待时间，其他 Session
     * 仍可由不同 worker 并行执行。
     */
    (void)session;
    const std::chrono::steady_clock::time_point loadBegin =
        std::chrono::steady_clock::now();
    // 0 表示全部未摘要 Turn；正常每次超过 8 个便压缩，所以常态只有 8-9 个。
    ConversationSnapshot snapshot = conversationStore_->load(sessionId, 0);

    /*
     * 每次成功保存后未摘要 Turn 通常最多为 9 个。达到阈值时，把较早完整 Turn 摘要，
     * 最近 8 个仍保留原文。摘要失败属于派生数据失败，不阻止本次使用原始历史。
     */
    if (snapshot.turns.size() > contextBuilder_.recentTurnsToKeep())
    {
        const size_t compactCount =
            snapshot.turns.size() - contextBuilder_.recentTurnsToKeep();
        std::vector<ConversationTurn> oldTurns(
            snapshot.turns.begin(), snapshot.turns.begin() + compactCount);
        const ConversationSummary summary =
            contextBuilder_.extendSummary(snapshot.summary, oldTurns);
        try
        {
            conversationStore_->saveSummary(sessionId, summary);
            snapshot.summary = summary;
            snapshot.turns.erase(snapshot.turns.begin(),
                                 snapshot.turns.begin() + compactCount);
        }
        catch (const std::exception &)
        {
            LOG_ERROR << "failed to save conversation summary";
        }
    }
    const ContextBuildResult context = contextBuilder_.build(snapshot);
    const long historyLoadMs = elapsedMilliseconds(
        loadBegin, std::chrono::steady_clock::now());

    const AgentRunResult runtimeResult = streaming
        ? runtime_->runStreamingUntil(context.history, message, deadline,
                                      eventCallback, cancelled)
        : runtime_->runUntil(context.history, message, deadline);
    if (streaming && cancelled && cancelled())
    {
        AgentRunResult cancelledResult = runtimeResult;
        cancelledResult.error = AgentRunResult::kCancelled;
        cancelledResult.errorMessage = "agent stream was cancelled";
        cancelledResult.answer.clear();
        AgentResult result = makeAgentResult(cancelledResult);
        result.historyLoadMs = historyLoadMs;
        result.contextEstimatedTokens = context.estimatedTokens;
        result.contextRecentTurns = context.recentTurns;
        result.summaryUsed = context.summaryUsed;
        return result;
    }
    AgentResult result = makeAgentResult(runtimeResult);
    result.historyLoadMs = historyLoadMs;
    result.contextEstimatedTokens = context.estimatedTokens;
    result.contextRecentTurns = context.recentTurns;
    result.summaryUsed = context.summaryUsed;
    if (result.ok())
    {
        const std::chrono::steady_clock::time_point saveBegin =
            std::chrono::steady_clock::now();
        ConversationTurn turn;
        /*
         * 只有成功 Run 才尝试生成持久化 Turn。当前用 runId 作为 turnId 便于把数据库
         * 历史与 agent_trace 关联；失败或取消 Run 不会成为 Conversation Turn。
         */
        turn.turnId = runId;
        turn.userMessage = message;
        turn.assistantMessage = result.answer;
        turn.toolExecutions = result.toolExecutions;
        TokenEstimator estimator;
        turn.estimatedTokens = estimator.estimateTurn(turn);
        try
        {
            conversationStore_->saveTurn(sessionId, turn);
        }
        catch (const std::exception &)
        {
            // 保留已经完成的模型、工具和 Token metrics，方便定位持久化故障。
            result.error = AgentResult::kInternalError;
            result.errorMessage = "failed to persist conversation turn";
        }
        result.historySaveMs = elapsedMilliseconds(
            saveBegin, std::chrono::steady_clock::now());
    }
    return result;
}

AgentDemoService::AgentResult AgentDemoService::makeAgentResult(
    const AgentRunResult &runtimeResult)
{
    AgentResult result;
    result.toolExecutions = runtimeResult.toolExecutions;
    result.metrics = runtimeResult.metrics;
    if (!runtimeResult.ok())
    {
        switch (runtimeResult.error)
        {
        case AgentRunResult::kNotConfigured:
            result.error = AgentResult::kNotConfigured;
            break;
        case AgentRunResult::kUpstreamTimeout:
            result.error = AgentResult::kUpstreamTimeout;
            break;
        case AgentRunResult::kUpstreamError:
            switch (runtimeResult.upstreamError)
            {
            case AgentModelResponse::kAuthentication:
                result.error = AgentResult::kUpstreamAuthentication;
                break;
            case AgentModelResponse::kPaymentRequired:
                result.error = AgentResult::kUpstreamPaymentRequired;
                break;
            case AgentModelResponse::kRateLimited:
                result.error = AgentResult::kUpstreamRateLimited;
                break;
            case AgentModelResponse::kRejectedRequest:
                result.error = AgentResult::kUpstreamRejectedRequest;
                break;
            case AgentModelResponse::kUnavailable:
                result.error = AgentResult::kUpstreamUnavailable;
                break;
            default:
                result.error = AgentResult::kUpstreamError;
                break;
            }
            break;
        case AgentRunResult::kInvalidModelResponse:
            result.error = AgentResult::kInvalidUpstreamResponse;
            break;
        case AgentRunResult::kDeadlineExceeded:
            result.error = AgentResult::kRunDeadlineExceeded;
            break;
        case AgentRunResult::kCancelled:
            result.error = AgentResult::kCancelled;
            break;
        case AgentRunResult::kBudgetExceeded:
            result.error = AgentResult::kExecutionLimit;
            break;
        case AgentRunResult::kInternalError:
        default:
            result.error = AgentResult::kInternalError;
            break;
        }
        result.errorMessage = runtimeResult.errorMessage;
        return result;
    }

    result.answer = normalizeMultiline(runtimeResult.answer);
    if (result.answer.empty())
    {
        result.error = AgentResult::kUpstreamError;
        result.errorMessage = "model returned an empty final answer";
        return result;
    }

    if (result.toolExecutions.empty())
    {
        result.toolName = "none";
    }
    else
    {
        const AgentToolExecution &last = result.toolExecutions.back();
        result.toolName = last.toolName;
        result.toolResult = last.output;
    }
    return result;
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
