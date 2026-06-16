#pragma once
#include <string>
#include <map>

namespace haicode {

// Returns the context window (max input tokens) for a given model, or 0 if
// unknown. Resolution order:
//   1. Exact match in `config_overrides` (keyed by model_id).
//   2. Hardcoded prefix table (longest-prefix match) covering common
//      Anthropic, OpenAI, and Llama models.
//   3. 0 (unknown) — caller should render "—" rather than a number.
int get_context_window(const std::string& provider_id,
                       const std::string& model_id,
                       const std::map<std::string, int>& config_overrides);

} // namespace haicode
