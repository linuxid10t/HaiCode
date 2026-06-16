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
// Provider factories
std::shared_ptr<Provider> make_anthropic_provider(const std::string& api_key,
                                                   const std::string& base_url = "");
std::shared_ptr<Provider> make_openai_provider(const std::string& api_key,
                                                const std::string& base_url = "");
// Register all built-in tools into a registry
void register_builtin_tools(ToolRegistry& registry);
} // namespace haicode
