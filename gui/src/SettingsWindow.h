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
class BMenuItem;
class BTabView;
class BButton;
class BCheckBox;
class BGroupView;

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
    // Toggle chatgpt-type controls (sign-in button/status) vs key/url fields.
    void _UpdateTypeSpecificUI();

    BMessenger  target_;
    bool        editing_;
    std::string existing_key_;  // real key (when editing); posted back if field left blank
    BButton*      oauth_btn_    = nullptr;
    BStringView*  oauth_status_ = nullptr;
    // Last visibility applied to the oauth / key-url row groups. Hide() and
    // Show() nest (hide count) and IsHidden() lies before the window is
    // shown, so toggling is edge-triggered off these instead.
    bool oauth_rows_visible_ = true;
    bool key_rows_visible_   = true;
};

class SettingsWindow : public BWindow {
public:
    // config is the full current AppConfig; its providers seed the Providers
    // tab and its scalars seed the General/Tools tabs. project_dir locates
    // project-level skill files for the Skills tab (global + project dirs
    // are scanned; project entries shadow same-name global ones).
    SettingsWindow(const haicode::AppConfig& config,
                   BMessenger target,
                   const std::string& project_dir);

    void MessageReceived(BMessage* msg) override;

private:
    void _RepopulateList();
    std::string _SummaryFor(const haicode::ProviderConfig& p) const;
    void _OpenEditor(const std::string& editing_id);
    void _ApplyDialogResult(BMessage* msg);
    void _SendProviderUpdate();
    void _Save();
    void _FetchModelsForMarkedProvider();
    void _FetchFBModelsForMarkedProvider();  // fallback pair's model fetch
    // Rebuild both provider dropdowns from config_.providers (add/remove),
    // preserving the marked ids when still present. Returns nothing; callers
    // compare marks before/after to decide whether a refetch is needed.
    void _RebuildProviderMenus();
    std::string _MarkedProviderId() const;
    std::string _MarkedFBProviderId() const; // fallback provider ("" = "(none)")
    void _RefreshContextField();   // sync context field to marked model's window
    void _RefreshVisionMenu();     // sync vision dropdown to marked model's override
    std::string _MarkedVision() const;           // "auto"/"yes"/"no" of marked item
    std::string _MarkedWSEngine() const;          // engine id of marked menu item
    void _UpdateKeyFieldVisibility();             // show key field for exa/zai only
    void _RememberKeyForEngine(const char* engine_id); // stash typed key per engine

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
    // Vision override for the selected model (Auto/Yes/No), mirroring
    // context_field_'s per-default-model scope.
    BPopUpMenu*  vision_menu_      = nullptr;
    BMenuField*  vision_field_     = nullptr;
    // Vision fallback pair: a vision-capable (provider, model) used to
    // describe images for text-only primaries. "(none)" = feature off.
    BPopUpMenu*  fb_provider_menu_ = nullptr;
    BMenuField*  fb_provider_field_= nullptr;
    BPopUpMenu*  fb_model_menu_    = nullptr;
    BMenuField*  fb_model_field_   = nullptr;
    // Last model fetch errored (empty list + non-empty error). Clicking the
    // respective dropdown re-fetches; the no-key "(none available)" case is
    // not a failure and never arms these (matching MainWindow's rule).
    bool         models_load_failed_    = false;
    bool         fb_models_load_failed_ = false;
    BRadioButton* mode_plan_radio_   = nullptr;
    BRadioButton* mode_build_radio_  = nullptr;

    // Tools tab
    BTextControl* build_cmd_field_   = nullptr;
    BMenuField*   ws_engine_field_   = nullptr;
    BPopUpMenu*   ws_engine_menu_    = nullptr;
    BTextControl* ws_max_field_      = nullptr;
    // Shown only when the selected engine needs an API key (exa/zai).
    BTextControl* ws_key_field_      = nullptr;
    // Hint below the key field, visible when the marked engine has a key
    // stored in config (provider-editor "key already set" pattern).
    BStringView* ws_key_hint_        = nullptr;
    // Key already stored in config for the engine currently shown in the
    // field (not shown in the field itself, which starts empty and is
    // masked); empty field at save time means "keep existing".
    std::string ws_existing_key_;
    // Engine id whose key state the field is currently showing.
    std::string ws_current_engine_;
    // Keys typed but not yet saved, keyed by engine, so switching engines in
    // the dropdown doesn't lose in-progress input.
    std::map<std::string, std::string> ws_typed_keys_;

    // Skills tab: default-enabled skills for new sessions. One checkbox per
    // discovered skill file; checked ids are sent as repeated
    // "default_skill" strings in the MSG_SETTINGS_SAVED message.
    std::string project_dir_;
    std::vector<std::pair<std::string, BCheckBox*>> skill_checks_;
    BGroupView* skills_tab_ = nullptr;

    BMessenger   target_;
};
