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
        throw DbError("Failed to open database '" + path + "': " + err);
    }
    // Wait out short lock contention (e.g. another process finishing a
    // write) instead of failing immediately.
    if (sqlite3_busy_timeout(db_, 5000) != SQLITE_OK)
        throw DbError(std::string("busy_timeout failed: ") + sqlite3_errmsg(db_));
    // Enable WAL for better concurrency. journal_mode returns a row; use a
    // statement so the result is consumed and checked.
    {
        DbStmt stmt(db_, "PRAGMA journal_mode=WAL;");
        if (!stmt.expect_row())
            throw DbError("journal_mode=WAL returned no row");
    }
    DbStmt fk(db_, "PRAGMA foreign_keys=ON;");
    fk.expect_done();
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
        throw DbError("SQL error: " + msg);
    }
}

int Database::user_version() {
    DbStmt stmt(db_, "PRAGMA user_version;");
    if (!stmt.expect_row())
        throw DbError("user_version returned no row");
    return stmt.int_col(0);
}

void Database::set_user_version(int version) {
    exec("PRAGMA user_version = " + std::to_string(version) + ";");
}

void Database::set_busy_timeout(int ms) {
    if (sqlite3_busy_timeout(db_, ms) != SQLITE_OK)
        throw DbError(std::string("busy_timeout failed: ") + sqlite3_errmsg(db_));
}

void Database::migrate() {
    // Numbered migrations keyed on PRAGMA user_version. Each step runs once
    // and bumps the cursor; fresh databases walk every step idempotently.
    int version = user_version();
    if (version < 1) {
        exec(kSchema);
        // Column additions for pre-existing databases. SQLite has no ADD
        // COLUMN IF NOT EXISTS, so check pragma table_info first (fresh
        // kSchema already contains the column).
        auto has_column = [&](const char* table, const char* col) {
            std::string q = std::string("PRAGMA table_info(") + table + ")";
            DbStmt st(db_, q);
            while (st.expect_row()) {
                std::string name = st.text(1);
                if (name == col) return true;
            }
            return false;
        };
        if (!has_column("session", "tok_last_input"))
            exec("ALTER TABLE session ADD COLUMN tok_last_input INTEGER NOT NULL DEFAULT 0");
        set_user_version(1);
    }
    if (version < 2) {
        // The permission table was never read or written by any code path;
        // policy lives in config files and the in-memory PermissionGate.
        exec("DROP TABLE IF EXISTS permission");
        set_user_version(2);
    }
}

int Database::prune_orphans(std::string& error) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    error.clear();
    int removed = 0;
    try {
        DbTxn txn(db_);
        static const char* kTables[] = {
            "session_message", "session_todo", "compaction_checkpoint"
        };
        for (const char* table : kTables) {
            DbStmt stmt(db_, std::string("DELETE FROM ") + table
                + " WHERE session_id NOT IN (SELECT id FROM session)");
            stmt.expect_done();
            removed += sqlite3_changes(db_);
        }
        txn.commit();
        DbStmt check(db_, "PRAGMA foreign_key_check;");
        int violations = 0;
        while (check.expect_row()) ++violations;
        if (violations > 0)
            error = std::to_string(violations) + " foreign key violation(s) remain";
    } catch (const std::exception& e) {
        error = e.what();
        return 0;
    }
    return removed;
}

