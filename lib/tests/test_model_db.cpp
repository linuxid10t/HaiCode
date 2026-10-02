// Model Database: the user's per-model entries ("models", "max_output",
// "vision", "pricing" in config.json) and the built-in tables form ONE
// prefix database — longest matching key wins, a user key wins a tie with a
// built-in key. Also covers the merged editor view (build_model_database),
// the per-entry config write path (save_model_db_entry), and the "max_output"
// config key round trip.
#include <haicode/config.h>
#include <haicode/model_db.h>
#include <haicode/model_info.h>
#include <haicode/pricing.h>
#include "test_check.h"

#include <unistd.h>

#include <cmath>
#include <fstream>
#include <iostream>
#include <string>

using namespace haicode;

static bool eq(double a, double b) { return std::fabs(a - b) < 1e-9; }

static const ModelDbRow* find_row(const std::vector<ModelDbRow>& rows,
                                  const std::string& key) {
    for (auto& r : rows)
        if (r.key == key) return &r;
    return nullptr;
}

static void context_and_vision_prefix_database() {
    // Exact-id entries still win (the pre-existing override semantics).
    TEST_REQUIRE(get_context_window("", "gpt-5.5", {{"gpt-5.5", 300000}}) == 300000,
                 "exact user entry beats the built-in");
    // A user key equal to a built-in prefix replaces that row for every id
    // it covers (tie → user).
    TEST_REQUIRE(get_context_window("", "claude-opus-4-5-20251101",
                                    {{"claude-opus-4", 150000}}) == 150000,
                 "re-keyed built-in prefix replaces the built-in value");
    // A SHORT user prefix doesn't shadow a longer built-in row...
    TEST_REQUIRE(get_context_window("", "gpt-5.5", {{"gpt", 64000}}) == 1050000,
                 "short user prefix must not shadow longer built-in");
    // ...but does apply where it is the longest match.
    TEST_REQUIRE(get_context_window("", "my-local-7b", {{"my-local", 32768}}) == 32768,
                 "user prefix covers otherwise-unknown ids");
    TEST_REQUIRE(get_context_window("", "My-Local-7B", {{"my-local", 32768}}) == 32768,
                 "user prefix matches case-insensitively");
    TEST_REQUIRE(get_context_window("", "unrelated", {{"my-local", 32768}}) == 0,
                 "unmatched ids stay unknown");
    TEST_REQUIRE(user_context_window("gpt-5.5", {{"gpt", 64000}}) == 0,
                 "losing user prefix reports no user window");
    TEST_REQUIRE(user_context_window("my-local-7b", {{"my-local", 32768}}) == 32768,
                 "winning user prefix reported");
    // Non-positive user values never apply.
    TEST_REQUIRE(get_context_window("", "gpt-5.5", {{"gpt-5.5", 0}}) == 1050000,
                 "zero user window ignored");

    // Vision: explicit false on an exact id beats the table; a short user
    // prefix doesn't shadow a longer built-in entry.
    TEST_REQUIRE(!model_supports_vision("claude-sonnet-4-5", {{"claude-sonnet-4-5", false}}),
                 "exact vision=false wins");
    TEST_REQUIRE(model_supports_vision("claude-sonnet-4-5", {{"claude", false}}),
                 "short vision prefix must not shadow longer built-in");
    TEST_REQUIRE(model_supports_vision("my-vlm-13b", {{"my-vlm", true}}),
                 "user vision prefix covers unknown ids");
    TEST_REQUIRE(!model_supports_vision("my-llm", {{"my-vlm", true}}),
                 "unknown ids still fail closed");
    std::cout << "[OK] context/vision prefix database\n";
}

