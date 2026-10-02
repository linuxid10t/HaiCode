#include <haicode/config.h>
#include <haicode/default_prompt.h>
#include <haicode/util.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <FindDirectory.h>
#include <Path.h>

namespace haicode {

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Parse one permission object {action, resource, effect} into `out`.
// `effect` defaults to "ask" when missing or unrecognized. Entries with no
// action are silently skipped.
static void append_permission(std::vector<PermissionRule>& out,
                              const nlohmann::json& p,
                              PermissionEffect default_effect = PermissionEffect::Ask) {
    std::string action   = p.value("action", "");
    std::string resource = p.value("resource", "*");
    std::string effect   = p.value("effect", "");
    if (action.empty()) return;
    PermissionEffect e = default_effect;
    if (effect == "allow") e = PermissionEffect::Allow;
    else if (effect == "deny") e = PermissionEffect::Deny;
    else if (effect == "ask")  e = PermissionEffect::Ask;
    out.push_back({action, resource, e});
}

AppConfig ConfigLoader::load(const std::string& project_dir) {
    // Global config: B_USER_SETTINGS_DIRECTORY/haicode/config.json
    BPath settings_path;
    ConfigLayer global_layer;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &settings_path) == B_OK) {
        std::string global_path = std::string(settings_path.Path()) + "/haicode/config.json";
        global_layer = load_layer(global_path);
    }
    return load_with_layers(global_layer, project_dir);
}

// Case-insensitive instruction-file lookup: BFS is case-sensitive, so a
// repo shipping AGENTS.md (the common convention) would be silently
// ignored. Scans the project directory for an entry whose name equals
// base_name ignoring case; the exact spelling wins when several variants
// exist. Returns "" when there is no such entry (or the directory cannot
// be read).
std::string find_project_instructions_file(const std::string& project_dir,
                                           const char* base_name) {
    DIR* d = opendir(project_dir.c_str());
    if (!d) return "";
    const size_t len = strlen(base_name);
    std::string found;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        const char* name = ent->d_name;
        size_t i = 0;
        while (i < len && name[i] != '\0'
               && std::tolower((unsigned char)name[i])
                  == std::tolower((unsigned char)base_name[i]))
            ++i;
        if (i != len || name[i] != '\0') continue;
        found = project_dir + "/" + name;
        if (strcmp(name, base_name) == 0)
            break;  // exact spelling beats a case variant seen earlier
    }
    closedir(d);
    return found;
}

AppConfig load_with_layers(const ConfigLayer& global_layer,
                           const std::string& project_dir,
                           ProjectTrust* trust_state) {
    // Project config, with the trust boundary applied.
    ConfigLayer project_layer = load_project_layer(project_dir, "",
                                                   trust_state);

    AppConfig result = merge(global_layer, project_layer);

    // Project-only: read agents.md, falling back to claude.md — both looked
    // up case-insensitively (find_project_instructions_file). If both exist,
    // agents.md wins. Empty content is treated as absent (no block emitted
    // by the engine). Unreadable-but-exists logs a warning.
    std::string agents_path = find_project_instructions_file(project_dir,
                                                             kAgentsMdFilename);
    std::string claude_path = find_project_instructions_file(project_dir,
                                                             kClaudeMdFilename);
    std::string content = read_file(agents_path);
    std::string source  = kAgentsMdFilename;
    if (content.empty()) {
        struct stat st;
        if (!agents_path.empty() && ::stat(agents_path.c_str(), &st) == 0) {
            fprintf(stderr, "[config] warning: %s exists but could not be read\n",
                    agents_path.c_str());
        }
        content = read_file(claude_path);
        source  = claude_path.empty() ? kClaudeMdFilename
                : claude_path.substr(claude_path.rfind('/') + 1);
        if (content.empty()) {
            struct stat cst;
            if (!claude_path.empty() && ::stat(claude_path.c_str(), &cst) == 0) {
                fprintf(stderr, "[config] warning: %s exists but could not be read\n",
                        claude_path.c_str());
            }
        }
    }
    if (!content.empty()) {
        result.agents_md = content;
        fprintf(stderr, "[config] loaded project instructions from %s\n", source.c_str());
    }
    return result;
}

