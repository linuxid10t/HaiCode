#include <haicode/config.h>
#include <haicode/default_prompt.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <sys/stat.h>
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
    AppConfig global_cfg;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &settings_path) == B_OK) {
        std::string global_path = std::string(settings_path.Path()) + "/haicode/config.json";
        global_cfg = load_file(global_path);
    }

    // Project config
    AppConfig project_cfg = load_file(project_dir + "/.haicode/config.json");

    AppConfig result = merge(global_cfg, project_cfg);

    // Project-only: read agents.md, falling back to claude.md. If both exist,
    // agents.md wins. Empty content is treated as absent (no block emitted by
    // the engine). Unreadable-but-exists logs a warning.
    std::string agents_path = project_dir + "/" + kAgentsMdFilename;
    std::string claude_path = project_dir + "/" + kClaudeMdFilename;
    std::string content = read_file(agents_path);
    std::string source  = kAgentsMdFilename;
    if (content.empty()) {
        struct stat st;
        if (::stat(agents_path.c_str(), &st) == 0) {
            fprintf(stderr, "[config] warning: %s exists but could not be read\n",
                    agents_path.c_str());
        }
        content = read_file(claude_path);
        source  = kClaudeMdFilename;
        if (content.empty()) {
            struct stat cst;
            if (::stat(claude_path.c_str(), &cst) == 0) {
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

AppConfig ConfigLoader::load_file(const std::string& path) {
    std::string content = read_file(path);
    AppConfig cfg;
    if (content.empty()) return cfg;

    try {
        auto j = nlohmann::json::parse(content, nullptr, false);
        if (j.is_discarded()) return cfg;

        if (j.contains("model") && j["model"].is_string())
            cfg.model = j["model"].get<std::string>();
        if (j.contains("provider") && j["provider"].is_string())
            cfg.provider = j["provider"].get<std::string>();
        if (j.contains("agent") && j["agent"].is_string())
            cfg.agent = j["agent"].get<std::string>();
        if (j.contains("shell") && j["shell"].is_string())
            cfg.shell = j["shell"].get<std::string>();

        if (j.contains("providers") && j["providers"].is_object()) {
            for (auto& [k, v] : j["providers"].items()) {
                ProviderConfig p;
                p.id = k;
                if (v.contains("api_key") && v["api_key"].is_string())
                    p.api_key = v["api_key"].get<std::string>();
                if (v.contains("base_url") && v["base_url"].is_string())
                    p.base_url = v["base_url"].get<std::string>();
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
        }

        if (j.contains("permissions") && j["permissions"].is_array()) {
            for (auto& p : j["permissions"])
                if (p.is_object())
                    append_permission(cfg.permissions, p);
        }

        if (j.contains("instructions") && j["instructions"].is_array()) {
            for (auto& s : j["instructions"])
                if (s.is_string())
                    cfg.instructions.push_back(s.get<std::string>());
        }

        // Per-model context-window overrides: {"models": {"foo": 128000, ...}}
        if (j.contains("models") && j["models"].is_object()) {
            for (auto& [k, v] : j["models"].items()) {
                if (v.is_number_integer())
                    cfg.model_contexts[k] = v.get<int>();
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
            }
        }

        // web_search tool config: {"web_search": {"engine": "mojeek", "max_results": 5}}
        if (j.contains("web_search") && j["web_search"].is_object()) {
            auto& ws = j["web_search"];
            if (ws.contains("engine") && ws["engine"].is_string())
                cfg.web_search_engine = ws["engine"].get<std::string>();
            if (ws.contains("max_results") && ws["max_results"].is_number_integer()) {
                int n = ws["max_results"].get<int>();
                if (n > 0) cfg.web_search_max_results = n;
            }
        }
    } catch (...) {}

    return cfg;
}

AppConfig ConfigLoader::merge(const AppConfig& base, const AppConfig& overlay) {
    AppConfig result = base;
    if (!overlay.model.empty()) result.model = overlay.model;
    if (!overlay.provider.empty()) result.provider = overlay.provider;
    if (!overlay.agent.empty()) result.agent = overlay.agent;
    if (overlay.shell) result.shell = overlay.shell;
    for (auto& [k, v] : overlay.providers)
        result.providers[k] = v;
    for (auto& [id, ov] : overlay.agents) {
        AgentConfig& dst = result.agents[id];
        dst.id = id;
        if (ov.model)         dst.model         = *ov.model;
        if (ov.system_prompt) dst.system_prompt = *ov.system_prompt;
        if (ov.max_steps)     dst.max_steps     = *ov.max_steps;
        if (!ov.color.empty()) dst.color        = ov.color;
        if (!ov.permissions.empty()) dst.permissions = ov.permissions;
    }
    for (auto& r : overlay.permissions)
        result.permissions.push_back(r);
    for (auto& s : overlay.instructions)
        result.instructions.push_back(s);
    for (auto& [k, v] : overlay.model_contexts)
        result.model_contexts[k] = v;
    for (auto& [k, v] : overlay.pricing)
        result.pricing[k] = v;
    if (!overlay.web_search_engine.empty())
        result.web_search_engine = overlay.web_search_engine;
    if (overlay.web_search_max_results > 0)
        result.web_search_max_results = overlay.web_search_max_results;
    return result;
}

} // namespace haicode
