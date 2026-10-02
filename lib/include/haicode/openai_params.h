#pragma once
#include "provider.h"
#include "model_context_parse.h"
#include <nlohmann/json.hpp>
#include <string>

namespace haicode {

// Pure, server-free helpers extracted from the OpenAI-compatible provider so
// request building and usage parsing are unit-testable (Task 24).

// Builds the /chat/completions streaming request body. Behavior matrix:
//   - reasoning models (o1*/o3*/o4*/gpt-5*): max_completion_tokens instead of
//     max_tokens, temperature/top_p omitted (400 unsupported_parameter);
//   - flavored local servers (vLLM/llama.cpp/LM Studio/Ollama): effort "off"
//     becomes chat_template_kwargs {enable_thinking:false}; reasoning_effort
//     is never sent (these servers don't use it);
//   - Generic/OpenRouter: capability-mapped reasoning_effort ("off" → "none"
//     on gpt-5*, omitted on o-series; non-reasoning models omit it entirely).
nlohmann::json build_openai_body(const LLMRequest& request, ServerFlavor flavor);

// Reads a stream-options usage object: prompt_tokens, completion_tokens, and
// prompt_tokens_details.cached_tokens → cache_read. Pure; leaves other usage
// fields untouched.
void parse_openai_usage(const nlohmann::json& u, TokenUsage& usage);

} // namespace haicode