ConfigLayer load_layer(const std::string& path) {
    std::string content = read_file(path);
    ConfigLayer layer;
    AppConfig& cfg = layer.values;
    if (content.empty()) return layer;

    try {
        auto j = nlohmann::json::parse(content, nullptr, false);
        if (j.is_discarded()) return layer;

        // Scalar keys record presence only when a valid value was accepted:
        // an ignored/invalid value must not let the layer clobber the other
        // one with a struct default (that was the old merge bug).
        if (j.contains("model") && j["model"].is_string()) {
            cfg.model = j["model"].get<std::string>();
            layer.present.insert("model");
        }
        if (j.contains("provider") && j["provider"].is_string()) {
            cfg.provider = j["provider"].get<std::string>();
            layer.present.insert("provider");
        }
        if (j.contains("agent") && j["agent"].is_string()) {
            cfg.agent = j["agent"].get<std::string>();
            layer.present.insert("agent");
        }

        if (j.contains("providers") && j["providers"].is_object()) {
            for (auto& [k, v] : j["providers"].items()) {
                ProviderConfig p;
                p.id = k;
                std::string pk = "providers/" + k + "/";
                if (v.contains("type") && v["type"].is_string()) {
                    p.type = v["type"].get<std::string>();
                    layer.present.insert(pk + "type");
                }
                // Default type: "anthropic" id → anthropic, "chatgpt" id →
                // chatgpt (Codex OAuth), else openai.
                if (p.type.empty())
                    p.type = (k == "anthropic") ? "anthropic"
                          : (k == "chatgpt")   ? "chatgpt"
                                               : "openai";
                if (v.contains("api_key") && v["api_key"].is_string()) {
                    p.api_key = v["api_key"].get<std::string>();
                    layer.present.insert(pk + "api_key");
                }
                if (v.contains("base_url") && v["base_url"].is_string()) {
                    p.base_url = v["base_url"].get<std::string>();
                    layer.present.insert(pk + "base_url");
                }
                cfg.providers[k] = p;
            }
        }

        if (j.contains("agents") && j["agents"].is_object()) {
            for (auto& [k, v] : j["agents"].items()) {
                AgentConfig a;
                a.id = k;
                if (v.contains("model") && v["model"].is_string())
                    a.model = v["model"].get<std::string>();
                if (v.contains("system_prompt") && v["system_prompt"].is_string())
                    a.system_prompt = v["system_prompt"].get<std::string>();
                if (v.contains("color") && v["color"].is_string())
                    a.color = v["color"].get<std::string>();
                if (v.contains("max_steps") && v["max_steps"].is_number_integer()) {
                    int ms = v["max_steps"].get<int>();
                    if (ms > 0) a.max_steps = ms;
                }
                if (v.contains("permissions") && v["permissions"].is_array()) {
                    for (auto& p : v["permissions"])
                        if (p.is_object())
                            append_permission(a.permissions, p);
                }
                cfg.agents[k] = std::move(a);
            }
            layer.present.insert("agents");
        }

        if (j.contains("permissions") && j["permissions"].is_array()) {
            for (auto& p : j["permissions"])
                if (p.is_object())
                    append_permission(cfg.permissions, p);
            layer.present.insert("permissions");
        }

        if (j.contains("instructions") && j["instructions"].is_array()) {
            for (auto& s : j["instructions"])
                if (s.is_string())
                    cfg.instructions.push_back(s.get<std::string>());
            layer.present.insert("instructions");
        }

        // Default-enabled skills for new sessions: ["git-commit.md", ...]
        if (j.contains("skills") && j["skills"].is_array()) {
            for (auto& s : j["skills"])
                if (s.is_string())
                    cfg.default_skills.push_back(s.get<std::string>());
            layer.present.insert("skills");
        }

        // Per-model context-window overrides: {"models": {"foo": 128000, ...}}
        if (j.contains("models") && j["models"].is_object()) {
            for (auto& [k, v] : j["models"].items()) {
                if (v.is_number_integer()) {
                    cfg.model_contexts[k] = v.get<int>();
                    layer.present.insert("models/" + k);
                }
            }
        }

        // Per-model vision-capability overrides:
        // {"vision": {"claude-sonnet-4": true, "some-text-only-model": false}}
        if (j.contains("vision") && j["vision"].is_object()) {
            for (auto& [k, v] : j["vision"].items()) {
                if (v.is_boolean()) {
                    cfg.model_vision[k] = v.get<bool>();
                    layer.present.insert("vision/" + k);
                }
            }
        }

        // Per-model output-cap overrides: {"max_output": {"foo": 16384}}
        if (j.contains("max_output") && j["max_output"].is_object()) {
            for (auto& [k, v] : j["max_output"].items()) {
                if (v.is_number_integer()) {
                    cfg.model_max_output[k] = v.get<int>();
                    layer.present.insert("max_output/" + k);
                }
            }
        }

        // Vision fallback pair:
        // {"vision_fallback": {"provider": "openai", "model": "gpt-4o-mini"}}
        // Describes images via this model when the primary is text-only.
        // Per-subkey presence: an overlay naming only the provider keeps the
        // base layer's model.
        if (j.contains("vision_fallback") && j["vision_fallback"].is_object()) {
            const auto& vf = j["vision_fallback"];
            if (vf.contains("provider") && vf["provider"].is_string()) {
                cfg.vision_fallback_provider = vf["provider"].get<std::string>();
                layer.present.insert("vision_fallback/provider");
            }
            if (vf.contains("model") && vf["model"].is_string()) {
                cfg.vision_fallback_model = vf["model"].get<std::string>();
                layer.present.insert("vision_fallback/model");
            }
        }

        // Per-model token-price overrides:
        // {"pricing": {"anthropic:claude-sonnet-4": {"input": 3.0, "output": 15.0,
        //   "cache_read": 0.30, "cache_write": 3.75}}}
        // All values USD per 1M tokens. Unspecified fields default to 0.
        if (j.contains("pricing") && j["pricing"].is_object()) {
            for (auto& [k, v] : j["pricing"].items()) {
                if (!v.is_object()) continue;
                ModelPricing p;
                p.input       = v.value("input",       0.0);
                p.output      = v.value("output",      0.0);
                p.cache_read  = v.value("cache_read",  0.0);
                p.cache_write = v.value("cache_write", 0.0);
                cfg.pricing[k] = p;
                layer.present.insert("pricing/" + k);
            }
        }

        if (j.contains("build_command") && j["build_command"].is_string()) {
            cfg.build_command = j["build_command"].get<std::string>();
            layer.present.insert("build_command");
        }

        // Default session mode: "plan", "chat", or "build". Unrecognized
        // values are ignored so the struct default stands.
        if (j.contains("default_mode") && j["default_mode"].is_string()) {
            std::string dm = j["default_mode"].get<std::string>();
            if (dm == "plan" || dm == "chat" || dm == "build") {
                cfg.default_mode = dm;
                layer.present.insert("default_mode");
            }
        }

        // Thinking-block display: "off", "on", or "on_while_thinking".
        // Unrecognized values are ignored so the struct default stands.
        if (j.contains("thinking_display") && j["thinking_display"].is_string()) {
            std::string td = j["thinking_display"].get<std::string>();
            if (td == "off" || td == "on" || td == "on_while_thinking") {
                cfg.thinking_display = td;
                layer.present.insert("thinking_display");
            }
        }

        // Auto-compaction tuning. Presence is recorded even when the value
        // equals the struct default: an explicit "auto_compact": true in the
        // project layer must re-enable what the global layer turned off.
        if (j.contains("auto_compact") && j["auto_compact"].is_boolean()) {
            cfg.auto_compact = j["auto_compact"].get<bool>();
            layer.present.insert("auto_compact");
        }
        if (j.contains("auto_compact_threshold") && j["auto_compact_threshold"].is_number()) {
            double t = j["auto_compact_threshold"].get<double>();
            if (t > 0.0 && t < 1.0) {
                cfg.auto_compact_threshold = t;
                layer.present.insert("auto_compact_threshold");
            }
        }
        if (j.contains("compaction_buffer") && j["compaction_buffer"].is_number_integer()) {
            int v = j["compaction_buffer"].get<int>();
            if (v > 0) {
                cfg.compaction_buffer = v;
                layer.present.insert("compaction_buffer");
            }
        }
        if (j.contains("compaction_recent_context") && j["compaction_recent_context"].is_number_integer()) {
            int v = j["compaction_recent_context"].get<int>();
            if (v > 0) {
                cfg.compaction_recent_context = v;
                layer.present.insert("compaction_recent_context");
            }
        }
        if (j.contains("compaction_summary_max_tokens") && j["compaction_summary_max_tokens"].is_number_integer()) {
            int v = j["compaction_summary_max_tokens"].get<int>();
            if (v > 0) {
                cfg.compaction_summary_max_tokens = v;
                layer.present.insert("compaction_summary_max_tokens");
            }
        }

        // Session autonaming: master toggle + LLM refine sub-flag.
        if (j.contains("autoname_sessions") && j["autoname_sessions"].is_boolean()) {
            cfg.autoname_sessions = j["autoname_sessions"].get<bool>();
            layer.present.insert("autoname_sessions");
        }
        if (j.contains("autoname_llm_refine") && j["autoname_llm_refine"].is_boolean()) {
            cfg.autoname_llm_refine = j["autoname_llm_refine"].get<bool>();
            layer.present.insert("autoname_llm_refine");
        }

        // web_search tool config: {"web_search": {"engine": "ddg_lite", "max_results": 5}}
        if (j.contains("web_search") && j["web_search"].is_object()) {
            auto& ws = j["web_search"];
            if (ws.contains("engine") && ws["engine"].is_string()) {
                cfg.web_search_engine = ws["engine"].get<std::string>();
                layer.present.insert("web_search/engine");
            }
            if (ws.contains("max_results") && ws["max_results"].is_number_integer()) {
                int n = ws["max_results"].get<int>();
                if (n > 0) {
                    cfg.web_search_max_results = n;
                    layer.present.insert("web_search/max_results");
                }
            }
            if (ws.contains("api_keys") && ws["api_keys"].is_object()) {
                for (auto& [engine, key] : ws["api_keys"].items())
                    if (key.is_string()) {
                        cfg.web_search_api_keys[engine] = key.get<std::string>();
                        layer.present.insert("web_search/api_keys/" + engine);
                    }
            }
        }
    } catch (...) {}

    return layer;
}

