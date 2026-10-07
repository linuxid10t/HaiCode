#pragma once
#include <string>
#include <cstdint>

namespace haicode {

// How a model's thinking/reasoning is configured on the wire.
enum class ThinkingMode {
    None,      // no thinking support: never send a thinking param
    Budgeted,  // extended thinking via {type:"enabled", budget_tokens}
               // (3-7 series, 4.5 series) — HaiCode sends no auto config
    Adaptive,  // {type:"adaptive"} accepted (4.6+, 5.x, Fable/Mythos)
};

// Effort bitmask for the Anthropic output_config.effort values a model
// accepts. low/medium/high are always available when effort is supported
// at all; xhigh/max availability is per-model.
constexpr uint8_t kEffortLow    = 1 << 0;
constexpr uint8_t kEffortMedium = 1 << 1;
constexpr uint8_t kEffortHigh   = 1 << 2;
constexpr uint8_t kEffortXHigh  = 1 << 3;
constexpr uint8_t kEffortMax    = 1 << 4;
constexpr uint8_t kEffortAll    = 0x1F;

struct AnthropicModelCaps {
    bool supports_effort = false;
    uint8_t effort_mask = 0;
    ThinkingMode thinking = ThinkingMode::None;
};

// Longest-prefix (case-insensitive) capability lookup for Anthropic model
// ids, verified against the platform docs' per-model thinking/effort
// tables. Unknown ids fail closed: no effort param, no thinking param.
AnthropicModelCaps anthropic_model_caps(const std::string& model_id);

// Maps a UI effort setting ("off"/"minimal"/"low"/"medium"/"high"/"xhigh"/
// "max") to the value actually sent in output_config.effort for this model,
// or "" when the param must be omitted (model without effort support, or
// empty/default UI setting). "off" is not a valid API value: it maps to
// "low", the lowest valid effort. Unsupported high levels step down to the
// nearest valid one.
std::string anthropic_map_effort(const std::string& model_id,
                                 const std::string& ui_effort);

// True for OpenAI reasoning-model families (o1*/o3*/o4*/gpt-5*) that
// require max_completion_tokens instead of max_tokens and reject
// temperature/top_p with 400 unsupported_parameter.
bool openai_is_reasoning_model(const std::string& model_id);

// Maps a UI effort setting to a chat-completions reasoning_effort value,
// or "" when it must be omitted. "off" is not a valid wire value: for
// gpt-5* it maps to "none" (documented default); o-series has no "none",
// so it is omitted there.
std::string openai_map_effort(const std::string& model_id,
                              const std::string& ui_effort);

// Maps a UI effort setting to the reasoning_effort sent to a llama.cpp
// server, or "" when it must be omitted (empty/default UI setting). The
// server turns "none" into enable_thinking=false and hands any other value
// to the model's jinja chat template, so the value must be one the template
// accepts: "off" → "none"; Mistral Small 4 (whose template raises on
// anything but "none"/"high") gets "high" for every other level; other
// models get low/medium/high ("minimal" → "low", "xhigh"/"max" → "high" —
// gpt-oss templates know only those three, the rest ignore the variable).
std::string llamacpp_map_effort(const std::string& model_id,
                                const std::string& ui_effort);

} // namespace haicode
