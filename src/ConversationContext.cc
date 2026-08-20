#include "ConversationContext.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace
{

class SQLiteConnection
{
public:
    explicit SQLiteConnection(const std::string &path) : database_(NULL)
    {
        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                          SQLITE_OPEN_FULLMUTEX;
        if (::sqlite3_open_v2(path.c_str(), &database_, flags, NULL) != SQLITE_OK)
        {
            const std::string error = database_ ? ::sqlite3_errmsg(database_) :
                "sqlite3_open_v2 failed";
            if (database_) ::sqlite3_close(database_);
            database_ = NULL;
            throw std::runtime_error(error);
        }
        ::sqlite3_busy_timeout(database_, 3000);
        execute("PRAGMA foreign_keys=ON;");
    }

    ~SQLiteConnection()
    {
        if (database_) ::sqlite3_close(database_);
    }

    sqlite3 *get() const { return database_; }

    void execute(const char *sql) const
    {
        char *error = NULL;
        if (::sqlite3_exec(database_, sql, NULL, NULL, &error) != SQLITE_OK)
        {
            const std::string message = error ? error : "sqlite execution failed";
            ::sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

private:
    SQLiteConnection(const SQLiteConnection &);
    SQLiteConnection &operator=(const SQLiteConnection &);

    sqlite3 *database_;
};

class SQLiteStatement
{
public:
    SQLiteStatement(sqlite3 *database, const char *sql) : statement_(NULL)
    {
        if (::sqlite3_prepare_v2(database, sql, -1, &statement_, NULL) != SQLITE_OK)
        {
            throw std::runtime_error(::sqlite3_errmsg(database));
        }
    }

    ~SQLiteStatement()
    {
        if (statement_) ::sqlite3_finalize(statement_);
    }

    sqlite3_stmt *get() const { return statement_; }

    void bindText(int index, const std::string &value)
    {
        if (::sqlite3_bind_text(statement_, index, value.data(),
                                static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
        {
            throw std::runtime_error("failed to bind SQLite text");
        }
    }

    void bindInt64(int index, int64_t value)
    {
        if (::sqlite3_bind_int64(statement_, index, value) != SQLITE_OK)
        {
            throw std::runtime_error("failed to bind SQLite integer");
        }
    }

    void expectDone()
    {
        if (::sqlite3_step(statement_) != SQLITE_DONE)
        {
            throw std::runtime_error(::sqlite3_errmsg(
                ::sqlite3_db_handle(statement_)));
        }
    }

private:
    SQLiteStatement(const SQLiteStatement &);
    SQLiteStatement &operator=(const SQLiteStatement &);

    sqlite3_stmt *statement_;
};

int64_t unixMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string serializeTools(const std::vector<AgentToolExecution> &tools)
{
    nlohmann::json output = nlohmann::json::array();
    for (size_t i = 0; i < tools.size(); ++i)
    {
        output.push_back({
            {"id", tools[i].toolCallId},
            {"name", tools[i].toolName},
            {"ok", tools[i].success},
            {"output", tools[i].output},
            {"latency_ms", tools[i].latencyMs}
        });
    }
    return output.dump();
}

std::vector<AgentToolExecution> parseTools(const std::string &input)
{
    std::vector<AgentToolExecution> tools;
    if (input.empty()) return tools;
    const nlohmann::json parsed = nlohmann::json::parse(input);
    if (!parsed.is_array()) throw std::runtime_error("stored tool trace is not an array");
    for (size_t i = 0; i < parsed.size(); ++i)
    {
        AgentToolExecution tool;
        tool.toolCallId = parsed[i].at("id").get<std::string>();
        tool.toolName = parsed[i].at("name").get<std::string>();
        tool.success = parsed[i].at("ok").get<bool>();
        tool.output = parsed[i].at("output").get<std::string>();
        tool.latencyMs = parsed[i].at("latency_ms").get<long>();
        tools.push_back(tool);
    }
    return tools;
}

std::string columnText(sqlite3_stmt *statement, int column)
{
    const unsigned char *value = ::sqlite3_column_text(statement, column);
    const int bytes = ::sqlite3_column_bytes(statement, column);
    return value ? std::string(reinterpret_cast<const char *>(value), bytes) : std::string();
}

bool isUnsafeDisplayCodePoint(uint32_t codePoint)
{
    return codePoint < 0x20 || (codePoint >= 0x7f && codePoint <= 0x9f) ||
           (codePoint >= 0x202a && codePoint <= 0x202e) ||
           (codePoint >= 0x2066 && codePoint <= 0x2069);
}

uint32_t decodeUtf8CodePoint(const std::string &text, size_t begin, size_t width)
{
    const unsigned char first = static_cast<unsigned char>(text[begin]);
    if (width == 1) return first;
    uint32_t value = first & (0x7f >> width);
    for (size_t i = 1; i < width; ++i)
    {
        const unsigned char next = static_cast<unsigned char>(text[begin + i]);
        if ((next & 0xc0) != 0x80) return 0xfffd;
        value = (value << 6) | (next & 0x3f);
    }
    return value;
}

std::string defaultSessionTitle(const std::string &message)
{
    std::string title;
    size_t codePoints = 0;
    bool previousSpace = false;
    for (size_t i = 0; i < message.size() && codePoints < 36;)
    {
        const unsigned char ch = static_cast<unsigned char>(message[i]);
        size_t width = 1;
        if ((ch & 0xE0) == 0xC0) width = 2;
        else if ((ch & 0xF0) == 0xE0) width = 3;
        else if ((ch & 0xF8) == 0xF0) width = 4;
        if (i + width > message.size()) width = 1;

        const uint32_t codePoint = decodeUtf8CodePoint(message, i, width);
        if (isUnsafeDisplayCodePoint(codePoint))
        {
            if (!title.empty() && !previousSpace) title += ' ';
            previousSpace = true;
        }
        else if (width == 1 && std::isspace(ch))
        {
            if (!title.empty() && !previousSpace) title += ' ';
            previousSpace = true;
        }
        else
        {
            title.append(message, i, width);
            previousSpace = false;
            ++codePoints;
        }
        i += width;
    }
    while (!title.empty() && title[title.size() - 1] == ' ') title.erase(title.size() - 1);
    return title.empty() ? std::string("新聊天") : title;
}

bool sessionsTableHasTitle(sqlite3 *database)
{
    SQLiteStatement columns(database, "PRAGMA table_info(sessions);");
    int step = SQLITE_ROW;
    while ((step = ::sqlite3_step(columns.get())) == SQLITE_ROW)
    {
        if (columnText(columns.get(), 1) == "title") return true;
    }
    if (step != SQLITE_DONE) throw std::runtime_error(::sqlite3_errmsg(database));
    return false;
}

} // namespace

SQLiteConversationStore::SQLiteConversationStore(const std::string &databasePath)
    : databasePath_(databasePath)
{
    if (databasePath_.empty())
    {
        throw std::invalid_argument("conversation database path must not be empty");
    }
    initializeSchema();
}

void SQLiteConversationStore::initializeSchema()
{
    SQLiteConnection connection(databasePath_);
    connection.execute("PRAGMA journal_mode=WAL;");
    connection.execute(
        "CREATE TABLE IF NOT EXISTS sessions ("
        "session_id TEXT PRIMARY KEY,"
        "title TEXT NOT NULL DEFAULT '',"
        "created_at INTEGER NOT NULL,"
        "updated_at INTEGER NOT NULL,"
        "next_sequence INTEGER NOT NULL DEFAULT 1,"
        "summary_version INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS turns ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "session_id TEXT NOT NULL,"
        "sequence INTEGER NOT NULL,"
        "turn_id TEXT NOT NULL,"
        "user_message TEXT NOT NULL,"
        "assistant_message TEXT NOT NULL,"
        "tool_executions_json TEXT NOT NULL,"
        "estimated_tokens INTEGER NOT NULL,"
        "created_at INTEGER NOT NULL,"
        "UNIQUE(session_id, sequence),"
        "UNIQUE(session_id, turn_id),"
        "FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE"
        ");"
        "CREATE INDEX IF NOT EXISTS turns_session_sequence "
        "ON turns(session_id, sequence);"
        "CREATE TABLE IF NOT EXISTS conversation_summaries ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "session_id TEXT NOT NULL,"
        "version INTEGER NOT NULL,"
        "covered_until_sequence INTEGER NOT NULL,"
        "content TEXT NOT NULL,"
        "created_at INTEGER NOT NULL,"
        "UNIQUE(session_id, version),"
        "FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE"
        ");");
    connection.execute(
        "CREATE INDEX IF NOT EXISTS sessions_updated_order "
        "ON sessions(updated_at DESC, created_at DESC, session_id DESC);"
    );
    // Existing local databases predate chat-window management; migrate them in place.
    if (!sessionsTableHasTitle(connection.get()))
    {
        try
        {
            connection.execute(
                "ALTER TABLE sessions ADD COLUMN title TEXT NOT NULL DEFAULT '';"
            );
        }
        catch (const std::exception &)
        {
            // Overlapping startup may race between table_info and ALTER. Continue only if
            // the other process completed the same migration; all other errors remain fatal.
            if (!sessionsTableHasTitle(connection.get())) throw;
        }
    }
}

void SQLiteConversationStore::createSession(
    const std::string &sessionId, const std::string &title)
{
    SQLiteConnection connection(databasePath_);
    const int64_t now = unixMilliseconds();
    SQLiteStatement statement(connection.get(),
        "INSERT INTO sessions(session_id, title, created_at, updated_at) "
        "VALUES(?, ?, ?, ?);");
    statement.bindText(1, sessionId);
    statement.bindText(2, title);
    statement.bindInt64(3, now);
    statement.bindInt64(4, now);
    statement.expectDone();
}

std::vector<ConversationSessionInfo> SQLiteConversationStore::listSessions(
    const std::string &sessionPrefix, size_t limit) const
{
    std::vector<ConversationSessionInfo> sessions;
    if (limit == 0) return sessions;
    SQLiteConnection connection(databasePath_);
    SQLiteStatement statement(connection.get(),
        "WITH recent AS ("
        "SELECT session_id, title, created_at, updated_at FROM sessions "
        "WHERE session_id LIKE ? ESCAPE '\\' "
        "ORDER BY updated_at DESC, created_at DESC, session_id DESC LIMIT ?"
        ") "
        "SELECT r.session_id, r.title, r.created_at, r.updated_at, COUNT(t.id) "
        "FROM recent AS r LEFT JOIN turns AS t ON t.session_id=r.session_id "
        "GROUP BY r.session_id, r.title, r.created_at, r.updated_at "
        "ORDER BY r.updated_at DESC, r.created_at DESC, r.session_id DESC;");
    std::string escapedPrefix;
    for (size_t i = 0; i < sessionPrefix.size(); ++i)
    {
        if (sessionPrefix[i] == '%' || sessionPrefix[i] == '_' || sessionPrefix[i] == '\\')
            escapedPrefix += '\\';
        escapedPrefix += sessionPrefix[i];
    }
    statement.bindText(1, escapedPrefix + "%");
    statement.bindInt64(2, static_cast<int64_t>(limit));
    int step = SQLITE_ROW;
    while ((step = ::sqlite3_step(statement.get())) == SQLITE_ROW)
    {
        ConversationSessionInfo info;
        info.sessionId = columnText(statement.get(), 0);
        info.title = columnText(statement.get(), 1);
        if (info.title.empty()) info.title = "新聊天";
        info.createdAtMs = ::sqlite3_column_int64(statement.get(), 2);
        info.updatedAtMs = ::sqlite3_column_int64(statement.get(), 3);
        info.turnCount = static_cast<size_t>(::sqlite3_column_int64(statement.get(), 4));
        sessions.push_back(info);
    }
    if (step != SQLITE_DONE) throw std::runtime_error(::sqlite3_errmsg(connection.get()));
    return sessions;
}

ConversationSnapshot SQLiteConversationStore::load(
    const std::string &sessionId, size_t maxRecentTurns) const
{
    ConversationSnapshot snapshot;
    SQLiteConnection connection(databasePath_);

    SQLiteStatement summary(connection.get(),
        "SELECT version, covered_until_sequence, content "
        "FROM conversation_summaries WHERE session_id=? "
        "ORDER BY version DESC LIMIT 1;");
    summary.bindText(1, sessionId);
    const int summaryStep = ::sqlite3_step(summary.get());
    if (summaryStep == SQLITE_ROW)
    {
        snapshot.summary.version = ::sqlite3_column_int64(summary.get(), 0);
        snapshot.summary.coveredUntilSequence = ::sqlite3_column_int64(summary.get(), 1);
        snapshot.summary.content = columnText(summary.get(), 2);
    }
    else if (summaryStep != SQLITE_DONE)
    {
        throw std::runtime_error(::sqlite3_errmsg(connection.get()));
    }

    SQLiteStatement turns(connection.get(),
        "SELECT sequence, turn_id, user_message, assistant_message, "
        "tool_executions_json, estimated_tokens FROM ("
        "SELECT sequence, turn_id, user_message, assistant_message, "
        "tool_executions_json, estimated_tokens FROM turns "
        "WHERE session_id=? AND sequence>? ORDER BY sequence DESC "
        "LIMIT CASE WHEN ?=0 THEN -1 ELSE ? END"
        ") ORDER BY sequence ASC;");
    turns.bindText(1, sessionId);
    turns.bindInt64(2, snapshot.summary.coveredUntilSequence);
    turns.bindInt64(3, static_cast<int64_t>(maxRecentTurns));
    turns.bindInt64(4, static_cast<int64_t>(maxRecentTurns));
    int turnStep = SQLITE_ROW;
    while ((turnStep = ::sqlite3_step(turns.get())) == SQLITE_ROW)
    {
        ConversationTurn turn;
        turn.sequence = ::sqlite3_column_int64(turns.get(), 0);
        turn.turnId = columnText(turns.get(), 1);
        turn.userMessage = columnText(turns.get(), 2);
        turn.assistantMessage = columnText(turns.get(), 3);
        turn.toolExecutions = parseTools(columnText(turns.get(), 4));
        turn.estimatedTokens = static_cast<size_t>(::sqlite3_column_int64(turns.get(), 5));
        snapshot.turns.push_back(turn);
    }
    if (turnStep != SQLITE_DONE)
    {
        throw std::runtime_error(::sqlite3_errmsg(connection.get()));
    }
    return snapshot;
}

int64_t SQLiteConversationStore::saveTurn(
    const std::string &sessionId, const ConversationTurn &turn)
{
    SQLiteConnection connection(databasePath_);
    /*
     * BEGIN IMMEDIATE 在事务开始时取得写入意向，尽早暴露并发写竞争。下面的 Session
     * 创建、sequence 分配、Turn 插入和 next_sequence 更新必须一起 COMMIT，任何一步
     * 失败都一起 ROLLBACK，避免“消息写入了但序号没推进”或相反情况。
     */
    connection.execute("BEGIN IMMEDIATE;");
    try
    {
        const int64_t now = unixMilliseconds();
        SQLiteStatement createSession(connection.get(),
            "INSERT OR IGNORE INTO sessions(session_id, title, created_at, updated_at) "
            "VALUES(?, '', ?, ?);");
        createSession.bindText(1, sessionId);
        createSession.bindInt64(2, now);
        createSession.bindInt64(3, now);
        createSession.expectDone();

        SQLiteStatement sequenceStatement(connection.get(),
            "SELECT next_sequence FROM sessions WHERE session_id=?;");
        sequenceStatement.bindText(1, sessionId);
        if (::sqlite3_step(sequenceStatement.get()) != SQLITE_ROW)
        {
            throw std::runtime_error("failed to allocate conversation sequence");
        }
        const int64_t sequence = ::sqlite3_column_int64(sequenceStatement.get(), 0);
        const std::string generatedTitle = defaultSessionTitle(turn.userMessage);

        SQLiteStatement insert(connection.get(),
            "INSERT INTO turns(session_id, sequence, turn_id, user_message, "
            "assistant_message, tool_executions_json, estimated_tokens, created_at) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, ?);");
        insert.bindText(1, sessionId);
        insert.bindInt64(2, sequence);
        insert.bindText(3, turn.turnId);
        insert.bindText(4, turn.userMessage);
        insert.bindText(5, turn.assistantMessage);
        insert.bindText(6, serializeTools(turn.toolExecutions));
        insert.bindInt64(7, static_cast<int64_t>(turn.estimatedTokens));
        insert.bindInt64(8, now);
        insert.expectDone();

        SQLiteStatement update(connection.get(),
            "UPDATE sessions SET next_sequence=next_sequence+1, updated_at=?, "
            "title=CASE WHEN title='' OR title='新聊天' THEN ? ELSE title END "
            "WHERE session_id=?;");
        update.bindInt64(1, now);
        update.bindText(2, generatedTitle);
        update.bindText(3, sessionId);
        update.expectDone();
        connection.execute("COMMIT;");
        return sequence;
    }
    catch (...)
    {
        try { connection.execute("ROLLBACK;"); } catch (...) {}
        throw;
    }
}

void SQLiteConversationStore::saveSummary(
    const std::string &sessionId, const ConversationSummary &summary)
{
    SQLiteConnection connection(databasePath_);
    connection.execute("BEGIN IMMEDIATE;");
    try
    {
        SQLiteStatement insert(connection.get(),
            "INSERT INTO conversation_summaries(session_id, version, "
            "covered_until_sequence, content, created_at) VALUES(?, ?, ?, ?, ?);");
        insert.bindText(1, sessionId);
        insert.bindInt64(2, summary.version);
        insert.bindInt64(3, summary.coveredUntilSequence);
        insert.bindText(4, summary.content);
        insert.bindInt64(5, unixMilliseconds());
        insert.expectDone();

        SQLiteStatement update(connection.get(),
            "UPDATE sessions SET summary_version=?, updated_at=? WHERE session_id=?;");
        update.bindInt64(1, summary.version);
        update.bindInt64(2, unixMilliseconds());
        update.bindText(3, sessionId);
        update.expectDone();
        connection.execute("COMMIT;");
    }
    catch (...)
    {
        try { connection.execute("ROLLBACK;"); } catch (...) {}
        throw;
    }
}

void SQLiteConversationStore::deleteSession(const std::string &sessionId)
{
    SQLiteConnection connection(databasePath_);
    SQLiteStatement statement(connection.get(),
        "DELETE FROM sessions WHERE session_id=?;");
    statement.bindText(1, sessionId);
    statement.expectDone();
}

size_t TokenEstimator::estimateText(const std::string &text) const
{
    size_t ascii = 0;
    size_t nonAsciiCodePoints = 0;
    for (size_t i = 0; i < text.size();)
    {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch < 0x80)
        {
            ++ascii;
            ++i;
            continue;
        }
        size_t width = 1;
        if ((ch & 0xE0) == 0xC0) width = 2;
        else if ((ch & 0xF0) == 0xE0) width = 3;
        else if ((ch & 0xF8) == 0xF0) width = 4;
        if (i + width > text.size()) width = 1;
        ++nonAsciiCodePoints;
        i += width;
    }
    return (ascii + 3) / 4 + nonAsciiCodePoints;
}

size_t TokenEstimator::estimateMessage(const AgentConversationMessage &message) const
{
    return 4 + estimateText(message.role) + estimateText(message.content);
}

size_t TokenEstimator::estimateTurn(const ConversationTurn &turn) const
{
    AgentConversationMessage user;
    user.role = "user";
    user.content = turn.userMessage;
    AgentConversationMessage assistant;
    assistant.role = "assistant";
    assistant.content = turn.assistantMessage;
    return estimateMessage(user) + estimateMessage(assistant);
}

ContextBuilder::ContextBuilder(size_t historyTokenBudget,
                               size_t summaryTokenBudget,
                               size_t recentTurnsToKeep)
    : historyTokenBudget_(historyTokenBudget)
    , summaryTokenBudget_(summaryTokenBudget)
    , recentTurnsToKeep_(recentTurnsToKeep)
{
    if (historyTokenBudget_ == 0 || summaryTokenBudget_ == 0 ||
        recentTurnsToKeep_ == 0 || summaryTokenBudget_ >= historyTokenBudget_)
    {
        throw std::invalid_argument("invalid conversation context budgets");
    }
}

ContextBuildResult ContextBuilder::build(const ConversationSnapshot &snapshot) const
{
    // 本函数只构造历史切片；完整请求还会由 Runtime 加入 system、当前 user 和 tools。
    ContextBuildResult result;
    size_t available = historyTokenBudget_;

    std::vector<ConversationTurn> selected;
    for (size_t offset = 0; offset < snapshot.turns.size(); ++offset)
    {
        const ConversationTurn &turn = snapshot.turns[snapshot.turns.size() - 1 - offset];
        // 不信任数据库缓存值作为安全预算，加载时根据真实文本重新估算。
        const size_t tokens = estimator_.estimateTurn(turn);
        if (tokens > available)
        {
            break; // Turn 是原子单位，不能只保留 user 或只保留 assistant。
        }
        selected.push_back(turn);
        available -= tokens;
        result.estimatedTokens += tokens;
    }
    std::reverse(selected.begin(), selected.end());

    if (!snapshot.summary.content.empty())
    {
        AgentConversationMessage summary;
        /*
         * 摘要包含用户原文，不能提升成 system 权限，否则历史中的 Prompt Injection
         * 会获得比当前用户更高的优先级。assistant 角色仅作为不可信历史参考。
         */
        summary.role = "assistant";
        const std::string prefix =
            "Earlier conversation excerpts (untrusted reference data):\n";
        summary.content = prefix;
        const size_t fixedTokens = estimator_.estimateMessage(summary);
        const size_t contentBudget = available > fixedTokens
            ? std::min(summaryTokenBudget_, available - fixedTokens) : 0;
        summary.content += truncateToTokenBudget(snapshot.summary.content, contentBudget);
        const size_t tokens = estimator_.estimateMessage(summary);
        if (contentBudget > 0 && tokens <= available)
        {
            result.history.push_back(summary);
            result.estimatedTokens += tokens;
            available -= tokens;
            result.summaryUsed = true;
        }
    }

    for (size_t i = 0; i < selected.size(); ++i)
    {
        AgentConversationMessage user;
        user.role = "user";
        user.content = selected[i].userMessage;
        result.history.push_back(user);
        AgentConversationMessage assistant;
        assistant.role = "assistant";
        assistant.content = selected[i].assistantMessage;
        result.history.push_back(assistant);
    }
    result.recentTurns = selected.size();
    return result;
}

ConversationSummary ContextBuilder::extendSummary(
    const ConversationSummary &existing,
    const std::vector<ConversationTurn> &turns) const
{
    // 这是确定性历史压缩，不会提取长期事实，也不会按查询检索，因此不是 Memory/RAG。
    ConversationSummary summary = existing;
    if (turns.empty()) return summary;

    std::ostringstream text;
    if (!existing.content.empty())
    {
        text << existing.content << "\n";
    }
    for (size_t i = 0; i < turns.size(); ++i)
    {
        text << "- User: " << truncateToTokenBudget(turns[i].userMessage, 80) << "\n"
             << "  Assistant: " << truncateToTokenBudget(turns[i].assistantMessage, 120)
             << "\n";
        summary.coveredUntilSequence = turns[i].sequence;
    }
    summary.version = existing.version + 1;
    const std::string combined = text.str();
    if (estimator_.estimateText(combined) <= summaryTokenBudget_)
    {
        summary.content = combined;
    }
    else
    {
        /*
         * 滚动摘要达到上限时优先保留末尾较新的决策。这里先找到 UTF-8 code point
         * 边界，再复用前向截断，避免长期只留下最早会话而新信息永远进不来。
         */
        size_t begin = combined.size();
        size_t tokens = 0;
        while (begin > 0 && tokens < summaryTokenBudget_ - 1)
        {
            size_t previous = begin - 1;
            while (previous > 0 &&
                   (static_cast<unsigned char>(combined[previous]) & 0xC0) == 0x80)
            {
                --previous;
            }
            begin = previous;
            ++tokens;
        }
        summary.content = std::string("...") + combined.substr(begin);
    }
    return summary;
}

std::string ContextBuilder::truncateToTokenBudget(
    const std::string &text, size_t tokenBudget) const
{
    if (tokenBudget == 0) return std::string();
    if (estimator_.estimateText(text) <= tokenBudget) return text;

    // 按 UTF-8 code point 追加，避免在中文多字节编码中间截断。
    std::string output;
    for (size_t i = 0; i < text.size();)
    {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        size_t width = 1;
        if ((ch & 0xE0) == 0xC0) width = 2;
        else if ((ch & 0xF0) == 0xE0) width = 3;
        else if ((ch & 0xF8) == 0xF0) width = 4;
        if (i + width > text.size()) width = 1;
        const std::string piece = text.substr(i, width);
        if (estimator_.estimateText(output + piece) + 1 > tokenBudget) break;
        output += piece;
        i += width;
    }
    output += "...";
    return output;
}