AppConfig ConfigLoader::load_file(const std::string& path) {
    return load_layer(path).values;
}

// Presence-based merge: the overlay wins exactly for keys its layer
// recorded as present. Struct defaults never participate, so an absent
// project key can't clobber the global layer and a project key whose value
// equals a struct default still counts as an explicit override.
AppConfig merge(const ConfigLayer& base, const ConfigLayer& overlay) {
    AppConfig result = base.values;
    const AppConfig& ov = overlay.values;

    if (overlay.has("model"))            result.model            = ov.model;
    if (overlay.has("provider"))         result.provider         = ov.provider;
    if (overlay.has("agent"))            result.agent            = ov.agent;
    if (overlay.has("build_command"))    result.build_command    = ov.build_command;
    if (overlay.has("default_mode"))     result.default_mode     = ov.default_mode;
    if (overlay.has("thinking_display")) result.thinking_display = ov.thinking_display;

    // Tuning scalars: presence-driven, so an explicit "auto_compact": true
    // in the project layer re-enables what the global layer turned off (the
    // old default-comparison merge could only ever turn booleans off).
    if (overlay.has("auto_compact"))
        result.auto_compact = ov.auto_compact;
    if (overlay.has("auto_compact_threshold"))
        result.auto_compact_threshold = ov.auto_compact_threshold;
    if (overlay.has("compaction_buffer"))
        result.compaction_buffer = ov.compaction_buffer;
    if (overlay.has("compaction_recent_context"))
        result.compaction_recent_context = ov.compaction_recent_context;
    if (overlay.has("compaction_summary_max_tokens"))
        result.compaction_summary_max_tokens = ov.compaction_summary_max_tokens;
    if (overlay.has("autoname_sessions"))
        result.autoname_sessions = ov.autoname_sessions;
    if (overlay.has("autoname_llm_refine"))
        result.autoname_llm_refine = ov.autoname_llm_refine;

    // Providers: PER-SUBKEY merge. An overlay entry naming only base_url
    // keeps the base entry's api_key/type — the old wholesale replacement
    // dropped credentials whenever a project config touched an entry.
    for (auto& [id, op] : ov.providers) {
        auto it = result.providers.find(id);
        if (it == result.providers.end()) {
            result.providers[id] = op;
            continue;
        }
        ProviderConfig& dst = it->second;
        std::string pk = "providers/" + id + "/";
        if (overlay.has(pk + "type"))    dst.type     = op.type;
        if (overlay.has(pk + "api_key")) dst.api_key  = op.api_key;
        if (overlay.has(pk + "base_url")) dst.base_url = op.base_url;
    }

    // Agents: fields are optional/empty-means-absent, so the existing
    // per-field overlay semantics already encode presence.
    for (auto& [id, oag] : ov.agents) {
        AgentConfig& dst = result.agents[id];
        dst.id = id;
        if (oag.model)         dst.model         = *oag.model;
        if (oag.system_prompt) dst.system_prompt = *oag.system_prompt;
        if (oag.max_steps)     dst.max_steps     = *oag.max_steps;
        if (!oag.color.empty()) dst.color        = oag.color;
        if (!oag.permissions.empty()) dst.permissions = oag.permissions;
    }

    // Ordered collections: overlay entries append after base entries.
    for (auto& r : ov.permissions)
        result.permissions.push_back(r);
    for (auto& s : ov.instructions)
        result.instructions.push_back(s);
    // Append-with-dedup: a project config listing an already-default skill
    // must not enable it twice (the prompt block would embed it twice).
    for (auto& s : ov.default_skills) {
        if (std::find(result.default_skills.begin(),
                      result.default_skills.end(), s)
                == result.default_skills.end())
            result.default_skills.push_back(s);
    }

    // Per-key maps: overlay wins per key, base keys survive.
    for (auto& [k, v] : ov.model_contexts)
        result.model_contexts[k] = v;
    for (auto& [k, v] : ov.model_vision)
        result.model_vision[k] = v;
    for (auto& [k, v] : ov.model_max_output)
        result.model_max_output[k] = v;
    for (auto& [k, v] : ov.pricing)
        result.pricing[k] = v;

    // Vision fallback: per-subkey presence.
    if (overlay.has("vision_fallback/provider"))
        result.vision_fallback_provider = ov.vision_fallback_provider;
    if (overlay.has("vision_fallback/model"))
        result.vision_fallback_model = ov.vision_fallback_model;

    // web_search: per-subkey presence; api_keys per-engine.
    if (overlay.has("web_search/engine"))
        result.web_search_engine = ov.web_search_engine;
    if (overlay.has("web_search/max_results"))
        result.web_search_max_results = ov.web_search_max_results;
    for (auto& [k, v] : ov.web_search_api_keys)
        result.web_search_api_keys[k] = v;

    return result;
}

