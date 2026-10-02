#pragma once
#include "pricing.h"
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace haicode {

struct AppConfig;

// The user's model-database entries: the per-model override maps of the
// config file ("models", "max_output", "vision", "pricing"), each keyed by
// a model id or prefix. Built-in tables + these entries form one prefix
// database (see model_info.h): longest key wins, user wins ties.
struct ModelOverrides {
    std::map<std::string, int> contexts;           // "models"
    std::map<std::string, int> max_output;         // "max_output"
    std::map<std::string, bool> vision;            // "vision"
    std::map<std::string, ModelPricing> pricing;   // "pricing"
};

ModelOverrides model_overrides_from(const AppConfig& cfg);

// One user entry as edited in the Model Database window. Unset fields are
// not overridden (the built-in value, if any, applies).
struct ModelDbEntry {
    std::optional<int> context;
    std::optional<int> max_output;
    std::optional<bool> vision;
    std::optional<ModelPricing> pricing;

    bool empty() const {
        return !context && !max_output && !vision && !pricing;
    }
};

// One row of the merged database view, keyed by model id/prefix (the union
// of the built-in tables' keys and the user's keys). Built-in pricing keys
// ("anthropic:claude-opus-5", "*:glm-5") are folded onto their model part.
// Values are RESOLVED for a model whose id equals the key — the same
// longest-prefix lookups the engine runs — so a row shows what that model
// actually gets (e.g. "gpt-5.5" shows vision via the "gpt-5" entry).
struct ModelDbRow {
    std::string key;
    ModelDbEntry builtin;    // resolved from the built-in tables only
    ModelDbEntry user;       // the user's entry under exactly this key
    ModelDbEntry effective;  // resolved with the user's entries applied
    // Per field: the effective value comes from a user entry (this key's
    // or a shorter user prefix that wins the lookup).
    bool user_context = false;
    bool user_max_output = false;
    bool user_vision = false;
    bool user_pricing = false;
    // Effective price is the base of a built-in tier ladder.
    bool price_tiers = false;
    // Some built-in table has an entry under exactly this key.
    bool builtin_key = false;

    bool has_user() const { return !user.empty(); }
};

// Union of the built-in tables' keys and `user`'s keys, sorted by key
// (case-insensitive, merged case-insensitively).
std::vector<ModelDbRow> build_model_database(const ModelOverrides& user);

// The user's entry for exactly `key` (no prefix resolution).
ModelDbEntry user_entry(const ModelOverrides& user, const std::string& key);

// Replace the user's entry for `key` in the config file at `path` through
// update_config_file (atomic, 0600, unrelated keys preserved): set fields
// are written, unset ones erased from their map; maps left empty are
// removed. An empty `entry` removes the key everywhere (revert to built-in).
bool save_model_db_entry(const std::string& path, const std::string& key,
                         const ModelDbEntry& entry, std::string& error);

} // namespace haicode
