// Task 26 — model metadata tables: context-window prefixes (per-version
// entries), max-output clamps, refreshed pricing, the kind-aware pricing
// fallback chain, long-context/input-size tiers, model-id normalization,
// and local-server freeness. Numbers verified against MODEL_NUMBERS.md
// (2026-10-02); the test exists so a future table edit cannot drift.
#include <haicode/pricing.h>
#include <haicode/model_info.h>
#include <iostream>
#include <cmath>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << msg << "\n"; return false; } } while(0)

using namespace haicode;

static bool eq(double a, double b) { return std::fabs(a - b) < 1e-9; }

// ---- context-window table: per-version entries ----

static bool test_window_table() {
    // Claude 4.6+ are 1M; 4.5 and older Opus stay 200K (the short
    // "claude-opus-4" prefix must not leak 200K onto 4.6+).
    CHECK(get_context_window("", "claude-opus-4-6", {}) == 1000000,
          "opus-4-6 is 1M");
    CHECK(get_context_window("", "claude-opus-4-8-20260101", {}) == 1000000,
          "dated opus-4-8 resolves through the longest prefix");
    CHECK(get_context_window("", "claude-opus-4-5", {}) == 200000,
          "opus-4-5 stays 200K");
    CHECK(get_context_window("", "claude-opus-5", {}) == 1000000,
          "opus-5 is 1M");
    CHECK(get_context_window("", "claude-sonnet-5-5", {}) == 1000000,
          "sonnet-5.5 is 1M");
    CHECK(get_context_window("", "claude-fable-5-1", {}) == 1000000,
          "fable-5.1 is 1M");
    CHECK(get_context_window("", "claude-mythos-5", {}) == 1000000,
          "mythos-5 is 1M");
    CHECK(get_context_window("", "claude-haiku-4-5", {}) == 200000,
          "haiku-4.5 stays 200K");
    // OpenAI corrections
    CHECK(get_context_window("", "gpt-5", {}) == 400000, "gpt-5 window 400K");
    CHECK(get_context_window("", "gpt-4.1", {}) == 1047576, "gpt-4.1 window");
    CHECK(get_context_window("", "o3", {}) == 200000, "o3 window");
    // Other labs added from the reference table
    CHECK(get_context_window("", "gemini-2.5-pro", {}) == 1048576,
          "gemini-2.5-pro window");
    CHECK(get_context_window("", "grok-4.7", {}) == 500000, "grok-4.7 window");
    CHECK(get_context_window("", "glm-5.2", {}) == 1000000, "glm-5.2 is 1M");
    CHECK(get_context_window("", "glm-5", {}) == 200000, "glm-5 is 200K");
    CHECK(get_context_window("", "kimi-k3", {}) == 1048576, "kimi-k3 window");
    CHECK(get_context_window("", "muse-spark-1.3", {}) == 1048576,
          "muse-spark window");
    // Unknown model: 0
    CHECK(get_context_window("", "mystery-model", {}) == 0,
          "unknown model resolves 0");
    std::cout << "[OK] context window table (per-version entries)\n";
    return true;
}

// ---- max-output clamps ----

static bool test_max_output_clamp() {
    CHECK(get_max_output_tokens("claude-opus-5") == 128000, "opus-5 cap 128K");
    CHECK(get_max_output_tokens("claude-3-5-sonnet") == 8192,
          "3-5-sonnet cap 8K (kDefaultMaxTokens would 400)");
    CHECK(get_max_output_tokens("gpt-4o") == 16384, "gpt-4o cap 16K");
    CHECK(get_max_output_tokens("o3") == 100000, "o3 cap 100K");
    CHECK(get_max_output_tokens("unknown-model") == 0, "no cap when unknown");

    CHECK(clamp_max_tokens("claude-3-5-sonnet", 32768) == 8192,
          "32768 default clamped to 3-5 cap");
    CHECK(clamp_max_tokens("claude-opus-5", 32768) == 32768,
          "under-cap request unchanged");
    CHECK(clamp_max_tokens("claude-opus-5", 200000) == 128000,
          "over-cap request clamped");
    CHECK(clamp_max_tokens("any", 0) == 0, "unset stays unset");
    CHECK(clamp_max_tokens("any", -1) == -1, "negative sentinel unchanged");
    std::cout << "[OK] max-output clamps\n";
    return true;
}

// ---- refreshed pricing entries ----