bool Database::reclaim_space(std::string& error,
                             int64_t* bytes_before, int64_t* bytes_after,
                             int64_t minimum_free_bytes) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    error.clear();
    if (bytes_before) *bytes_before = 0;
    if (bytes_after) *bytes_after = 0;
    auto pragma = [&](const char* sql) -> int64_t {
        DbStmt stmt(db_, sql);
        if (!stmt.expect_row()) throw DbError("PRAGMA returned no row");
        return stmt.int64_col(0);
    };
    auto checkpoint = [&]() {
        DbStmt stmt(db_, "PRAGMA wal_checkpoint(TRUNCATE);");
        if (!stmt.expect_row()) throw DbError("wal_checkpoint returned no row");
        if (stmt.int_col(0) != 0) throw DbError("WAL checkpoint is busy");
    };
    try {
        int64_t page_size = pragma("PRAGMA page_size;");
        int64_t before = pragma("PRAGMA page_count;") * page_size;
        if (bytes_before) *bytes_before = before;
        if (bytes_after) *bytes_after = before;
        if (minimum_free_bytes > 0
                && pragma("PRAGMA freelist_count;") * page_size < minimum_free_bytes)
            return true;
        // Each helper finalizes its statement before VACUUM starts.
        checkpoint();
        exec("VACUUM;");
        checkpoint();
        if (bytes_after) *bytes_after = pragma("PRAGMA page_count;") * page_size;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

// ---- SessionStore ----

SessionStore::SessionStore(Database& db) : db_(db), conn_mu_(db.connection_mutex()) {}

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

    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt stmt(db_.handle(),
        "INSERT INTO session (id, project_id, title, directory, agent, model_json,"
        " cost, tok_input, tok_output, tok_reasoning, tok_cache_read, tok_cache_write,"
        " time_created, time_updated)"
        " VALUES (?,?,?,?,?,?,0,0,0,0,0,0,?,?)");
    stmt.bind(1, s.id)
        .bind(2, s.project_id)
        .bind(3, s.title)
        .bind(4, s.directory)
        .bind(5, s.agent)
        .bind(6, s.model_json)
        .bind(7, s.time_created)
        .bind(8, s.time_updated)
        .expect_done();

    return s;
}

std::optional<SessionInfo> SessionStore::get(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt stmt(db_.handle(),
        "SELECT id, project_id, title, directory, agent, model_json,"
        " cost, tok_input, tok_output, tok_reasoning, tok_cache_read, tok_cache_write,"
        " tok_last_input, time_created, time_updated"
        " FROM session WHERE id=?");
    stmt.bind(1, session_id);

    if (!stmt.expect_row()) return std::nullopt;
    SessionInfo s;
    s.id         = stmt.text(0);
    s.project_id = stmt.text(1);
    s.title      = stmt.text(2);
    s.directory  = stmt.text(3);
    s.agent      = stmt.text(4);
    s.model_json = stmt.text(5);
    s.cost              = stmt.dbl_col(6);
    s.tokens.input      = stmt.int_col(7);
    s.tokens.output     = stmt.int_col(8);
    s.tokens.reasoning  = stmt.int_col(9);
    s.tokens.cache_read = stmt.int_col(10);
    s.tokens.cache_write= stmt.int_col(11);
    s.last_input_tokens = stmt.int_col(12);
    s.time_created      = stmt.int64_col(13);
    s.time_updated      = stmt.int64_col(14);
    return s;
}

std::vector<SessionInfo> SessionStore::list(int limit) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt stmt(db_.handle(),
        "SELECT id, project_id, title, directory, agent, model_json,"
        " cost, tok_input, tok_output, tok_reasoning, tok_cache_read, tok_cache_write,"
        " tok_last_input, time_created, time_updated"
        " FROM session ORDER BY time_updated DESC LIMIT ?");
    stmt.bind(1, limit);

    std::vector<SessionInfo> results;
    while (stmt.expect_row()) {
        SessionInfo s;
        s.id         = stmt.text(0);
        s.project_id = stmt.text(1);
        s.title      = stmt.text(2);
        s.directory  = stmt.text(3);
        s.agent      = stmt.text(4);
        s.model_json = stmt.text(5);
        s.cost              = stmt.dbl_col(6);
        s.tokens.input      = stmt.int_col(7);
        s.tokens.output     = stmt.int_col(8);
        s.tokens.reasoning  = stmt.int_col(9);
        s.tokens.cache_read = stmt.int_col(10);
        s.tokens.cache_write= stmt.int_col(11);
        s.last_input_tokens = stmt.int_col(12);
        s.time_created      = stmt.int64_col(13);
        s.time_updated      = stmt.int64_col(14);
        results.push_back(std::move(s));
    }
    return results;
}

