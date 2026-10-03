#include <haicode/pricing.h>

// last_verified: 2026-10-02 against MODEL_NUMBERS.md (repo root), which was
// compiled from the labs' published tables via LiteLLM + secondary sources.

#include <algorithm>
#include <cstring>

namespace haicode {

// ---- built-in flat pricing -------------------------------------------------
// Keys are "<kind>:<model-prefix>" (kind = provider kind, not the user's
// provider id) or "*:<model-prefix>" for labs whose model ids are globally
// unique and reached through arbitrary proxies/aggregators. Longest-prefix
// match. Where the model is tiered (long-context surcharge or input-size
// tiers), the entry here is the BASE (first-tier) price; the full ladder
// lives in kTiered and is consulted by compute_step_cost.
static const struct { const char* key; ModelPricing p; } kBuiltin[] = {
    // Anthropic — cache_write is the 5-minute tier (1.25x input); 1-hour
    // writes (2x) are not modeled. Fable/Mythos 5 vs 5.1 differ in cache_read.
    {"anthropic:claude-fable-5-1",   {10.0,  50.0,  0.25, 12.50}},
    {"anthropic:claude-fable-5",     {10.0,  50.0,  1.00, 12.50}},
    {"anthropic:claude-mythos-5-1",  {10.0,  50.0,  0.25, 12.50}},
    {"anthropic:claude-mythos-5",    {10.0,  50.0,  1.00, 12.50}},
    {"anthropic:claude-opus-5-5",    { 4.0,  20.0,  0.20,  5.00}},
    {"anthropic:claude-opus-5",      { 5.0,  25.0,  0.50,  6.25}},
    {"anthropic:claude-opus-4",      { 5.0,  25.0,  0.50,  6.25}},  // 4.5-4.8
    {"anthropic:claude-sonnet-5",    { 2.0,  10.0,  0.20,  2.50}},
    {"anthropic:claude-sonnet-4-6",  { 3.0,  15.0,  0.30,  3.75}},
    {"anthropic:claude-sonnet-4-5",  { 3.0,  15.0,  0.30,  3.75}},  // surcharge >200K
    {"anthropic:claude-sonnet-4",    { 3.0,  15.0,  0.30,  3.75}},
    {"anthropic:claude-haiku-4-5",   { 1.0,   5.0,  0.10,  1.25}},
    {"anthropic:claude-3-5-haiku",   { 0.80,  4.0,  0.08,  1.00}},
    // Retired-but-kept ids for old sessions (pre-date the current table).
    {"anthropic:claude-opus-4-1",    {15.0,  75.0,  1.50, 18.75}},
    {"anthropic:claude-3-7-sonnet",  { 3.0,  15.0,  0.30,  3.75}},
    // OpenAI — prompt-caching discount on cache_read; cache_write charge only
    // on GPT-5.6+/GPT-6 (~1.25x input). Retired o1/gpt-4o prices kept.
    {"openai:gpt-6-astra",           {10.0,  50.0,  1.00, 12.50}},
    {"openai:gpt-6.1-sol",           { 2.0,  10.0,  0.10,  2.50}},
    {"openai:gpt-6-sol",             { 2.0,  10.0,  0.20,  2.50}},
    {"openai:gpt-6-luna",            { 0.1,   0.5,  0.01,  0.125}},
    {"openai:gpt-5.6-terra",         { 2.0,  12.0,  0.20,  2.50}},
    {"openai:gpt-5.6-luna",          { 0.2,   1.2,  0.02,  0.25}},
    {"openai:gpt-5.6",               { 4.0,  20.0,  0.40,  5.00}},
    {"openai:gpt-5.5",               { 5.0,  30.0,  0.50,  0.0}},
    {"openai:gpt-5.4",               { 2.5,  15.0,  0.25,  0.0}},
    {"openai:gpt-5.4-mini",          { 0.75,  4.5,  0.075, 0.0}},
    {"openai:gpt-5.4-nano",          { 0.2,   1.25, 0.02,  0.0}},
    {"openai:gpt-5.2",               { 1.75,  14.0, 0.175, 0.0}},
    {"openai:gpt-5-pro",             {15.0, 120.0,  0.00,  0.0}},
    {"openai:gpt-5-mini",            { 0.25,  2.0,  0.025, 0.0}},
    {"openai:gpt-5-nano",            { 0.05,  0.4,  0.005, 0.0}},
    {"openai:gpt-5",                 { 1.25, 10.0,  0.125, 0.0}},
    {"openai:gpt-4.1-mini",          { 0.4,   1.6,  0.10,  0.0}},
    {"openai:gpt-4.1",               { 2.0,   8.0,  0.50,  0.0}},
    {"openai:gpt-4o-mini",           { 0.15,  0.60, 0.075, 0.0}},
    {"openai:gpt-4o",                { 2.50, 10.0,  1.25,  0.0}},
    {"openai:o4-mini",               { 1.1,   4.4,  0.275, 0.0}},
    {"openai:o3-pro",                {20.0,  80.0,  0.00,  0.0}},
    {"openai:o3",                    { 2.0,   8.0,  0.50,  0.0}},
    {"openai:o1",                    {15.0,  60.0,  7.50,  0.0}},
    // Other labs — first-party list prices under the any-provider wildcard.
    // Peak/off-peak models (DeepSeek) use the PEAK rate so cost is never
    // under-reported; regional variants (xAI US +10%, Alibaba regions) are
    // not modeled.
    {"*:grok-4.20",                  { 1.25,  2.5,  0.20,  0.0}},
    {"*:grok-4.3",                   { 1.25,  2.5,  0.20,  0.0}},
    {"*:grok-4.5",                   { 2.0,   6.0,  0.30,  0.0}},
    {"*:grok-build-latest",          { 2.0,   6.0,  0.30,  0.0}},
    {"*:grok-4",                     { 2.0,   6.0,  0.50,  0.0}},
    {"*:grok-code-fast",             { 1.0,   2.0,  0.20,  0.0}},
    {"*:glm-5.3-flash",              { 0.15,  0.5,  0.03,  0.0}},
    {"*:glm-5.3",                    { 1.4,   4.4,  0.26,  0.0}},
    {"*:glm-5.2",                    { 1.4,   4.4,  0.26,  0.0}},
    {"*:glm-5.1",                    { 1.4,   4.4,  0.26,  0.0}},
    {"*:glm-5-code",                 { 1.2,   5.0,  0.30,  0.0}},
    {"*:glm-5",                      { 1.0,   3.2,  0.20,  0.0}},
    {"*:glm-4.7-flash",              { 0.0,   0.0,  0.00,  0.0}},  // free
    {"*:glm-4.7",                    { 0.6,   2.2,  0.11,  0.0}},
    {"*:glm-4.6",                    { 0.6,   2.2,  0.11,  0.0}},
    {"*:glm-4.5-flash",              { 0.0,   0.0,  0.00,  0.0}},  // free
    {"*:glm-4.5-airx",               { 1.1,   4.5,  0.00,  0.0}},
    {"*:glm-4.5-air",                { 0.2,   1.1,  0.00,  0.0}},
    {"*:glm-4.5-x",                  { 2.2,   8.9,  0.00,  0.0}},
    {"*:glm-4.5v",                   { 0.6,   1.8,  0.00,  0.0}},
    {"*:glm-4.5",                    { 0.6,   2.2,  0.00,  0.0}},
    {"*:kimi-k3",                    { 3.0,  15.0,  0.30,  0.0}},
    {"*:kimi-k2.7-code",             { 0.95,  4.0,  0.19,  0.0}},
    {"*:kimi-k2.6",                  { 0.95,  4.0,  0.16,  0.0}},
    {"*:kimi-k2.5",                  { 0.6,   3.0,  0.10,  0.0}},
    // Legacy Moonshot v1 ids (window sized by name suffix).
    {"*:moonshot-v1-128k",           { 2.0,   5.0,  0.00,  0.0}},
    {"*:moonshot-v1-32k",            { 1.0,   3.0,  0.00,  0.0}},
    {"*:moonshot-v1-8k",             { 0.2,   2.0,  0.00,  0.0}},
    {"*:minimax-m3",                 { 0.3,   1.2,  0.06,  0.0}},
    {"*:minimax-m2",                 { 0.3,   1.2,  0.03,  0.375}},
    {"*:deepseek-v4-pro",            { 1.32,  3.96, 0.044, 0.0}},
    {"*:deepseek-v4-flash",          { 0.44,  1.32, 0.014, 0.0}},
    // Retired 2026-07-24 ids, kept for old sessions; peak rate so cost is
    // never under-reported (MODEL_NUMBERS §6).
    {"*:deepseek-chat",              { 0.28,  0.42, 0.028, 0.0}},
    {"*:deepseek-reasoner",          { 0.28,  0.42, 0.028, 0.0}},
    {"*:mistral-large-latest",       { 0.5,   1.5,  0.05,  0.0}},
    {"*:mistral-medium-latest",      { 1.5,   7.5,  0.15,  0.0}},
    {"*:mistral-small-latest",       { 0.15,  0.6,  0.015, 0.0}},
    {"*:ministral-14b-latest",       { 0.2,   0.2,  0.02,  0.0}},
    {"*:ministral-8b-latest",        { 0.15,  0.15, 0.015, 0.0}},
    {"*:ministral-3b-latest",        { 0.1,   0.1,  0.01,  0.0}},
    {"*:devstral",                   { 0.4,   2.0,  0.04,  0.0}},
    {"*:devstral-small",             { 0.1,   0.3,  0.01,  0.0}},
    {"*:codestral",                  { 0.3,   0.9,  0.03,  0.0}},
    {"*:pixtral-large",              { 2.0,   6.0,  0.20,  0.0}},
    {"*:open-mistral-nemo",          { 0.3,   0.3,  0.03,  0.0}},
    {"*:muse-spark-1.3-contributor", { 0.1,   0.2,  0.002, 0.0}},
    {"*:muse-spark",                 { 1.25,  4.25, 0.15,  0.0}},
    {"*:qwen3.8-max",                { 2.0,   6.0,  0.25,  0.0}},
    // qwen3.8-flash / -omni-flash share the flash rate (the bare qwen3.8
    // prefix below covers both ids).
    {"*:qwen3.8-omni-flash",         { 0.15,  0.47, 0.016, 0.0}},
    {"*:qwen3.8",                    { 0.15,  0.47, 0.016, 0.2}},
    {"*:qwen3.7-max",                { 2.5,   7.5,  0.50,  0.0}},
    // Input-tiered models: compute_step_cost only consults kTiered after
    // lookup_pricing hits a base, so this row is required even though the
    // ladder's first tier duplicates it.
    {"*:qwen3.5-plus",               { 0.4,   2.4,  0.00,  0.0}},
    {"*:qwen3-max",                  { 1.2,   6.0,  0.00,  0.0}},
    {"*:qwen3-coder-plus",           { 1.0,   5.0,  0.10,  0.0}},
    {"*:qwen3-coder-flash",          { 0.3,   1.5,  0.08,  0.0}},
    {"*:qwen-plus",                  { 0.4,   1.2,  0.00,  0.0}},
    {"*:qwen-flash",                 { 0.05,  0.4,  0.00,  0.0}},
    {"*:qwen3-vl-plus",              { 0.2,   1.6,  0.00,  0.0}},
    {"*:qwen-turbo",                 { 0.05,  0.2,  0.00,  0.0}},
    {"*:qwq-plus",                   { 0.8,   2.4,  0.00,  0.0}},
    {"*:doubao-seed-2-1-pro",        { 0.8625, 4.3125, 0.1725, 0.0}},
    {"*:doubao-seed-2-1-turbo",      { 0.4313, 2.1562, 0.0862, 0.0}},
    {"*:doubao-seed-2-0-pro",        { 0.46,  2.3,  0.00,  0.0}},
    {"*:doubao-seed-2-0-lite",       { 0.087, 0.52, 0.00,  0.0}},
    {"*:amazon.nova-2-pro",          { 2.1875, 17.5,  0.5469, 0.0}},
    {"*:amazon.nova-2",              { 0.3,    2.5,   0.075,  0.0}},
    {"*:mimo-v2.6-pro",              { 0.435, 0.87, 0.0036, 0.0}},
    {"*:mimo-v2.6",                  { 0.14,   0.28,  0.0028, 0.0}},
    // Cohere — command-a-plus prices as blank/$0 in the source: unpriced.
    {"*:command-a-03",               { 2.5,  10.0,  0.00,  0.0}},
    {"*:command-r-plus",             { 2.5,  10.0,  0.00,  0.0}},
};

// ---- tier ladder (long-context surcharges + input-size tiers) -------------
// Ordered breakpoints; tier is chosen from total prompt tokens (input +
// cache_read + cache_write) and the WHOLE request is billed at that tier
// (verified whole-request only for xAI; convention elsewhere). The first
// entry's price duplicates kBuiltin so a plain lookup bills correctly.
struct PriceTier {
    int up_to_prompt_tokens;   // inclusive upper bound; 0 = unbounded (last)
    ModelPricing price;
};
static const struct { const char* model_prefix; PriceTier tiers[4]; } kTiered[] = {
    {"claude-sonnet-4-5", {
        {200000, { 3.0, 15.0, 0.30, 3.75}},
        {0,      { 6.0, 22.5, 0.60, 3.75}},  // >200K long-context rate
    }},
    {"gpt-6-astra", {
        {272000, {10.0, 50.0, 1.00, 12.50}},
        {0,      {20.0, 75.0, 2.00, 12.50}},
    }},
    {"gpt-6.1-sol", {
        {272000, { 2.0, 10.0, 0.10, 2.50}},
        {0,      { 4.0, 15.0, 0.20, 2.50}},
    }},
    {"gpt-6-sol", {
        {272000, { 2.0, 10.0, 0.20, 2.50}},
        {0,      { 4.0, 15.0, 0.40, 2.50}},
    }},
    {"gpt-6-luna", {
        {272000, { 0.1,  0.5,  0.010, 0.125}},
        {0,      { 0.2,  0.75, 0.020, 0.125}},
    }},
    {"gpt-5.6-terra", {
        {272000, { 2.0, 12.0, 0.20, 2.50}},
        {0,      { 4.0, 18.0, 0.40, 2.50}},
    }},
    {"gpt-5.6-luna", {
        {272000, { 0.2,  1.2,  0.020, 0.25}},
        {0,      { 0.4,  1.8,  0.040, 0.25}},
    }},
    {"gpt-5.6", {
        {272000, { 4.0, 20.0, 0.40, 5.00}},
        {0,      { 8.0, 30.0, 0.80, 5.00}},
    }},
    {"gpt-5.5", {
        {272000, { 5.0, 30.0, 0.50, 0.0}},
        {0,      {10.0, 45.0, 1.00, 0.0}},
    }},
    {"gpt-5.4", {
        {272000, { 2.5, 15.0, 0.25, 0.0}},
        {0,      { 5.0, 22.5, 0.50, 0.0}},
    }},
    // mini/nano are flat (no >272K surcharge): single-sentinel tiers shadow
    // the family ladder so the longest-prefix match can't leak 5/22.5 onto
    // them at long prompts.
    {"gpt-5.4-mini", {
        {0,      { 0.75,  4.5, 0.075, 0.0}},
    }},
    {"gpt-5.4-nano", {
        {0,      { 0.2,  1.25, 0.02, 0.0}},
    }},
    {"gemini-3.1-pro", {
        {200000, { 2.0, 12.0, 0.20, 0.0}},
        {0,      { 4.0, 18.0, 0.40, 0.0}},
    }},
    {"gemini-2.5-pro", {
        {200000, { 1.25, 10.0, 0.125, 0.0}},
        {0,      { 2.5,  15.0, 0.25,  0.0}},
    }},
    // xAI thresholds are >= (at) rather than >; one token over the boundary
    // bills the same either way in practice.
    {"grok-4.20", {
        {199999, { 1.25,  2.5,  0.20, 0.0}},
        {0,      { 2.5,   5.0,  0.40, 0.0}},
    }},
    {"grok-4.3", {
        {199999, { 1.25,  2.5,  0.20, 0.0}},
        {0,      { 2.5,   5.0,  0.40, 0.0}},
    }},
    // 4.5/4.6/4.7 share the >=200K surcharge but 4.5 has the cheaper
    // cache-read (0.3/0.6 vs 0.5/1.0) — it needs its own ladder.
    {"grok-4.5", {
        {199999, { 2.0,   6.0,  0.30, 0.0}},
        {0,      { 4.0,  12.0,  0.60, 0.0}},
    }},
    {"grok-build-latest", {
        {199999, { 2.0,   6.0,  0.30, 0.0}},
        {0,      { 4.0,  12.0,  0.60, 0.0}},
    }},
    {"grok-4", {
        {199999, { 2.0,   6.0,  0.50, 0.0}},
        {0,      { 4.0,  12.0,  1.00, 0.0}},
    }},
    {"minimax-m3", {
        {512000, { 0.3,  1.2,  0.06, 0.0}},
        {0,      { 0.6,  2.4,  0.12, 0.0}},
    }},
    // Alibaba input-size tiers (Singapore/international region).
    {"qwen3-coder-plus", {
        { 32000, { 1.0,  5.0,  0.10, 0.0}},
        {128000, { 1.8,  9.0,  0.18, 0.0}},
        {256000, { 3.0, 15.0,  0.30, 0.0}},
        {0,      { 6.0, 60.0,  0.60, 0.0}},
    }},
    {"qwen3-coder-flash", {
        { 32000, { 0.3,  1.5,  0.08, 0.0}},
        {128000, { 0.5,  2.5,  0.12, 0.0}},
        {256000, { 0.8,  4.0,  0.20, 0.0}},
        {0,      { 1.6,  9.6,  0.40, 0.0}},
    }},
    {"qwen3-max", {
        { 32000, { 1.2,  6.0,  0.0, 0.0}},
        {128000, { 2.4, 12.0,  0.0, 0.0}},
        {0,      { 3.0, 15.0,  0.0, 0.0}},  // top tier runs to the 252K window
    }},
    {"qwen3-vl-plus", {
        { 32000, { 0.2,  1.6,  0.0, 0.0}},
        {128000, { 0.3,  2.4,  0.0, 0.0}},
        {0,      { 0.6,  4.8,  0.0, 0.0}},
    }},
    {"qwen3.5-plus", {
        {256000, { 0.4,  2.4,  0.0, 0.0}},
        {0,      { 0.5,  3.0,  0.0, 0.0}},
    }},
    {"qwen-plus", {
        {256000, { 0.4,  1.2,  0.0, 0.0}},
        {0,      { 1.2,  3.6,  0.0, 0.0}},
    }},
    {"qwen-flash", {
        {256000, { 0.05, 0.4,  0.0, 0.0}},
        {0,      { 0.25, 2.0,  0.0, 0.0}},
    }},
    // ByteDance input-size tiers.
    {"doubao-seed-2-0-pro", {
        { 32000, { 0.46, 2.3,  0.0, 0.0}},
        {128000, { 0.7,  3.5,  0.0, 0.0}},
        {0,      { 1.4,  7.0,  0.0, 0.0}},
    }},
    {"doubao-seed-2-0-lite", {
        { 32000, { 0.087, 0.52, 0.0, 0.0}},
        {128000, { 0.13,  0.78, 0.0, 0.0}},
        {0,      { 0.26,  1.6,  0.0, 0.0}},
    }},
};

// Gemini flat entries live in kBuiltin via their own keys for lookup; the
// surcharge models above are the only tiered ones.
static const struct { const char* key; ModelPricing p; } kBuiltinExtra[] = {
    {"*:gemini-3.8-flash",  { 0.75, 3.75,  0.075, 0.0}},  // intro until 2026-12-31
    {"*:gemini-3.5-flash",  { 1.5,  9.0,   0.15,  0.0}},
    {"*:gemini-3.1-pro",    { 2.0,  12.0,  0.20,  0.0}},  // surcharge >200K
    {"*:gemini-2.5-pro",    { 1.25, 10.0,  0.125, 0.0}},  // surcharge >200K
    {"*:gemini-2.5-flash",  { 0.3,  2.5,   0.03,  0.0}},
    {"*:gemini-3-flash",    { 0.5,  3.0,   0.05,  0.0}},
    {"*:gemini-3.5-flash-lite", { 0.3, 2.5, 0.03, 0.0}},
};

static std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
    return s;
}

