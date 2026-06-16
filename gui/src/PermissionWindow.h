#pragma once

#include <Window.h>
#include <Messenger.h>

#include <string>
#include <future>

#include <haicode/tool.h>

class PermissionWindow : public BWindow {
public:
    PermissionWindow(const std::string& action,
                     const std::string& resource,
                     const std::string& detail,
                     BMessenger reply_target,
                     void* promise_ptr);

    void MessageReceived(BMessage* msg) override;
    bool QuitRequested() override;

private:
    void _SendReply(int32 effect);

    BMessenger  reply_target_;
    void*       promise_ptr_;
    bool        promise_fulfilled_ = false;
    std::string action_;
    std::string resource_;
};