bool SessionStore::try_update_times(
        std::vector<std::pair<std::string, int64_t>>& out, int limit) {
    std::unique_lock<std::mutex> lock(conn_mu_, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    DbStmt stmt(db_.handle(),
        "SELECT id, time_updated FROM session ORDER BY time_updated DESC LIMIT ?");
    stmt.bind(1, limit);
    std::vector<std::pair<std::string, int64_t>> rows;
    while (stmt.expect_row())
        rows.emplace_back(stmt.text(0), stmt.int64_col(1));
    out = std::move(rows);
    return true;
}

std::vector<std::string> SessionStore::sessions_matching(const SessionFilter& filter) {
    std::lock_guard<std::mutex> lock(conn_mu_);

    struct Arg {
        bool is_int;
        std::string s;
        int64_t i;
    };
    std::string sql = "SELECT id FROM session WHERE 1";
    std::vector<Arg> args;
    if (!filter.directory.empty()) {
        sql += " AND directory=?";
        args.push_back({false, filter.directory, 0});
    }
    if (filter.updated_before_ms > 0) {
        sql += " AND time_updated<?";
        args.push_back({true, "", filter.updated_before_ms});
    }
    if (filter.untitled_only)
        sql += " AND title=''";
    if (filter.empty_only)
        sql += " AND NOT EXISTS (SELECT 1 FROM session_message m"
               " WHERE m.session_id = session.id AND m.type='user_prompted')";
    sql += " ORDER BY time_updated DESC";

    DbStmt stmt(db_.handle(), sql);
    for (size_t i = 0; i < args.size(); ++i) {
        const Arg& a = args[i];
        if (a.is_int) stmt.bind(int(i) + 1, a.i);
        else          stmt.bind(int(i) + 1, a.s);
    }

    std::vector<std::string> ids;
    while (stmt.expect_row())
        ids.push_back(stmt.text(0));
    return ids;
}

void SessionStore::update_directory(const std::string& session_id, const std::string& directory) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt stmt(db_.handle(),
        "UPDATE session SET directory=?, project_id=?, time_updated=? WHERE id=?");
    stmt.bind(1, directory)
        .bind(2, directory)
        .bind(3, util::now_ms())
        .bind(4, session_id)
        .expect_done();
}

void SessionStore::update_title(const std::string& session_id, const std::string& title) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt stmt(db_.handle(),
        "UPDATE session SET title=?, time_updated=? WHERE id=?");
    stmt.bind(1, title)
        .bind(2, util::now_ms())
        .bind(3, session_id)
        .expect_done();
}

bool SessionStore::update_title_if_current(const std::string& session_id,
                                           const std::string& title,
                                           const std::string& baseline) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt stmt(db_.handle(),
        "UPDATE session SET title=?, time_updated=? WHERE id=? AND title=?");
    stmt.bind(1, title).bind(2, util::now_ms()).bind(3, session_id)
        .bind(4, baseline).expect_done();
    return sqlite3_changes(db_.handle()) != 0;
}

void SessionStore::update_cost(const std::string& session_id, double cost,
                                const TokenUsage& tokens, bool sets_context) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    // tok_last_input=COALESCE(?, tok_last_input): a NULL bind keeps the
    // current seed (maintenance calls / estimates must not move it).
    DbStmt stmt(db_.handle(),
        "UPDATE session SET cost=cost+?, tok_input=tok_input+?, tok_output=tok_output+?,"
        " tok_reasoning=tok_reasoning+?, tok_cache_read=tok_cache_read+?,"
        " tok_cache_write=tok_cache_write+?,"
        " tok_last_input=COALESCE(?, tok_last_input), time_updated=? WHERE id=?");
    stmt.bind(1, cost)
        .bind(2, tokens.input)
        .bind(3, tokens.output)
        .bind(4, tokens.reasoning)
        .bind(5, tokens.cache_read)
        .bind(6, tokens.cache_write)
        .bind(8, util::now_ms())
        .bind(9, session_id);
    // Per-request input size (not cumulative) — seeds the context meter on
    // session reopen. Matches the engine's prev_total_input arithmetic.
    if (sets_context) stmt.bind(7, tokens.total_input());
    else              stmt.bind_null(7);
    stmt.expect_done();
}

