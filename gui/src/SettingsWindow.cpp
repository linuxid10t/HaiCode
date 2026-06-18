#include "SettingsWindow.h"
#include "Messages.h"

#include <Application.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuItem.h>
#include <RadioButton.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <StringItem.h>
#include <StringView.h>
#include <TextControl.h>

#include <cstdio>
#include <nlohmann/json.hpp>

// ---------------------------------------------------------------------------
// ProviderEditWindow
// ---------------------------------------------------------------------------

ProviderEditWindow::ProviderEditWindow(BMessenger target,
                                       const std::string& editing_id,
                                       const std::string& type,
                                       const std::string& api_key,
                                       const std::string& base_url)
    : BWindow(BRect(0, 0, 460, 280),
              editing_id.empty() ? "Add Provider" : ("Edit Provider: " + editing_id).c_str(),
              B_TITLED_WINDOW,
              B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
    , target_(target)
    , editing_(!editing_id.empty())
{
    auto* id_field = new BTextControl("id", "Provider id:", editing_id.c_str(), nullptr);
    if (editing_) id_field->SetEnabled(false);

    auto* key_field = new BTextControl("api_key", "API key:", api_key.c_str(), nullptr);
    key_field->TextView()->HideTyping(true);

    auto* url_field = new BTextControl("base_url", "Base URL (optional):", base_url.c_str(), nullptr);

    bool want_anthropic = (type != "openai");
    auto* ant_radio = new BRadioButton("type_anthropic", "Anthropic", nullptr);
    auto* oai_radio = new BRadioButton("type_openai",    "OpenAI-compatible", nullptr);
    (want_anthropic ? ant_radio : oai_radio)->SetValue(B_CONTROL_ON);

    float label_w = 130.0f;
    id_field->SetDivider(label_w);
    key_field->SetDivider(label_w);
    url_field->SetDivider(label_w);

    auto* ok_btn = new BButton("ok", "OK", new BMessage(MSG_PROVIDER_DIALOG_DONE));
    ok_btn->MakeDefault(true);
    auto* cancel_btn = new BButton("cancel", "Cancel",
                                   new BMessage(B_QUIT_REQUESTED));

    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(id_field)
        .Add(key_field)
        .Add(url_field)
        .AddGroup(B_HORIZONTAL)
            .Add(ant_radio)
            .Add(oai_radio)
            .AddGlue()
        .End()
        .AddGlue()
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(cancel_btn)
            .Add(ok_btn)
        .End()
    .End();

    CenterOnScreen();
}

void
ProviderEditWindow::MessageReceived(BMessage* msg)
{
    switch (msg->what) {
        case MSG_PROVIDER_DIALOG_DONE:
            _Done();
            break;
        case B_QUIT_REQUESTED:
            Quit();
            break;
        default:
            BWindow::MessageReceived(msg);
    }
}

void
ProviderEditWindow::_Done()
{
    BTextControl* id_field  = dynamic_cast<BTextControl*>(FindView("id"));
    BTextControl* key_field = dynamic_cast<BTextControl*>(FindView("api_key"));
    BTextControl* url_field = dynamic_cast<BTextControl*>(FindView("base_url"));
    BRadioButton* ant_radio = dynamic_cast<BRadioButton*>(FindView("type_anthropic"));

    std::string id = id_field ? id_field->Text() : "";
    if (id.empty()) {
        // No id entered; just keep the dialog open.
        return;
    }

    BMessage done(MSG_PROVIDER_DIALOG_DONE);
    done.AddString("id", id.c_str());
    done.AddString("api_key", key_field ? key_field->Text() : "");
    done.AddString("base_url", url_field ? url_field->Text() : "");
    done.AddString("type", (ant_radio && ant_radio->Value() == B_CONTROL_ON)
                            ? "anthropic" : "openai");
    done.AddBool("editing", editing_);
    target_.SendMessage(&done);
    Quit();
}

// ---------------------------------------------------------------------------
// SettingsWindow
// ---------------------------------------------------------------------------

static const uint32 MSG_SAVE   = 'SAVs';
static const uint32 MSG_CANCEL = 'CANs';

SettingsWindow::SettingsWindow(const std::map<std::string, haicode::ProviderConfig>& providers,
                               BMessenger target)
    : BWindow(BRect(0, 0, 520, 360),
              "Preferences",
              B_TITLED_WINDOW,
              B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
    , providers_(providers)
    , target_(target)
{
    list_ = new BListView("providers_list", B_SINGLE_SELECTION_LIST);
    list_->SetSelectionMessage(new BMessage(MSG_LIST_SEL));
    list_->SetExplicitMinSize(BSize(480, 140));
    list_->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 180));
    auto* scroll = new BScrollView("providers_scroll", list_,
                                   0, false, true, B_FANCY_BORDER);

    empty_hint_ = new BStringView("hint",
        "No providers configured. Click Add to create one.");

    auto* add_btn    = new BButton("add",    "Add",    new BMessage(MSG_PROVIDER_ADD));
    auto* edit_btn   = new BButton("edit",   "Edit",   new BMessage(MSG_PROVIDER_EDIT));
    auto* remove_btn = new BButton("remove", "Remove", new BMessage(MSG_PROVIDER_REMOVE));
    edit_btn->SetEnabled(false);
    remove_btn->SetEnabled(false);

    auto* save_btn   = new BButton("save",   "Save",   new BMessage(MSG_SAVE));
    auto* cancel_btn = new BButton("cancel", "Cancel", new BMessage(MSG_CANCEL));
    save_btn->MakeDefault(true);

    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(new BStringView("title", "Providers"))
        .Add(scroll)
        .Add(empty_hint_)
        .AddGroup(B_HORIZONTAL)
            .Add(add_btn)
            .Add(edit_btn)
            .Add(remove_btn)
            .AddGlue()
        .End()
        .Add(new BSeparatorView(B_HORIZONTAL))
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(cancel_btn)
            .Add(save_btn)
        .End()
    .End();

    CenterOnScreen();
    _RepopulateList();
}

