#include <haicode/tool.h>
#include <haicode/util.h>
#include <atomic>
#include <climits>
#include <fnmatch.h>
#include <cstdlib>
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

// Symlink-aware containment. Both sides are resolved with realpath and
// containment is checked on the resolved paths, so a symlink inside `base`
// pointing outside the tree no longer passes — and conversely, a path that
// is inside only after resolution (e.g. under Haiku's /tmp symlink) passes
// even when the two inputs are asymmetrically resolved. If either realpath
// fails (missing or broken target — nothing to leak), the lexical verdict
// stands (it still catches `..` escapes of nonexistent paths).
static bool path_resolves_within(const std::string& path, const std::string& base) {
    char rp[PATH_MAX], rb[PATH_MAX];
    if (realpath(path.c_str(), rp) && realpath(base.c_str(), rb))
        return is_path_within(rp, rb);
    return is_path_within(path, base);
}

// Roots that read-only tools may always access regardless of config/session
// rules: Haiku's system headers and documentation are ground truth for BeAPI
// work and live outside every project directory.
static bool is_within_always_readable_root(const std::string& path) {
    static const std::vector<std::string> roots = {
        "/boot/system/develop/headers",
        "/boot/system/non-packaged/develop/headers",
        "/boot/system/documentation",
    };
    for (const auto& root : roots) {
        if (path_resolves_within(path, root)) return true;
    }
    return false;
}

void PermissionGate::set_rules(const std::vector<PermissionRule>& rules) {
    std::lock_guard<std::mutex> lock(*mu_);
    rules_ = rules;
}

void PermissionGate::set_session_rules(const std::vector<PermissionRule>& rules) {
    set_session_rules("", rules);
}

void PermissionGate::set_session_rules(const std::string& session_id,
                                       const std::vector<PermissionRule>& rules) {
    std::lock_guard<std::mutex> lock(*mu_);
    session_rules_[session_id] = rules;
}

void PermissionGate::add_allow(const std::string& action,
                               const std::string& resource) {
    add_allow("", action, resource);
}

void PermissionGate::add_allow(const std::string& session_id,
                               const std::string& action,
                               const std::string& resource) {
    PermissionRule r;
    r.action = action;
    r.resource = resource;
    r.effect = PermissionEffect::Allow;
    std::lock_guard<std::mutex> lock(*mu_);
    session_allows_[session_id].push_back(r);
}

PermissionSnapshot PermissionGate::snapshot(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(*mu_);
    PermissionSnapshot result;
    result.configured = rules_;
    auto copy = [&](const auto& map, auto& out) {
        auto it = map.find(session_id);
        if (it != map.end()) out = it->second;
    };
    copy(session_rules_, result.session);
    copy(session_allows_, result.temporary_patterns);
    copy(exact_allows_, result.temporary_exact);
    return result;
}

void PermissionGate::add_exact_allow(const std::string& session_id,
    const std::string& action, const std::string& resource) {
    std::lock_guard<std::mutex> lock(*mu_);
    auto& grants = exact_allows_[session_id];
    for (const auto& grant : grants)
        if (grant.action == action && grant.resource == resource) return;
    grants.push_back({action, resource, PermissionEffect::Allow});
}

bool PermissionGate::revoke_exact_allow(const std::string& session_id,
    const std::string& action, const std::string& resource) {
    std::lock_guard<std::mutex> lock(*mu_);
    auto found = exact_allows_.find(session_id);
    if (found == exact_allows_.end()) return false;
    auto& grants = found->second;
    for (auto it = grants.begin(); it != grants.end(); ++it) {
        if (it->action == action && it->resource == resource) {
            grants.erase(it);
            return true;
        }
    }
    return false;
}

bool PermissionGate::revoke_pattern_allow(const std::string& session_id,
    const std::string& action, const std::string& resource) {
    std::lock_guard<std::mutex> lock(*mu_);
    auto found = session_allows_.find(session_id);
    if (found == session_allows_.end()) return false;
    auto& grants = found->second;
    for (auto it = grants.begin(); it != grants.end(); ++it) {
        if (it->action == action && it->resource == resource) {
            grants.erase(it);
            return true;
        }
    }
    return false;
}

