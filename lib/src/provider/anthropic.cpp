#include <haicode/provider.h>
#include <haicode/util.h>
#include <haicode/anthropic_params.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <sstream>
#include <atomic>
#include <map>
#include <set>
#include <mutex>
#include <memory>

namespace haicode {

class AnthropicProvider : public Provider {
public:
    explicit AnthropicProvider(const std::string& api_key,
                                const std::string& base_url = "https://api.anthropic.com/v1",
                                const std::string& id = "anthropic")
        : api_key_(api_key), base_url_(base_url), id_(id.empty() ? "anthropic" : id) {}

    std::string id() const override { return id_; }
    std::string kind() const override { return "anthropic"; }

    void stream(const LLMRequest& request, StreamCallbacks callbacks,
                const std::string& stream_token = "") override {
        // Per-request cancel flag registered under stream_token. An empty
        // token can't be cancelled individually (shutdown uses cancel("")
        // which sweeps everything, including this entry).
        std::shared_ptr<std::atomic<bool>> flag =
            std::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard<std::mutex> lock(cancel_mu_);
            cancel_flags_[stream_token].push_back(flag);
        }
        // Pop this request's entry before returning — including the early
        // `return`s below — so tokens don't leak entries across runs and a
        // finished run can't be cancelled by a later token match.
        struct FlagPop {
            AnthropicProvider* p;
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

        // Body construction lives in build_anthropic_body() (pure, testable):
        // capability-mapped effort, adaptive thinking config, sampling-param
        // gating, cache breakpoints.
        nlohmann::json body = build_anthropic_body(request);

        // SSE parse state
        struct ParseState {
            std::string current_block_type;
            std::string current_tool_call_id;
            std::string current_tool_name;
            std::string accumulated_tool_input;
            std::string current_text_id;
            int current_block_index = -1;
            ThinkingBlockAcc current_thinking;
            std::vector<ToolCall> tool_calls;
            FinishReason finish_reason = FinishReason::EndTurn;
            TokenUsage usage;
            std::string finish_str;
        };

        ParseState state;
        // replace handler: a stray invalid byte (post-truncation edge cases)
        // degrades to U+FFFD instead of throwing type_error.316 off the
        // runner thread.
        std::string body_str = body.dump(-1, ' ', false,
            nlohmann::json::error_handler_t::replace);
        bool error_occurred = false;

        std::map<std::string, std::string> headers = {
            {"x-api-key", api_key_},
            {"anthropic-version", "2023-06-01"},
            {"content-type", "application/json"},
            {"accept", "text/event-stream"}
        };

