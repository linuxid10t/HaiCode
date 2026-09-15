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
    std::string type;  // "anthropic" or "openai"; inferred from id if empty
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
    // Per-model vision-capability overrides (keyed by exact model_id), parsed
    // from the top-level "vision" object in config.json. Used by
    // model_supports_vision() as a hard override on the built-in prefix table;
    // models absent from both are treated as NOT vision-capable (fail-closed).
    std::map<std::string, bool> model_vision;
    // Vision fallback: a (provider, model) pair that IS vision-capable, used
    // to describe images for text-only primary models. Both empty = feature
    // off. Parsed from the top-level "vision_fallback" object in config.json:
    // {"provider": "...", "model": "..."}. Separate from "vision" (a per-model
    // bool override map with different semantics).
    std::string vision_fallback_provider;
    std::string vision_fallback_model;
    // Per-model token-price overrides (USD per 1M tokens), keyed
    // "provider_id:model_id" or "provider_id:model-prefix". Overlays the
    // built-in defaults in lib/src/pricing/pricing.cpp. Populated from the
    // top-level "pricing" object in config.json.
    std::map<std::string, ModelPricing> pricing;
    // web_search tool config. engine = "" (unset; the runtime falls back to
    // "ddg_lite"), or one of "ddg_lite", "ddg_html", "exa", "zai".
    // Must default to empty: merge() treats any non-empty overlay value as
    // authoritative, so a non-empty default would clobber the global config
    // whenever the project config file is absent.
    // Exa and Z.ai are API-key services; keys resolve at execute time from
    // web_search_api_keys (config) with an $EXA_API_KEY / $ZAI_API_KEY fallback.
    std::string web_search_engine;
    int         web_search_max_results = 5;
    // Per-engine API keys, keyed by engine name ("exa", "zai").
    std::map<std::string, std::string> web_search_api_keys;
    // Shell command to run after a successful write or edit tool call. If the
    // command exits non-zero, the output is appended to the tool result so the
    // model sees the build error immediately. Configured via "build_command" in
    // project .haicode/config.json (e.g. "make -C build -j4 2>&1").
    std::string build_command;
    // Initial mode for newly created sessions: "plan" or "build".
    // Defaults to "plan" so new sessions start in Plan mode unless overridden.
    std::string default_mode = "plan";
    // Skill ids (filenames, e.g. "git-commit.md") enabled by default for
    // newly created sessions. Parsed from the top-level "skills" array in
    // config.json (global or project); seeded into each new session's
    // model_json by SessionEngine::create_session.
    std::vector<std::string> default_skills;

    // Auto-compaction: when the input-token usage for a session approaches the
    // model's context window, summarize the older portion of the conversation
    // into a single synthetic message so the session can continue. Disabled
    // entirely when the model's context window is unknown (0).
    bool   auto_compact          = true;
    double auto_compact_threshold = 0.80; // 0.0–1.0 fraction of the window
    int    auto_compact_reserve   = 8192; // tokens reserved for summary + output

    // Checkpoint compaction tuning (lib/src/compaction/compaction.cpp).
    int compaction_buffer            = 8192;  // safety margin off the window
    int compaction_recent_context     = 10240; // retained-tail token budget
    int compaction_summary_max_tokens = 4096;  // summarizer output cap + validation budget

    // Session autonaming. When enabled, new sessions get a title derived from
    // the first user prompt (heuristic, immediate) and — if the refine flag is
    // also set — a concise title generated by a one-shot LLM call after the
    // first turn completes. The master toggle disables both.
    bool   autoname_sessions    = true;
    bool   autoname_llm_refine  = true;
};

class ConfigLoader {
public:
    // Load and merge global + project config
    AppConfig load(const std::string& project_dir);

    // Load a single JSON file (returns empty config on missing file)
    AppConfig load_file(const std::string& path);

    // Merge two configs: overlay values win over base for scalars; collections
    // (permissions, instructions, providers) are merged/appended.
    AppConfig merge(const AppConfig& base, const AppConfig& overlay);
};

} // namespace haicode
