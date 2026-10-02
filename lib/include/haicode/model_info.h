#pragma once
#include <string>
#include <map>
#include <utility>
#include <vector>
#include "provider.h"

namespace haicode {

class Provider;

// "Model database" semantics shared by every per-model lookup below: the
// user's entries (config.json "models"/"vision"/"max_output", keyed by model
// id OR prefix) and the built-in prefix table form ONE table. Keys match
// case-insensitively as prefixes of the model id; the longest matching key
// wins, and a user key wins a tie with a built-in key — so an exact-id entry
// always wins, re-keying a built-in prefix replaces it, and a short user
// prefix ("gpt-5") never shadows a longer built-in one ("gpt-5.5").

// Returns the context window (max input tokens) for a given model, or 0 if
// unknown. Resolution order:
//   1-2. Prefix database: `config_overrides` + the hardcoded table covering
//      common Anthropic, OpenAI, and Llama models (see above).
//   3. 0 (unknown) — caller should render "—" rather than a number.
int get_context_window(const std::string& provider_id,
                       const std::string& model_id,
                       const std::map<std::string, int>& config_overrides);

// The user's window for this model when a user entry wins the prefix
// database (and so outranks live discovery too), else 0. Lets the UI apply
// the same "user entry beats discovery" rule as get_context_window.
int user_context_window(const std::string& model_id,
                        const std::map<std::string, int>& config_overrides);

// Same resolution as above, plus live discovery: a user entry that wins
// the prefix database is returned first; otherwise, when `provider` is
// non-null, provider->get_model_context(model_id) (the discovery path used
// by local-server providers — Ollama, vLLM, etc.) outranks the built-in
// table.
int get_context_window(const std::string& provider_id,
                       const std::string& model_id,
                       const std::map<std::string, int>& config_overrides,
                       const Provider* provider);

// True when a model can accept image inputs (vision), false otherwise.
// Resolution order:
//   1-2. Prefix database: `config_overrides` + the hardcoded table of known
//      vision-capable families (a `false` user entry hides the screenshot
//      tool even for known-vision models).
//   3. false — fail-closed so vision-gated tools are hidden from unknown
//      (possibly text-only) models rather than letting them call the tool
//      and get a provider error.
bool model_supports_vision(const std::string& model_id,
                           const std::map<std::string, bool>& config_overrides);

// Hard per-response output cap for a model (0 = no known cap): prefix
// database of `overrides` + the built-in output-cap table. Used to clamp an
// explicit max_tokens override so it cannot exceed what the API accepts.
int get_max_output_tokens(const std::string& model_id,
                          const std::map<std::string, int>& overrides);

// Same, against the process-wide user output caps installed by
// set_max_output_overrides(). Providers clamp max_tokens while building the
// request body without access to the app config, so the GUI installs the
// merged config's "max_output" map here whenever the config changes.
int get_max_output_tokens(const std::string& model_id);
void set_max_output_overrides(const std::map<std::string, int>& overrides);

// Clamps a requested max_tokens (>0) down to the model's published output
// cap; unchanged when unset (<=0) or no cap is known.
int clamp_max_tokens(const std::string& model_id, int requested);

// Read-only copies of the built-in tables (prefix → value), in table order,
// for the Model Database editor.
std::vector<std::pair<std::string, int>>  builtin_context_windows();
std::vector<std::pair<std::string, int>>  builtin_max_outputs();
std::vector<std::pair<std::string, bool>> builtin_vision_entries();

} // namespace haicode
