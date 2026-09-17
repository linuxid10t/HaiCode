#pragma once
#include "types.h"
#include "provider.h"
#include "config.h"
#include <string>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <nlohmann/json.hpp>

namespace haicode {

struct ToolContext {
    std::string session_id;
    std::string agent_id;
    std::string assistant_message_id;
    std::string call_id;
    std::string working_dir;
    // Read-only view of the merged AppConfig. Set by SessionEngine before tool
    // execution; tools that need configurable behaviour (e.g. web_search)
    // read from this pointer. May be null in tests.
    const AppConfig* config = nullptr;
};

struct ToolResult {
    bool success = true;
    std::string output;
    std::string error;
    bool denied  = false;  // true when stopped by PermissionGate (not an execution error)
};

class Tool {
public:
    virtual ~Tool() = default;
    virtual std::string name() const = 0;
    virtual std::string description() const = 0;
    virtual nlohmann::json input_schema() const = 0;
    virtual std::string required_permission() const { return name(); }
    // Resource string presented to the permission gate. Default is the working
    // directory; tools override to return a meaningful target (command, path,
    // search pattern, etc.) so permission rules can scope by `resource`.
    virtual std::string resource(const nlohmann::json& input,
                                 const ToolContext& ctx) const {
        (void)input;
        return ctx.working_dir;
    }
    virtual ToolResult execute(const nlohmann::json& input, const ToolContext& ctx) = 0;
};

class PermissionGate {
public:
    // session_id is the id of the session the check runs for ("" = unscoped,
    // used by tests and legacy callers).
    using AskCallback = std::function<PermissionEffect(
        const std::string& session_id,
        const std::string& action,
        const std::string& resource,
        const nlohmann::json& input)>;

    void set_rules(const std::vector<PermissionRule>& rules);
    // Un-keyed forms operate on the "" scope (tests / legacy callers).
    void set_session_rules(const std::vector<PermissionRule>& rules);
    void set_session_rules(const std::string& session_id,
                           const std::vector<PermissionRule>& rules);
    void add_allow(const std::string& action, const std::string& resource);
    void add_allow(const std::string& session_id,
                   const std::string& action,
                   const std::string& resource);
    void set_ask_callback(AskCallback cb);

    PermissionEffect check(const std::string& action, const std::string& resource,
                           const nlohmann::json& input = nlohmann::json::object());
    PermissionEffect check(const std::string& session_id,
                           const std::string& action, const std::string& resource,
                           const nlohmann::json& input);

private:
    PermissionEffect match_rules(const std::vector<PermissionRule>& rules,
                                  const std::string& action,
                                  const std::string& resource) const;

    // Held via unique_ptr so the gate remains movable (test helpers return it
    // by value); the pointed-to mutex itself is never relocated.
    mutable std::unique_ptr<std::mutex> mu_ = std::make_unique<std::mutex>();
    std::vector<PermissionRule> rules_;               // config rules, global
    // Toggle-derived rules per session, replaced wholesale per session.
    std::map<std::string, std::vector<PermissionRule>> session_rules_;
    // Allow-Always grants per session, kept separate so replacing a session's
    // rules never wipes its accumulated grants.
    std::map<std::string, std::vector<PermissionRule>> session_allows_;
    AskCallback ask_cb_;
};

class ToolRegistry {
public:
    void register_tool(std::shared_ptr<Tool> tool);
    std::vector<ToolDefinition> definitions() const;
    std::shared_ptr<Tool> get(const std::string& name) const;
    // Executes a tool, converting any exception thrown by the tool into a
    // failed ToolResult and sanitizing output/error to valid UTF-8 — so a
    // misbehaving tool can never crash the engine.
    ToolResult execute(const std::string& name, const nlohmann::json& input,
                       const ToolContext& ctx, PermissionGate& gate);

private:
    ToolResult execute_impl(const std::string& name, const nlohmann::json& input,
                            const ToolContext& ctx, PermissionGate& gate);
    std::map<std::string, std::shared_ptr<Tool>> tools_;
};

} // namespace haicode