nlohmann::json providers_to_json(const std::map<std::string, ProviderConfig>& providers) {
    nlohmann::json j = nlohmann::json::object();
    for (auto& [id, p] : providers)
        j[id] = {{"type", p.type}, {"api_key", p.api_key}, {"base_url", p.base_url}};
    return j;
}

const std::vector<std::string>& global_scope_keys() {
    static const std::vector<std::string> keys = {
        "provider", "model", "default_mode", "thinking_display",
        "providers", "web_search", "skills", "models", "vision",
        "max_output", "vision_fallback", "pricing",
        "auto_compact", "auto_compact_threshold",
        "compaction_buffer", "compaction_recent_context",
        "compaction_summary_max_tokens",
        "autoname_sessions", "autoname_llm_refine",
    };
    return keys;
}

nlohmann::json global_scope_json(const AppConfig& cfg) {
    nlohmann::json j = nlohmann::json::object();
    if (!cfg.provider.empty()) j["provider"] = cfg.provider;
    if (!cfg.model.empty())    j["model"]    = cfg.model;
    if (!cfg.default_mode.empty()) j["default_mode"] = cfg.default_mode;
    // Erase-when-default so the file stays minimal for default behavior:
    // empty is the "unset" sentinel (default = "off"); explicit values —
    // including "off" — are persisted as the user's choice.
    if (!cfg.thinking_display.empty())
        j["thinking_display"] = cfg.thinking_display;
    j["providers"] = providers_to_json(cfg.providers);
    j["web_search"] = {
        {"engine", cfg.web_search_engine},
        {"max_results", cfg.web_search_max_results},
    };
    if (!cfg.web_search_api_keys.empty()) {
        nlohmann::json keys_j = nlohmann::json::object();
        for (auto& [engine, key] : cfg.web_search_api_keys)
            keys_j[engine] = key;
        j["web_search"]["api_keys"] = keys_j;
    }
    if (!cfg.default_skills.empty()) {
        nlohmann::json skills_j = nlohmann::json::array();
        for (auto& s : cfg.default_skills)
            skills_j.push_back(s);
        j["skills"] = skills_j;
    }
    if (!cfg.model_contexts.empty()) {
        nlohmann::json models_j = nlohmann::json::object();
        for (auto& [mid, win] : cfg.model_contexts)
            models_j[mid] = win;
        j["models"] = models_j;
    }
    if (!cfg.model_vision.empty()) {
        nlohmann::json vision_j = nlohmann::json::object();
        for (auto& [mid, vis] : cfg.model_vision)
            vision_j[mid] = vis;
        j["vision"] = vision_j;
    }
    if (!cfg.model_max_output.empty()) {
        nlohmann::json out_j = nlohmann::json::object();
        for (auto& [mid, cap] : cfg.model_max_output)
            out_j[mid] = cap;
        j["max_output"] = out_j;
    }
    if (!cfg.vision_fallback_model.empty()) {
        j["vision_fallback"] = {
            {"provider", cfg.vision_fallback_provider},
            {"model",    cfg.vision_fallback_model},
        };
    }
    if (!cfg.pricing.empty()) {
        nlohmann::json pricing_j = nlohmann::json::object();
        for (auto& [k, p] : cfg.pricing)
            pricing_j[k] = {{"input", p.input}, {"output", p.output},
                            {"cache_read", p.cache_read},
                            {"cache_write", p.cache_write}};
        j["pricing"] = pricing_j;
    }
    if (!cfg.auto_compact) j["auto_compact"] = false;
    if (cfg.auto_compact_threshold != 0.80)
        j["auto_compact_threshold"] = cfg.auto_compact_threshold;
    if (cfg.compaction_buffer != 8192)
        j["compaction_buffer"] = cfg.compaction_buffer;
    if (cfg.compaction_recent_context != 10240)
        j["compaction_recent_context"] = cfg.compaction_recent_context;
    if (cfg.compaction_summary_max_tokens != 4096)
        j["compaction_summary_max_tokens"] = cfg.compaction_summary_max_tokens;
    if (!cfg.autoname_sessions) j["autoname_sessions"] = false;
    if (!cfg.autoname_llm_refine) j["autoname_llm_refine"] = false;
    return j;
}

