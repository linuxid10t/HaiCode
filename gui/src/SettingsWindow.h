#pragma once

#include <Window.h>
#include <Messenger.h>
#include <map>
#include <string>
#include <vector>

#include <haicode/config.h>

class BListView;
class BStringView;

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
    // providers is the current map (id → config) to seed the list.
    SettingsWindow(const std::map<std::string, haicode::ProviderConfig>& providers,
                   BMessenger target);

    void MessageReceived(BMessage* msg) override;

private:
    void _RepopulateList();
    std::string _SummaryFor(const haicode::ProviderConfig& p) const;
    void _OpenEditor(const std::string& editing_id);
    void _ApplyDialogResult(BMessage* msg);
    void _Save();

    // Working copy of the providers map, mutated by add/edit/remove.
    std::map<std::string, haicode::ProviderConfig> providers_;

    BListView*   list_       = nullptr;
    BStringView* empty_hint_ = nullptr;
    BMessenger   target_;
};
