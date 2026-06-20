#include <haicode/provider.h>
#include <haicode/util.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <sstream>
#include <atomic>
#include <map>

namespace haicode {

class AnthropicProvider : public Provider {
public:
    explicit AnthropicProvider(const std::string& api_key,
                                const std::string& base_url = "https://api.anthropic.com/v1",
                                const std::string& id = "anthropic")
        : api_key_(api_key), base_url_(base_url), id_(id.empty() ? "anthropic" : id) {}

    std::string id() const override { return id_; }

    void stream(const LLMRequest& request, StreamCallbacks callbacks) override {
        cancelled_.store(false);

        nlohmann::json body;
        body["model"] = request.model_id;
        body["max_tokens"] = request.max_tokens;
        body["stream"] = true;
        if (request.temperature)
            body["temperature"] = *request.temperature;
        if (request.top_p)
            body["top_p"] = *request.top_p;

        // Reasoning effort → output_config.effort (adaptive thinking, Claude
        // 4.6+). "off" must be sent explicitly — omitting the param falls back
        // to the model default (thinking enabled), so "off" would not actually
        // disable thinking. "minimal" is not an Anthropic effort level; we let
        // it fall through (unset) to the model default like empty.
        if (request.reasoning_effort == "off"
            || request.reasoning_effort == "low"
            || request.reasoning_effort == "medium"
            || request.reasoning_effort == "high"
            || request.reasoning_effort == "xhigh"
            || request.reasoning_effort == "max") {
            body["output_config"] = {{"effort", request.reasoning_effort}};
        }

        // System — split into a stable cached block + an uncached dynamic
        // tail ({{STEPS_LEFT}}). The stable block carries cache_control so
        // Anthropic can prefix-cache it across turns.
        if (!request.system.empty()) {
            nlohmann::json arr = nlohmann::json::array();
            nlohmann::json stable_block;
            stable_block["type"] = "text";
            stable_block["text"] = request.system;
            stable_block["cache_control"] = {{"type", "ephemeral"}};
            arr.push_back(stable_block);
            if (!request.system_dynamic.empty()) {
                arr.push_back({{"type", "text"}, {"text", request.system_dynamic}});
            }
            body["system"] = arr;
        }

        // Messages — verbatim from the engine. cache_control on the last
        // block of the second-to-last message is added in a post-pass
        // below, so the conversation prefix (everything except the most
        // recent turn) hits the cache.
        body["messages"] = request.messages;

        // Tools — cache_control on the last entry. Stable across turns.
        if (!request.tools.empty()) {
            nlohmann::json tools_arr = nlohmann::json::array();
            for (auto& t : request.tools) {
                nlohmann::json tool;
                tool["name"] = t.name;
                tool["description"] = t.description;
                tool["input_schema"] = t.input_schema;
                tools_arr.push_back(tool);
            }
            tools_arr.back()["cache_control"] = {{"type", "ephemeral"}};
            body["tools"] = tools_arr;
        }

        // Conversation-prefix cache breakpoint. Mark the last block of the
        // second-to-last message so everything older than the current turn
        // is cached. Requires the message to expose a content array; if the
        // message's content is a plain string, promote it to a one-element
        // text-block array. Skip on messages that can't be normalized.
        auto& msgs = body["messages"];
        if (msgs.is_array() && msgs.size() >= 2) {
            auto& target = msgs[msgs.size() - 2];
            if (target.is_object() && target.contains("content")) {
                auto& content = target["content"];
                if (content.is_string()) {
                    std::string s = content.get<std::string>();
                    content = nlohmann::json::array({
                        {{"type", "text"}, {"text", s}}
                    });
                }
                if (content.is_array() && !content.empty()) {
                    content.back()["cache_control"] = {{"type", "ephemeral"}};
                }
            }
        }

        std::map<std::string, std::string> headers = {
            {"x-api-key", api_key_},
            {"anthropic-version", "2023-06-01"},
            {"content-type", "application/json"},
            {"accept", "text/event-stream"}
        };

        // SSE parse state
        struct ParseState {
            std::string current_block_type;
            std::string current_tool_call_id;
            std::string current_tool_name;
            std::string accumulated_tool_input;
            std::string current_text_id;
            int current_block_index = -1;
            std::vector<ToolCall> tool_calls;
            FinishReason finish_reason = FinishReason::EndTurn;
            TokenUsage usage;
            std::string finish_str;
        };

        ParseState state;
        std::string body_str = body.dump();
        bool error_occurred = false;

        http_.post_sse(base_url_ + "/messages", headers, body_str,
            [&](const SSEEvent& ev) -> bool {
                if (cancelled_.load()) return false;
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
                        } else if (dtype == "thinking_delta") {
                            std::string text = delta.value("thinking", "");
                            if (!text.empty() && callbacks.on_reasoning_delta)
                                callbacks.on_reasoning_delta(text);
                        }
                    } else if (etype == "content_block_stop") {
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
            });

        if (!error_occurred && callbacks.on_finish)
            callbacks.on_finish(state.finish_reason, state.usage, state.tool_calls);
    }

    void cancel() override {
        cancelled_.store(true);
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
            if (j.is_discarded() || !j.contains("data")) {
                if (body.empty())
                    error = "empty response from server";
                else
                    error = "invalid response from server";
                return result;
            }
            for (auto& m : j["data"]) {
                std::string mid = m.value("id", "");
                if (!mid.empty()) result.push_back(mid);
            }
        } catch (...) {
            error = "invalid response from server";
        }
        return result;
    }

private:
    std::string api_key_;
    std::string base_url_;
    std::string id_;
    HttpClient http_;
    std::atomic<bool> cancelled_{false};
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