bool sync_global_scope(const AppConfig& global, const std::string& path,
                       std::string& error) {
    return update_config_file(path, [&global](nlohmann::json& j) {
        nlohmann::json scope = global_scope_json(global);
        // Replace each Settings-owned key wholesale; erase the ones at
        // default so the file stays minimal. Everything else in the file —
        // permissions, last_directory, instructions, agents, ... — is
        // untouched.
        for (const auto& key : global_scope_keys()) {
            if (scope.contains(key)) j[key] = scope[key];
            else j.erase(key);
        }
        // build_command is project-only; a global one can only be a leak
        // from releases that saved the merged config wholesale. Erase it so
        // one project's build hook stops running in every project.
        j.erase("build_command");
    }, error);
}

// ---- Source-aware permission policy documents ----

static const char* effect_string(PermissionEffect e) {
    switch (e) {
    case PermissionEffect::Allow: return "allow";
    case PermissionEffect::Deny:  return "deny";
    default:                      return "ask";
    }
}

// Canonical serialization of a rule list: the fingerprint compares by parsed
// content, never raw text, so cosmetic reformatting of the file is not a
// conflict while any semantic permission change is.
static std::string rules_fingerprint(const std::vector<PermissionRule>& rules) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& r : rules)
        arr.push_back({{"action",   r.action},
                       {"resource", r.resource},
                       {"effect",   effect_string(r.effect)}});
    return arr.dump();
}

