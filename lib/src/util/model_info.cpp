#include <haicode/model_info.h>

namespace haicode {

// (prefix, context_window). Prefix is matched against the start of model_id
// (case-insensitive). Longest matching prefix wins. Keep prefixes as short as
// possible while still uniquely identifying a context-window family — e.g.
// "claude-opus-4" matches claude-opus-4-1, claude-opus-4-5, claude-opus-4-7,
// all of which currently share a 200k window.
static const struct { const char* prefix; int window; } kKnownModels[] = {
    // Anthropic
    {"claude-opus-4",      200000},
    {"claude-sonnet-4",    200000},
    {"claude-haiku-4",     200000},
    {"claude-3-7-sonnet",  200000},
    {"claude-3-7-haiku",   200000},
    {"claude-3-5-sonnet",  200000},
    {"claude-3-5-haiku",   200000},
    // OpenAI
    {"gpt-4o-mini",        128000},
    {"gpt-4o",             128000},
    {"gpt-4-turbo",        128000},
    {"gpt-4",                8192},
    {"gpt-3.5-turbo",      16385},
    {"o4-mini",            200000},
    {"o3-mini",            200000},
    {"o3",                 200000},
    {"o1-mini",            128000},
    {"o1",                 200000},
    {"gpt-5",              200000},
    // Meta
    {"llama-3.3",          128000},
    {"llama-3.1",          128000},
    {"llama-3",             8192},
};

// (prefix, vision). Prefix matched case-insensitively like kKnownModels.
// Covers Claude 3+ (all multimodal), OpenAI's image-capable generations
// (note: plain gpt-4 / gpt-3.5 are text-only and deliberately absent),
// Google Gemini, and common local vision models (llava, qwen*-vl,
// llama3.2-vision, llama4).
static const struct { const char* prefix; bool vision; } kVisionModels[] = {
    // Anthropic — vision from Claude 3 onward.
    {"claude-3",          true},
    {"claude-opus",       true},
    {"claude-sonnet",     true},
    {"claude-haiku",      true},
    // OpenAI
    {"gpt-4o",            true},
    {"chatgpt-4o",        true},
    {"gpt-4.1",           true},
    {"gpt-4-turbo",       true},
    {"o1",                true},
    {"o3",                true},
    {"o4",                true},
    {"gpt-5",             true},
    // Google
    {"gemini",            true},
    // Local / open vision models
    {"llava",             true},
    {"qwen-vl",           true},
    {"qwen2-vl",          true},
    {"qwen2.5-vl",        true},
    {"qwen3-vl",          true},
    {"llama3.2-vision",   true},
    {"llama-3.2-vision",  true},
    {"llama4",            true},
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
    // (e.g. Anthropic) return 0 immediately with no network I/O.
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

} // namespace haicode
