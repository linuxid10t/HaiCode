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

void PermissionGate::set_ask_callback(AskCallback cb) {
    std::lock_guard<std::mutex> lock(*mu_);
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
    return check("", action, resource, input);
}

PermissionEffect PermissionGate::check(const std::string& session_id,
                                       const std::string& action,
                                       const std::string& resource,
                                       const nlohmann::json& input) {
    // Snapshot the relevant layers under the lock; matching and especially
    // the ask callback (which blocks on a future) must run without it.
    std::vector<PermissionRule> config_rules, sess_rules, sess_allows;
    AskCallback ask;
    {
        std::lock_guard<std::mutex> lock(*mu_);
        config_rules = rules_;
        auto sr = session_rules_.find(session_id);
        if (sr != session_rules_.end()) sess_rules = sr->second;
        auto sa = session_allows_.find(session_id);
        if (sa != session_allows_.end()) sess_allows = sa->second;
        ask = ask_cb_;
    }

    // Session layers (Allow-Always grants + toggle rules) take priority
    auto allows_result = match_rules(sess_allows, action, resource);
    if (allows_result != PermissionEffect::Ask)
        return allows_result;
    auto session_result = match_rules(sess_rules, action, resource);
    if (session_result != PermissionEffect::Ask)
        return session_result;

    // Config rules
    auto config_result = match_rules(config_rules, action, resource);
    if (config_result != PermissionEffect::Ask)
        return config_result;

    // Ask the UI
    if (ask)
        return ask(session_id, action, resource, input);

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

ToolResult ToolRegistry::execute_impl(const std::string& name,
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

    // Execution-time mode restriction. The engine filters tools out of the
    // wire request, but a provider may still return a call for a hidden
    // tool — refuse it here, before any always-allow bypass (Chat mode must
    // block even `read`). Deliberately not a permission denial (denied=false)
    // so the turn isn't killed: the failed tool_result persists and the model
    // recovers with an allowed tool or a text reply.
    if (!tool_allowed_in_mode(name, ctx.mode)) {
        ToolResult r;
        r.success = false;
        r.error = "[mode restriction] tool '" + name + "' is not available in "
                + (ctx.mode == SessionMode::Chat ? "chat" : "plan")
                + " mode";
        return r;
    }
    if (!tool_available(name, ctx.mode))
        return {false, "", "[offline mode] tool '" + name + "' is unavailable while offline"};

    // Read-only tools inside the working directory are always allowed — no
    // prompt, no rule lookup. The user has implicitly trusted the project
    // tree by opening it. Operations outside the working dir still go
    // through the gate. Containment is symlink-aware (path_resolves_within)
    // so an in-project symlink pointing outside the tree stays gated.
    if (!ctx.working_dir.empty()) {
        // read, ls, grep, diff, find: resource() returns a resolved absolute path.
        if (name == "read" || name == "ls" || name == "grep" ||
            name == "diff" || name == "find" || name == "symbols") {
            std::string path = tool->resource(input, ctx);
            if (path_is_always_readable(path, ctx.working_dir))
                return tool->execute(input, ctx);
        }
        // glob: resource() returns the raw pattern. Extract the literal
        // prefix before any wildcard; relative patterns are joined with
        // working_dir first (they expand under it). The joined prefix must
        // resolve inside the tree (or an always-readable root) — this also
        // catches a relative pattern leading through a symlinked directory
        // out of the project.
        if (name == "glob") {
            std::string pattern = input.value("pattern", "");
            std::string prefix;
            size_t wild = pattern.find_first_of("*?[");
            prefix = (wild == std::string::npos)
                        ? pattern
                        : pattern.substr(0, wild);
            if (!prefix.empty() && prefix[0] != '/') {
                std::string base = ctx.working_dir;
                while (!base.empty() && base.back() == '/') base.pop_back();
                prefix = base + "/" + prefix;
            }
            if (prefix.empty()) prefix = ctx.working_dir;
            std::string base = normalize_path(prefix);
            if (path_resolves_within(base, ctx.working_dir) ||
                is_within_always_readable_root(base))
                return tool->execute(input, ctx);
        }
        // git: only provably read-only invocations (classified by
        // git_invocation_is_readonly — subcommand AND args) never modify the
        // repo — always allow. `git branch -D x` / `git stash clear` fall
        // through to the gate.
        if (name == "git") {
            std::vector<std::string> git_args;
            if (input.contains("args") && input["args"].is_array()) {
                for (const auto& a : input["args"])
                    if (a.is_string()) git_args.push_back(a.get<std::string>());
            }
            if (git_invocation_is_readonly(input.value("subcommand", ""),
                                           git_args))
                return tool->execute(input, ctx);
        }
    }
    // Web tools have no filesystem side effects — always allow.
    if (name == "web_search" || name == "web_extract")
        return tool->execute(input, ctx);

    // propose_plan, todo_write, and ask_user only write internal state or ask
    // the user a question — always allow.
    if (name == "propose_plan" || name == "todo_write" || name == "ask_user")
        return tool->execute(input, ctx);

    // screenshot is read-only (captures shared screen state, writes one file
    // to the temp directory) and must work in Plan mode — always allow.
    if (name == "screenshot")
        return tool->execute(input, ctx);

    // process list and check_port are read-only — always allow.
    if (name == "process") {
        std::string action = input.value("action", "");
        if (action == "list" || action == "check_port")
            return tool->execute(input, ctx);
    }

    auto perm = gate.check(ctx.session_id,
                           tool->required_permission(),
                           tool->resource(input, ctx),
                           input);
    // Only an explicit Allow executes. An unresolved Ask — no rule matched
    // and no ask callback resolved it (library consumers without a UI) —
    // must NOT fall through to execution.
    if (perm != PermissionEffect::Allow) {
        ToolResult r;
        r.success = false;
        r.denied  = true;
        r.error   = (perm == PermissionEffect::Deny)
                  ? "Permission denied for tool: " + name
                  : "Permission not granted for tool: " + name
                    + " (no applicable Allow rule or user approval)";
        return r;
    }

    return tool->execute(input, ctx);
}

} // namespace haicode