std::string global_config_path() {
    BPath settings_path;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &settings_path) == B_OK) {
        BPath cfg_path(settings_path);
        cfg_path.Append("haicode");
        cfg_path.Append("config.json");
        return cfg_path.Path();
    }
    return "";
}

std::string project_config_path(const std::string& project_dir) {
    if (project_dir.empty()) return "";
    return project_dir + "/.haicode/config.json";
}

PermissionPolicyDocument load_permission_document(const std::string& path) {
    PermissionPolicyDocument doc;
    doc.path = path;
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) {
        doc.exists = true;
        std::ifstream f(path);
        if (f.is_open()) {
            std::stringstream ss;
            ss << f.rdbuf();
            auto j = nlohmann::json::parse(ss.str(), nullptr, false);
            if (!j.is_discarded() && j.is_object()
                    && j.contains("permissions") && j["permissions"].is_array()) {
                for (const auto& p : j["permissions"])
                    append_permission(doc.rules, p);
            }
        }
    }
    doc.fingerprint = rules_fingerprint(doc.rules);
    return doc;
}

bool save_permission_document(const std::string& path,
                              const std::vector<PermissionRule>& rules,
                              const std::string& expected_fingerprint,
                              std::string& error) {
    error.clear();

    // 1. Re-read and validate the document as it exists now.
    nlohmann::json j = nlohmann::json::object();
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) {
        std::ifstream f(path);
        if (!f.is_open()) {
            error = "cannot open " + path;
            return false;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        j = nlohmann::json::parse(ss.str(), nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            error = path + " is not a valid JSON object";
            return false;
        }
    }

    // 2. Conflicting-edit detection against what the editor loaded.
    std::vector<PermissionRule> current;
    if (j.contains("permissions") && j["permissions"].is_array()) {
        for (const auto& p : j["permissions"])
            append_permission(current, p);
    }
    if (rules_fingerprint(current) != expected_fingerprint) {
        error = "the permissions in " + path
              + " changed since they were loaded";
        return false;
    }

    // 3. Replace only the permissions array; an empty list removes the key.
    if (rules.empty()) {
        j.erase("permissions");
    } else {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& r : rules)
            arr.push_back({{"action",   r.action},
                           {"resource", r.resource},
                           {"effect",   effect_string(r.effect)}});
        j["permissions"] = arr;
    }

    // 4. Write through the shared atomic path (mkstemp sibling + fsync +
    // rename). Default mode: policy files aren't secrets, and preservation
    // keeps an existing file's bits.
    auto slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        std::error_code ec;
        std::filesystem::create_directories(path.substr(0, slash), ec);
    }
    std::string write_err = util::atomic_write_file(path, j.dump(2) + "\n");
    if (!write_err.empty()) {
        error = write_err;
        return false;
    }
    return true;
}

