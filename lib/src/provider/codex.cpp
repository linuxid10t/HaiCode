#include <haicode/provider.h>
#include <haicode/util.h>
#include <haicode/codex_auth.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

namespace haicode {

// ---------------------------------------------------------------------------
// ChatGPT "Sign in with ChatGPT" provider (subscription-billed Codex backend).
//
// The OAuth access token from codex_auth is audience-locked to
// https://chatgpt.com/backend-api/codex/responses — the Responses API, not
// chat/completions — so this is a separate provider from OpenAIProvider:
//
//   POST {base}/codex/responses
//   headers: Authorization, chatgpt-account-id, originator,
//            OpenAI-Beta: responses=experimental
//   body:    instructions / input items / flat tools / stream:true
//
// Message translation mirrors translate_messages() in openai.cpp (same
// Anthropic-shaped context input) but emits Responses-API items:
//   user text      → {role:"user", content: string | [input_text,input_image]}
//   assistant text → {role:"assistant", content: string}
//   tool_use       → {type:"function_call", call_id, name, arguments}
//   tool_result    → {type:"function_call_output", call_id, output}
//   thinking       → dropped (we never stored encrypted reasoning content)
// ---------------------------------------------------------------------------

static std::vector<nlohmann::json> translate_to_responses_items(
    const std::vector<nlohmann::json>& src)
{
    std::vector<nlohmann::json> out;

    for (auto& m : src) {
        std::string role = m.value("role", "");
        if (!m.contains("content")) continue;
        auto& content = m["content"];

        if (role == "assistant") {
            if (content.is_string()) {
                std::string text = content.get<std::string>();
                if (!text.empty())
                    out.push_back({{"role", "assistant"}, {"content", text}});
                continue;
            }
            if (!content.is_array()) continue;
            // Text preamble first (the common shape), then one function_call
            // item per tool_use block. Thinking blocks are dropped: we don't
            // keep the encrypted reasoning content the API would need to
            // continue a reasoning chain across turns.
            std::string text_acc;
            std::vector<nlohmann::json> calls;
            for (auto& block : content) {
                std::string btype = block.value("type", "");
                if (btype == "text") {
                    text_acc += block.value("text", "");
                } else if (btype == "tool_use") {
                    nlohmann::json item;
                    item["type"]      = "function_call";
                    item["call_id"]   = block.value("id", "");
                    item["name"]      = block.value("name", "");
                    item["arguments"] = block.contains("input")
                        ? block["input"].dump() : "{}";
                    calls.push_back(item);
                } else if (btype != "thinking") {
                    fprintf(stderr, "codex: dropping unknown assistant "
                            "content block type '%s'\n", btype.c_str());
                }
            }
            if (!text_acc.empty())
                out.push_back({{"role", "assistant"}, {"content", text_acc}});
            for (auto& c : calls) out.push_back(c);
            continue;
        }

        // User (or tool-result-carrying) messages.
        if (content.is_string()) {
            out.push_back({{"role", "user"}, {"content", content}});
            continue;
        }
        if (!content.is_array()) continue;

        // function_call_output items must directly follow their calls, so
        // they are emitted before the user text/images follow-up.
        std::vector<nlohmann::json> outputs;
        std::string user_text;
        nlohmann::json image_parts = nlohmann::json::array();
        auto harvest_image = [&image_parts](const nlohmann::json& block) {
            auto src_it = block.find("source");
            if (src_it == block.end() || !src_it->is_object()) {
                fprintf(stderr, "codex: image block without source object, "
                        "dropping\n");
                return;
            }
            image_parts.push_back({
                {"type", "input_image"},
                {"image_url", "data:" + src_it->value("media_type", "image/png")
                              + ";base64," + src_it->value("data", "")}
            });
        };
        for (auto& block : content) {
            std::string btype = block.value("type", "");
            if (btype == "tool_result") {
                std::string output_text;
                auto& bc = block["content"];
                if (bc.is_string()) {
                    output_text = bc.get<std::string>();
                } else if (bc.is_array()) {
                    for (auto& sub : bc) {
                        if (sub.is_string())
                            output_text += sub.get<std::string>();
                        else if (sub.value("type", "") == "text")
                            output_text += sub.value("text", "");
                        else if (sub.value("type", "") == "image")
                            harvest_image(sub);
                        else
                            fprintf(stderr, "codex: dropping unsupported "
                                    "tool_result content block type '%s'\n",
                                    sub.value("type", "(none)").c_str());
                    }
                }
                nlohmann::json item;
                item["type"]    = "function_call_output";
                item["call_id"] = block.value("tool_use_id", "");
                item["output"]  = output_text;
                outputs.push_back(item);
            } else if (btype == "text") {
                if (!user_text.empty()) user_text += "\n";
                user_text += block.value("text", "");
            } else if (btype == "image") {
                harvest_image(block);
            } else {
                fprintf(stderr, "codex: dropping unknown user content block "
                        "type '%s'\n", btype.c_str());
            }
        }
        for (auto& o : outputs) out.push_back(o);
        if (!image_parts.empty()) {
            // Mixed content must stay an array (text + input_image parts).
            nlohmann::json uc = nlohmann::json::array();
            if (!user_text.empty())
                uc.push_back({{"type", "input_text"}, {"text", user_text}});
            for (auto& ip : image_parts) uc.push_back(ip);
            out.push_back({{"role", "user"}, {"content", uc}});
        } else if (!user_text.empty()) {
            out.push_back({{"role", "user"}, {"content", user_text}});
        }
    }
    return out;
}

class CodexProvider : public Provider {
public:
    explicit CodexProvider(const std::string& id = "chatgpt",
                           const std::string& base_url = "")
        : base_url_(base_url.empty() ? "https://chatgpt.com/backend-api"
                                     : base_url),
          id_(id.empty() ? "chatgpt" : id)
    {
        while (!base_url_.empty() && base_url_.back() == '/') base_url_.pop_back();
    }

