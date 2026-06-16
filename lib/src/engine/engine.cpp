#include <haicode/engine.h>
#include <haicode/util.h>
#include <haicode/default_prompt.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <sys/utsname.h>
#include <cstdio>

namespace haicode {

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
                                 const std::string& project_dir) {
    std::string out = tmpl;
    substitute_all(out, "{{MODEL}}", model);
    substitute_all(out, "{{OS}}", os_info);
    substitute_all(out, "{{PROJECT_DIR}}", project_dir);
    return out;
}

LLMRequest ContextBuilder::build(
    const std::vector<SessionMessage>& messages,
    const std::string& system_prompt,
    const std::vector<ToolDefinition>& tools,
    const std::string& model_id,
    const std::string& /*provider_id*/)
{
    LLMRequest req;
    req.model_id = model_id;
    req.system = system_prompt;
    req.tools = tools;
    req.messages = assemble_messages(messages);
    return req;
}

std::vector<nlohmann::json> ContextBuilder::assemble_messages(
    const std::vector<SessionMessage>& msgs)
{
    std::vector<nlohmann::json> result;

    for (auto& msg : msgs) {
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
                    // Build Anthropic-style content array with text + tool_use blocks.
                    // OpenAI's translate_messages() converts this on the fly.
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
            } else if (msg.type == "tool_result") {
                nlohmann::json m;
                m["role"] = "user";
                nlohmann::json content;
                content["type"] = "tool_result";
                content["tool_use_id"] = data.value("call_id", "");
                content["content"] = data.value("output", "");
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
    lock.unlock();
    // Detach threads — they will finish on their own once the interrupt flag fires.
    // We cannot safely join here as threads may be blocked in network I/O; the caller
    // (HaiCodeApp::QuitRequested) calls exit() immediately after, so the OS cleans up.
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

void SessionEngine::interrupt(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = interrupt_flags_.find(session_id);
    if (it != interrupt_flags_.end() && it->second)
        it->second->store(true);

    nlohmann::json ev;
    ev["session_id"] = session_id;
    bus_.publish(events::EventType::InterruptRequested, ev);
}

void SessionEngine::agentic_loop(const std::string& session_id) {
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
        bus_.publish(events::EventType::StepFailed, ev);
        return;
    }

    auto* interrupt_flag = interrupt_flags_[session_id];
    std::string prompt_tmpl = kDefaultSystemPrompt;
    if (!config_.agents.empty()) {
        auto ait = config_.agents.find(session.agent);
        if (ait != config_.agents.end() && ait->second.system_prompt)
            prompt_tmpl = *ait->second.system_prompt;
    }

    std::string os_info;
    struct utsname uts {};
    if (uname(&uts) == 0)
        os_info = std::string(uts.sysname) + " " + uts.release
              + " (" + uts.machine + ")";

    std::string system = render_prompt(prompt_tmpl, model_id, os_info, session.directory);

    fprintf(stderr, "[engine] session=%s dir='%s'\n[engine] system prompt:\n%s\n---\n",
            session_id.c_str(), session.directory.c_str(), system.c_str());
    fflush(stderr);

    // Agentic loop
    for (int step = 0; step < 20; ++step) {
        if (interrupt_flag && interrupt_flag->load()) break;

        auto messages = store_.load_messages(session_id);

        ContextBuilder builder;
        auto tool_defs = tools_.definitions();
        auto req = builder.build(messages, system, tool_defs, model_id, provider_id);

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
        std::string text_id = haicode::util::make_id("txt");
        std::vector<ToolCall> tool_calls;
        FinishReason finish_reason = FinishReason::EndTurn;
        TokenUsage usage;
        bool step_failed = false;

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
            nlohmann::json ev;
            ev["session_id"] = session_id;
            ev["error"] = error;
            bus_.publish(events::EventType::StepFailed, ev);
        };

        provider->stream(req, cbs);

        if (step_failed) break;

        // Persist assistant turn (text and/or tool calls)
        if (!full_text.empty() || !tool_calls.empty()) {
            nlohmann::json data;
            data["role"] = "assistant";
            data["text"] = full_text;
            if (!tool_calls.empty()) {
                auto calls_arr = nlohmann::json::array();
                for (auto& tc : tool_calls)
                    calls_arr.push_back({{"id",tc.id},{"name",tc.name},{"input",tc.input}});
                data["tool_calls"] = calls_arr;
            }
            store_.append_message(session_id, "assistant_text", data.dump());
        }

        // Update cost
        store_.update_cost(session_id, 0.0, usage);

        // Publish step ended
        {
            nlohmann::json ev;
            ev["session_id"] = session_id;
            ev["assistant_message_id"] = assistant_msg_id;
            ev["finish_reason"] = (finish_reason == FinishReason::ToolUse) ? "tool_use" : "end_turn";
            bus_.publish(events::EventType::StepEnded, ev);
        }

        if (finish_reason != FinishReason::ToolUse || tool_calls.empty()) break;
        if (interrupt_flag && interrupt_flag->load()) break;

        // Execute tool calls
        bool any_denied = false;
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

            auto result = tools_.execute(call.name, call.input, ctx, permissions_);

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

            if (result.denied) { any_denied = true; break; }
            if (interrupt_flag && interrupt_flag->load()) break;
        }

        if (any_denied) {
            nlohmann::json ev;
            ev["session_id"] = session_id;
            ev["error"] = "Stopped: permission denied by user.";
            bus_.publish(events::EventType::StepFailed, ev);
            break;
        }
    }
}

} // namespace haicode