void SessionStore::update_last_input_tokens(const std::string& session_id,
                                            int tokens) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt stmt(db_.handle(),
        "UPDATE session SET tok_last_input=?, time_updated=? WHERE id=?");
    stmt.bind(1, tokens)
        .bind(2, util::now_ms())
        .bind(3, session_id)
        .expect_done();
}

void SessionStore::delete_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt stmt(db_.handle(), "DELETE FROM session WHERE id=?");
    stmt.bind(1, session_id).expect_done();
}

void SessionStore::update_mode(const std::string& session_id, const std::string& mode_str) {
    // Read current model_json, patch the "mode" field, write it back.
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt sel(db_.handle(), "SELECT model_json FROM session WHERE id=?");
    sel.bind(1, session_id);
    if (!sel.expect_row()) return;
    std::string model_json = sel.text(0);

    try {
        auto j = nlohmann::json::parse(model_json, nullptr, false);
        if (j.is_discarded() || !j.is_object()) j = nlohmann::json::object();
        j["mode"] = mode_str;
        model_json = j.dump();
    } catch (...) {
        // Corrupt JSON — leave the row alone rather than wiping other fields.
        return;
    }

    DbStmt upd(db_.handle(),
        "UPDATE session SET model_json=?, time_updated=? WHERE id=?");
    upd.bind(1, model_json)
       .bind(2, util::now_ms())
       .bind(3, session_id)
       .expect_done();
}

void SessionStore::update_permission_flags(const std::string& session_id,
                                           bool auto_edits, bool yolo,
                                           bool read_everywhere) {
    // Read current model_json, patch the flag fields, write it back.
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt sel(db_.handle(), "SELECT model_json FROM session WHERE id=?");
    sel.bind(1, session_id);
    if (!sel.expect_row()) return;
    std::string model_json = sel.text(0);

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

    DbStmt upd(db_.handle(),
        "UPDATE session SET model_json=?, time_updated=? WHERE id=?");
    upd.bind(1, model_json)
       .bind(2, util::now_ms())
       .bind(3, session_id)
       .expect_done();
}

void SessionStore::update_skills(const std::string& session_id,
                                 const std::vector<std::string>& skills) {
    // Read current model_json, replace the "skills" array, write it back.
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt sel(db_.handle(), "SELECT model_json FROM session WHERE id=?");
    sel.bind(1, session_id);
    if (!sel.expect_row()) return;
    std::string model_json = sel.text(0);

    try {
        auto j = nlohmann::json::parse(model_json, nullptr, false);
        if (j.is_discarded() || !j.is_object()) j = nlohmann::json::object();
        j["skills"] = skills;
        model_json = j.dump();
    } catch (...) {
        // Corrupt JSON — leave the row alone rather than wiping other fields.
        return;
    }

    DbStmt upd(db_.handle(),
        "UPDATE session SET model_json=?, time_updated=? WHERE id=?");
    upd.bind(1, model_json)
       .bind(2, util::now_ms())
       .bind(3, session_id)
       .expect_done();
}

void SessionStore::update_provider_model(const std::string& session_id,
                                          const std::string& provider_id,
                                          const std::string& model_id) {
    // Read current model_json, patch the "id"/"provider_id" fields, write back.
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt sel(db_.handle(), "SELECT model_json FROM session WHERE id=?");
    sel.bind(1, session_id);
    if (!sel.expect_row()) return;
    std::string model_json = sel.text(0);

    try {
        auto j = nlohmann::json::parse(model_json, nullptr, false);
        if (j.is_discarded() || !j.is_object()) j = nlohmann::json::object();
        if (!provider_id.empty()) j["provider_id"] = provider_id;
        if (!model_id.empty())    j["id"]          = model_id;
        model_json = j.dump();
    } catch (...) {
        return;
    }

    DbStmt upd(db_.handle(),
        "UPDATE session SET model_json=?, time_updated=? WHERE id=?");
    upd.bind(1, model_json)
       .bind(2, util::now_ms())
       .bind(3, session_id)
       .expect_done();
}

