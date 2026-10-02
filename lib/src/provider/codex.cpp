#include <haicode/provider.h>
#include <haicode/util.h>
#include <haicode/codex_auth.h>
#include <haicode/codex_params.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <algorithm>
#include <chrono>
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
// Message translation (translate_to_responses_items in codex_params.cpp)
// mirrors translate_messages() in openai.cpp (same Anthropic-shaped context
// input) but emits Responses-API items:
//   user text      → {role:"user", content: string | [input_text,input_image]}
//   assistant text → {role:"assistant", content: string}
//   tool_use       → {type:"function_call", call_id, name, arguments}
//   tool_result    → {type:"function_call_output", call_id, output}
//   openai_reasoning → {type:"reasoning", summary, encrypted_content}
//                    (captured from this backend's output, replayed as-is)
//   thinking       → dropped (Anthropic signatures mean nothing here)
// ---------------------------------------------------------------------------

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
    std::string kind() const override { return "chatgpt"; }
    bool replays_reasoning_items() const override { return true; }

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

        // ---- Build request body (cache layout: see codex_params.h) ----
        const nlohmann::json body = build_codex_body(request);

        // replace handler: see anthropic.cpp — degrade, never throw, on any
        // stray invalid UTF-8 byte.
        const std::string body_str = body.dump(-1, ' ', false,
            nlohmann::json::error_handler_t::replace);

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
        // Replayable reasoning items, delivered just before on_finish so a
        // failed or interrupted attempt never reports partial reasoning.
        std::vector<nlohmann::json> reasoning_items;

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
            reasoning_items.clear();
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
                            if (e == "response.output_item.done"
                                    && item.value("type", "") == "reasoning") {
                                // Finished reasoning item: hand the
                                // replayable form to the engine so the next
                                // step can continue the chain (store:false
                                // keeps nothing server-side).
                                auto r = codex_reasoning_item_for_replay(item);
                                if (!r.is_null())
                                    reasoning_items.push_back(std::move(r));
                            } else if (item.value("type", "") == "function_call") {
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
                                const int total_input =
                                    std::max(0, u.value("input_tokens", 0));
                                usage.output = u.value("output_tokens", 0);
                                usage.cache_read = 0;
                                usage.cache_write = 0;
                                if (u.contains("input_tokens_details")
                                        && u["input_tokens_details"].is_object())
                                    usage.cache_read = std::clamp(
                                        u["input_tokens_details"].value(
                                            "cached_tokens", 0), 0, total_input);
                                usage.input = total_input - usage.cache_read;
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

        if (callbacks.on_reasoning_item)
            for (auto& r : reasoning_items) callbacks.on_reasoning_item(r);
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
        return fetch_catalog(error, 60);
    }

    // The context meter and auto-compaction need the catalog's per-model
    // window. list_models() only runs when the GUI fetches this provider's
    // models — not after a Settings save recreates the provider while it
    // isn't the default — and the prefix-table fallback can overstate the
    // backend's window several-fold, which effectively disables
    // auto-compaction (every step then resends the whole, growing history).
    // So a cache miss loads the catalog here, once, with a short timeout and
    // a backoff after failures.
    int get_model_context(const std::string& model_id) const override {
        if (int ctx = peek_model_context(model_id); ctx > 0) return ctx;
        std::lock_guard<std::mutex> fetch_lock(catalog_fetch_mu_);
        if (int ctx = peek_model_context(model_id); ctx > 0) return ctx;
        if (catalog_loaded_) return 0;  // model genuinely absent
        auto now = std::chrono::steady_clock::now();
        if (catalog_attempted_
                && now - last_catalog_attempt_ < std::chrono::minutes(5))
            return 0;
        catalog_attempted_ = true;
        last_catalog_attempt_ = now;
        std::string err;
        fetch_catalog(err, 15);
        if (!err.empty())
            fprintf(stderr, "[codex] context discovery failed: %s\n",
                    err.c_str());
        return peek_model_context(model_id);
    }

    int peek_model_context(const std::string& model_id) const override {
        std::lock_guard<std::mutex> lock(context_cache_mu_);
        auto it = context_cache_.find(model_id);
        return it != context_cache_.end() ? it->second : 0;
    }

private:
    std::vector<std::string> fetch_catalog(std::string& error,
                                           long timeout_s) const {
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
        std::string body = http_.get(models_url(), headers, timeout_s, &code);
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
                body = http_.get(models_url(), headers, timeout_s, &code);
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
        catalog_loaded_ = true;
        return result;
    }

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
    // Lazy catalog load state for get_model_context (guarded by
    // catalog_fetch_mu_, except catalog_loaded_ which list_models may set
    // from another thread).
    mutable std::mutex catalog_fetch_mu_;
    mutable std::atomic<bool> catalog_loaded_{false};
    mutable bool catalog_attempted_ = false;
    mutable std::chrono::steady_clock::time_point last_catalog_attempt_;

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
