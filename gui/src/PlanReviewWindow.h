#pragma once

#include <Window.h>
#include <Messenger.h>

#include <string>

class PlanReviewWindow : public BWindow {
public:
    PlanReviewWindow(const std::string& plan_markdown,
                     const std::string& plan_path,
                     const std::string& session_id,
                     BMessenger reply_target);

    void MessageReceived(BMessage* msg) override;
    bool QuitRequested() override;

private:
    void _SendDecision(bool approved);

    BMessenger   reply_target_;
    std::string  session_id_;
    bool         decided_ = false;
};