std::string
SettingsWindow::_SummaryFor(const haicode::ProviderConfig& p) const
{
    // "id  [type]  base_url-or-(default)"
    char buf[512];
    std::string url = p.base_url.empty() ? "(default endpoint)" : p.base_url;
    snprintf(buf, sizeof(buf), "%s  [%s]  %s", p.id.c_str(), p.type.c_str(), url.c_str());
    return buf;
}

void
SettingsWindow::_RepopulateList()
{
    list_->MakeEmpty();
    for (auto& [id, p] : providers_) {
        list_->AddItem(new BStringItem(_SummaryFor(p).c_str()));
    }
    bool empty = providers_.empty();
    if (empty) empty_hint_->Show(); else empty_hint_->Hide();
    if (auto* edit = dynamic_cast<BButton*>(FindView("edit")))
        edit->SetEnabled(false);
    if (auto* rm = dynamic_cast<BButton*>(FindView("remove")))
        rm->SetEnabled(false);
}

void
SettingsWindow::_OpenEditor(const std::string& editing_id)
{
    std::string type, key, url;
    if (!editing_id.empty()) {
        auto it = providers_.find(editing_id);
        if (it == providers_.end()) return;
        type = it->second.type;
        key  = it->second.api_key;
        url  = it->second.base_url;
    }
    auto* w = new ProviderEditWindow(BMessenger(this), editing_id, type, key, url);
    w->Show();
}

void
SettingsWindow::_ApplyDialogResult(BMessage* msg)
{
    const char* id_s = nullptr, *key_s = nullptr, *url_s = nullptr, *type_s = nullptr;
    msg->FindString("id",       &id_s);
    msg->FindString("api_key",  &key_s);
    msg->FindString("base_url", &url_s);
    msg->FindString("type",     &type_s);
    if (!id_s || !*id_s) return;

    haicode::ProviderConfig p;
    p.id      = id_s;
    p.api_key = key_s ? key_s : "";
    p.base_url= url_s ? url_s : "";
    p.type    = (type_s && *type_s) ? type_s
                                    : (p.id == "anthropic" ? "anthropic" : "openai");
    providers_[p.id] = std::move(p);
    _RepopulateList();
}

void
SettingsWindow::MessageReceived(BMessage* msg)
{
    switch (msg->what) {
        case MSG_PROVIDER_ADD:
            _OpenEditor("");
            break;
        case MSG_PROVIDER_EDIT: {
            int32 sel = list_->CurrentSelection();
            if (sel < 0) break;
            auto it = providers_.begin();
            std::advance(it, sel);
            _OpenEditor(it->first);
            break;
        }
        case MSG_PROVIDER_REMOVE: {
            int32 sel = list_->CurrentSelection();
            if (sel < 0) break;
            auto it = providers_.begin();
            std::advance(it, sel);
            providers_.erase(it);
            _RepopulateList();
            break;
        }
        case MSG_PROVIDER_DIALOG_DONE:
            _ApplyDialogResult(msg);
            break;
        case MSG_SAVE:
            _Save();
            break;
        case MSG_CANCEL:
            Quit();
            break;
        default: {
            // Selection-changed notifications land here.
            if (msg->what == MSG_LIST_SEL) {
                bool has_sel = list_->CurrentSelection() >= 0;
                if (auto* edit = dynamic_cast<BButton*>(FindView("edit")))
                    edit->SetEnabled(has_sel);
                if (auto* rm = dynamic_cast<BButton*>(FindView("remove")))
                    rm->SetEnabled(has_sel);
            } else {
                BWindow::MessageReceived(msg);
            }
        }
    }
}

void
SettingsWindow::_Save()
{
    // Serialize providers_ as a JSON object { "id": {type,key,url}, ... }.
    nlohmann::json j;
    for (auto& [id, p] : providers_) {
        j[id] = {
            {"type",     p.type},
            {"api_key",  p.api_key},
            {"base_url", p.base_url},
        };
    }
    BMessage saved(MSG_SETTINGS_SAVED);
    saved.AddString("providers", j.dump().c_str());
    target_.SendMessage(&saved);
    Quit();
}