static bool test_refreshed_pricing() {
    const ModelPricing* p = lookup_pricing("anthropic", "anthropic",
                                           "claude-opus-5-5", {});
    CHECK(p && eq(p->input, 4.0) && eq(p->output, 20.0)
          && eq(p->cache_read, 0.20) && eq(p->cache_write, 5.0),
          "opus-5.5 refreshed to 4/20/0.2/5");

    p = lookup_pricing("anthropic", "anthropic", "claude-opus-5", {});
    CHECK(p && eq(p->input, 5.0) && eq(p->output, 25.0),
          "opus-5 5/25");

    p = lookup_pricing("anthropic", "anthropic", "claude-opus-4-5", {});
    CHECK(p && eq(p->input, 5.0) && eq(p->cache_write, 6.25),
          "opus-4-5 keeps 5/25/.../6.25");

    p = lookup_pricing("anthropic", "anthropic", "claude-fable-5-1", {});
    CHECK(p && eq(p->input, 10.0) && eq(p->cache_read, 0.25),
          "fable-5.1 10/50/0.25/12.5");

    p = lookup_pricing("anthropic", "anthropic", "claude-sonnet-5", {});
    CHECK(p && eq(p->input, 2.0) && eq(p->output, 10.0),
          "sonnet-5 2/10");

    // OpenAI corrections vs the old table: o3 was 15/60, now 2/8.
    p = lookup_pricing("openai", "openai", "o3", {});
    CHECK(p && eq(p->input, 2.0) && eq(p->output, 8.0)
          && eq(p->cache_read, 0.50), "o3 corrected to 2/8/0.5");
    p = lookup_pricing("openai", "openai", "o4-mini", {});
    CHECK(p && eq(p->input, 1.1) && eq(p->output, 4.4),
          "o4-mini corrected to 1.1/4.4");
    p = lookup_pricing("openai", "openai", "gpt-5", {});
    CHECK(p && eq(p->input, 1.25) && eq(p->output, 10.0)
          && eq(p->cache_read, 0.125), "gpt-5 1.25/10/0.125");

    // Other labs resolve through the wildcard.
    p = lookup_pricing("myproxy", "openai", "glm-5.3", {});
    CHECK(p && eq(p->input, 1.4) && eq(p->output, 4.4),
          "glm-5.3 via wildcard");
    p = lookup_pricing("openrouter", "openai", "kimi-k3", {});
    CHECK(p && eq(p->input, 3.0), "kimi-k3 via wildcard");
    p = lookup_pricing("xai", "openai", "grok-4.7", {});
    CHECK(p && eq(p->input, 2.0) && eq(p->output, 6.0),
          "grok-4.7 via wildcard");
    std::cout << "[OK] refreshed pricing entries\n";
    return true;
}

// ---- kind-aware fallback chain ----

static bool test_kind_fallback() {
    // Custom provider id (proxy) + anthropic kind: built-in still resolves.
    const ModelPricing* p = lookup_pricing("my-anthropic-proxy", "anthropic",
                                           "claude-sonnet-5", {});
    CHECK(p && eq(p->input, 2.0), "kind fallback prices proxies");

    // Unknown kind + unknown model: null.
    CHECK(lookup_pricing("weird", "weirder", "no-such-model", {}) == nullptr,
          "unresolvable returns null");

    // Local server kinds are free regardless of the model id.
    p = lookup_pricing("my-ollama", "ollama", "gpt-5", {});
    CHECK(p && eq(p->input, 0.0) && eq(p->output, 0.0),
          "local server kind is free even for cloud-named models");

    // Config overrides keep winning, keyed by provider id OR kind OR model.
    std::map<std::string, ModelPricing> ov;
    ov["my-anthropic-proxy:claude-sonnet-5"] = {1.0, 2.0, 0.0, 0.0};
    p = lookup_pricing("my-anthropic-proxy", "anthropic",
                       "claude-sonnet-5", ov);
    CHECK(p && eq(p->input, 1.0), "override by provider id wins");
    ov.clear();
    ov["anthropic:claude-sonnet-5"] = {9.0, 9.0, 0.0, 0.0};
    p = lookup_pricing("my-anthropic-proxy", "anthropic",
                       "claude-sonnet-5", ov);
    CHECK(p && eq(p->input, 9.0), "override by kind wins over built-in");
    std::cout << "[OK] kind-aware pricing fallback chain\n";
    return true;
}

// ---- model-id normalization ----

static bool test_normalization() {
    // Vendor path prefix (aggregator wrapper)
    const ModelPricing* p = lookup_pricing("openrouter", "openai",
                                           "z-ai/glm-5.3", {});
    CHECK(p && eq(p->input, 1.4), "vendor path segment dropped");

    // Cloud region prefixes
    p = lookup_pricing("bedrock", "anthropic", "us.claude-opus-5", {});
    CHECK(p && eq(p->input, 5.0), "us. region prefix dropped");
    p = lookup_pricing("vertex", "anthropic", "anthropic.claude-opus-5", {});
    CHECK(p && eq(p->input, 5.0), "anthropic. wrapper dropped");

    // amazon. is NOT a wrapper — amazon.nova ids are genuine.
    p = lookup_pricing("bedrock", "openai", "amazon.nova-2-lite-v1:0", {});
    CHECK(p && eq(p->input, 0.3), "amazon.nova id kept intact");

    // OpenRouter variant suffixes; :free is price zero.
    p = lookup_pricing("openrouter", "openai", "glm-4.7:free", {});
    CHECK(p && eq(p->input, 0.0) && eq(p->output, 0.0),
          ":free variant prices at zero");
    p = lookup_pricing("openrouter", "openai", "kimi-k3:thinking", {});
    CHECK(p && eq(p->input, 3.0), ":thinking suffix dropped");
    std::cout << "[OK] model id normalization\n";
    return true;
}

