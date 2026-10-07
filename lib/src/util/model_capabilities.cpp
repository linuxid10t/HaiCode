#include <haicode/model_capabilities.h>

namespace haicode {

namespace {

bool starts_with_ci(const std::string& s, const char* prefix) {
    size_t n = 0;
    while (prefix[n]) ++n;
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = s[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') a += ('a' - 'A');
        if (b >= 'A' && b <= 'Z') b += ('a' - 'A');
        if (a != b) return false;
    }
    return true;
}

bool contains_ci(const std::string& s, const char* needle) {
    for (size_t i = 0; i < s.size(); ++i)
        if (starts_with_ci(s.substr(i), needle)) return true;
    return false;
}

struct CapsEntry {
    const char* prefix;
    uint8_t effort_mask;   // 0 = no effort support
    ThinkingMode thinking;
};

// Verified against the platform docs' per-model thinking and effort tables:
//   - adaptive thinking: 4.6+, 5.x, Fable/Mythos (incl. 5.5/5.1), Mythos Preview
//   - budgeted (type:"enabled"): 3-7 series and 4.5 series; adaptive is a
//     400 there, so we never auto-send a thinking param
//   - 3-5 series: no thinking support at all
//   - effort: low/medium/high on every effort-capable model; xhigh/max
//     availability per the effort page's level table (max also on 4.6).
static const CapsEntry kAnthropicCaps[] = {
    // 5.x — adaptive thinking, all five effort levels.
    {"claude-opus-5",      kEffortAll, ThinkingMode::Adaptive},
    {"claude-sonnet-5",    kEffortAll, ThinkingMode::Adaptive},
    {"claude-fable-5",     kEffortAll, ThinkingMode::Adaptive},
    {"claude-mythos-5",    kEffortAll, ThinkingMode::Adaptive},
    {"claude-mythos",      kEffortAll, ThinkingMode::Adaptive},  // Mythos Preview
    // 4.6 — adaptive, effort incl. max (no xhigh).
    {"claude-opus-4-6",    kEffortAll & ~kEffortXHigh, ThinkingMode::Adaptive},
    {"claude-sonnet-4-6",  kEffortAll & ~kEffortXHigh, ThinkingMode::Adaptive},
    // 4.5 — budgeted thinking; adaptive is rejected, effort unsupported.
    {"claude-opus-4-5",    0,          ThinkingMode::Budgeted},
    {"claude-sonnet-4-5",  0,          ThinkingMode::Budgeted},
    {"claude-haiku-4-5",   0,          ThinkingMode::Budgeted},
    // 3-7 — budgeted thinking.
    {"claude-3-7-sonnet",  0,          ThinkingMode::Budgeted},
    {"claude-3-7-haiku",   0,          ThinkingMode::Budgeted},
    // 3-5 — no thinking.
    {"claude-3-5-sonnet",  0,          ThinkingMode::None},
    {"claude-3-5-haiku",   0,          ThinkingMode::None},
};

} // namespace

AnthropicModelCaps anthropic_model_caps(const std::string& model_id) {
    const CapsEntry* best = nullptr;
    size_t best_len = 0;
    for (const auto& e : kAnthropicCaps) {
        if (!starts_with_ci(model_id, e.prefix)) continue;
        size_t n = 0;
        while (e.prefix[n]) ++n;
        if (!best || n > best_len) {
            best = &e;
            best_len = n;
        }
    }
    AnthropicModelCaps caps;
    if (best) {
        caps.supports_effort = best->effort_mask != 0;
        caps.effort_mask = best->effort_mask;
        caps.thinking = best->thinking;
    }
    return caps;
}

std::string anthropic_map_effort(const std::string& model_id,
                                 const std::string& ui_effort) {
    if (ui_effort.empty() || ui_effort == "default") return "";
    AnthropicModelCaps caps = anthropic_model_caps(model_id);
    if (!caps.supports_effort) return "";

    // "off"/"minimal" are not Anthropic values; "off" lands on the lowest
    // valid effort so thinking-heavy defaults are still reined in. ("off"
    // must not be sent verbatim — the API rejects it.)
    if (ui_effort == "off" || ui_effort == "minimal") return "low";

    uint8_t want = 0;
    if      (ui_effort == "low")    want = kEffortLow;
    else if (ui_effort == "medium") want = kEffortMedium;
    else if (ui_effort == "high")   want = kEffortHigh;
    else if (ui_effort == "xhigh")  want = kEffortXHigh;
    else if (ui_effort == "max")    want = kEffortMax;
    else return "";  // unknown setting: omit rather than send garbage

    if (caps.effort_mask & want) return ui_effort;
    // Unsupported level steps down to the nearest valid one; low/medium/
    // high are always present when effort is supported at all.
    if (want == kEffortMax || want == kEffortXHigh) {
        if (caps.effort_mask & kEffortHigh) return "high";
    }
    return "low";
}

bool openai_is_reasoning_model(const std::string& model_id) {
    return starts_with_ci(model_id, "o1")
        || starts_with_ci(model_id, "o3")
        || starts_with_ci(model_id, "o4")
        || starts_with_ci(model_id, "gpt-5");
}

std::string openai_map_effort(const std::string& model_id,
                              const std::string& ui_effort) {
    if (ui_effort.empty()) return "";
    // "none" exists on gpt-5* (documented default effort for gpt-5.1) but
    // not on the o-series, where "off" can only be omitted.
    if (ui_effort == "off")
        return starts_with_ci(model_id, "gpt-5") ? "none" : "";
    if (ui_effort == "minimal" || ui_effort == "low" || ui_effort == "medium"
            || ui_effort == "high" || ui_effort == "xhigh")
        return ui_effort;
    // "max" is Responses-API only; chat completions caps at xhigh.
    if (ui_effort == "max") return "xhigh";
    return "";
}

std::string llamacpp_map_effort(const std::string& model_id,
                                const std::string& ui_effort) {
    if (ui_effort.empty()) return "";
    if (ui_effort == "off") return "none";
    // llama.cpp model ids are whatever the server was started with: an
    // alias, a GGUF file name, or an -hf repo ("unsloth/Mistral-Small-4-
    // 119B-2603-GGUF:Q4_K_M"), so match anywhere in the id. The template
    // defaults to "none" (no reasoning) and raises on any value other than
    // "none"/"high" — a 500 from the server.
    if (contains_ci(model_id, "mistral-small-4")
            || contains_ci(model_id, "mistral-small-2603"))
        return "high";
    if (ui_effort == "minimal" || ui_effort == "low") return "low";
    if (ui_effort == "medium") return "medium";
    if (ui_effort == "high" || ui_effort == "xhigh" || ui_effort == "max")
        return "high";
    return "";
}

} // namespace haicode
