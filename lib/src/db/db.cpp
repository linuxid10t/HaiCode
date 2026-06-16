#include <haicode/db.h>
#include <haicode/util.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <sstream>

namespace haicode {

static const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS session (
    id              TEXT PRIMARY KEY,
    project_id      TEXT NOT NULL DEFAULT '',
    title           TEXT NOT NULL DEFAULT '',
    directory       TEXT NOT NULL DEFAULT '',
    agent           TEXT NOT NULL DEFAULT '',
    model_json      TEXT NOT NULL DEFAULT '{}',
    cost            REAL NOT NULL DEFAULT 0,
    tok_input       INTEGER NOT NULL DEFAULT 0,
    tok_output      INTEGER NOT NULL DEFAULT 0,
    tok_reasoning   INTEGER NOT NULL DEFAULT 0,
    tok_cache_read  INTEGER NOT NULL DEFAULT 0,
    tok_cache_write INTEGER NOT NULL DEFAULT 0,
    time_created    INTEGER NOT NULL,
    time_updated    INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS session_message (
    id           TEXT PRIMARY KEY,
    session_id   TEXT NOT NULL REFERENCES session(id) ON DELETE CASCADE,
    type         TEXT NOT NULL,
    seq          INTEGER NOT NULL,
    data_json    TEXT NOT NULL DEFAULT '{}',
    time_created INTEGER NOT NULL,
    time_updated INTEGER NOT NULL,
    UNIQUE(session_id, seq)
);

CREATE TABLE IF NOT EXISTS permission (
    id           TEXT PRIMARY KEY,
    project_id   TEXT NOT NULL,
    action       TEXT NOT NULL,
    resource     TEXT NOT NULL,
    time_created INTEGER NOT NULL,
    UNIQUE(project_id, action, resource)
);

CREATE INDEX IF NOT EXISTS idx_session_updated ON session(time_updated DESC);
CREATE INDEX IF NOT EXISTS idx_msg_session_seq ON session_message(session_id, seq);
)SQL";

Database::Database(const std::string& path) {
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string err = db_ ? sqlite3_errmsg(db_) : "unknown";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("Failed to open database '" + path + "': " + err);
    }
    // Enable WAL for better concurrency
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
}

Database::~Database() {
    if (db_) sqlite3_close(db_);
}

Database::Database(Database&& other) noexcept : db_(other.db_) {
    other.db_ = nullptr;
}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        if (db_) sqlite3_close(db_);
        db_ = other.db_;
        other.db_ = nullptr;
    }
    return *this;
}

void Database::exec(const std::string& sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("SQL error: " + msg);
    }
}

void Database::migrate() {
    exec(kSchema);
}

// ---- SessionStore ----

SessionStore::SessionStore(Database& db) : db_(db) {}

SessionInfo SessionStore::create(const std::string& project_dir,
                                  const std::string& agent,
                                  const std::string& model_json) {
    SessionInfo s;
    s.id = util::make_id("ses");
    s.project_id = project_dir;  // use dir as project ID for now
    s.directory = project_dir;
    s.agent = agent;
    s.model_json = model_json;
    s.time_created = util::now_ms();
    s.time_updated = s.time_created;

    const char* sql =
        "INSERT INTO session (id, project_id, title, directory, agent, model_json,"
        " cost, tok_input, tok_output, tok_reasoning, tok_cache_read, tok_cache_write,"
        " time_created, time_updated)"
        " VALUES (?,?,?,?,?,?,0,0,0,0,0,0,?,?)";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.handle(), sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, s.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, s.project_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, s.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, s.directory.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, s.agent.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, s.model_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, s.time_created);
    sqlite3_bind_int64(stmt, 8, s.time_updated);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return s;
}

