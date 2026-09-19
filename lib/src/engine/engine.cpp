#include <haicode/engine.h>
#include <haicode/util.h>
#include <haicode/default_prompt.h>
#include <haicode/pricing.h>
#include <haicode/model_info.h>
#include <haicode/compaction.h>
#include <haicode/skills.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <sys/utsname.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <set>
#include <dirent.h>

namespace haicode {

// Single-quote a string for safe shell embedding: 'value' with ' → '\''.
// Same contract as the static sq() in tools.cpp, duplicated here for the
// build hook's timeout/cd wrapping (tools.cpp's copy is file-local).
static std::string shell_quote(const std::string& s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'') r += "'\\''";
        else r += c;
    }
    r += "'";
    return r;
}

// Hard per-turn ceiling multiplier for the renewable step budget: even a
// turn that keeps earning renewals (each newly completed todo refills the
// window) terminates after this many iterations. Loop-runaway protection
// that renewal must not be able to lift.
static constexpr int kStepCeilingMultiplier = 4;

// Return the content of the most recently written *active* plan file under
// <project_dir>/.haicode/plans/, or empty string if none exist.
// Plans tagged <!-- haicode-status: active --> are live; any other status
// (implemented, discarded, …) is treated as retired and skipped.
// The status header line is stripped before returning the content.
static std::string load_latest_plan(const std::string& project_dir) {
    std::string plans_dir = project_dir + "/.haicode/plans";
    DIR* d = opendir(plans_dir.c_str());
    if (!d) return {};

    std::string latest_name;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string n = ent->d_name;
        if (n.size() < 4 || n.substr(n.size() - 3) != ".md") continue;
        if (n <= latest_name) continue;
        // Only consider plans marked active.
        std::ifstream probe(plans_dir + "/" + n);
        std::string first_line;
        if (std::getline(probe, first_line) &&
            first_line.find("haicode-status: active") != std::string::npos) {
            latest_name = n;
        }
    }
    closedir(d);

    if (latest_name.empty()) return {};

    std::ifstream f(plans_dir + "/" + latest_name);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();

    // Strip the status header line so it doesn't appear in the prompt.
    auto newline = content.find('\n');
    if (newline != std::string::npos &&
        content.substr(0, newline).find("haicode-status:") != std::string::npos) {
        content = content.substr(newline + 1);
    }
    return content;
}

