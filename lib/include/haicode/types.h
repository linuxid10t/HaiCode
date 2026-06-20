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

// One entry in a session's todo list. `status` is one of "pending",
// "in_progress", or "completed" — the model uses todo_write to replace
// the whole list atomically each turn.
struct Todo {
    std::string content;
    std::string active_form;
    std::string status = "pending";
};

struct ModelRef {
    std::string id;          // e.g. "claude-opus-4-5"
    std::string provider_id; // e.g. "anthropic"
};

// Per-session inference parameters, persisted in the session's model_json.
// Optional fields use a has_* flag so "unset" (provider/model default) is
// distinguishable from an explicit zero.
struct InferenceParams {
    int  max_tokens = 8192;
    bool has_temperature = false;
    double temperature = 0.0;
    bool has_top_p = false;
    double top_p = 0.0;
    // -1 = unset (agent/config default applies); >0 overrides.
    int  max_steps = -1;
    // "" = unset; one of off/minimal/low/medium/high/xhigh/max controls
    // reasoning depth (mapped per-provider: reasoning_effort for OpenAI,
    // output_config.effort for Anthropic).
    std::string reasoning_effort;
};

enum class PermissionEffect { Allow, Deny, Ask };

struct PermissionRule {
    std::string action;    // "bash", "edit", "read", "*"
    std::string resource;  // path/command or "*"
    PermissionEffect effect;
};

// Per-session operating mode. Build = full tool access; Plan = read-only +
// propose_plan only, until the user approves the plan and flips to Build.
enum class SessionMode { Build, Plan };

} // namespace haicode
