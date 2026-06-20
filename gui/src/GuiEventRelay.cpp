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
        int32 input_tokens  = 0;
        int32 output_tokens = 0;
        double cost_usd = 0.0;
        if (data.contains("usage") && data["usage"].is_object()) {
            input_tokens  = data["usage"].value("input",  0);
            output_tokens = data["usage"].value("output", 0);
            cost_usd      = data["usage"].value("cost_usd", 0.0);
        }
        BMessage msg(MSG_STEP_ENDED);
        msg.AddString("finish_reason", finish_reason.c_str());
        msg.AddInt32("usage_input",  input_tokens);
        msg.AddInt32("usage_output", output_tokens);
        msg.AddDouble("cost_usd",    cost_usd);
        main_window_.SendMessage(&msg);
    });

    // StepStarted → MSG_STEP_STARTED (so the UI can flip the thinking
    // indicator on before the first text delta arrives)
    bus_.subscribe(EventType::StepStarted, [this](const json& data) {
        std::string sid = data.value("session_id", "");
        if (!is_active_session(sid)) return;

        BMessage msg(MSG_STEP_STARTED);
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

    // PlanProposed → MSG_PLAN_PROPOSED
    bus_.subscribe(EventType::PlanProposed, [this](const json& data) {
        std::string sid = data.value("session_id", "");
        if (!is_active_session(sid)) return;

        std::string plan = data.value("plan", "");
        std::string path = data.value("path", "");
        BMessage msg(MSG_PLAN_PROPOSED);
        msg.AddString("plan", plan.c_str());
        msg.AddString("path", path.c_str());
        main_window_.SendMessage(&msg);
    });

    // TodoUpdated → MSG_TODOS_UPDATED
    // Pack the list as parallel repeated-string fields; MainWindow rebuilds.
    bus_.subscribe(EventType::TodoUpdated, [this](const json& data) {
        std::string sid = data.value("session_id", "");
        if (!is_active_session(sid)) return;

        BMessage msg(MSG_TODOS_UPDATED);
        if (data.contains("todos") && data["todos"].is_array()) {
            for (auto& t : data["todos"]) {
                msg.AddString("todo_content", t.value("content",    "").c_str());
                msg.AddString("todo_active", t.value("activeForm", "").c_str());
                msg.AddString("todo_status", t.value("status",     "pending").c_str());
            }
        }
        main_window_.SendMessage(&msg);
    });

    // BuildHookResult → MSG_BUILD_HOOK
    bus_.subscribe(EventType::BuildHookResult, [this](const json& data) {
        std::string sid = data.value("session_id", "");
        if (!is_active_session(sid)) return;

        BMessage msg(MSG_BUILD_HOOK);
        msg.AddBool("success",   data.value("success", false));
        msg.AddInt32("exit_code", data.value("exit_code", -1));
        main_window_.SendMessage(&msg);
    });

    // Interrupted → MSG_INTERRUPTED (engine confirms the runner thread stopped)
    bus_.subscribe(EventType::Interrupted, [this](const json& data) {
        std::string sid = data.value("session_id", "");
        if (!is_active_session(sid)) return;

        BMessage msg(MSG_INTERRUPTED);
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
