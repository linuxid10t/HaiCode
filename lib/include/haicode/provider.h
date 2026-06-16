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

struct LLMRequest {
    std::string model_id;
    std::string system;
    std::vector<nlohmann::json> messages;
    std::vector<ToolDefinition> tools;
    int max_tokens = 8192;
    std::optional<double> temperature;
};

struct ToolCall {
    std::string id;
    std::string name;
    nlohmann::json input;
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

class Provider {
public:
    virtual ~Provider() = default;
    virtual std::string id() const = 0;
    virtual void stream(const LLMRequest& request, StreamCallbacks callbacks) = 0;
    virtual void cancel() = 0;
    virtual std::vector<std::string> list_models() = 0;
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
