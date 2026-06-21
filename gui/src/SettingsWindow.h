#pragma once

#include <Window.h>
#include <Messenger.h>
#include <map>
#include <string>
#include <vector>

#include <haicode/config.h>

class BListView;
class BStringView;
class BTextControl;
class BRadioButton;
class BMenuField;
class BPopUpMenu;

// Modal-ish editor for a single provider entry. Owned by SettingsWindow;
// posts MSG_PROVIDER_DIALOG_DONE back to the parent with the edited fields.
class ProviderEditWindow : public BWindow {
public:
    // If editing_id is non-empty, dialog pre-fills with existing values and
    // disables the id field (rename not allowed on edit to avoid shadowing).
    ProviderEditWindow(BMessenger target,
                       const std::string& editing_id,
                       const std::string& type,
                       const std::string& api_key,
                       const std::string& base_url);

    void MessageReceived(BMessage* msg) override;

private:
    void _Done();

    BMessenger  target_;
    bool        editing_;
    std::string existing_key_;  // real key (when editing); posted back if field left blank
};

class SettingsWindow : public BWindow {
public:
    // config is the full current AppConfig; its providers seed the Providers
    // tab and its scalars seed the General/Tools tabs.
    SettingsWindow(const haicode::AppConfig& config,
                   BMessenger target);

    void MessageReceived(BMessage* msg) override;

private:
    void _RepopulateList();
    std::string _SummaryFor(const haicode::ProviderConfig& p) const;
    void _OpenEditor(const std::string& editing_id);
    void _ApplyDialogResult(BMessage* msg);
    void _Save();
    void _FetchModelsForMarkedProvider();
    std::string _MarkedProviderId() const;
    void _RefreshContextField();   // sync context field to marked model's window

    // Working copy of the full config; config_.providers is mutated by
    // add/edit/remove, and the scalar fields are read from the General/Tools
    // tab controls at save time.
    haicode::AppConfig config_;

    // Providers tab
    BListView*   list_       = nullptr;
    BStringView* empty_hint_ = nullptr;

    // General tab
    BPopUpMenu*  provider_menu_    = nullptr;
    BMenuField*  provider_field_   = nullptr;
    BPopUpMenu*  model_menu_       = nullptr;
    BMenuField*  model_field_      = nullptr;
    BTextControl* context_field_   = nullptr;
    BRadioButton* mode_plan_radio_   = nullptr;
    BRadioButton* mode_build_radio_  = nullptr;

    // Tools tab
    BTextControl* build_cmd_field_   = nullptr;
    BRadioButton* ws_mojeek_radio_   = nullptr;
    BRadioButton* ws_ddglite_radio_  = nullptr;
    BRadioButton* ws_ddghtml_radio_  = nullptr;
    BTextControl* ws_max_field_      = nullptr;

    BMessenger   target_;
};
