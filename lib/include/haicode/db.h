#pragma once
#include "types.h"
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <stdexcept>
#include <mutex>
#include <utility>
#include <sqlite3.h>

namespace haicode {

// Typed SQLite failure. Subclasses runtime_error so existing
// catch (std::exception) / catch (...) sites keep working.
struct DbError : std::runtime_error {
    explicit DbError(const std::string& what) : std::runtime_error(what) {}
};

// RAII prepared statement: the constructor prepares (throwing DbError with
// sqlite3_errmsg on failure), the destructor finalizes. Write paths must
// confirm the result with expect_done(); read paths advance with
// expect_row() (single lookups) or loop `while (stmt.expect_row())`.
class DbStmt {
public:
    DbStmt(sqlite3* db, const std::string& sql) : db_(db) {
        int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr);
        if (rc != SQLITE_OK) {
            std::string err = db ? sqlite3_errmsg(db) : "unknown";
            if (stmt_) { sqlite3_finalize(stmt_); stmt_ = nullptr; }
            throw DbError("prepare failed: " + err + " [" + sql + "]");
        }
    }
    ~DbStmt() { if (stmt_) sqlite3_finalize(stmt_); }
    DbStmt(const DbStmt&) = delete;
    DbStmt& operator=(const DbStmt&) = delete;

    // Raw step result, for callers that handle SQLITE_ROW/DONE themselves.
    int step() { return sqlite3_step(stmt_); }
    // Step once and require SQLITE_DONE (writes). Throws DbError otherwise.
    void expect_done() {
        int rc = sqlite3_step(stmt_);
        if (rc != SQLITE_DONE)
            throw DbError(std::string("step failed: ") + errmsg());
    }
    // Step once: true when a row is available, false at SQLITE_DONE,
    // DbError on anything else (reads).
    bool expect_row() {
        int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw DbError(std::string("step failed: ") + errmsg());
    }
    // Re-arm a statement for the next iteration of an insert loop.
    void reset() {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }

