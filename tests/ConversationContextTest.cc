#include "ConversationContext.h"

#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <sqlite3.h>

namespace
{

#define CHECK_TRUE(condition)                                                        \
    do                                                                               \
    {                                                                                \
        if (!(condition))                                                            \
        {                                                                            \
            throw std::runtime_error(std::string("check failed: ") + #condition);   \
        }                                                                            \
    } while (false)

std::string temporaryDatabasePath()
{
    char path[] = "/tmp/webserver-conversation-XXXXXX.db";
    const int descriptor = ::mkstemps(path, 3);
    if (descriptor < 0)
    {
        throw std::runtime_error("failed to create temporary database path");
    }
    ::close(descriptor);
    std::remove(path);
    return path;
}

ConversationTurn makeTurn(const std::string &id,
                          const std::string &user,
                          const std::string &assistant)
{
    ConversationTurn turn;
    turn.turnId = id;
    turn.userMessage = user;
    turn.assistantMessage = assistant;
    TokenEstimator estimator;
    turn.estimatedTokens = estimator.estimateTurn(turn);
    return turn;
}

void removeDatabaseFiles(const std::string &path)
{
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

void testPersistenceAndDelete()
{
    const std::string path = temporaryDatabasePath();
    removeDatabaseFiles(path);
    {
        SQLiteConversationStore store(path);
        store.createSession("http:empty-chat", "新聊天");
        store.createSession("tcp:temporary", "不应出现在 HTTP 列表");
        ConversationTurn first = makeTurn("run-1", "问题一", "回答一");
        AgentToolExecution tool;
        tool.toolCallId = "call-1";
        tool.toolName = "weather";
        tool.success = true;
        tool.output = "Sunny";
        tool.latencyMs = 3;
        first.toolExecutions.push_back(tool);
        CHECK_TRUE(store.saveTurn("session-a", first) == 1);
        CHECK_TRUE(store.saveTurn(
            "session-a", makeTurn("run-2", "问题二", "回答二")) == 2);
        bool duplicateRejected = false;
        try
        {
            store.saveTurn("session-a", makeTurn("run-2", "重复", "不应写入"));
        }
        catch (const std::exception &)
        {
            duplicateRejected = true;
        }
        CHECK_TRUE(duplicateRejected);
        // 唯一约束失败必须回滚整个事务，next_sequence 不能从 3 被错误推进到 4。
        CHECK_TRUE(store.saveTurn(
            "session-a", makeTurn("run-3", "问题三", "回答三")) == 3);
        CHECK_TRUE(store.saveTurn(
            "session-b", makeTurn("run-b", "另一个会话", "独立回答")) == 1);
        CHECK_TRUE(store.saveTurn(
            "http:empty-chat", makeTurn("chat-run", "C++ Reactor 如何工作？", "解释")) == 1);

        std::vector<ConversationSessionInfo> httpSessions =
            store.listSessions("http:", 10);
        CHECK_TRUE(httpSessions.size() == 1);
        CHECK_TRUE(httpSessions[0].sessionId == "http:empty-chat");
        CHECK_TRUE(httpSessions[0].title == "C++ Reactor 如何工作？");
        CHECK_TRUE(httpSessions[0].turnCount == 1);
    }

    // 重新构造 Store 等价于服务器重启，数据必须仍然存在。
    {
        SQLiteConversationStore reopened(path);
        ConversationSnapshot a = reopened.load("session-a", 20);
        CHECK_TRUE(a.turns.size() == 3);
        CHECK_TRUE(a.turns[0].sequence == 1);
        CHECK_TRUE(a.turns[0].toolExecutions.size() == 1);
        CHECK_TRUE(a.turns[1].assistantMessage == "回答二");
        CHECK_TRUE(a.turns[2].sequence == 3);
        ConversationSnapshot b = reopened.load("session-b", 20);
        CHECK_TRUE(b.turns.size() == 1);

        ConversationSummary summary;
        summary.version = 1;
        summary.coveredUntilSequence = 1;
        summary.content = "较早对话摘要";
        reopened.saveSummary("session-a", summary);
        ConversationSnapshot summarized = reopened.load("session-a", 20);
        CHECK_TRUE(summarized.summary.version == 1);
        CHECK_TRUE(summarized.turns.size() == 2);
        CHECK_TRUE(summarized.turns[0].sequence == 2);

        reopened.deleteSession("session-a");
        ConversationSnapshot deleted = reopened.load("session-a", 20);
        CHECK_TRUE(deleted.turns.empty());
        CHECK_TRUE(deleted.summary.content.empty());
        CHECK_TRUE(reopened.load("session-b", 20).turns.size() == 1);
    }
    removeDatabaseFiles(path);
}

void testContextBudgetAndTurnAtomicity()
{
    ConversationSnapshot snapshot;
    snapshot.summary.version = 1;
    snapshot.summary.coveredUntilSequence = 2;
    snapshot.summary.content = "用户正在学习 C++ Reactor，并要求中文注释。";
    for (int i = 0; i < 5; ++i)
    {
        ConversationTurn turn = makeTurn(
            std::string("turn-") + static_cast<char>('0' + i),
            std::string("user-") + std::string(30, static_cast<char>('a' + i)),
            std::string("assistant-") + std::string(40, static_cast<char>('A' + i)));
        turn.sequence = i + 3;
        snapshot.turns.push_back(turn);
    }

    ContextBuilder builder(80, 20, 2);
    ContextBuildResult result = builder.build(snapshot);
    CHECK_TRUE(result.estimatedTokens <= 80);
    CHECK_TRUE(result.history.size() % 2 == (result.summaryUsed ? 1 : 0));
    CHECK_TRUE(result.recentTurns > 0);
    // 最近 Turn 应优先保留，且 user/assistant 必须成对出现。
    CHECK_TRUE(result.history.back().content.find("assistant-") == 0);
    const size_t firstTurn = result.summaryUsed ? 1 : 0;
    for (size_t i = firstTurn; i < result.history.size(); i += 2)
    {
        CHECK_TRUE(result.history[i].role == "user");
        CHECK_TRUE(result.history[i + 1].role == "assistant");
    }
}

void testDeterministicSummaryAndUtf8Estimator()
{
    TokenEstimator estimator;
    CHECK_TRUE(estimator.estimateText("abcd") == 1);
    CHECK_TRUE(estimator.estimateText("中文") == 2);

    ContextBuilder builder(200, 40, 2);
    std::vector<ConversationTurn> oldTurns;
    ConversationTurn first = makeTurn("one", "第一问", "第一答");
    first.sequence = 1;
    oldTurns.push_back(first);
    ConversationTurn second = makeTurn("two", "第二问", "第二答");
    second.sequence = 2;
    oldTurns.push_back(second);

    ConversationSummary empty;
    ConversationSummary summary = builder.extendSummary(empty, oldTurns);
    CHECK_TRUE(summary.version == 1);
    CHECK_TRUE(summary.coveredUntilSequence == 2);
    CHECK_TRUE(!summary.content.empty());
    CHECK_TRUE(estimator.estimateText(summary.content) <= 40);
}

void testOldSchemaTitleMigration()
{
    const std::string path = temporaryDatabasePath();
    removeDatabaseFiles(path);
    sqlite3 *database = NULL;
    CHECK_TRUE(::sqlite3_open(path.c_str(), &database) == SQLITE_OK);
    const char *oldSessions =
        "CREATE TABLE sessions("
        "session_id TEXT PRIMARY KEY, created_at INTEGER NOT NULL, "
        "updated_at INTEGER NOT NULL, next_sequence INTEGER NOT NULL DEFAULT 1, "
        "summary_version INTEGER NOT NULL DEFAULT 0);";
    CHECK_TRUE(::sqlite3_exec(database, oldSessions, NULL, NULL, NULL) == SQLITE_OK);
    ::sqlite3_close(database);

    SQLiteConversationStore migrated(path);
    migrated.createSession("http:migrated", "迁移后的聊天");
    const std::vector<ConversationSessionInfo> sessions =
        migrated.listSessions("http:", 10);
    CHECK_TRUE(sessions.size() == 1);
    CHECK_TRUE(sessions[0].title == "迁移后的聊天");
    removeDatabaseFiles(path);
}

} // namespace

int main()
{
    try
    {
        testPersistenceAndDelete();
        testContextBudgetAndTurnAtomicity();
        testDeterministicSummaryAndUtf8Estimator();
        testOldSchemaTitleMigration();
    }
    catch (const std::exception &ex)
    {
        std::cerr << "ConversationContextTest failed: " << ex.what() << std::endl;
        return 1;
    }
    std::cout << "ConversationContextTest passed: 4 cases" << std::endl;
    return 0;
}
