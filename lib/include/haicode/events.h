#pragma once
#include "types.h"
#include <string>
#include <functional>
#include <map>
#include <vector>
#include <mutex>
#include <nlohmann/json.hpp>

namespace haicode {
namespace events {

enum class EventType {
    Prompted,
    StepStarted,
    StepEnded,
    StepFailed,
    TextStarted,
    TextDelta,
    TextEnded,
    ReasoningStarted,
    ReasoningDelta,
    ReasoningEnded,
    ToolInputStarted,
    ToolInputDelta,
    ToolInputEnded,
    ToolCalled,
    ToolProgress,
    ToolSuccess,
    ToolFailed,
    CompactionStarted,
    CompactionEnded,
    InterruptRequested,
    PermissionRequested,
    PermissionGranted,
    PlanProposed,
};

struct BaseEvent {
    EventType type;
    std::string session_id;
    int64_t timestamp_ms = 0;
};

struct Prompted : BaseEvent {
    std::string user_message_id;
    std::string text;
};

struct StepStarted : BaseEvent {
    std::string assistant_message_id;
    std::string model_id;
};

struct StepEnded : BaseEvent {
    std::string assistant_message_id;
    std::string finish_reason;  // "end_turn", "tool_use", "max_tokens"
    TokenUsage usage;
};

struct StepFailed : BaseEvent {
    std::string error;
};

struct TextDelta : BaseEvent {
    std::string assistant_message_id;
    std::string text_id;
    std::string delta;
};

struct TextEnded : BaseEvent {
    std::string assistant_message_id;
    std::string text_id;
    std::string full_text;
};

struct ToolCalled : BaseEvent {
    std::string assistant_message_id;
    std::string call_id;
    std::string tool_name;
    nlohmann::json input;
};

struct ToolSuccess : BaseEvent {
    std::string call_id;
    std::string output;
};

struct ToolFailed : BaseEvent {
    std::string call_id;
    std::string error;
};

struct PermissionRequested : BaseEvent {
    std::string call_id;
    std::string action;
    std::string resource;
    nlohmann::json tool_input;
};

} // namespace events

// Thread-safe event bus
class SessionEventBus {
public:
    using Handler = std::function<void(const nlohmann::json&)>;

    void subscribe(events::EventType type, Handler handler);
    void unsubscribe_all();

    void publish(events::EventType type, const nlohmann::json& data);

private:
    std::map<events::EventType, std::vector<Handler>> handlers_;
    std::mutex mu_;
};

} // namespace haicode
