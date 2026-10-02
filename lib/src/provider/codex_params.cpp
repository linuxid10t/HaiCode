#include <haicode/codex_params.h>
#include <haicode/model_capabilities.h>
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace haicode {

bool codex_model_reasons(const std::string& model_id) {
    if (openai_is_reasoning_model(model_id)) return true;
    std::string lower = model_id;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lower.find("codex") != std::string::npos;
}

nlohmann::json codex_reasoning_item_for_replay(const nlohmann::json& item) {
    if (!item.is_object() || item.value("type", "") != "reasoning")
        return nullptr;
    auto enc = item.find("encrypted_content");
    if (enc == item.end() || !enc->is_string() || enc->get_ref<const std::string&>().empty())
        return nullptr;
    nlohmann::json out;
    out["type"] = "reasoning";
    auto sum = item.find("summary");
    out["summary"] = (sum != item.end() && sum->is_array())
        ? *sum : nlohmann::json::array();
    out["encrypted_content"] = *enc;
    return out;
}

std::vector<nlohmann::json> translate_to_responses_items(
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
            // Output order of a Responses turn: reasoning items, then the
            // text message, then one function_call item per tool_use block.
            // Replayed reasoning (encrypted_content captured on an earlier
            // step) lets the model continue its chain instead of re-deriving
            // it every step. Anthropic thinking blocks are dropped — their
            // signatures mean nothing to this backend.
            std::vector<nlohmann::json> reasoning;
            std::string text_acc;
            std::vector<nlohmann::json> calls;
            for (auto& block : content) {
                std::string btype = block.value("type", "");
                if (btype == kOpenAIReasoningBlock) {
                    auto it = block.find("item");
                    if (it != block.end()) {
                        auto item = codex_reasoning_item_for_replay(*it);
                        if (!item.is_null()) reasoning.push_back(std::move(item));
                    }
                } else if (btype == "text") {
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
            for (auto& r : reasoning) out.push_back(r);
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

nlohmann::json build_codex_body(const LLMRequest& request) {
    nlohmann::json body;
    body["model"]  = request.model_id;
    body["stream"] = true;
    // Codex sends store:false — conversation history lives client-side.
    body["store"] = false;

    // Instructions = the byte-stable system prompt ONLY. The Responses
    // prompt is laid out tools → instructions → input, so anything that
    // varies here invalidates the cached prefix for the entire conversation
    // history behind it.
    if (!request.system.empty())
        body["instructions"] = request.system;

    if (request.max_tokens)
        body["max_output_tokens"] = *request.max_tokens;
    // Temperature is ignored by this backend (verified by third-party
    // contract docs); omit rather than send a field it may reject.

    // "off" is not a valid Responses effort value — omit entirely.
    if (!request.reasoning_effort.empty()
            && request.reasoning_effort != "off")
        body["reasoning"] = {{"effort", request.reasoning_effort}};

    // store:false means the server keeps nothing between steps: without the
    // encrypted reasoning content the model re-derives its reasoning from
    // scratch on every step of a tool loop (billed as output tokens).
    if (codex_model_reasons(request.model_id))
        body["include"] = nlohmann::json::array({"reasoning.encrypted_content"});

    // Session-scoped cache routing (the Codex CLI sends its conversation id
    // here). Without it, consecutive steps can land on servers that don't
    // hold the prefix.
    if (!request.cache_key.empty())
        body["prompt_cache_key"] = request.cache_key;

    auto input = translate_to_responses_items(request.messages);
    // Per-step dynamic content goes LAST (same strategy as the OpenAI
    // provider): everything before it stays byte-identical between steps,
    // and on the next step the new items land where this one was. The
    // Responses API has no strict role alternation, so a separate trailing
    // user item is always legal.
    if (!request.system_dynamic.empty())
        input.push_back({{"role", "user"}, {"content", request.system_dynamic}});
    body["input"] = input;

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
    return body;
}

} // namespace haicode
