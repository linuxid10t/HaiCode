#pragma once
#include "types.h"
#include "provider.h"
#include <string>
#include <functional>
#include <map>
#include <memory>
#include <vector>
#include <nlohmann/json.hpp>

namespace haicode {

struct ToolContext {
    std::string session_id;
    std::string agent_id;
    std::string assistant_message_id;
    std::string call_id;
    std::string working_dir;
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
    using AskCallback = std::function<PermissionEffect(
        const std::string& action,
        const std::string& resource,
        const nlohmann::json& input)>;

    void set_rules(const std::vector<PermissionRule>& rules);
    void set_session_rules(const std::vector<PermissionRule>& rules);
    void add_allow(const std::string& action, const std::string& resource);
    void set_ask_callback(AskCallback cb);

    PermissionEffect check(const std::string& action, const std::string& resource,
                           const nlohmann::json& input = nlohmann::json::object());

private:
    PermissionEffect match_rules(const std::vector<PermissionRule>& rules,
                                  const std::string& action,
                                  const std::string& resource) const;

    std::vector<PermissionRule> rules_;
    std::vector<PermissionRule> session_rules_;
    AskCallback ask_cb_;
};

class ToolRegistry {
public:
    void register_tool(std::shared_ptr<Tool> tool);
    std::vector<ToolDefinition> definitions() const;
    std::shared_ptr<Tool> get(const std::string& name) const;
    ToolResult execute(const std::string& name, const nlohmann::json& input,
                       const ToolContext& ctx, PermissionGate& gate);

private:
    std::map<std::string, std::shared_ptr<Tool>> tools_;
};

} // namespace haicode
