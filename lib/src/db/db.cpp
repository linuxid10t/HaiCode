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
    tok_last_input  INTEGER NOT NULL DEFAULT 0,
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

CREATE TABLE IF NOT EXISTS session_todo (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id   TEXT NOT NULL REFERENCES session(id) ON DELETE CASCADE,
    position     INTEGER NOT NULL,
    content      TEXT NOT NULL,
    active_form  TEXT NOT NULL DEFAULT '',
    status       TEXT NOT NULL DEFAULT 'pending',
    time_created INTEGER NOT NULL,
    time_updated INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS compaction_checkpoint (
    id                     TEXT PRIMARY KEY,
    session_id             TEXT NOT NULL REFERENCES session(id) ON DELETE CASCADE,
    through_seq            INTEGER NOT NULL,
    summary                TEXT NOT NULL DEFAULT '',
    recent_context         TEXT NOT NULL DEFAULT '',
    previous_checkpoint_id TEXT NOT NULL DEFAULT '',
    status                 TEXT NOT NULL DEFAULT 'pending',
    time_created           INTEGER NOT NULL,
    time_updated           INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_checkpoint_session
    ON compaction_checkpoint(session_id, through_seq DESC);

CREATE INDEX IF NOT EXISTS idx_session_updated ON session(time_updated DESC);
CREATE INDEX IF NOT EXISTS idx_msg_session_seq ON session_message(session_id, seq);
CREATE INDEX IF NOT EXISTS idx_todo_session_pos ON session_todo(session_id, position);
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
    // Column additions for pre-existing databases. SQLite has no ADD COLUMN
    // IF NOT EXISTS, so check pragma table_info first.
    auto has_column = [&](const char* table, const char* col) {
        std::string q = std::string("PRAGMA table_info(") + table + ")";
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db_, q.c_str(), -1, &st, nullptr);
        bool found = false;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char* name = (const char*)sqlite3_column_text(st, 1);
            if (name && strcmp(name, col) == 0) { found = true; break; }
        }
        sqlite3_finalize(st);
        return found;
    };
    if (!has_column("session", "tok_last_input"))
        exec("ALTER TABLE session ADD COLUMN tok_last_input INTEGER NOT NULL DEFAULT 0");
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
        " tok_last_input, time_created, time_updated"
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
        s.last_input_tokens   = sqlite3_column_int(stmt, 12);
        s.time_created        = sqlite3_column_int64(stmt, 13);
        s.time_updated        = sqlite3_column_int64(stmt, 14);
    }
    sqlite3_finalize(stmt);
    if (!found) return std::nullopt;
    return s;
}