    std::string id() const override { return id_; }

    void stream(const LLMRequest& request, StreamCallbacks callbacks,
                const std::string& stream_token = "") override {
        std::shared_ptr<std::atomic<bool>> flag =
            std::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard<std::mutex> lock(cancel_mu_);
            cancel_flags_[stream_token].push_back(flag);
        }
        struct FlagPop {
            CodexProvider* p;
            std::string token;
            std::shared_ptr<std::atomic<bool>> flag;
            ~FlagPop() {
                std::lock_guard<std::mutex> lock(p->cancel_mu_);
                auto it = p->cancel_flags_.find(token);
                if (it == p->cancel_flags_.end()) return;
                auto& v = it->second;
                for (auto vit = v.begin(); vit != v.end(); ++vit)
                    if (vit->get() == flag.get()) { v.erase(vit); break; }
                if (v.empty()) p->cancel_flags_.erase(it);
            }
        } pop{this, stream_token, flag};

        // ---- Build request body ----
        nlohmann::json body;
        body["model"]  = request.model_id;
        body["stream"] = true;
        // Codex sends store:false — conversation history lives client-side.
        body["store"] = false;

        // Instructions = stable system prompt (+ dynamic tail appended, so
        // the cacheable prefix stays byte-identical across steps).
        std::string instructions = request.system;
        if (!request.system_dynamic.empty()) {
            if (!instructions.empty()) instructions += "\n\n";
            instructions += request.system_dynamic;
        }
        if (!instructions.empty())
            body["instructions"] = instructions;

        if (request.max_tokens)
            body["max_output_tokens"] = *request.max_tokens;
        // Temperature is ignored by this backend (verified by third-party
        // contract docs); omit rather than send a field it may reject.

        // "off" is not a valid Responses effort value — omit entirely.
        if (!request.reasoning_effort.empty()
                && request.reasoning_effort != "off")
            body["reasoning"] = {{"effort", request.reasoning_effort}};

        body["input"] = translate_to_responses_items(request.messages);

        if (!request.tools.empty()) {
            nlohmann::json tools_arr = nlohmann::json::array();
            for (auto& t : request.tools) {
                tools_arr.push_back({
                    {"type", "function"},
                    {"name", t.name},
                    {"description", t.description},
                    {"parameters", t.input_schema},
                });
            }
            body["tools"] = tools_arr;
            body["tool_choice"] = "auto";
            body["parallel_tool_calls"] = true;
        }

