#pragma once
#include "types.h"
#include <string>
#include <vector>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>

namespace haicode {

struct ToolDefinition {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
};

enum class FinishReason { EndTurn, ToolUse, MaxTokens, Error, Stopped };

// Anthropic's Messages API requires max_tokens (no omit-and-default); this is
// the fallback when a request leaves it unset. Also the compaction output
// allowance when unset.
inline constexpr int kDefaultMaxTokens = 8192;

struct LLMRequest {
    std::string model_id;
    std::string system;          // byte-stable across turns (cacheable prefix)
    std::string system_dynamic;  // per-step content that varies (e.g. {{STEPS_LEFT}})
    std::vector<nlohmann::json> messages;
    std::vector<ToolDefinition> tools;
    std::optional<int> max_tokens;  // unset = provider/model default
    std::optional<double> temperature;
    std::optional<double> top_p;
    // "" = unset; off/minimal/low/medium/high/xhigh/max. OpenAI maps this to
    // reasoning_effort; Anthropic maps it to output_config.effort.
    std::string reasoning_effort;
};

struct ToolCall {
    std::string id;
    std::string name;
    nlohmann::json input;
    // Set when the streamed tool input could not be parsed (empty/partial JSON).
    // The engine drops these calls so the model isn't shown a phantom empty
    // tool_use on the next turn.
    bool parse_failed = false;
    std::string raw_input;
};

struct LLMResponse {
    std::string assistant_message_id;
    std::string text;
    std::vector<ToolCall> tool_calls;
    FinishReason finish_reason = FinishReason::EndTurn;
    TokenUsage usage;
};

// Streaming callback bundle
struct StreamCallbacks {
    std::function<void(const std::string& text_id, const std::string& delta)> on_text_delta;
    std::function<void(const std::string& delta)> on_reasoning_delta;
    std::function<void(const std::string& call_id, const std::string& name,
                       const std::string& input_delta)> on_tool_input_delta;
    std::function<void(FinishReason, TokenUsage, std::vector<ToolCall>)> on_finish;
    std::function<void(const std::string& error)> on_error;
};

// Per-stream cancellation token. The engine mints one per agentic-loop run
// ("s:<session_id>:r<seq>") and passes it to stream() and cancel(), so
// interrupting session A cannot abort session B's in-flight stream when both
// run through the same shared Provider object. Implementations must scope
// cancellation to the matching stream; an empty token means "cancel every
// stream on this provider" (engine shutdown uses this). A token the provider
// doesn't know (never passed to stream(), or its stream already ended) is a
// no-op.
class Provider {
public:
    virtual ~Provider() = default;
    virtual std::string id() const = 0;
    virtual void stream(const LLMRequest& request, StreamCallbacks callbacks,
                        const std::string& stream_token = "") = 0;
    virtual void cancel(const std::string& stream_token = "") = 0;
    virtual std::vector<std::string> list_models(std::string& error) = 0;
    // Discovered context window for a model, or 0 if unknown. Used as a
    // fallback when get_context_window()'s config/prefix table returns 0,
    // so local servers (Ollama, vLLM, etc.) can populate the context meter
    // and enable auto-compaction without manual config.
    virtual int get_model_context(const std::string& model_id) const { return 0; }
    // Cache-only variant of get_model_context(): returns a previously
    // discovered window without any network I/O, so UI threads can render a
    // provisional value immediately while discovery runs asynchronously.
    virtual int peek_model_context(const std::string& model_id) const { return 0; }
};

class ProviderRegistry {
public:
    void register_provider(std::shared_ptr<Provider> provider);
    std::shared_ptr<Provider> get(const std::string& provider_id) const;
    std::vector<std::string> available_ids() const;

private:
    std::map<std::string, std::shared_ptr<Provider>> providers_;
};

} // namespace haicode