bool update_config_file(const std::string& path,
                        const std::function<void(nlohmann::json&)>& mutate,
                        std::string& error) {
    error.clear();
    if (path.empty()) {
        error = "empty config path";
        return false;
    }

    // Read the existing document; missing file starts from an empty object.
    nlohmann::json j = nlohmann::json::object();
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) {
        std::ifstream f(path);
        if (!f.is_open()) {
            error = "cannot open " + path;
            return false;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        j = nlohmann::json::parse(ss.str(), nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            error = path + " is not valid JSON — refusing to overwrite it";
            return false;
        }
    }

    mutate(j);

    // Create every missing parent (a fresh project's .haicode/ can be
    // arbitrarily deep); create_directories tolerates existing levels.
    auto slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        std::error_code ec;
        std::filesystem::create_directories(path.substr(0, slash), ec);
        // Existing dir vs. genuine failure is distinguished by the write
        // below; a permissions error surfaces there with the path attached.
    }

    // 0600: the file carries provider API keys (and later trust records) —
    // explicit mode wins over any pre-existing group/world-readable bits.
    std::string write_err = util::atomic_write_file(path, j.dump(2) + "\n", 0600);
    if (!write_err.empty()) {
        error = write_err;
        return false;
    }
    return true;
}

// ---- Project trust boundary ----

// Gated keys grant authority; providers are never project-configurable.
static const std::string kWsKeyPrefix = "web_search/api_keys/";

bool has_gated_keys(const ConfigLayer& project) {
    if (project.has("permissions") && !project.values.permissions.empty())
        return true;
    if (project.has("build_command") && !project.values.build_command.empty())
        return true;
    if (project.has("agents") && !project.values.agents.empty())
        return true;
    for (const auto& k : project.present)
        if (k.rfind(kWsKeyPrefix, 0) == 0)
            return true;
    return false;
}

std::string project_gated_fingerprint(const ConfigLayer& project) {
    // Canonical JSON of the gated content AS PARSED: cosmetic reformatting
    // or reordering of the file keeps the fingerprint stable (nlohmann
    // objects serialize in key order), while any semantic change to a
    // gated key invalidates it and re-prompts.
    nlohmann::json j = nlohmann::json::object();
    if (project.has("permissions") && !project.values.permissions.empty()) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& r : project.values.permissions)
            arr.push_back({{"action", r.action}, {"resource", r.resource},
                           {"effect", effect_string(r.effect)}});
        j["permissions"] = arr;
    }
    if (project.has("build_command") && !project.values.build_command.empty())
        j["build_command"] = project.values.build_command;
    if (project.has("agents") && !project.values.agents.empty()) {
        nlohmann::json agents = nlohmann::json::object();
        for (const auto& [id, ag] : project.values.agents) {
            nlohmann::json o = nlohmann::json::object();
            if (ag.model)         o["model"]         = *ag.model;
            if (ag.system_prompt) o["system_prompt"] = *ag.system_prompt;
            if (ag.max_steps)     o["max_steps"]     = *ag.max_steps;
            if (!ag.color.empty()) o["color"]        = ag.color;
            if (!ag.permissions.empty()) {
                nlohmann::json parr = nlohmann::json::array();
                for (const auto& r : ag.permissions)
                    parr.push_back({{"action", r.action},
                                    {"resource", r.resource},
                                    {"effect", effect_string(r.effect)}});
                o["permissions"] = parr;
            }
            agents[id] = o;
        }
        j["agents"] = agents;
    }
    nlohmann::json keys = nlohmann::json::object();
    for (const auto& k : project.present) {
        if (k.rfind(kWsKeyPrefix, 0) != 0) continue;
        std::string engine = k.substr(kWsKeyPrefix.size());
        auto it = project.values.web_search_api_keys.find(engine);
        if (it != project.values.web_search_api_keys.end())
            keys[engine] = it->second;
    }
    if (!keys.empty()) j["web_search_api_keys"] = keys;
    return util::sha256_hex(j.dump());
}

