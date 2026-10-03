#pragma once
#include "types.h"
#include <string>
#include <map>
#include <utility>
#include <vector>

namespace haicode {

// Per-model token prices in USD per 1,000,000 tokens.
struct ModelPricing {
    double input       = 0.0;
    double output      = 0.0;
    double cache_read  = 0.0;
    double cache_write = 0.0;  // 5-minute write (1.25x input on Anthropic);
                               // 1-hour writes (2x) are not modeled
};

// Resolves pricing for (provider_id, model_id) with the legacy lookup:
// longest-prefix match against overrides + built-ins keyed
// "<provider_id>:<model-prefix>". Kept for compatibility.
const ModelPricing* lookup_pricing(
    const std::string& provider_id,
    const std::string& model_id,
    const std::map<std::string, ModelPricing>& overrides);

// Full resolution with provider-kind fallback chain (Task 26):
//   1. local server kinds (ollama/vllm/lmstudio/llamacpp) short-circuit to
//      free — cloud prices must never apply to a local server;
//   2. config overrides, matched against "<provider_id>:<model>" and
//      "<provider_kind>:<model>" and "<model>" (prefixes allowed) together
//      with the built-ins keyed "<kind>:<model>" (anthropic:/openai:) then
//      "*:<model>" (globally-unique ids from other labs), as ONE prefix
//      database: the longest matched model part wins and an override wins a
//      tie, so a user can re-key a built-in entry but a short override
//      ("gpt-5") never shadows a longer built-in ("gpt-5.5");
//   3. nullptr = unknown, zero cost.
// Model ids are normalized first: lowercase, leading vendor path segment
// ("z-ai/glm-5.3") and cloud region prefixes ("us.", "eu.", "global.",
// "anthropic.", "meta.", "amazon.") dropped, ":free"/":thinking"/":beta"
// suffixes dropped (":free" also zeroes the price), "@date" snapshots kept.
const ModelPricing* lookup_pricing(
    const std::string& provider_id,
    const std::string& provider_kind,
    const std::string& model_id,
    const std::map<std::string, ModelPricing>& overrides);

// Computes per-turn cost from token usage and the resolved pricing.
// Reasoning tokens are billed at the output rate (standard for
// Anthropic extended thinking and OpenAI o-series). Result is in USD.
double compute_cost(const TokenUsage& usage, const ModelPricing& pricing);

// True for local-server provider kinds, whose inference costs nothing:
// cloud list prices must never be applied to a user's own Ollama/vLLM/
// LM Studio/llama.cpp instance serving a cloud-named model id.
bool is_local_provider_kind(const std::string& provider_kind);

// Resolves pricing through the kind-aware chain and computes the step cost,
// applying long-context surcharge tiers where the model has one: above the
// threshold (measured on total prompt tokens: input + cache_read +
// cache_write) the WHOLE request is repriced at the higher rate (xAI's
// documented behavior; assumed for the others). A winning config override
// is a flat price and replaces the tier ladder. Returns 0.0 when no pricing
// resolves (unknown model or local server).
double compute_step_cost(const TokenUsage& usage,
                         const std::string& provider_id,
                         const std::string& provider_kind,
                         const std::string& model_id,
                         const std::map<std::string, ModelPricing>& overrides);

// Best-effort usage for a request whose final usage report hasn't arrived
// (still streaming, interrupted, or failed mid-stream). Inputs:
//   reported         — usage the provider reported so far (Anthropic sends
//                      its input/cache buckets at message_start; others
//                      report nothing until the end);
//   streamed_chars   — text + reasoning + tool-input characters received;
//   est_request_tokens — chars/4 estimate of the whole request;
//   cached_prefix_tokens — prompt size of the previous step in this turn,
//                      used as the likely cache-read share when the
//                      provider hasn't reported input (in a tool loop the
//                      prior request is the cached prefix of this one).
// Reported input buckets win over the estimate; output is the larger of
// the reported count and streamed_chars/4 (rounded up).
TokenUsage estimate_inflight_usage(const TokenUsage& reported,
                                   size_t streamed_chars,
                                   int est_request_tokens,
                                   int cached_prefix_tokens);

// Read-only copy of the built-in flat price table ("<kind>:<prefix>" or
// "*:<prefix>" keys, base tier for tiered models), for the Model Database
// editor.
std::vector<std::pair<std::string, ModelPricing>> builtin_pricing_entries();

// True when the built-in table bills this model on a long-context or
// input-size tier ladder (the flat entry is only its base tier).
bool has_price_tiers(const std::string& model_id);

// Share of the prompt (input + cache reads + cache writes) served from the
// provider's prompt cache, floored to a whole percent: 100 only when every
// prompt token was a cache read. -1 when there is no prompt to measure.
// Callers decide whether to show it — a provider that never reports cache
// activity (most local servers) would otherwise read as a constant 0%.
int cache_hit_percent(const TokenUsage& usage);

} // namespace haicode
