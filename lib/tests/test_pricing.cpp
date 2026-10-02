// Task 26 — model metadata tables: context-window prefixes (per-version
// entries), max-output clamps, refreshed pricing, the kind-aware pricing
// fallback chain, long-context/input-size tiers, model-id normalization,
// and local-server freeness. Numbers verified against MODEL_NUMBERS.md
// (2026-10-02); the test exists so a future table edit cannot drift.
#include <haicode/pricing.h>
#include <haicode/model_info.h>
#include <haicode/openai_params.h>
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
    // mini/nano keep the 400K family window: the bare gpt-5.4 prefix must
    // not leak 1.05M onto them (compaction would fire past the real limit).
    CHECK(get_context_window("", "gpt-5.4-mini", {}) == 400000,
          "gpt-5.4-mini window 400K, not the gpt-5.4 1.05M");
    CHECK(get_context_window("", "gpt-5.4-nano", {}) == 400000,
          "gpt-5.4-nano window 400K");
    CHECK(get_context_window("", "gpt-5.4", {}) == 1050000,
          "gpt-5.4 itself keeps 1.05M");
    // Other labs added from the reference table
    CHECK(get_context_window("", "gemini-2.5-pro", {}) == 1048576,
          "gemini-2.5-pro window");
    CHECK(get_context_window("", "grok-4.7", {}) == 500000, "grok-4.7 window");
    CHECK(get_context_window("", "glm-5.2", {}) == 1000000, "glm-5.2 is 1M");
    CHECK(get_context_window("", "glm-5", {}) == 200000, "glm-5 is 200K");
    CHECK(get_context_window("", "glm-5.3", {}) == 1000000,
          "glm-5.3 is 1M (flash is the 1,048,576 one)");
    CHECK(get_context_window("", "glm-5.3-flash", {}) == 1048576,
          "glm-5.3-flash window");
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
    // Qwen caps published by DashScope; turbo/qwq sit below the streaming
    // default, so an unclamped default request would 400.
    CHECK(clamp_max_tokens("qwq-plus", 32768) == 8192,
          "qwq-plus default clamped to 8K");
    CHECK(clamp_max_tokens("qwen-turbo", 32768) == 16384,
          "qwen-turbo default clamped to 16K");
    CHECK(clamp_max_tokens("qwen3.8-max", 200000) == 131072,
          "qwen3.8-max clamped to 128K output");
    CHECK(clamp_max_tokens("qwen3-vl-plus", 32768) == 32768,
          "qwen3-vl at cap unchanged");
    // Other new published caps.
    CHECK(clamp_max_tokens("gemma-3-27b-it", 32768) == 8192,
          "gemma-3 clamped to 8K");
    CHECK(clamp_max_tokens("gemma-4-31b-it", 32768) == 32768,
          "gemma-4 cap 32K");
    CHECK(clamp_max_tokens("glm-4.5", 32768) == 32000,
          "glm-4.5 clamped to 32K (also air/x/airx variants)");
    CHECK(clamp_max_tokens("command-a-03-2025", 32768) == 8000,
          "command-a-03 clamped to 8K");
    CHECK(clamp_max_tokens("command-r-plus-08-2024", 32768) == 4096,
          "command-r-plus clamped to 4K");
    CHECK(clamp_max_tokens("deepseek-chat", 32768) == 8192,
          "legacy deepseek-chat clamped to 8K");
    CHECK(clamp_max_tokens("deepseek-reasoner", 200000) == 65536,
          "legacy deepseek-reasoner clamped to 64K");
    CHECK(clamp_max_tokens("doubao-seed-2-0-pro-260215", 200000) == 128000,
          "doubao 2-0 clamped to 128K");
    CHECK(clamp_max_tokens("mimo-v2.6-flash", 200000) == 131072,
          "mimo-v2.6 clamped to 128K");
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
    // grok-4.5 has its own ladder: the >200K cache-read is 0.6, not the 1.0
    // inherited from the grok-4 entry.
    CHECK(eq(compute_step_cost(k2, "xai", "openai", "grok-4.5", {}),
             200000 * 4.0 / 1e6), "grok-4.5 at threshold repriced 4/12/0.6");
    TokenUsage k3; k3.input = 200000; k3.cache_read = 100000; k3.output = 0;
    CHECK(eq(compute_step_cost(k3, "xai", "openai", "grok-4.5", {}),
             200000 * 4.0 / 1e6 + 100000 * 0.6 / 1e6),
          "grok-4.5 tier-2 cache_read 0.6 (not grok-4's 1.0)");

    // qwen3-max / qwen3-vl-plus input-size ladders (32K/128K breakpoints).
    TokenUsage qa; qa.input = 200000; qa.output = 1000;
    CHECK(eq(compute_step_cost(qa, "dashscope", "openai", "qwen3-max", {}),
             200000 * 3.0 / 1e6 + 1000 * 15.0 / 1e6),
          "qwen3-max top tier 3/15");
    TokenUsage qb; qb.input = 100000; qb.output = 1000;
    CHECK(eq(compute_step_cost(qb, "dashscope", "openai", "qwen3-max", {}),
             100000 * 2.4 / 1e6 + 1000 * 12.0 / 1e6),
          "qwen3-max middle tier 2.4/12");
    CHECK(eq(compute_step_cost(qb, "dashscope", "openai", "qwen3-vl-plus", {}),
             100000 * 0.3 / 1e6 + 1000 * 2.4 / 1e6),
          "qwen3-vl-plus middle tier 0.3/2.4");
    TokenUsage qc; qc.input = 900000; qc.output = 1000;
    CHECK(eq(compute_step_cost(qc, "dashscope", "openai", "qwen3-vl-plus", {}),
             900000 * 0.6 / 1e6 + 1000 * 4.8 / 1e6),
          "qwen3-vl-plus top tier 0.6/4.8");

    // Flat mini/nano must not inherit gpt-5.4's >272K surcharge: a 500K
    // prompt still bills 0.75/4.5, not 5/22.5.
    TokenUsage mini; mini.input = 500000; mini.output = 1000;
    CHECK(eq(compute_step_cost(mini, "openai", "openai", "gpt-5.4-mini", {}),
             500000 * 0.75 / 1e6 + 1000 * 4.5 / 1e6),
          "gpt-5.4-mini stays flat at long prompts");
    CHECK(eq(compute_step_cost(mini, "openai", "openai", "gpt-5.4", {}),
             500000 * 5.0 / 1e6 + 1000 * 22.5 / 1e6),
          "gpt-5.4 itself reprices above 272K");

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

