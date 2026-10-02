#include <haicode/model_info.h>
#include <climits>

// last_verified: 2026-10-02 against MODEL_NUMBERS.md (repo root).

namespace haicode {

// (prefix, context_window). Prefix is matched against the start of model_id
// (case-insensitive). Longest matching prefix wins. Keep prefixes as short as
// possible while still uniquely identifying a context-window family — but
// where a family's window changed between versions (Claude Opus 4.5 = 200K
// while 4.6+ = 1M), list explicit per-version entries so a short prefix can't
// hand the older window to newer models. Underestimating a window is safe
// (compaction just triggers early); overestimating overflows the real window.
static const struct { const char* prefix; int window; } kKnownModels[] = {
    // Anthropic — 5.x / Fable / Mythos / Opus 4.6+ are 1M-window.
    {"claude-fable-5",      1000000},
    {"claude-mythos",       1000000},
    {"claude-opus-5",       1000000},
    {"claude-opus-4-8",     1000000},
    {"claude-opus-4-7",     1000000},
    {"claude-opus-4-6",     1000000},
    {"claude-sonnet-5",     1000000},
    {"claude-sonnet-4-6",   1000000},
    {"claude-sonnet-4-5",   1000000},  // long-context surcharge >200K
    {"claude-opus-4",       200000},   // 4, 4-1, 4-5
    {"claude-sonnet-4",     200000},   // plain Sonnet 4
    {"claude-haiku-4-5",    200000},
    {"claude-3-7-sonnet",   200000},
    {"claude-3-7-haiku",    200000},
    {"claude-3-5-sonnet",   200000},
    {"claude-3-5-haiku",    200000},
    // OpenAI — 1.05M rows carry a 922K max-input cap; usable_input_tokens
    // subtracts the output reserve itself, so the total window is listed.
    {"gpt-6",               1050000},
    {"gpt-5.6",             1050000},
    {"gpt-5.5",             1050000},
    {"gpt-5.4",             1050000},
    {"gpt-5-pro",           400000},
    {"gpt-5",               400000},
    {"gpt-4.1",             1047576},
    {"gpt-4o",              128000},
    {"gpt-4-turbo",         128000},
    {"gpt-4",               8192},
    {"gpt-3.5-turbo",       16385},
    {"o3",                  200000},
    {"o4-mini",             200000},
    {"o1-mini",             128000},
    {"o1",                  200000},
    // Google
    {"gemini-3",            1048576},
    {"gemini-2.5",          1048576},
    {"gemma-4",             262144},
    {"gemma-3",             131072},
    // xAI
    {"grok-4.3",            1000000},
    {"grok-4.20",           1000000},
    {"grok-4",              500000},
    {"grok-build",          500000},
    {"grok-code-fast",      256000},
    // Z.ai (GLM) — 5.2+ are 1M; 5/5.1/5-code and 4.x are 200K; 4.5 128K.
    {"glm-5.3",             1048576},
    {"glm-5.2",             1000000},
    {"glm-5",               200000},
    {"glm-4.7",             200000},
    {"glm-4.5",             128000},
    // Moonshot (Kimi)
    {"kimi-k3",             1048576},
    {"kimi-k2",             262144},
    {"moonshot-v1-128k",    131072},
    {"moonshot-v1-32k",     32768},
    {"moonshot-v1-8k",      8192},
    // MiniMax
    {"minimax-m3",          1000000},
    {"minimax-m2.5",        1000000},
    {"minimax-m2.1",        1000000},
    {"minimax-m2",          200000},
    // DeepSeek
    {"deepseek-v4",         1000000},
    {"deepseek-chat",       131072},
    {"deepseek-reasoner",   131072},
    // Mistral
    {"mistral-large",       262144},
    {"mistral-medium",      262144},
    {"mistral-small",       262144},
    {"ministral-14b",       262144},
    {"ministral-8b",        262144},
    {"ministral-3b",        131072},
    {"devstral",            256000},
    {"codestral",           128000},
    {"pixtral-large",       128000},
    {"open-mistral-nemo",   128000},
    // Alibaba (Qwen) first-party
    {"qwen3.8",             991808},
    {"qwen3.7",             991808},
    {"qwen3.5-plus",        991808},
    {"qwen3-coder",         997952},
    {"qwen3-vl",            260096},
    {"qwen3-next",          262144},
    {"qwen3-max",           258048},
    {"qwen-plus",           997952},
    {"qwen-flash",          997952},
    {"qwen-turbo",          1000000},
    {"qwq-plus",            98304},
    // Meta Muse (closed-weight first-party) + open-weight Llama 4
    {"muse-spark",          1048576},
    {"muse-glimmer",        131072},
    {"llama-4-maverick",    1048576},
    {"llama-4-scout",       10000000},
    // Nvidia Nemotron (native windows; hosts commonly serve less —
    // discovery on flavored servers overrides this table)
    {"nemotron-3-ultra",    1000000},
    {"nemotron-3.5",        262144},
    {"nemotron-3",          262144},
    {"nvidia-nemotron",     131072},
    // Others
    {"mimo-v2.6",           1048576},
    {"doubao-seed-2-1",     256000},
    {"doubao-seed-2-0-pro", 256000},
    {"doubao-seed-2-0-lite",256000},
    {"amazon.nova-2",       1000000},
    {"command-a-03",        256000},
    {"command-a-plus",      128000},
    {"command-r-plus",      128000},
    {"llama-3.3",           128000},
    {"llama-3.1",           128000},
    {"llama-3",             8192},
};

// (prefix, vision). Prefix matched case-insensitively like kKnownModels.
// Fail-closed below: unknown models are treated as text-only. `?` rows in the
// source table are deliberately absent (unknown ≠ text-only forever, but the
// screenshot tool must not be offered on an unverified model).
static const struct { const char* prefix; bool vision; } kVisionModels[] = {
    // Anthropic — vision from Claude 3 onward.
    {"claude-3",          true},
    {"claude-opus",       true},
    {"claude-sonnet",     true},
    {"claude-haiku",      true},
    {"claude-fable",      true},
    {"claude-mythos",     true},
    // OpenAI
    {"gpt-4o",            true},
    {"chatgpt-4o",        true},
    {"gpt-4.1",           true},
    {"gpt-4-turbo",       true},
    {"o1",                true},
    {"o3",                true},
    {"o4",                true},
    {"gpt-5",             true},
    {"gpt-6",             true},
    // Google
    {"gemini",            true},
    {"gemma-3",           true},
    {"gemma-4",           true},
    // xAI — Grok 4.x accepts images.
    {"grok-4",            true},
    // Z.ai — only the Flash tiers are confirmed vision.
    {"glm-5.3-flash",     true},
    {"glm-4.5v",          true},
    // Moonshot — K2.5 and later.
    {"kimi-k3",           true},
    {"kimi-k2.5",         true},
    {"kimi-k2.6",         true},
    {"kimi-k2.7",         true},
    // MiniMax / DeepSeek (V4 Pro is text-only: no entry, fails closed;
    // Flash accepts images)
    {"minimax-m3",        true},
    {"deepseek-v4-flash", true},
    // Mistral — Large/Medium/Small/Ministral confirmed; coding models unverified.
    {"mistral-large",     true},
    {"mistral-medium",    true},
    {"mistral-small",     true},
    {"ministral",         true},
    {"pixtral",           true},
    // Alibaba
    {"qwen3.8",           true},
    {"qwen-vl",           true},
    {"qwen2-vl",          true},
    {"qwen2.5-vl",        true},
    {"qwen3-vl",          true},
    // Meta Muse + Llama 4
    {"muse-spark",        true},
    {"muse-glimmer",      true},
    // Xiaomi / ByteDance / Amazon
    {"mimo-v2.6",         true},
    {"doubao-seed",       true},
    {"amazon.nova-2",     true},
    // Local / open vision models
    {"llava",             true},
    {"llama3.2-vision",   true},
    {"llama-3.2-vision",  true},
    {"llama4",            true},
};

// (prefix, max output tokens). Hard per-response output caps for first-party
// models; used to clamp an explicit max_tokens override so it cannot 400.
// Absent prefix = no clamp. Where the source only published "max out =
// window" (xAI, Kimi K2.x, Mistral, ByteDance), there is no verified separate
// cap, so no entry — the streaming default stays kDefaultMaxTokens.
static const struct { const char* prefix; int max_out; } kMaxOutput[] = {
    // Anthropic
    {"claude-fable-5",     128000},
    {"claude-mythos",      128000},
    {"claude-opus-5",      128000},
    {"claude-opus-4-6",    128000},
    {"claude-sonnet-5",    128000},
    {"claude-sonnet-4-6",  128000},
    {"claude-opus-4",       64000},  // 4-5
    {"claude-sonnet-4-5",   64000},
    {"claude-haiku-4-5",    64000},
    // 3-x: 8192 without thinking config (HaiCode never sends one for
    // budgeted models); 3-7 could reach 64K with explicit budgets.
    {"claude-3-7-sonnet",    8192},
    {"claude-3-7-haiku",     8192},
    {"claude-3-5-sonnet",    8192},
    {"claude-3-5-haiku",     8192},
    // OpenAI
    {"gpt-6",              128000},
    {"gpt-5.6",            128000},
    {"gpt-5.5",            128000},
    {"gpt-5.4",            128000},
    {"gpt-5-pro",          272000},
    {"gpt-5",              128000},
    {"gpt-4.1",             32768},
    {"gpt-4o",              16384},
    {"o3",                 100000},
    {"o4-mini",            100000},
    {"o1",                 100000},
    // Google
    {"gemini-3",            65536},
    {"gemini-2.5",          65536},
    // Z.ai / Moonshot / MiniMax / DeepSeek / Meta Muse / Amazon
    {"glm-5.3",            128000},
    {"glm-5.2",            128000},
    {"glm-5",              128000},
    {"glm-4.7",            128000},
    {"kimi-k3",            131072},
    {"minimax-m3",         524288},  // hard cap; 131072 recommended
    {"deepseek-v4",        393216},
    {"muse-spark",         131072},
    {"amazon.nova-2",       64000},
};

static bool starts_with_ci(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        char a = s[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') a += ('a' - 'A');
        if (b >= 'A' && b <= 'Z') b += ('a' - 'A');
        if (a != b) return false;
    }
    return true;
}

int get_context_window(const std::string& /*provider_id*/,
                       const std::string& model_id,
                       const std::map<std::string, int>& config_overrides) {
    // 1. Exact-match config override.
    auto it = config_overrides.find(model_id);
    if (it != config_overrides.end() && it->second > 0)
        return it->second;

    // 2. Longest-prefix hardcoded match.
    const std::string* best_prefix = nullptr;
    int best_window = 0;
    for (auto& entry : kKnownModels) {
        std::string p = entry.prefix;
        if (starts_with_ci(model_id, p)) {
            if (!best_prefix || p.size() > best_prefix->size()) {
                best_prefix = &p;
                best_window = entry.window;
            }
        }
    }
    return best_window;  // 0 if no prefix matched
}

int get_context_window(const std::string& provider_id,
                       const std::string& model_id,
                       const std::map<std::string, int>& config_overrides,
                       const Provider* provider) {
    // 1. Exact-match config override wins over everything.
    auto it = config_overrides.find(model_id);
    if (it != config_overrides.end() && it->second > 0)
        return it->second;

    // 2. Live provider discovery outranks the hardcoded prefix table: the
    // server's reported window is authoritative when available (a local
    // vLLM/Ollama instance may be configured with a smaller max_model_len
    // than the model's nominal size). Discovery is cached inside the
    // provider after the first fetch, and providers without an override
    // return 0 immediately with no network I/O.
    if (provider) {
        int discovered = provider->get_model_context(model_id);
        if (discovered > 0) return discovered;
    }

    // 3. Hardcoded prefix-table match. Reuse the 3-arg overload with an
    // empty override map (the override was already checked in step 1).
    return get_context_window(provider_id, model_id, {});
}

bool model_supports_vision(const std::string& model_id,
                           const std::map<std::string, bool>& config_overrides) {
    // 1. Exact-match config override (explicit false wins over the table).
    auto it = config_overrides.find(model_id);
    if (it != config_overrides.end())
        return it->second;

    // 2. Longest-prefix hardcoded match.
    const char* best_prefix = nullptr;
    bool best_vision = false;
    for (auto& entry : kVisionModels) {
        std::string p = entry.prefix;
        if (starts_with_ci(model_id, p)) {
            if (!best_prefix || p.size() > strlen(best_prefix)) {
                best_prefix = entry.prefix;
                best_vision = entry.vision;
            }
        }
    }
    if (best_prefix) return best_vision;

    // 3. Fail-closed: unknown models are assumed text-only.
    return false;
}

int get_max_output_tokens(const std::string& model_id) {
    const std::string* best_prefix = nullptr;
    int best_out = 0;
    for (auto& entry : kMaxOutput) {
        std::string p = entry.prefix;
        if (starts_with_ci(model_id, p)) {
            if (!best_prefix || p.size() > best_prefix->size()) {
                best_prefix = &p;
                best_out = entry.max_out;
            }
        }
    }
    return best_out;  // 0 = no known cap
}

int clamp_max_tokens(const std::string& model_id, int requested) {
    if (requested <= 0) return requested;
    int cap = get_max_output_tokens(model_id);
    if (cap > 0 && requested > cap) return cap;
    return requested;
}

} // namespace haicode