void PermissionGate::revoke_temporary_allows(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(*mu_);
    exact_allows_.erase(session_id);
    session_allows_.erase(session_id);
}

AuthorizationDecision PermissionGate::evaluate(const std::string& session_id,
    const std::string& action, const std::string& resource) const {
    auto state = snapshot(session_id);
    for (size_t i = 0; i < state.temporary_exact.size(); ++i) {
        const auto& rule = state.temporary_exact[i];
        if (rule.action == action && rule.resource == resource)
            return {PermissionEffect::Allow, "Exact temporary session grant",
                "temporary_exact", static_cast<int>(i)};
    }
    auto match = [&](const auto& rules, const std::string& source) {
        for (int i = static_cast<int>(rules.size()) - 1; i >= 0; --i) {
            const auto& rule = rules[i];
            if (fnmatch(rule.action.c_str(), action.c_str(), 0) == 0
                && fnmatch(rule.resource.c_str(), resource.c_str(), 0) == 0)
                return AuthorizationDecision{rule.effect, "Matched " + source + " rule",
                    source, i};
        }
        return AuthorizationDecision{};
    };
    for (auto decision : {match(state.temporary_patterns, "temporary_pattern"),
            match(state.session, "session"), match(state.configured, "configuration")})
        if (decision.effect != PermissionEffect::Ask) return decision;
    return {PermissionEffect::Ask, "No applicable Allow or Deny; approval required", "prompt"};
}

void PermissionGate::set_ask_callback(AskCallback cb) {
    std::lock_guard<std::mutex> lock(*mu_);
    ask_cb_ = std::move(cb);
}

void PermissionGate::set_ask_callback_ex(AskCallbackEx cb) {
    std::lock_guard<std::mutex> lock(*mu_);
    ask_cb_ex_ = std::move(cb);
}

PermissionEffect PermissionGate::check_tool(const ToolContext& ctx,
                                            const std::string& action,
                                            const std::string& resource,
                                            const nlohmann::json& input) {
    auto decision = evaluate(ctx.session_id, action, resource);
    if (decision.effect != PermissionEffect::Ask) return decision.effect;
    AskCallbackEx ask_ex;
    AskCallback ask;
    {
        std::lock_guard<std::mutex> lock(*mu_);
        ask_ex = ask_cb_ex_;
        ask = ask_cb_;
    }
    // Context-aware callback preferred; legacy callback keeps old callers
    // (library tests) working when only it is registered.
    if (ask_ex) return ask_ex(ctx, ctx.tool_name, action, resource, input);
    if (ask) return ask(ctx.session_id, action, resource, input);
    return PermissionEffect::Ask;
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
    return check("", action, resource, input);
}

PermissionEffect PermissionGate::check(const std::string& session_id,
                                       const std::string& action,
                                       const std::string& resource,
                                       const nlohmann::json& input) {
    auto decision = evaluate(session_id, action, resource);
    if (decision.effect != PermissionEffect::Ask) return decision.effect;
    AskCallback ask;
    {
        std::lock_guard<std::mutex> lock(*mu_);
        ask = ask_cb_;
    }
    if (ask) return ask(session_id, action, resource, input);
    return PermissionEffect::Ask;
}

// Symlink-aware readability check shared by the gate's read-only bypass and
// ReadTool's O_NOFOLLOW fallback (see tool.h). Defined here next to the
// containment helpers it builds on.
bool path_is_always_readable(const std::string& path,
                             const std::string& working_dir) {
    if (!working_dir.empty() && path_resolves_within(path, working_dir))
        return true;
    return is_within_always_readable_root(path);
}

