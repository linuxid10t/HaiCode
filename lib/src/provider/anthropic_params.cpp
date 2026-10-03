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

    // System — the byte-stable body only, with a cache breakpoint. Any
    // per-request dynamic text (`system_dynamic`) must NOT live here: the
    // cache prefix is tools → system → messages, so a changing system block
    // invalidates the cache for the whole conversation behind it. It rides
    // at the tail of the messages instead (below). Exception: with no
    // messages to carry it, it stays a second system block. (The engine
    // sends the field empty — its per-step state is already inside
    // `messages` as persisted status updates.)
    if (!request.system.empty() || (request.messages.empty()
                                    && !request.system_dynamic.empty())) {
        nlohmann::json arr = nlohmann::json::array();
        if (!request.system.empty()) {
            nlohmann::json stable_block;
            stable_block["type"] = "text";
            stable_block["text"] = request.system;
            stable_block["cache_control"] = {{"type", "ephemeral"}};
            arr.push_back(stable_block);
        }
        if (request.messages.empty() && !request.system_dynamic.empty())
            arr.push_back({{"type", "text"}, {"text", request.system_dynamic}});
        body["system"] = arr;
    }

    // Messages — verbatim from the engine; the cache breakpoints and the
    // dynamic tail are applied in a post-pass below.
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

    // Conversation-prefix cache breakpoints, then the dynamic tail.
    //
    // (1) Last block of the LAST message: everything up to and including
    //     this step's input (e.g. fresh tool results) is written to the
    //     cache, so the next step reads it instead of paying full price
    //     for it and then again for the cache write.
    // (2) Last block of the second-to-last message: a fallback read point
    //     when the last message holds more blocks than the cache lookback
    //     window (many parallel tool results).
    // Together with system and tools that is 4 breakpoints — the API max.
    //
    // (3) A non-empty system_dynamic is appended as a text block AFTER
    //     breakpoint (1), so it can change per request without invalidating
    //     anything cached.
    //     A trailing text block after tool_result blocks is legal. If the
    //     conversation ends on an assistant turn, it becomes its own user
    //     message (user after assistant keeps alternation legal).
    //
    // String content is promoted to a one-element text array to carry a
    // breakpoint; the API renders both forms identically, so the promoted
    // message still matches the plain-string form on later steps.
    auto promote = [](nlohmann::json& msg) -> nlohmann::json* {
        if (!msg.is_object() || !msg.contains("content")) return nullptr;
        auto& content = msg["content"];
        if (content.is_string()) {
            std::string s = content.get<std::string>();
            if (s.empty()) return nullptr;  // empty text blocks are rejected
            content = nlohmann::json::array({
                {{"type", "text"}, {"text", s}}
            });
        }
        return (content.is_array() && !content.empty()) ? &content : nullptr;
    };
    auto& msgs = body["messages"];
    if (msgs.is_array() && !msgs.empty()) {
        if (auto* c = promote(msgs.back()))
            c->back()["cache_control"] = {{"type", "ephemeral"}};
        if (msgs.size() >= 2)
            if (auto* c = promote(msgs[msgs.size() - 2]))
                c->back()["cache_control"] = {{"type", "ephemeral"}};

        if (!request.system_dynamic.empty()) {
            nlohmann::json dyn = {{"type", "text"},
                                  {"text", request.system_dynamic}};
            auto& last = msgs.back();
            if (last.is_object() && last.value("role", "") == "user") {
                auto& content = last["content"];
                if (content.is_string())  // only when empty (see promote)
                    content = nlohmann::json::array();
                if (content.is_array()) content.push_back(dyn);
            } else {
                msgs.push_back({{"role", "user"},
                                {"content", nlohmann::json::array({dyn})}});
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