std::vector<SessionInfo> SessionStore::list(int limit) {
    const char* sql =
        "SELECT id, project_id, title, directory, agent, model_json,"
        " cost, tok_input, tok_output, tok_reasoning, tok_cache_read, tok_cache_write,"
        " tok_last_input, time_created, time_updated"
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
        s.last_input_tokens   = sqlite3_column_int(stmt, 12);
        s.time_created        = sqlite3_column_int64(stmt, 13);
        s.time_updated        = sqlite3_column_int64(stmt, 14);
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
        " tok_cache_write=tok_cache_write+?,"
        " tok_last_input=?, time_updated=? WHERE id=?";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.handle(), sql, -1, &stmt, nullptr);
    sqlite3_bind_double(stmt, 1, cost);
    sqlite3_bind_int(stmt, 2, tokens.input);
    sqlite3_bind_int(stmt, 3, tokens.output);
    sqlite3_bind_int(stmt, 4, tokens.reasoning);
    sqlite3_bind_int(stmt, 5, tokens.cache_read);
    sqlite3_bind_int(stmt, 6, tokens.cache_write);
    // Per-request input size (not cumulative) — seeds the context meter on
    // session reopen. Matches the engine's prev_total_input arithmetic.
    sqlite3_bind_int(stmt, 7, tokens.input + tokens.cache_read + tokens.cache_write);
    sqlite3_bind_int64(stmt, 8, util::now_ms());
    sqlite3_bind_text(stmt, 9, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void SessionStore::update_last_input_tokens(const std::string& session_id,
                                            int tokens) {
    const char* sql =
        "UPDATE session SET tok_last_input=?, time_updated=? WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.handle(), sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, tokens);
    sqlite3_bind_int64(stmt, 2, util::now_ms());
    sqlite3_bind_text(stmt, 3, session_id.c_str(), -1, SQLITE_TRANSIENT);
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

void SessionStore::update_permission_flags(const std::string& session_id,
                                           bool auto_edits, bool yolo,
                                           bool read_everywhere) {
    // Read current model_json, patch the flag fields, write it back.
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
        j["auto_edits"] = auto_edits;
        j["yolo"] = yolo;
        j["allow_read_everywhere"] = read_everywhere;
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

void SessionStore::update_inference(const std::string& session_id,
                                    const InferenceParams& params) {
    // Read current model_json, patch the inference fields, write back.
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
        if (params.max_tokens > 0)   j["max_tokens"] = params.max_tokens;
        else                         j.erase("max_tokens");
        if (params.has_temperature) j["temperature"] = params.temperature;
        else                          j.erase("temperature");
        if (params.has_top_p)        j["top_p"] = params.top_p;
        else                          j.erase("top_p");
        if (params.max_steps > 0)    j["max_steps"] = params.max_steps;
        else                          j.erase("max_steps");
        if (!params.reasoning_effort.empty()) j["reasoning_effort"] = params.reasoning_effort;
        else                                  j.erase("reasoning_effort");
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

void SessionStore::update_message_data(const std::string& session_id,
                                       int seq,
                                       const std::string& data_json) {
    const char* sql =
        "UPDATE session_message SET data_json=?, time_updated=?"
        " WHERE session_id=? AND seq=?";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.handle(), sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, data_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, util::now_ms());
    sqlite3_bind_text(stmt, 3, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, seq);
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

std::string SessionStore::insert_checkpoint(const std::string& session_id,
                                            int through_seq,
                                            const std::string& recent_context,
                                            const std::string& previous_checkpoint_id) {
    int64_t now = util::now_ms();
    std::string id = util::make_id("ckpt");
    const char* ins =
        "INSERT INTO compaction_checkpoint (id, session_id, through_seq,"
        " summary, recent_context, previous_checkpoint_id, status,"
        " time_created, time_updated) VALUES (?,?,?,?,?,?,?,?,?)";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.handle(), ins, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, through_seq);
    sqlite3_bind_text(stmt, 4, "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, recent_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, previous_checkpoint_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, "pending", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 8, now);
    sqlite3_bind_int64(stmt, 9, now);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return id;
}

void SessionStore::complete_checkpoint(const std::string& checkpoint_id,
                                       const std::string& summary) {
    const char* upd =
        "UPDATE compaction_checkpoint SET summary=?, status='complete',"
        " time_updated=? WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.handle(), upd, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, summary.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, util::now_ms());
    sqlite3_bind_text(stmt, 3, checkpoint_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void SessionStore::fail_checkpoint(const std::string& checkpoint_id) {
    const char* upd =
        "UPDATE compaction_checkpoint SET status='failed', time_updated=?"
        " WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.handle(), upd, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, util::now_ms());
    sqlite3_bind_text(stmt, 2, checkpoint_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::optional<CompactionCheckpoint> SessionStore::latest_complete_checkpoint(
    const std::string& session_id) {
    const char* sel =
        "SELECT id, session_id, through_seq, summary, recent_context,"
        " previous_checkpoint_id, status, time_created, time_updated"
        " FROM compaction_checkpoint WHERE session_id=? AND status='complete'"
        " ORDER BY through_seq DESC LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.handle(), sel, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<CompactionCheckpoint> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        CompactionCheckpoint cp;
        cp.id                     = (const char*)sqlite3_column_text(stmt, 0);
        cp.session_id             = (const char*)sqlite3_column_text(stmt, 1);
        cp.through_seq            = sqlite3_column_int(stmt, 2);
        cp.summary                = (const char*)sqlite3_column_text(stmt, 3);
        cp.recent_context         = (const char*)sqlite3_column_text(stmt, 4);
        cp.previous_checkpoint_id = (const char*)sqlite3_column_text(stmt, 5);
        cp.status                 = (const char*)sqlite3_column_text(stmt, 6);
        cp.time_created           = sqlite3_column_int64(stmt, 7);
        cp.time_updated           = sqlite3_column_int64(stmt, 8);
        out = std::move(cp);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<CompactionCheckpoint> SessionStore::list_complete_checkpoints(
    const std::string& session_id) {
    const char* sel =
        "SELECT id, session_id, through_seq, summary, recent_context,"
        " previous_checkpoint_id, status, time_created, time_updated"
        " FROM compaction_checkpoint WHERE session_id=? AND status='complete'"
        " ORDER BY through_seq ASC";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.handle(), sel, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    std::vector<CompactionCheckpoint> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CompactionCheckpoint cp;
        cp.id                     = (const char*)sqlite3_column_text(stmt, 0);
        cp.session_id             = (const char*)sqlite3_column_text(stmt, 1);
        cp.through_seq            = sqlite3_column_int(stmt, 2);
        cp.summary                = (const char*)sqlite3_column_text(stmt, 3);
        cp.recent_context         = (const char*)sqlite3_column_text(stmt, 4);
        cp.previous_checkpoint_id = (const char*)sqlite3_column_text(stmt, 5);
        cp.status                 = (const char*)sqlite3_column_text(stmt, 6);
        cp.time_created           = sqlite3_column_int64(stmt, 7);
        cp.time_updated           = sqlite3_column_int64(stmt, 8);
        out.push_back(std::move(cp));
    }
    sqlite3_finalize(stmt);
    return out;
}

void SessionStore::replace_todos(const std::string& session_id,
                                 const std::vector<Todo>& todos) {
    // Atomic whole-list replace: DELETE then INSERT each row inside one
    // transaction. Matches the todo_write tool's whole-list semantics.
    sqlite3_exec(db_.handle(), "BEGIN;", nullptr, nullptr, nullptr);

    {
        const char* del = "DELETE FROM session_todo WHERE session_id=?";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_.handle(), del, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    if (!todos.empty()) {
        const char* ins =
            "INSERT INTO session_todo"
            " (session_id, position, content, active_form, status, time_created, time_updated)"
            " VALUES (?, ?, ?, ?, ?, ?, ?)";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_.handle(), ins, -1, &stmt, nullptr);
        int64_t now = util::now_ms();
        for (size_t i = 0; i < todos.size(); ++i) {
            const auto& t = todos[i];
            sqlite3_bind_text(stmt, 1, session_id.c_str(),   -1, SQLITE_TRANSIENT);
            sqlite3_bind_int (stmt, 2, static_cast<int>(i));
            sqlite3_bind_text(stmt, 3, t.content.c_str(),    -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, t.active_form.c_str(),-1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, t.status.c_str(),     -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 6, now);
            sqlite3_bind_int64(stmt, 7, now);
            sqlite3_step(stmt);
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_exec(db_.handle(), "COMMIT;", nullptr, nullptr, nullptr);
}

std::vector<Todo> SessionStore::load_todos(const std::string& session_id) {
    const char* sql =
        "SELECT content, active_form, status"
        " FROM session_todo WHERE session_id=? ORDER BY position ASC";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.handle(), sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<Todo> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Todo t;
        const char* c = (const char*)sqlite3_column_text(stmt, 0);
        const char* a = (const char*)sqlite3_column_text(stmt, 1);
        const char* s = (const char*)sqlite3_column_text(stmt, 2);
        t.content    = c ? c : "";
        t.active_form= a ? a : "";
        t.status     = s ? s : "pending";
        out.push_back(std::move(t));
    }
    sqlite3_finalize(stmt);
    return out;
}

void SessionStore::update_tool_result_by_call_id(const std::string& call_id,
                                                  const std::string& new_output) {
    // Find the tool_result message whose data_json contains this call_id,
    // then update its data_json with the new output. The data_json is a JSON
    // object with at least {call_id, output}. We replace the 'output' field.
    const char* sql =
        "SELECT id, data_json FROM session_message"
        " WHERE type='tool_result' AND data_json LIKE ?";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.handle(), sql, -1, &stmt, nullptr);
    std::string pattern = "%\"call_id\":\"" + call_id + "\"%";
    sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);

    std::string msg_id;
    std::string data_json;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* id_col = (const char*)sqlite3_column_text(stmt, 0);
        const char* data_col = (const char*)sqlite3_column_text(stmt, 1);
        if (id_col) msg_id = id_col;
        if (data_col) data_json = data_col;
    }
    sqlite3_finalize(stmt);

    if (msg_id.empty()) return;

    // Parse the JSON, update 'output', serialize back.
    try {
        auto j = nlohmann::json::parse(data_json);
        j["output"] = new_output;
        std::string updated = j.dump();
        int64_t now = util::now_ms();
        const char* upd =
            "UPDATE session_message SET data_json=?, time_updated=? WHERE id=?";
        sqlite3_stmt* stmt2 = nullptr;
        sqlite3_prepare_v2(db_.handle(), upd, -1, &stmt2, nullptr);
        sqlite3_bind_text(stmt2, 1, updated.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt2, 2, now);
        sqlite3_bind_text(stmt2, 3, msg_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt2);
        sqlite3_finalize(stmt2);
    } catch (...) {
        // Parse error or other issue — silently fail, no-op.
    }
}

} // namespace haicode