        long code = 0;
        std::string transport_err;
        http_.post_sse(base_url_ + "/messages", headers, body_str,
            [&](const SSEEvent& ev) -> bool {
                if (flag->load()) return false;
                if (ev.data == "[DONE]") return true;
                if (ev.data.empty()) return true;

                try {
                    auto d = nlohmann::json::parse(ev.data, nullptr, false);
                    if (d.is_discarded()) return true;

                    // Check for error
                    if (d.contains("type") && d["type"] == "error") {
                        std::string msg = d.value("error", nlohmann::json{})
                                          .value("message", "Unknown Anthropic error");
                        if (callbacks.on_error) callbacks.on_error(msg);
                        error_occurred = true;
                        return false;
                    }

                    const std::string etype = ev.event;

                    if (etype == "content_block_start") {
                        auto& block = d["content_block"];
                        std::string btype = block.value("type", "");
                        state.current_block_type = btype;
                        state.current_block_index = d.value("index", 0);

                        if (btype == "text") {
                            state.current_text_id = util::make_id("txt");
                        } else if (btype == "tool_use") {
                            state.current_tool_call_id = block.value("id", "");
                            state.current_tool_name = block.value("name", "");
                            state.accumulated_tool_input.clear();
                        } else if (btype == "thinking") {
                            state.current_thinking = ThinkingBlockAcc{};
                        }
                    } else if (etype == "content_block_delta") {
                        auto& delta = d["delta"];
                        std::string dtype = delta.value("type", "");

                        if (dtype == "text_delta") {
                            std::string text = delta.value("text", "");
                            if (callbacks.on_text_delta)
                                callbacks.on_text_delta(state.current_text_id, text);
                        } else if (dtype == "input_json_delta") {
                            std::string partial = delta.value("partial_json", "");
                            state.accumulated_tool_input += partial;
                            if (callbacks.on_tool_input_delta)
                                callbacks.on_tool_input_delta(
                                    state.current_tool_call_id,
                                    state.current_tool_name,
                                    partial);
                        } else if (dtype == "thinking_delta"
                                   || dtype == "signature_delta") {
                            // Signature fragments must accumulate alongside the
                            // thinking text: the (thinking, signature) pair is
                            // replayed verbatim in later tool-loop turns.
                            state.current_thinking.apply_delta(delta);
                            std::string text = delta.value("thinking", "");
                            if (dtype == "thinking_delta"
                                    && !text.empty()
                                    && callbacks.on_reasoning_delta)
                                callbacks.on_reasoning_delta(text);
                        }
                    } else if (etype == "content_block_stop") {
                        if (state.current_block_type == "thinking") {
                            // Deliver even with empty thinking text: signature-
                            // only blocks (display:"omitted") must still round-
                            // trip in tool loops.
                            if (callbacks.on_thinking_block)
                                callbacks.on_thinking_block(
                                    state.current_thinking.thinking,
                                    state.current_thinking.signature);
                            state.current_thinking = ThinkingBlockAcc{};
                        }
                        if (state.current_block_type == "tool_use") {
                            ToolCall tc;
                            tc.id = state.current_tool_call_id;
                            tc.name = state.current_tool_name;
                            tc.raw_input = state.accumulated_tool_input;
                            try {
                                tc.input = nlohmann::json::parse(
                                    state.accumulated_tool_input, nullptr, false);
                                if (tc.input.is_discarded()) {
                                    tc.input = nlohmann::json::object();
                                    tc.parse_failed = true;
                                    fprintf(stderr,
                                        "[anthropic] tool_use %s (%s) input parse failed; "
                                        "raw=%zu bytes: %.200s\n",
                                        tc.id.c_str(), tc.name.c_str(),
                                        state.accumulated_tool_input.size(),
                                        state.accumulated_tool_input.c_str());
                                }
                            } catch (...) {
                                tc.input = nlohmann::json::object();
                                tc.parse_failed = true;
                                fprintf(stderr,
                                    "[anthropic] tool_use %s (%s) input parse threw; "
                                    "raw=%zu bytes: %.200s\n",
                                    tc.id.c_str(), tc.name.c_str(),
                                    state.accumulated_tool_input.size(),
                                    state.accumulated_tool_input.c_str());
                            }
                            state.tool_calls.push_back(tc);
                        }
                        state.current_block_type.clear();
                    } else if (etype == "message_delta") {
                        auto& delta = d["delta"];
                        state.finish_str = delta.value("stop_reason", "end_turn");
                        if (state.finish_str == "tool_use")
                            state.finish_reason = FinishReason::ToolUse;
                        else if (state.finish_str == "max_tokens")
                            state.finish_reason = FinishReason::MaxTokens;

                        if (d.contains("usage")) {
                            state.usage.output = d["usage"].value("output_tokens", 0);
                        }
                    } else if (etype == "message_start") {
                        if (d.contains("message") && d["message"].contains("usage")) {
                            state.usage.input = d["message"]["usage"].value("input_tokens", 0);
                            state.usage.cache_read = d["message"]["usage"].value("cache_read_input_tokens", 0);
                            state.usage.cache_write = d["message"]["usage"].value("cache_creation_input_tokens", 0);
                        }
                    }
                } catch (...) {}
                return true;
            }, &code, &transport_err);

