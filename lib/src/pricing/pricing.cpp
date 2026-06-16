#include <haicode/pricing.h>

namespace haicode {

// Built-in defaults (USD per 1M tokens, public list prices).
// Keys are "provider_id:model-prefix" — longest-prefix match means
// "anthropic:claude-sonnet-4" covers claude-sonnet-4-5, -4-6, and any
// dated variant like claude-sonnet-4-6-20250514.
static const struct { const char* key; ModelPricing p; } kBuiltin[] = {
    // Anthropic Claude 4.x — cache_write is 1.25x input, cache_read is 0.1x input.
    {"anthropic:claude-opus-4",    { 15.0,  75.0, 1.50, 18.75}},
    {"anthropic:claude-sonnet-4",  {  3.0,  15.0, 0.30,  3.75}},
    {"anthropic:claude-haiku-4",   {  1.0,   5.0, 0.10,  1.25}},
    // OpenAI — prompt-caching discount on cache_read; no cache_write charge.
    {"openai:gpt-4o-mini",         {  0.15,  0.60, 0.075, 0.0}},
    {"openai:gpt-4o",              {  2.50, 10.0,  1.25,  0.0}},
    {"openai:o4-mini",             {  1.50,  6.0,  0.75,  0.0}},
    {"openai:o3-mini",             {  3.0,  12.0,  1.50,  0.0}},
    {"openai:o3",                  { 15.0,  60.0,  7.50,  0.0}},
    {"openai:o1",                  { 15.0,  60.0,  7.50,  0.0}},
    {"openai:gpt-5",               {  5.0,  15.0,  2.50,  0.0}},
};

static std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
    return s;
}

const ModelPricing* lookup_pricing(
    const std::string& provider_id,
    const std::string& model_id,
    const std::map<std::string, ModelPricing>& overrides)
{
    std::string full_lc = lower(provider_id + ":" + model_id);

    const ModelPricing* best = nullptr;
    size_t best_len = 0;

    for (auto& e : kBuiltin) {
        std::string k_lc = lower(e.key);
        if (full_lc.rfind(k_lc, 0) == 0 && k_lc.size() > best_len) {
            best = &e.p;
            best_len = k_lc.size();
        }
    }
    // Override prefix wins on tie so a user can replace a built-in entry
    // by reusing the same key.
    for (auto& [k, v] : overrides) {
        std::string k_lc = lower(k);
        if (full_lc.rfind(k_lc, 0) == 0 && k_lc.size() >= best_len) {
            best = &v;
            best_len = k_lc.size();
        }
    }
    return best;
}

double compute_cost(const TokenUsage& u, const ModelPricing& p) {
    double cost = 0.0;
    cost += static_cast<double>(u.input)       * p.input;
    cost += static_cast<double>(u.output)      * p.output;
    // Reasoning tokens are billed at the output rate (Anthropic extended
    // thinking, OpenAI o-series).
    cost += static_cast<double>(u.reasoning)   * p.output;
    cost += static_cast<double>(u.cache_read)  * p.cache_read;
    cost += static_cast<double>(u.cache_write) * p.cache_write;
    return cost / 1'000'000.0;
}

} // namespace haicode
