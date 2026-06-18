#include <haicode/tool.h>
#include <fnmatch.h>
#include <set>
#include <string>
#include <vector>

namespace haicode {

// Lexically normalize a POSIX path: collapses "//", resolves "." and "..",
// preserves whether the input was absolute. Does not touch the filesystem,
// so a ".." escape in `path` is caught even if the resolved target is missing.
static std::string normalize_path(const std::string& p) {
    bool absolute = (!p.empty() && p[0] == '/');
    std::vector<std::string> parts;
    std::string seg;
    auto flush = [&]() {
        if (seg == "..") {
            if (!parts.empty() && parts.back() != "..") parts.pop_back();
            else if (!absolute) parts.push_back("..");
        } else if (seg != "." && !seg.empty()) {
            parts.push_back(seg);
        }
        seg.clear();
    };
    for (char c : p) {
        if (c == '/') flush();
        else seg += c;
    }
    flush();

    if (absolute) {
        std::string out = "/";
        for (size_t i = 0; i < parts.size(); i++) {
            if (i > 0) out += "/";
            out += parts[i];
        }
        return out;
    }
    if (parts.empty()) return ".";
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i > 0) out += "/";
        out += parts[i];
    }
    return out;
}

// True if `path` is `base` itself or somewhere underneath it. Both inputs are
// normalized lexically first; both must be absolute for an unambiguous answer.
static bool is_path_within(const std::string& path, const std::string& base) {
    if (path.empty() || base.empty()) return false;
    std::string npath = normalize_path(path);
    std::string nbase = normalize_path(base);
    if (npath.empty() || npath[0] != '/') return false;
    if (nbase.empty() || nbase[0] != '/') return false;
    if (nbase == "/") return true;
    if (npath == nbase) return true;
    return npath.size() > nbase.size()
        && npath[nbase.size()] == '/'
        && npath.compare(0, nbase.size(), nbase) == 0;
}

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

    // Read-only tools inside the working directory are always allowed — no
    // prompt, no rule lookup. The user has implicitly trusted the project
    // tree by opening it. Operations outside the working dir still go
    // through the gate.
    if (!ctx.working_dir.empty()) {
        // read, ls, grep, diff, find: resource() returns a resolved absolute path.
        if (name == "read" || name == "ls" || name == "grep" ||
            name == "diff" || name == "find" || name == "symbols") {
            if (is_path_within(tool->resource(input, ctx), ctx.working_dir))
                return tool->execute(input, ctx);
        }
        // glob: resource() returns the raw pattern. Relative patterns always
        // expand under working_dir. For absolute patterns, extract the
        // literal prefix before any wildcard and verify it's inside the tree.
        if (name == "glob") {
            std::string pattern = input.value("pattern", "");
            if (pattern.empty() || pattern[0] != '/') {
                return tool->execute(input, ctx);
            }
            size_t wild = pattern.find_first_of("*?[");
            std::string prefix = (wild == std::string::npos)
                                    ? pattern
                                    : pattern.substr(0, wild);
            if (is_path_within(normalize_path(prefix), ctx.working_dir))
                return tool->execute(input, ctx);
        }
        // git read-only subcommands never modify the repo — always allow.
        if (name == "git") {
            static const std::set<std::string> git_readonly = {
                "status", "diff", "log", "show", "branch", "blame",
                "ls-files", "shortlog", "describe", "rev-parse",
            };
            if (git_readonly.count(input.value("subcommand", "")))
                return tool->execute(input, ctx);
        }
    }
    // Web tools have no filesystem side effects — always allow.
    if (name == "web_search" || name == "web_extract")
        return tool->execute(input, ctx);

    // propose_plan and todo_write only write internal state — always allow.
    if (name == "propose_plan" || name == "todo_write")
        return tool->execute(input, ctx);

    // process list and check_port are read-only — always allow.
    if (name == "process") {
        std::string action = input.value("action", "");
        if (action == "list" || action == "check_port")
            return tool->execute(input, ctx);
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
