#include <haicode/openai_params.h>
#include <haicode/model_capabilities.h>

namespace haicode {

nlohmann::json build_openai_body(const LLMRequest& request, ServerFlavor flavor) {
    nlohmann::json body;
    body["model"]  = request.model_id;
    body["stream"] = true;

    const bool reasoning = openai_is_reasoning_model(request.model_id);

    // Reasoning models (o1*/o3*/o4*/gpt-5*) reject max_tokens with 400
    // unsupported_parameter and require max_completion_tokens; they also
    // reject temperature/top_p.
    if (request.max_tokens) {
        if (reasoning)
            body["max_completion_tokens"] = *request.max_tokens;
        else
            body["max_tokens"] = *request.max_tokens;
    }
    if (!reasoning) {
        if (request.temperature)
            body["temperature"] = *request.temperature;
        if (request.top_p)
            body["top_p"] = *request.top_p;
    }

    // Flavored local servers (vLLM/llama.cpp/LM Studio/Ollama running Qwen3,
    // DeepSeek, ...) don't use reasoning_effort: effort "off" suppresses their
    // thinking mode via chat_template_kwargs instead. True OpenAI ignores
    // unknown extra fields, but Generic/OpenRouter get the mapped
    // reasoning_effort only.
    const bool flavored = flavor == ServerFlavor::VLLM
                       || flavor == ServerFlavor::LlamaCpp
                       || flavor == ServerFlavor::LMStudio
                       || flavor == ServerFlavor::Ollama;
    if (flavored) {
        if (request.reasoning_effort == "off")
            body["chat_template_kwargs"] = {{"enable_thinking", false}};
    } else {
        const std::string effort = openai_map_effort(request.model_id,
                                                     request.reasoning_effort);
        if (!effort.empty())
            body["reasoning_effort"] = effort;
    }

    // Include usage in stream_options (supported by OpenAI and most compat
    // endpoints).
    body["stream_options"] = { {"include_usage", true} };

    // llama.cpp (and LM Studio's llama.cpp-backed GGUF runtime) only
    // populate the KV/prefix cache when explicitly asked. LM Studio's
    // OpenAI-compat layer ignores unknown fields with a log warning, so
    // this is safe to send; other flavors keep their exact contract
    // (e.g. OpenAI rejects `cache_prompt`).
    if (flavor == ServerFlavor::LlamaCpp || flavor == ServerFlavor::LMStudio)
        body["cache_prompt"] = true;

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

    return body;
}

void parse_openai_usage(const nlohmann::json& u, TokenUsage& usage) {
    if (!u.is_object()) return;
    usage.input  = u.value("prompt_tokens", 0);
    usage.output = u.value("completion_tokens", 0);
    // Prompt-caching discount tokens ride in a details object; without them
    // cached input is billed (and metered) at full price.
    if (u.contains("prompt_tokens_details")
            && u["prompt_tokens_details"].is_object()) {
        usage.cache_read =
            u["prompt_tokens_details"].value("cached_tokens", 0);
    }
}

} // namespace haicode