    DbStmt& bind(int idx, const std::string& v) {
        if (sqlite3_bind_text(stmt_, idx, v.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
            throw DbError(std::string("bind(text) failed: ") + errmsg());
        return *this;
    }
    DbStmt& bind(int idx, int v) {
        if (sqlite3_bind_int(stmt_, idx, v) != SQLITE_OK)
            throw DbError(std::string("bind(int) failed: ") + errmsg());
        return *this;
    }
    DbStmt& bind(int idx, int64_t v) {
        if (sqlite3_bind_int64(stmt_, idx, v) != SQLITE_OK)
            throw DbError(std::string("bind(int64) failed: ") + errmsg());
        return *this;
    }
    DbStmt& bind(int idx, double v) {
        if (sqlite3_bind_double(stmt_, idx, v) != SQLITE_OK)
            throw DbError(std::string("bind(double) failed: ") + errmsg());
        return *this;
    }
    DbStmt& bind_null(int idx) {
        if (sqlite3_bind_null(stmt_, idx) != SQLITE_OK)
            throw DbError(std::string("bind(null) failed: ") + errmsg());
        return *this;
    }

    // NULL columns read as empty/zero, matching the old column helpers.
    std::string text(int col) {
        const unsigned char* t = sqlite3_column_text(stmt_, col);
        return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
    }
    int     int_col(int col)   { return sqlite3_column_int(stmt_, col); }
    int64_t int64_col(int col) { return sqlite3_column_int64(stmt_, col); }
    double  dbl_col(int col)   { return sqlite3_column_double(stmt_, col); }

private:
    const char* errmsg() const { return db_ ? sqlite3_errmsg(db_) : "unknown"; }

    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

// RAII transaction on a raw connection: BEGIN is checked (throws DbError),
// COMMIT runs in commit() and is checked, and any outstanding transaction is
// rolled back on destruction — including when an exception unwinds through
// the guard or COMMIT itself fails.
class DbTxn {
public:
    explicit DbTxn(sqlite3* db) : db_(db) {
        char* err = nullptr;
        if (sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "unknown";
            sqlite3_free(err);
            throw DbError("BEGIN failed: " + msg);
        }
        open_ = true;
    }
    ~DbTxn() {
        if (open_) sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
    DbTxn(const DbTxn&) = delete;
    DbTxn& operator=(const DbTxn&) = delete;

    void commit() {
        char* err = nullptr;
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "unknown";
            sqlite3_free(err);
            // Leave open_ set so the destructor rolls back whatever the
            // failed commit left behind.
            throw DbError("COMMIT failed: " + msg);
        }
        open_ = false;
    }

private:
    sqlite3* db_;
    bool open_ = false;
};

class Database {
public:
    explicit Database(const std::string& path);
    ~Database();

    // Non-copyable, movable
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) noexcept;
    Database& operator=(Database&&) noexcept;

    void exec(const std::string& sql);
    void migrate();

    // Delete session_message / session_todo / compaction_checkpoint rows whose
    // session no longer exists. ON DELETE CASCADE makes orphans unexpected for
    // anything this code wrote; this repairs pre-FK databases and hand-edited
    // ones. Returns the number of rows removed; `error` is set when a delete
    // failed or violations remain afterwards.
    int prune_orphans(std::string& error);
    // Serialized with SessionStore operations. A nonzero minimum_free_bytes
    // skips VACUUM when too little space can be recovered. Checkpoints before
    // and after VACUUM ensure the compacted file is applied immediately.
    bool reclaim_space(std::string& error,
                       int64_t* bytes_before = nullptr,
                       int64_t* bytes_after = nullptr,
                       int64_t minimum_free_bytes = 0);

    // PRAGMA user_version — the schema version cursor for numbered migrations.
    int  user_version();
    void set_user_version(int version);
    // Override the open-time busy timeout (tests use a short one to prove
    // lock failures surface quickly instead of waiting the default 5 s).
    void set_busy_timeout(int ms);

    sqlite3* handle() { return db_; }
    std::mutex& connection_mutex() { return conn_mu_; }

private:
    sqlite3* db_ = nullptr;
    std::mutex conn_mu_;
};

// ---- Session types ----

struct SessionInfo {
    std::string id;
    std::string project_id;
    std::string title;
    std::string directory;
    std::string agent;
    std::string model_json;   // serialized ModelRef
    double cost = 0.0;
    TokenUsage tokens;
    // Last provider-reported per-request input size (input + cache_read +
    // cache_write). Seeds the context meter when a session is reopened.
    int last_input_tokens = 0;
    int64_t time_created = 0;
    int64_t time_updated = 0;
};

// Which stored sessions a bulk cleanup targets. Every predicate left at its
// default matches everything, so an all-default filter means "all sessions".
struct SessionFilter {
    std::string directory;             // "" = any project
    int64_t updated_before_ms = 0;     // 0 = no age limit
    bool untitled_only = false;        // title = ''
    bool empty_only = false;           // no 'user_prompted' message row
};

struct SessionMessage {
    std::string id;
    std::string session_id;
    std::string type;
    int seq = 0;
    std::string data_json;   // JSON blob
    int64_t time_created = 0;
    int64_t time_updated = 0;
};

// A compaction checkpoint records that every conversation record with
// seq <= through_seq has been folded into `summary`. The underlying rows are
// never deleted; context assembly slices seq > through_seq and prepends the
// rendered checkpoint block instead.
struct CompactionCheckpoint {
    std::string id;
    std::string session_id;
    int through_seq = 0;
    std::string summary;          // Markdown with the required sections
    std::string recent_context;   // serialized retained tail at checkpoint time
    std::string previous_checkpoint_id;
    std::string status;           // "pending" | "complete" | "failed"
    int64_t time_created = 0;
    int64_t time_updated = 0;
};

class SessionStore {
public:
    explicit SessionStore(Database& db);

    SessionInfo create(const std::string& project_dir,
                       const std::string& agent,
                       const std::string& model_json);
    std::optional<SessionInfo> get(const std::string& session_id);
    std::vector<SessionInfo> list(int limit = 50);
    // (id, time_updated) of the newest `limit` sessions — list()'s order —
    // without blocking: false (out untouched) when another thread holds the
    // shared connection (e.g. housekeeping's VACUUM), so the sidebar's
    // periodic timestamp refresh can retry later instead of freezing the
    // GUI looper.
    bool try_update_times(std::vector<std::pair<std::string, int64_t>>& out,
                          int limit = 50);
    // Ids of every stored session matching `filter`, newest first. Deliberately
    // uncapped (unlike list()'s 50) so a bulk cleanup can reach sessions that
    // never made it into the sidebar.
    std::vector<std::string> sessions_matching(const SessionFilter& filter);
    void update_title(const std::string& session_id, const std::string& title);
    bool update_title_if_current(const std::string& session_id,
                                 const std::string& title,
                                 const std::string& baseline);
    void update_directory(const std::string& session_id, const std::string& directory);
    // Adds a request's cost and token buckets to the session totals. When
    // `sets_context` is true (a real conversation request with reported
    // usage) tok_last_input becomes this request's prompt size; maintenance
    // calls (title, summary, image description) and estimated partial usage
    // pass false so they never overwrite the context-meter seed.
    void update_cost(const std::string& session_id, double cost, const TokenUsage& tokens,
                     bool sets_context = true);
    // Overwrite just the per-request input seed (tok_last_input) — used after
    // compaction, where no provider report will arrive until the next step.
    void update_last_input_tokens(const std::string& session_id, int tokens);
    // Patch the "mode" field inside the session's model_json blob. No-op if the
    // session does not exist. mode_str should be "build" or "plan".
    void update_mode(const std::string& session_id, const std::string& mode_str);
    // Patch the "auto_edits", "yolo", and "allow_read_everywhere"
    // permission-flag fields inside the session's model_json blob. No-op if
    // the session does not exist.
    void update_permission_flags(const std::string& session_id,
                                 bool auto_edits, bool yolo,
                                 bool read_everywhere);
    // Overwrite the "skills" array (enabled skill ids) inside the session's
    // model_json blob. Replaces the whole list in one write. No-op if the
    // session does not exist.
    void update_skills(const std::string& session_id,
                       const std::vector<std::string>& skills);
    // Patch the "id" (model) and "provider_id" fields inside the session's
    // model_json blob. Either string may be empty to leave that field untouched.
    // No-op if the session does not exist.
    void update_provider_model(const std::string& session_id,
                               const std::string& provider_id,
                               const std::string& model_id);
    // Patch the inference params (max_tokens, temperature, top_p, max_steps,
    // reasoning_effort) stored inside the session's model_json blob. Optional
    // fields (has_temperature/has_top_p) are erased when unset. No-op if the
    // session does not exist.
    void update_inference(const std::string& session_id,
                          const InferenceParams& params);
    void delete_session(const std::string& session_id);

    void append_message(const std::string& session_id,
                        const std::string& type,
                        const std::string& data_json);
    // Overwrite one message row's data_json (used by the vision-fallback
    // backfill to persist image descriptions on attachment entries). No-op if
    // the (session_id, seq) row does not exist.
    void update_message_data(const std::string& session_id,
                             int seq,
                             const std::string& data_json);
    // Delete every message row with seq > `seq` (used by retry to drop the
    // last turn's assistant output while keeping its user_prompted row).
    // Checkpoints and todos are untouched: checkpoints only reference
    // boundaries (a through_seq above the deleted tail is impossible since
    // compaction never covers the current turn), and the retried turn's
    // todo_write calls replace the todo list anyway.
    void delete_messages_after(const std::string& session_id, int seq);
    std::vector<SessionMessage> load_messages(const std::string& session_id);

    // ---- Compaction checkpoints (non-destructive compaction) ----
    // Create a pending checkpoint row (visible crash marker) and return its
    // id. Messages arriving afterward get higher seqs and stay after the
    // boundary by construction.
    std::string insert_checkpoint(const std::string& session_id,
                                  int through_seq,
                                  const std::string& recent_context,
                                  const std::string& previous_checkpoint_id);
    // Atomically commit: set summary + status="complete" in one UPDATE.
    void complete_checkpoint(const std::string& checkpoint_id,
                             const std::string& summary);
    // Mark failed; the previous checkpoint and boundary stay active.
    void fail_checkpoint(const std::string& checkpoint_id);
    // Latest checkpoint with status="complete" (highest through_seq), if any.
    std::optional<CompactionCheckpoint> latest_complete_checkpoint(
        const std::string& session_id);
    // All complete checkpoints for the session, ascending through_seq. The
    // frontends use this to replay [context compacted] transcript entries at
    // the exact point each compaction occurred, without storing message rows.
    std::vector<CompactionCheckpoint> list_complete_checkpoints(
        const std::string& session_id);
    // Delete non-complete checkpoint rows (crash "pending" markers and "failed"
    // attempts) older than `older_than_ms`. Complete rows are never deleted —
    // they are the [context compacted] transcript source and the compaction
    // chain. Returns the number of rows removed.
    int prune_stale_checkpoints(int64_t older_than_ms);
    // Blank recent_context on every complete checkpoint except each session's
    // newest-by-through_seq one: only that row is ever read again (by
    // compact_history). The chain and every summary stay intact. Returns the
    // number of rows cleared.
    int clear_stale_checkpoint_contexts();

    // Atomic whole-list replace for the todo_write tool. Deletes every
    // existing row for the session and inserts the new list in one
    // transaction.
    void replace_todos(const std::string& session_id,
                       const std::vector<Todo>& todos);
    std::vector<Todo> load_todos(const std::string& session_id);

    // Update the 'output' field of `session_id`'s newest tool_result row
    // matching `call_id`. Used by the engine to replace the placeholder output
    // echoed by ask_user with the user's real answer after they reply in the
    // UI. Scoped to the session and newest-first because OpenAI-compatible
    // servers pass their own call ids through, and some reuse short ids
    // (`call_0`) across turns and sessions. No-op if no row matches.
    void update_tool_result_by_call_id(const std::string& session_id,
                                       const std::string& call_id,
                                       const std::string& new_output);

private:
    Database& db_;
    // All sessions share one sqlite connection; this serializes every store
    // operation in-process. It is the boundary that keeps a replace_todos
    // transaction from absorbing another thread's writes and stops reads
    // from observing mid-transaction state. No public method may call
    // another public method (would self-deadlock on this mutex).
    std::mutex& conn_mu_;
};

} // namespace haicode
