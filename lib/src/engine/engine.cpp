#include <haicode/engine.h>
#include <haicode/util.h>
#include <haicode/default_prompt.h>
#include <haicode/pricing.h>
#include <haicode/model_info.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <sys/utsname.h>
#include <cstdio>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <set>
#include <dirent.h>

namespace haicode {

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

std::string render_dynamic_prompt(const std::string& model,
                                  const std::string& os_info,
                                  const std::string& project_dir,
                                  int steps_left) {
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

// Rough token estimate: ~4 characters per token (the common BPE average for
// English/code). Used only as a pre-flight compaction trigger before any
// provider usage is available; once a step reports real usage, that value
// takes over. Slightly over-estimates JSON/whitespace-heavy content, which is
// the safe direction (trigger a touch early rather than a touch late).
static int estimate_request_tokens(const std::string& system,
                                    const std::string& system_dynamic,
                                    const std::vector<nlohmann::json>& messages)
{
    size_t chars = system.size() + system_dynamic.size();
    for (auto& m : messages)
        chars += m.dump().size();
    return static_cast<int>(chars / 4);
}

int choose_tail_start_seq(const std::vector<SessionMessage>& msgs, int K) {
    if (msgs.empty()) return -1;

    // Multi-turn case: tail starts at the most recent user_prompted, provided
    // it is not the very first message (otherwise the head would be empty).
    for (size_t i = msgs.size(); i-- > 0; ) {
        if (msgs[i].type == "user_prompted") {
            if (i > 0) return msgs[i].seq;
            break;  // only user_prompted is at index 0 — fall through
        }
    }

    // Single-turn fallback: keep the last K assistant_text-with-tool_calls
    // round-trips intact and summarize everything before them.
    int count = 0;
    int boundary_idx = -1;
    for (size_t i = msgs.size(); i-- > 0; ) {
        if (msgs[i].type != "assistant_text") continue;
        bool has_tools = false;
        try {
            auto d = nlohmann::json::parse(msgs[i].data_json);
            has_tools = d.contains("tool_calls")
                     && d["tool_calls"].is_array()
                     && !d["tool_calls"].empty();
        } catch (...) {}
        if (!has_tools) continue;
        ++count;
        if (count >= K) { boundary_idx = static_cast<int>(i); break; }
    }
    // Need something before the boundary to summarize.
    if (boundary_idx <= 0) return -1;
    return msgs[boundary_idx].seq;
}

LLMRequest ContextBuilder::build(
    const std::vector<SessionMessage>& messages,
    const std::string& system_prompt,
    const std::string& system_dynamic,
    const std::vector<ToolDefinition>& tools,
    const std::string& model_id,
    const std::string& /*provider_id*/)
{
    LLMRequest req;
    req.model_id = model_id;
    req.system = system_prompt;
    req.system_dynamic = system_dynamic;
    req.tools = tools;
    req.messages = assemble_messages(messages);
    return req;
}

std::vector<nlohmann::json> ContextBuilder::assemble_messages(
    const std::vector<SessionMessage>& msgs)
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
                m["content"] = data.value("text", "");
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
                content["content"] = output;
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
    std::unique_lock<std::mutex> lock(mu_);
    for (auto& [id, flag] : interrupt_flags_)
        if (flag) flag->store(true);
    auto threads = std::move(runner_threads_);
    // Cancel all providers so in-flight HTTP requests abort immediately.
    for (auto& [id, provider] : session_providers_) {
        lock.unlock();
        provider->cancel();
        lock.lock();
    }
    lock.unlock();
    // Detach threads — they will finish on their own once the interrupt flag
    // fires and cancel() unblocks the HTTP request. We cannot safely join
    // here because a thread may be blocked on a permission future (waiting
    // for user input that will never come during shutdown). In practice the
    // caller (HaiCodeApp::QuitRequested) calls exit() immediately after,
    // so the OS cleans up.
    for (auto& [id, t] : threads)
        if (t.joinable()) t.detach();
    for (auto& [id, flag] : interrupt_flags_)
        delete flag;
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

    auto session = store_.create(project_dir, eff_agent, model_json.dump());
    return session.id;
}

void SessionEngine::submit_prompt(const std::string& session_id,
                                   const std::string& text) {
    // Persist the user message
    nlohmann::json data;
    data["role"] = "user";
    data["text"] = text;
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
    bus_.publish(events::EventType::Prompted, ev);

    // Start runner thread if not already running for this session
    std::lock_guard<std::mutex> lock(mu_);
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

    // 3. Publish Interrupted event so UIs know the interrupt was processed.
    nlohmann::json ev;
    ev["session_id"] = session_id;
    bus_.publish(events::EventType::Interrupted, ev);
}

void SessionEngine::set_mode(const std::string& session_id, SessionMode mode) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        session_modes_[session_id] = mode;
    }
    store_.update_mode(session_id, mode == SessionMode::Plan ? "plan" : "build");
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
            return mj.value("mode", config_.default_mode) == "plan"
                ? SessionMode::Plan : SessionMode::Build;
        } catch (...) {}
    }
    return (config_.default_mode == "plan") ? SessionMode::Plan : SessionMode::Build;
}

