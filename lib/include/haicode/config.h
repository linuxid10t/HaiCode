#pragma once
#include "types.h"
#include "pricing.h"
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
    std::optional<int> max_steps;
    std::vector<PermissionRule> permissions;
    std::string color;
};

struct AppConfig {
    std::string model;
    std::string provider;
    std::string agent;
    std::optional<std::string> shell;
    bool autoupdate = false;
    std::map<std::string, ProviderConfig> providers;
    std::map<std::string, MCPServerConfig> mcp;
    std::map<std::string, AgentConfig> agents;
    std::vector<PermissionRule> permissions;
    std::vector<std::string> instructions;
    // Contents of <project_dir>/agents.md (or claude.md fallback), read verbatim
    // by ConfigLoader::load(). Project-specific only; never merged from global config.
    std::string agents_md;
    // Per-model context-window overrides (keyed by exact model_id). Empty by default;
    // populated from the top-level "models" object in config.json. Used by
    // get_context_window() as a hard override on top of the hardcoded prefix table.
    std::map<std::string, int> model_contexts;
    // Per-model token-price overrides (USD per 1M tokens), keyed
    // "provider_id:model_id" or "provider_id:model-prefix". Overlays the
    // built-in defaults in lib/src/pricing/pricing.cpp. Populated from the
    // top-level "pricing" object in config.json.
    std::map<std::string, ModelPricing> pricing;
    // web_search tool config. engine = "mojeek" (default), "ddg_lite", or "ddg_html".
    // Mojeek is the default because DuckDuckGo's lite/html endpoints now serve a
    // CAPTCHA "anomaly" page to most non-browser clients.
    std::string web_search_engine = "mojeek";
    int         web_search_max_results = 5;
    // Shell command to run after a successful write or edit tool call. If the
    // command exits non-zero, the output is appended to the tool result so the
    // model sees the build error immediately. Configured via "build_command" in
    // project .haicode/config.json (e.g. "make -C build -j4 2>&1").
    std::string build_command;
};

class ConfigLoader {
public:
    // Load and merge global + project config
    AppConfig load(const std::string& project_dir);

    // Load a single JSON file (returns empty config on missing file)
    AppConfig load_file(const std::string& path);

private:
    AppConfig merge(const AppConfig& base, const AppConfig& overlay);
};

} // namespace haicode
