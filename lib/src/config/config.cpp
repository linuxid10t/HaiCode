#include <haicode/config.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
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

    return merge(global_cfg, project_cfg);
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
    } catch (...) {}

    return cfg;
}

AppConfig ConfigLoader::merge(const AppConfig& base, const AppConfig& overlay) {
    AppConfig result = base;
    if (!overlay.model.empty()) result.model = overlay.model;
    if (!overlay.agent.empty()) result.agent = overlay.agent;
    if (overlay.shell) result.shell = overlay.shell;
    for (auto& [k, v] : overlay.providers)
        result.providers[k] = v;
    for (auto& r : overlay.permissions)
        result.permissions.push_back(r);
    for (auto& s : overlay.instructions)
        result.instructions.push_back(s);
    return result;
}

PermissionRule ConfigLoader::parse_permission(const std::string& effect,
                                               const std::string& action,
                                               const std::string& resource) {
    PermissionRule r;
    r.action = action;
    r.resource = resource;
    if (effect == "allow") r.effect = PermissionEffect::Allow;
    else if (effect == "deny") r.effect = PermissionEffect::Deny;
    else r.effect = PermissionEffect::Ask;
    return r;
}

} // namespace haicode