static void substitute_all(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

static std::string render_prompt(const std::string& tmpl,
                                 const std::string& model,
                                 const std::string& os_info,
                                 const std::string& project_dir,
                                 int steps_left) {
    std::string out = tmpl;
    substitute_all(out, "{{MODEL}}", model);
    substitute_all(out, "{{OS}}", os_info);
    substitute_all(out, "{{PROJECT_DIR}}", project_dir);
    substitute_all(out, "{{STEPS_LEFT}}", std::to_string(steps_left));
    return out;
}

// Wire label for a SessionMode. Shared by the skills-block build (mode
// capability note) and log lines so the strings never drift apart.
static std::string mode_label(SessionMode mode) {
    switch (mode) {
        case SessionMode::Plan: return "plan";
        case SessionMode::Chat: return "chat";
        default:                return "build";
    }
}

std::string render_dynamic_prompt(const std::string& model,
                                  const std::string& os_info,
                                  const std::string& project_dir,
                                  int steps_left,
                                  int max_steps) {
    // Emit the budget sentence only in the final stretch: the last
    // min(10, max_steps/2) steps (floor of 1 so a tiny budget still gets a
    // final CRITICAL). Outside that window the budget is plentiful and the
    // block is pure noise — and per-step churn for prefix caches.
    // Because the threshold is capped at 10 and the "getting tight" line
    // fires at <= 14, the first appearance always lands inside that band;
    // the base sentence never appears without an escalation line.
    int threshold = std::max(1, std::min(10, max_steps / 2));
    if (steps_left > threshold) return {};

    std::string base = render_prompt(kDynamicSystemPromptNeutral, model,
                                     os_info, project_dir, steps_left);
    if (steps_left <= 4) {
        base += "\nCRITICAL: only " + std::to_string(steps_left)
              + " step(s) left. Stop exploring. Produce your final answer now, "
                "or summarize what is done and what remains.";
    } else if (steps_left <= 14) {
        base += "\nBudget is getting tight (" + std::to_string(steps_left)
              + " steps left). Wrap up the current sub-task and avoid starting "
                "new exploratory reads.";
    }
    return base;
}

// Render the current todo list as a short markdown block, appended to the
// dynamic system text in Build mode. Returns "" when the list is empty so
// callers can blindly concatenate. Capped at 20 items.
static std::string render_todos_block(const std::vector<Todo>& todos) {
    if (todos.empty()) return {};
    // A fully-completed list must carry an explicit stop signal — rendering
    // bare [x] items gives weaker models no cue to wrap up.
    bool all_complete = true;
    for (const auto& t : todos) {
        if (t.status != "completed") { all_complete = false; break; }
    }
    if (all_complete) {
        return "\n\n# Active todos\n\nAll items complete. Wrap up and report "
               "the outcome to the user — do not start unlisted work.\n";
    }
    std::string out = "\n\n# Active todos\n\n";
    const size_t cap = 20;
    size_t shown = 0;
    for (const auto& t : todos) {
        if (shown >= cap) break;
        const std::string& s = t.status;
        const char* marker = (s == "completed")   ? "[x]"
                           : (s == "in_progress") ? "[in_progress]"
                                                  : "[ ]";
        out += "- ";
        out += marker;
        out += ' ';
        out += t.content;
        out += '\n';
        ++shown;
    }
    if (todos.size() > cap) {
        out += "- …and " + std::to_string(todos.size() - cap) + " more\n";
    }
    return out;
}

// Derive a short, human-readable session title from the first user message.
// Takes the first non-empty line, collapses internal whitespace to single
// spaces, strips leading/trailing whitespace, and truncates to ~60 characters
// at a word boundary (appending an ellipsis when it truncates).
static std::string derive_heuristic_title(const std::string& text) {
    // Take the first non-empty line.
    std::string line;
    std::istringstream ss(text);
    std::string raw;
    while (std::getline(ss, raw)) {
        // Trim whitespace.
        size_t b = raw.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        size_t e = raw.find_last_not_of(" \t\r\n");
        line = raw.substr(b, e - b + 1);
        break;
    }
    if (line.empty()) return {};

    // Collapse runs of whitespace into single spaces.
    std::string collapsed;
    collapsed.reserve(line.size());
    bool in_ws = false;
    for (char c : line) {
        if (c == ' ' || c == '\t') {
            if (!in_ws && !collapsed.empty()) collapsed.push_back(' ');
            in_ws = true;
        } else {
            collapsed.push_back(c);
            in_ws = false;
        }
    }

    constexpr size_t kMax = 60;
    if (collapsed.size() <= kMax) return collapsed;

    // Truncate at the last word boundary at or before kMax.
    size_t cut = collapsed.find_last_of(' ', kMax);
    if (cut == std::string::npos || cut == 0) cut = kMax;
    return collapsed.substr(0, cut) + "…";
}

// Rough token estimate lives in compaction.cpp (estimate_request_tokens) so
// the engine, compaction, and tests share one arithmetic.

LLMRequest ContextBuilder::build(
    const std::vector<SessionMessage>& messages,
    const std::string& system_prompt,
    const std::string& system_dynamic,
    const std::vector<ToolDefinition>& tools,
    const std::string& model_id,
    const std::string& /*provider_id*/,
    bool model_accepts_images)
{
    LLMRequest req;
    req.model_id = model_id;
    req.system = system_prompt;
    req.system_dynamic = system_dynamic;
    req.tools = tools;
    req.messages = assemble_messages(messages, model_accepts_images);
    return req;
}

// Text stand-in for an image block when the primary model is text-only.
// Uses the persisted vision-fallback description when present; otherwise the
// same placeholder shape compaction renders for image attachments.
static std::string render_image_as_text(const nlohmann::json& att) {
    std::string path = att.value("path", "");
    if (att.contains("description") && att["description"].is_string()) {
        std::string d = att["description"].get<std::string>();
        if (!d.empty())
            return "[image" + (path.empty() ? "" : ": " + path)
                 + " — described by vision fallback: " + d + "]";
    }
    std::string name = path.empty() ? "unnamed" : path;
    return "[image attachment: " + name + ", "
         + att.value("media_type", "image/png") + "]";
}

// Marker row for an attachment whose file could not be read (or was empty):
// the transcript must show that something was attached rather than silently
// dropping it. Consumers render these as a short unavailable-text block.
static nlohmann::json absent_attachment_row(const Attachment& att) {
    return {
        {"kind",       att.kind.empty() ? "image" : att.kind},
        {"media_type", att.media_type},
        {"path",       att.path},
        {"absent",     true}
    };
}

// True for attachments ingested as text (code/plain files) rather than
// vision blocks. Defaults to "image" for rows persisted before text support.
static bool att_is_text(const nlohmann::json& att) {
    return att.value("kind", "image") == "text";
}

// Engine-side mirror of the GUI's 256 KB text-attachment cap, enforced at the
// submit boundary so programmatic callers can't flood context either. Same
// marker phrasing as compaction's tool-result truncation.
static constexpr size_t MAX_TEXT_ATTACHMENT_BYTES = 256 * 1024;

static std::string clamp_text_attachment(std::string raw) {
    if (raw.size() <= MAX_TEXT_ATTACHMENT_BYTES) return raw;
    size_t dropped = raw.size() - MAX_TEXT_ATTACHMENT_BYTES;
    raw.resize(MAX_TEXT_ATTACHMENT_BYTES);
    raw += "\n[truncated: " + std::to_string(dropped) + " more bytes]";
    return raw;
}

// Fenced, path-labeled text block carrying a decoded text attachment, so the
// model sees provenance as well as content.
static std::string render_text_attachment(const nlohmann::json& att) {
    std::string body = util::sanitize_utf8(
        util::base64_decode(att.value("data_b64", "")));
    std::string path = att.value("path", "");
    return "\n\nAttached file: " + (path.empty() ? "unnamed" : path)
         + "\n```\n" + body + "\n```\n";
}

std::vector<nlohmann::json> ContextBuilder::assemble_messages(
    const std::vector<SessionMessage>& msgs,
    bool model_accepts_images)
{
    // Tool results from past user turns are truncated to keep context lean.
    // "Past turn" = before the most recent user_prompted message.
    // Everything from the current user's turn (same agentic loop run) is kept
    // in full — truncating mid-turn would hide tool outputs the model just
    // produced and cause it to think its tools are failing.
    static const size_t MAX_OLD_TOOL_RESULT = 10 * 1024;

    // Find the index of the last user_prompted message.
    // Tool results before that index are from previous turns and can be trimmed.
    size_t last_user_prompt_idx = 0;
    for (size_t i = 0; i < msgs.size(); ++i) {
        if (msgs[i].type == "user_prompted")
            last_user_prompt_idx = i;
    }

    std::vector<nlohmann::json> result;

    for (size_t i = 0; i < msgs.size(); ++i) {
        auto& msg = msgs[i];
        try {
            auto data = nlohmann::json::parse(msg.data_json);

            if (msg.type == "user_prompted") {
                nlohmann::json m;
                m["role"] = "user";
                // Deferred mode-change notice: set_mode queued it and
                // submit_prompt attached it here; it goes out with this
                // request only and is never rendered in the transcript.
                std::string notice = data.value("mode_notice", "");
                // One-shot slash-command skill invocation: submit_prompt
                // resolved "/<id> args" at submit time and stored the body
                // on this row. The framed body is emitted only while this
                // row is the current turn's prompt (same past-turn rule as
                // MAX_OLD_TOOL_RESULT); later turns see a compact marker so
                // the body applies to exactly one turn. The model receives
                // the args, not the raw "/<id>" command text.
                std::string skill_prefix;
                std::string text_out = data.value("text", "");
                if (data.contains("skill")) {
                    std::string sid   = data.value("skill", "");
                    std::string sargs = data.value("skill_args", "");
                    if (i == last_user_prompt_idx) {
                        if (data.contains("skill_block")) {
                            skill_prefix = "[skill invoked: /" + sid
                                + " — apply the following skill instructions "
                                  "to this message; they override your "
                                  "defaults for this turn only]\n"
                                + data.value("skill_block", "")
                                + "\n[end of skill /" + sid
                                + " instructions — they do not apply to "
                                  "later turns]";
                        } else if (data.value("skill_active", false)) {
                            skill_prefix = "[skill '/" + sid
                                + "' invoked for this message; it is already "
                                  "active this session — apply it now with "
                                  "priority]";
                        } else {
                            // Matched at submit but the body could not be
                            // resolved (file vanished / unreadable).
                            skill_prefix = "[skill '/" + sid
                                + "' invoked but its file could not be read]";
                        }
                    } else {
                        skill_prefix = "[skill '/" + sid
                            + "' was invoked one-shot for this turn; its "
                              "instructions no longer apply]";
                    }
                    text_out = sargs;
                }
                if (data.contains("attachments") && data["attachments"].is_array()
                        && !data["attachments"].empty()) {
                    // Mixed text + image content — Anthropic block style.
                    // OpenAI's translate_messages maps image blocks to
                    // image_url entries on the fly.
                    nlohmann::json content = nlohmann::json::array();
                    if (!notice.empty())
                        content.push_back({{"type", "text"}, {"text", notice}});
                    if (!skill_prefix.empty())
                        content.push_back({{"type", "text"},
                                           {"text", skill_prefix}});
                    if (!text_out.empty())
                        content.push_back({{"type", "text"}, {"text", text_out}});
                    for (const auto& att : data["attachments"]) {
                        if (att.value("absent", false)) {
                            content.push_back({{"type", "text"},
                                {"text", "[attachment unavailable: "
                                      + att.value("path", "") + "]"}});
                        } else if (att_is_text(att)) {
                            content.push_back({{"type", "text"},
                                {"text", render_text_attachment(att)}});
                        } else if (model_accepts_images) {
                            content.push_back({
                                {"type", "image"},
                                {"source", {
                                    {"type", "base64"},
                                    {"media_type", att.value("media_type", "image/png")},
                                    {"data", att.value("data_b64", "")}
                                }}
                            });
                        } else {
                            content.push_back({{"type", "text"},
                                               {"text", render_image_as_text(att)}});
                        }
                    }
                    m["content"] = content;
                } else {
                    std::string combined = notice;
                    if (!skill_prefix.empty()) {
                        if (!combined.empty()) combined += "\n";
                        combined += skill_prefix;
                    }
                    if (!text_out.empty()) {
                        if (!combined.empty()) combined += "\n";
                        combined += text_out;
                    }
                    m["content"] = combined;
                }
                result.push_back(m);
            } else if (msg.type == "assistant_text") {
                nlohmann::json m;
                m["role"] = "assistant";
                std::string text = data.value("text", "");
                bool has_tools = data.contains("tool_calls")
                              && data["tool_calls"].is_array()
                              && !data["tool_calls"].empty();
                if (has_tools) {
                    // Build Anthropic-style content array: text + tool_use
                    // blocks. OpenAI's translate_messages() converts on the fly.
                    nlohmann::json content = nlohmann::json::array();
                    if (!text.empty())
                        content.push_back({{"type","text"},{"text",text}});
                    for (auto& tc : data["tool_calls"]) {
                        nlohmann::json block;
                        block["type"]  = "tool_use";
                        block["id"]    = tc.value("id", "");
                        block["name"]  = tc.value("name", "");
                        block["input"] = tc.contains("input") ? tc["input"]
                                                               : nlohmann::json::object();
                        content.push_back(block);
                    }
                    m["content"] = content;
                } else {
                    m["content"] = text;
                }
                result.push_back(m);
            } else if (msg.type == "compaction_summary") {
                // A prior compaction's summary. Emit it as an assistant turn
                // (with a fixed lead-in) so the tail's first message — which
                // by construction is a user turn — produces a legal
                // user/assistant alternation. Emitting as `user` would put
                // two consecutive user messages on the wire, which Anthropic
                // rejects with "conversation roles must alternate".
                nlohmann::json m;
                m["role"] = "assistant";
                m["content"] = std::string(
                        "[compacted earlier turns — the context window was approaching "
                        "its limit, so the system summarized everything before this point. "
                        "Treat the following as the canonical record of what happened "
                        "earlier; the raw messages are no longer in context.]\n\n"
                        "Summary of the prior conversation:\n")
                             + data.value("text", "");
                result.push_back(m);
                // Single-turn fallback (Fix 2) can put the summary right
                // before an assistant_text turn, which would still violate
                // alternation. Look ahead at the next *renderable* message;
                // if it would emit as assistant, insert a stub user turn.
                for (size_t j = i + 1; j < msgs.size(); ++j) {
                    const auto& nx = msgs[j];
                    if (nx.type == "assistant_text") {
                        nlohmann::json stub;
                        stub["role"] = "user";
                        stub["content"] = "Continue from where the summary leaves off.";
                        result.push_back(stub);
                        break;
                    }
                    if (nx.type == "user_prompted" || nx.type == "tool_result"
                        || nx.type == "compaction_summary") {
                        break;  // already a user-emitting turn — no stub needed
                    }
                    // unknown type — keep looking
                }
            } else if (msg.type == "tool_result") {
                nlohmann::json m;
                m["role"] = "user";
                nlohmann::json content;
                content["type"] = "tool_result";
                content["tool_use_id"] = data.value("call_id", "");
                std::string output = data.value("output", "");
                // Truncate outputs from previous user turns. The model already
                // acted on them; keeping them full inflates context on every step.
                // Tool results from the current turn are never truncated so the
                // model can see every tool it called within this agentic run.
                if (i < last_user_prompt_idx
                        && output.size() > MAX_OLD_TOOL_RESULT) {
                    size_t dropped = output.size() - MAX_OLD_TOOL_RESULT;
                    output.resize(MAX_OLD_TOOL_RESULT);
                    output += "\n[truncated: " + std::to_string(dropped)
                            + " more bytes]";
                }
                // Images returned by tools (e.g. screenshot) are persisted in
                // an "attachments" array. Anthropic tool_result content
                // accepts mixed text + image blocks; OpenAI's
                // translate_messages splits the image blocks out on the fly.
                bool has_images = data.contains("attachments")
                               && data["attachments"].is_array()
                               && !data["attachments"].empty();
                if (has_images) {
                    nlohmann::json blocks = nlohmann::json::array();
                    if (!output.empty())
                        blocks.push_back({{"type", "text"}, {"text", output}});
                    for (const auto& att : data["attachments"]) {
                        if (model_accepts_images) {
                            blocks.push_back({
                                {"type", "image"},
                                {"source", {
                                    {"type", "base64"},
                                    {"media_type", att.value("media_type", "image/png")},
                                    {"data", att.value("data_b64", "")}
                                }}
                            });
                        } else {
                            blocks.push_back({{"type", "text"},
                                              {"text", render_image_as_text(att)}});
                        }
                    }
                    content["content"] = blocks;
                } else {
                    content["content"] = output;
                }
                m["content"] = nlohmann::json::array({content});
                result.push_back(m);
            }
        } catch (...) {}
    }

    return result;
}

SessionEngine::SessionEngine(SessionStore& store,
                              ProviderRegistry& providers,
                              ToolRegistry& tools,
                              PermissionGate& permissions,
                              SessionEventBus& bus,
                              const AppConfig& config)
    : store_(store)
    , providers_(providers)
    , tools_(tools)
    , permissions_(permissions)
    , bus_(bus)
    , config_(config)
{}

SessionEngine::~SessionEngine() {
    // 1. Snapshot everything the join needs, under mu_, and flip
    // shutting_down_ so submit_prompt/continue_session/compact_now refuse
    // to spawn new runners from this point on.
    std::vector<std::shared_ptr<Provider>> providers;
    std::vector<std::thread> threads;
    std::vector<std::atomic<bool>*> flags;
    {
        std::lock_guard<std::mutex> lock(mu_);
        shutting_down_ = true;
        for (auto& [id, flag] : interrupt_flags_)
            if (flag) flag->store(true);
        for (auto& [id, provider] : session_providers_)
            providers.push_back(provider);
        for (auto& [id, t] : runner_threads_)
            if (t.joinable()) threads.push_back(std::move(t));
        for (auto& [id, flag] : interrupt_flags_)
            flags.push_back(flag);
        // The moved-from thread handles are gone; drop the map entries so
        // nothing can join them twice.
        runner_threads_.clear();
    }

    // 2. Release the ask waits and cancel in-flight HTTP requests (no locks
    // held — cancel can block on network teardown).
    cancel_pending_asks();
    for (auto& provider : providers)
        provider->cancel();

    // 3. Join the snapshotted runners. A worker blocked on a permission
    // future cannot be waited out here; callers must not destroy a running
    // engine while a permission dialog is pending (the GUI routes through
    // _RecreateEngine(); the app-quit path calls exit() before destruction
    // ever runs).
    for (auto& t : threads)
        t.join();

    // 4. Free per-session interrupt flags (workers are gone by now).
    for (auto* flag : flags)
        delete flag;
}

void SessionEngine::cancel_pending_asks() {
    cancel_pending_asks("");
}

void SessionEngine::cancel_pending_asks(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(ask_mu_);
    for (auto& [call_id, pa] : pending_ask_) {
        if (!session_id.empty() && pa.session_id != session_id) continue;
        if (!pa.replied) {
            pa.answer = "(interrupted)";
            pa.replied = true;
        }
    }
    asking_cv_.notify_all();
}

std::string SessionEngine::create_session(const std::string& project_dir,
                                           const std::string& agent_id,
                                           const std::string& model_id,
                                           const std::string& provider_id) {
    std::string eff_model = model_id.empty() ? config_.model : model_id;
    std::string eff_agent = agent_id.empty() ? config_.agent : agent_id;

    // Determine provider: use explicit arg, then fall back to registered providers
    std::string eff_provider = provider_id;
    if (eff_provider.empty()) {
        if (providers_.get("anthropic"))      eff_provider = "anthropic";
        else if (providers_.get("openai"))    eff_provider = "openai";
        else                                   eff_provider = "anthropic";
    }

    nlohmann::json model_json;
    model_json["id"]          = eff_model;
    model_json["provider_id"] = eff_provider;
    model_json["mode"]        = config_.default_mode;
    model_json["skills"]      = config_.default_skills;

    auto session = store_.create(project_dir, eff_agent, model_json.dump());
    return session.id;
}

void SessionEngine::submit_prompt(const std::string& session_id,
                                   const std::string& text) {
    submit_prompt(session_id, text, {});
}

void SessionEngine::submit_prompt(const std::string& session_id,
                                   const std::string& text,
                                   const std::vector<Attachment>& attachments) {
    // Pop any queued mode-change notice. It rides as metadata on this row
    // (ContextBuilder prepends it to the outgoing text) instead of being
    // persisted as its own message. First message of a session: dropped —
    // the system prompt's mode block already states the mode.
    std::string mode_notice;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = pending_mode_notice_.find(session_id);
        if (it != pending_mode_notice_.end()) {
            if (!store_.load_messages(session_id).empty())
                mode_notice = it->second;
            pending_mode_notice_.erase(session_id);
        }
    }

    // Slash-command skill invocation ("/caveman fix the commit"): one-shot.
    // The skill body is resolved now (submit time) so the row is immutable
    // even if the file changes mid-session, and rides this row as metadata
    // exactly like mode_notice. The transcript keeps the raw text. When the
    // skill is already enabled for the session (model_json["skills"]) the
    // system prompt carries it — record that instead of duplicating the
    // body.
    std::string skill_id, skill_args, skill_block;
    bool skill_active = false;
    {
        auto sess = store_.get(session_id);
        if (sess) {
            SkillInfo sk;
            std::string args;
            if (parse_skill_invocation(sess->directory, text, sk, args)) {
                skill_id = sk.id;
                skill_args = args;
                auto mj = nlohmann::json::parse(sess->model_json, nullptr,
                                                false);
                if (mj.is_object() && mj.contains("skills")
                        && mj["skills"].is_array()) {
                    for (auto& s : mj["skills"]) {
                        if (s.is_string() && s.get<std::string>() == sk.id) {
                            skill_active = true;
                            break;
                        }
                    }
                }
                if (!skill_active)
                    skill_block = build_skills_block(sess->directory, {sk.id},
                                                     mode_label(get_mode(session_id)));
            }
        }
    }

    // Persist the user message
    nlohmann::json data;
    data["role"] = "user";
    data["text"] = text;
    if (!mode_notice.empty())
        data["mode_notice"] = mode_notice;
    if (!skill_id.empty()) {
        data["skill"] = skill_id;
        data["skill_args"] = skill_args;
        if (skill_active)
            data["skill_active"] = true;
        else if (!skill_block.empty())
            data["skill_block"] = skill_block;
    }
    if (!attachments.empty()) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& att : attachments) {
            // Text payloads are clamped to MAX_TEXT_ATTACHMENT_BYTES no
            // matter which side of the boundary supplied them, so a
            // programmatic caller can't flood context either.
            const bool is_text = att.kind == "text";
            std::string b64 = att.data_b64;
            if (!b64.empty() && is_text) {
                b64 = util::base64_encode(
                    clamp_text_attachment(util::base64_decode(b64)));
            }
            if (b64.empty() && !att.path.empty()) {
                std::ifstream f(att.path, std::ios::binary);
                if (!f) {
                    fprintf(stderr, "haicode: attachment unreadable, marking absent: %s\n",
                            att.path.c_str());
                    arr.push_back(absent_attachment_row(att));
                    continue;
                }
                std::ostringstream ss;
                ss << f.rdbuf();
                std::string raw = ss.str();
                if (is_text) raw = clamp_text_attachment(raw);
                b64 = util::base64_encode(raw);
            }
            if (b64.empty()) {
                // 0-byte file (or neither payload nor path): keep a marker
                // so the transcript shows something was attached instead of
                // silently dropping it.
                arr.push_back(absent_attachment_row(att));
                continue;
            }
            arr.push_back({
                {"kind",       att.kind},
                {"media_type", att.media_type},
                {"path",       att.path},
                {"data_b64",   b64}
            });
        }
        if (!arr.empty())
            data["attachments"] = arr;
    }
    store_.append_message(session_id, "user_prompted", data.dump());

    // Immediate heuristic autonaming: if the session still has no title, derive
    // one from this first user message so the sidebar is descriptive without
    // waiting for an LLM round-trip.
    if (config_.autoname_sessions) {
        auto sess = store_.get(session_id);
        if (sess && sess->title.empty()) {
            std::string title = derive_heuristic_title(text);
            if (!title.empty()) {
                store_.update_title(session_id, title);
                nlohmann::json rev;
                rev["session_id"] = session_id;
                rev["title"] = title;
                bus_.publish(events::EventType::SessionRenamed, rev);
            }
        }
    }

    // A fresh user turn rearms the compaction hysteresis so the first
    // step of this turn is allowed to compact again if needed.
    {
        std::lock_guard<std::mutex> lock(mu_);
        last_compaction_step_[session_id] = -1;
    }

    // Publish event
    nlohmann::json ev;
    ev["session_id"] = session_id;
    ev["text"] = text;
    if (data.contains("attachments")) {
        nlohmann::json names = nlohmann::json::array();
        for (const auto& a : data["attachments"]) {
            std::string p = a.value("path", "");
            size_t slash = p.find_last_of('/');
            names.push_back(slash == std::string::npos ? p : p.substr(slash + 1));
        }
        ev["attachments"] = names;
    }
    bus_.publish(events::EventType::Prompted, ev);

    // Start runner thread if not already running for this session. Refuse
    // to spawn once the destructor has begun: it snapshotted the thread set
    // under mu_, so a new runner would outlive the engine. The persisted
    // prompt stays in the DB for the next engine instance.
    std::lock_guard<std::mutex> lock(mu_);
    if (shutting_down_) return;
    bool running = session_running_.count(session_id) && session_running_[session_id];
    if (!running) {
        // Join the previous thread (safe — it has already exited since running==false)
        auto th_it = runner_threads_.find(session_id);
        if (th_it != runner_threads_.end() && th_it->second.joinable())
            th_it->second.join();

        if (interrupt_flags_.count(session_id))
            delete interrupt_flags_[session_id];
        interrupt_flags_[session_id] = new std::atomic<bool>(false);
        session_running_[session_id] = true;

        runner_threads_[session_id] = std::thread([this, session_id]() {
            agentic_loop(session_id);
            std::lock_guard<std::mutex> g(mu_);
            session_running_[session_id] = false;
        });
    }
}

