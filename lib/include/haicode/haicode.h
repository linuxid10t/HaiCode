#pragma once
#include "types.h"
#include "config.h"
#include "db.h"
#include "events.h"
#include "provider.h"
#include "tool.h"
#include "engine.h"
#include "util.h"

namespace haicode {
// Provider factories. `id` overrides the provider's reported id; if empty,
// the factory uses the canonical "anthropic"/"openai" id.
std::shared_ptr<Provider> make_anthropic_provider(const std::string& api_key,
                                                   const std::string& base_url = "",
                                                   const std::string& id = "");
std::shared_ptr<Provider> make_openai_provider(const std::string& api_key,
                                                const std::string& base_url = "",
                                                const std::string& id = "");
// Register all built-in tools into a registry
void register_builtin_tools(ToolRegistry& registry);
// Register web_search and web_extract tools. Called by register_builtin_tools.
void register_web_tools(ToolRegistry& registry);
} // namespace haicode