void SessionStore::update_inference(const std::string& session_id,
                                    const InferenceParams& params) {
    // Read current model_json, patch the inference fields, write back.
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt sel(db_.handle(), "SELECT model_json FROM session WHERE id=?");
    sel.bind(1, session_id);
    if (!sel.expect_row()) return;
    std::string model_json = sel.text(0);

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

    DbStmt upd(db_.handle(),
        "UPDATE session SET model_json=?, time_updated=? WHERE id=?");
    upd.bind(1, model_json)
       .bind(2, util::now_ms())
       .bind(3, session_id)
       .expect_done();
}

void SessionStore::append_message(const std::string& session_id,
                                   const std::string& type,
                                   const std::string& data_json) {
    // Single atomic statement: the next seq is computed inside the INSERT,
    // so two concurrent writers can never allocate the same seq — the
    // UNIQUE(session_id, seq) constraint turns a collision into a thrown
    // DbError instead of a silently lost row. MAX over an empty set is
    // NULL, so the first message gets seq 1.
    int64_t now = util::now_ms();
    std::string id = util::make_id("msg");

    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt stmt(db_.handle(),
        "INSERT INTO session_message"
        " (id, session_id, type, seq, data_json, time_created, time_updated)"
        " SELECT ?, ?, ?, COALESCE(MAX(seq),0)+1, ?, ?, ?"
        " FROM session_message WHERE session_id=?");
    stmt.bind(1, id)
        .bind(2, session_id)
        .bind(3, type)
        .bind(4, data_json)
        .bind(5, now)
        .bind(6, now)
        .bind(7, session_id)
        .expect_done();

    // Update session's time_updated
    DbStmt upd(db_.handle(), "UPDATE session SET time_updated=? WHERE id=?");
    upd.bind(1, now)
       .bind(2, session_id)
       .expect_done();
}

void SessionStore::update_message_data(const std::string& session_id,
                                       int seq,
                                       const std::string& data_json) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt stmt(db_.handle(),
        "UPDATE session_message SET data_json=?, time_updated=?"
        " WHERE session_id=? AND seq=?");
    stmt.bind(1, data_json)
        .bind(2, util::now_ms())
        .bind(3, session_id)
        .bind(4, seq)
        .expect_done();
}

void SessionStore::delete_messages_after(const std::string& session_id, int seq) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    {
        DbStmt stmt(db_.handle(),
            "DELETE FROM session_message WHERE session_id=? AND seq>?");
        stmt.bind(1, session_id)
            .bind(2, seq)
            .expect_done();
    }
    DbStmt upd(db_.handle(), "UPDATE session SET time_updated=? WHERE id=?");
    upd.bind(1, util::now_ms())
       .bind(2, session_id)
       .expect_done();
}

std::vector<SessionMessage> SessionStore::load_messages(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt stmt(db_.handle(),
        "SELECT id, session_id, type, seq, data_json, time_created, time_updated"
        " FROM session_message WHERE session_id=? ORDER BY seq ASC");
    stmt.bind(1, session_id);

    std::vector<SessionMessage> results;
    while (stmt.expect_row()) {
        SessionMessage m;
        m.id           = stmt.text(0);
        m.session_id   = stmt.text(1);
        m.type         = stmt.text(2);
        m.seq          = stmt.int_col(3);
        m.data_json    = stmt.text(4);
        m.time_created = stmt.int64_col(5);
        m.time_updated = stmt.int64_col(6);
        results.push_back(std::move(m));
    }
    return results;
}