void SessionEngine::inject_message(const std::string& session_id,
                                   const std::string& text) {
    // An explicit injection (plan approval) supersedes any queued mode
    // notice: its own text already tells the model the mode changed.
    {
        std::lock_guard<std::mutex> lock(mu_);
        pending_mode_notice_.erase(session_id);
    }
    nlohmann::json data;
    data["role"] = "user";
    data["text"] = text;
    store_.append_message(session_id, "user_prompted", data.dump());

    nlohmann::json ev;
    ev["session_id"] = session_id;
    ev["text"] = text;
    bus_.publish(events::EventType::Prompted, ev);
}

void SessionEngine::continue_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    // No new runners during destruction (see submit_prompt).
    if (shutting_down_) return;
    bool running = session_running_.count(session_id) && session_running_[session_id];
    if (!running) {
        auto th_it = runner_threads_.find(session_id);
        if (th_it != runner_threads_.end() && th_it->second.joinable())
            th_it->second.join();
        if (interrupt_flags_.count(session_id))
            delete interrupt_flags_[session_id];
        interrupt_flags_[session_id] = new std::atomic<bool>(false);
        session_running_[session_id] = true;
        runner_threads_[session_id] = std::thread([this, session_id]() {
            agentic_loop(session_id);
            std::lock_guard<std::mutex> g(mu_);
            session_running_[session_id] = false;
        });
    }
}

void SessionEngine::interrupt(const std::string& session_id) {
    // 1. Set interrupt flag and get provider pointer (under lock).
    Provider* active_provider = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);

        auto it = interrupt_flags_.find(session_id);
        if (it != interrupt_flags_.end() && it->second)
            it->second->store(true);

        // Get the active provider so we can cancel its HTTP request.
        auto prov_it = session_providers_.find(session_id);
        if (prov_it != session_providers_.end()) {
            active_provider = prov_it->second.get();
        }
    }

    // 2. Cancel in-flight HTTP request — this sets cancelled_ on both the
    // provider and HttpClient, causing libcurl to abort the transfer
    // immediately so the agentic loop can break out.
    // We do NOT join the runner thread here: interrupt() is called from the
    // UI thread, and the runner may be blocked on a permission future (which
    // would deadlock the join). The thread will be joined lazily on the next
    // submit_prompt()/continue_session() call, and the Interrupted event
    // below gives the UI immediate feedback.
    if (active_provider) {
        active_provider->cancel();
    }

    // 3. Release any ask_user wait for THIS session: mark it replied with
    // "(interrupted)" and wake asking_cv_. The worker overwrites the
    // placeholder row and the post-wait interrupt_flag check breaks the
    // loop — without this, a session parked on a question would hang until
    // engine destruction. Scoped so stopping one session never answers
    // another session's open question.
    cancel_pending_asks(session_id);

    // 4. Publish Interrupted event so UIs know the interrupt was processed.
    nlohmann::json ev;
    ev["session_id"] = session_id;
    bus_.publish(events::EventType::Interrupted, ev);
}

bool SessionEngine::is_running(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = session_running_.find(session_id);
    return it != session_running_.end() && it->second;
}

void SessionEngine::set_mode(const std::string& session_id, SessionMode mode) {
    // Resolve the previous mode before either copy is overwritten (get_mode
    // reads the in-memory cache, else the DB value update_mode is about to
    // replace).
    SessionMode prev = get_mode(session_id);
    {
        std::lock_guard<std::mutex> lock(mu_);
        session_modes_[session_id] = mode;
        if (prev != mode) {
            pending_mode_notice_[session_id] =
                mode == SessionMode::Plan ? kSwitchedToPlanMessage
              : mode == SessionMode::Chat ? kSwitchedToChatMessage
                                          : kSwitchedToBuildMessage;
        }
    }
    store_.update_mode(session_id, mode == SessionMode::Plan ? "plan"
                                                       : mode == SessionMode::Chat ? "chat"
                                                                                   : "build");
}

void SessionEngine::update_provider_model(const std::string& session_id,
                                           const std::string& provider_id,
                                           const std::string& model_id) {
    store_.update_provider_model(session_id, provider_id, model_id);
}

void SessionEngine::update_inference(const std::string& session_id,
                                     const InferenceParams& params) {
    store_.update_inference(session_id, params);
}

std::vector<Todo> SessionEngine::get_todos(const std::string& session_id) {
    return store_.load_todos(session_id);
}

SessionMode SessionEngine::get_mode(const std::string& session_id) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = session_modes_.find(session_id);
        if (it != session_modes_.end()) return it->second;
    }
    // Not in memory (no prompt submitted yet this run): read from DB.
    if (auto si = store_.get(session_id)) {
        try {
            auto mj = nlohmann::json::parse(si->model_json, nullptr, false);
            std::string ms = mj.value("mode", config_.default_mode);
            if (ms == "plan") return SessionMode::Plan;
            if (ms == "chat") return SessionMode::Chat;
            return SessionMode::Build;
        } catch (...) {}
    }
    if (config_.default_mode == "plan") return SessionMode::Plan;
    if (config_.default_mode == "chat") return SessionMode::Chat;
    return SessionMode::Build;
}