static void max_output_overrides() {
    // Explicit-map overload.
    TEST_REQUIRE(get_max_output_tokens("claude-3-5-sonnet", {}) == 8192, "built-in cap");
    TEST_REQUIRE(get_max_output_tokens("claude-3-5-sonnet",
                                       {{"claude-3-5-sonnet", 4096}}) == 4096,
                 "user cap replaces built-in");
    TEST_REQUIRE(get_max_output_tokens("my-model", {{"my-model", 2048}}) == 2048,
                 "user cap for unknown model");

    // Process-wide registry drives clamp_max_tokens (used by providers).
    TEST_REQUIRE(clamp_max_tokens("my-model", 32768) == 32768, "no cap → unchanged");
    set_max_output_overrides({{"my-model", 2048}, {"claude-3-5-sonnet", 4096}});
    TEST_REQUIRE(clamp_max_tokens("my-model", 32768) == 2048, "user cap clamps");
    TEST_REQUIRE(clamp_max_tokens("my-model", 1000) == 1000, "below cap unchanged");
    TEST_REQUIRE(clamp_max_tokens("claude-3-5-sonnet", 8192) == 4096,
                 "user cap below built-in clamps");
    set_max_output_overrides({});
    TEST_REQUIRE(clamp_max_tokens("my-model", 32768) == 32768, "cleared registry");
    TEST_REQUIRE(clamp_max_tokens("claude-3-5-sonnet", 32768) == 8192,
                 "built-in cap after clearing");
    std::cout << "[OK] max_output overrides\n";
}

static void pricing_prefix_database() {
    std::map<std::string, ModelPricing> ov;
    // Same key as a built-in model part: user wins the tie.
    ov["claude-opus-5"] = {1.0, 2.0, 0.0, 0.0};
    const ModelPricing* p = lookup_pricing("anthropic", "anthropic", "claude-opus-5", ov);
    TEST_REQUIRE(p && eq(p->input, 1.0), "bare-key override ties built-in → user");
    // The longer built-in claude-opus-5-5 row is NOT shadowed by it.
    p = lookup_pricing("anthropic", "anthropic", "claude-opus-5-5", ov);
    TEST_REQUIRE(p && eq(p->input, 4.0), "short override must not shadow longer built-in");

    ov.clear();
    ov["gpt"] = {0.5, 0.5, 0.0, 0.0};
    p = lookup_pricing("openai", "openai", "gpt-5", ov);
    TEST_REQUIRE(p && eq(p->input, 1.25), "short 'gpt' override loses to built-in gpt-5");
    p = lookup_pricing("openai", "openai", "gpt-custom-finetune", ov);
    TEST_REQUIRE(p && eq(p->input, 0.5), "short override covers unknown ids");

    // A winning override on a tiered model is flat: the tier ladder must
    // not reprice it (it used to, so the override never took effect).
    ov.clear();
    ov["qwen3-max"] = {10.0, 10.0, 0.0, 0.0};
    TokenUsage u;
    u.input = 1000000;
    double cost = compute_step_cost(u, "dashscope", "openai", "qwen3-max", ov);
    TEST_REQUIRE(eq(cost, 10.0), "override replaces the tier ladder");
    // Control: without the override the 1M-token prompt is top-tier priced.
    cost = compute_step_cost(u, "dashscope", "openai", "qwen3-max", {});
    TEST_REQUIRE(eq(cost, 3.0), "built-in tier ladder still applies");
    std::cout << "[OK] pricing prefix database\n";
}

