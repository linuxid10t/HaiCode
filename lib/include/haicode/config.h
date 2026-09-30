#pragma once
#include "types.h"
#include "pricing.h"
#include <string>
#include <map>
#include <vector>
#include <optional>
#include <set>
#include <functional>
#include <nlohmann/json.hpp>

namespace haicode {

struct ProviderConfig {
    std::string id;
    std::string type;  // "anthropic" or "openai"; inferred from id if empty
    std::string api_key;
    std::string base_url;
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
    // Thinking-block display in the chat view: "off" (always collapsed),
    // "on" (always expanded), or "on_while_thinking" (expanded while
    // streaming, collapsed after). Empty = "on_while_thinking".
    std::string thinking_display;
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

    // Load a single JSON file (returns empty config on missing file).
    // Thin wrapper over load_layer() for callers that don't care about key
    // presence (tests, tooling).
    AppConfig load_file(const std::string& path);
};

// One config file's parse result: the values plus WHICH keys were present in
// the file. Presence — not comparison against struct defaults — drives
// merging: an absent project key must never clobber the global layer, and a
// project key whose value happens to equal the struct default (e.g.
// "auto_compact": true) must still count as an explicit override.
struct ConfigLayer {
    AppConfig values;               // struct defaults for absent keys
    std::set<std::string> present;  // "default_mode", "web_search/max_results",
                                    // "providers/<id>/api_key", ...
    bool has(const std::string& key) const { return present.count(key) != 0; }
};

// Parse one file into a layer. A missing or invalid file yields an empty
// layer (nothing present), so it can never clobber the other layer.
ConfigLayer load_layer(const std::string& path);

// Presence-based merge: the overlay wins exactly for keys present in the
// overlay layer (per subkey for provider entries); collections (permissions,
// instructions, skills) keep append semantics and per-key maps (models,
// vision, pricing, search api keys) keep per-key overlay semantics.
AppConfig merge(const ConfigLayer& base, const ConfigLayer& overlay);

// One configurable permission source (global or project file), kept
// un-merged so the policy editor can show and edit each independently.
struct PermissionPolicyDocument {
    std::string path;                   // actual file path on disk
    bool exists = false;                // file present (vs. would-be-created)
    std::vector<PermissionRule> rules;  // this source's rules only, in order
    // Canonical serialization of `rules` as they were read. Pass back to
    // save_permission_document() to detect concurrent edits.
    std::string fingerprint;
};

std::string global_config_path();                       // settings:/haicode/config.json
std::string project_config_path(const std::string& project_dir);

// Read one source's permission rules without merging. A missing or invalid
// file yields exists=false / empty rules, never an exception.
PermissionPolicyDocument load_permission_document(const std::string& path);

// Atomically replace ONLY the "permissions" array in the file at `path`,
// preserving every unrelated key. Fails (false + error) when the existing
// file is unparseable, its permissions changed since `expected_fingerprint`
// (conflicting edit), or the write could not be completed. An empty rule
// list removes the key. Missing parent directories are created.
bool save_permission_document(const std::string& path,
                              const std::vector<PermissionRule>& rules,
                              const std::string& expected_fingerprint,
                              std::string& error);

// Central, non-destructive config-file update: read the existing JSON object
// at `path` (a missing file starts from {}), apply `mutate`, and write the
// result back atomically (mkstemp + fsync + rename) preserving every key the
// mutate lambda did not touch. An existing file that does not parse as a
// JSON object is NEVER overwritten — the call fails with an error naming the
// path so the caller can surface it (a hand-edited typo must not cost the
// user their API keys). Missing parent directories are created.
bool update_config_file(const std::string& path,
                        const std::function<void(nlohmann::json&)>& mutate,
                        std::string& error);

} // namespace haicode