// ---- long-context / input-size tiers ----

static bool test_tiered_pricing() {
    TokenUsage u;
    u.input = 100000;
    u.output = 1000;
    // Below the threshold: base rate.
    double below = compute_step_cost(u, "anthropic", "anthropic",
                                     "claude-sonnet-4-5", {});
    // Above: whole request repriced 6/22.5.
    TokenUsage big = u;
    big.input = 250000;
    double above = compute_step_cost(big, "anthropic", "anthropic",
                                     "claude-sonnet-4-5", {});
    CHECK(eq(below, 100000 * 3.0 / 1e6 + 1000 * 15.0 / 1e6),
          "sonnet-4-5 below 200K bills base 3/15");
    CHECK(eq(above, 250000 * 6.0 / 1e6 + 1000 * 22.5 / 1e6),
          "sonnet-4-5 above 200K reprices whole request 6/22.5");
    CHECK(above > below * 1.9, "surcharge actually applies");

    // gpt-5.5 above 272K (cache_read counts toward the tier's prompt size;
    // billing still splits input vs cache_read at the tier-2 rates).
    TokenUsage g;
    g.input = 200000;
    g.cache_read = 100000;  // 300K total prompt
    g.output = 0;
    double cost = compute_step_cost(g, "openai", "openai", "gpt-5.5", {});
    CHECK(eq(cost, 200000 * 10.0 / 1e6 + 100000 * 1.00 / 1e6),
          "gpt-5.5 above 272K bills tier-2 rates (cache counts for tier size)");

    // qwen3-coder-plus input-size ladder: 32K/128K/256K breakpoints.
    TokenUsage q1; q1.input = 1000; q1.output = 1000;
    TokenUsage q2; q2.input = 100000; q2.output = 1000;
    TokenUsage q3; q3.input = 900000; q3.output = 1000;
    CHECK(eq(compute_step_cost(q1, "dashscope", "openai", "qwen3-coder-plus", {}),
             1000 * 1.0 / 1e6 + 1000 * 5.0 / 1e6),
          "qwen coder tier 1 (<=32K)");
    CHECK(eq(compute_step_cost(q2, "dashscope", "openai", "qwen3-coder-plus", {}),
             100000 * 1.8 / 1e6 + 1000 * 9.0 / 1e6),
          "qwen coder tier 2 (<=128K)");
    CHECK(eq(compute_step_cost(q3, "dashscope", "openai", "qwen3-coder-plus", {}),
             900000 * 6.0 / 1e6 + 1000 * 60.0 / 1e6),
          "qwen coder tier 4 (unbounded)");

    // Grok >= 200K threshold (tier boundary 199999).
    TokenUsage k1; k1.input = 199999; k1.output = 0;
    TokenUsage k2; k2.input = 200000; k2.output = 0;
    CHECK(eq(compute_step_cost(k1, "xai", "openai", "grok-4.6", {}),
             199999 * 2.0 / 1e6), "grok below threshold base");
    CHECK(eq(compute_step_cost(k2, "xai", "openai", "grok-4.6", {}),
             200000 * 4.0 / 1e6), "grok at threshold repriced");

    // Flat models never tier.
    TokenUsage f; f.input = 900000; f.output = 1000;
    CHECK(eq(compute_step_cost(f, "anthropic", "anthropic", "claude-opus-5", {}),
             900000 * 5.0 / 1e6 + 1000 * 25.0 / 1e6),
          "opus-5 flat at any length");

    // Unknown model / local server: zero.
    CHECK(eq(compute_step_cost(f, "x", "y", "mystery", {}), 0.0),
          "unknown model costs 0");
    CHECK(eq(compute_step_cost(f, "local", "ollama", "gpt-5", {}), 0.0),
          "local server costs 0");
    std::cout << "[OK] tiered pricing (long-context + input tiers)\n";
    return true;
}

// ---- compute_cost basics stay intact ----

static bool test_compute_cost() {
    TokenUsage u;
    u.input = 1000000;
    u.output = 1000000;
    u.reasoning = 500000;
    u.cache_read = 2000000;
    u.cache_write = 0;
    ModelPricing p{3.0, 15.0, 0.30, 3.75};
    // reasoning at output rate; cache_read discounted.
    double expect = 3.0 + 15.0 + 0.5 * 15.0 + 2.0 * 0.30;
    CHECK(eq(compute_cost(u, p), expect),
          "compute_cost: reasoning at output rate, cache discounted");
    std::cout << "[OK] compute_cost arithmetic\n";
    return true;
}

int main() {
    bool ok = true;
    ok = test_window_table() && ok;
    ok = test_max_output_clamp() && ok;
    ok = test_refreshed_pricing() && ok;
    ok = test_kind_fallback() && ok;
    ok = test_normalization() && ok;
    ok = test_tiered_pricing() && ok;
    ok = test_compute_cost() && ok;
    if (!ok) return 1;
    std::cout << "All pricing/metadata tests passed\n";
    return 0;
}