static void merged_view() {
    ModelOverrides user;
    user.contexts["gpt-5.5"] = 300000;
    user.vision["my-local"] = true;
    user.max_output["my-local"] = 4096;
    user.pricing["my-proxy:claude-opus-5"] = {1.0, 2.0, 0.1, 0.0};
    user.contexts["my"] = 8000;  // short user prefix covering "my-local"
    auto rows = build_model_database(user);

    const ModelDbRow* r = find_row(rows, "gpt-5.5");
    TEST_REQUIRE(r, "gpt-5.5 row present");
    TEST_REQUIRE(r->builtin.context == 1050000, "built-in window kept");
    TEST_REQUIRE(r->user.context == 300000, "user window merged");
    TEST_REQUIRE(r->effective.context == 300000 && r->user_context, "effective = user");
    TEST_REQUIRE(r->effective.max_output == 128000 && !r->user_max_output, "built-in max out");
    // Resolved, not exact-key: no "gpt-5.5" vision row exists, the "gpt-5"
    // entry covers it.
    TEST_REQUIRE(r->effective.vision == true && !r->user_vision,
                 "vision resolved through the shorter built-in prefix");
    TEST_REQUIRE(r->effective.pricing && eq(r->effective.pricing->input, 5.0),
                 "pricing folded from 'openai:gpt-5.5'");
    TEST_REQUIRE(r->price_tiers, "gpt-5.5 has a tier ladder");
    TEST_REQUIRE(r->has_user() && r->builtin_key, "row flags");

    r = find_row(rows, "gpt-5.4-mini");
    TEST_REQUIRE(r && r->effective.pricing && !r->price_tiers,
                 "one-tier ladder (gpt-5.4-mini) is flat");
    TEST_REQUIRE(r->effective.context == 400000, "gpt-5.4-mini window");

    r = find_row(rows, "glm-5");
    TEST_REQUIRE(r && r->effective.pricing && eq(r->effective.pricing->input, 1.0),
                 "pricing folded from '*:glm-5'");
    TEST_REQUIRE(!r->has_user(), "untouched built-in row");

    r = find_row(rows, "my-local");
    TEST_REQUIRE(r && !r->builtin_key && r->user.vision == true
                 && r->user.max_output == 4096, "user-only row");
    TEST_REQUIRE(r->effective.vision == true && r->user_vision, "user vision wins");
    TEST_REQUIRE(r->effective.context == 8000 && r->user_context && !r->user.context,
                 "shorter user prefix supplies the effective window");
    TEST_REQUIRE(!r->builtin.vision && !r->builtin.context, "no built-in values");
    r = find_row(rows, "my-proxy:claude-opus-5");
    TEST_REQUIRE(r && r->user.pricing && r->user_pricing
                 && eq(r->effective.pricing->input, 1.0),
                 "provider-scoped pricing key shown as-is");

    // Every row's effective values agree with the engine's own lookups.
    for (auto& row : rows) {
        TEST_REQUIRE(row.effective.context.value_or(0)
                     == get_context_window("", row.key, user.contexts),
                     "effective window matches get_context_window: " + row.key);
        TEST_REQUIRE(row.effective.max_output.value_or(0)
                     == get_max_output_tokens(row.key, user.max_output),
                     "effective cap matches get_max_output_tokens: " + row.key);
        TEST_REQUIRE(row.effective.vision.value_or(false)
                     == model_supports_vision(row.key, user.vision),
                     "effective vision matches model_supports_vision: " + row.key);
    }

    for (size_t i = 1; i < rows.size(); ++i) {
        std::string a = rows[i - 1].key, b = rows[i].key;
        for (auto& c : a) c = std::tolower(static_cast<unsigned char>(c));
        for (auto& c : b) c = std::tolower(static_cast<unsigned char>(c));
        TEST_REQUIRE(a < b, "rows sorted case-insensitively, no duplicates");
    }

    // Case-insensitive merge: a user "GPT-5.5" folds onto the built-in row
    // and the row shows the user's spelling (the key a save rewrites).
    ModelOverrides upper;
    upper.contexts["GPT-5.5"] = 1;
    rows = build_model_database(upper);
    r = find_row(rows, "GPT-5.5");
    TEST_REQUIRE(r && r->builtin.context == 1050000 && r->user.context == 1
                 && r->effective.context == 1, "case-insensitive merge");
    TEST_REQUIRE(!find_row(rows, "gpt-5.5"), "no duplicate lowercase row");
    std::cout << "[OK] merged database view\n";
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

static void save_entry_round_trip() {
    const std::string path = "/tmp/haicode_test_model_db_" +
                             std::to_string(getpid()) + ".json";
    {
        std::ofstream f(path);
        f << R"({"model": "gpt-5", "models": {"other": 1234},
                 "providers": {"anthropic": {"api_key": "sk-keep"}}})";
    }
    std::string err;
    ModelDbEntry e;
    e.context = 64000;
    e.max_output = 8192;
    e.vision = false;
    e.pricing = ModelPricing{1.5, 6.0, 0.15, 1.875};
    TEST_REQUIRE(save_model_db_entry(path, "my-model", e, err), "save: " + err);

    AppConfig cfg = ConfigLoader().load_file(path);
    TEST_REQUIRE(cfg.model_contexts["my-model"] == 64000, "context saved");
    TEST_REQUIRE(cfg.model_contexts["other"] == 1234, "unrelated entry preserved");
    TEST_REQUIRE(cfg.model_max_output["my-model"] == 8192, "max_output saved + parsed");
    TEST_REQUIRE(cfg.model_vision.count("my-model") && !cfg.model_vision["my-model"],
                 "vision saved");
    TEST_REQUIRE(eq(cfg.pricing["my-model"].cache_write, 1.875), "pricing saved");
    TEST_REQUIRE(cfg.providers["anthropic"].api_key == "sk-keep", "api key preserved");
    TEST_REQUIRE(cfg.model == "gpt-5", "unrelated scalar preserved");

    // Partial entry: unset fields are erased, empty maps removed.
    ModelDbEntry partial;
    partial.context = 32000;
    TEST_REQUIRE(save_model_db_entry(path, "my-model", partial, err), "partial: " + err);
    cfg = ConfigLoader().load_file(path);
    TEST_REQUIRE(cfg.model_contexts["my-model"] == 32000, "context updated");
    TEST_REQUIRE(cfg.model_max_output.empty() && cfg.model_vision.empty()
                 && cfg.pricing.empty(), "unset fields erased");
    std::string body = read_file(path);
    TEST_REQUIRE(body.find("\"max_output\"") == std::string::npos
                 && body.find("\"pricing\"") == std::string::npos,
                 "empty maps removed from the file");

    // Empty entry = revert to built-in.
    TEST_REQUIRE(save_model_db_entry(path, "my-model", ModelDbEntry{}, err), "revert: " + err);
    cfg = ConfigLoader().load_file(path);
    TEST_REQUIRE(!cfg.model_contexts.count("my-model"), "reverted");
    TEST_REQUIRE(cfg.model_contexts["other"] == 1234, "other entry survives revert");

    TEST_REQUIRE(!save_model_db_entry(path, "", e, err), "empty key refused");

    // Unparseable file is refused and left byte-identical.
    { std::ofstream f(path); f << "{ not json"; }
    TEST_REQUIRE(!save_model_db_entry(path, "my-model", e, err), "bad file refused");
    TEST_REQUIRE(read_file(path) == "{ not json", "bad file untouched");
    unlink(path.c_str());

    // max_output merges per key like "models", and global_scope_json
    // carries it so a Settings save can't drop the user's caps.
    ConfigLayer base, overlay;
    base.values.model_max_output = {{"a", 1}, {"b", 2}};
    overlay.values.model_max_output = {{"b", 3}};
    AppConfig merged = merge(base, overlay);
    TEST_REQUIRE(merged.model_max_output["a"] == 1 && merged.model_max_output["b"] == 3,
                 "max_output per-key merge");
    auto j = global_scope_json(merged);
    TEST_REQUIRE(j.contains("max_output") && j["max_output"]["b"] == 3,
                 "max_output in global scope");
    std::cout << "[OK] save_model_db_entry round trip\n";
}

int main() {
    context_and_vision_prefix_database();
    max_output_overrides();
    pricing_prefix_database();
    merged_view();
    save_entry_round_trip();
    std::cout << "All model database tests passed\n";
    return 0;
}
