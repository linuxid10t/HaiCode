#pragma once
#include "provider.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace haicode {

// Pure, server-free helpers extracted from the ChatGPT (Codex backend)
// provider so request building is unit-testable.

// Content-block type the engine uses to carry a replayable Responses-API
// reasoning item inside an assistant content array (only emitted when the
// active provider replays reasoning — see Provider::replays_reasoning_items).
inline constexpr const char* kOpenAIReasoningBlock = "openai_reasoning";

// True for models whose Responses output carries reasoning items (o-series,
// gpt-5*, codex-*). Gates `include: ["reasoning.encrypted_content"]`, which
// non-reasoning models reject.
bool codex_model_reasons(const std::string& model_id);

// Reduces a streamed `response.output_item.done` reasoning item to the shape
// that can be sent back on a store:false request: {type, summary,
// encrypted_content}. The server-side `id` is dropped (items are not
// persisted with store:false, so referencing it fails). Returns null when
// the item is not a reasoning item or carries no encrypted_content.
nlohmann::json codex_reasoning_item_for_replay(const nlohmann::json& item);

// Reads a Responses-API usage object into disjoint TokenUsage buckets.
// The API's input_tokens includes cached_tokens and its output_tokens
// INCLUDES reasoning_tokens; both are split out here so compute_cost
// (which bills every bucket) charges each token exactly once.
TokenUsage parse_codex_usage(const nlohmann::json& u);

// Translates the engine's Anthropic-shaped context into Responses-API input
// items (see codex.cpp header comment for the mapping).
std::vector<nlohmann::json> translate_to_responses_items(
    const std::vector<nlohmann::json>& src);

// Builds the /codex/responses request body. Cache-relevant layout:
//   - `instructions` carries only the byte-stable system prompt;
//   - a non-empty `system_dynamic` is the LAST input item, so the whole
//     history before it stays a reusable cached prefix (the engine sends it
//     empty — per-step state rides `messages` as persisted status updates);
//   - `prompt_cache_key` = request.cache_key (the session id) routes every
//     step of a session to the same prompt cache;
//   - reasoning models get `include: ["reasoning.encrypted_content"]` so
//     their reasoning can be replayed on the next step (store:false).
nlohmann::json build_codex_body(const LLMRequest& request);

} // namespace haicode