std::string SessionStore::insert_checkpoint(const std::string& session_id,
                                            int through_seq,
                                            const std::string& recent_context,
                                            const std::string& previous_checkpoint_id) {
    int64_t now = util::now_ms();
    std::string id = util::make_id("ckpt");
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt stmt(db_.handle(),
        "INSERT INTO compaction_checkpoint (id, session_id, through_seq,"
        " summary, recent_context, previous_checkpoint_id, status,"
        " time_created, time_updated) VALUES (?,?,?,?,?,?,?,?,?)");
    stmt.bind(1, id)
        .bind(2, session_id)
        .bind(3, through_seq)
        .bind(4, std::string())
        .bind(5, recent_context)
        .bind(6, previous_checkpoint_id)
        .bind(7, std::string("pending"))
        .bind(8, now)
        .bind(9, now)
        .expect_done();
    return id;
}

void SessionStore::complete_checkpoint(const std::string& checkpoint_id,
                                       const std::string& summary) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt stmt(db_.handle(),
        "UPDATE compaction_checkpoint SET summary=?, status='complete',"
        " time_updated=? WHERE id=?");
    stmt.bind(1, summary)
        .bind(2, util::now_ms())
        .bind(3, checkpoint_id)
        .expect_done();
}

void SessionStore::fail_checkpoint(const std::string& checkpoint_id) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt stmt(db_.handle(),
        "UPDATE compaction_checkpoint SET status='failed', time_updated=?"
        " WHERE id=?");
    stmt.bind(1, util::now_ms())
        .bind(2, checkpoint_id)
        .expect_done();
}

std::optional<CompactionCheckpoint> SessionStore::latest_complete_checkpoint(
    const std::string& session_id) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt stmt(db_.handle(),
        "SELECT id, session_id, through_seq, summary, recent_context,"
        " previous_checkpoint_id, status, time_created, time_updated"
        " FROM compaction_checkpoint WHERE session_id=? AND status='complete'"
        " ORDER BY through_seq DESC LIMIT 1");
    stmt.bind(1, session_id);
    if (!stmt.expect_row()) return std::nullopt;
    CompactionCheckpoint cp;
    cp.id                     = stmt.text(0);
    cp.session_id             = stmt.text(1);
    cp.through_seq            = stmt.int_col(2);
    cp.summary                = stmt.text(3);
    cp.recent_context         = stmt.text(4);
    cp.previous_checkpoint_id = stmt.text(5);
    cp.status                 = stmt.text(6);
    cp.time_created           = stmt.int64_col(7);
    cp.time_updated           = stmt.int64_col(8);
    return cp;
}

std::vector<CompactionCheckpoint> SessionStore::list_complete_checkpoints(
    const std::string& session_id) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt stmt(db_.handle(),
        "SELECT id, session_id, through_seq, summary, recent_context,"
        " previous_checkpoint_id, status, time_created, time_updated"
        " FROM compaction_checkpoint WHERE session_id=? AND status='complete'"
        " ORDER BY through_seq ASC");
    stmt.bind(1, session_id);
    std::vector<CompactionCheckpoint> out;
    while (stmt.expect_row()) {
        CompactionCheckpoint cp;
        cp.id                     = stmt.text(0);
        cp.session_id             = stmt.text(1);
        cp.through_seq            = stmt.int_col(2);
        cp.summary                = stmt.text(3);
        cp.recent_context         = stmt.text(4);
        cp.previous_checkpoint_id = stmt.text(5);
        cp.status                 = stmt.text(6);
        cp.time_created           = stmt.int64_col(7);
        cp.time_updated           = stmt.int64_col(8);
        out.push_back(std::move(cp));
    }
    return out;
}

int SessionStore::prune_stale_checkpoints(int64_t older_than_ms) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt stmt(db_.handle(),
        "DELETE FROM compaction_checkpoint"
        " WHERE status<>'complete' AND time_updated<?");
    stmt.bind(1, older_than_ms).expect_done();
    return sqlite3_changes(db_.handle());
}