// ---- tool_allowed_in_mode ----
//
// Plan and Chat use fail-closed allowlists: anything not explicitly safe for
// the mode is refused. Build mode has no restriction. The engine reuses this
// for the wire-request filter so the two checks can never diverge.
bool tool_allowed_in_mode(const std::string& tool_name, SessionMode mode) {
    switch (mode) {
    case SessionMode::Plan: {
        static const std::set<std::string> plan_allowed = {
            "read", "glob", "grep", "ls", "find",
            "web_search", "web_extract",
            "diff", "todo_write", "ask_user",
            "propose_plan", "discard_plan",
            "screenshot",
        };
        return plan_allowed.count(tool_name) > 0;
    }
    case SessionMode::Chat: {
        static const std::set<std::string> chat_allowed = {
            "web_search", "web_extract", "todo_write", "ask_user",
        };
        return chat_allowed.count(tool_name) > 0;
    }
    case SessionMode::Build:
        return true;
    }
    return true;
}

// ---- tool availability ----

static std::atomic<bool> sOfflineMode{false};

void set_offline_mode(bool offline) {
    sOfflineMode.store(offline);
}

bool offline_mode() {
    return sOfflineMode.load();
}

bool tool_available(const std::string& tool_name, SessionMode mode) {
    return tool_allowed_in_mode(tool_name, mode)
        && !(offline_mode() && (tool_name == "web_search" || tool_name == "web_extract"));
}

// ---- git_invocation_is_readonly ----
//
// True only for invocations that provably cannot mutate the repo or write
// files. The whole invocation is classified — the subcommand alone is not
// enough (`git branch -D foo` deletes, `git stash clear` wipes). Anything
// unrecognized fails closed and goes through the gate.
bool git_invocation_is_readonly(const std::string& subcommand,
                                const std::vector<std::string>& args) {
    // Any of these can redirect git's output to a file, which is a write.
    for (const auto& a : args) {
        if (a == "--output" || a.rfind("--output=", 0) == 0)
            return false;
    }

    // Unconditionally read-only subcommands (protected from `--output` above).
    static const std::set<std::string> always = {
        "status", "diff", "log", "show", "blame",
        "ls-files", "shortlog", "describe", "rev-parse",
    };
    if (always.count(subcommand)) return true;

    auto is_listing_flag = [](const std::string& a) -> bool {
        static const std::set<std::string> flags = {
            "-l", "--list", "-a", "--all", "-r", "--remotes",
            "-v", "-vv", "--verbose", "-q", "--quiet",
            "-n", "--show-current",
        };
        if (flags.count(a)) return true;
        static const char* prefixes[] = {
            "--format=", "--contains", "--merged", "--no-merged",
            "--points-at", "--sort=",
        };
        for (const char* p : prefixes)
            if (a.rfind(p, 0) == 0) return true;
        return false;
    };

    // branch/tag: listing forms only. A positional argument creates
    // (`git branch foo`, `git tag v1`) and unknown flags fail closed —
    // EXCEPT after an explicit list flag, where positionals are patterns
    // (`git tag -l 'v*'`, `git branch --list feat*`). For branch only the
    // long form triggers pattern mode: `-l` wobbled historically between
    // --list and reflog-create, so it alone never unlocks positionals.
    if (subcommand == "branch" || subcommand == "tag") {
        if (args.empty()) return true;
        bool list_mode = (args[0] == "--list")
                      || (subcommand == "tag" && args[0] == "-l");
        for (const auto& a : args) {
            if (is_listing_flag(a)) continue;
            if (list_mode && !a.empty() && a[0] != '-') continue;  // pattern
            return false;
        }
        return true;
    }

    // stash: only `list` and `show` read. Bare `git stash` pushes.
    if (subcommand == "stash") {
        return !args.empty() && (args[0] == "list" || args[0] == "show");
    }

    return false;
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
    ToolResult r;
    try {
        r = execute_impl(name, input, ctx, gate);
    } catch (const std::exception& e) {
        r.success = false;
        r.error   = "tool '" + name + "' failed: " + e.what();
    } catch (...) {
        r.success = false;
        r.error   = "tool '" + name + "' failed with an unknown exception";
    }
    // Tool output is external content (web pages, command output) and may
    // contain invalid UTF-8 — sanitize so downstream JSON serialization can't
    // throw.
    if (!r.output.empty()) r.output = util::sanitize_utf8(r.output);
    if (!r.error.empty())  r.error  = util::sanitize_utf8(r.error);
    return r;
}

