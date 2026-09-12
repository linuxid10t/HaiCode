#pragma once
#include <string>
#include <map>
#include "provider.h"

namespace haicode {

class Provider;

// Returns the context window (max input tokens) for a given model, or 0 if
// unknown. Resolution order:
//   1. Exact match in `config_overrides` (keyed by model_id).
//   2. Hardcoded prefix table (longest-prefix match) covering common
//      Anthropic, OpenAI, and Llama models.
//   3. 0 (unknown) — caller should render "—" rather than a number.
int get_context_window(const std::string& provider_id,
                       const std::string& model_id,
                       const std::map<std::string, int>& config_overrides);

// Same resolution as above, plus a fallback tier:
//   4. If 1-3 return 0 and `provider` is non-null, consult
//      provider->get_model_context(model_id) — the discovery path used by
//      local-server providers (Ollama, vLLM, etc.).
int get_context_window(const std::string& provider_id,
                       const std::string& model_id,
                       const std::map<std::string, int>& config_overrides,
                       const Provider* provider);

// True when a model can accept image inputs (vision), false otherwise.
// Resolution order:
//   1. Exact match in `config_overrides` (keyed by model_id; a `false`
//      override hides the screenshot tool even for known-vision models).
//   2. Hardcoded prefix table of known vision-capable families.
//   3. false — fail-closed so vision-gated tools are hidden from unknown
//      (possibly text-only) models rather than letting them call the tool
//      and get a provider error.
bool model_supports_vision(const std::string& model_id,
                           const std::map<std::string, bool>& config_overrides);

} // namespace haicode
