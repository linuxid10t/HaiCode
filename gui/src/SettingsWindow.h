#pragma once

#include <Window.h>
#include <Messenger.h>
#include <string>

class BTextControl;

class SettingsWindow : public BWindow {
public:
    SettingsWindow(const std::string& anthropic_key,
                   const std::string& anthropic_url,
                   const std::string& openai_key,
                   const std::string& openai_url,
                   BMessenger target);

    void MessageReceived(BMessage* msg) override;

private:
    void _Save();

    BTextControl* anthropic_key_field_  = nullptr;
    BTextControl* anthropic_url_field_  = nullptr;
    BTextControl* openai_key_field_     = nullptr;
    BTextControl* openai_url_field_     = nullptr;
    BMessenger    target_;
};
