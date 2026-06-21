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
    int window = get_context_window(provider_id, model_id, config_overrides);
    if (window > 0) return window;
    if (provider)
        return provider->get_model_context(model_id);
    return 0;
}

} // namespace haicode
