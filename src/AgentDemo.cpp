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
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "TcpConnection.h"
#include "EventLoop.h"
#include "ThreadPool.h"

namespace
{

std::string trim(const std::string &input)//去掉字符串开头和结尾的空白字符。
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

bool fileExists(const std::string &path)//查看文件是否存在/配置文件
{
    std::ifstream input(path.c_str());
    return input.good();
}

AgentDemoConfig loadAgentDemoConfig()//解析配置文件
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

    if (path.empty())
    {
        return config;
    }

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

    return config;
}

std::string toLower(std::string input)//字符转小写，目前没用
{
    std::transform(input.begin(), input.end(), input.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return input;
}

std::string jsonUnescape(const std::string &input)//把 JSON 字符串里的转义序列还原成普通字符。
{
    std::string output;
    output.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i)
    {
        if (input[i] != '\\' || i + 1 >= input.size())
        {
            output.push_back(input[i]);
            continue;
        }

        char escaped = input[++i];
        switch (escaped)
        {
        case '"':
            output.push_back('"');
            break;
        case '\\':
            output.push_back('\\');
            break;
        case '/':
            output.push_back('/');
            break;
        case 'b':
            output.push_back('\b');
            break;
        case 'f':
            output.push_back('\f');
            break;
        case 'n':
            output.push_back('\n');
            break;
        case 'r':
            output.push_back('\r');
            break;
        case 't':
            output.push_back('\t');
            break;
        default:
            output.push_back(escaped);
            break;
        }
    }
    return output;
}

bool jsonExtractObject(const std::string &text, std::string *objectText)//它是为了从 AI 返回的文本里粗略提取 JSON 对象，方便后面 jsonGetString() 读取 tool、input、answer 字段。
{
    size_t begin = text.find('{');
    size_t end = text.rfind('}');
    if (begin == std::string::npos || end == std::string::npos || end <= begin)
    {
        return false;
    }
    *objectText = text.substr(begin, end - begin + 1);
    return true;
}