std::optional<SessionInfo> SessionStore::get(const std::string& session_id) {
    const char* sql =
        "SELECT id, project_id, title, directory, agent, model_json,"
        " cost, tok_input, tok_output, tok_reasoning, tok_cache_read, tok_cache_write,"
        " time_created, time_updated"
        " FROM session WHERE id=?";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.handle(), sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);

    SessionInfo s;
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = true;
        s.id         = (const char*)sqlite3_column_text(stmt, 0);
        s.project_id = (const char*)sqlite3_column_text(stmt, 1);
        s.title      = (const char*)sqlite3_column_text(stmt, 2);
        s.directory  = (const char*)sqlite3_column_text(stmt, 3);
        s.agent      = (const char*)sqlite3_column_text(stmt, 4);
        s.model_json = (const char*)sqlite3_column_text(stmt, 5);
        s.cost                = sqlite3_column_double(stmt, 6);
        s.tokens.input        = sqlite3_column_int(stmt, 7);
        s.tokens.output       = sqlite3_column_int(stmt, 8);
        s.tokens.reasoning    = sqlite3_column_int(stmt, 9);
        s.tokens.cache_read   = sqlite3_column_int(stmt, 10);
        s.tokens.cache_write  = sqlite3_column_int(stmt, 11);
        s.time_created        = sqlite3_column_int64(stmt, 12);
        s.time_updated        = sqlite3_column_int64(stmt, 13);
    }
    sqlite3_finalize(stmt);
    if (!found) return std::nullopt;
    return s;
}

std::vector<SessionInfo> SessionStore::list(int limit) {
    const char* sql =
        "SELECT id, project_id, title, directory, agent, model_json,"
        " cost, tok_input, tok_output, tok_reasoning, tok_cache_read, tok_cache_write,"
        " time_created, time_updated"
        " FROM session ORDER BY time_updated DESC LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.handle(), sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, limit);

    std::vector<SessionInfo> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SessionInfo s;
        s.id         = (const char*)sqlite3_column_text(stmt, 0);
        s.project_id = (const char*)sqlite3_column_text(stmt, 1);
        s.title      = (const char*)sqlite3_column_text(stmt, 2);
        s.directory  = (const char*)sqlite3_column_text(stmt, 3);
        s.agent      = (const char*)sqlite3_column_text(stmt, 4);
        s.model_json = (const char*)sqlite3_column_text(stmt, 5);
        s.cost                = sqlite3_column_double(stmt, 6);
        s.tokens.input        = sqlite3_column_int(stmt, 7);
        s.tokens.output       = sqlite3_column_int(stmt, 8);
        s.tokens.reasoning    = sqlite3_column_int(stmt, 9);
        s.tokens.cache_read   = sqlite3_column_int(stmt, 10);
        s.tokens.cache_write  = sqlite3_column_int(stmt, 11);
        s.time_created        = sqlite3_column_int64(stmt, 12);
        s.time_updated        = sqlite3_column_int64(stmt, 13);
        results.push_back(s);
    }
    sqlite3_finalize(stmt);
    return results;
}

