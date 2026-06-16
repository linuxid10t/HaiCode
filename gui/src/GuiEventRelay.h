#pragma once

#include <Messenger.h>

#include <haicode/events.h>

#include <string>
#include <mutex>

class GuiEventRelay {
public:
    GuiEventRelay(BMessenger main_window,
                  haicode::SessionEventBus& bus,
                  const std::string& active_session_id);

    void attach();
    void set_active_session(const std::string& sid);

private:
    bool is_active_session(const std::string& sid);

    BMessenger               main_window_;
    haicode::SessionEventBus& bus_;
    std::string              active_session_id_;
    std::mutex               mu_;
};
