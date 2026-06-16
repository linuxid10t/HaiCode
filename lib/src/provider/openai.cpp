#include <haicode/provider.h>
#include <haicode/util.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <map>
#include <string>

namespace haicode {

// ---------------------------------------------------------------------------
// Message format translation
//
// ContextBuilder emits messages in a loosely Anthropic-shaped format:
//   user    : { "role":"user",      "content": <string|array> }
//   asst    : { "role":"assistant", "content": <string> }
//   tool res: { "role":"user",      "content": [{"type":"tool_result",
//               "tool_use_id":"...", "content":"..."}] }
//
// OpenAI wants:
//   user    : { "role":"user",      "content": <string> }
//   asst    : { "role":"assistant", "content": <string> }
//   tool res: { "role":"tool",      "content": <string>,
//               "tool_call_id":"..." }
//
// We convert on the fly, dropping any message we can't translate cleanly.
// ---------------------------------------------------------------------------

static std::vector<nlohmann::json>
translate_messages(const std::string& system,
                   const std::vector<nlohmann::json>& src)
{
    std::vector<nlohmann::json> out;

    if (!system.empty())
        out.push_back({ {"role", "system"}, {"content", system} });

    for (auto& m : src) {
        std::string role = m.value("role", "");
        auto& content    = m["content"];

        // Plain string content — pass through
        if (content.is_string()) {
            out.push_back({ {"role", role}, {"content", content} });
            continue;
        }

        // Array content
        if (content.is_array()) {
            if (role == "assistant") {
                // Assistant content array: may contain text + tool_use blocks.
                // Convert to OpenAI format: content string + tool_calls array.
                std::string text_acc;
                nlohmann::json tc_arr = nlohmann::json::array();
                for (auto& block : content) {
                    std::string btype = block.value("type", "");
                    if (btype == "text") {
                        text_acc += block.value("text", "");
                    } else if (btype == "tool_use") {
                        nlohmann::json tc;
                        tc["id"]   = block.value("id", "");
                        tc["type"] = "function";
                        tc["function"] = {
                            {"name", block.value("name", "")},
                            {"arguments", block.contains("input")
                                ? block["input"].dump() : "{}"}
                        };
                        tc_arr.push_back(tc);
                    }
                }
                nlohmann::json asst;
                asst["role"]    = "assistant";
                asst["content"] = text_acc.empty() ? nullptr
                                                    : nlohmann::json(text_acc);
                if (!tc_arr.empty())
                    asst["tool_calls"] = tc_arr;
                out.push_back(asst);
            } else {
                // User content array: look for tool_result blocks
                for (auto& block : content) {
                    std::string btype = block.value("type", "");
                    if (btype == "tool_result") {
                        nlohmann::json tr;
                        tr["role"]         = "tool";
                        tr["tool_call_id"] = block.value("tool_use_id", "");
                        auto& bc = block["content"];
                        if (bc.is_string())
                            tr["content"] = bc;
                        else if (bc.is_array() && !bc.empty())
                            tr["content"] = bc[0].value("text", bc.dump());
                        else
                            tr["content"] = "";
                        out.push_back(tr);
                    }
                }
            }
            continue;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// OpenAI provider
// ---------------------------------------------------------------------------

class OpenAIProvider : public Provider {
public:
    explicit OpenAIProvider(const std::string& api_key,
                             const std::string& base_url = "https://api.openai.com")
        : api_key_(api_key), base_url_(base_url) {}

    std::string id() const override { return "openai"; }

    void stream(const LLMRequest& request, StreamCallbacks callbacks) override {
        cancelled_.store(false);

        // ---- Build request body ----
        nlohmann::json body;
        body["model"]       = request.model_id;
        body["stream"]      = true;
        body["max_tokens"]  = request.max_tokens;
        if (request.temperature)
            body["temperature"] = *request.temperature;

        // Include usage in stream_options (supported by OpenAI and most compat endpoints)
        body["stream_options"] = { {"include_usage", true} };

        // Translate messages (system is prepended inside)
        body["messages"] = translate_messages(request.system, request.messages);

        // Tools — wrap each as {"type":"function","function":{...}}
        if (!request.tools.empty()) {
            nlohmann::json tools_arr = nlohmann::json::array();
            for (auto& t : request.tools) {
                nlohmann::json fn;
                fn["name"]        = t.name;
                fn["description"] = t.description;
                fn["parameters"]  = t.input_schema;
                tools_arr.push_back({ {"type", "function"}, {"function", fn} });
            }
            body["tools"]       = tools_arr;
            body["tool_choice"] = "auto";
        }

        std::map<std::string, std::string> headers = {
            {"Content-Type",   "application/json"},
            {"Accept",         "text/event-stream"},
        };
        if (!api_key_.empty())
            headers["Authorization"] = "Bearer " + api_key_;

        // ---- SSE parse state ----
        // OpenAI sends one chunk per delta:
        //   {"choices":[{"delta":{"content":"..."}, "finish_reason":null}]}
        //   {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"...","type":"function",
        //                "function":{"name":"bash","arguments":""}}]}}]}
        //   {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"..."}}]}]}
        //   {"choices":[{"delta":{},"finish_reason":"tool_calls"}]}
        //   {"usage":{"prompt_tokens":N,"completion_tokens":M}}  (stream_options)

        struct ToolCallState {
            std::string id;
            std::string name;
            std::string arguments;
        };
        std::map<int, ToolCallState> tool_call_map;  // keyed by index

        std::string text_id = util::make_id("txt");
        FinishReason finish_reason = FinishReason::EndTurn;
        TokenUsage usage;
        bool error_occurred = false;

        std::string body_str = body.dump();
        http_.post_sse(base_url_ + "/v1/chat/completions", headers, body_str,
            [&](const SSEEvent& ev) -> bool {
                if (cancelled_.load()) return false;
                if (ev.data == "[DONE]") return false;  // clean stop
                if (ev.data.empty()) return true;

                try {
                    auto d = nlohmann::json::parse(ev.data, nullptr, false);
                    if (d.is_discarded()) return true;

                    // Error object (non-streaming error wrapped in SSE)
                    if (d.contains("error")) {
                        std::string msg = d["error"].value("message", "Unknown OpenAI error");
                        if (callbacks.on_error) callbacks.on_error(msg);
                        error_occurred = true;
                        return false;
                    }

                    // Usage chunk (stream_options — comes after [DONE] on some endpoints,
                    // or as the last non-DONE chunk)
                    if (d.contains("usage") && !d.contains("choices")) {
                        auto& u = d["usage"];
                        usage.input  = u.value("prompt_tokens",     0);
                        usage.output = u.value("completion_tokens",  0);
                        return true;
                    }

                    if (!d.contains("choices") || !d["choices"].is_array()
                            || d["choices"].empty())
                        return true;

                    auto& choice = d["choices"][0];
                    auto& delta  = choice["delta"];

                    // finish_reason
                    if (!choice["finish_reason"].is_null()) {
                        std::string fr = choice["finish_reason"].get<std::string>();
                        if (fr == "tool_calls")
                            finish_reason = FinishReason::ToolUse;
                        else if (fr == "length")
                            finish_reason = FinishReason::MaxTokens;
                        else if (fr == "stop")
                            finish_reason = FinishReason::EndTurn;
                    }

                    // Text delta
                    if (delta.contains("content") && delta["content"].is_string()) {
                        std::string text = delta["content"].get<std::string>();
                        if (!text.empty() && callbacks.on_text_delta)
                            callbacks.on_text_delta(text_id, text);
                    }

                    // Tool call deltas
                    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                        for (auto& tc_delta : delta["tool_calls"]) {
                            int idx = tc_delta.value("index", 0);
                            auto& state = tool_call_map[idx];

                            if (tc_delta.contains("id"))
                                state.id = tc_delta["id"].get<std::string>();

                            if (tc_delta.contains("function")) {
                                auto& fn = tc_delta["function"];
                                if (fn.contains("name") && fn["name"].is_string())
                                    state.name = fn["name"].get<std::string>();
                                if (fn.contains("arguments") && fn["arguments"].is_string()) {
                                    std::string args_chunk = fn["arguments"].get<std::string>();
                                    state.arguments += args_chunk;
                                    if (callbacks.on_tool_input_delta)
                                        callbacks.on_tool_input_delta(
                                            state.id, state.name, args_chunk);
                                }
                            }
                        }
                    }
                } catch (...) {}
                return true;
            });

        if (error_occurred) return;

        // Materialise accumulated tool calls
        std::vector<ToolCall> tool_calls;
        for (auto& [idx, state] : tool_call_map) {
            ToolCall tc;
            tc.id   = state.id.empty() ? util::make_id("tc") : state.id;
            tc.name = state.name;
            try {
                tc.input = nlohmann::json::parse(state.arguments, nullptr, false);
                if (tc.input.is_discarded()) tc.input = nlohmann::json::object();
            } catch (...) {
                tc.input = nlohmann::json::object();
            }
            tool_calls.push_back(std::move(tc));
        }

        if (callbacks.on_finish)
            callbacks.on_finish(finish_reason, usage, tool_calls);
    }

    void cancel() override {
        cancelled_.store(true);
        http_.cancel();
    }

    std::vector<std::string> list_models() override {
        std::map<std::string, std::string> headers = {
            {"accept", "application/json"},
        };
        if (!api_key_.empty())
            headers["Authorization"] = "Bearer " + api_key_;
        std::string body = http_.get(base_url_ + "/v1/models", headers);
        std::vector<std::string> result;
        try {
            auto j = nlohmann::json::parse(body, nullptr, false);
            if (j.is_discarded() || !j.contains("data")) return result;
            for (auto& m : j["data"]) {
                std::string mid = m.value("id", "");
                if (mid.empty()) continue;
                // Skip non-chat model categories
                if (mid.find("whisper")         != std::string::npos) continue;
                if (mid.find("dall-e")          != std::string::npos) continue;
                if (mid.find("tts")             != std::string::npos) continue;
                if (mid.find("text-embedding")  != std::string::npos) continue;
                if (mid.find("embedding")       != std::string::npos) continue;
                if (mid.find("moderation")      != std::string::npos) continue;
                if (mid.find("babbage")         != std::string::npos) continue;
                if (mid.find("davinci")         != std::string::npos) continue;
                result.push_back(mid);
            }
        } catch (...) {}
        return result;
    }

private:
    std::string api_key_;
    std::string base_url_;
    HttpClient  http_;
    std::atomic<bool> cancelled_{false};
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::shared_ptr<Provider> make_openai_provider(const std::string& api_key,
                                                const std::string& base_url) {
    if (base_url.empty())
        return std::make_shared<OpenAIProvider>(api_key);
    std::string url = base_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    return std::make_shared<OpenAIProvider>(api_key, url);
}

} // namespace haicode