void SessionStore::update_directory(const std::string& session_id, const std::string& directory) {
    const char* sql = "UPDATE session SET directory=?, project_id=?, time_updated=? WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.handle(), sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, directory.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, directory.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, util::now_ms());
    sqlite3_bind_text(stmt, 4, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void SessionStore::update_title(const std::string& session_id, const std::string& title) {
    const char* sql = "UPDATE session SET title=?, time_updated=? WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.handle(), sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, util::now_ms());
    sqlite3_bind_text(stmt, 3, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void SessionStore::update_cost(const std::string& session_id, double cost,
                                const TokenUsage& tokens) {
    const char* sql =
        "UPDATE session SET cost=cost+?, tok_input=tok_input+?, tok_output=tok_output+?,"
        " tok_reasoning=tok_reasoning+?, tok_cache_read=tok_cache_read+?,"
        " tok_cache_write=tok_cache_write+?, time_updated=? WHERE id=?";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.handle(), sql, -1, &stmt, nullptr);
    sqlite3_bind_double(stmt, 1, cost);
    sqlite3_bind_int(stmt, 2, tokens.input);
    sqlite3_bind_int(stmt, 3, tokens.output);
    sqlite3_bind_int(stmt, 4, tokens.reasoning);
    sqlite3_bind_int(stmt, 5, tokens.cache_read);
    sqlite3_bind_int(stmt, 6, tokens.cache_write);
    sqlite3_bind_int64(stmt, 7, util::now_ms());
    sqlite3_bind_text(stmt, 8, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void SessionStore::delete_session(const std::string& session_id) {
    const char* sql = "DELETE FROM session WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.handle(), sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void SessionStore::update_mode(const std::string& session_id, const std::string& mode_str) {
    // Read current model_json, patch the "mode" field, write it back.
    sqlite3_stmt* sel = nullptr;
    sqlite3_prepare_v2(db_.handle(),
        "SELECT model_json FROM session WHERE id=?", -1, &sel, nullptr);
    sqlite3_bind_text(sel, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);

    std::string model_json;
    bool found = false;
    if (sqlite3_step(sel) == SQLITE_ROW) {
        const unsigned char* txt = sqlite3_column_text(sel, 0);
        if (txt) model_json = reinterpret_cast<const char*>(txt);
        found = true;
    }
    sqlite3_finalize(sel);
    if (!found) return;

    try {
        auto j = nlohmann::json::parse(model_json, nullptr, false);
        if (j.is_discarded() || !j.is_object()) j = nlohmann::json::object();
        j["mode"] = mode_str;
        model_json = j.dump();
    } catch (...) {
        // Corrupt JSON — leave the row alone rather than wiping other fields.
        return;
    }

    sqlite3_stmt* upd = nullptr;
    sqlite3_prepare_v2(db_.handle(),
        "UPDATE session SET model_json=?, time_updated=? WHERE id=?", -1, &upd, nullptr);
    sqlite3_bind_text(upd, 1, model_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(upd, 2, util::now_ms());
    sqlite3_bind_text(upd, 3, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(upd);
    sqlite3_finalize(upd);
}

void SessionStore::update_provider_model(const std::string& session_id,
                                          const std::string& provider_id,
                                          const std::string& model_id) {
    // Read current model_json, patch the "id"/"provider_id" fields, write back.
    sqlite3_stmt* sel = nullptr;
    sqlite3_prepare_v2(db_.handle(),
        "SELECT model_json FROM session WHERE id=?", -1, &sel, nullptr);
    sqlite3_bind_text(sel, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);

    std::string model_json;
    bool found = false;
    if (sqlite3_step(sel) == SQLITE_ROW) {
        const unsigned char* txt = sqlite3_column_text(sel, 0);
        if (txt) model_json = reinterpret_cast<const char*>(txt);
        found = true;
    }
    sqlite3_finalize(sel);
    if (!found) return;

    try {
        auto j = nlohmann::json::parse(model_json, nullptr, false);
        if (j.is_discarded() || !j.is_object()) j = nlohmann::json::object();
        if (!provider_id.empty()) j["provider_id"] = provider_id;
        if (!model_id.empty())    j["id"]          = model_id;
        model_json = j.dump();
    } catch (...) {
        return;
    }

    sqlite3_stmt* upd = nullptr;
    sqlite3_prepare_v2(db_.handle(),
        "UPDATE session SET model_json=?, time_updated=? WHERE id=?", -1, &upd, nullptr);
    sqlite3_bind_text(upd, 1, model_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(upd, 2, util::now_ms());
    sqlite3_bind_text(upd, 3, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(upd);
    sqlite3_finalize(upd);
}

int SessionStore::next_seq(const std::string& session_id) {
    const char* sql =
        "SELECT COALESCE(MAX(seq),0)+1 FROM session_message WHERE session_id=?";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.handle(), sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    int seq = 1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        seq = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return seq;
}

void SessionStore::append_message(const std::string& session_id,
                                   const std::string& type,
                                   const std::string& data_json) {
    int seq = next_seq(session_id);
    int64_t now = util::now_ms();
    std::string id = util::make_id("msg");

    const char* sql =
        "INSERT INTO session_message (id, session_id, type, seq, data_json, time_created, time_updated)"
        " VALUES (?,?,?,?,?,?,?)";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.handle(), sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, seq);
    sqlite3_bind_text(stmt, 5, data_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, now);
    sqlite3_bind_int64(stmt, 7, now);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Update session's time_updated
    const char* upd = "UPDATE session SET time_updated=? WHERE id=?";
    sqlite3_prepare_v2(db_.handle(), upd, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, now);
    sqlite3_bind_text(stmt, 2, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<SessionMessage> SessionStore::load_messages(const std::string& session_id) {
    const char* sql =
        "SELECT id, session_id, type, seq, data_json, time_created, time_updated"
        " FROM session_message WHERE session_id=? ORDER BY seq ASC";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.handle(), sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<SessionMessage> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SessionMessage m;
        m.id           = (const char*)sqlite3_column_text(stmt, 0);
        m.session_id   = (const char*)sqlite3_column_text(stmt, 1);
        m.type         = (const char*)sqlite3_column_text(stmt, 2);
        m.seq          = sqlite3_column_int(stmt, 3);
        m.data_json    = (const char*)sqlite3_column_text(stmt, 4);
        m.time_created = sqlite3_column_int64(stmt, 5);
        m.time_updated = sqlite3_column_int64(stmt, 6);
        results.push_back(m);
    }
    sqlite3_finalize(stmt);
    return results;
}

} // namespace haicode