bool runCommand(const std::vector<std::string> &args, std::string *output, int *exitCode)
{
    /*
    当前 WebServer 在处理 AI 请求时会 fork 出一个子进程，在子进程里通过 execvp 执行 curl，请求 DeepSeek API；父进程通过 pipe 读取 curl 的标准输出/标准错误，并通过 waitpid 等待子进程退出，然后解析返回内容。
    */
    int pipefd[2];
    if (::pipe(pipefd) < 0)
    {
        return false;
    }

    pid_t pid = ::fork();
    if (pid < 0)
    {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return false;
    }

    if (pid == 0)
    {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);

        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (size_t i = 0; i < args.size(); ++i)
        {
            argv.push_back(const_cast<char *>(args[i].c_str()));
        }
        argv.push_back(NULL);
        ::execvp(argv[0], &argv[0]);
        _exit(127);
    }

    ::close(pipefd[1]);
    std::string data;
    char buffer[4096];
    ssize_t bytesRead = 0;
    while ((bytesRead = ::read(pipefd[0], buffer, sizeof(buffer))) > 0)
    {
        data.append(buffer, static_cast<size_t>(bytesRead));
    }
    ::close(pipefd[0]);

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0)
    {
        return false;
    }

    *output = data;
    if (WIFEXITED(status))
    {
        *exitCode = WEXITSTATUS(status);
    }
    else
    {
        *exitCode = -1;
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

//这是核心方法。它封装了一次完整的 DeepSeek 调用流程：
class DeepSeekClient
{//DeepSeekClient 是一个很轻量的 DeepSeek API 客户端封装，只不过它没有直接用 HTTP 库，而是通过 curl 子进程完成 HTTP 请求。
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
              std::string *error) const
    {
        const AgentDemoConfig &config = getAgentDemoConfig();
        if (config.deepseekApiKey.empty() || config.deepseekApiKey == "YOUR_DEEPSEEK_API_KEY")
        {
            *error = "deepseek_api_key is not configured";
            return false;
        }

        char fileTemplate[] = "/tmp/deepseek_payload_XXXXXX";
        int fd = ::mkstemp(fileTemplate);
        if (fd < 0)
        {
            *error = "failed to create payload file";
            return false;
        }

        std::string payload =
            std::string("{\"model\":\"") + jsonEscape(config.deepseekModel) +
            "\",\"stream\":false,\"messages\":[{\"role\":\"system\",\"content\":\"" +
            jsonEscape(systemPrompt) + "\"},{\"role\":\"user\",\"content\":\"" +
            jsonEscape(userPrompt) + "\"}]}";

        bool writeOk = true;
        {
            std::ofstream payloadFile(fileTemplate);
            if (!payloadFile)
            {
                writeOk = false;
            }
            else
            {
                payloadFile << payload;
            }
        }
        ::close(fd);

        if (!writeOk)
        {
            ::unlink(fileTemplate);
            *error = "failed to write payload";
            return false;
        }

        std::vector<std::string> args;
        args.push_back("curl");
        args.push_back("--silent");
        args.push_back("--show-error");
        args.push_back("--fail-with-body");
        args.push_back(config.deepseekApiUrl);
        args.push_back("-H");
        args.push_back("Content-Type: application/json");
        args.push_back("-H");
        args.push_back(std::string("Authorization: Bearer ") + config.deepseekApiKey);
        args.push_back("--data-binary");
        args.push_back(std::string("@") + fileTemplate);

        std::string commandOutput;
        int exitCode = 0;
        bool ok = runCommand(args, &commandOutput, &exitCode);
        ::unlink(fileTemplate);

        *rawOutput = commandOutput;

        if (!ok)
        {
            *error = "failed to execute curl";
            return false;
        }

        if (exitCode != 0)
        {
            *error = trim(commandOutput);
            return false;
        }

        if (!jsonGetString(commandOutput, "content", content))
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

const AgentDemoConfig &getAgentDemoConfig()
{
    static AgentDemoConfig config = loadAgentDemoConfig();
    return config;
}

AgentDemoService::AgentDemoService()
    : workerPool_(new ThreadPool(2))
{
    workerPool_->start();
}

AgentDemoService::~AgentDemoService()
{
    workerPool_->stop();
}

void AgentDemoService::onConnection(const TcpConnectionPtr &conn)
{
    if (conn->connected())
    {
        std::cout << "Agent connection UP: " << conn->peerAddress().toIpPort() << std::endl;
        std::ostringstream welcome;
        welcome << "Agent Demo connected.\n"
                << "Config: " << getAgentDemoConfig().configPath << "\n"
                << "DeepSeek configured: " << (deepSeekClient().isConfigured() ? "yes" : "no") << "\n"
                << "Type one question per line. Commands: /health /clear /quit\n> ";
        conn->send(welcome.str());
        return;
    }

    std::cout << "Agent connection DOWN: " << conn->peerAddress().toIpPort() << std::endl;
    std::lock_guard<std::mutex> lock(mutex_);
    pendingRequests_.erase(conn->name());
    conversations_.erase(conn->name());
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
                << ", deepseek_configured=" << (deepSeekClient().isConfigured() ? "yes" : "no")
                << ", config=" << getAgentDemoConfig().configPath << "\n> ";
            conn->send(oss.str());
            continue;
        }

        if (message == "/clear")
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                conversations_[conn->name()].clear();
            }
            conn->send("Conversation cleared.\n> ");
            continue;
        }

        TcpConnectionPtr safeConn = conn;//这是为了保证异步任务执行期间，TcpConnection 对象不会提前析构。
        std::string connectionName = conn->name();
        bool submitted = workerPool_->submit([this, safeConn, connectionName, message]() {
            std::string answer = handleChatLine(connectionName, message);
            safeConn->getLoop()->queueInLoop([safeConn, answer]() {
                if (safeConn->connected())
                {
                    safeConn->send(answer + "\n\n> ");
                }
            });
        });

        if (!submitted)
        {
            conn->send("Agent worker pool is not available.\n\n> ");
        }
    }
}
//整个与ai交流的过程
std::string AgentDemoService::handleChatLine(const std::string &connectionName, const std::string &message)
{
    std::vector<ChatMessage> history;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        history = conversations_[connectionName];
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

    if (!deepSeekClient().chat(plannerPrompt, plannerUserPrompt, &plannerContent, &plannerOutput, &plannerError))
    {
        return std::string("DeepSeek planner request failed: ") + plannerError;
    }

    std::string plannerJson;
    if (!jsonExtractObject(plannerContent, &plannerJson))
    {
        std::string fallback = normalizeMultiline(plannerContent);
        appendHistory(connectionName, message, fallback);
        return formatChatReply(fallback, "none", "");
    }

    std::string toolName;
    if (!jsonGetString(plannerJson, "tool", &toolName))
    {
        std::string fallback = normalizeMultiline(plannerContent);
        appendHistory(connectionName, message, fallback);
        return formatChatReply(fallback, "none", "");
    }

    toolName = trim(toolName);
    if (toolName == "none")
    {
        std::string answer;
        if (!jsonGetString(plannerJson, "answer", &answer))
        {
            answer = plannerContent;
        }
        answer = normalizeMultiline(answer);
        appendHistory(connectionName, message, answer);
        return formatChatReply(answer, "none", "");
    }

    std::string toolResult;
    if (toolName == "calculator")
    {
        std::string expression;
        if (!jsonGetString(plannerJson, "input", &expression))
        {
            return "calculator tool missing input";
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
        return std::string("unsupported tool: ") + toolName;
    }

    std::string finalContent;
    std::string finalRaw;
    std::string finalError;
    std::ostringstream toolUserPrompt;
    toolUserPrompt << "User question: " << message << "\n"
                   << "Tool used: " << toolName << "\n"
                   << "Tool result: " << toolResult << "\n"
                   << "Please answer in concise Chinese.";

    if (!deepSeekClient().chat(
            "You are a helpful assistant. Use the provided tool result to answer accurately.",
            conversationContext.empty()
                ? toolUserPrompt.str()
                : std::string("Conversation history:\n") + conversationContext + "\n" + toolUserPrompt.str(),
            &finalContent,
            &finalRaw,
            &finalError))
    {
        finalContent = std::string("工具结果：") + toolResult;
    }
    finalContent = normalizeMultiline(finalContent);
    appendHistory(connectionName, message, finalContent);
    return formatChatReply(finalContent, toolName, toolResult);
}

std::string jsonEscape(const std::string &input)
{
    std::string output;
    output.reserve(input.size() + 16);
    for (size_t i = 0; i < input.size(); ++i)
    {
        unsigned char ch = static_cast<unsigned char>(input[i]);
        switch (ch)
        {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                char buffer[8] = {0};
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                output += buffer;
            }
            else
            {
                output.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return output;
}

bool jsonGetString(const std::string &json, const std::string &key, std::string *value)
{
    std::string pattern = "\"" + key + "\"";
    size_t keyPos = json.find(pattern);
    while (keyPos != std::string::npos)
    {
        size_t colonPos = json.find(':', keyPos + pattern.size());
        if (colonPos == std::string::npos)
        {
            return false;
        }

        size_t pos = colonPos + 1;
        while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])))
        {
            ++pos;
        }

        if (pos >= json.size() || json[pos] != '"')
        {
            keyPos = json.find(pattern, keyPos + pattern.size());
            continue;
        }

        ++pos;
        std::string raw;
        bool escaped = false;
        for (; pos < json.size(); ++pos)
        {
            char ch = json[pos];
            if (escaped)
            {
                raw.push_back('\\');
                raw.push_back(ch);
                escaped = false;
                continue;
            }

            if (ch == '\\')
            {
                escaped = true;
                continue;
            }

            if (ch == '"')
            {
                *value = jsonUnescape(raw);
                return true;
            }

            raw.push_back(ch);
        }
        return false;
    }

    return false;
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

void AgentDemoService::appendHistory(const std::string &connectionName,
                                     const std::string &userMessage,
                                     const std::string &assistantMessage)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ChatMessage> &history = conversations_[connectionName];
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
}