        const std::string body_str = body.dump();

        // ---- SSE parse state (reset per attempt) ----
        struct ToolCallState {
            std::string call_id;
            std::string name;
            std::string arguments;
        };
        std::string text_id = util::make_id("txt");
        FinishReason finish_reason = FinishReason::EndTurn;
        TokenUsage usage;
        bool error_occurred = false;
        bool any_tool_calls = false;

        long code = 0;
        std::string transport_err;
        // Tool-call accumulation. Declared outside the retry loop so the
        // post-loop materialisation can read it; cleared per attempt since a
        // 401-rejected attempt produced no usable partial output.
        std::map<std::string, ToolCallState> item_tool;
        std::vector<std::string> item_order;

        // One forced refresh-and-retry on 401 (token revoked/expired server
        // side between our skew refresh and this request).
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::string token, account_id, auth_err;
            if (!codex_auth_get_access(token, account_id, auth_err)) {
                if (callbacks.on_error)
                    callbacks.on_error(auth_err);
                return;
            }

            std::map<std::string, std::string> headers = {
                {"Content-Type",   "application/json"},
                {"Accept",         "text/event-stream"},
                {"Authorization",  "Bearer " + token},
                {"originator",     "codex_cli_rs"},
                {"OpenAI-Beta",    "responses=experimental"},
            };
            if (!account_id.empty())
                headers["chatgpt-account-id"] = account_id;

            item_tool.clear();
            item_order.clear();
            code = 0;
            transport_err.clear();

            http_.post_sse(base_url_ + "/codex/responses", headers, body_str,
                [&](const SSEEvent& ev) -> bool {
                    if (flag->load()) return false;
                    if (ev.data.empty()) return true;
                    auto d = nlohmann::json::parse(ev.data, nullptr, false);
                    if (d.is_discarded()) return true;
                    const std::string& e = ev.event;

                    if (e == "response.output_text.delta") {
                        std::string t = d.value("delta", "");
                        if (!t.empty() && callbacks.on_text_delta)
                            callbacks.on_text_delta(text_id, t);
                    } else if (e == "response.reasoning_summary_text.delta"
                               || e == "response.reasoning_text.delta") {
                        std::string t = d.value("delta", "");
                        if (!t.empty() && callbacks.on_reasoning_delta)
                            callbacks.on_reasoning_delta(t);
                    } else if (e == "response.output_item.added"
                               || e == "response.output_item.done") {
                        if (d.contains("item") && d["item"].is_object()) {
                            auto& item = d["item"];
                            if (item.value("type", "") == "function_call") {
                                std::string item_id = item.value("id", "");
                                auto ins = item_tool.emplace(item_id,
                                                             ToolCallState{});
                                if (ins.second)
                                    item_order.push_back(item_id);
                                auto& st = ins.first->second;
                                if (item.contains("call_id")
                                        && item["call_id"].is_string())
                                    st.call_id = item["call_id"].get<std::string>();
                                if (item.contains("name")
                                        && item["name"].is_string())
                                    st.name = item["name"].get<std::string>();
                                // The done event carries authoritative args.
                                if (e == "response.output_item.done"
                                        && item.contains("arguments")
                                        && item["arguments"].is_string())
                                    st.arguments =
                                        item["arguments"].get<std::string>();
                            }
                        }
                    } else if (e == "response.function_call_arguments.delta") {
                        std::string item_id = d.value("item_id", "");
                        auto ins = item_tool.emplace(item_id, ToolCallState{});
                        if (ins.second) item_order.push_back(item_id);
                        auto& st = ins.first->second;
                        std::string chunk = d.value("delta", "");
                        st.arguments += chunk;
                        if (!chunk.empty() && callbacks.on_tool_input_delta)
                            callbacks.on_tool_input_delta(st.call_id, st.name,
                                                          chunk);
                    } else if (e == "response.function_call_arguments.done") {
                        auto it = item_tool.find(d.value("item_id", ""));
                        if (it != item_tool.end() && d.contains("arguments")
                                && d["arguments"].is_string())
                            it->second.arguments =
                                d["arguments"].get<std::string>();
                    } else if (e == "response.completed"
                               || e == "response.incomplete") {
                        if (d.contains("response") && d["response"].is_object()) {
                            auto& r = d["response"];
                            if (r.contains("usage") && r["usage"].is_object()) {
                                auto& u = r["usage"];
                                usage.input  = u.value("input_tokens", 0);
                                usage.output = u.value("output_tokens", 0);
                                if (u.contains("input_tokens_details")
                                        && u["input_tokens_details"].is_object())
                                    usage.cache_read =
                                        u["input_tokens_details"].value(
                                            "cached_tokens", 0);
                                if (u.contains("output_tokens_details")
                                        && u["output_tokens_details"].is_object())
                                    usage.reasoning =
                                        u["output_tokens_details"].value(
                                            "reasoning_tokens", 0);
                            }
                            if (e == "response.incomplete")
                                finish_reason = FinishReason::MaxTokens;
                        }
                    } else if (e == "response.failed" || e == "error"
                               || e == "exception") {
                        std::string msg = "unknown error event";
                        if (d.contains("message") && d["message"].is_string())
                            msg = d["message"].get<std::string>();
                        else if (d.contains("response")
                                 && d["response"].is_object()
                                 && d["response"].contains("error")
                                 && d["response"]["error"].is_object())
                            msg = d["response"]["error"].value("message",
                                                               msg);
                        if (callbacks.on_error)
                            callbacks.on_error(util::sanitize_utf8(msg));
                        error_occurred = true;
                        return false;
                    }
                    return true;
                }, &code, &transport_err);