// ---- rows completed against MODEL_NUMBERS.md (Task 26 sweep) ----

static bool test_reference_rows() {
    // gpt-5.4-mini/nano previously billed at the gpt-5.4 rate (3.3x over).
    const ModelPricing* p = lookup_pricing("openai", "openai",
                                           "gpt-5.4-mini", {});
    CHECK(p && eq(p->input, 0.75) && eq(p->output, 4.5)
          && eq(p->cache_read, 0.075), "gpt-5.4-mini 0.75/4.5/0.075");
    p = lookup_pricing("openai", "openai", "gpt-5.4-nano", {});
    CHECK(p && eq(p->input, 0.2) && eq(p->output, 1.25),
          "gpt-5.4-nano 0.2/1.25");
    p = lookup_pricing("openai", "openai", "gpt-5.2", {});
    CHECK(p && eq(p->input, 1.75) && eq(p->output, 14.0),
          "gpt-5.2 1.75/14 (not the gpt-5 rate)");

    // GLM: 5.1 shares the 5.2/5.3 rate, not glm-5's; 4.5 variants differ.
    p = lookup_pricing("zai", "openai", "glm-5.1", {});
    CHECK(p && eq(p->input, 1.4) && eq(p->output, 4.4),
          "glm-5.1 1.4/4.4 (not the glm-5 1.0/3.2 rate)");
    p = lookup_pricing("zai", "openai", "glm-4.5-air", {});
    CHECK(p && eq(p->input, 0.2) && eq(p->output, 1.1),
          "glm-4.5-air 0.2/1.1");
    p = lookup_pricing("zai", "openai", "glm-4.5-airx", {});
    CHECK(p && eq(p->input, 1.1) && eq(p->output, 4.5),
          "glm-4.5-airx 1.1/4.5 (longest prefix beats -air)");
    p = lookup_pricing("zai", "openai", "glm-4.5-x", {});
    CHECK(p && eq(p->input, 2.2) && eq(p->output, 8.9),
          "glm-4.5-x 2.2/8.9");
    p = lookup_pricing("zai", "openai", "glm-4.5v", {});
    CHECK(p && eq(p->input, 0.6) && eq(p->output, 1.8),
          "glm-4.5v 0.6/1.8");

    // Legacy Moonshot / DeepSeek ids keep pricing for old sessions.
    p = lookup_pricing("moonshot", "openai", "moonshot-v1-128k", {});
    CHECK(p && eq(p->input, 2.0) && eq(p->output, 5.0),
          "moonshot-v1-128k 2/5");
    p = lookup_pricing("moonshot", "openai", "moonshot-v1-8k", {});
    CHECK(p && eq(p->input, 0.2) && eq(p->output, 2.0),
          "moonshot-v1-8k 0.2/2");
    p = lookup_pricing("deepseek", "openai", "deepseek-chat", {});
    CHECK(p && eq(p->input, 0.28) && eq(p->output, 0.42)
          && eq(p->cache_read, 0.028), "deepseek-chat peak 0.28/0.42/0.028");
    p = lookup_pricing("deepseek", "openai", "deepseek-reasoner", {});
    CHECK(p && eq(p->input, 0.28), "deepseek-reasoner peak rate");

    // Alibaba first-party rows that previously fell through to $0 or a
    // family rate.
    p = lookup_pricing("dashscope", "openai", "qwen3.8-max", {});
    CHECK(p && eq(p->input, 2.0) && eq(p->output, 6.0),
          "qwen3.8-max 2/6 (not the flash rate)");
    p = lookup_pricing("dashscope", "openai", "qwen3.7-max", {});
    CHECK(p && eq(p->input, 2.5) && eq(p->output, 7.5),
          "qwen3.7-max 2.5/7.5");
    p = lookup_pricing("dashscope", "openai", "qwen-turbo", {});
    CHECK(p && eq(p->input, 0.05) && eq(p->output, 0.2),
          "qwen-turbo 0.05/0.2");
    p = lookup_pricing("dashscope", "openai", "qwq-plus", {});
    CHECK(p && eq(p->input, 0.8) && eq(p->output, 2.4),
          "qwq-plus 0.8/2.4");
    p = lookup_pricing("volcengine", "openai", "doubao-seed-2-0-pro-260215", {});
    CHECK(p && eq(p->input, 0.46), "doubao 2-0 pro base (tiered)");
    p = lookup_pricing("bytedance", "openai", "doubao-seed-2-0-lite-260215", {});
    CHECK(p && eq(p->input, 0.087), "doubao 2-0 lite base (tiered)");

    // xAI 4.5 has its own cache-read, not grok-4's 0.5.
    p = lookup_pricing("xai", "openai", "grok-4.5", {});
    CHECK(p && eq(p->input, 2.0) && eq(p->cache_read, 0.30),
          "grok-4.5 2/6/0.3");

    // Mistral coding/legacy rows.
    p = lookup_pricing("mistral", "openai", "devstral-latest", {});
    CHECK(p && eq(p->input, 0.4) && eq(p->output, 2.0), "devstral 0.4/2");
    p = lookup_pricing("mistral", "openai", "devstral-small-latest", {});
    CHECK(p && eq(p->input, 0.1) && eq(p->output, 0.3),
          "devstral-small 0.1/0.3");
    p = lookup_pricing("mistral", "openai", "codestral-latest", {});
    CHECK(p && eq(p->input, 0.3) && eq(p->output, 0.9), "codestral 0.3/0.9");
    p = lookup_pricing("mistral", "openai", "pixtral-large-latest", {});
    CHECK(p && eq(p->input, 2.0) && eq(p->output, 6.0),
          "pixtral-large 2/6");
    p = lookup_pricing("mistral", "openai", "open-mistral-nemo", {});
    CHECK(p && eq(p->input, 0.3) && eq(p->output, 0.3),
          "open-mistral-nemo 0.3/0.3");

    // Xiaomi Pro tier and Cohere.
    p = lookup_pricing("xiaomi", "openai", "mimo-v2.6-pro", {});
    CHECK(p && eq(p->input, 0.435) && eq(p->output, 0.87)
          && eq(p->cache_read, 0.0036), "mimo-v2.6-pro 0.435/0.87/0.0036");
    p = lookup_pricing("cohere", "openai", "command-a-03-2025", {});
    CHECK(p && eq(p->input, 2.5) && eq(p->output, 10.0),
          "command-a-03 2.5/10");
    p = lookup_pricing("cohere", "openai", "command-r-plus-08-2024", {});
    CHECK(p && eq(p->input, 2.5), "command-r-plus 2.5/10");
    std::cout << "[OK] reference rows (MODEL_NUMBERS.md sweep)\n";
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

static bool test_cached_input_billing() {
    TokenUsage u;
    parse_openai_usage(nlohmann::json{
        {"prompt_tokens", 200000}, {"completion_tokens", 1000},
        {"prompt_tokens_details", {{"cached_tokens", 150000}}}}, u);
    CHECK(u.total_input() == 200000, "normalized prompt total");
    CHECK(eq(compute_cost(u, ModelPricing{2.0, 10.0, 0.2, 0.0}),
             (50000 * 2.0 + 1000 * 10.0 + 150000 * 0.2) / 1e6),
          "cached input billed only at the cache rate");
    const auto* p = lookup_pricing("openai", "openai", "gpt-5.5", {});
    CHECK(p, "base pricing exists");
    CHECK(eq(compute_step_cost(u, "openai", "openai", "gpt-5.5", {}),
             compute_cost(u, *p)),
          "200K prompt plus cached subset stays below the 272K tier");
    return true;
}

int main() {
    bool ok = true;
    ok = test_cached_input_billing() && ok;
    ok = test_window_table() && ok;
    ok = test_max_output_clamp() && ok;
    ok = test_refreshed_pricing() && ok;
    ok = test_reference_rows() && ok;
    ok = test_kind_fallback() && ok;
    ok = test_normalization() && ok;
    ok = test_tiered_pricing() && ok;
    ok = test_compute_cost() && ok;
    if (!ok) return 1;
    std::cout << "All pricing/metadata tests passed\n";
    return 0;
}
