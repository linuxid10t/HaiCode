#pragma once
#include "types.h"
#include <string>
#include <map>

namespace haicode {

// Per-model token prices in USD per 1,000,000 tokens.
struct ModelPricing {
    double input       = 0.0;
    double output      = 0.0;
    double cache_read  = 0.0;
    double cache_write = 0.0;
};

// Resolves pricing for (provider_id, model_id). Resolution is
// longest-prefix match against the union of:
//   1. `overrides` (from AppConfig::pricing) — keys like
//      "anthropic:claude-sonnet-4" match all variants
//      (claude-sonnet-4-5, claude-sonnet-4-6-20250514, ...).
//   2. Built-in defaults for common Anthropic and OpenAI models.
// Override entries win ties (equal prefix length) against built-ins.
// Returns nullptr if no entry matches — caller should treat the model
// as "unknown, zero cost".
const ModelPricing* lookup_pricing(
    const std::string& provider_id,
    const std::string& model_id,
    const std::map<std::string, ModelPricing>& overrides);

// Computes per-turn cost from token usage and the resolved pricing.
// Reasoning tokens are billed at the output rate (standard for
// Anthropic extended thinking and OpenAI o-series). Result is in USD.
double compute_cost(const TokenUsage& usage, const ModelPricing& pricing);

} // namespace haicode
