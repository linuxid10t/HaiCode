#pragma once
#include "db.h"
#include "events.h"
#include "provider.h"
#include "tool.h"
#include "config.h"
#include <string>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>

namespace haicode {

class ContextBuilder {
public:
    LLMRequest build(const std::vector<SessionMessage>& messages,
                     const std::string& system_prompt,
                     const std::string& system_dynamic,
                     const std::vector<ToolDefinition>& tools,
                     const std::string& model_id,
                     const std::string& provider_id);
private:
    std::vector<nlohmann::json> assemble_messages(const std::vector<SessionMessage>& msgs);
};

class SessionEngine {
public:
    SessionEngine(SessionStore& store,
                  ProviderRegistry& providers,
                  ToolRegistry& tools,
                  PermissionGate& permissions,
                  SessionEventBus& bus,
                  const AppConfig& config);
    ~SessionEngine();

    std::string create_session(const std::string& project_dir,
                                const std::string& agent_id = "",
                                const std::string& model_id = "",
                                const std::string& provider_id = "");

    void submit_prompt(const std::string& session_id, const std::string& text);
    // Resume the agentic loop without adding a new user message (used after plan approval).
    void continue_session(const std::string& session_id);
    void interrupt(const std::string& session_id);

    // Per-session Plan/Build mode. Persisted into model_json so it survives
    // process restarts; also cached in session_modes_ for synchronous reads.
    void      set_mode(const std::string& session_id, SessionMode mode);
    SessionMode get_mode(const std::string& session_id);

    // Patch the active session's stored provider/model. Either string may be
    // empty to leave that field untouched. Takes effect on the next prompt.
    void update_provider_model(const std::string& session_id,
                               const std::string& provider_id,
                               const std::string& model_id);

    // Read the current persisted todo list for a session (used by UIs on
    // session switch). Delegates to SessionStore::load_todos.
    std::vector<Todo> get_todos(const std::string& session_id);

    const AppConfig& config() const { return config_; }

private:
    void agentic_loop(const std::string& session_id);

    SessionStore& store_;
    ProviderRegistry& providers_;
    ToolRegistry& tools_;
    PermissionGate& permissions_;
    SessionEventBus& bus_;
    AppConfig config_;

    std::map<std::string, std::thread> runner_threads_;
    std::map<std::string, std::atomic<bool>*> interrupt_flags_;
    std::map<std::string, bool> session_running_;  // true while agentic_loop is executing
    std::map<std::string, SessionMode> session_modes_;
    std::mutex mu_;
};

} // namespace haicode
