#include <haicode/tool.h>
#include <haicode/util.h>
#include <atomic>
#include <climits>
#include <fnmatch.h>
#include <cstdlib>
#include <cstring>
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

void PermissionGate::erase_session_state(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(*mu_);
    session_rules_.erase(session_id);
    session_allows_.erase(session_id);
    exact_allows_.erase(session_id);
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
    // Task 20: a bash Allow pattern carrying glob metacharacters must cover
    // EVERY ;/&&/||/|-separated segment of the command, not just its first
    // word — whole-string fnmatch would let `make*` authorize
    // `make; rm -rf ~`. Deny patterns keep whole-string matching
    // (over-matching a Deny is the safe direction); literal patterns are
    // exact-match by nature.
    auto resource_matches = [&](const PermissionRule& rule) {
        if (action == "bash" && rule.effect == PermissionEffect::Allow
                && rule.resource.find_first_of("*?[") != std::string::npos)
            return bash_pattern_authorizes(rule.resource, resource);
        return fnmatch(rule.resource.c_str(), resource.c_str(), 0) == 0;
    };
    auto match = [&](const auto& rules, const std::string& source, bool& matched) {
        for (int i = static_cast<int>(rules.size()) - 1; i >= 0; --i) {
            const auto& rule = rules[i];
            if (fnmatch(rule.action.c_str(), action.c_str(), 0) == 0
                && resource_matches(rule)) {
                matched = true;
                return AuthorizationDecision{rule.effect, "Matched " + source + " rule",
                    source, i};
            }
        }
        matched = false;
        return AuthorizationDecision{};
    };
    // An Ask match falls through to the next layer (precedence unchanged),
    // but the highest-precedence matched Ask is remembered and reported
    // instead of the anonymous no-match default: ToolRegistry::evaluate
    // distinguishes "an explicit rule said Ask" from "nothing matched" so a
    // configured Ask rule can override a built-in exemption.
    const std::vector<PermissionRule>* layers[] = {
        &state.temporary_patterns, &state.session, &state.configured};
    const char* sources[] = {"temporary_pattern", "session", "configuration"};
    AuthorizationDecision ask_decision;
    bool have_ask = false;
    for (size_t l = 0; l < 3; ++l) {
        bool matched = false;
        auto decision = match(*layers[l], sources[l], matched);
        if (!matched) continue;
        if (decision.effect != PermissionEffect::Ask) return decision;
        if (!have_ask) { ask_decision = decision; have_ask = true; }
    }
    if (have_ask) return ask_decision;
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
// for the wire-request filter so the two checks can never diverge. `git` is
// listed for Plan because reviews need history and diffs; ToolRegistry::
// evaluate narrows it to read-only invocations there.
bool tool_allowed_in_mode(const std::string& tool_name, SessionMode mode) {
    switch (mode) {
    case SessionMode::Plan: {
        static const std::set<std::string> plan_allowed = {
            "read", "glob", "grep", "ls", "find", "symbols", "git",
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

// Long options that turn an otherwise read-only invocation into a file
// write, a program run, or a read of an arbitrary file.
static const char* const kGitUnsafeLongOptions[] = {
    "--output",               // diff/log family: writes the output to a file
    "--ext-diff",             // runs diff.external / diff.<driver>.command
    "--textconv",             // runs diff.<driver>.textconv
    "--filters",              // cat-file: runs smudge/clean filter drivers
    "--no-index",             // diff/grep: arbitrary filesystem paths
    "--contents",             // blame: annotates an arbitrary file's contents
    "--ignore-revs-file",     // blame: reads an arbitrary file
    "--open-files-in-pager",  // grep -O: runs a program
    "--file",                 // grep -f: reads patterns from an arbitrary file
};

// True when `arg` names one of kGitUnsafeLongOptions, exactly or as an
// abbreviation: parse-options subcommands (blame, grep, cat-file) accept
// any unambiguous prefix, so `--content=/etc/passwd` means `--contents`
// and `cat-file --text` means `--textconv`. An exact real option wins over
// the abbreviation, so a subcommand's own options that happen to prefix an
// unsafe one stay allowed — but only for the subcommands that have them.
static bool git_arg_is_unsafe_long_option(const std::string& subcommand,
                                          const std::string& arg) {
    if (arg.size() <= 2 || arg.compare(0, 2, "--") != 0) return false;
    std::string name = arg.substr(0, arg.find('='));
    if (name == "--text" && subcommand != "cat-file") return false;
    if (name == "--ignore-rev" && subcommand == "blame") return false;
    if (name == "--filter" && subcommand == "rev-list") return false;
    for (const char* opt : kGitUnsafeLongOptions) {
        std::string o(opt);
        if (name.size() <= o.size() && o.compare(0, name.size(), name) == 0)
            return true;
    }
    return false;
}

// Short-option clusters that run a program or read an arbitrary file:
// `grep -O<cmd>` opens a pager program, `grep -f <file>` reads a pattern
// file, `blame -S <file>` reads a revs file. A value-taking letter (`grep
// -e`, `blame -L`) ends the cluster — the rest is its value — and with
// nothing after it, the next argument is the value; `*value_next` reports
// that so the caller skips checking it as an option.
static bool git_short_cluster_is_unsafe(const std::string& subcommand,
                                        const std::string& arg, bool* value_next) {
    *value_next = false;
    if (arg.size() < 2 || arg[0] != '-' || arg[1] == '-') return false;
    const char* unsafe = subcommand == "grep" ? "Of" : subcommand == "blame" ? "S" : "";
    char value_letter = subcommand == "grep" ? 'e' : subcommand == "blame" ? 'L' : 0;
    for (size_t i = 1; i < arg.size(); ++i) {
        if (arg[i] != '\0' && std::strchr(unsafe, arg[i])) return true;
        if (arg[i] == value_letter) {
            *value_next = i + 1 == arg.size();
            return false;
        }
    }
    return false;
}

// True when a positional argument names a path outside `working_dir`. git
// diff silently switches to --no-index (and prints any file it can read)
// when one of its paths lies outside the work tree. Revisions and ranges
// (`HEAD~1`, `main...topic`) resolve lexically inside, so they pass.
static bool git_arg_escapes(const std::string& arg, const std::string& working_dir) {
    if (arg.empty()) return false;
    if (working_dir.empty()) {
        if (arg[0] == '/') return true;
        std::string n = normalize_path(arg);
        return n == ".." || n.rfind("../", 0) == 0;
    }
    std::string p = arg[0] == '/' ? arg : working_dir + "/" + arg;
    return !path_resolves_within(p, working_dir);
}

// ---- git_invocation_is_readonly ----
//
// True only for invocations that provably cannot mutate the repo, write
// files, run a program, or read outside the project (the GitTool adds the
// -c/flag hardening that keeps repo config from running drivers on these).
// The whole invocation is classified — the subcommand alone is not
// enough (`git branch -D foo` deletes, `git stash clear` wipes). Anything
// unrecognized fails closed and goes through the gate.
bool git_invocation_is_readonly(const std::string& subcommand,
                                const std::vector<std::string>& args,
                                const std::string& working_dir) {
    bool skip_value = false;
    for (const auto& a : args) {
        if (skip_value) { skip_value = false; continue; }
        if (git_arg_is_unsafe_long_option(subcommand, a)) return false;
        if (git_short_cluster_is_unsafe(subcommand, a, &skip_value)) return false;
    }

    if (subcommand == "diff") {
        bool after_separator = false;
        for (const auto& a : args) {
            if (!after_separator && a == "--") { after_separator = true; continue; }
            if (!after_separator && !a.empty() && a[0] == '-') continue;
            if (git_arg_escapes(a, working_dir)) return false;
        }
    }

    // Unconditionally read-only subcommands (protected by the checks above).
    static const std::set<std::string> always = {
        "status", "diff", "log", "show", "blame",
        "ls-files", "shortlog", "describe", "rev-parse",
        "merge-base", "rev-list", "ls-tree", "cat-file", "grep",
        "for-each-ref",
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

// ---- bash_pattern_authorizes ----
//
// A bash Allow pattern carrying glob metacharacters must cover EVERY
// command in the invocation, not just its first word: fnmatch on the whole
// string lets `make*` authorize `make; rm -rf ~` (the `*` happily matches
// the rest). The command is split quote-aware at the shell's command
// separators (`;`, newline, `|`, `|&`, `||`, `&&`, and `&` — which needs no
// surrounding whitespace: `make&rm x` is two commands) and each segment
// must fnmatch the pattern. A segment that can do more than the pattern
// shows never matches any pattern except the universal `*` — the safe
// direction is to ask:
//   - command substitution (`$(...)`, backticks) and process substitution
//     (`<(...)`, `>(...)`) run commands whose text the pattern never sees;
//   - an output redirect to a file (`>`, `>>`, `>|`, `&>`, `>&word`, `<>`)
//     writes anywhere (`echo x > ~/.profile`); fd duplications (`2>&1`,
//     `>&-`) and `/dev/null` targets stay matchable.
bool bash_pattern_authorizes(const std::string& pattern, const std::string& command) {
    if (pattern == "*") return true;

    std::vector<std::string> segments;
    std::vector<bool> unmatchable;
    std::string cur;
    bool cur_unmatchable = false;
    char quote = 0;
    auto flush = [&]() {
        size_t b = cur.find_first_not_of(" \t\r");
        if (b == std::string::npos) { cur.clear(); cur_unmatchable = false; return; }
        size_t e = cur.find_last_not_of(" \t\r");
        segments.push_back(cur.substr(b, e - b + 1));
        unmatchable.push_back(cur_unmatchable);
        cur.clear();
        cur_unmatchable = false;
    };
    auto marks_subst = [&](const std::string& s, size_t i) {
        return s[i] == '`'
            || (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '(');
    };
    // Called with `i` on a `>` outside quotes that does not open a process
    // substitution. True when the redirect it starts can only duplicate a
    // file descriptor or write /dev/null. The target word is read up to
    // whitespace or an operator character; a quoted or expanded target is
    // never `/dev/null` verbatim, so it counts as a file (over-strict).
    auto redirect_is_harmless = [&](size_t i) {
        size_t j = i + 1;
        bool dup = false;
        if (j < command.size() && (command[j] == '>' || command[j] == '|')) ++j;
        else if (j < command.size() && command[j] == '&') { ++j; dup = true; }
        while (j < command.size() && (command[j] == ' ' || command[j] == '\t')) ++j;
        size_t k = j;
        while (k < command.size()
               && std::strchr(" \t\r\n;|&<>()", command[k]) == nullptr) ++k;
        std::string target = command.substr(j, k - j);
        if (target == "/dev/null") return true;
        return dup && !target.empty()
            && target.find_first_not_of("0123456789-") == std::string::npos;
    };
    for (size_t i = 0; i < command.size(); ++i) {
        char c = command[i];
        if (quote) {
            // Substitution inside double quotes really executes; inside
            // single quotes it is literal — flagging both is over-strict,
            // which is the safe direction.
            if (marks_subst(command, i)) cur_unmatchable = true;
            if (quote == '"' && c == '\\' && i + 1 < command.size()) {
                cur += c;
                cur += command[++i];
                continue;
            }
            if (c == quote) quote = 0;
            cur += c;
            continue;
        }
        if (c == '\\') {
            if (i + 1 < command.size()) { cur += c; cur += command[++i]; }
            continue;
        }
        if (c == '\'' || c == '"') { quote = c; cur += c; continue; }
        if (c == ';' || c == '\n') { flush(); continue; }
        if (c == '|') {
            flush();
            // `||` is one operator; `|&` pipes stderr too.
            if (i + 1 < command.size()
                    && (command[i + 1] == '|' || command[i + 1] == '&')) ++i;
            continue;
        }
        if (c == '&') {
            if (i + 1 < command.size() && command[i + 1] == '&') {
                flush();
                ++i;
                continue;
            }
            // A lone `&` backgrounds the left side and starts a new command,
            // with or without whitespace around it (`make&rm x`). It belongs
            // to a redirect only after `>`/`<` (`2>&1`, `>&2`, `<&3`) or
            // before `>` (`&>log`, `&>>log`) — the `>` branch judges those.
            bool redirect = (i > 0 && (command[i - 1] == '>' || command[i - 1] == '<'))
                         || (i + 1 < command.size() && command[i + 1] == '>');
            if (!redirect) { flush(); continue; }
            cur += c;
            continue;
        }
        if ((c == '<' || c == '>') && i + 1 < command.size() && command[i + 1] == '(') {
            cur_unmatchable = true;  // process substitution runs a command
        } else if (c == '>') {
            // `>>`, `>|`, `>&` are judged as one operator from the first `>`;
            // the second `>` of `>>` is skipped so it isn't judged twice.
            if (!redirect_is_harmless(i)) cur_unmatchable = true;
            if (i + 1 < command.size() && command[i + 1] == '>') { cur += c; c = command[++i]; }
        }
        if (marks_subst(command, i)) cur_unmatchable = true;
        cur += c;
    }
    flush();

    if (segments.empty()) return true;
    for (size_t i = 0; i < segments.size(); ++i) {
        if (unmatchable[i]) return false;
        if (fnmatch(pattern.c_str(), segments[i].c_str(), 0) != 0) return false;
    }
    return true;
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

// The directory a glob pattern reads from: its literal prefix before the
// first wildcard, joined onto `working_dir` when relative, lexically
// normalized. Shared by the system-root and project-dir glob exemptions.
static std::string glob_base_dir(const nlohmann::json& input,
                                 const std::string& working_dir) {
    std::string pattern = input.value("pattern", "");
    size_t wild = pattern.find_first_of("*?[");
    std::string prefix = wild == std::string::npos ? pattern : pattern.substr(0, wild);
    if (!prefix.empty() && prefix[0] != '/') {
        std::string base = working_dir;
        while (!base.empty() && base.back() == '/') base.pop_back();
        prefix = base + "/" + prefix;
    }
    if (prefix.empty()) prefix = working_dir;
    return normalize_path(prefix);
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

    std::vector<std::string> git_args;
    if (name == "git" && input.contains("args") && input["args"].is_array())
        for (const auto& arg : input["args"])
            if (arg.is_string()) git_args.push_back(arg.get<std::string>());
    const std::string git_sub = name == "git" ? input.value("subcommand", "") : "";
    // Plan mode is read-only: git is offered there for reviews, but no rule,
    // grant, or toggle may let it run a mutating invocation — same hard
    // boundary as the allowlist above.
    if (name == "git" && ctx.mode == SessionMode::Plan
            && !git_invocation_is_readonly(git_sub, git_args, ctx.working_dir))
        return {PermissionEffect::Deny,
            "[mode restriction] git is read-only in plan mode; 'git " + git_sub
                + "' with these arguments can modify the repository, run a "
                  "program, or read outside the project",
            "mode", -1, true};

    auto builtin = [](const std::string& reason) {
        return AuthorizationDecision{PermissionEffect::Allow, reason, "builtin"};
    };
    // Absolute exemption: Haiku's system header/doc roots are ground truth
    // for BeAPI work and live outside every project — no rule layer may
    // deny them. Returns immediately.
    if (!ctx.working_dir.empty()) {
        if (name == "read" || name == "ls" || name == "grep" ||
            name == "diff" || name == "find" || name == "symbols") {
            if (is_within_always_readable_root(tool->resource(input, ctx)))
                return builtin("Read within trusted system root");
        }
        if (name == "glob") {
            std::string base = glob_base_dir(input, ctx.working_dir);
            if (is_within_always_readable_root(base))
                return builtin("Glob within trusted system root");
        }
    }
    // Overridable exemptions: working-dir containment, read-only git, web
    // and interaction tools, screenshot, read-only process inspection. Held
    // as a fallback and returned only when no explicit rule layer matched,
    // so a configured Deny/Ask on any of these takes effect.
    AuthorizationDecision exemption;
    bool has_exemption = false;
    if (!ctx.working_dir.empty()) {
        if (name == "read" || name == "ls" || name == "grep" ||
            name == "diff" || name == "find" || name == "symbols") {
            if (path_resolves_within(tool->resource(input, ctx), ctx.working_dir)) {
                exemption = builtin("Read within project directory");
                has_exemption = true;
            }
        }
        if (name == "glob") {
            std::string base = glob_base_dir(input, ctx.working_dir);
            if (path_resolves_within(base, ctx.working_dir)) {
                exemption = builtin("Glob within project directory");
                has_exemption = true;
            }
        }
        if (name == "git") {
            if (git_invocation_is_readonly(git_sub, git_args, ctx.working_dir)) {
                exemption = builtin("Read-only Git invocation");
                has_exemption = true;
            }
        }
    }
    if (name == "web_search" || name == "web_extract") {
        exemption = builtin("Built-in web tool exemption");
        has_exemption = true;
    }
    if (name == "propose_plan" || name == "todo_write" || name == "ask_user") {
        exemption = builtin("Built-in interaction tool exemption");
        has_exemption = true;
    }
    if (name == "screenshot") {
        exemption = builtin("Built-in screenshot exemption");
        has_exemption = true;
    }
    if (name == "process") {
        std::string action = input.value("action", "");
        if (action == "list" || action == "check_port") {
            exemption = builtin("Read-only process inspection");
            has_exemption = true;
        }
    }
    auto g = gate.evaluate(ctx.session_id, tool->required_permission(),
                           tool->resource(input, ctx));
    // Anything other than the anonymous no-match default is an explicit
    // Allow/Deny/Ask from some rule layer (the gate reports a matched Ask
    // with that layer's source). Explicit rules override the built-in
    // exemptions; with no matching rule the exemption — if any — applies.
    if (g.effect != PermissionEffect::Ask || g.source != "prompt")
        return g;
    if (has_exemption) return exemption;
    return g;
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