static bool starts_with(const std::string& s, const char* prefix) {
    size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

// Model-id normalization (aggregators/clouds wrap the id): lowercase, drop
// leading region prefixes ("us.", "eu.", "global."), drop vendor wrapper
// segments that are never part of the model name ("anthropic.", "meta.",
// a leading "vendor/" path segment), drop ":free"/":thinking"/":beta"
// suffixes. "amazon." is KEPT — "amazon.nova-..." is the genuine Bedrock
// model id, not a wrapper.
static std::string normalize_model_id(const std::string& raw, bool& is_free) {
    std::string m = lower(raw);
    is_free = false;
    // ":free" / ":thinking" / ":beta" suffixes (OpenRouter variants)
    static const char* kSuffixes[] = {":free", ":thinking", ":beta"};
    for (const char* suf : kSuffixes) {
        size_t n = std::strlen(suf);
        if (m.size() > n && m.compare(m.size() - n, n, suf) == 0) {
            if (std::string(suf) == ":free") is_free = true;
            m.erase(m.size() - n);
        }
    }
    // Bedrock inference-profile region prefixes
    for (const char* p : {"us.", "eu.", "global."}) {
        if (starts_with(m, p)) { m.erase(0, std::strlen(p)); break; }
    }
    // Vendor wrapper prefixes that are never part of the model name
    for (const char* p : {"anthropic.", "meta."}) {
        if (starts_with(m, p)) { m.erase(0, std::strlen(p)); break; }
    }
    // Aggregator path prefix ("z-ai/glm-5.3", "moonshotai/kimi-k3")
    size_t slash = m.find('/');
    if (slash != std::string::npos && slash < m.size() - 1)
        m.erase(0, slash + 1);
    return m;
}

// Longest-prefix match of "kind:model" against a table of {key, p} entries.
// `len_out` receives the matched MODEL-part length (key minus "kind:").
template <typename Entries>
static const ModelPricing* prefix_match(const std::string& kind,
                                        const std::string& model,
                                        const Entries& entries,
                                        size_t* len_out = nullptr) {
    const std::string full = kind + ":" + model;
    const ModelPricing* best = nullptr;
    size_t best_len = 0;
    for (const auto& e : entries) {
        std::string k = lower(e.key);
        if (full.rfind(k, 0) == 0 && k.size() > best_len) {
            // Only structured keys ("x:y") participate; a bare model key
            // without ':' is matched by the caller's wildcard pass.
            if (k.find(':') == std::string::npos) continue;
            best = &e.p;
            best_len = k.size();
        }
    }
    if (len_out) *len_out = best ? best_len - (kind.size() + 1) : 0;
    return best;
}

bool is_local_provider_kind(const std::string& provider_kind) {
    return provider_kind == "ollama" || provider_kind == "vllm"
        || provider_kind == "lmstudio" || provider_kind == "llamacpp";
}

const ModelPricing* lookup_pricing(
    const std::string& provider_id,
    const std::string& model_id,
    const std::map<std::string, ModelPricing>& overrides)
{
    return lookup_pricing(provider_id, provider_id, model_id, overrides);
}

// Full resolution; `from_override` reports whether a user entry won (its
// flat price then replaces any built-in tier ladder).
static const ModelPricing* resolve_pricing(
    const std::string& provider_id,
    const std::string& provider_kind,
    const std::string& model_id,
    const std::map<std::string, ModelPricing>& overrides,
    bool& from_override)
{
    from_override = false;
    bool free_variant = false;
    const std::string model = normalize_model_id(model_id, free_variant);
    if (free_variant) {
        static const ModelPricing kFree{};
        return &kFree;
    }
    // A local server (user's own Ollama/vLLM/LM Studio/llama.cpp) is free
    // regardless of the model id it serves.
    if (is_local_provider_kind(provider_kind)) {
        static const ModelPricing kLocalFree{};
        return &kLocalFree;
    }

    // Built-ins: provider-kind key first, then any-provider wildcard. The
    // matched model-part length competes with the user's entries below.
    const std::string& kind = provider_kind.empty() ? provider_id : provider_kind;
    const ModelPricing* builtin = nullptr;
    size_t builtin_len = 0;
    {
        size_t len = 0;
        if ((builtin = prefix_match(kind, model, kBuiltin, &len))) builtin_len = len;
        else if ((builtin = prefix_match(kind, model, kBuiltinExtra, &len))) builtin_len = len;
        else if ((builtin = prefix_match("*", model, kBuiltin, &len))) builtin_len = len;
        else if ((builtin = prefix_match("*", model, kBuiltinExtra, &len))) builtin_len = len;
    }

    // Config overrides: keyed by provider id, provider kind, or bare model
    // (prefixes allowed). They join the built-ins as one prefix database:
    // the override wins when its matched model part is at least as long as
    // the built-in's (a user can re-key a built-in by reusing its key; a
    // short "gpt-5" entry doesn't shadow the built-in "gpt-5.5" row).
    const std::string prefixes[3] = {
        lower(provider_id) + ":",
        lower(provider_kind) + ":",
        "",
    };
    for (const std::string& pre : prefixes) {
        const std::string full = pre + model;
        const ModelPricing* best = nullptr;
        size_t best_len = 0;
        bool any = false;
        for (auto& [k, v] : overrides) {
            std::string k_lc = lower(k);
            if (k_lc.size() < pre.size()) continue;
            if (full.rfind(k_lc, 0) == 0 && (!any || k_lc.size() >= best_len)) {
                best = &v;
                best_len = k_lc.size();
                any = true;
            }
        }
        if (best && best_len - pre.size() >= builtin_len) {
            from_override = true;
            return best;
        }
    }
    return builtin;
}

const ModelPricing* lookup_pricing(
    const std::string& provider_id,
    const std::string& provider_kind,
    const std::string& model_id,
    const std::map<std::string, ModelPricing>& overrides)
{
    bool from_override = false;
    return resolve_pricing(provider_id, provider_kind, model_id, overrides,
                           from_override);
}

TokenUsage estimate_inflight_usage(const TokenUsage& reported,
                                   size_t streamed_chars,
                                   int est_request_tokens,
                                   int cached_prefix_tokens) {
    TokenUsage u = reported;
    if (reported.total_input() <= 0) {
        const int total  = std::max(0, est_request_tokens);
        const int cached = std::clamp(cached_prefix_tokens, 0, total);
        u.input       = total - cached;
        u.cache_read  = cached;
        u.cache_write = 0;
    }
    const int streamed = static_cast<int>((streamed_chars + 3) / 4);
    u.output = std::max(reported.output, streamed);
    return u;
}

int cache_hit_percent(const TokenUsage& u) {
    // 64-bit: session totals are int and cache_read * 100 overflows int32
    // past ~21M cached tokens, which a long session easily reaches.
    const long long total = static_cast<long long>(std::max(0, u.input))
                          + std::max(0, u.cache_read)
                          + std::max(0, u.cache_write);
    if (total <= 0) return -1;
    return static_cast<int>(std::max(0, u.cache_read) * 100LL / total);
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

double compute_step_cost(const TokenUsage& usage,
                         const std::string& provider_id,
                         const std::string& provider_kind,
                         const std::string& model_id,
                         const std::map<std::string, ModelPricing>& overrides) {
    bool from_override = false;
    const ModelPricing* base = resolve_pricing(provider_id, provider_kind,
                                               model_id, overrides,
                                               from_override);
    if (!base) return 0.0;
    // A user price is flat: it replaces the built-in tier ladder too
    // (otherwise an override on a tiered model would never take effect).
    if (from_override) return compute_cost(usage, *base);

    // Tier ladder: the longest-prefix entry whose thresholds cover the
    // request's total prompt size. Tier overrides the base price only when
    // the model actually has a ladder; flat models bill at `base`.
    bool free_variant = false;
    std::string model = normalize_model_id(model_id, free_variant);
    const PriceTier* tiers = nullptr;
    size_t tiers_count = 0;
    size_t best_len = 0;
    for (const auto& e : kTiered) {
        std::string p = e.model_prefix;
        if (model.rfind(p, 0) == 0 && p.size() > best_len) {
            tiers = e.tiers;
            tiers_count = sizeof(e.tiers) / sizeof(e.tiers[0]);
            best_len = p.size();
        }
    }
    if (tiers) {
        const int prompt_tokens = usage.total_input();
        for (size_t i = 0; i < tiers_count; ++i) {
            if (tiers[i].up_to_prompt_tokens == 0
                    || prompt_tokens <= tiers[i].up_to_prompt_tokens)
                return compute_cost(usage, tiers[i].price);
        }
    }
    return compute_cost(usage, *base);
}

std::vector<std::pair<std::string, ModelPricing>> builtin_pricing_entries() {
    std::vector<std::pair<std::string, ModelPricing>> out;
    for (const auto& e : kBuiltin) out.emplace_back(e.key, e.p);
    for (const auto& e : kBuiltinExtra) out.emplace_back(e.key, e.p);
    return out;
}

bool has_price_tiers(const std::string& model_id) {
    bool free_variant = false;
    const std::string model = normalize_model_id(model_id, free_variant);
    // Longest ladder wins (as in compute_step_cost); a one-tier ladder
    // (gpt-5.4-mini, shielding it from the gpt-5.4 surcharge) is flat.
    const PriceTier* tiers = nullptr;
    size_t best_len = 0;
    for (const auto& e : kTiered) {
        const std::string p = e.model_prefix;
        if (model.rfind(p, 0) == 0 && p.size() > best_len) {
            tiers = e.tiers;
            best_len = p.size();
        }
    }
    return tiers && tiers[0].up_to_prompt_tokens != 0;
}

} // namespace haicode
