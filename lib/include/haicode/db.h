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
    void delete_session(const std::string& session_id);

    void append_message(const std::string& session_id,
                        const std::string& type,
                        const std::string& data_json);
    std::vector<SessionMessage> load_messages(const std::string& session_id);

private:
    Database& db_;
    int next_seq(const std::string& session_id);
};

} // namespace haicode
