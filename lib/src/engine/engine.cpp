#include <haicode/engine.h>
#include <haicode/util.h>
#include <haicode/default_prompt.h>
#include <haicode/pricing.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <sys/utsname.h>
#include <cstdio>
#include <algorithm>

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
                                 const std::string& project_dir,
                                 int steps_left) {
    std::string out = tmpl;
    substitute_all(out, "{{MODEL}}", model);
    substitute_all(out, "{{OS}}", os_info);
    substitute_all(out, "{{PROJECT_DIR}}", project_dir);
    substitute_all(out, "{{STEPS_LEFT}}", std::to_string(steps_left));
    return out;
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

std::vector<Todo> SessionEngine::get_todos(const std::string& session_id) {
    return store_.load_todos(session_id);
}

SessionMode SessionEngine::get_mode(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = session_modes_.find(session_id);
    if (it != session_modes_.end()) return it->second;
    return SessionMode::Build;
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
            std::string mode_str = model_json.value("mode", "build");
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
                       + instructions_block
                       + plan_mode_block;

    // Dynamic per-step content ({{STEPS_LEFT}}). Emitted as a separate
    // system text block by the Anthropic provider so the stable body
    // above stays byte-identical across turns and hits the prefix cache.
    std::string system_dynamic = render_prompt(kDynamicSystemPrompt, model_id,
                                                os_info, session.directory,
                                                max_steps);

    fprintf(stderr, "[engine] session=%s dir='%s' agent=%s mode=%s max_steps=%d instructions=%zu\n[engine] system prompt:\n%s\n---\n",
            session_id.c_str(), session.directory.c_str(), session.agent.c_str(),
            mode == SessionMode::Plan ? "plan" : "build",
            max_steps, config_.instructions.size(), system.c_str());
    fflush(stderr);

    // Agentic loop
    int step = 0;
    for (; step < max_steps; ++step) {
        if (interrupt_flag && interrupt_flag->load()) break;

        // Re-read model_id/provider_id from the session each step. The user
        // can change either via the dropdown mid-loop, and the system prompt
        // (re-rendered below) plus the outgoing request must reflect the new
        // values on the very next step.
        if (auto s_now = store_.get(session_id)) {
            auto mj_now = nlohmann::json::parse(s_now->model_json, nullptr, false);
            if (mj_now.is_object()) {
                if (auto v = mj_now.value("id", ""); !v.empty())        model_id    = v;
                if (auto v = mj_now.value("provider_id", ""); !v.empty()) provider_id = v;
            }
        }

        // Re-render the system prompt each step so {{MODEL}} and {{STEPS_LEFT}}
        // stay current.
        system = render_prompt(prompt_tmpl, model_id, os_info, session.directory,
                               max_steps - step) + agents_md_block + instructions_block
                              + plan_mode_block;
        // {{STEPS_LEFT}} decrements each step → re-render the dynamic block too.
        system_dynamic = render_prompt(kDynamicSystemPrompt, model_id, os_info,
                                        session.directory, max_steps - step);

        auto messages = store_.load_messages(session_id);

        ContextBuilder builder;
        auto tool_defs = tools_.definitions();
        // Filter tools by mode. In Plan mode the model gets only read-only
        // research tools plus propose_plan. In Build mode propose_plan is
        // hidden (it's only meaningful when planning).
        if (mode == SessionMode::Plan) {
            std::erase_if(tool_defs, [](const ToolDefinition& td) {
                return td.name == "bash" || td.name == "write"
                    || td.name == "edit" || td.name == "external_terminal";
            });
        } else {
            std::erase_if(tool_defs, [](const ToolDefinition& td) {
                return td.name == "propose_plan";
            });
        }
        auto req = builder.build(messages, system, system_dynamic, tool_defs,
                                  model_id, provider_id);

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

        // Split off tool calls whose streamed input failed to parse. We don't
        // execute them, and crucially we don't persist them — otherwise the
        // next iteration would send the model its own phantom empty tool_use
        // block, which is what causes the "stumble on empty toolcall" loop.
        bool had_parse_failure = false;
        for (auto it = tool_calls.begin(); it != tool_calls.end(); ) {
            if (it->parse_failed) {
                had_parse_failure = true;
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

        // Persist assistant turn (text and/or valid tool calls only)
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

        // Compute per-turn cost from token usage and resolved pricing.
        // Unknown models fall back to 0.0 — silent, no warning.
        const ModelPricing* price = lookup_pricing(provider_id, model_id,
                                                    config_.pricing);
        double step_cost = price ? compute_cost(usage, *price) : 0.0;

        // Update cost
        store_.update_cost(session_id, step_cost, usage);

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
}

} // namespace haicode