            if (flag->load()) return;

            if (code == 401 && attempt == 0) {
                std::string refresh_err;
                if (codex_auth_force_refresh(refresh_err))
                    continue;
                if (callbacks.on_error)
                    callbacks.on_error("session expired and refresh failed ("
                                       + refresh_err
                                       + ") — sign in with ChatGPT again");
                return;
            }
            break;
        }

        if (flag->load()) return;

        if (code == -1) {
            if (error_occurred) return;
            if (callbacks.on_error)
                callbacks.on_error(util::sanitize_utf8(
                    "connection failed: " + transport_err));
            return;
        }
        if (code >= 400) {
            if (error_occurred) return;
            std::string hint = code == 401
                ? " — try Sign in with ChatGPT again" : "";
            if (callbacks.on_error)
                callbacks.on_error(util::sanitize_utf8(
                    "HTTP " + std::to_string(code) + ": " + transport_err
                    + hint));
            return;
        }
        if (error_occurred) return;

        // Materialise accumulated tool calls (in output_item.added order).
        std::vector<ToolCall> tool_calls;
        for (auto& item_id : item_order) {
            auto& st = item_tool[item_id];
            ToolCall tc;
            tc.id        = st.call_id.empty() ? util::make_id("tc")
                                              : st.call_id;
            tc.name      = st.name;
            tc.raw_input = st.arguments;
            tc.input = nlohmann::json::parse(st.arguments, nullptr, false);
            if (tc.input.is_discarded()) {
                tc.input = nlohmann::json::object();
                tc.parse_failed = true;
                fprintf(stderr,
                        "[codex] tool_call %s (%s) arguments parse failed; "
                        "raw=%zu bytes: %.200s\n",
                        tc.id.c_str(), tc.name.c_str(),
                        st.arguments.size(), st.arguments.c_str());
            }
            tool_calls.push_back(std::move(tc));
        }
        any_tool_calls = !tool_calls.empty();

        if (finish_reason != FinishReason::MaxTokens)
            finish_reason = any_tool_calls ? FinishReason::ToolUse
                                           : FinishReason::EndTurn;

