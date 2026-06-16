#include "GuiEventRelay.h"
#include "Messages.h"

#include <Message.h>
#include <Messenger.h>

#include <haicode/events.h>

#include <string>
#include <mutex>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace haicode;
using namespace haicode::events;

GuiEventRelay::GuiEventRelay(BMessenger main_window,
                             SessionEventBus& bus,
                             const std::string& active_session_id)
    : main_window_(main_window)
    , bus_(bus)
    , active_session_id_(active_session_id)
{
}

void
GuiEventRelay::set_active_session(const std::string& sid)
{
    std::lock_guard<std::mutex> lock(mu_);
    active_session_id_ = sid;
}

bool
GuiEventRelay::is_active_session(const std::string& sid)
{
    std::lock_guard<std::mutex> lock(mu_);
    return sid == active_session_id_;
}

void
GuiEventRelay::attach()
{
    // TextDelta → MSG_TEXT_DELTA
    bus_.subscribe(EventType::TextDelta, [this](const json& data) {
        std::string sid = data.value("session_id", "");
        if (!is_active_session(sid)) return;

        std::string delta = data.value("delta", "");
        BMessage msg(MSG_TEXT_DELTA);
        msg.AddString("delta", delta.c_str());
        main_window_.SendMessage(&msg);
    });

    // TextEnded → MSG_TEXT_DELTA with empty string signals end (use MSG_STEP_ENDED handling)
    // We use a separate EndStreaming signal via MSG_STEP_ENDED

    // ToolCalled → MSG_TOOL_CALLED
    bus_.subscribe(EventType::ToolCalled, [this](const json& data) {
        std::string sid = data.value("session_id", "");
        if (!is_active_session(sid)) return;

        std::string tool_name   = data.value("tool_name", "");
        std::string input_json;
        if (data.contains("input")) {
            try { input_json = data["input"].dump(2); } catch (...) {}
        }

        BMessage msg(MSG_TOOL_CALLED);
        msg.AddString("tool_name",  tool_name.c_str());
        msg.AddString("input_json", input_json.c_str());
        main_window_.SendMessage(&msg);
    });

    // ToolSuccess → MSG_TOOL_RESULT (success)
    bus_.subscribe(EventType::ToolSuccess, [this](const json& data) {
        std::string sid = data.value("session_id", "");
        if (!is_active_session(sid)) return;

        std::string output = data.value("output", "");
        BMessage msg(MSG_TOOL_RESULT);
        msg.AddString("output",  output.c_str());
        msg.AddBool("success",   true);
        main_window_.SendMessage(&msg);
    });

    // ToolFailed → MSG_TOOL_RESULT (failure)
    bus_.subscribe(EventType::ToolFailed, [this](const json& data) {
        std::string sid = data.value("session_id", "");
        if (!is_active_session(sid)) return;

        std::string error = data.value("error", "");
        BMessage msg(MSG_TOOL_RESULT);
        msg.AddString("output",  error.c_str());
        msg.AddBool("success",   false);
        main_window_.SendMessage(&msg);
    });

    // StepEnded → MSG_STEP_ENDED
    bus_.subscribe(EventType::StepEnded, [this](const json& data) {
        std::string sid = data.value("session_id", "");
        if (!is_active_session(sid)) return;

        std::string finish_reason = data.value("finish_reason", "");
        BMessage msg(MSG_STEP_ENDED);
        msg.AddString("finish_reason", finish_reason.c_str());
        main_window_.SendMessage(&msg);
    });

    // StepFailed → MSG_STEP_FAILED
    bus_.subscribe(EventType::StepFailed, [this](const json& data) {
        std::string sid = data.value("session_id", "");
        if (!is_active_session(sid)) return;

        std::string error = data.value("error", "");
        BMessage msg(MSG_STEP_FAILED);
        msg.AddString("error", error.c_str());
        main_window_.SendMessage(&msg);
    });

    // PermissionRequested → MSG_PERMISSION_REQ
    // Note: The promise_ptr is passed directly (engine thread blocks on future.get())
    // The promise is created in HaiCodeApp's permission callback and packed into the message
    bus_.subscribe(EventType::PermissionRequested, [this](const json& data) {
        std::string sid = data.value("session_id", "");
        if (!is_active_session(sid)) return;

        // The permission callback in HaiCodeApp handles the promise machinery.
        // We don't need to subscribe here — HaiCodeApp wires the callback directly
        // to the PermissionGate, which calls it synchronously on the engine thread.
        // This handler is a no-op; kept for potential future use.
    });
}
