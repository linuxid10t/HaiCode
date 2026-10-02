#include <haicode/model_db.h>
#include <haicode/config.h>
#include <haicode/model_info.h>

#include <type_traits>

namespace haicode {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
    return s;
}

// "<kind>:<prefix>" / "*:<prefix>" → "<prefix>".
std::string pricing_model_part(const std::string& key) {
    size_t colon = key.find(':');
    return colon == std::string::npos ? key : key.substr(colon + 1);
}

} // namespace

ModelOverrides model_overrides_from(const AppConfig& cfg) {
    ModelOverrides o;
    o.contexts   = cfg.model_contexts;
    o.max_output = cfg.model_max_output;
    o.vision     = cfg.model_vision;
    o.pricing    = cfg.pricing;
    return o;
}

ModelDbEntry user_entry(const ModelOverrides& user, const std::string& key) {
    ModelDbEntry e;
    if (auto it = user.contexts.find(key); it != user.contexts.end())
        e.context = it->second;
    if (auto it = user.max_output.find(key); it != user.max_output.end())
        e.max_output = it->second;
    if (auto it = user.vision.find(key); it != user.vision.end())
        e.vision = it->second;
    if (auto it = user.pricing.find(key); it != user.pricing.end())
        e.pricing = it->second;
    return e;
}

// Longest key in `m` that case-insensitively prefixes `id` (0 = none);
// int maps ignore non-positive values, as the lookups do.
template <typename V>
size_t longest_key(const std::string& id, const std::map<std::string, V>& m) {
    const std::string lid = lower(id);
    size_t best = 0;
    for (auto& [k, v] : m) {
        if constexpr (std::is_same_v<V, int>) { if (v <= 0) continue; }
        if (!k.empty() && lid.rfind(lower(k), 0) == 0 && k.size() > best)
            best = k.size();
    }
    return best;
}

std::vector<ModelDbRow> build_model_database(const ModelOverrides& user) {
    // Built-in tables as prefix maps, for match lengths and exact-key flags.
    std::map<std::string, int> b_ctx, b_out;
    std::map<std::string, bool> b_vis;
    for (auto& [k, v] : builtin_context_windows()) b_ctx.emplace(k, v);
    for (auto& [k, v] : builtin_max_outputs())     b_out.emplace(k, v);
    for (auto& [k, v] : builtin_vision_entries())  b_vis.emplace(k, v);
    // Model part → provider kind of its first built-in price ("*" = any).
    std::map<std::string, std::string> price_kind;
    for (auto& [key, _] : builtin_pricing_entries()) {
        size_t colon = key.find(':');
        price_kind.emplace(lower(pricing_model_part(key)),
                           colon == std::string::npos ? "*" : key.substr(0, colon));
    }

    // Keys merge case-insensitively; a user key shows the user's spelling,
    // since that is the key a save must rewrite.
    std::map<std::string, std::string> keys;  // lower → display
    for (auto& [k, _] : b_ctx) keys.emplace(lower(k), k);
    for (auto& [k, _] : b_out) keys.emplace(lower(k), k);
    for (auto& [k, _] : b_vis) keys.emplace(lower(k), k);
    for (auto& [k, _] : price_kind) keys.emplace(k, k);
    auto add_user = [&](const auto& m) {
        for (auto& [k, _] : m) if (!k.empty()) keys[lower(k)] = k;
    };
    add_user(user.contexts);
    add_user(user.max_output);
    add_user(user.vision);
    add_user(user.pricing);

    std::vector<ModelDbRow> out;
    out.reserve(keys.size());
    for (auto& [lk, key] : keys) {
        ModelDbRow r;
        r.key = key;
        r.user = user_entry(user, key);
        r.builtin_key = b_ctx.count(lk) || b_out.count(lk) || b_vis.count(lk)
                     || price_kind.count(lk);

        // Context window / output cap: the engine's lookups, with and
        // without the user's entries.
        if (int v = get_context_window("", key, {}); v > 0) r.builtin.context = v;
        if (int v = get_context_window("", key, user.contexts); v > 0) r.effective.context = v;
        r.user_context = user_context_window(key, user.contexts) > 0;

        if (int v = get_max_output_tokens(key, {}); v > 0) r.builtin.max_output = v;
        if (int v = get_max_output_tokens(key, user.max_output); v > 0) r.effective.max_output = v;
        size_t u_len = longest_key(key, user.max_output);
        r.user_max_output = u_len > 0 && u_len >= longest_key(key, b_out);

        // Vision: unset (not "no") when nothing matches — the lookup's
        // fail-closed false is shown as unknown.
        size_t bv_len = longest_key(key, b_vis);
        size_t uv_len = longest_key(key, user.vision);
        if (bv_len > 0) r.builtin.vision = model_supports_vision(key, {});
        if (bv_len > 0 || uv_len > 0)
            r.effective.vision = model_supports_vision(key, user.vision);
        r.user_vision = uv_len > 0 && uv_len >= bv_len;

        // Pricing depends on the provider kind: resolve under the kind of
        // this key's own built-in price, else Anthropic's then OpenAI's
        // (both also consult the "*:" entries). A user price wins when the
        // lookup returns an entry from the user's map.
        std::vector<std::string> kinds;
        if (auto it = price_kind.find(lk); it != price_kind.end() && it->second != "*")
            kinds.push_back(it->second);
        kinds.push_back("anthropic");
        kinds.push_back("openai");
        for (const std::string& kind : kinds) {
            const ModelPricing* b = lookup_pricing(kind, kind, key, {});
            const ModelPricing* e = lookup_pricing(kind, kind, key, user.pricing);
            if (!b && !e) continue;
            if (b) r.builtin.pricing = *b;
            if (e) r.effective.pricing = *e;
            r.user_pricing = e && e != b;
            break;
        }
        r.price_tiers = r.effective.pricing && !r.user_pricing
                     && has_price_tiers(key);
        out.push_back(std::move(r));
    }
    return out;  // std::map order = lowercased key order
}

bool save_model_db_entry(const std::string& path, const std::string& key,
                         const ModelDbEntry& entry, std::string& error) {
    if (key.empty()) {
        error = "model id or prefix is empty";
        return false;
    }
    return update_config_file(path, [&](nlohmann::json& j) {
        auto set_or_erase = [&](const char* map_key, bool set,
                                const nlohmann::json& value) {
            if (set) {
                if (!j.contains(map_key) || !j[map_key].is_object())
                    j[map_key] = nlohmann::json::object();
                j[map_key][key] = value;
            } else if (j.contains(map_key) && j[map_key].is_object()) {
                j[map_key].erase(key);
                if (j[map_key].empty()) j.erase(map_key);
            }
        };
        set_or_erase("models", entry.context.has_value(),
                     entry.context.value_or(0));
        set_or_erase("max_output", entry.max_output.has_value(),
                     entry.max_output.value_or(0));
        set_or_erase("vision", entry.vision.has_value(),
                     entry.vision.value_or(false));
        nlohmann::json price = nlohmann::json::object();
        if (entry.pricing) {
            price = {{"input", entry.pricing->input},
                     {"output", entry.pricing->output},
                     {"cache_read", entry.pricing->cache_read},
                     {"cache_write", entry.pricing->cache_write}};
        }
        set_or_erase("pricing", entry.pricing.has_value(), price);
    }, error);
}

} // namespace haicode