        if (callbacks.on_finish)
            callbacks.on_finish(finish_reason, usage, tool_calls);
    }

    void cancel(const std::string& stream_token = "") override {
        std::lock_guard<std::mutex> lock(cancel_mu_);
        size_t total = 0, matching = 0;
        for (auto& [token, flags] : cancel_flags_) {
            total += flags.size();
            if (token == stream_token) matching += flags.size();
        }
        if (stream_token.empty()) {
            for (auto& [token, flags] : cancel_flags_)
                for (auto& f : flags) f->store(true);
        } else {
            auto it = cancel_flags_.find(stream_token);
            if (it == cancel_flags_.end()) return;
            for (auto& f : it->second) f->store(true);
        }
        if (total == matching)
            http_.cancel();
    }

    std::vector<std::string> list_models(std::string& error) override {
        error.clear();
        std::vector<std::string> result;
        std::string token, account_id, auth_err;
        if (!codex_auth_get_access(token, account_id, auth_err)) {
            error = auth_err;
            return result;
        }
        std::map<std::string, std::string> headers = {
            {"accept", "application/json"},
            {"Authorization", "Bearer " + token},
        };
        if (!account_id.empty())
            headers["chatgpt-account-id"] = account_id;
        long code = 0;
        std::string body = http_.get(models_url(), headers, 60, &code);
        if (code == 401) {
            // Maybe the token aged out between get_access and this GET.
            std::string refresh_err;
            if (codex_auth_force_refresh(refresh_err)) {
                if (!codex_auth_get_access(token, account_id, refresh_err)) {
                    error = refresh_err;
                    return result;
                }
                headers["Authorization"] = "Bearer " + token;
                if (!account_id.empty())
                    headers["chatgpt-account-id"] = account_id;
                code = 0;
                body = http_.get(models_url(), headers, 60, &code);
            }
        }
        if (code == -1) {
            error = "connection failed contacting chatgpt.com";
            return result;
        }
        if (code != 200) {
            error = "models endpoint returned HTTP " + std::to_string(code);
            return result;
        }
        auto j = nlohmann::json::parse(body, nullptr, false);
        if (j.is_discarded()) {
            error = "invalid response from models endpoint";
            return result;
        }
        const nlohmann::json* arr = nullptr;
        if (j.is_object()) {
            if (j.contains("data") && j["data"].is_array()) arr = &j["data"];
            else if (j.contains("models") && j["models"].is_array())
                arr = &j["models"];
        } else if (j.is_array()) {
            arr = &j;
        }
        if (!arr) {
            error = "models response has no data array";
            return result;
        }
        for (auto& m : *arr) {
            if (m.is_string()) {
                result.push_back(m.get<std::string>());
            } else if (m.is_object()) {
                std::string mid;
                if (m.contains("id") && m["id"].is_string())
                    mid = m["id"].get<std::string>();
                else if (m.contains("slug") && m["slug"].is_string())
                    mid = m["slug"].get<std::string>();
                if (mid.empty()) continue;
                // The codex catalog carries per-model context windows; cache
                // them so the context meter works for models absent from the
                // hardcoded prefix table (same pattern as the OpenAI flavors).
                if (m.contains("context_window")
                        && m["context_window"].is_number()) {
                    int ctx = m["context_window"].get<int>();
                    if (ctx > 0) {
                        std::lock_guard<std::mutex> lock(context_cache_mu_);
                        context_cache_[mid] = ctx;
                    }
                }
                result.push_back(mid);
            }
        }
        return result;
    }

    int get_model_context(const std::string& model_id) const override {
        std::lock_guard<std::mutex> lock(context_cache_mu_);
        auto it = context_cache_.find(model_id);
        return it != context_cache_.end() ? it->second : 0;
    }

    int peek_model_context(const std::string& model_id) const override {
        std::lock_guard<std::mutex> lock(context_cache_mu_);
        auto it = context_cache_.find(model_id);
        return it != context_cache_.end() ? it->second : 0;
    }

private:
    std::string base_url_;
    std::string id_;
    mutable HttpClient http_;
    std::mutex cancel_mu_;
    std::map<std::string, std::vector<std::shared_ptr<std::atomic<bool>>>>
        cancel_flags_;
    // Context windows harvested from the models catalog (list_models may run
    // on a settings thread while the UI thread reads the active model's
    // window), so the cache is mutex-guarded.
    mutable std::mutex context_cache_mu_;
    mutable std::map<std::string, int> context_cache_;

    // The models endpoint rejects requests without a client_version query
    // (FastAPI "Field required" — HTTP 400).
    std::string models_url() const {
        return base_url_ + "/codex/models?client_version=1.0.0";
    }
};

std::shared_ptr<Provider> make_codex_provider(const std::string& id,
                                              const std::string& base_url) {
    return std::make_shared<CodexProvider>(id, base_url);
}

} // namespace haicode
