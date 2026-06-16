#include <haicode/tool.h>
#include <fnmatch.h>

namespace haicode {

void PermissionGate::set_rules(const std::vector<PermissionRule>& rules) {
    rules_ = rules;
}

void PermissionGate::set_session_rules(const std::vector<PermissionRule>& rules) {
    session_rules_ = rules;
}

void PermissionGate::add_allow(const std::string& action, const std::string& resource) {
    PermissionRule r;
    r.action = action;
    r.resource = resource;
    r.effect = PermissionEffect::Allow;
    session_rules_.push_back(r);
}

void PermissionGate::set_ask_callback(AskCallback cb) {
    ask_cb_ = std::move(cb);
}

PermissionEffect PermissionGate::match_rules(const std::vector<PermissionRule>& rules,
                                              const std::string& action,
                                              const std::string& resource) const {
    for (auto it = rules.rbegin(); it != rules.rend(); ++it) {
        bool action_match = (it->action == "*" || it->action == action ||
                             fnmatch(it->action.c_str(), action.c_str(), 0) == 0);
        bool resource_match = (it->resource == "*" ||
                               fnmatch(it->resource.c_str(), resource.c_str(), 0) == 0);
        if (action_match && resource_match)
            return it->effect;
    }
    return PermissionEffect::Ask;
}

PermissionEffect PermissionGate::check(const std::string& action,
                                        const std::string& resource,
                                        const nlohmann::json& input) {
    // Session rules (from "allow always" decisions) take priority
    auto session_result = match_rules(session_rules_, action, resource);
    if (session_result != PermissionEffect::Ask)
        return session_result;

    // Config rules
    auto config_result = match_rules(rules_, action, resource);
    if (config_result != PermissionEffect::Ask)
        return config_result;

    // Ask the UI
    if (ask_cb_)
        return ask_cb_(action, resource, input);

    return PermissionEffect::Ask;
}

// ---- ToolRegistry ----

void ToolRegistry::register_tool(std::shared_ptr<Tool> tool) {
    tools_[tool->name()] = std::move(tool);
}

std::vector<ToolDefinition> ToolRegistry::definitions() const {
    std::vector<ToolDefinition> defs;
    for (auto& [name, tool] : tools_) {
        ToolDefinition d;
        d.name = tool->name();
        d.description = tool->description();
        d.input_schema = tool->input_schema();
        defs.push_back(d);
    }
    return defs;
}

std::shared_ptr<Tool> ToolRegistry::get(const std::string& name) const {
    auto it = tools_.find(name);
    if (it == tools_.end()) return nullptr;
    return it->second;
}

ToolResult ToolRegistry::execute(const std::string& name,
                                  const nlohmann::json& input,
                                  const ToolContext& ctx,
                                  PermissionGate& gate) {
    auto tool = get(name);
    if (!tool) {
        ToolResult r;
        r.success = false;
        r.error = "Unknown tool: " + name;
        return r;
    }

    auto perm = gate.check(tool->required_permission(),
                           tool->resource(input, ctx),
                           input);
    if (perm == PermissionEffect::Deny) {
        ToolResult r;
        r.success = false;
        r.denied  = true;
        r.error   = "Permission denied for tool: " + name;
        return r;
    }

    return tool->execute(input, ctx);
}

} // namespace haicode