void SessionEngine::agentic_loop(const std::string& session_id) {
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
            mode = (mode_str == "plan") ? SessionMode::Plan : SessionMode::Build;
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

    std::string system = render_prompt(prompt_tmpl, model_id, os_info, session.directory, max_steps)
                       + agents_md_block
                       + latest_plan_block
                       + instructions_block
                       + plan_mode_block;

    // Dynamic per-step content ({{STEPS_LEFT}}). Emitted as a separate
    // system text block by the Anthropic provider so the stable body
    // above stays byte-identical across turns and hits the prefix cache.
    std::string system_dynamic = render_dynamic_prompt(model_id, os_info,
                                                       session.directory,
                                                       max_steps);

    fprintf(stderr, "[engine] session=%s dir='%s' agent=%s mode=%s max_steps=%d instructions=%zu\n[engine] system prompt:\n%s\n---\n",
            session_id.c_str(), session.directory.c_str(), session.agent.c_str(),
            mode == SessionMode::Plan ? "plan" : "build",
            max_steps, config_.instructions.size(), system.c_str());
    fflush(stderr);

    // Agentic loop
    int step = 0;
    // Input-token count reported by the provider on the previous step. The true
    // prompt size is input + cache_read + cache_write (cache reads/writes still
    // occupy the context window). Used to decide whether to compact.
    int prev_total_input = 0;
    for (; step < max_steps; ++step) {
        if (interrupt_flag && interrupt_flag->load()) break;

        // Re-read model_id/provider_id (and inference params) from the session
        // each step. The user can change either via the dropdown mid-loop, and
        // the system prompt (re-rendered below) plus the outgoing request must
        // reflect the new values on the very next step.
        nlohmann::json mj_now;
        if (auto s_now = store_.get(session_id)) {
            mj_now = nlohmann::json::parse(s_now->model_json, nullptr, false);
            if (mj_now.is_object()) {
                if (auto v = mj_now.value("id", ""); !v.empty())        model_id    = v;
                if (auto v = mj_now.value("provider_id", ""); !v.empty()) provider_id = v;
            }
        }

        // Re-read mode each step. The user can toggle Plan/Build mid-loop
        // (GUI _ToggleMode / TUI toggle_mode call set_mode, which updates the
        // in-memory cache + DB synchronously); the tool allowlist and the
        // plan-mode system block below must reflect the flip on the next step,
        // not on the next turn.
        mode = get_mode(session_id);
        plan_mode_block = (mode == SessionMode::Plan) ? kPlanModeInstructions
                                                       : std::string{};

        // Re-render the system prompt each step so {{MODEL}} and {{STEPS_LEFT}}
        // stay current.
        system = render_prompt(prompt_tmpl, model_id, os_info, session.directory,
                               max_steps - step) + agents_md_block + latest_plan_block
                              + instructions_block + plan_mode_block;
        // {{STEPS_LEFT}} decrements each step → re-render the dynamic block too.
        system_dynamic = render_dynamic_prompt(model_id, os_info,
                                                session.directory,
                                                max_steps - step);

        // Re-inject the current todo list (Build mode only) so the model
        // stays anchored to outstanding work without having to remember it
        // from the plan. Lives in the dynamic block to preserve the stable
        // body's prefix cache.
        if (mode == SessionMode::Build) {
            auto todos_now = store_.load_todos(session_id);
            system_dynamic += render_todos_block(todos_now);
        }

        auto messages = store_.load_messages(session_id);

        ContextBuilder builder;
        auto tool_defs = tools_.definitions();
        // Filter tools by mode. Plan mode uses an allowlist (fail-closed):
        // anything not explicitly safe for research is hidden, so future
        // tools don't silently leak into Plan turns. Build mode has no filter.
        if (mode == SessionMode::Plan) {
            static const std::set<std::string> plan_allowed = {
                "read", "glob", "grep", "ls", "find",
                "web_search", "web_extract",
                "diff", "todo_write", "ask_user",
                "propose_plan", "discard_plan",
            };
            std::erase_if(tool_defs, [](const ToolDefinition& td) {
                return !plan_allowed.count(td.name);
            });
        }
        auto req = builder.build(messages, system, system_dynamic, tool_defs,
                                  model_id, provider_id);

        // Auto-compaction: if the context is approaching the model's window,
        // summarize the older portion of the conversation before sending the
        // request. Disabled when the window is unknown (0) or auto_compact is off.
        //
        // Token count source: prefer the provider's reported usage from the
        // previous step (prev_total_input) since it's exact. It starts at 0 and
        // is set after each step — it is NOT reset between turns, so on step 0
        // of any turn after the first it still holds the previous turn's final
        // usage, which is accurate enough. The estimate fallback only fires on
        // the very first step of the very first turn, where no usage exists.
        if (config_.auto_compact) {
            int window = haicode::get_context_window(provider_id, model_id,
                                                     config_.model_contexts,
                                                     provider.get());
            if (window > 0) {
                int effective = window - config_.auto_compact_reserve;
                int threshold = static_cast<int>(effective * config_.auto_compact_threshold);
                int current_tokens = prev_total_input;
                if (current_tokens == 0) {
                    current_tokens = estimate_request_tokens(system, system_dynamic,
                                                             req.messages);
                }
                int lcs;
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    auto it = last_compaction_step_.find(session_id);
                    lcs = (it != last_compaction_step_.end()) ? it->second : -1;
                }
                if (should_compact_with_hysteresis(current_tokens, threshold,
                                                   step, lcs)) {
                    if (compact_history(session_id, *provider, model_id,
                                        provider_id, interrupt_flag,
                                        current_tokens, threshold)) {
                        {
                            std::lock_guard<std::mutex> lock(mu_);
                            last_compaction_step_[session_id] = step;
                        }
                        // Reload + rebuild so the compacted history is sent.
                        messages = store_.load_messages(session_id);
                        req = builder.build(messages, system, system_dynamic,
                                            tool_defs, model_id, provider_id);
                    }
                }
            }
        }

        // Apply per-session inference params (max_tokens / temperature / top_p /
        // reasoning_effort) stored in model_json. Defaults inside LLMRequest
        // win when not present.
        if (mj_now.is_object()) {
            if (mj_now.contains("max_tokens"))
                req.max_tokens = mj_now.value("max_tokens", req.max_tokens);
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

            auto result = tools_.execute(call.name, call.input, ctx, permissions_);

            // After a successful write or edit, run the configured build command
            // so the model sees compile errors immediately rather than discovering
            // them several steps later.
            if (result.success && !config_.build_command.empty()
                    && (call.name == "write" || call.name == "edit")) {
                FILE* bp = popen(config_.build_command.c_str(), "r");
                if (bp) {
                    std::string build_out;
                    std::array<char, 4096> buf;
                    while (fgets(buf.data(), buf.size(), bp))
                        build_out += buf.data();
                    int brc = pclose(bp);
                    int bec = WIFEXITED(brc) ? WEXITSTATUS(brc) : -1;

                    {
                        nlohmann::json bev;
                        bev["session_id"] = session_id;
                        bev["success"]    = (bec == 0);
                        bev["exit_code"]  = bec;
                        bus_.publish(events::EventType::BuildHookResult, bev);
                    }

                    if (bec != 0) {
                        result.output += "\n\n[build_hook] Build failed (exit "
                                       + std::to_string(bec) + "):\n" + build_out;
                        result.success = false;
                        result.error   = result.output;
                    }
                }
            }

            // Persist tool result
            nlohmann::json data;
            data["call_id"] = call.id;
            data["output"] = result.success ? result.output : result.error;
            data["success"] = result.success;
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
                            std::string tmp = plan_path + ".tmp_write";
                            if (std::ofstream out{tmp, std::ios::binary}) {
                                out.write(pcontent.data(), pcontent.size());
                                if (out.good()) rename(tmp.c_str(), plan_path.c_str());
                            }
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
    }

    // Loop exited because the step budget ran out (no break fired, so step == max_steps).
    // The last iteration would have left the UI in "tool_use ended, waiting for next step"
    // state with no follow-up ever coming — surface a clear message so the user isn't
    // left staring at silence.
    if (step == max_steps) {
        nlohmann::json ev;
        ev["session_id"] = session_id;
        ev["error"] = "Step limit reached (" + std::to_string(max_steps)
                     + "). Send another message to continue.";
        bus_.publish(events::EventType::StepFailed, ev);
    }

    // First-turn LLM title refinement. Fires at most once per session: gated
    // on exactly one user_prompted message (the initial prompt). Skipped for
    // resumed/continued sessions and disabled sessions. Best-effort — failures
    // leave the heuristic title in place.
    if (config_.autoname_sessions && config_.autoname_llm_refine) {
        int user_count = 0;
        for (const auto& m : store_.load_messages(session_id)) {
            if (m.type == "user_prompted") ++user_count;
        }
        if (user_count == 1) {
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

// First-pass summarization instruction. Prepended to the head slice as a user
// turn; the model returns the summary text.
static const char* kCompactionSummaryPromptFirst =
    "Summarize the following conversation between a user and a coding assistant. "
    "Preserve: the user's goals and key decisions, file paths and identifiers "
    "mentioned, tool outcomes that affected the solution, any unresolved problems, "
    "and the current state of any work in progress. Be concise — aim for under "
    "1500 tokens. Write only the summary prose, no preamble.";

// Resegment prompt — used when the head already begins with a prior
// compaction summary. Tells the model to keep the old summary verbatim under
// "## Segment 1" and add a "## Segment 2" for the new messages.
static const char* kCompactionSummaryPromptResegment =
    "The conversation history below begins with a prior summary written during "
    "an earlier compaction. Preserve that prior summary VERBATIM under a "
    "`## Segment 1` heading. Then summarize the remaining new messages under a "
    "`## Segment 2` heading, following the same preservation rules: user goals "
    "and decisions, file paths and identifiers, tool outcomes, unresolved "
    "problems, current state. Be concise — aim for under 1500 tokens total. "
    "Write only the two-segment summary, no preamble.";

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

const char* select_summary_prompt(const std::vector<SessionMessage>& head) {
    if (!head.empty() && head.front().type == "compaction_summary")
        return kCompactionSummaryPromptResegment;
    return kCompactionSummaryPromptFirst;
}

bool SessionEngine::compact_history(const std::string& session_id,
                                     Provider& provider,
                                     const std::string& model_id,
                                     const std::string& /*provider_id*/,
                                     std::atomic<bool>* interrupt_flag,
                                     int prev_input_tokens,
                                     int threshold_tokens)
{
    auto messages = store_.load_messages(session_id);
    if (messages.size() < 4) return false;  // too little to compact

    // Pick the boundary. Multi-turn: at the last user_prompted (if any). Else:
    // single-turn fallback that keeps the last K round-trips intact.
    int tail_start_seq = choose_tail_start_seq(messages);
    if (tail_start_seq < 0) return false;

    std::vector<SessionMessage> head;
    int messages_before = 0;
    for (auto& m : messages) {
        if (m.seq < tail_start_seq) { head.push_back(m); ++messages_before; }
    }
    if (head.empty()) return false;

    {
        nlohmann::json ev;
        ev["session_id"]         = session_id;
        ev["prev_input_tokens"]  = prev_input_tokens;
        ev["threshold"]          = threshold_tokens;
        ev["messages_before"]    = messages_before;
        bus_.publish(events::EventType::CompactionStarted, ev);
    }

    // Build the head into provider messages and prepend the summarization
    // instruction as the first user turn. Pick the resegment prompt if the
    // head already starts with a prior summary, so the model preserves it
    // verbatim under "## Segment 1" instead of paraphrasing it again.
    ContextBuilder builder;
    auto assembled = builder.assemble_messages(head);
    nlohmann::json instr;
    instr["role"] = "user";
    instr["content"] = select_summary_prompt(head);
    assembled.insert(assembled.begin(), instr);

    LLMRequest req;
    req.model_id    = model_id;
    req.system      = "You are a precise conversation summarizer.";
    req.messages    = std::move(assembled);
    req.max_tokens  = 2048;

    std::string summary;
    TokenUsage sum_usage;
    bool summary_failed = false;
    std::string summary_error;

    StreamCallbacks cbs;
    cbs.on_text_delta = [&](const std::string& /*tid*/, const std::string& delta) {
        summary += delta;
    };
    cbs.on_finish = [&](FinishReason /*reason*/, TokenUsage tok,
                        std::vector<ToolCall> /*calls*/) {
        sum_usage = tok;
    };
    cbs.on_error = [&](const std::string& error) {
        summary_failed = true;
        summary_error = error;
    };

    provider.stream(req, cbs);

    // If the user interrupted mid-summary, abort without persisting.
    if (interrupt_flag && interrupt_flag->load()) return false;

    // If summarization failed, don't compact — better to fail open and let the
    // next step's request proceed (the provider will reject it if truly over
    // the limit, surfacing a StepFailed the user can act on).
    if (summary_failed || summary.empty()) {
        fprintf(stderr, "[engine] compaction skipped: summary %s (error=%s)\n",
                summary.empty() ? "was empty" : "failed",
                summary_error.c_str());
        return false;
    }

    // Persist: stored as raw summary text. `assemble_messages` wraps it as an
    // assistant turn with a fixed "Here is a summary of the prior conversation:"
    // lead-in when sending to the provider, so the on-disk text stays clean
    // for resegmentation (Fix 4) and scrollback rendering.
    nlohmann::json data;
    data["role"] = "assistant";
    data["text"] = summary;
    store_.compact_messages(session_id, tail_start_seq, data.dump());

    {
        nlohmann::json ev;
        ev["session_id"]      = session_id;
        ev["summary_tokens"]  = sum_usage.output;
        ev["messages_before"] = messages.size();
        ev["messages_after"]  = (messages.size() - messages_before) + 1;
        bus_.publish(events::EventType::CompactionEnded, ev);
    }

    return true;
}

void SessionEngine::refine_title_llm(const std::string& session_id,
                                      Provider& provider,
                                      const std::string& model_id)
{
    // Load the first user_prompted message — the seed for the title.
    auto messages = store_.load_messages(session_id);
    std::string first_user;
    for (const auto& m : messages) {
        if (m.type != "user_prompted") continue;
        auto j = nlohmann::json::parse(m.data_json, nullptr, false);
        if (j.is_object()) first_user = j.value("text", "");
        break;
    }
    if (first_user.empty()) return;

    // Clamp very long prompts so the summarization prompt stays cheap.
    if (first_user.size() > 2000) first_user = first_user.substr(0, 2000) + "…";

    nlohmann::json user_msg;
    user_msg["role"] = "user";
    user_msg["content"] =
        "Write a concise session title (at most 6 words, no quotes, no "
        "trailing punctuation) summarizing what the user wants in this "
        "prompt. Respond with the title and nothing else.\n\n"
        "Prompt:\n" + first_user;

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

    store_.update_title(session_id, title);
    nlohmann::json ev;
    ev["session_id"] = session_id;
    ev["title"] = title;
    bus_.publish(events::EventType::SessionRenamed, ev);
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
            compact_history(session_id, *provider, model_id, provider_id,
                            nullptr, 0, 0);
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
