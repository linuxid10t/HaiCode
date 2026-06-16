#include "SettingsWindow.h"
#include "Messages.h"

#include <Application.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <SeparatorView.h>
#include <StringView.h>
#include <TextControl.h>

static const uint32 MSG_SAVE   = 'SAVs';
static const uint32 MSG_CANCEL = 'CANs';

SettingsWindow::SettingsWindow(const std::string& anthropic_key,
                               const std::string& anthropic_url,
                               const std::string& openai_key,
                               const std::string& openai_url,
                               BMessenger target)
    : BWindow(BRect(0, 0, 480, 320),
              "Preferences",
              B_TITLED_WINDOW,
              B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
    , target_(target)
{
    anthropic_key_field_ = new BTextControl("anthropic_key", "Anthropic API key:", "", nullptr);
    anthropic_key_field_->TextView()->HideTyping(true);
    anthropic_key_field_->SetText(anthropic_key.c_str());

    anthropic_url_field_ = new BTextControl("anthropic_url",
        "Base URL (default: api.anthropic.com):", anthropic_url.c_str(), nullptr);

    openai_key_field_ = new BTextControl("openai_key", "OpenAI API key (optional):", "", nullptr);
    openai_key_field_->TextView()->HideTyping(true);
    openai_key_field_->SetText(openai_key.c_str());

    openai_url_field_ = new BTextControl("openai_url",
        "Base URL (required for local servers):", openai_url.c_str(), nullptr);

    // Align label widths
    float labelWidth = 160.0f;
    anthropic_key_field_->SetDivider(labelWidth);
    anthropic_url_field_->SetDivider(labelWidth);
    openai_key_field_->SetDivider(labelWidth);
    openai_url_field_->SetDivider(labelWidth);

    BButton* save_btn   = new BButton("save",   "Save",   new BMessage(MSG_SAVE));
    BButton* cancel_btn = new BButton("cancel", "Cancel", new BMessage(MSG_CANCEL));
    save_btn->MakeDefault(true);

    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(new BStringView("a_label", "Anthropic"))
        .Add(anthropic_key_field_)
        .Add(anthropic_url_field_)
        .Add(new BSeparatorView(B_HORIZONTAL))
        .Add(new BStringView("o_label", "OpenAI / compatible"))
        .Add(openai_key_field_)
        .Add(openai_url_field_)
        .AddGlue()
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(cancel_btn)
            .Add(save_btn)
        .End()
    .End();

    CenterOnScreen();
}

void
SettingsWindow::MessageReceived(BMessage* msg)
{
    switch (msg->what) {
        case MSG_SAVE:
            _Save();
            break;
        case MSG_CANCEL:
            Quit();
            break;
        default:
            BWindow::MessageReceived(msg);
    }
}

void
SettingsWindow::_Save()
{
    BMessage saved(MSG_SETTINGS_SAVED);
    saved.AddString("anthropic_key", anthropic_key_field_->Text());
    saved.AddString("anthropic_url", anthropic_url_field_->Text());
    saved.AddString("openai_key",    openai_key_field_->Text());
    saved.AddString("openai_url",    openai_url_field_->Text());
    target_.SendMessage(&saved);
    Quit();
}
