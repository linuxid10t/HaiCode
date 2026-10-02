#pragma once
#include "provider.h"
#include <nlohmann/json.hpp>
#include <string>

namespace haicode {

// Pure, server-free helpers extracted from the Anthropic provider so request
// building and thinking-block parsing are unit-testable (Task 23).

// Builds the /messages streaming request body: model, max_tokens, sampling
// params (omitted on adaptive-thinking models, which reject non-default
// temperature/top_p), capability-mapped output_config.effort, adaptive
// thinking with display:"summarized", cached system blocks, verbatim
// messages, tools with a trailing cache breakpoint, and the
// conversation-prefix cache_control post-pass.
nlohmann::json build_anthropic_body(const LLMRequest& request);

// Accumulates one streaming thinking content block. thinking_delta and
// signature_delta fragments arrive as separate content_block_delta events;
// the finished (thinking, signature) pair must round-trip unchanged in
// later turns of a tool loop.
struct ThinkingBlockAcc {
    std::string thinking;
    std::string signature;
    // delta is the content_block_delta "delta" object; unrecognized types
    // are ignored.
    void apply_delta(const nlohmann::json& delta);
};

// One entry from the Models API (/v1/models): the id plus the discovered
// max_input_tokens (0 when the server didn't report one).
struct AnthropicModelEntry {
    std::string id;
    int max_input_tokens = 0;
};

// Parses a /v1/models response body (already JSON). Returns false when the
// document is not a models list (missing "data"); entries with empty ids
// are skipped. Pure, unit-testable.
bool parse_anthropic_models(const nlohmann::json& j,
                            std::vector<AnthropicModelEntry>& out);

} // namespace haicode