void SessionEngine::agentic_loop(const std::string& session_id) {
    // No global lock anymore: concurrent sessions run their loops in
    // parallel. Lifetime is guaranteed by ~SessionEngine()'s snapshot-join —
    // it flips shutting_down_ (closing the spawn paths), sets interrupt
    // flags, then joins exactly the threads it snapshotted under mu_ before
    // any engine state is freed. See engine.h.
    auto session_opt = store_.get(session_id);
    if (!session_opt) return;

    auto session = *session_opt;
    auto model_json = nlohmann::json::parse(session.model_json, nullptr, false);
    std::string model_id = model_json.value("id", config_.model);
    std::string provider_id = model_json.value("provider_id", "anthropic");

    // Resolve session mode. The in-memory cache (set_mode) wins; otherwise we
    // fall back to the value persisted in model_json (covers a fresh process
    // start with no toggling yet this run).
    SessionMode mode;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = session_modes_.find(session_id);
        if (it != session_modes_.end()) {
            mode = it->second;
        } else {
            std::string mode_str = model_json.value("mode", config_.default_mode);
            mode = (mode_str == "plan") ? SessionMode::Plan
                 : (mode_str == "chat") ? SessionMode::Chat
                                        : SessionMode::Build;
            session_modes_[session_id] = mode;
        }
    }

    auto provider = providers_.get(provider_id);
    if (!provider) {
        nlohmann::json ev;
        ev["session_id"] = session_id;
        ev["error"] = "No provider available for: " + provider_id;
        bus_.publish(events::EventType::StepFailed, ev);
        return;
    }
    // The registry key the provider object above was fetched under. The
    // per-step re-read below compares against this (not just provider_id)
    // so a mid-loop switch re-fetches the object and refreshes the cancel
    // map exactly once.
    std::string fetched_provider_id = provider_id;

    // Store active provider so interrupt() can cancel in-flight HTTP requests.
    {
        std::lock_guard<std::mutex> lock(mu_);
        session_providers_[session_id] = provider;
    }

    auto* interrupt_flag = interrupt_flags_[session_id];
    std::string prompt_tmpl = kDefaultSystemPrompt;
    constexpr int DEFAULT_MAX_STEPS = 50;
    int max_steps = DEFAULT_MAX_STEPS;
    auto ait = config_.agents.find(session.agent);
    if (ait != config_.agents.end()) {
        if (ait->second.system_prompt)
            prompt_tmpl = *ait->second.system_prompt;
        if (ait->second.max_steps && *ait->second.max_steps > 0)
            max_steps = *ait->second.max_steps;
    }
    // Per-session override (from the Inference tab) wins over agent/config.
    if (model_json.contains("max_steps") && model_json["max_steps"].is_number()) {
        int sess_ms = model_json.value("max_steps", 0);
        if (sess_ms > 0) max_steps = sess_ms;
    }
    // {{STEPS_LEFT}} in the stable template is re-substituted every step, so
    // the "byte-stable" body varies and defeats Anthropic prefix caching.
    // The dynamic block already shows the live count; warn, don't fail.
    if (prompt_tmpl.find("{{STEPS_LEFT}}") != std::string::npos) {
        fprintf(stderr, "[engine] warning: agent '%s' system_prompt contains "
                        "{{STEPS_LEFT}}; it will change every step and break "
                        "Anthropic prefix caching. Remove it — the dynamic "
                        "sentence already renders the count in the final "
                        "stretch of the session.\n",
                session.agent.c_str());
        fflush(stderr);
    }

    std::string os_info;
    struct utsname uts {};
    if (uname(&uts) == 0)
        os_info = std::string(uts.sysname) + " " + uts.release
              + " (" + uts.machine + ")";

    // Build the static instruction block once. Appended to the system prompt
    // every step so it survives the {{STEPS_LEFT}} re-render.
    std::string instructions_block;
    if (!config_.instructions.empty()) {
        instructions_block = "\n\n# Additional instructions\n\n";
        for (auto& s : config_.instructions) {
            if (s.empty()) continue;
            instructions_block += "- ";
            instructions_block += s;
            instructions_block += "\n";
        }
    }

    // Build the agents.md block once. Project-only; read by ConfigLoader.
    // Placed before instructions_block so per-config instructions can add to
    // or override what the project file says.
    std::string agents_md_block;
    if (!config_.agents_md.empty()) {
        agents_md_block = "\n\n# Project instructions (agents.md)\n\n"
                        + config_.agents_md;
    } else {
        agents_md_block = "\n\n# Project instructions\n\n"
            "No agents.md found for this project. If the user asks you to set "
            "up project instructions, or if you think a persistent record of "
            "build commands and conventions would help, use the "
            "`write_agents_md` tool to create one. It will be appended to "
            "your system prompt for every future session in this project.";
    }

    // Skills block: markdown files enabled for this session (persisted in
    // model_json["skills"]). Rebuilt each step so mid-session toggles in the
    // UI take effect on the next step. Part of the stable prompt body, like
    // agents.md — skill changes are rare, so prefix-cache impact matches.
    // The block carries a mode capability note, so it is also rebuilt when
    // the mode flips mid-loop (block_mode caches what the current block
    // was built with).
    std::vector<std::string> enabled_skills;
    if (model_json.contains("skills") && model_json["skills"].is_array()) {
        for (auto& s : model_json["skills"])
            if (s.is_string()) enabled_skills.push_back(s.get<std::string>());
    }
    std::string block_mode = mode_label(mode);
    std::string skills_block = build_skills_block(session.directory,
                                                  enabled_skills,
                                                  block_mode);

    // Inject the most recent plan file so the agent has it in context even
    // when starting a fresh session after planning was done in a prior one.
    std::string latest_plan_block;
    {
        std::string plan_content = load_latest_plan(session.directory);
        if (!plan_content.empty()) {
            latest_plan_block = "\n\n# Most recent plan (.haicode/plans/)\n\n"
                              + plan_content;
        }
    }

    // Plan-mode block: appended only when the session is in Plan mode. Tells
    // the model it must research and propose_plan rather than modify files.
    // The engine separately filters out state-modifying tools when this block
    // is active, so the model literally cannot call them.
    std::string plan_mode_block;
    if (mode == SessionMode::Plan) {
        plan_mode_block = kPlanModeInstructions;
    }

    // Chat-mode block: appended only when the session is in Chat mode.
    // Conversation + web research only; the engine separately strips every
    // tool except the chat allowlist, so there is zero local computer access.
    std::string chat_mode_block;
    if (mode == SessionMode::Chat) {
        chat_mode_block = kChatModeInstructions;
    }

    std::string system = render_prompt(prompt_tmpl, model_id, os_info, session.directory, max_steps)
                       + agents_md_block
                       + skills_block
                       + latest_plan_block
                       + instructions_block
                       + plan_mode_block
                       + chat_mode_block;

    // Dynamic per-step content ({{STEPS_LEFT}}). Emitted as a separate
    // system text block by the Anthropic provider so the stable body
    // above stays byte-identical across turns and hits the prefix cache.
    std::string system_dynamic = render_dynamic_prompt(model_id, os_info,
                                                       session.directory,
                                                       max_steps, max_steps);

    fprintf(stderr, "[engine] session=%s dir='%s' agent=%s mode=%s max_steps=%d (renewable, ceiling=%d) instructions=%zu\n",
            session_id.c_str(), session.directory.c_str(), session.agent.c_str(),
            mode_label(mode).c_str(),
            max_steps, kStepCeilingMultiplier * max_steps, config_.instructions.size());
    // The full prompt embeds project agents.md content; dump it only when
    // explicitly debugging prompt assembly.
    if (std::getenv("HPCODE_DEBUG_PROMPT") && *std::getenv("HPCODE_DEBUG_PROMPT")) {
        fprintf(stderr, "[engine] system prompt:\n%s\n---\n", system.c_str());
    }
    fflush(stderr);

    // Agentic loop. The step budget is renewable: each todo completion above
    // the turn's high-water mark refills steps_left to max_steps, so a large
    // multi-file turn doesn't die mid-work while the model is still
    // performing normally. Two guards keep renewal loop-proof: only NEW
    // completions renew (completed_high_water), and ceiling_left caps the
    // turn at kStepCeilingMultiplier * max_steps iterations no matter what.
    // iter is the monotonic counter the compaction hysteresis keys off — it
    // must never go backwards, or step - last_compaction_step would go
    // negative and silently suppress auto-compaction for the rest of the
    // turn.
    int iter = 0;
    int steps_left = max_steps;
    int ceiling_left = kStepCeilingMultiplier * max_steps;
    // Completed todos at loop start: items finished in earlier turns must
    // not be "re-completed" for a renewal.
    int completed_high_water = 0;
    for (const auto& td : store_.load_todos(session_id))
        if (td.status == "completed") ++completed_high_water;
    // Input-token count reported by the provider on the previous step. The true
    // prompt size is input + cache_read + cache_write (cache reads/writes still
    // occupy the context window). Used to decide whether to compact.
    int prev_total_input = 0;
    while (steps_left > 0 && ceiling_left > 0) {
        if (interrupt_flag && interrupt_flag->load()) break;

        // Re-read mode each step, before the skills re-read below (the
        // skills-block rebuild depends on it). The user can toggle Plan/Build
        // mid-loop (the GUI's _ToggleMode calls set_mode, which updates the
        // in-memory cache + DB synchronously); the tool allowlist and the
        // plan-mode system block below must reflect the flip on the next step,
        // not on the next turn.
        mode = get_mode(session_id);

        // Re-read model_id/provider_id (and inference params) from the session
        // each step. The user can change either via the dropdown mid-loop, and
        // the system prompt (re-rendered below) plus the outgoing request must
        // reflect the new values on the very next step.
        nlohmann::json mj_now;
        bool skills_changed = false;
        if (auto s_now = store_.get(session_id)) {
            mj_now = nlohmann::json::parse(s_now->model_json, nullptr, false);
            if (mj_now.is_object()) {
                if (auto v = mj_now.value("id", ""); !v.empty())        model_id    = v;
                if (auto v = mj_now.value("provider_id", ""); !v.empty()) provider_id = v;
                // Mid-loop provider switch: the strings above are not enough —
                // the provider OBJECT (connection, auth, base_url) and the
                // interrupt cancel map must follow. Re-fetch on change;
                // overwriting the same session_providers_ key drops the stale
                // entry. Unknown id: keep the old object and warn (review #7).
                if (provider_id != fetched_provider_id) {
                    auto next = providers_.get(provider_id);
                    if (next) {
                        provider = next;
                        fetched_provider_id = provider_id;
                        std::lock_guard<std::mutex> lock(mu_);
                        session_providers_[session_id] = provider;
                        fprintf(stderr, "[engine] session=%s switched provider "
                                        "mid-loop -> %s\n",
                                session_id.c_str(), provider_id.c_str());
                    } else {
                        fprintf(stderr, "[engine] session=%s provider switch to "
                                        "unknown id '%s'; keeping %s\n",
                                session_id.c_str(), provider_id.c_str(),
                                fetched_provider_id.c_str());
                        provider_id = fetched_provider_id;
                    }
                }
                // Skills can be toggled mid-session from the UI; rebuild the
                // block so the change lands on the next step.
                std::vector<std::string> skills_now;
                if (mj_now.contains("skills") && mj_now["skills"].is_array()) {
                    for (auto& s : mj_now["skills"])
                        if (s.is_string())
                            skills_now.push_back(s.get<std::string>());
                }
                if (skills_now != enabled_skills) {
                    enabled_skills = std::move(skills_now);
                    skills_changed = true;
                }
            }
        }

        plan_mode_block = (mode == SessionMode::Plan) ? kPlanModeInstructions
                                                       : std::string{};
        chat_mode_block = (mode == SessionMode::Chat) ? kChatModeInstructions
                                                       : std::string{};

        // The skills block carries a mode capability note: rebuild it when
        // either the enabled set or the mode flipped this step.
        if (skills_changed || mode_label(mode) != block_mode) {
            block_mode = mode_label(mode);
            skills_block = build_skills_block(session.directory,
                                              enabled_skills, block_mode);
        }

        // Re-render the system prompt each step so {{MODEL}} and {{STEPS_LEFT}}
        // stay current. steps_left is renewable: a todo completion mid-turn
        // refills it to max_steps, so it can move up as well as down.
        system = render_prompt(prompt_tmpl, model_id, os_info, session.directory,
                               steps_left) + agents_md_block + skills_block
                              + latest_plan_block
                              + instructions_block + plan_mode_block + chat_mode_block;
        // steps_left changes each step (and resets on renewal) → re-render the
        // dynamic block too.
        system_dynamic = render_dynamic_prompt(model_id, os_info,
                                               session.directory,
                                               steps_left, max_steps);

        // Re-inject the current todo list (Build and Chat modes) so the model
        // stays anchored to outstanding work. Chat allows todo_write, so it
        // must also see the list. Lives in the dynamic block to preserve the
        // stable body's prefix cache.
        if (mode != SessionMode::Plan) {
            auto todos_now = store_.load_todos(session_id);
            system_dynamic += render_todos_block(todos_now);
        }

        auto messages = load_context_messages(session_id);

        // Vision fallback: when the primary is text-only and a fallback is
        // configured, describe any not-yet-described attachments (once,
        // persisted) so assembly below emits text instead of image bytes.
        bool primary_supports_vision =
            model_supports_vision(model_id, config_.model_vision);
        if (!primary_supports_vision)
            backfill_attachment_descriptions(session_id, messages);

        ContextBuilder builder;
        auto tool_defs = tools_.definitions();
        // Filter tools by mode via the shared allowlist (tool_allowed_in_mode,
        // also enforced at execution time in ToolRegistry::execute_impl, so
        // the wire list and the execution check can never diverge). Plan and
        // Chat are fail-closed: anything not explicitly safe for that mode is
        // hidden, so future tools don't silently leak into restricted turns.
        // Build mode has no filter. (Chat = conversation + web research only;
        // todo_write touches only app-internal session state and ask_user
        // only round-trips to the UI.)
        std::erase_if(tool_defs, [&](const ToolDefinition& td) {
            return !tool_allowed_in_mode(td.name, mode);
        });
        // Vision gate: the screenshot tool returns an image the model must be
        // able to see. Hide it from text-only models unless a vision fallback
        // is configured (the returned PNG gets described by the backfill pass
        // above). model_id was re-read from the session above, so a mid-loop
        // model switch re-evaluates on the next step.
        std::erase_if(tool_defs, [&](const ToolDefinition& td) {
            return td.name == "screenshot"
                && !primary_supports_vision && !vision_fallback_ready();
        });
        auto req = builder.build(messages, system, system_dynamic, tool_defs,
                                  model_id, provider_id, primary_supports_vision);

        // Auto-compaction: if the context is approaching the model's window,
        // summarize the older portion of the conversation before sending the
        // request. Disabled when the window is unknown (0) or auto_compact is off.
        //
        // Token count source: prefer the provider's reported usage from the
        // previous step (prev_total_input) since it's exact. It is a local
        // reset to 0 at the start of every turn, so step 0 of each turn falls
        // back to the chars/4 estimate (system + tools + messages); from
        // step 1 onward the real usage takes over.
        if (config_.auto_compact) {
            int window = haicode::get_context_window(provider_id, model_id,
                                                     config_.model_contexts,
                                                     provider.get());
            if (window > 0) {
                int threshold = usable_input_tokens(window,
                                                    req.max_tokens.value_or(kDefaultMaxTokens),
                                                    config_.compaction_buffer,
                                                    config_.auto_compact_threshold);
                int current_tokens = prev_total_input;
                if (current_tokens == 0) {
                    current_tokens = estimate_request_tokens(system, system_dynamic,
                                                             req.messages,
                                                             tool_defs);
                }
                int lcs;
                bool already_compacting;
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    auto it = last_compaction_step_.find(session_id);
                    lcs = (it != last_compaction_step_.end()) ? it->second : -1;
                    already_compacting =
                        compaction_in_progress_.count(session_id) > 0;
                }
                if (!already_compacting
                        && should_compact_with_hysteresis(current_tokens, threshold,
                                                          iter, lcs)) {
                    if (compact_history(session_id, *provider, model_id,
                                        provider_id, interrupt_flag,
                                        current_tokens, threshold)) {
                        {
                            std::lock_guard<std::mutex> lock(mu_);
                            last_compaction_step_[session_id] = iter;
                        }
                        messages = load_context_messages(session_id);
                        req = builder.build(messages, system, system_dynamic,
                                            tool_defs, model_id, provider_id);
                    }
                }
            }
        }

        // Apply per-session inference params (max_tokens / temperature / top_p /
        // reasoning_effort) stored in model_json. Provider defaults win when
        // not present.
        if (mj_now.is_object()) {
            if (int v = mj_now.value("max_tokens", 0); v > 0)
                req.max_tokens = v;
            if (mj_now.contains("temperature"))
                req.temperature = mj_now.value("temperature", 0.0);
            if (mj_now.contains("top_p"))
                req.top_p = mj_now.value("top_p", 0.0);
            if (mj_now.contains("reasoning_effort"))
                req.reasoning_effort = mj_now.value("reasoning_effort", "");
        }

        std::string assistant_msg_id = haicode::util::make_id("amsg");

        // Publish step started
        {
            nlohmann::json ev;
            ev["session_id"] = session_id;
            ev["assistant_message_id"] = assistant_msg_id;
            ev["model_id"] = model_id;
            bus_.publish(events::EventType::StepStarted, ev);
        }

        std::string full_text;
        std::string full_reasoning;
        std::string text_id = haicode::util::make_id("txt");
        std::vector<ToolCall> tool_calls;
        FinishReason finish_reason = FinishReason::EndTurn;
        TokenUsage usage;
        bool step_failed = false;
        std::string step_error;

        StreamCallbacks cbs;
        cbs.on_text_delta = [&](const std::string& /*tid*/, const std::string& delta) {
            full_text += delta;
            nlohmann::json ev;
            ev["session_id"] = session_id;
            ev["assistant_message_id"] = assistant_msg_id;
            ev["text_id"] = text_id;
            ev["delta"] = delta;
            bus_.publish(events::EventType::TextDelta, ev);
        };
        cbs.on_reasoning_delta = [&](const std::string& delta) {
            if (full_reasoning.empty()) {
                nlohmann::json ev;
                ev["session_id"] = session_id;
                ev["assistant_message_id"] = assistant_msg_id;
                bus_.publish(events::EventType::ReasoningStarted, ev);
            }
            full_reasoning += delta;
            nlohmann::json ev;
            ev["session_id"] = session_id;
            ev["assistant_message_id"] = assistant_msg_id;
            ev["delta"] = delta;
            bus_.publish(events::EventType::ReasoningDelta, ev);
        };
        cbs.on_tool_input_delta = [&](const std::string& call_id,
                                       const std::string& name,
                                       const std::string& /*delta*/) {
            // Tool call streaming — we'll collect fully via on_finish
            (void)call_id; (void)name;
        };
        cbs.on_finish = [&](FinishReason reason, TokenUsage tok,
                             std::vector<ToolCall> calls) {
            finish_reason = reason;
            usage = tok;
            tool_calls = std::move(calls);
        };
        cbs.on_error = [&](const std::string& error) {
            step_failed = true;
            step_error = error;
        };

        provider->stream(req, cbs);

        // One retry on transient errors (provider overload, gateway timeout,
        // dropped connection), but only when nothing has streamed yet — if the
        // UI has already received text deltas, retrying would emit them again.
        auto is_transient_err = [](const std::string& err) {
            std::string lo = err;
            std::transform(lo.begin(), lo.end(), lo.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            static const char* hits[] = {
                "overloaded", "timeout", "timed out", "connection",
                "internal server", "service unavailable", "bad gateway",
                "gateway timeout", "temporarily unavailable",
            };
            for (const char* h : hits) {
                if (lo.find(h) != std::string::npos) return true;
            }
            return false;
        };
        if (step_failed && is_transient_err(step_error)
                && full_text.empty() && full_reasoning.empty()
                && tool_calls.empty()
                && !(interrupt_flag && interrupt_flag->load())) {
            fprintf(stderr, "[engine] transient provider error: %s — retrying once\n",
                    step_error.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            step_failed = false;
            step_error.clear();
            // Surface the retry in the UI so the user sees fresh activity.
            {
                nlohmann::json ev;
                ev["session_id"] = session_id;
                ev["assistant_message_id"] = assistant_msg_id;
                ev["model_id"] = model_id;
                bus_.publish(events::EventType::StepStarted, ev);
            }
            provider->stream(req, cbs);
        }

        // Context-overflow recovery: if the provider rejected the request as
        // too large AND nothing streamed (no side effects), compact once
        // through the same checkpoint path and retry once. No loop — a second
        // overflow falls through to StepFailed.
        auto is_overflow_err = [](const std::string& err) {
            std::string lo = err;
            std::transform(lo.begin(), lo.end(), lo.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            static const char* hits[] = {
                "context", "too long", "token limit", "context_length",
                "maximum context", "exceeds",
            };
            for (const char* h : hits) {
                if (lo.find(h) != std::string::npos) return true;
            }
            return false;
        };
        if (step_failed && is_overflow_err(step_error)
                && full_text.empty() && full_reasoning.empty()
                && tool_calls.empty()
                && !(interrupt_flag && interrupt_flag->load())) {
            fprintf(stderr, "[engine] context overflow: %s — compacting and "
                    "retrying once\n", step_error.c_str());
            if (compact_history(session_id, *provider, model_id, provider_id,
                                interrupt_flag, prev_total_input, 0)) {
                messages = load_context_messages(session_id);
                req = builder.build(messages, system, system_dynamic,
                                    tool_defs, model_id, provider_id);
                step_failed = false;
                step_error.clear();
                {
                    nlohmann::json ev;
                    ev["session_id"] = session_id;
                    ev["assistant_message_id"] = assistant_msg_id;
                    ev["model_id"] = model_id;
                    bus_.publish(events::EventType::StepStarted, ev);
                }
                provider->stream(req, cbs);
            }
        }

        if (step_failed) {
            nlohmann::json ev;
            ev["session_id"] = session_id;
            ev["error"] = step_error;
            bus_.publish(events::EventType::StepFailed, ev);
            break;
        }

        if (!full_reasoning.empty()) {
            nlohmann::json ev;
            ev["session_id"] = session_id;
            ev["assistant_message_id"] = assistant_msg_id;
            ev["full_text"] = full_reasoning;
            bus_.publish(events::EventType::ReasoningEnded, ev);
        }
        // Split off tool calls whose streamed input failed to parse. We don't
        // execute them, and crucially we don't persist them — otherwise the
        // next iteration would send the model its own phantom empty tool_use
        // block, which is what causes the "stumble on empty toolcall" loop.
        bool had_parse_failure = false;
        std::vector<std::string> failed_names;
        for (auto it = tool_calls.begin(); it != tool_calls.end(); ) {
            if (it->parse_failed) {
                had_parse_failure = true;
                failed_names.push_back(it->name);
                fprintf(stderr,
                    "[engine] dropping malformed tool_call %s (%s); raw=%zu bytes: %.200s\n",
                    it->id.c_str(), it->name.c_str(),
                    it->raw_input.size(), it->raw_input.c_str());

                nlohmann::json ev;
                ev["session_id"] = session_id;
                ev["call_id"]    = it->id;
                ev["tool_name"]  = it->name;
                ev["input"]      = it->input;
                ev["error"]      = "Tool input could not be parsed (likely lost during "
                                   "streaming). Not executed — retry the tool call.";
                bus_.publish(events::EventType::ToolFailed, ev);
                it = tool_calls.erase(it);
            } else {
                ++it;
            }
        }

        // If any tool calls were dropped, embed a synthetic note into the
        // assistant turn so the model sees on its next iteration that the calls
        // failed and need to be retried. Encoded as assistant text (not a
        // separate user message) to keep role alternation clean and avoid
        // forging a fake user turn in the chat history.
        if (had_parse_failure) {
            std::string names_str;
            for (size_t k = 0; k < failed_names.size(); ++k) {
                if (k) names_str += ", ";
                names_str += failed_names[k];
            }
            std::string note = "\n\n[SYSTEM NOTE: tool call input JSON failed to "
                               "parse during streaming (calls: " + names_str +
                               "). Those calls were dropped. Retry them on the "
                               "next turn.]";
            if (full_text.empty()) full_text = note;
            else full_text += note;
        }

        // Persist assistant turn (text and/or valid tool calls only).
        // On interrupt, omit tool_calls: they won't be executed, so recording
        // them would leave an assistant(tool_calls) with no following
        // tool_result — the model's chat template rejects that as a
        // role-alternation violation on the next request.
        const bool interrupted = interrupt_flag && interrupt_flag->load();
        if (!full_text.empty() || (!tool_calls.empty() && !interrupted)) {
            nlohmann::json data;
            data["role"] = "assistant";
            data["text"] = full_text;
            if (!full_reasoning.empty())
                data["reasoning"] = full_reasoning;
            if (!tool_calls.empty() && !interrupted) {
                auto calls_arr = nlohmann::json::array();
                for (auto& tc : tool_calls)
                    calls_arr.push_back({{"id",tc.id},{"name",tc.name},{"input",tc.input}});
                data["tool_calls"] = calls_arr;
            }
            store_.append_message(session_id, "assistant_text", data.dump());
        }

        // Compute per-turn cost from token usage and resolved pricing.
        // Unknown models fall back to 0.0 — silent, no warning.
        const ModelPricing* price = lookup_pricing(provider_id, model_id,
                                                    config_.pricing);
        double step_cost = price ? compute_cost(usage, *price) : 0.0;

        // Update cost
        store_.update_cost(session_id, step_cost, usage);

        // Track the prompt size this step reported, for next iteration's
        // compaction decision. Cache reads/writes still occupy the window, so
        // the sum is the true prompt size.
        prev_total_input = usage.input + usage.cache_read + usage.cache_write;

        // Publish step ended
        {
            nlohmann::json ev;
            ev["session_id"] = session_id;
            ev["assistant_message_id"] = assistant_msg_id;
            ev["finish_reason"] = (finish_reason == FinishReason::ToolUse) ? "tool_use" : "end_turn";
            ev["usage"] = {
                {"input",       usage.input},
                {"output",      usage.output},
                {"reasoning",   usage.reasoning},
                {"cache_read",  usage.cache_read},
                {"cache_write", usage.cache_write},
                {"cost_usd",    step_cost}
            };
            bus_.publish(events::EventType::StepEnded, ev);
        }

        // Break unless the model wanted to call tools and we have at least one
        // valid call to run. If every call was dropped due to a parse failure,
        // continue the loop so the model gets another turn to retry.
        if (finish_reason != FinishReason::ToolUse) break;
        if (tool_calls.empty() && !had_parse_failure) break;
        if (interrupt_flag && interrupt_flag->load()) break;

        // Execute tool calls
        bool any_denied = false;
        bool any_proposed = false;
        for (auto& call : tool_calls) {
            {
                nlohmann::json ev;
                ev["session_id"] = session_id;
                ev["call_id"] = call.id;
                ev["tool_name"] = call.name;
                ev["input"] = call.input;
                bus_.publish(events::EventType::ToolCalled, ev);
            }

            ToolContext ctx;
            ctx.session_id = session_id;
            ctx.call_id = call.id;
            ctx.working_dir = session.directory;
            ctx.config = &config_;
            ctx.mode = mode;

            auto result = tools_.execute(call.name, call.input, ctx, permissions_);

            // After a successful write or edit, run the configured build command
            // so the model sees compile errors immediately rather than
            // discovering them several steps later. Wrapped like BashTool:
            // cwd = the session's project directory, 300s timeout via the
            // `timeout` binary, output capped at 100 KB — the bare popen
            // used to inherit the app cwd with no bound on either.
            if (result.success && !config_.build_command.empty()
                    && (call.name == "write" || call.name == "edit")) {
                constexpr int kBuildHookTimeoutSec = 300;
                std::string inner = "cd " + shell_quote(session.directory)
                                  + " && { " + config_.build_command + "; }";
                std::string full_cmd = "timeout "
                                     + std::to_string(kBuildHookTimeoutSec)
                                     + " sh -c " + shell_quote(inner) + " 2>&1";
                FILE* bp = popen(full_cmd.c_str(), "r");
                if (bp) {
                    std::string build_out;
                    std::array<char, 4096> buf;
                    while (fgets(buf.data(), buf.size(), bp)) {
                        build_out += buf.data();
                        if (build_out.size() >= 100 * 1024) {
                            build_out.resize(100 * 1024);
                            build_out += "\n[output truncated]";
                            break;
                        }
                    }
                    int brc = pclose(bp);
                    int bec = WIFEXITED(brc) ? WEXITSTATUS(brc) : -1;

                    {
                        nlohmann::json bev;
                        bev["session_id"] = session_id;
                        bev["success"]    = (bec == 0);
                        bev["exit_code"]  = bec;
                        bus_.publish(events::EventType::BuildHookResult, bev);
                    }

                    if (bec == 124) {
                        result.output += "\n\n[build_hook] Build timed out after "
                                       + std::to_string(kBuildHookTimeoutSec)
                                       + "s\n" + build_out;
                        result.success = false;
                        result.error   = result.output;
                    } else if (bec != 0) {
                        result.output += "\n\n[build_hook] Build failed (exit "
                                       + std::to_string(bec) + "):\n" + build_out;
                        result.success = false;
                        result.error   = result.output;
                    }
                }
            }

            // Persist tool result. Tools may return a JSON object with a
            // reserved "attachments" array (screenshot): the array is moved
            // onto the row alongside the output text so it persists like user
            // attachments, and the stored output becomes the summary text.
            nlohmann::json data;
            data["call_id"] = call.id;
            data["success"] = result.success;
            if (result.success) {
                nlohmann::json out_json = nlohmann::json::parse(result.output,
                                                                nullptr, false);
                if (out_json.is_object() && out_json.contains("attachments")
                        && out_json["attachments"].is_array()
                        && !out_json["attachments"].empty()) {
                    data["output"] = out_json.value("summary", "");
                    data["attachments"] = out_json["attachments"];
                } else {
                    data["output"] = result.output;
                }
            } else {
                // A failed tool may still have produced output (bash stdout,
                // timeout captures) — that diagnostics text is what lets the
                // model recover. Persist it alongside the failure status.
                data["output"] = result.output.empty()
                    ? result.error
                    : result.error + "\n" + result.output;
            }
            store_.append_message(session_id, "tool_result", data.dump());

            {
                nlohmann::json ev;
                ev["session_id"] = session_id;
                ev["call_id"] = call.id;
                ev["output"] = data["output"];
                ev["success"] = result.success;
                bus_.publish(result.success ? events::EventType::ToolSuccess
                                            : events::EventType::ToolFailed, ev);
            }

            // Plan mode sentinel: a successful propose_plan ends the turn.
            // Publish PlanProposed so the UI can show its review window,
            // then break out of both the tool-call loop and (via the outer
            // `if (any_proposed)` check) the step loop.
            if (call.name == "propose_plan" && result.success) {
                std::string plan_path;
                try {
                    auto out_j = nlohmann::json::parse(result.output, nullptr, false);
                    if (out_j.is_object())
                        plan_path = out_j.value("path", "");
                } catch (...) {}

                nlohmann::json ev;
                ev["session_id"] = session_id;
                ev["path"]       = plan_path;
                ev["plan"]       = call.input.value("plan", "");
                bus_.publish(events::EventType::PlanProposed, ev);
                any_proposed = true;
                break;
            }

            // ask_user: the tool returned a placeholder. Publish
            // AskUserRequested so the UI shows a dialog, then block until the
            // user replies via reply_to_ask(). Once replied, overwrite the
            // placeholder tool_result with the real answer and re-publish
            // ToolSuccess so the UI's tool bubble shows the picked answer.
            if (call.name == "ask_user" && result.success) {
                std::string question = call.input.value("question", "");
                std::vector<std::string> options;
                if (call.input.contains("options") && call.input["options"].is_array()) {
                    for (auto& opt : call.input["options"]) {
                        if (opt.is_string()) options.push_back(opt.get<std::string>());
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(ask_mu_);
                    PendingAsk pa;
                    pa.session_id = session_id;
                    pa.call_id    = call.id;
                    pa.question   = question;
                    pa.options    = options;
                    pa.replied    = false;
                    pending_ask_[call.id] = pa;
                }

                nlohmann::json ev;
                ev["session_id"] = session_id;
                ev["call_id"]    = call.id;
                ev["question"]   = question;
                ev["options"]    = options;
                bus_.publish(events::EventType::AskUserRequested, ev);

                std::string answer;
                {
                    std::unique_lock<std::mutex> lock(ask_mu_);
                    const std::string& cid = call.id;
                    asking_cv_.wait(lock, [this, cid]() {
                        auto it = pending_ask_.find(cid);
                        return it != pending_ask_.end() && it->second.replied;
                    });
                    answer = pending_ask_[call.id].answer;
                    pending_ask_.erase(call.id);
                }

                // Overwrite the placeholder tool_result row with the real answer.
                nlohmann::json reply_out = {{"answer", answer}};
                store_.update_tool_result_by_call_id(call.id, reply_out.dump(2));

                // Re-publish ToolSuccess so the UI swaps the placeholder bubble
                // for the user's picked answer.
                {
                    nlohmann::json rev;
                    rev["session_id"] = session_id;
                    rev["call_id"]    = call.id;
                    rev["output"]     = reply_out.dump(2);
                    rev["success"]    = true;
                    bus_.publish(events::EventType::ToolSuccess, rev);
                }
                continue;
            }

            // todo_write: persist the parsed list to the session_todo
            // table and publish a TodoUpdated event so the UI can refresh.
            // Unlike propose_plan, this does NOT end the turn — the model
            // typically calls todo_write then continues with the first
            // in_progress item.
            if (call.name == "todo_write" && result.success) {
                std::vector<Todo> todos;
                try {
                    auto out_j = nlohmann::json::parse(result.output, nullptr, false);
                    if (out_j.is_object() && out_j.contains("todos")
                        && out_j["todos"].is_array()) {
                        for (auto& t : out_j["todos"]) {
                            Todo td;
                            td.content     = t.value("content", "");
                            td.active_form = t.value("activeForm", "");
                            td.status      = t.value("status", "pending");
                            todos.push_back(std::move(td));
                        }
                    }
                } catch (...) {}

                store_.replace_todos(session_id, todos);

                nlohmann::json ev;
                ev["session_id"] = session_id;
                nlohmann::json arr = nlohmann::json::array();
                for (auto& t : todos) {
                    arr.push_back({
                        {"content",    t.content},
                        {"activeForm", t.active_form},
                        {"status",     t.status}
                    });
                }
                ev["todos"] = arr;
                bus_.publish(events::EventType::TodoUpdated, ev);

                // Renewable step budget: a todo completion above the turn's
                // high-water mark refills the window to max_steps. Only NEW
                // completions count — re-completing the same items (or
                // flip-flopping a status) must never renew. ceiling_left is
                // deliberately untouched: renewal cannot lift the hard cap.
                int completed_now = 0;
                for (const auto& t : todos)
                    if (t.status == "completed") ++completed_now;
                if (completed_now > completed_high_water) {
                    completed_high_water = completed_now;
                    if (steps_left < max_steps) {
                        steps_left = max_steps;
                        fprintf(stderr, "[engine] session=%s todo progress "
                                        "(%d completed) — step budget renewed "
                                        "to %d\n",
                                session_id.c_str(), completed_now, max_steps);
                        fflush(stderr);
                    }
                }

                // Auto-retire the active plan when every todo is completed.
                if (!todos.empty() &&
                    std::all_of(todos.begin(), todos.end(),
                                [](const Todo& t){ return t.status == "completed"; })) {
                    std::string plans_dir = session.directory + "/.haicode/plans";
                    DIR* pd = opendir(plans_dir.c_str());
                    if (pd) {
                        std::string latest;
                        struct dirent* pe;
                        while ((pe = readdir(pd)) != nullptr) {
                            std::string n = pe->d_name;
                            if (n.size() < 4 || n.substr(n.size() - 3) != ".md") continue;
                            if (n <= latest) continue;
                            std::ifstream probe(plans_dir + "/" + n);
                            std::string first;
                            if (std::getline(probe, first) &&
                                first.find("haicode-status: active") != std::string::npos)
                                latest = n;
                        }
                        closedir(pd);
                        if (!latest.empty()) {
                            std::string plan_path = plans_dir + "/" + latest;
                            std::ifstream pf(plan_path);
                            std::ostringstream pss;
                            pss << pf.rdbuf();
                            std::string pcontent = pss.str();
                            const std::string kOld = "<!-- haicode-status: active -->";
                            const std::string kNew = "<!-- haicode-status: implemented -->";
                            auto ppos = pcontent.find(kOld);
                            if (ppos != std::string::npos)
                                pcontent.replace(ppos, kOld.size(), kNew);
                            // Best-effort: ignore write errors (plan still implemented).
                            (void)util::atomic_write_file(plan_path, pcontent);
                        }
                    }
                }
            }

            if (result.denied) { any_denied = true; break; }
            if (interrupt_flag && interrupt_flag->load()) break;
        }

        if (any_proposed) break;

        if (any_denied) {
            nlohmann::json ev;
            ev["session_id"] = session_id;
            ev["error"] = "Stopped: permission denied by user.";
            bus_.publish(events::EventType::StepFailed, ev);
            break;
        }

        // Budget bookkeeping at the end of each iteration. Every non-depletion
        // exit breaks mid-body before reaching here, so reaching the loop
        // condition with steps_left == 0 (or ceiling_left == 0) is unambiguous.
        --steps_left;
        --ceiling_left;
        ++iter;
    }

    // Loop exited by depleting a budget (no break fired — every other exit
    // breaks mid-body above). The last iteration would have left the UI in
    // "tool_use ended, waiting for next step" state with no follow-up ever
    // coming — surface a clear message so the user isn't left staring at
    // silence.
    if (ceiling_left <= 0) {
        nlohmann::json ev;
        ev["session_id"] = session_id;
        ev["error"] = "Hard step ceiling reached ("
                     + std::to_string(kStepCeilingMultiplier * max_steps)
                     + " steps this turn). Send another message to continue.";
        bus_.publish(events::EventType::StepFailed, ev);
    } else if (steps_left <= 0) {
        nlohmann::json ev;
        ev["session_id"] = session_id;
        ev["error"] = "Turn step budget exhausted ("
                     + std::to_string(max_steps)
                     + " steps without completing a todo)."
                       " Send another message to continue.";
        bus_.publish(events::EventType::StepFailed, ev);
    }

    // Periodic LLM title refinement. Fires on turn 1 (generates a fresh title)
    // and again every 5 turns (6, 11, 16, …) so the model can reconsider the
    // title as the conversation's focus shifts. The 1/6/11/… cadence comes
    // from `user_count % 5 == 1`. Best-effort — failures leave the existing
    // title in place. Skipped when autonaming or refine is disabled.
    if (config_.autoname_sessions && config_.autoname_llm_refine) {
        int user_count = 0;
        for (const auto& m : store_.load_messages(session_id)) {
            if (m.type == "user_prompted") ++user_count;
        }
        if (user_count >= 1 && user_count % 5 == 1) {
            refine_title_llm(session_id, *provider, model_id);
        }
    }

    // Clear the active provider entry now that the loop has exited.
    {
        std::lock_guard<std::mutex> lock(mu_);
        session_providers_.erase(session_id);
    }
}

void SessionEngine::reply_to_ask(const std::string& session_id,
                                  const std::string& call_id,
                                  const std::string& answer) {
    std::lock_guard<std::mutex> lock(ask_mu_);
    auto it = pending_ask_.find(call_id);
    if (it != pending_ask_.end() && it->second.session_id == session_id) {
        it->second.answer = answer;
        it->second.replied = true;
        asking_cv_.notify_all();
    }
}

bool should_compact_with_hysteresis(int prev_total_input,
                                    int threshold_tokens,
                                    int step,
                                    int last_compaction_step) {
    if (threshold_tokens <= 0) return false;
    if (prev_total_input < threshold_tokens) return false;
    // First time we've ever crossed threshold this turn — fire.
    if (last_compaction_step < 0) return true;
    // Already compacted recently. Enforce a 2-step minimum gap so a tail that
    // remains above threshold doesn't trigger compaction on every single step.
    if (step - last_compaction_step < 2) return false;
    return true;
}

std::vector<SessionMessage> SessionEngine::load_context_messages(
    const std::string& session_id)
{
    auto msgs = store_.load_messages(session_id);
    auto cp = store_.latest_complete_checkpoint(session_id);
    if (!cp) return msgs;
    return apply_checkpoint(msgs, *cp);
}

bool SessionEngine::compact_history(const std::string& session_id,
                                     Provider& provider,
                                     const std::string& model_id,
                                     const std::string& provider_id,
                                     std::atomic<bool>* interrupt_flag,
                                     int prev_input_tokens,
                                     int threshold_tokens)
{
    // Single-flight: the auto path (agentic loop thread) and compact_now
    // (background worker) both check/set this under mu_.
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (compaction_in_progress_.count(session_id)) return false;
        compaction_in_progress_.insert(session_id);
    }
    auto clear_guard = [this, &session_id]() {
        std::lock_guard<std::mutex> lock(mu_);
        compaction_in_progress_.erase(session_id);
    };

    auto prev_cp_opt = store_.latest_complete_checkpoint(session_id);
    int prev_through_seq = prev_cp_opt ? prev_cp_opt->through_seq : 0;

    auto messages = store_.load_messages(session_id);
    int through_seq = split_history(messages, config_.compaction_recent_context,
                                    prev_through_seq);
    if (through_seq < 0) { clear_guard(); return false; }

    std::vector<SessionMessage> older, recent;
    for (auto& m : messages) {
        if (m.seq <= 0) continue;                      // legacy summary rows
        if (m.seq <= prev_through_seq) continue;       // already summarized
        (m.seq <= through_seq ? older : recent).push_back(m);
    }
    if (older.empty()) { clear_guard(); return false; }

    {
        nlohmann::json ev;
        ev["session_id"]        = session_id;
        ev["prev_input_tokens"] = prev_input_tokens;
        ev["threshold"]         = threshold_tokens;
        ev["messages_before"]   = older.size();
        ev["through_seq"]       = through_seq;
        bus_.publish(events::EventType::CompactionStarted, ev);
    }

    std::string checkpoint_id;  // set once the pending row exists

    auto fail = [&](const char* why, const std::string& err) {
        fprintf(stderr, "[engine] compaction failed: %s (%s)\n", why, err.c_str());
        if (!checkpoint_id.empty()) store_.fail_checkpoint(checkpoint_id);
        nlohmann::json ev;
        ev["session_id"]      = session_id;
        ev["status"]          = "failed";
        ev["messages_before"] = older.size();
        ev["messages_after"]  = older.size();
        ev["through_seq"]     = through_seq;
        bus_.publish(events::EventType::CompactionEnded, ev);
        clear_guard();
        return false;
    };

    const size_t max_tool_out = 10 * 1024;
    std::string serialized_older = serialize_history(older, max_tool_out);
    std::string recent_context   = serialize_history(recent, max_tool_out);
    std::string previous_summary = prev_cp_opt ? prev_cp_opt->summary : "";
    std::string aged_context     = prev_cp_opt ? prev_cp_opt->recent_context : "";

    // Pending row first — the visible crash marker. Messages arriving during
    // summarization get higher seqs and stay after the boundary.
    checkpoint_id = store_.insert_checkpoint(
        session_id, through_seq, recent_context,
        prev_cp_opt ? prev_cp_opt->id : "");

    const int summary_cap = config_.compaction_summary_max_tokens;

    // Progress: map the summarizer's streamed output onto a percent of the
    // output cap, rescaled into [lo, hi] so a chunked two-pass merge can give
    // each pass its slice of the range. Published only when the percent
    // changes, so a fast stream doesn't flood the bus.
    int progress_lo = 0, progress_hi = 99;
    int last_progress = -1;
    auto emit_progress = [&](const std::string& acc) {
        int tokens = estimate_text_tokens(acc);
        int pct = summary_cap > 0 ? (tokens * 100) / summary_cap : 100;
        if (pct > 100) pct = 100;
        int scaled = progress_lo
                   + (pct * (progress_hi - progress_lo) + 50) / 100;
        if (scaled == last_progress) return;
        last_progress = scaled;
        nlohmann::json ev;
        ev["session_id"] = session_id;
        ev["percent"]    = scaled;
        bus_.publish(events::EventType::CompactionProgress, ev);
    };

    auto run_summary = [&](const std::string& prompt, std::string& out,
                           std::string& err) -> bool {
        LLMRequest r;
        r.model_id   = model_id;
        r.system     = "You are a precise conversation summarizer.";
        r.max_tokens = summary_cap;
        nlohmann::json m;
        m["role"] = "user";
        m["content"] = prompt;
        r.messages = {m};
        out.clear();
        bool failed = false;
        StreamCallbacks cbs;
        cbs.on_text_delta = [&](const std::string&, const std::string& d) {
            out += d;
            emit_progress(out);
        };
        // Providers invoke on_finish unconditionally; leaving it unset makes
        // an empty std::function call (std::bad_function_call → abort).
        cbs.on_finish = [](FinishReason, TokenUsage, std::vector<ToolCall>) {};
        cbs.on_error = [&](const std::string& e) { failed = true; err = e; };
        provider.stream(r, cbs);
        return !failed;
    };

    // Generate + validate, with one corrective retry re-sending the template
    // plus the invalid draft.
    auto summarize_validated = [&](const std::string& prompt, std::string& out,
                                   std::string& err) -> bool {
        if (!run_summary(prompt, out, err)) return false;
        if (validate_summary(out, summary_cap)) return true;
        fprintf(stderr, "[engine] summary failed validation; retrying once\n");
        last_progress = -1;  // the retry restarts the stream; re-emit from low
        std::string retry = prompt +
            "\n\n--- YOUR PREVIOUS ATTEMPT (invalid — fix its structure, keep "
            "its content) ---\n" + out;
        if (!run_summary(retry, out, err)) return false;
        return validate_summary(out, summary_cap);
    };

    int window = haicode::get_context_window(provider_id, model_id,
                                             config_.model_contexts, &provider);
    if (window <= 0) {
        // Manual compaction (and overflow recovery) must work even when the
        // window is unknown: estimate it from the current context size + 20%.
        // The auto-trigger is NOT affected — it stays gated on a known window.
        size_t chars = 0;
        for (const auto& m : messages) chars += m.data_json.size();
        int base = static_cast<int>(chars / 4) + 8192;  // + system/tools overhead
        window = base + base / 5;
    }
    int room = window - std::max(summary_cap, config_.compaction_buffer);

    auto fit_serialized = [&](const std::string& prev,
                              const std::string& serialized) {
        if (room <= 0) return serialized;
        size_t overhead = 1600 + prev.size();
        size_t allowed  = static_cast<size_t>(room) * 4;
        if (overhead >= allowed) allowed = overhead + 1;
        if (serialized.size() + overhead <= allowed) return serialized;
        std::string t = serialized.substr(0, allowed - overhead);
        t += "\n[truncated: " + std::to_string(serialized.size() - t.size())
           + " more bytes — older content omitted]";
        return t;
    };

    std::string summary, err;
    bool generated = false;
    {
        std::string prompt = build_summary_prompt(previous_summary, aged_context,
                                                  serialized_older);
        if (room <= 0 || estimate_text_tokens(prompt) < room) {
            generated = summarize_validated(prompt, summary, err);
        } else {
            // Bounded two-pass chunked merge, each pass clamped to fit.
            size_t mid = older.size() / 2;
            while (mid < older.size() - 1 && older[mid].type == "tool_result")
                ++mid;  // don't split an assistant from its tool results
            std::vector<SessionMessage> first(older.begin(), older.begin() + mid),
                                        second(older.begin() + mid, older.end());
            progress_lo = 0;  progress_hi = 49;  last_progress = -1;
            std::string pass1, p1 = build_summary_prompt(
                previous_summary, aged_context,
                fit_serialized(previous_summary + aged_context,
                               serialize_history(first, max_tool_out)));
            if (!summarize_validated(p1, pass1, err))
                return fail("chunked pass 1 failed", err);
            if (interrupt_flag && interrupt_flag->load())
                return fail("interrupted", "");
            progress_lo = 50; progress_hi = 99;  last_progress = -1;
            std::string p2 = build_summary_prompt(
                pass1, "",
                fit_serialized(pass1, serialize_history(second, max_tool_out)));
            generated = summarize_validated(p2, summary, err);
        }
    }

    if (interrupt_flag && interrupt_flag->load())
        return fail("interrupted", "");
    if (!generated)
        return fail("summarizer call failed", err);
    if (!validate_summary(summary, summary_cap))
        return fail("summary still invalid after retry", "");

    // Atomic commit: one UPDATE flips pending → complete.
    store_.complete_checkpoint(checkpoint_id, summary);

    // Refresh the context meter: no provider usage will arrive until the next
    // step, so estimate the post-compaction request (checkpoint block +
    // retained tail — exactly what load_context_messages now returns) with
    // the same chars/4 + overhead approximation the unknown-window path uses.
    // Persisted to tok_last_input so a relaunch shows it; the next StepEnded
    // replaces it with the exact reported usage.
    int post_context_tokens = 0;
    {
        size_t chars = 0;
        for (const auto& m : load_context_messages(session_id))
            chars += m.data_json.size();
        post_context_tokens = static_cast<int>(chars / 4) + 8192;
    }
    store_.update_last_input_tokens(session_id, post_context_tokens);

    {
        nlohmann::json ev;
        ev["session_id"]      = session_id;
        ev["status"]          = "complete";
        ev["checkpoint_id"]   = checkpoint_id;
        ev["through_seq"]     = through_seq;
        ev["messages_before"] = older.size() + recent.size();
        ev["messages_after"]  = recent.size() + 1;
        ev["summary"]         = summary;
        ev["context_tokens"]  = post_context_tokens;
        bus_.publish(events::EventType::CompactionEnded, ev);
    }

    clear_guard();
    return true;
}

void SessionEngine::refine_title_llm(const std::string& session_id,
                                      Provider& provider,
                                      const std::string& model_id)
{
    // Load the full message history so we can summarize the conversation's
    // actual subject, not just the first prompt. We take the latest user
    // prompt and a sample of recent assistant text to keep the call cheap.
    auto messages = store_.load_messages(session_id);
    std::vector<std::string> user_texts;
    for (const auto& m : messages) {
        if (m.type != "user_prompted") continue;
        auto j = nlohmann::json::parse(m.data_json, nullptr, false);
        if (j.is_object()) user_texts.push_back(j.value("text", ""));
    }
    if (user_texts.empty()) return;

    // Reconsideration: the current persisted title (may be empty on first call).
    auto sess = store_.get(session_id);
    std::string current_title = sess ? sess->title : "";

    // Build a compact digest of the user's prompts so the model has the real
    // subject matter. Clamp each to keep the request small.
    auto clamp = [](std::string s) -> std::string {
        if (s.size() > 400) s = s.substr(0, 400) + "…";
        return s;
    };
    std::string digest;
    for (size_t i = 0; i < user_texts.size(); ++i) {
        digest += std::to_string(i + 1) + ". " + clamp(user_texts[i]) + "\n";
    }

    nlohmann::json user_msg;
    user_msg["role"] = "user";
    if (current_title.empty()) {
        user_msg["content"] =
            "Write a concise session title (at most 6 words, no quotes, no "
            "trailing punctuation) summarizing what the user wants based on "
            "these prompts. Respond with only the title.\n\n"
            "Prompts:\n" + digest;
    } else {
        user_msg["content"] =
            "The current session title is: \"" + current_title + "\"\n\n"
            "Here are the user's prompts so far:\n" + digest +
            "\nReconsider the title. If it still fits the conversation, "
            "repeat it verbatim. If it no longer reflects what the session is "
            "about, write a better concise title (at most 6 words, no quotes, "
            "no trailing punctuation). Respond with only the title.";
    }

    LLMRequest req;
    req.model_id    = model_id;
    req.system      = "You generate short descriptive titles for chat sessions.";
    req.messages    = {std::move(user_msg)};
    req.max_tokens  = 48;

    std::string raw_title;
    bool failed = false;
    std::string err;

    StreamCallbacks cbs;
    cbs.on_text_delta = [&](const std::string& /*tid*/, const std::string& delta) {
        raw_title += delta;
    };
    cbs.on_finish = [&](FinishReason, TokenUsage, std::vector<ToolCall>) {};
    cbs.on_error = [&](const std::string& error) {
        failed = true;
        err = error;
    };

    provider.stream(req, cbs);

    if (failed) {
        fprintf(stderr, "[engine] title refinement failed: %s\n", err.c_str());
        return;
    }

    // Trim surrounding whitespace and stray quotes the model sometimes adds.
    auto trim = [](std::string s) {
        size_t b = s.find_first_not_of(" \t\r\n\"'");
        if (b == std::string::npos) return std::string();
        size_t e = s.find_last_not_of(" \t\r\n\"'");
        return s.substr(b, e - b + 1);
    };
    // The model may emit a trailing newline or a second line; keep only the first.
    size_t nl = raw_title.find('\n');
    if (nl != std::string::npos) raw_title = raw_title.substr(0, nl);
    std::string title = trim(raw_title);
    if (title.empty()) return;

    // On reconsideration, skip the write if the model echoed the existing title
    // verbatim — avoids a needless DB update and sidebar flicker.
    if (!current_title.empty() && title == current_title) return;

    store_.update_title(session_id, title);
    nlohmann::json ev;
    ev["session_id"] = session_id;
    ev["title"] = title;
    bus_.publish(events::EventType::SessionRenamed, ev);
}

bool SessionEngine::vision_fallback_ready()
{
    if (config_.vision_fallback_model.empty())
        return false;
    // Resolve the fallback provider: explicit config wins; empty means "same
    // provider as the session".
    std::string pid = config_.vision_fallback_provider;
    if (pid.empty()) pid = config_.provider;
    if (!providers_.get(pid)) {
        static bool warned = false;
        if (!warned) {
            fprintf(stderr, "[engine] vision fallback provider not registered: %s\n",
                    pid.c_str());
            warned = true;
        }
        return false;
    }
    // No prefix-table check on the fallback model: it is an explicit user
    // choice, trusted as-is. Local vision servers (llava, qwen-vl, custom
    // names) are rarely in the kVisionModels table, and rejecting them here
    // silently disabled the feature for exactly the setups that need it.
    return true;
}

std::string SessionEngine::describe_image(Provider& provider,
                                          const std::string& model_id,
                                          const nlohmann::json& att)
{
    nlohmann::json content = nlohmann::json::array();
    content.push_back({{"type", "text"}, {"text",
        "Describe this image factually and concisely for a text-only coding "
        "assistant. Cover: UI elements and their layout, any text content "
        "visible in the image (error messages verbatim), relevant colors or "
        "state indicators. Plain text only, no markdown headers."}});
    content.push_back({
        {"type", "image"},
        {"source", {
            {"type", "base64"},
            {"media_type", att.value("media_type", "image/png")},
            {"data", att.value("data_b64", "")}
        }}
    });
    nlohmann::json user_msg;
    user_msg["role"] = "user";
    user_msg["content"] = content;

    LLMRequest req;
    req.model_id  = model_id;
    req.system    = "You describe images for a text-only coding assistant.";
    req.messages  = {std::move(user_msg)};
    req.max_tokens = 1024;

    std::string text;
    bool failed = false;
    std::string err;

    StreamCallbacks cbs;
    cbs.on_text_delta = [&](const std::string& /*tid*/, const std::string& delta) {
        text += delta;
    };
    cbs.on_finish = [&](FinishReason, TokenUsage, std::vector<ToolCall>) {};
    cbs.on_error = [&](const std::string& error) {
        failed = true;
        err = error;
    };

    provider.stream(req, cbs);

    if (failed) {
        fprintf(stderr, "[engine] vision fallback describe failed: %s\n",
                err.c_str());
        return "";
    }
    return text;
}

void SessionEngine::backfill_attachment_descriptions(
    const std::string& session_id,
    std::vector<SessionMessage>& messages)
{
    if (!vision_fallback_ready()) return;

    std::string pid = config_.vision_fallback_provider;
    if (pid.empty()) pid = config_.provider;
    auto provider = providers_.get(pid);
    if (!provider) return;

    for (auto& msg : messages) {
        if (msg.type != "user_prompted" && msg.type != "tool_result") continue;
        auto data = nlohmann::json::parse(msg.data_json, nullptr, false);
        if (!data.is_object() || !data.contains("attachments")
                || !data["attachments"].is_array())
            continue;

        bool changed = false;
        for (auto& att : data["attachments"]) {
            if (!att.is_object()) continue;
            if (att.value("absent", false)) continue;  // nothing to describe
            if (att_is_text(att)) continue;  // no vision needed
            if (att.contains("description")) continue;  // describe once
            std::string desc = describe_image(*provider,
                                              config_.vision_fallback_model,
                                              att);
            if (!desc.empty()) {
                // Clamp persisted descriptions so one verbose image can't
                // bloat every future request.
                if (desc.size() > 4096) desc.resize(4096);
                att["description"] = desc;
                changed = true;
            }
        }
        if (changed) {
            std::string updated = data.dump();
            store_.update_message_data(session_id, msg.seq, updated);
            msg.data_json = std::move(updated);
        }
    }
}

void SessionEngine::seed_todos(const std::string& session_id,
                               const std::vector<Todo>& todos)
{
    store_.replace_todos(session_id, todos);

    nlohmann::json ev;
    ev["session_id"] = session_id;
    ev["todos"] = nlohmann::json::array();
    for (auto& t : todos) {
        nlohmann::json item;
        item["content"]    = t.content;
        item["activeForm"] = t.active_form;
        item["status"]     = t.status;
        ev["todos"].push_back(item);
    }
    bus_.publish(events::EventType::TodoUpdated, ev);
}

void SessionEngine::compact_now(const std::string& session_id) {
    auto session_opt = store_.get(session_id);
    if (!session_opt) return;
    auto session = *session_opt;
    auto model_json = nlohmann::json::parse(session.model_json, nullptr, false);
    std::string model_id = model_json.value("id", config_.model);
    std::string provider_id = model_json.value("provider_id", "anthropic");

    auto provider = providers_.get(provider_id);
    if (!provider) {
        nlohmann::json ev;
        ev["session_id"] = session_id;
        ev["error"] = "No provider available for: " + provider_id;
        bus_.publish(events::EventType::CompactionEnded, ev);
        return;
    }

    // Acquire the same lock submit_prompt / continue_session use, and mark
    // the session as running for the duration of the worker thread. This
    // closes the race where compact_now used to release mu_ before spawning
    // a detached worker, letting a subsequent submit_prompt also start the
    // agentic_loop and trample the same SQLite rows.
    std::lock_guard<std::mutex> lock(mu_);
    // No new runners during destruction (see submit_prompt).
    if (shutting_down_) return;
    if (session_running_.count(session_id) && session_running_[session_id]) {
        nlohmann::json ev;
        ev["session_id"] = session_id;
        ev["error"] = "Cannot compact while the session is running.";
        bus_.publish(events::EventType::CompactionEnded, ev);
        return;
    }

    // Join any previously finished runner thread before overwriting the slot
    // (matches the pattern in submit_prompt / continue_session).
    auto th_it = runner_threads_.find(session_id);
    if (th_it != runner_threads_.end() && th_it->second.joinable())
        th_it->second.join();

    session_running_[session_id] = true;
    runner_threads_[session_id] = std::thread(
        [this, session_id, provider, model_id, provider_id]() {
            // Sentinel threshold of 0 — compact_history only echoes it in the
            // CompactionStarted payload, not in the decision logic.
            bool committed = compact_history(session_id, *provider, model_id,
                                             provider_id, nullptr, 0, 0);
            if (!committed) {
                // Manual compactions must not fail silently: the user pressed
                // a button. Explain why nothing changed.
                auto cp = store_.latest_complete_checkpoint(session_id);
                nlohmann::json ev;
                ev["session_id"] = session_id;
                ev["status"]     = "skipped";
                ev["messages_before"] = store_.load_messages(session_id).size();
                ev["messages_after"]  = ev["messages_before"];
                bus_.publish(events::EventType::CompactionEnded, ev);
            }
            std::lock_guard<std::mutex> g(mu_);
            session_running_[session_id] = false;
        });
}

static std::string to_active_form(const std::string& content) {
    auto sp = content.find(' ');
    if (sp == std::string::npos) return content;
    std::string verb = content.substr(0, sp);
    std::string rest = content.substr(sp);
    static const std::map<std::string, std::string> g = {
        {"Add","Adding"}, {"Write","Writing"}, {"Fix","Fixing"},
        {"Update","Updating"}, {"Remove","Removing"}, {"Delete","Deleting"},
        {"Create","Creating"}, {"Implement","Implementing"}, {"Refactor","Refactoring"},
        {"Test","Testing"}, {"Move","Moving"}, {"Rename","Renaming"},
        {"Check","Checking"}, {"Run","Running"}, {"Build","Building"},
        {"Install","Installing"}, {"Parse","Parsing"}, {"Handle","Handling"},
        {"Set","Setting"}, {"Make","Making"}, {"Enable","Enabling"},
        {"Disable","Disabling"}, {"Read","Reading"}, {"Load","Loading"},
        {"Save","Saving"}, {"Edit","Editing"}, {"Patch","Patching"},
        {"Clean","Cleaning"}, {"Migrate","Migrating"}, {"Deploy","Deploying"},
        {"Configure","Configuring"}, {"Register","Registering"},
        {"Subscribe","Subscribing"}, {"Publish","Publishing"},
        {"Emit","Emitting"}, {"Send","Sending"}, {"Fetch","Fetching"},
        {"Wire","Wiring"}, {"Connect","Connecting"}, {"Expose","Exposing"},
        {"Extend","Extending"}, {"Declare","Declaring"},
    };
    auto it = g.find(verb);
    return it != g.end() ? it->second + rest : content;
}

std::vector<Todo> parse_plan_tasks(const std::string& markdown) {
    std::vector<Todo> todos;
    bool in_tasks = false;
    std::istringstream ss(markdown);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.rfind("## Task", 0) == 0) { in_tasks = true; continue; }
        if (in_tasks && line.size() >= 2 && line[0] == '#' && line[1] == '#') break;
        if (!in_tasks) continue;
        std::string content;
        if      (line.rfind("- [ ] ", 0) == 0) content = line.substr(6);
        else if (line.rfind("- [x] ", 0) == 0) content = line.substr(6);
        else if (line.rfind("- [X] ", 0) == 0) content = line.substr(6);
        else if (line.rfind("- ", 0) == 0)     content = line.substr(2);
        else continue;
        while (!content.empty() && (content.back() == ' ' || content.back() == '\r'))
            content.pop_back();
        if (content.empty()) continue;
        Todo t;
        t.content     = content;
        t.active_form = to_active_form(content);
        t.status      = "pending";
        todos.push_back(t);
    }
    return todos;
}

} // namespace haicode
