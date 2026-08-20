#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "AgentRuntime.h"

/**
 * DeepSeek Provider 协议的纯函数层。
 *
 * HTTP、TLS 和 API Key 由 DeepSeekClient 处理；本层只负责标准结构与 Provider JSON
 * 之间的转换。纯函数不访问网络，因此可以直接用固定 fixture 测试 finish_reason、
 * assistant role、function type 和 thinking 开关等容易被 Fake Model 绕过的细节。
 *
 * Provider 接入分两层：DeepSeekProtocol 负责 JSON 编解码和协议校验；AgentDemo.cc 中
 * 的 DeepSeekClient 负责认证、HTTP/TLS、超时和 libcurl。两者共同实现
 * AgentModelClient 边界。
 */
nlohmann::json buildDeepSeekChatRequest(
    const std::string &model,
    const std::vector<nlohmann::json> &messages,
    const nlohmann::json &tools,
    bool thinkingEnabled = true);

AgentModelResponse parseDeepSeekChatResponse(const std::string &body);

// 将 DeepSeek HTTP 状态转换成稳定错误类别，不把上游 Body 或凭据回传给客户端。
AgentModelResponse classifyDeepSeekHttpError(long httpStatus);

nlohmann::json buildDeepSeekStreamingRequest(
    const std::string &model,
    const std::vector<nlohmann::json> &messages,
    const nlohmann::json &tools,
    bool thinkingEnabled = true);

/**
 * 增量解析 DeepSeek 返回的 SSE 字节流。feed() 可以接收任意 TCP/libcurl 分片：半行、
 * 一个完整事件或多个事件都可以。Parser 只在空行形成完整 SSE event 后解析 JSON，
 * 并按 tool_call index 聚合 id/name/arguments 分片。
 *
 * libcurl 已经处理 TCP、TLS 和 HTTP framing，所以 feed() 收到的是 Response Body 的
 * 任意分片，不含 HTTP Chunk Header。本 Parser 处理“上游 Provider SSE + DeepSeek
 * JSON”；下游客户端 SSE 由 HttpStreamResponder 重新编码，是另一条协议链路。
 */
class DeepSeekSseParser
{
public:
    explicit DeepSeekSseParser(
        const AgentModelClient::TextDeltaCallback &onDelta,
        const AgentModelClient::ThinkingStateCallback &onThinking =
            AgentModelClient::ThinkingStateCallback());

    bool feed(const char *data, size_t length, std::string *error);
    AgentModelResponse finish();

private:
    bool processLine(const std::string &line, std::string *error);
    bool processEvent(std::string *error);
    bool processJsonEvent(const nlohmann::json &event, std::string *error);
    bool completeThinking(std::string *error);

    AgentModelClient::TextDeltaCallback onDelta_;
    AgentModelClient::ThinkingStateCallback onThinking_;
    std::string pendingLine_;
    std::vector<std::string> dataLines_;
    size_t dataBytes_;
    AgentModelResponse response_;
    std::string finishReason_;
    bool done_;
    bool failed_;
    bool thinkingStarted_;
    bool thinkingCompleted_;
    bool answerOrToolStarted_;
    std::string errorMessage_;
};
