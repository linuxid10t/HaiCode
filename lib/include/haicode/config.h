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

// Canonical provider-map serialization ({"id": {type, api_key, base_url}}).
// Shared by the GUI's provider saves and global_scope_json so the two can't
// drift.
nlohmann::json providers_to_json(const std::map<std::string, ProviderConfig>& providers);

// The set of top-level config keys the Settings window owns at GLOBAL scope.
// Used together with global_scope_json() to sync a save onto the global file:
// keys present in the returned object are written, absent ones erased —
// nothing outside this set is ever touched by a Settings save.
const std::vector<std::string>& global_scope_keys();

// Serialize ONLY the global-scope keys of `cfg` (see global_scope_keys()).
// Deliberately excluded: `build_command` (project-only — each project's
// build hook lives in its own .haicode/config.json), `permissions` (owned
// by the Permissions center's policy editor), `instructions`, `agents`/
// `agent`, and `last_directory` (written on directory change). Keys at
// their default/empty value are omitted, so syncing erases them and the
// file stays minimal.
nlohmann::json global_scope_json(const AppConfig& cfg);

// Sync the Settings-owned global-scope values in `global` onto the config
// file at `path`: every key in global_scope_keys() is replaced from
// global_scope_json(global) (absent ones erased), and ALL other keys —
// permissions, last_directory, instructions, agents, ... — are preserved
// untouched. This is the exact write path of a Settings save, in the lib so
// its non-leak guarantees are testable headless.
bool sync_global_scope(const AppConfig& global, const std::string& path,
                       std::string& error);

// ---- Project trust boundary ------------------------------------------------
//
// A repository's .haicode/config.json is untrusted input: a hostile repo
// could otherwise point provider base_urls at itself (capturing env-fallback
// API keys and the OAuth token), allow-all its permissions, or set a build
// hook that runs arbitrary shell. Rules, enforced in load_project_layer():
//
//   * `providers` NEVER merges from the project layer — credentials and
//     endpoints come from the global layer only, trusted or not.
//   * `permissions`, `build_command`, `agents`, and `web_search.api_keys`
//     are gated: they are stripped unless the user recorded trust for this
//     project's CURRENT gated content (fingerprint match).
//   * Everything else (model, default_mode, vision, ...) merges normally —
//     untrusted projects lose only the keys that grant authority.

// Outcome of consulting the trust store for one project directory.
struct ProjectTrust {
    bool needed = false;       // the project layer carries gated keys
    std::string fingerprint;   // canonical digest of those keys as parsed
    bool granted = false;      // trust record matches the fingerprint
    // Human-readable list of exactly what the gated keys would enable, for
    // the trust prompt ("2 permission rules (including allow-all), build
    // command: ..."). Empty when !needed.
    std::string summary;
};

// Load the project layer with the trust boundary applied: project providers
// are always removed; gated keys are removed unless the trust store at
// `trust_store_path` (the global config file; empty = global_config_path())
// holds a matching fingerprint for realpath(project_dir). `state_out`, when
// given, reports the pre-strip trust state so the GUI can prompt.
ConfigLayer load_project_layer(const std::string& project_dir,
                               const std::string& trust_store_path,
                               ProjectTrust* state_out = nullptr);

// The "trusted_projects" map in the trust-store file:
// {"trusted_projects": {"<realpath(project_dir)>": "<fingerprint>"}}.
// A missing/invalid file or key yields an empty map.
std::map<std::string, std::string> load_trusted_projects(
    const std::string& trust_store_path);

// Record (or replace) the trust fingerprint for realpath(project_dir) via
// update_config_file — atomic, 0600, preserves unrelated keys.
bool store_trust_record(const std::string& trust_store_path,
                        const std::string& project_dir,
                        const std::string& fingerprint,
                        std::string& error);

// Digest of the gated keys exactly as parsed into the layer. Two layers
// describing the same gated content (regardless of formatting or unrelated
// keys) produce the same fingerprint; any gated change does not.
std::string project_gated_fingerprint(const ConfigLayer& project);

// Does the layer carry any gated key?
bool has_gated_keys(const ConfigLayer& project);

// Remove every gated key (values + presence) from the layer, in place.
void strip_untrusted(ConfigLayer& project);

// Full load with a caller-supplied GLOBAL layer: loads the project layer
// through the trust boundary (load_project_layer), merges, and attaches
// agents.md / claude.md. The GUI keeps its (possibly just-edited) global
// layer in memory and re-merges through this after a Settings save.
// `trust_state`, when given, receives the project's pre-strip trust state
// (needed + granted) so the caller can prompt.
AppConfig load_with_layers(const ConfigLayer& global_layer,
                           const std::string& project_dir,
                           ProjectTrust* trust_state = nullptr);

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
