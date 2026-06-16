#pragma once
#include <string>
#include <vector>
#include <optional>
#include <map>
#include <functional>
#include <cstdint>

namespace haicode {

struct TokenUsage {
    int input = 0;
    int output = 0;
    int reasoning = 0;
    int cache_read = 0;
    int cache_write = 0;
};

struct ModelRef {
    std::string id;          // e.g. "claude-opus-4-5"
    std::string provider_id; // e.g. "anthropic"
};

enum class PermissionEffect { Allow, Deny, Ask };

struct PermissionRule {
    std::string action;    // "bash", "edit", "read", "*"
    std::string resource;  // path/command or "*"
    PermissionEffect effect;
};

} // namespace haicode