int SessionStore::clear_stale_checkpoint_contexts() {
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt stmt(db_.handle(),
        "UPDATE compaction_checkpoint SET recent_context=''"
        " WHERE status='complete' AND recent_context<>'' AND id NOT IN"
        " (SELECT id FROM compaction_checkpoint c"
        "  WHERE c.status='complete' AND c.through_seq=("
        "   SELECT MAX(through_seq) FROM compaction_checkpoint"
        "   WHERE session_id=c.session_id AND status='complete'))");
    stmt.expect_done();
    return sqlite3_changes(db_.handle());
}

void SessionStore::replace_todos(const std::string& session_id,
                                 const std::vector<Todo>& todos) {
    // Atomic whole-list replace: DELETE then INSERT each row inside one
    // transaction. Matches the todo_write tool's whole-list semantics.
    // conn_mu_ is held across the entire transaction: all sessions share
    // one sqlite connection, so without the store-level lock a concurrent
    // writer's BEGIN would fail and its writes would get absorbed here.
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbTxn txn(db_.handle());

    {
        DbStmt stmt(db_.handle(), "DELETE FROM session_todo WHERE session_id=?");
        stmt.bind(1, session_id).expect_done();
    }

    if (!todos.empty()) {
        DbStmt stmt(db_.handle(),
            "INSERT INTO session_todo"
            " (session_id, position, content, active_form, status, time_created, time_updated)"
            " VALUES (?, ?, ?, ?, ?, ?, ?)");
        int64_t now = util::now_ms();
        for (size_t i = 0; i < todos.size(); ++i) {
            const auto& t = todos[i];
            stmt.bind(1, session_id)
                .bind(2, static_cast<int>(i))
                .bind(3, t.content)
                .bind(4, t.active_form)
                .bind(5, t.status)
                .bind(6, now)
                .bind(7, now)
                .expect_done();
            stmt.reset();
        }
    }

    txn.commit();
}

std::vector<Todo> SessionStore::load_todos(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt stmt(db_.handle(),
        "SELECT content, active_form, status"
        " FROM session_todo WHERE session_id=? ORDER BY position ASC");
    stmt.bind(1, session_id);

    std::vector<Todo> out;
    while (stmt.expect_row()) {
        Todo t;
        t.content    = stmt.text(0);
        t.active_form= stmt.text(1);
        t.status     = stmt.text(2);
        out.push_back(std::move(t));
    }
    return out;
}

void SessionStore::update_tool_result_by_call_id(const std::string& session_id,
                                                  const std::string& call_id,
                                                  const std::string& new_output) {
    // Find this session's newest tool_result row whose call_id matches, then
    // replace its 'output' field. The call_id is compared on the parsed JSON
    // below; the LIKE only narrows candidates, so its '_'/'%' wildcards (and
    // '_' is common: "toolu_...", "call_0") can't select the wrong row.
    std::lock_guard<std::mutex> lock(conn_mu_);
    DbStmt sel(db_.handle(),
        "SELECT id, data_json FROM session_message"
        " WHERE session_id=? AND type='tool_result' AND data_json LIKE ?"
        " ORDER BY seq DESC");
    sel.bind(1, session_id)
       .bind(2, "%\"call_id\":\"" + call_id + "\"%");

    std::string msg_id;
    nlohmann::json j;
    while (sel.expect_row()) {
        // An unparseable row is skipped, never rewritten.
        auto candidate = nlohmann::json::parse(sel.text(1), nullptr, false);
        if (candidate.is_object() && candidate.value("call_id", "") == call_id) {
            msg_id = sel.text(0);
            j = std::move(candidate);
            break;
        }
    }

    if (msg_id.empty()) return;

    try {
        j["output"] = new_output;
        std::string updated = j.dump();
        DbStmt upd(db_.handle(),
            "UPDATE session_message SET data_json=?, time_updated=? WHERE id=?");
        upd.bind(1, updated)
           .bind(2, util::now_ms())
           .bind(3, msg_id)
           .expect_done();
    } catch (const nlohmann::json::exception&) {
        // Serialization error (e.g. invalid UTF-8) — don't touch the row.
    }
}

} // namespace haicode