AuthorizationDecision ToolRegistry::evaluate(const std::string& name,
                                  const nlohmann::json& input,
                                  const ToolContext& ctx,
                                  const PermissionGate& gate) const {
    auto tool = get(name);
    if (!tool)
        return {PermissionEffect::Deny, "Unknown tool: " + name, "unknown", -1, true};

    if (!tool_allowed_in_mode(name, ctx.mode))
        return {PermissionEffect::Deny,
            "[mode restriction] tool '" + name + "' is not available in "
                + (ctx.mode == SessionMode::Chat ? "chat" : "plan") + " mode",
            "mode", -1, true};
    if (!tool_available(name, ctx.mode))
        return {PermissionEffect::Deny,
            "[offline mode] tool '" + name + "' is unavailable while offline",
            "offline", -1, true};

    auto builtin = [](const std::string& reason) {
        return AuthorizationDecision{PermissionEffect::Allow, reason, "builtin"};
    };
    if (!ctx.working_dir.empty()) {
        if (name == "read" || name == "ls" || name == "grep" ||
            name == "diff" || name == "find" || name == "symbols") {
            if (path_is_always_readable(tool->resource(input, ctx), ctx.working_dir))
                return builtin("Read within project or trusted system root");
        }
        if (name == "glob") {
            std::string pattern = input.value("pattern", "");
            size_t wild = pattern.find_first_of("*?[");
            std::string prefix = wild == std::string::npos ? pattern : pattern.substr(0, wild);
            if (!prefix.empty() && prefix[0] != '/') {
                std::string base = ctx.working_dir;
                while (!base.empty() && base.back() == '/') base.pop_back();
                prefix = base + "/" + prefix;
            }
            if (prefix.empty()) prefix = ctx.working_dir;
            std::string base = normalize_path(prefix);
            if (path_resolves_within(base, ctx.working_dir) || is_within_always_readable_root(base))
                return builtin("Glob within project or trusted system root");
        }
        if (name == "git") {
            std::vector<std::string> args;
            if (input.contains("args") && input["args"].is_array())
                for (const auto& arg : input["args"])
                    if (arg.is_string()) args.push_back(arg.get<std::string>());
            if (git_invocation_is_readonly(input.value("subcommand", ""), args))
                return builtin("Read-only Git invocation");
        }
    }
    if (name == "web_search" || name == "web_extract")
        return builtin("Built-in web tool exemption");
    if (name == "propose_plan" || name == "todo_write" || name == "ask_user")
        return builtin("Built-in interaction tool exemption");
    if (name == "screenshot") return builtin("Built-in screenshot exemption");
    if (name == "process") {
        std::string action = input.value("action", "");
        if (action == "list" || action == "check_port")
            return builtin("Read-only process inspection");
    }
    return gate.evaluate(ctx.session_id, tool->required_permission(), tool->resource(input, ctx));
}

ToolResult ToolRegistry::execute_impl(const std::string& name,
    const nlohmann::json& input, const ToolContext& ctx, PermissionGate& gate) {
    auto decision = evaluate(name, input, ctx, gate);
    if (decision.blocked) return {false, "", decision.reason};
    auto tool = get(name);
    auto perm = decision.effect;
    if (perm == PermissionEffect::Ask) {
        perm = gate.check_tool(ctx, tool->required_permission(),
                               tool->resource(input, ctx), input);
        // An interrupt that landed while the user was deciding must stop the
        // call before it executes — approval waits can take arbitrarily long,
        // and the loop's pre-call interrupt check has already passed.
        if (perm == PermissionEffect::Allow && ctx.interrupt
                && ctx.interrupt->load())
            return {false, "",
                    "[interrupted] tool '" + name
                        + "' not run: run interrupted", true};
    }
    if (perm != PermissionEffect::Allow) {
        return {false, "", perm == PermissionEffect::Deny
            ? "Permission denied for tool: " + name
            : "Permission not granted for tool: " + name
                + " (no applicable Allow rule or user approval)", true};
    }
    return tool->execute(input, ctx);
}

} // namespace haicode