        // Interrupt: the consumer cancelled — stay quiet, no error event.
        if (flag->load()) return;

        // Transport failure / HTTP error: report the real cause instead of
        // finishing with an empty assistant turn (review #8).
        if (code == -1) {
            if (callbacks.on_error)
                callbacks.on_error(util::sanitize_utf8(
                    "connection failed: " + transport_err +
                    " (check base_url / network)"));
            return;
        }
        if (code >= 400) {
            if (callbacks.on_error)
                callbacks.on_error(util::sanitize_utf8(
                    "HTTP " + std::to_string(code) + ": " + transport_err));
            return;
        }

        if (!error_occurred && callbacks.on_finish)
            callbacks.on_finish(state.finish_reason, state.usage, state.tool_calls);
    }

    void cancel(const std::string& stream_token = "") override {
        // Token-scoped: only the matching streams' flags are set — their
        // write_cb returns 0 at the next chunk and curl aborts that transfer.
        // http_.cancel() would abort every transfer on the shared client
        // (crossing sessions), so it fires only when this provider has no
        // other stream in flight; otherwise the abort rides the next SSE
        // chunk (providers emit keepalives/pings during generation).
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
        std::map<std::string, std::string> headers = {
            {"x-api-key",         api_key_},
            {"anthropic-version", "2023-06-01"},
            {"accept",            "application/json"},
        };
        std::string url = base_url_ + "/models";
        long code = 0;
        std::string body = http_.get(url, headers, 60, &code);
        std::vector<std::string> result;
        if (code == -1) {
            error = "connection failed (check base_url / network)";
            return result;
        }
        if (code == 401 || code == 403) {
            error = "authentication failed (check api_key)";
            return result;
        }
        if (code == 404) {
            error = "models endpoint not found at " + url +
                    " — check base_url (it must include the API version path, "
                    "e.g. .../v1); some Anthropic-compatible proxies (e.g. Z.ai "
                    "devpack) do not expose a models list, so enter the model "
                    "id manually";
            return result;
        }
        if (code >= 400) {
            error = "HTTP " + std::to_string(code);
            return result;
        }
        try {
            auto j = nlohmann::json::parse(body, nullptr, false);
            std::vector<AnthropicModelEntry> entries;
            if (j.is_discarded() || !parse_anthropic_models(j, entries)) {
                if (body.empty())
                    error = "empty response from server";
                else
                    error = "invalid response from server";
                return result;
            }
            for (auto& e : entries) {
                // Cache the discovered limits so the context meter and
                // auto-compaction work without config overrides (same
                // pattern as the OpenAI flavors/codex).
                if (e.max_input_tokens > 0) {
                    std::lock_guard<std::mutex> lock(context_cache_mu_);
                    context_cache_[e.id] = e.max_input_tokens;
                }
                result.push_back(e.id);
            }
        } catch (...) {
            error = "invalid response from server";
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
    std::string api_key_;
    std::string base_url_;
    std::string id_;
    HttpClient http_;
    // Per-stream cancel flags keyed by stream_token ("" included). Guarded
    // by cancel_mu_; stream() registers its flag on entry and pops it before
    // returning, so entries are always in-flight streams.
    std::mutex cancel_mu_;
    std::map<std::string, std::vector<std::shared_ptr<std::atomic<bool>>>> cancel_flags_;
    // Discovered id -> max_input_tokens from the Models API; guarded by
    // context_cache_mu_. Populated during list_models().
    mutable std::mutex context_cache_mu_;
    mutable std::map<std::string, int> context_cache_;
};

// Factory function
std::shared_ptr<Provider> make_anthropic_provider(const std::string& api_key,
                                                   const std::string& base_url,
                                                   const std::string& id) {
    if (base_url.empty())
        return std::make_shared<AnthropicProvider>(api_key, "https://api.anthropic.com/v1", id);
    std::string url = base_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    return std::make_shared<AnthropicProvider>(api_key, url, id);
}

} // namespace haicode
