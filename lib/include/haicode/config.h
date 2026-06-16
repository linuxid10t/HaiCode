#pragma once
#include "types.h"
#include <string>
#include <map>
#include <vector>
#include <optional>

namespace haicode {

struct ProviderConfig {
    std::string id;
    std::string api_key;
    std::string base_url;
    std::map<std::string, std::string> env;
};

struct MCPServerConfig {
    enum class Type { Local, Remote };
    Type type = Type::Local;
    std::vector<std::string> command;
    std::string url;
    std::map<std::string, std::string> environment;
    bool disabled = false;
    int timeout_ms = 30000;
};

struct AgentConfig {
    std::string id;
    std::optional<std::string> model;
    std::optional<std::string> system_prompt;
    std::vector<PermissionRule> permissions;
    std::string color;
};

struct AppConfig {
    std::string model;
    std::string agent;
    std::optional<std::string> shell;
    bool autoupdate = false;
    std::map<std::string, ProviderConfig> providers;
    std::map<std::string, MCPServerConfig> mcp;
    std::map<std::string, AgentConfig> agents;
    std::vector<PermissionRule> permissions;
    std::vector<std::string> instructions;
};

class ConfigLoader {
public:
    // Load and merge global + project config
    AppConfig load(const std::string& project_dir);

    // Load a single JSON file (returns empty config on missing file)
    AppConfig load_file(const std::string& path);

private:
    AppConfig merge(const AppConfig& base, const AppConfig& overlay);
    PermissionRule parse_permission(const std::string& effect,
                                    const std::string& action,
                                    const std::string& resource);
};

} // namespace haicode