void strip_untrusted(ConfigLayer& project) {
    project.values.permissions.clear();
    project.present.erase("permissions");
    project.values.build_command.clear();
    project.present.erase("build_command");
    project.values.agents.clear();
    project.present.erase("agents");
    project.values.web_search_api_keys.clear();
    for (auto it = project.present.begin(); it != project.present.end();) {
        if (it->rfind(kWsKeyPrefix, 0) == 0)
            it = project.present.erase(it);
        else
            ++it;
    }
}

// Credentials and endpoints are global-only: a project config can never
// register, re-point, or re-key a provider — not even a trusted one.
static void strip_project_providers(ConfigLayer& project) {
    project.values.providers.clear();
    for (auto it = project.present.begin(); it != project.present.end();) {
        if (it->rfind("providers/", 0) == 0)
            it = project.present.erase(it);
        else
            ++it;
    }
}

// Trust records key on the resolved path so aliases (/boot/home/... vs ~/...)
// can't mint separate identities; an unresolvable path falls back to itself.
static std::string canonical_project_key(const std::string& project_dir) {
    char* resolved = realpath(project_dir.c_str(), nullptr);
    if (!resolved) return project_dir;
    std::string key(resolved);
    free(resolved);
    return key;
}

// What the trust prompt shows: exactly what the gated keys would enable.
// Engine NAMES only for search keys — never the secret values.
static std::string trust_summary(const ConfigLayer& project) {
    std::string out;
    if (!project.values.permissions.empty()) {
        size_t n = project.values.permissions.size();
        bool allow_all = false;
        for (const auto& r : project.values.permissions)
            if (r.effect == PermissionEffect::Allow && r.resource == "*")
                allow_all = true;
        out += std::to_string(n) + " permission rule" + (n == 1 ? "" : "s");
        if (allow_all) out += " (including an allow-all rule)";
        out += "\n";
    }
    if (!project.values.build_command.empty())
        out += "build command: " + project.values.build_command + "\n";
    if (!project.values.agents.empty()) {
        out += std::to_string(project.values.agents.size()) + " agent override"
             + (project.values.agents.size() == 1 ? "" : "s") + " (";
        bool first = true;
        for (const auto& [id, a] : project.values.agents) {
            if (!first) out += ", ";
            first = false;
            out += id;
        }
        out += ")\n";
    }
    std::string engines;
    for (const auto& k : project.present) {
        if (k.rfind(kWsKeyPrefix, 0) != 0) continue;
        if (!engines.empty()) engines += ", ";
        engines += k.substr(kWsKeyPrefix.size());
    }
    if (!engines.empty())
        out += "web-search API keys for: " + engines + "\n";
    return out;
}

std::map<std::string, std::string> load_trusted_projects(
    const std::string& store_path) {
    std::string path = store_path.empty() ? global_config_path() : store_path;
    std::map<std::string, std::string> out;
    if (path.empty()) return out;
    std::ifstream f(path);
    if (!f.is_open()) return out;
    std::stringstream ss;
    ss << f.rdbuf();
    auto j = nlohmann::json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return out;
    if (!j.contains("trusted_projects") || !j["trusted_projects"].is_object())
        return out;
    for (auto& [k, v] : j["trusted_projects"].items())
        if (v.is_string())
            out[k] = v.get<std::string>();
    return out;
}

bool store_trust_record(const std::string& store_path,
                        const std::string& project_dir,
                        const std::string& fingerprint,
                        std::string& error) {
    std::string path = store_path.empty() ? global_config_path() : store_path;
    if (path.empty()) {
        error = "cannot determine the settings directory";
        return false;
    }
    std::string key = canonical_project_key(project_dir);
    return update_config_file(path, [&key, &fingerprint](nlohmann::json& j) {
        if (!j.contains("trusted_projects") || !j["trusted_projects"].is_object())
            j["trusted_projects"] = nlohmann::json::object();
        j["trusted_projects"][key] = fingerprint;
    }, error);
}

ConfigLayer load_project_layer(const std::string& project_dir,
                               const std::string& trust_store_path,
                               ProjectTrust* state_out) {
    ConfigLayer project = load_layer(project_dir + "/.haicode/config.json");
    strip_project_providers(project);

    ProjectTrust state;
    if (has_gated_keys(project)) {
        state.needed = true;
        state.fingerprint = project_gated_fingerprint(project);
        auto trusted = load_trusted_projects(trust_store_path);
        auto it = trusted.find(canonical_project_key(project_dir));
        state.granted = it != trusted.end() && it->second == state.fingerprint;
        state.summary = trust_summary(project);
        if (!state.granted)
            strip_untrusted(project);
    }
    if (state_out)
        *state_out = state;
    return project;
}

} // namespace haicode
