#include <haicode/anthropic_params.h>
#include <haicode/model_capabilities.h>
#include <haicode/model_info.h>

namespace haicode {

nlohmann::json build_anthropic_body(const LLMRequest& request) {
    nlohmann::json body;
    body["model"] = request.model_id;
    // Anthropic's API has no omit-and-default for this field. The fallback
    // (and any explicit override) is clamped to the model's published output
    // cap — e.g. claude-3-5-sonnet rejects anything above 8192.
    body["max_tokens"] = clamp_max_tokens(
        request.model_id, request.max_tokens.value_or(kDefaultMaxTokens));
    body["stream"] = true;

    const AnthropicModelCaps caps = anthropic_model_caps(request.model_id);

    // Sampling params conflict with an active thinking config and Claude 5
    // rejects non-default values outright, so they are only sent when no
    // thinking param is being attached (budgeted models get no auto config,
    // and their no-thinking-field default accepts sampling).
    if (caps.thinking != ThinkingMode::Adaptive) {
        if (request.temperature)
            body["temperature"] = *request.temperature;
        if (request.top_p)
            body["top_p"] = *request.top_p;
    }

    // Capability-mapped effort. "off" maps to "low" (the lowest valid
    // value); models without effort support omit the param entirely.
    const std::string effort = anthropic_map_effort(request.model_id,
                                                    request.reasoning_effort);
    if (!effort.empty())
        body["output_config"] = {{"effort", effort}};

    // Adaptive thinking with a visible summary: current models default to
    // display:"omitted", which would leave the reasoning panel empty.
    // Budgeted models get no auto config (HaiCode never sends
    // budget_tokens), and thinking-disabled is never sent — several
    // current models reject it with 400.
    if (caps.thinking == ThinkingMode::Adaptive)
        body["thinking"] = {{"type", "adaptive"}, {"display", "summarized"}};

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

    return body;
}

void ThinkingBlockAcc::apply_delta(const nlohmann::json& delta) {
    if (!delta.is_object()) return;
    const std::string dtype = delta.value("type", "");
    if (dtype == "thinking_delta") {
        thinking += delta.value("thinking", "");
    } else if (dtype == "signature_delta") {
        signature += delta.value("signature", "");
    }
}

bool parse_anthropic_models(const nlohmann::json& j,
                            std::vector<AnthropicModelEntry>& out) {
    if (!j.is_object() || !j.contains("data") || !j["data"].is_array())
        return false;
    for (auto& m : j["data"]) {
        if (!m.is_object()) continue;
        AnthropicModelEntry e;
        e.id = m.value("id", "");
        if (e.id.empty()) continue;
        if (m.contains("max_input_tokens") && m["max_input_tokens"].is_number())
            e.max_input_tokens = m["max_input_tokens"].get<int>();
        out.push_back(std::move(e));
    }
    return true;
}

} // namespace haicode
