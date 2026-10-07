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
//   - llama.cpp: llamacpp_map_effort's value as reasoning_effort, mirrored
//     into chat_template_kwargs (plus enable_thinking:false for "off"), so
//     templates gated on it (Mistral Small 4: "high") actually reason;
//   - other flavored local servers (vLLM/LM Studio/Ollama): effort "off"
//     becomes chat_template_kwargs {enable_thinking:false}; reasoning_effort
//     is never sent;
//   - Generic/OpenRouter: capability-mapped reasoning_effort ("off" → "none"
//     on gpt-5*, omitted on o-series; non-reasoning models omit it entirely);
//   - llama.cpp: return_progress, so prompt prefill streams prompt_progress
//     chunks instead of minutes of silence (the chunks carry no content and
//     the stream parser skips them).
nlohmann::json build_openai_body(const LLMRequest& request, ServerFlavor flavor);

// post_sse stall limit (seconds below 1 byte/s before the stream aborts) for
// a flavor. Hosted endpoints keep HttpClient's 60 s default. Self-hosted
// servers (vLLM/llama.cpp/LM Studio/Ollama) send nothing while they prefill
// the prompt — an uncached 100K-token request at a few hundred tokens/s is
// minutes of silence — so they get kLocalServerStallTimeoutSec: still a bound
// for a wedged server, and interrupt cancels at any point regardless.
inline constexpr long kLocalServerStallTimeoutSec = 30 * 60;
long openai_stall_timeout(ServerFlavor flavor);

// Reads a stream-options usage object: prompt_tokens, completion_tokens, and
// prompt_tokens_details.cached_tokens → cache_read. Pure; leaves other usage
// fields untouched.
void parse_openai_usage(const nlohmann::json& u, TokenUsage& usage);

} // namespace haicode
