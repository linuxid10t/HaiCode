#pragma once

#include <Window.h>
#include <Messenger.h>

#include <string>

#include <haicode/permission_requests.h>

// Modeless approval window for one broker request. Replies go to be_app as
// MSG_PERMISSION_DECISION keyed by request id (never a promise pointer);
// Escape and window close deny. No Allow button is the default.
class PermissionWindow : public BWindow {
public:
    // notify_target gets MSG_PERMISSION_WINDOW_CLOSED (with "session_id")
    // after the decision was sent, so MainWindow can surface the next
    // queued request for that session.
    PermissionWindow(const haicode::PermissionRequest& req,
                     const std::string& session_label,
                     const std::string& build_command,
                     BMessenger notify_target);

    void MessageReceived(BMessage* msg) override;
    bool QuitRequested() override;

private:
    void _SendDecision(int32 decision);

    BMessenger notify_target_;
    std::string request_id_;
    std::string session_id_;
    bool replied_ = false;
};
