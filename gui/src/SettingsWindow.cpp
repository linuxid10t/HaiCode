#include "SettingsWindow.h"
#include "Messages.h"

#include <Application.h>
#include <Button.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <RadioButton.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <StringItem.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>

#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>

#include <haicode/model_info.h>

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
    , existing_key_(api_key)
{
    auto* id_field = new BTextControl("id", "Provider id:", editing_id.c_str(), nullptr);
    if (editing_) id_field->SetEnabled(false);

    // Key field starts empty. When editing a provider that already has a key,
    // show a hint so the user knows an empty field means "keep existing".
    // Mask typing so the key isn't shown in cleartext.
    auto* key_field = new BTextControl("api_key", "API key:", "", nullptr);
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

    BStringView* key_hint = nullptr;
    if (editing_ && !existing_key_.empty()) {
        key_hint = new BStringView("key_hint",
            "(key already set — leave blank to keep, or enter a new one)");
    }

    auto* ok_btn = new BButton("ok", "OK", new BMessage(MSG_PROVIDER_DIALOG_DONE));
    ok_btn->MakeDefault(true);
    auto* cancel_btn = new BButton("cancel", "Cancel",
                                   new BMessage(B_QUIT_REQUESTED));

    auto layout = BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(id_field)
        .Add(key_field);
    if (key_hint)
        layout.Add(key_hint);
    layout.Add(url_field)
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

    // When editing, an empty key field means "keep the existing key" rather
    // than "blank it out". This is the safety fix for the disappearing-key bug.
    std::string key = key_field ? key_field->Text() : "";
    if (editing_ && key.empty())
        key = existing_key_;

    BMessage done(MSG_PROVIDER_DIALOG_DONE);
    done.AddString("id", id.c_str());
    done.AddString("api_key", key.c_str());
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
static const uint32 MSG_MODEL_CHANGED = 'MODc';  // model dropdown selection changed

SettingsWindow::SettingsWindow(const haicode::AppConfig& config,
                               BMessenger target)
    : BWindow(BRect(0, 0, 560, 400),
              "Preferences",
              B_TITLED_WINDOW,
              B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
    , config_(config)
    , target_(target)
{
    // ---- Providers tab ----
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

    auto* providers_tab = new BGroupView(B_VERTICAL, B_USE_DEFAULT_SPACING);
    BLayoutBuilder::Group<>(providers_tab)
        .SetInsets(B_USE_DEFAULT_SPACING)
        .Add(scroll)
        .Add(empty_hint_)
        .AddGroup(B_HORIZONTAL)
            .Add(add_btn)
            .Add(edit_btn)
            .Add(remove_btn)
            .AddGlue()
        .End();

    // ---- General tab ----
    float label_w = 120.0f;

    // Default provider dropdown — one item per configured provider.
    provider_menu_ = new BPopUpMenu("provider");
    provider_menu_->SetRadioMode(true);
    provider_menu_->SetLabelFromMarked(true);
    bool found_provider = false;
    for (auto& [id, p] : config_.providers) {
        auto* msg = new BMessage(MSG_SET_PROVIDER);
        msg->AddString("provider_id", id.c_str());
        auto* item = new BMenuItem(id.c_str(), msg);
        provider_menu_->AddItem(item);
        if (id == config_.provider) {
            item->SetMarked(true);
            found_provider = true;
        }
    }
    if (!found_provider && provider_menu_->CountItems() > 0)
        provider_menu_->ItemAt(0)->SetMarked(true);
    if (provider_menu_->CountItems() == 0) {
        auto* item = new BMenuItem("(none configured)", nullptr);
        item->SetEnabled(false);
        item->SetMarked(true);
        provider_menu_->AddItem(item);
    }
    provider_field_ = new BMenuField("provider_field", "Default provider:",
                                     provider_menu_);

    // Default model dropdown — populated after a fetch (per selected provider).
    model_menu_ = new BPopUpMenu("model");
    model_menu_->SetRadioMode(true);
    model_menu_->SetLabelFromMarked(true);
    {
        auto* loading = new BMenuItem("(loading\xe2\x80\xa6)", nullptr);
        loading->SetEnabled(false);
        loading->SetMarked(true);
        model_menu_->AddItem(loading);
    }
    model_field_ = new BMenuField("model_field", "Default model:", model_menu_);

    // Context-window override for the selected model. Pre-filled from
    // config_.model_contexts (exact match on config_.model); updated when the
    // model dropdown selection changes. Empty means "use built-in default".
    context_field_ = new BTextControl("ctx", "Context size (tokens):",
                                      "", nullptr);
    context_field_->SetDivider(label_w);
    {
        std::string model_for_ctx = config_.model;
        auto mcit = config_.model_contexts.find(model_for_ctx);
        if (mcit != config_.model_contexts.end() && mcit->second > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", mcit->second);
            context_field_->SetText(buf);
        } else {
            int builtin = haicode::get_context_window(config_.provider,
                                                      model_for_ctx,
                                                      config_.model_contexts);
            if (builtin > 0) {
                char buf[16];
                snprintf(buf, sizeof(buf), "%d", builtin);
                context_field_->SetText(buf);
            }
        }
    }

    bool mode_is_plan = (config_.default_mode != "build");
    mode_plan_radio_  = new BRadioButton("mode_plan",  "Plan mode (default)", nullptr);
    mode_build_radio_ = new BRadioButton("mode_build", "Build mode", nullptr);
    (mode_is_plan ? mode_plan_radio_ : mode_build_radio_)->SetValue(B_CONTROL_ON);

    auto* general_tab = new BGroupView(B_VERTICAL, B_USE_DEFAULT_SPACING);
    BLayoutBuilder::Group<>(general_tab)
        .SetInsets(B_USE_DEFAULT_SPACING)
        .Add(provider_field_)
        .Add(model_field_)
        .Add(context_field_)
        .Add(new BStringView("ctx_hint",
            "(context window in tokens; sets/overrides the limit for this model)"
            "\nLeave blank to use the built-in default for known models."))
        .Add(new BSeparatorView(B_HORIZONTAL))
        .Add(new BStringView("mode_label", "New-session start mode:"))
        .AddGroup(B_VERTICAL)
            .Add(mode_plan_radio_)
            .Add(mode_build_radio_)
        .End()
        .AddGlue();

    // ---- Tools tab ----
    build_cmd_field_ = new BTextControl("build_command", "Build command:",
                                        config_.build_command.c_str(), nullptr);
    build_cmd_field_->SetDivider(label_w);

    bool ws_mojeek  = (config_.web_search_engine != "ddg_lite"
                       && config_.web_search_engine != "ddg_html");
    bool ws_ddglite = (config_.web_search_engine == "ddg_lite");
    ws_mojeek_radio_  = new BRadioButton("ws_mojeek",  "Mojeek",  nullptr);
    ws_ddglite_radio_ = new BRadioButton("ws_ddglite", "DuckDuckGo Lite", nullptr);
    ws_ddghtml_radio_ = new BRadioButton("ws_ddghtml", "DuckDuckGo HTML", nullptr);
    ws_mojeek_radio_->SetValue(ws_mojeek  ? B_CONTROL_ON : B_CONTROL_OFF);
    ws_ddglite_radio_->SetValue(ws_ddglite ? B_CONTROL_ON : B_CONTROL_OFF);
    ws_ddghtml_radio_->SetValue(config_.web_search_engine == "ddg_html"
                                ? B_CONTROL_ON : B_CONTROL_OFF);

    char maxbuf[16];
    snprintf(maxbuf, sizeof(maxbuf), "%d", config_.web_search_max_results);
    ws_max_field_ = new BTextControl("ws_max", "Max results:",
                                     maxbuf, nullptr);
    ws_max_field_->SetDivider(label_w);

    auto* tools_tab = new BGroupView(B_VERTICAL, B_USE_DEFAULT_SPACING);
    BLayoutBuilder::Group<>(tools_tab)
        .SetInsets(B_USE_DEFAULT_SPACING)
        .Add(build_cmd_field_)
        .Add(new BStringView("bc_hint",
            "(run after each successful write/edit; non-zero exit shows errors to the model)"))
        .Add(new BSeparatorView(B_HORIZONTAL))
        .Add(new BStringView("ws_label", "Web search engine:"))
        .AddGroup(B_VERTICAL)
            .Add(ws_mojeek_radio_)
            .Add(ws_ddglite_radio_)
            .Add(ws_ddghtml_radio_)
        .End()
        .Add(ws_max_field_)
        .AddGlue();

    // ---- Tabs ----
    auto* tab_view = new BTabView("prefs_tabs", B_WIDTH_FROM_WIDEST);
    tab_view->AddTab(providers_tab, new BTab());
    tab_view->AddTab(general_tab,   new BTab());
    tab_view->AddTab(tools_tab,     new BTab());
    tab_view->TabAt(0)->SetLabel("Providers");
    tab_view->TabAt(1)->SetLabel("General");
    tab_view->TabAt(2)->SetLabel("Tools");

    auto* save_btn   = new BButton("save",   "Save",   new BMessage(MSG_SAVE));
    auto* cancel_btn = new BButton("cancel", "Cancel", new BMessage(MSG_CANCEL));
    save_btn->MakeDefault(true);

    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(tab_view)
        .Add(new BSeparatorView(B_HORIZONTAL))
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(cancel_btn)
            .Add(save_btn)
        .End()
    .End();

    CenterOnScreen();
    _RepopulateList();

    // Kick off an initial model-list fetch for the currently-marked provider
    // so the Default model dropdown is populated on open.
    _FetchModelsForMarkedProvider();
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
    for (auto& [id, p] : config_.providers) {
        list_->AddItem(new BStringItem(_SummaryFor(p).c_str()));
    }
    bool empty = config_.providers.empty();
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
        auto it = config_.providers.find(editing_id);
        if (it == config_.providers.end()) return;
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
    p.base_url= url_s ? url_s : "";
    p.type    = (type_s && *type_s) ? type_s
                                    : (p.id == "anthropic" ? "anthropic" : "openai");
    // Defensive: an empty incoming key must not overwrite an existing key.
    // (The edit dialog already posts the real key when the field is left blank;
    // this guards against any caller that forgets to.)
    std::string incoming_key = key_s ? key_s : "";
    auto existing = config_.providers.find(p.id);
    if (incoming_key.empty() && existing != config_.providers.end())
        p.api_key = existing->second.api_key;
    else
        p.api_key = incoming_key;
    config_.providers[p.id] = std::move(p);
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
            auto it = config_.providers.begin();
            std::advance(it, sel);
            _OpenEditor(it->first);
            break;
        }
        case MSG_PROVIDER_REMOVE: {
            int32 sel = list_->CurrentSelection();
            if (sel < 0) break;
            auto it = config_.providers.begin();
            std::advance(it, sel);
            config_.providers.erase(it);
            _RepopulateList();
            break;
        }
        case MSG_PROVIDER_DIALOG_DONE:
            _ApplyDialogResult(msg);
            break;
        case MSG_SAVE:
            _Save();
            break;
        case MSG_MODEL_CHANGED:
            // Model dropdown selection changed — sync the context field.
            _RefreshContextField();
            break;
        case MSG_SET_PROVIDER: {
            // Provider dropdown changed — refetch the model list.
            while (model_menu_->CountItems() > 0)
                delete model_menu_->RemoveItem((int32)0);
            auto* loading = new BMenuItem("(loading\xe2\x80\xa6)", nullptr);
            loading->SetEnabled(false);
            loading->SetMarked(true);
            model_menu_->AddItem(loading);
            _FetchModelsForMarkedProvider();
            break;
        }
        case MSG_MODELS_LOADED: {
            // Repopulate model dropdown from the fetched list.
            std::string preserved = config_.model;
            while (model_menu_->CountItems() > 0)
                delete model_menu_->RemoveItem((int32)0);
            const char* m = nullptr;
            for (int32 i = 0; msg->FindString("model", i, &m) == B_OK; ++i)
                model_menu_->AddItem(
                    new BMenuItem(m, new BMessage(MSG_MODEL_CHANGED)));
            BMenuItem* to_mark = nullptr;
            if (model_menu_->CountItems() > 0) {
                if (auto* existing = model_menu_->FindItem(preserved.c_str()))
                    to_mark = existing;
                else
                    to_mark = model_menu_->ItemAt(0);
            } else {
                std::string label = "(none available)";
                const char* err = nullptr;
                if (msg->FindString("error", &err) == B_OK && err && *err)
                    label = std::string("(fetch failed: ") + err + ")";
                to_mark = new BMenuItem(label.c_str(), nullptr);
                to_mark->SetEnabled(false);
                model_menu_->AddItem(to_mark);
            }
            if (to_mark) to_mark->SetMarked(true);
            model_menu_->SetLabelFromMarked(true);
            _RefreshContextField();
            break;
        }
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
    // Serialize providers as a JSON object { "id": {type,key,url}, ... }.
    nlohmann::json j;
    for (auto& [id, p] : config_.providers) {
        j[id] = {
            {"type",     p.type},
            {"api_key",  p.api_key},
            {"base_url", p.base_url},
        };
    }
    BMessage saved(MSG_SETTINGS_SAVED);
    saved.AddString("providers", j.dump().c_str());

    // Scalars from the General/Tools tabs.
    std::string provider_sel, model_sel;
    if (auto* m = provider_menu_->FindMarked()) provider_sel = m->Label();
    if (auto* m = model_menu_->FindMarked())    model_sel   = m->Label();
    saved.AddString("model", model_sel.c_str());
    saved.AddString("provider", provider_sel.c_str());
    const char* mode = "plan";
    if (mode_build_radio_ && mode_build_radio_->Value() == B_CONTROL_ON)
        mode = "build";
    saved.AddString("default_mode", mode);
    saved.AddString("build_command",
                    build_cmd_field_ ? build_cmd_field_->Text() : "");
    const char* ws_engine = "mojeek";
    if (ws_ddglite_radio_ && ws_ddglite_radio_->Value() == B_CONTROL_ON)
        ws_engine = "ddg_lite";
    else if (ws_ddghtml_radio_ && ws_ddghtml_radio_->Value() == B_CONTROL_ON)
        ws_engine = "ddg_html";
    saved.AddString("web_search_engine", ws_engine);
    int32 ws_max = 5;
    if (ws_max_field_ && ws_max_field_->Text()) {
        long n = std::atol(ws_max_field_->Text());
        if (n > 0) ws_max = static_cast<int32>(n);
    }
    saved.AddInt32("web_search_max_results", ws_max);

    // Context-window override for the selected model.
    if (context_field_ && context_field_->Text() && *context_field_->Text()) {
        long ctx = std::atol(context_field_->Text());
        if (ctx > 0 && !model_sel.empty()) {
            saved.AddInt32("context_window", static_cast<int32>(ctx));
            saved.AddString("context_model", model_sel.c_str());
        }
    }

    target_.SendMessage(&saved);
    Quit();
}

std::string
SettingsWindow::_MarkedProviderId() const
{
    if (auto* marked = provider_menu_->FindMarked())
        return marked->Label();
    return "";
}

void
SettingsWindow::_RefreshContextField()
{
    if (!context_field_) return;
    std::string model;
    if (auto* marked = model_menu_->FindMarked()) {
        std::string label = marked->Label();
        // Skip placeholder labels like "(loading…)" / "(none available)".
        if (!label.empty() && label[0] != '(') model = label;
    }

    auto mcit = config_.model_contexts.find(model);
    if (mcit != config_.model_contexts.end() && mcit->second > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", mcit->second);
        context_field_->SetText(buf);
        return;
    }
    int builtin = haicode::get_context_window(_MarkedProviderId(), model,
                                              config_.model_contexts);
    context_field_->SetText(builtin > 0 ? std::to_string(builtin).c_str() : "");
}

void
SettingsWindow::_FetchModelsForMarkedProvider()
{
    std::string pid = _MarkedProviderId();
    if (pid.empty()) return;
    BMessage fetch(MSG_FETCH_MODELS);
    fetch.AddString("provider_id", pid.c_str());
    // Route the reply back to this window instead of MainWindow.
    fetch.AddMessenger("reply", BMessenger(this));
    be_app->PostMessage(&fetch);
}
