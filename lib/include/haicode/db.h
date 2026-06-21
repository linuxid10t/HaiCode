#pragma once
#include "types.h"
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <sqlite3.h>

namespace haicode {

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

    sqlite3* handle() { return db_; }

private:
    sqlite3* db_ = nullptr;
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
    int64_t time_created = 0;
    int64_t time_updated = 0;
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

class SessionStore {
public:
    explicit SessionStore(Database& db);

    SessionInfo create(const std::string& project_dir,
                       const std::string& agent,
                       const std::string& model_json);
    std::optional<SessionInfo> get(const std::string& session_id);
    std::vector<SessionInfo> list(int limit = 50);
    void update_title(const std::string& session_id, const std::string& title);
    void update_directory(const std::string& session_id, const std::string& directory);
    void update_cost(const std::string& session_id, double cost, const TokenUsage& tokens);
    // Patch the "mode" field inside the session's model_json blob. No-op if the
    // session does not exist. mode_str should be "build" or "plan".
    void update_mode(const std::string& session_id, const std::string& mode_str);
    // Patch the "auto_edits" and "yolo" permission-flag fields inside the
    // session's model_json blob. No-op if the session does not exist.
    void update_permission_flags(const std::string& session_id,
                                 bool auto_edits, bool yolo);
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
    std::vector<SessionMessage> load_messages(const std::string& session_id);

    // Replace the conversation head (everything with seq < keep_from_seq)
    // with a single compaction_summary message. Runs inside one transaction.
    // The summary row takes seq 0 (always free after the head delete, since
    // seqs start at 1) so it precedes the untouched tail and load_messages'
    // ORDER BY seq ASC keeps it first. keep_from_seq <= 1 is a no-op.
    void compact_messages(const std::string& session_id,
                          int keep_from_seq,
                          const std::string& summary_data_json);

    // Atomic whole-list replace for the todo_write tool. Deletes every
    // existing row for the session and inserts the new list in one
    // transaction.
    void replace_todos(const std::string& session_id,
                       const std::vector<Todo>& todos);
    std::vector<Todo> load_todos(const std::string& session_id);

private:
    Database& db_;
    int next_seq(const std::string& session_id);
};

} // namespace haicode
