#include <haicode/provider.h>
#include <haicode/util.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstdio>
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
                   const std::string& system_dynamic,
                   const std::vector<nlohmann::json>& src)
{
    std::vector<nlohmann::json> out;

    // Concatenate stable + dynamic system content. OpenAI caches
    // automatically; no cache_control markers needed.
    std::string joined = system;
    if (!system_dynamic.empty()) {
        if (!joined.empty()) joined += "\n\n";
        joined += system_dynamic;
    }
    if (!joined.empty())
        out.push_back({ {"role", "system"}, {"content", joined} });

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
                    } else {
                        // Unknown assistant block (thinking, image, etc.).
                        // Warn rather than silently drop so future additions
                        // surface visibly instead of vanishing from the
                        // OpenAI request.
                        fprintf(stderr, "openai: dropping unknown assistant "
                                "content block type '%s'\n",
                                btype.c_str());
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
                // User content array. Anthropic allows mixing text,
                // tool_result, image blocks; OpenAI can't carry all of
                // these in one message. tool_result blocks become separate
                // "tool" role messages; remaining text blocks concatenate
                // into a single "user" message emitted AFTER the tool
                // messages (flow: assistant tool_calls → tool responses →
                // user follow-up). Unknown block types warn but don't fail.
                std::string user_text;
                for (auto& block : content) {
                    std::string btype = block.value("type", "");
                    if (btype == "tool_result") {
                        nlohmann::json tr;
                        tr["role"]         = "tool";
                        tr["tool_call_id"] = block.value("tool_use_id", "");
                        auto& bc = block["content"];
                        if (bc.is_string()) {
                            tr["content"] = bc;
                        } else if (bc.is_array() && !bc.empty()) {
                            // Concatenate string and text blocks; warn on
                            // anything else (image results, etc.) instead
                            // of silently keeping only the first element.
                            std::string acc;
                            for (auto& sub : bc) {
                                if (sub.is_string())
                                    acc += sub.get<std::string>();
                                else if (sub.value("type", "") == "text")
                                    acc += sub.value("text", "");
                                else
                                    fprintf(stderr,
                                        "openai: dropping unsupported "
                                        "tool_result content block type "
                                        "'%s'\n",
                                        sub.value("type", "(none)").c_str());
                            }
                            tr["content"] = acc;
                        } else {
                            tr["content"] = "";
                        }
                        out.push_back(tr);
                    } else if (btype == "text") {
                        user_text += block.value("text", "");
                    } else if (btype == "image") {
                        // OpenAI vision uses {type:"image_url", image_url:{...}}
                        // but the Anthropic source shape {type:"image",
                        // source:{...}} differs. Warn until that mapping
                        // is implemented — don't silently drop.
                        fprintf(stderr, "openai: image block in user content "
                                "not yet translated (needs Anthropic source "
                                "→ image_url conversion)\n");
                    } else {
                        fprintf(stderr, "openai: dropping unknown user "
                                "content block type '%s'\n",
                                btype.c_str());
                    }
                }
                if (!user_text.empty()) {
                    out.push_back({
                        {"role", "user"}, {"content", user_text}
                    });
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
                             const std::string& base_url = "https://api.openai.com/v1",
                             const std::string& id = "openai")
        : api_key_(api_key), base_url_(base_url), id_(id.empty() ? "openai" : id) {}

    std::string id() const override { return id_; }

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
        body["messages"] = translate_messages(request.system, request.system_dynamic,
                                                request.messages);

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
        http_.post_sse(base_url_ + "/chat/completions", headers, body_str,
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

                    // Usage chunk (stream_options.include_usage). Per OpenAI spec
                    // this arrives as the final chunk with an empty choices array
                    // ({"choices":[],"usage":{...}}); some compat endpoints also
                    // attach usage to the last content chunk. Read it whenever present
                    // regardless of choices, then continue normal processing below.
                    if (d.contains("usage") && d["usage"].is_object()) {
                        auto& u = d["usage"];
                        usage.input  = u.value("prompt_tokens",     0);
                        usage.output = u.value("completion_tokens", 0);
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
            tc.id       = state.id.empty() ? util::make_id("tc") : state.id;
            tc.name     = state.name;
            tc.raw_input = state.arguments;
            try {
                tc.input = nlohmann::json::parse(state.arguments, nullptr, false);
                if (tc.input.is_discarded()) {
                    tc.input = nlohmann::json::object();
                    tc.parse_failed = true;
                    fprintf(stderr,
                        "[openai] tool_call %s (%s) arguments parse failed; "
                        "raw=%zu bytes: %.200s\n",
                        tc.id.c_str(), tc.name.c_str(),
                        state.arguments.size(), state.arguments.c_str());
                }
            } catch (...) {
                tc.input = nlohmann::json::object();
                tc.parse_failed = true;
                fprintf(stderr,
                    "[openai] tool_call %s (%s) arguments parse threw; "
                    "raw=%zu bytes: %.200s\n",
                    tc.id.c_str(), tc.name.c_str(),
                    state.arguments.size(), state.arguments.c_str());
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

    std::vector<std::string> list_models(std::string& error) override {
        error.clear();
        std::map<std::string, std::string> headers = {
            {"accept", "application/json"},
        };
        if (!api_key_.empty())
            headers["Authorization"] = "Bearer " + api_key_;
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
                    " (check base_url — it must include the API version path, "
                    "e.g. .../v1)";
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
        } catch (...) {
            error = "invalid response from server";
        }
        return result;
    }

private:
    std::string api_key_;
    std::string base_url_;
    std::string id_;
    HttpClient  http_;
    std::atomic<bool> cancelled_{false};
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::shared_ptr<Provider> make_openai_provider(const std::string& api_key,
                                                const std::string& base_url,
                                                const std::string& id) {
    if (base_url.empty())
        return std::make_shared<OpenAIProvider>(api_key, "https://api.openai.com/v1", id);
    std::string url = base_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    return std::make_shared<OpenAIProvider>(api_key, url, id);
}

} // namespace haicode
