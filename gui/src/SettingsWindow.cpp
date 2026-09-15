#include "SettingsWindow.h"
#include "Messages.h"

#include <Application.h>
#include <Button.h>
#include <CheckBox.h>
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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>

#include <haicode/model_info.h>
#include <haicode/skills.h>

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

    // Provider type selector: a dropdown covering all supported server kinds.
    // Unknown/legacy types fall back to "openai" (the generic OpenAI-compatible
    // path); the recognized new types map to flavored providers with context
    // discovery.
    auto* type_menu = new BPopUpMenu("type_menu");
    struct TypeEntry { const char* label; const char* value; };
    static const TypeEntry kTypes[] = {
        {"Anthropic",          "anthropic"},
        {"OpenAI-compatible",  "openai"},
        {"Ollama",             "ollama"},
        {"vLLM",               "vllm"},
        {"OpenRouter",         "openrouter"},
        {"LM Studio",          "lmstudio"},
        {"llama.cpp",          "llamacpp"},
    };
    std::string current_type = type.empty() ? "openai" : type;
    BMenuItem* mark_item = nullptr;
    for (auto& t : kTypes) {
        auto* item = new BMenuItem(t.label, nullptr);
        type_menu->AddItem(item);
        if (current_type == t.value)
            mark_item = item;
    }
    if (!mark_item)
        mark_item = type_menu->ItemAt(1);  // default: OpenAI-compatible
    mark_item->SetMarked(true);
    auto* type_field = new BMenuField("type_field", "Type:", type_menu);

    float label_w = 130.0f;
    id_field->SetDivider(label_w);
    key_field->SetDivider(label_w);
    url_field->SetDivider(label_w);
    type_field->SetDivider(label_w);

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
        .Add(type_field)
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
    BMenuField* type_field  = dynamic_cast<BMenuField*>(FindView("type_field"));

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

    // Read the selected type value from the menu's marked item label→value map.
    std::string selected_type = "openai";
    if (type_field && type_field->Menu()) {
        BMenuItem* marked = type_field->Menu()->FindMarked();
        if (marked) {
            std::string label = marked->Label();
            static const std::map<std::string, std::string> kLabelToType = {
                {"Anthropic",          "anthropic"},
                {"OpenAI-compatible",  "openai"},
                {"Ollama",             "ollama"},
                {"vLLM",               "vllm"},
                {"OpenRouter",         "openrouter"},
                {"LM Studio",          "lmstudio"},
                {"llama.cpp",          "llamacpp"},
            };
            auto it = kLabelToType.find(label);
            if (it != kLabelToType.end())
                selected_type = it->second;
        }
    }

    BMessage done(MSG_PROVIDER_DIALOG_DONE);
    done.AddString("id", id.c_str());
    done.AddString("api_key", key.c_str());
    done.AddString("base_url", url_field ? url_field->Text() : "");
    done.AddString("type", selected_type.c_str());
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
                               BMessenger target,
                               const std::string& project_dir)
    : BWindow(BRect(0, 0, 560, 400),
              "Preferences",
              B_TITLED_WINDOW,
              B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
    , config_(config)
    , target_(target)
    , project_dir_(project_dir)
{
    // ---- Providers tab ----
    list_ = new BListView("providers_list", B_SINGLE_SELECTION_LIST);
    list_->SetSelectionMessage(new BMessage(MSG_LIST_SEL));
    list_->SetExplicitMinSize(BSize(480, 140));
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

    // Vision override for the selected model: Auto (table + fail-closed),
    // Yes/No (explicit override stored in config "vision"). Initial marking
    // follows config_.model_vision for config_.model; _RefreshVisionMenu()
    // re-syncs when the model dropdown selection changes.
    vision_menu_ = new BPopUpMenu("vision_menu");
    vision_menu_->SetRadioMode(true);
    vision_menu_->SetLabelFromMarked(true);
    struct VisionEntry { const char* label; const char* value; };
    static const VisionEntry kVisionChoices[] = {
        {"Auto (detect)", "auto"},
        {"Yes",           "yes"},
        {"No",            "no"},
    };
    std::string initial_vision = "auto";
    auto vit = config_.model_vision.find(config_.model);
    if (vit != config_.model_vision.end()) initial_vision = vit->second ? "yes" : "no";
    BMenuItem* vision_marked = nullptr;
    for (auto& e : kVisionChoices) {
        auto* item = new BMenuItem(e.label, new BMessage());
        item->Message()->AddString("vision", e.value);
        vision_menu_->AddItem(item);
        if (std::string(e.value) == initial_vision) vision_marked = item;
    }
    if (!vision_marked) vision_marked = vision_menu_->ItemAt(0);
    vision_marked->SetMarked(true);
    vision_field_ = new BMenuField("vision_field", "Vision:", vision_menu_);

    // Vision fallback pair: a vision-capable (provider, model) that describes
    // images for text-only primaries. Leading "(none)" item = feature off.
    fb_provider_menu_ = new BPopUpMenu("fb_provider");
    fb_provider_menu_->SetRadioMode(true);
    fb_provider_menu_->SetLabelFromMarked(true);
    {
        auto* none_item = new BMenuItem("(none)", new BMessage(MSG_FB_PROVIDER_SET));
        none_item->Message()->AddString("provider_id", "");
        fb_provider_menu_->AddItem(none_item);
        bool fb_found = config_.vision_fallback_provider.empty();
        if (fb_found) none_item->SetMarked(true);
        for (auto& [id, p] : config_.providers) {
            auto* m = new BMessage(MSG_FB_PROVIDER_SET);
            m->AddString("provider_id", id.c_str());
            auto* item = new BMenuItem(id.c_str(), m);
            fb_provider_menu_->AddItem(item);
            if (id == config_.vision_fallback_provider) {
                item->SetMarked(true);
                fb_found = true;
            }
        }
    }
    fb_provider_field_ = new BMenuField("fb_provider_field",
                                        "Vision fallback provider:",
                                        fb_provider_menu_);

    fb_model_menu_ = new BPopUpMenu("fb_model");
    fb_model_menu_->SetRadioMode(true);
    fb_model_menu_->SetLabelFromMarked(true);
    if (!config_.vision_fallback_model.empty()) {
        // Pre-mark the configured model; the fetch replaces placeholder items
        // but preserves the marked label (same as the primary model menu).
        auto* item = new BMenuItem(config_.vision_fallback_model.c_str(), nullptr);
        item->SetMarked(true);
        fb_model_menu_->AddItem(item);
    } else {
        auto* off = new BMenuItem("(off)", nullptr);
        off->SetEnabled(false);
        off->SetMarked(true);
        fb_model_menu_->AddItem(off);
    }
    fb_model_field_ = new BMenuField("fb_model_field",
                                     "Vision fallback model:", fb_model_menu_);

    auto* general_tab = new BGroupView(B_VERTICAL, B_USE_DEFAULT_SPACING);
    BLayoutBuilder::Group<>(general_tab)
        .SetInsets(B_USE_DEFAULT_SPACING)
        .Add(provider_field_)
        .Add(model_field_)
        .Add(context_field_)
        .Add(new BStringView("ctx_hint",
            "(context window in tokens; sets/overrides the limit for this model)"
            "\nLeave blank to use the built-in default for known models."))
        .Add(vision_field_)
        .Add(new BStringView("vision_hint",
            "(enables the screenshot tool for image-capable models"
            " not in the built-in table)"))
        .Add(fb_provider_field_)
        .Add(fb_model_field_)
        .Add(new BStringView("fb_hint",
            "(when the default model is text-only, this vision-capable pair"
            " describes images for it; (none) disables)"))
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

    bool ws_ddg_lite = (config_.web_search_engine != "ddg_html"
                        && config_.web_search_engine != "exa"
                        && config_.web_search_engine != "zai");

    // Engine selector: dropdown instead of a radio group. Each item carries
    // the engine id in its message; save reads it from the marked item's
    // message, never the label. Engines that need an API key reveal a masked
    // key field when selected.
    ws_engine_menu_ = new BPopUpMenu("ws_engine_menu");
    ws_engine_menu_->SetRadioMode(true);
    ws_engine_menu_->SetLabelFromMarked(true);
    struct WSEntry { const char* label; const char* value; };
    static const WSEntry kWSEngines[] = {
        {"DuckDuckGo Lite", "ddg_lite"},
        {"DuckDuckGo HTML", "ddg_html"},
        {"Exa",             "exa"},
        {"Z.AI",            "zai"},
    };
    BMenuItem* ws_marked = nullptr;
    for (auto& e : kWSEngines) {
        auto* item = new BMenuItem(e.label,
                                   new BMessage(MSG_WS_ENGINE_SELECTED));
        item->Message()->AddString("engine", e.value);
        ws_engine_menu_->AddItem(item);
        if ((ws_ddg_lite && std::string(e.value) == "ddg_lite")
            || std::string(e.value) == config_.web_search_engine)
            ws_marked = item;
    }
    if (!ws_marked) ws_marked = ws_engine_menu_->ItemAt(0);
    ws_marked->SetMarked(true);
    ws_engine_field_ = new BMenuField("ws_engine_field", "Search engine:",
                                      ws_engine_menu_);

    // Key field starts empty and masked; hidden entirely for keyless engines.
    // Same keep-if-empty save semantics as the provider editor, with the
    // stored key swapped per engine in _RememberKeyForEngine().
    ws_key_field_ = new BTextControl("ws_key", "API key:", "", nullptr);
    ws_key_field_->TextView()->HideTyping(true);
    ws_key_field_->SetDivider(130.0f);

    // Same hint as the provider editor: tells the user a key is stored even
    // though the masked field looks empty. Toggled in
    // _UpdateKeyFieldVisibility().
    ws_key_hint_ = new BStringView("ws_key_hint",
        "(key already set — leave blank to keep, or enter a new one)");

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
        .Add(ws_engine_field_)
        .Add(ws_key_field_)
        .Add(ws_key_hint_)
        .Add(ws_max_field_)
        .AddGlue();

    // Initialize per-engine stored key + visibility for the marked engine.
    ws_current_engine_ = _MarkedWSEngine();
    if (ws_current_engine_.empty()) ws_current_engine_ = "ddg_lite";
    _RememberKeyForEngine(ws_current_engine_.c_str());
    _UpdateKeyFieldVisibility();

    // ---- Skills tab ----
    // Default-enabled skills for new sessions. Same discovery as the
    // engine's (list_skills: global + project dirs, project shadows).
    {
        auto* skills_tab = new BGroupView(B_VERTICAL, B_USE_DEFAULT_SPACING);
        BLayoutBuilder::Group<>(skills_tab)
            .SetInsets(B_USE_DEFAULT_SPACING);
        auto skills = haicode::list_skills(project_dir_);
        for (auto& sk : skills) {
            auto* chk = new BCheckBox(("skill_" + sk.id).c_str(),
                                      sk.name.c_str(), nullptr);
            if (!sk.description.empty()) chk->SetToolTip(sk.description.c_str());
            chk->SetValue(std::find(config_.default_skills.begin(),
                                    config_.default_skills.end(), sk.id)
                          != config_.default_skills.end()
                              ? B_CONTROL_ON : B_CONTROL_OFF);
            chk->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
            skills_tab->AddChild(chk);
            skill_checks_.emplace_back(sk.id, chk);
        }
        skills_tab->AddChild(new BStringView("skills_note",
            "(checked skills are enabled by default in new sessions)"));
        skills_tab_ = skills_tab;
    }

    // ---- Tabs ----
    auto* tab_view = new BTabView("prefs_tabs", B_WIDTH_FROM_WIDEST);
    tab_view->AddTab(providers_tab, new BTab());
    tab_view->AddTab(general_tab,   new BTab());
    tab_view->AddTab(tools_tab,     new BTab());
    tab_view->AddTab(skills_tab_,   new BTab());
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
    _FetchFBModelsForMarkedProvider();
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
SettingsWindow::_RebuildProviderMenus()
{
    // Rebuild both provider dropdowns from config_.providers after an add or
    // remove, preserving each menu's marked id when it still exists. Without
    // this the menus (built once in the constructor) go stale: a new provider
    // can't be selected as default, and removing the marked one leaves a ghost
    // entry that Save would write back into config_.provider.

    std::string keep_primary = _MarkedProviderId();
    std::string keep_fb      = _MarkedFBProviderId();

    while (provider_menu_->CountItems() > 0)
        delete provider_menu_->RemoveItem((int32)0);
    bool found_provider = false;
    for (auto& [id, p] : config_.providers) {
        auto* msg = new BMessage(MSG_SET_PROVIDER);
        msg->AddString("provider_id", id.c_str());
        auto* item = new BMenuItem(id.c_str(), msg);
        provider_menu_->AddItem(item);
        if (id == keep_primary) {
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

    while (fb_provider_menu_->CountItems() > 0)
        delete fb_provider_menu_->RemoveItem((int32)0);
    {
        auto* none_item = new BMenuItem("(none)", new BMessage(MSG_FB_PROVIDER_SET));
        none_item->Message()->AddString("provider_id", "");
        fb_provider_menu_->AddItem(none_item);
        if (keep_fb.empty())
            none_item->SetMarked(true);
        for (auto& [id, p] : config_.providers) {
            auto* m = new BMessage(MSG_FB_PROVIDER_SET);
            m->AddString("provider_id", id.c_str());
            auto* item = new BMenuItem(id.c_str(), m);
            fb_provider_menu_->AddItem(item);
            if (id == keep_fb)
                item->SetMarked(true);
        }
    }
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

    // Keep the General-tab provider dropdowns in sync with add/edit. If the
    // mark moved (e.g. first provider added while "(none configured)" was
    // marked), refetch so the model list matches the new default.
    std::string prev_primary = _MarkedProviderId();
    std::string prev_fb      = _MarkedFBProviderId();
    _RebuildProviderMenus();
    if (_MarkedProviderId() != prev_primary)
        _FetchModelsForMarkedProvider();
    if (_MarkedFBProviderId() != prev_fb)
        _FetchFBModelsForMarkedProvider();
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

            // Refresh the General-tab dropdowns: removing the marked default
            // moves the mark to another provider (or "(none configured)"), so
            // refetch when it did.
            std::string prev_primary = _MarkedProviderId();
            std::string prev_fb      = _MarkedFBProviderId();
            _RebuildProviderMenus();
            if (_MarkedProviderId() != prev_primary)
                _FetchModelsForMarkedProvider();
            if (_MarkedFBProviderId() != prev_fb)
                _FetchFBModelsForMarkedProvider();
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
            _RefreshVisionMenu();
            break;
        case MSG_FB_PROVIDER_SET: {
            // Fallback provider dropdown changed — refetch its model list.
            while (fb_model_menu_->CountItems() > 0)
                delete fb_model_menu_->RemoveItem((int32)0);
            if (_MarkedFBProviderId().empty()) {
                // "(none)" disables the feature outright: no fetch, no
                // loading state — the model menu resolves immediately to
                // "(off)" (a stale in-flight reply is dropped by the
                // provider-id guard in MSG_FB_MODELS_LOADED).
                auto* off = new BMenuItem("(off)", nullptr);
                off->SetEnabled(false);
                off->SetMarked(true);
                fb_model_menu_->AddItem(off);
                break;
            }
            auto* loading = new BMenuItem("(loading\xe2\x80\xa6)", nullptr);
            loading->SetEnabled(false);
            loading->SetMarked(true);
            fb_model_menu_->AddItem(loading);
            _FetchFBModelsForMarkedProvider();
            break;
        }
        case MSG_FB_MODELS_LOADED: {
            // Same stale-reply guard as the primary pair; also discards late
            // replies after switching back to "(none)" (empty id never matches).
            const char* fb_loaded_pid = nullptr;
            if (msg->FindString("provider_id", &fb_loaded_pid) == B_OK
                    && fb_loaded_pid && std::string(fb_loaded_pid) != _MarkedFBProviderId())
                break;

            std::string preserved = config_.vision_fallback_model;
            while (fb_model_menu_->CountItems() > 0)
                delete fb_model_menu_->RemoveItem((int32)0);
            const char* m = nullptr;
            int32 idx = 0;
            bool any = false;
            BMenuItem* to_mark = nullptr;
            while (msg->FindString("model", idx++, &m) == B_OK) {
                if (m && *m) {
                    fb_model_menu_->AddItem(new BMenuItem(m, nullptr));
                    any = true;
                }
            }
            if (any) {
                if (!preserved.empty())
                    to_mark = fb_model_menu_->FindItem(preserved.c_str());
                if (!to_mark) to_mark = fb_model_menu_->ItemAt(0);
            } else {
                std::string label = "(none available)";
                const char* err = nullptr;
                if (msg->FindString("error", &err) == B_OK && err && *err)
                    label = std::string("(fetch failed: ") + err + ")";
                to_mark = new BMenuItem(label.c_str(), nullptr);
                to_mark->SetEnabled(false);
                fb_model_menu_->AddItem(to_mark);
            }
            if (to_mark) to_mark->SetMarked(true);
            fb_model_menu_->SetLabelFromMarked(true);
            break;
        }
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
            // Discard replies for a provider that is no longer marked — fetches
            // run on detached threads and can land out of order after the user
            // switched providers (same guard MainWindow applies).
            const char* loaded_pid = nullptr;
            if (msg->FindString("provider_id", &loaded_pid) == B_OK
                    && loaded_pid && std::string(loaded_pid) != _MarkedProviderId())
                break;

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
            _RefreshVisionMenu();
            break;
        }
        case MSG_WS_ENGINE_SELECTED: {
            // The menu marks the new item before invoking this, so the engine
            // whose field content is on display is ws_current_engine_.
            std::string next = _MarkedWSEngine();
            if (next.empty() || next == ws_current_engine_) break;
            if (ws_key_field_) {
                const char* text = ws_key_field_->Text();
                if (text && *text)
                    ws_typed_keys_[ws_current_engine_] = text;
                else
                    ws_typed_keys_.erase(ws_current_engine_);  // blank = keep stored
                // Swap in the new engine's in-progress text (or blank).
                auto it = ws_typed_keys_.find(next);
                ws_key_field_->SetText(it != ws_typed_keys_.end()
                                           ? it->second.c_str() : "");
            }
            ws_current_engine_ = next;
            _RememberKeyForEngine(ws_current_engine_.c_str());
            _UpdateKeyFieldVisibility();
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
    // Selected engine comes from the dropdown's marked item's message.
    saved.AddString("web_search_engine", _MarkedWSEngine().c_str());
    // Key field follows provider-editor semantics: empty means keep the key
    // already stored for the displayed engine. A typed key overrides it.
    const char* ws_key_text = ws_key_field_ ? ws_key_field_->Text() : "";
    if (*ws_key_text) {
        saved.AddString("web_search_key", ws_key_text);
        saved.AddString("web_search_key_engine", ws_current_engine_.c_str());
    } else if (ws_current_engine_ == "exa" || ws_current_engine_ == "zai") {
        // Blank field on a key-engine: apply any in-progress stash, then
        // signal "keep existing" by sending the stored key's engine with an
        // empty key (HaiCodeApp keeps the stored value for that engine).
        saved.AddString("web_search_key", "");
        saved.AddString("web_search_key_engine", ws_current_engine_.c_str());
    }
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

    // Vision override for the selected model. Unlike the context pair, always
    // sent when a real model is selected — "auto" explicitly erases a stored
    // override. Placeholder labels like "(loading…)" are skipped.
    if (!model_sel.empty() && model_sel[0] != '(') {
        saved.AddString("vision_override", _MarkedVision().c_str());
        saved.AddString("vision_model", model_sel.c_str());
    }

    // Vision fallback pair. "(none)" provider or a placeholder/unset model
    // label means the feature is off (sent as empty strings).
    std::string fb_provider = _MarkedFBProviderId();
    std::string fb_model;
    if (auto* mk = fb_model_menu_->FindMarked()) {
        std::string lbl = mk->Label();
        if (!lbl.empty() && lbl[0] != '(') fb_model = lbl;
    }
    saved.AddString("vision_fallback_provider", fb_provider.c_str());
    saved.AddString("vision_fallback_model", fb_model.c_str());

    // Default-enabled skills (Skills tab): repeated "default_skill" ids.
    for (auto& [id, chk] : skill_checks_)
        if (chk && chk->Value() == B_CONTROL_ON)
            saved.AddString("default_skill", id.c_str());

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
SettingsWindow::_RefreshVisionMenu()
{
    if (!vision_menu_) return;
    std::string model;
    if (auto* marked = model_menu_->FindMarked()) {
        std::string label = marked->Label();
        // Skip placeholder labels like "(loading…)" / "(none available)".
        if (!label.empty() && label[0] != '(') model = label;
    }

    std::string want = "auto";
    auto vit = config_.model_vision.find(model);
    if (vit != config_.model_vision.end()) want = vit->second ? "yes" : "no";

    for (int32 i = 0; i < vision_menu_->CountItems(); ++i) {
        BMenuItem* item = vision_menu_->ItemAt(i);
        const char* value = nullptr;
        if (item->Message()
            && item->Message()->FindString("vision", &value) == B_OK
            && value && std::string(value) == want) {
            item->SetMarked(true);
            return;
        }
    }
}

std::string
SettingsWindow::_MarkedVision() const
{
    if (auto* marked = vision_menu_->FindMarked()) {
        const char* vision = nullptr;
        if (marked->Message()
            && marked->Message()->FindString("vision", &vision) == B_OK
            && vision)
            return vision;
    }
    return "auto";
}

std::string
SettingsWindow::_MarkedWSEngine() const
{
    if (auto* marked = ws_engine_menu_->FindMarked()) {
        const char* engine = nullptr;
        if (marked->Message()
            && marked->Message()->FindString("engine", &engine) == B_OK
            && engine)
            return engine;
    }
    return "";
}

void
SettingsWindow::_UpdateKeyFieldVisibility()
{
    if (!ws_key_field_) return;
    const std::string& engine = ws_current_engine_;
    bool needs_key = (engine == "exa" || engine == "zai");
    // Provider-editor pattern: hint appears under the field only when a key
    // is already stored for the displayed engine.
    bool show_hint = (needs_key && !ws_existing_key_.empty());

    // Apply Show()/Hide() only on state changes so the hide count stays
    // balanced, and invalidate the layout once if anything flipped. This
    // must not early-return when the field stays visible (exa→zai switch):
    // the hint can still change.
    bool changed = false;
    if (needs_key == ws_key_field_->IsHidden()) {
        if (needs_key) ws_key_field_->Show();
        else           ws_key_field_->Hide();
        changed = true;
    }
    if (ws_key_hint_ && show_hint == ws_key_hint_->IsHidden()) {
        if (show_hint) ws_key_hint_->Show();
        else           ws_key_hint_->Hide();
        changed = true;
    }
    if (changed) {
        if (BView* parent = ws_key_field_->Parent())
            parent->InvalidateLayout();
    }
}

void
SettingsWindow::_RememberKeyForEngine(const char* engine_id)
{
    if (!engine_id) return;
    // Capture the key stored in config for this engine so _Save() can apply
    // "empty field = keep existing" against the right engine.
    auto it = config_.web_search_api_keys.find(engine_id);
    ws_existing_key_ = (it != config_.web_search_api_keys.end()) ? it->second : "";
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

void
SettingsWindow::_FetchFBModelsForMarkedProvider()
{
    std::string pid = _MarkedFBProviderId();
    if (pid.empty()) return;
    BMessage fetch(MSG_FETCH_MODELS);
    fetch.AddString("provider_id", pid.c_str());
    fetch.AddMessenger("reply", BMessenger(this));
    // Distinct reply constant so the fallback list doesn't clobber the
    // primary model dropdown (app handler echoes this back as `what`).
    fetch.AddInt32("reply_what", MSG_FB_MODELS_LOADED);
    be_app->PostMessage(&fetch);
}

std::string
SettingsWindow::_MarkedFBProviderId() const
{
    if (auto* marked = fb_provider_menu_->FindMarked()) {
        const char* pid = nullptr;
        if (marked->Message()
            && marked->Message()->FindString("provider_id", &pid) == B_OK
            && pid)
            return pid;
    }
    return "";
}
