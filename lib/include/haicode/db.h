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
    // Last provider-reported per-request input size (input + cache_read +
    // cache_write). Seeds the context meter when a session is reopened.
    int last_input_tokens = 0;
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
    void update_title(const std::string& session_id, const std::string& title);
    void update_directory(const std::string& session_id, const std::string& directory);
    void update_cost(const std::string& session_id, double cost, const TokenUsage& tokens);
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

    // Atomic whole-list replace for the todo_write tool. Deletes every
    // existing row for the session and inserts the new list in one
    // transaction.
    void replace_todos(const std::string& session_id,
                       const std::vector<Todo>& todos);
    std::vector<Todo> load_todos(const std::string& session_id);

    // Update the 'output' field of the tool_result row matching `call_id`.
    // Used by the engine to replace the placeholder output echoed by ask_user
    // with the user's real answer after they reply in the UI. No-op if no
    // row matches.
    void update_tool_result_by_call_id(const std::string& call_id,
                                       const std::string& new_output);

private:
    Database& db_;
    int next_seq(const std::string& session_id);
};

} // namespace haicode
