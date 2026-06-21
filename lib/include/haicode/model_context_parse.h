#pragma once
#include <nlohmann/json.hpp>

namespace haicode {

// Distinguishes OpenAI-compatible servers that carry context-window metadata
// in different places. All flavors share stream()/translate_messages() — only
// list_models() context parsing and lazy get_model_context() differ.
enum class ServerFlavor {
    Generic,     // plain OpenAI / no context discovery
    VLLM,        // /v1/models carries max_model_len
    OpenRouter,  // /v1/models carries context_length
    LMStudio,    // native /api/{v0,v1}/models carries contextLength/maxContextLength
    LlamaCpp,    // /props carries default_generation_settings.n_ctx
    Ollama,      // POST /api/show carries model_info["llama.context_length"]
};

// Parse context window from a single /v1/models `data[]` entry, based on the
// server flavor. Returns 0 if not present/unparseable.
int parse_model_context_inline(const nlohmann::json& m, ServerFlavor flavor);

// Parse Ollama's /api/show response: model_info["llama.context_length"].
int parse_ollama_show(const nlohmann::json& j);

// Parse llama.cpp's /props response: default_generation_settings.n_ctx.
int parse_llamacpp_props(const nlohmann::json& j);

// Parse LM Studio's native /api/v0/models or /api/v1/models response.
// Returns the loaded `contextLength` of the model matching `model_id`,
// falling back to `maxContextLength`. 0 if not found/unparseable.
int parse_lmstudio_native_models(const nlohmann::json& j,
                                 const std::string& model_id);

} // namespace haicode
