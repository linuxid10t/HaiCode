#include "MainWindow.h"
#include "Messages.h"
#include "ChatView.h"
#include "PermissionWindow.h"
#include "PlanReviewWindow.h"

#include <Application.h>
#include <Window.h>
#include <View.h>
#include <TextView.h>
#include <ScrollView.h>
#include <Button.h>
#include <MenuBar.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <ListView.h>
#include <StringItem.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <SplitView.h>
#include <Message.h>
#include <Messenger.h>
#include <String.h>
#include <SupportDefs.h>
#include <GraphicsDefs.h>
#include <FilePanel.h>
#include <Entry.h>
#include <Path.h>
#include <StringView.h>
#include <Alert.h>

#include <haicode/engine.h>
#include <haicode/db.h>
#include <haicode/model_info.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <future>
#include <memory>
#include <ctime>
#include <cstdint>
#include <climits>
#include <cstdio>
#include <algorithm>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// SessionListView — BListView with right-click context menu
// ---------------------------------------------------------------------------

class SessionListView : public BListView {
public:
    SessionListView()
        : BListView("session_list", B_SINGLE_SELECTION_LIST)
    {
        SetSelectionMessage(new BMessage(MSG_SELECT_SESSION));
    }

    void MouseDown(BPoint where) override
    {
        int32 buttons = 0;
        if (Window()->CurrentMessage()->FindInt32("buttons", &buttons) == B_OK
            && (buttons & B_SECONDARY_MOUSE_BUTTON))
        {
            // Right-click: select the item under the cursor first
            int32 idx = IndexOf(where);
            if (idx >= 0) Select(idx);

            BPopUpMenu* menu = new BPopUpMenu("session_ctx", false, false);
            BMessage* del_msg = new BMessage(MSG_DELETE_SESSION);
            del_msg->AddInt32("index", idx >= 0 ? idx : CurrentSelection());
            menu->AddItem(new BMenuItem("Delete Session", del_msg));

            ConvertToScreen(&where);
            menu->Go(where, true, true, true);
            delete menu;
        } else {
            BListView::MouseDown(where);
        }
    }
};

// ---------------------------------------------------------------------------
// InputTextView — BTextView subclass that sends MSG_SUBMIT_PROMPT on Enter
// ---------------------------------------------------------------------------

class InputTextView : public BTextView {
public:
    InputTextView(const char* name)
        : BTextView(name, B_WILL_DRAW | B_PULSE_NEEDED | B_FRAME_EVENTS
                        | B_NAVIGABLE | B_SUPPORTS_LAYOUT)
    {
        SetWordWrap(true);
        SetExplicitMinSize(BSize(B_SIZE_UNSET, 60));
    }

    void AttachedToWindow() override
    {
        BTextView::AttachedToWindow();
        SetViewColor(255, 255, 255);
        SetLowColor(255, 255, 255);
        MakeFocus(true);
    }

    void KeyDown(const char* bytes, int32 numBytes) override
    {
        if (numBytes == 1 && bytes[0] == B_ENTER && !(modifiers() & B_SHIFT_KEY)) {
            Window()->PostMessage(MSG_SUBMIT_PROMPT);
        } else {
            BTextView::KeyDown(bytes, numBytes);
        }
    }
};

// ---------------------------------------------------------------------------
// MainWindow
// ---------------------------------------------------------------------------

static std::string dir_basename(const std::string& path) {
    if (path.empty()) return "/";
    std::string p = path;
    if (p.back() == '/' && p.size() > 1) p.pop_back();
    auto pos = p.rfind('/');
    return (pos == std::string::npos) ? p : p.substr(pos + 1);
}

MainWindow::MainWindow(haicode::SessionEngine& engine,
                       haicode::SessionStore& store,
                       const std::string& project_dir,
                       const std::string& default_model,
                       const std::string& default_provider)
    : BWindow(BRect(100, 100, 1100, 750),
              "HaiCode",
              B_TITLED_WINDOW,
              B_QUIT_ON_WINDOW_CLOSE | B_AUTO_UPDATE_SIZE_LIMITS)
    , engine_(&engine)
    , store_(store)
    , project_dir_(project_dir)
    , default_model_(default_model)
    , default_provider_(default_provider.empty() ? "anthropic" : default_provider)
{
    // ---- Menu bar ----
    menu_bar_ = new BMenuBar("menu_bar");
    BMenu* settings_menu = new BMenu("Settings");
    settings_menu->AddItem(new BMenuItem("Preferences" B_UTF8_ELLIPSIS,
                                         new BMessage(MSG_SHOW_SETTINGS), ','));
    menu_bar_->AddItem(settings_menu);

    // ---- Toolbar: New Session, Dir picker, Model selector, Interrupt ----
    new_session_btn_ = new BButton("new_session", "New Session", new BMessage(MSG_NEW_SESSION));
    interrupt_btn_   = new BButton("interrupt",   "Interrupt",   new BMessage(MSG_INTERRUPT));
    interrupt_btn_->SetEnabled(false);

    std::string dir_label = dir_basename(project_dir_);
    dir_btn_ = new BButton("dir_btn", dir_label.c_str(), new BMessage(MSG_CHOOSE_DIR));

    // Provider selector — user picks endpoint type; model list is fetched dynamically
    provider_menu_ = new BPopUpMenu("Anthropic");
    auto* ap_item = new BMenuItem("Anthropic", new BMessage(MSG_FETCH_MODELS));
    auto* oi_item = new BMenuItem("OpenAI / compatible", new BMessage(MSG_FETCH_MODELS));
    // Mark the restored provider (default_provider_ was seeded from config
    // by HaiCodeApp). Falls back to Anthropic when unset.
    (default_provider_ == "openai" ? oi_item : ap_item)->SetMarked(true);
    provider_menu_->AddItem(ap_item);
    provider_menu_->AddItem(oi_item);
    provider_field_ = new BMenuField("provider_field", "Provider:", provider_menu_);

    // Model list — starts empty; populated after MSG_MODELS_LOADED.
    // Radio mode keeps at most one item marked, so the dropdown's marked item
    // always mirrors default_model_ (the single source of truth for the next
    // session's model).
    model_menu_ = new BPopUpMenu("(loading…)");
    model_menu_->SetRadioMode(true);
    model_menu_->SetLabelFromMarked(true);
    auto* loading_item = new BMenuItem("(loading\xe2\x80\xa6)", nullptr);
    loading_item->SetEnabled(false);
    loading_item->SetMarked(true);
    model_menu_->AddItem(loading_item);
    model_field_ = new BMenuField("model_field", "Model:", model_menu_);

    // Mode toggle — switches between Build (default) and Plan
    mode_btn_ = new BButton("mode", "Mode: Build", new BMessage(MSG_TOGGLE_MODE));

    auto_edits_chk_ = new BCheckBox("auto_edits", "Auto-allow edits",
                                    new BMessage(MSG_AUTO_ALLOW_EDITS));
    yolo_chk_       = new BCheckBox("yolo", "YOLO", new BMessage(MSG_YOLO));

    // ---- Session list (left sidebar) ----
    session_list_ = new SessionListView();
    session_scroll_ = new BScrollView("session_scroll", session_list_,
                                      0, false, true, B_FANCY_BORDER);
    session_scroll_->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));

    // ---- ChatView ----
    chat_view_ = new ChatView("chat_view");

    // ---- Input area ----
    input_view_ = new InputTextView("input_view");
    BScrollView* input_scroll = new BScrollView("input_scroll", input_view_,
                                                0, false, true, B_FANCY_BORDER);
    input_scroll->SetExplicitMinSize(BSize(B_SIZE_UNSET, 70));
    input_scroll->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 100));

    send_btn_ = new BButton("send", "Send \xe2\x96\xb6", new BMessage(MSG_SUBMIT_PROMPT));
    send_btn_->MakeDefault(false);

    // ---- Status strip (engine state, token counts, context size) ----
    status_strip_ = new BStringView("status_strip", "[BUILD] \xe2\x9c\x93 idle");
    status_strip_->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    BFont status_font(*be_plain_font);
    status_font.SetSize(11.0f);
    status_strip_->SetFont(&status_font);

    // ---- Layout ----
    // Toolbar group
    BGroupView* toolbar_group = new BGroupView(B_HORIZONTAL, B_USE_SMALL_SPACING);
    BLayoutBuilder::Group<>(toolbar_group)
        .Add(new_session_btn_)
        .Add(dir_btn_)
        .Add(provider_field_)
        .Add(model_field_)
        .Add(mode_btn_)
        .Add(interrupt_btn_)
        .AddGlue()
    .End();

    // Input group (label + text + send button)
    BGroupView* input_group = new BGroupView(B_HORIZONTAL, B_USE_SMALL_SPACING);
    BLayoutBuilder::Group<>(input_group)
        .Add(input_scroll)
        .Add(send_btn_)
    .End();

    auto* sessions_label   = new BStringView("sessions_label",   "Sessions");
    auto* transcript_label = new BStringView("transcript_label", "Conversation");
    auto* prompt_label     = new BStringView("prompt_label",     "Prompt");
    sessions_label->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    transcript_label->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    prompt_label->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

    // Sessions pane — built separately so we can enforce a minimum width.
    auto* sessions_group = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
    BLayoutBuilder::Group<>(sessions_group)
        .Add(sessions_label)
        .Add(session_scroll_)
    .End();
    sessions_group->SetExplicitMinSize(BSize(200, B_SIZE_UNSET));
    sessions_group->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));

    // Todos pane — third split child, mirrors the sessions pane pattern.
    // Filled lazily from MSG_TODOS_UPDATED; header shows done/total count.
    // Hidden initially; shown when the first todo is added.
    todos_header_ = new BStringView("todos_header", "Todos");
    todos_header_->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    todos_list_   = new BListView("todos_list", B_SINGLE_SELECTION_LIST);
    todos_scroll_ = new BScrollView("todos_scroll", todos_list_,
                                    0, false, true, B_FANCY_BORDER);
    todos_group_ = new BGroupView(B_VERTICAL, B_USE_SMALL_SPACING);
    BLayoutBuilder::Group<>(todos_group_)
        .Add(todos_header_)
        .Add(todos_scroll_)
    .End();
    todos_group_->SetExplicitMinSize(BSize(180, B_SIZE_UNSET));
    todos_group_->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));

    // Menu bar sits at the top; content area below with window insets.
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .Add(menu_bar_)
        .AddGroup(B_HORIZONTAL, 0)
            .SetInsets(B_USE_WINDOW_INSETS)
            .AddSplit(B_HORIZONTAL, B_USE_HALF_ITEM_SPACING)
                .Add(sessions_group, 0.20f)
                .AddGroup(B_VERTICAL, B_USE_SMALL_SPACING, 0.60f)
                    .Add(toolbar_group)
                    .Add(status_strip_)
                    .Add(transcript_label)
                    .Add(chat_view_->ScrollContainer())
                    .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
                        .Add(prompt_label)
                        .AddGlue()
                        .Add(auto_edits_chk_)
                        .Add(yolo_chk_)
                    .End()
                    .Add(input_group)
                .End()
                .Add(todos_group_, 0.20f)
                .SetCollapsible(0, true)
                .SetCollapsible(2, true)
            .End()
        .End()
    .End();

    // Todos pane starts hidden — shown only when todos exist.
    todos_group_->Hide();


    // Populate session list and open/create initial session
    _RefreshSessionList();
    if (!session_ids_.empty()) {
        _SwitchToSession(0);
    } else {
        _NewSession();
    }

}

bool
MainWindow::QuitRequested()
{
    delete dir_panel_;
    dir_panel_ = nullptr;
    delete chat_view_;
    chat_view_ = nullptr;
    be_app->PostMessage(B_QUIT_REQUESTED);
    return true;
}

void
MainWindow::SelectProvider(const std::string& provider_id)
{
    default_provider_ = (provider_id == "openai") ? "openai" : "anthropic";
    bool want_openai = (default_provider_ == "openai");
    for (int32 i = 0; i < provider_menu_->CountItems(); i++) {
        BMenuItem* item = provider_menu_->ItemAt(i);
        bool is_openai = std::string(item->Label()) == "OpenAI / compatible";
        item->SetMarked(want_openai ? is_openai : !is_openai);
    }
}

void
MainWindow::MessageReceived(BMessage* msg)
{
    switch (msg->what) {
        case MSG_SUBMIT_PROMPT:
            _SubmitPrompt();
            break;
        case MSG_INTERRUPT:
            if (!active_session_id_.empty())
                engine_->interrupt(active_session_id_);
            interrupt_btn_->SetEnabled(false);
            break;
        case MSG_NEW_SESSION:
            _NewSession();
            break;
        case MSG_CHOOSE_DIR: {
            if (!dir_panel_) {
                dir_panel_ = new BFilePanel(B_OPEN_PANEL, new BMessenger(this),
                                            nullptr, B_DIRECTORY_NODE, false);
                dir_panel_->SetButtonLabel(B_DEFAULT_BUTTON, "Select");
                dir_panel_->Window()->SetTitle("Select Working Directory");
            }
            BEntry entry(project_dir_.c_str());
            entry_ref ref;
            if (entry.GetRef(&ref) == B_OK)
                dir_panel_->SetPanelDirectory(&ref);
            dir_panel_->Show();
            break;
        }
        case B_REFS_RECEIVED: {
            entry_ref ref;
            if (msg->FindRef("refs", &ref) == B_OK) {
                BEntry entry(&ref, true);
                BPath path;
                if (entry.GetPath(&path) == B_OK && entry.IsDirectory()) {
                    project_dir_ = path.Path();
                    dir_btn_->SetLabel(dir_basename(project_dir_).c_str());
                    if (!active_session_id_.empty())
                        store_.update_directory(active_session_id_, project_dir_);
                    BMessage notify(MSG_DIR_CHANGED);
                    notify.AddString("path", project_dir_.c_str());
                    be_app->PostMessage(&notify);
                }
            }
            break;
        }
        case MSG_SELECT_SESSION: {
            if (suppress_next_select_ > 0) {
                --suppress_next_select_;
                break;
            }
            int32 idx = -1;
            if (msg->FindInt32("index", &idx) != B_OK)
                idx = session_list_->CurrentSelection();
            if (idx >= 0)
                _SelectSession(static_cast<int>(idx));
            break;
        }
        case MSG_DELETE_SESSION: {
            int32 idx = -1;
            msg->FindInt32("index", &idx);
            if (idx >= 0 && idx < (int32)session_ids_.size()) {
                std::string sid = session_ids_[idx];
                store_.delete_session(sid);
                if (active_session_id_ == sid)
                    active_session_id_.clear();
                _RefreshSessionList();
                if (!session_ids_.empty()) {
                    int32 next = std::min(idx, (int32)session_ids_.size() - 1);
                    _SwitchToSession(next);
                } else {
                    _NewSession();
                }
            }
            break;
        }
        case MSG_TEXT_DELTA:
            _HandleTextDelta(msg);
            break;
        case MSG_TOOL_CALLED:
            _HandleToolCalled(msg);
            break;
        case MSG_TOOL_RESULT:
            _HandleToolResult(msg);
            break;
        case MSG_STEP_STARTED:
            _HandleStepStarted();
            break;
        case MSG_STEP_ENDED:
            _HandleStepEnded(msg);
            break;
        case MSG_STEP_FAILED:
            _HandleStepFailed(msg);
            break;
        case MSG_PERMISSION_REQ:
            _HandlePermissionReq(msg);
            break;
        case MSG_PLAN_PROPOSED:
            _HandlePlanProposed(msg);
            break;
        case MSG_TODOS_UPDATED:
            _HandleTodosUpdated(msg);
            break;
        case MSG_PLAN_DECISION:
            _HandlePlanDecision(msg);
            break;
        case MSG_TOGGLE_MODE:
            _ToggleMode();
            break;
        case MSG_SHOW_SETTINGS:
            be_app->PostMessage(msg);
            break;
        case MSG_AUTO_ALLOW_EDITS:
        case MSG_YOLO:
            be_app->PostMessage(msg);
            break;
        case MSG_FETCH_MODELS: {
            // Provider changed (or initial fetch from be_app/HaiCodeApp).
            // Read the marked item from the menu — radio mode moves the mark
            // on click before this handler runs.
            BMenuItem* marked = provider_menu_->FindMarked();
            std::string pid = (marked && std::string(marked->Label()) == "OpenAI / compatible")
                              ? "openai" : "anthropic";
            // If the message came from outside the menu (e.g. settings save or
            // startup), it carries provider_id explicitly — prefer that.
            const char* pid_str = nullptr;
            if (msg->FindString("provider_id", &pid_str) == B_OK && pid_str) {
                pid = pid_str;
            }
            default_provider_ = (pid == "openai") ? "openai" : "anthropic";
            _ApplyProviderModelToActiveSession();
            _PersistProviderModel();

            // Immediately reset the model dropdown so the user isn't shown the
            // previous provider's models with a stale mark while the fetch is
            // in flight. default_model_ is preserved so that if the fetch fails
            // or returns no matches, the next session still uses a sensible
            // value.
            while (model_menu_->CountItems() > 0)
                delete model_menu_->RemoveItem((int32)0);
            auto* loading_item = new BMenuItem("(loading\xe2\x80\xa6)", nullptr);
            loading_item->SetEnabled(false);
            loading_item->SetMarked(true);
            model_menu_->AddItem(loading_item);

            BMessage fwd(MSG_FETCH_MODELS);
            fwd.AddString("provider_id", default_provider_.c_str());
            be_app->PostMessage(&fwd);
            break;
        }
        case MSG_MODEL_SELECTED: {
            // User clicked a model menu item. default_model_ is the single
            // source of truth — _NewSession/_UpdateMaxContext read it instead
            // of querying the menu, so we just sync it here.
            BMenuItem* marked = model_menu_->FindMarked();
            if (marked) {
                default_model_ = marked->Label();
                _ApplyProviderModelToActiveSession();
                _UpdateMaxContext();
                _PersistProviderModel();
            }
            break;
        }
        case MSG_MODELS_LOADED: {
            // Discard results from a provider that is no longer selected.
            // Without this check, a stale fetch (e.g. Anthropic completing
            // after the user switched to OpenAI) overwrites default_model_
            // and patches the active session with the wrong provider's model.
            {
                const char* loaded_pid = nullptr;
                if (msg->FindString("provider_id", &loaded_pid) == B_OK
                        && loaded_pid && loaded_pid != default_provider_) {
                    break;
                }
            }

            // Repopulate model dropdown with server-provided list.
            // default_model_ is preserved across the repopulation: if the
            // freshly fetched list still contains it, we re-mark it; otherwise
            // we fall back to the first available model and update
            // default_model_ to match.
            std::string preserved = default_model_;

            while (model_menu_->CountItems() > 0)
                delete model_menu_->RemoveItem((int32)0);

            const char* m = nullptr;
            for (int32 i = 0; msg->FindString("model", i, &m) == B_OK; ++i) {
                model_menu_->AddItem(new BMenuItem(m, new BMessage(MSG_MODEL_SELECTED)));
            }

            BMenuItem* to_mark = nullptr;
            if (model_menu_->CountItems() > 0) {
                // Prefer re-marking the previously selected model.
                if (auto* existing = model_menu_->FindItem(preserved.c_str()))
                    to_mark = existing;
                else
                    to_mark = model_menu_->ItemAt(0);
            } else {
                auto* none_item = new BMenuItem("(none available)", nullptr);
                none_item->SetEnabled(false);
                model_menu_->AddItem(none_item);
                to_mark = none_item;
            }
            to_mark->SetMarked(true);
            default_model_ = to_mark->Label();
            _UpdateMaxContext();
            // Sync the engine: without this, switching provider leaves the
            // active session's stored model stale (the auto-marked default
            // never reaches the DB), so the next prompt goes out against
            // the wrong API.
            _ApplyProviderModelToActiveSession();
            _PersistProviderModel();
            break;
        }
        case MSG_PERMISSION_REP: {
            void* promise_raw = nullptr;
            int32 effect_int  = 2; // default Deny
            msg->FindPointer("promise_ptr", &promise_raw);
            msg->FindInt32("effect", &effect_int);

            if (effect_int == 1) {
                // "Allow Always" — persist the rule in the PermissionGate via be_app
                const char* action   = nullptr;
                const char* resource = nullptr;
                msg->FindString("action",   &action);
                msg->FindString("resource", &resource);
                if (action && resource) {
                    BMessage perm(MSG_ADD_PERMISSION);
                    perm.AddString("action",   action);
                    perm.AddString("resource", resource);
                    be_app->PostMessage(&perm);
                }
            }

            if (promise_raw) {
                auto* promise = static_cast<std::promise<haicode::PermissionEffect>*>(promise_raw);
                haicode::PermissionEffect effect = (effect_int <= 1)
                    ? haicode::PermissionEffect::Allow
                    : haicode::PermissionEffect::Deny;
                promise->set_value(effect);
                delete promise;
            }
            break;
        }
        default:
            BWindow::MessageReceived(msg);
            break;
    }
}

void
MainWindow::_RefreshSessionList()
{
    // Remove old items
    session_list_->MakeEmpty();
    session_ids_.clear();

    auto sessions = store_.list(50);
    for (auto& si : sessions) {
        std::string title;
        if (!si.title.empty()) {
            title = si.title;
        } else if (si.id.size() >= 20) {
            // ID format: prefix_XXXXXXXXXXXXXXXX (16 hex descending timestamp) + 8 hex random
            // Recover creation time: creation_ms = INT64_MAX - desc
            try {
                uint64_t desc = std::stoull(si.id.substr(4, 16), nullptr, 16);
                int64_t  ms   = (int64_t)(INT64_MAX - (int64_t)desc);
                time_t   sec  = (time_t)(ms / 1000);
                struct tm t;
                localtime_r(&sec, &t);
                char buf[32];
                strftime(buf, sizeof(buf), "%m/%d %H:%M", &t);
                title = buf;
            } catch (...) {
                title = si.id.substr(si.id.size() - 8);
            }
        } else {
            title = si.id;
        }
        session_list_->AddItem(new BStringItem(title.c_str()));
        session_ids_.push_back(si.id);
    }
}

void
MainWindow::_NewSession()
{
    // default_model_ / default_provider_ are the single sources of truth — kept
    // in sync with the dropdowns via MSG_MODEL_SELECTED / MSG_FETCH_MODELS /
    // MSG_MODELS_LOADED / _SelectSession. Reading FindMarked() here would race
    // with repopulation and menu-mark timing.
    std::string model = default_model_;
    std::string provider = default_provider_;
    std::string sid = engine_->create_session(project_dir_, "", model, provider);
    active_session_id_ = sid;

    // Notify relay of new active session
    BMessage notify(MSG_ACTIVE_SESSION);
    notify.AddString("session_id", sid.c_str());
    be_app->PostMessage(&notify);

    // Refresh list and select new item. BListView may emit 1 or 2 selection
    // notifications on Select() (deselect previous + select new) — bump the
    // suppress counter high enough to swallow all of them so a stray
    // MSG_SELECT_SESSION doesn't run _SelectSession on the wrong session.
    _RefreshSessionList();
    for (int i = 0; i < (int)session_ids_.size(); ++i) {
        if (session_ids_[i] == sid) {
            suppress_next_select_ = 2;
            session_list_->Select(i);
            break;
        }
    }

    chat_view_->Clear();
    chat_view_->AppendSystem("New session started.");
    interrupt_btn_->SetEnabled(false);
    last_prompt_input_ = 0;
    last_prompt_output_ = 0;
    session_input_total_ = 0;
    session_output_total_ = 0;
    current_context_tokens_ = 0;
    session_cost_ = 0.0;
    engine_running_ = false;
    streaming_state_ = "idle";
    current_tool_name_.clear();
    _UpdateMaxContext();
    _RefreshModeButton();
    _RefreshTodosFromEngine();
    _UpdateStatusStrip();
    if (input_view_->Window()) input_view_->MakeFocus(true);

    // Reset permission checkboxes. SetValue() changes the visual state but
    // does NOT invoke the message, so post explicit resets to be_app so it
    // clears always_rules_ and toggle flags via _ApplySessionRules().
    if (auto_edits_chk_) auto_edits_chk_->SetValue(B_CONTROL_OFF);
    if (yolo_chk_)       yolo_chk_->SetValue(B_CONTROL_OFF);
    be_app->PostMessage(MSG_NEW_SESSION);
    {
        BMessage m(MSG_AUTO_ALLOW_EDITS);
        m.AddInt32("be:value", B_CONTROL_OFF);
        be_app->PostMessage(&m);
    }
    {
        BMessage m(MSG_YOLO);
        m.AddInt32("be:value", B_CONTROL_OFF);
        be_app->PostMessage(&m);
    }
}

void
MainWindow::_SelectSession(int idx)
{
    if (idx < 0 || idx >= (int)session_ids_.size()) return;

    active_session_id_ = session_ids_[idx];

    // Notify relay of newly active session
    BMessage notify(MSG_ACTIVE_SESSION);
    notify.AddString("session_id", active_session_id_.c_str());
    be_app->PostMessage(&notify);

    // Sync toolbar to session's stored provider/model/directory
    auto si = store_.get(active_session_id_);
    if (si) {
        // Restore working directory
        if (!si->directory.empty()) {
            project_dir_ = si->directory;
            dir_btn_->SetLabel(dir_basename(project_dir_).c_str());
        }

        // Restore provider + model dropdowns
        std::string provider_id, model_id;
        try {
            auto mj = nlohmann::json::parse(si->model_json);
            provider_id = mj.value("provider_id", "");
            model_id    = mj.value("id", "");
        } catch (...) {}

        if (!provider_id.empty())
            SelectProvider(provider_id);

        if (!model_id.empty()) {
            // Try to mark the matching item. If none matches (different
            // provider, list still loading), explicitly clear any stale mark
            // so the dropdown doesn't show a model from a previous session.
            // default_model_ is set unconditionally — _NewSession reads it
            // directly, not the menu state.
            bool found = false;
            for (int32 i = 0; i < model_menu_->CountItems(); i++) {
                BMenuItem* item = model_menu_->ItemAt(i);
                if (!item) continue;
                if (model_id == item->Label()) {
                    item->SetMarked(true);
                    found = true;
                }
            }
            if (!found) {
                for (int32 i = 0; i < model_menu_->CountItems(); i++) {
                    if (auto* item = model_menu_->ItemAt(i))
                        item->SetMarked(false);
                }
            }
            default_model_ = model_id;
        }
    }

    chat_view_->Clear();
    _LoadHistory(active_session_id_);
    interrupt_btn_->SetEnabled(false);
    last_prompt_input_ = 0;
    last_prompt_output_ = 0;
    session_input_total_ = 0;
    session_output_total_ = 0;
    current_context_tokens_ = 0;
    session_cost_ = 0.0;
    engine_running_ = false;
    streaming_state_ = "idle";
    current_tool_name_.clear();
    _UpdateMaxContext();
    _RefreshModeButton();
    _RefreshTodosFromEngine();
    _UpdateStatusStrip();
    if (input_view_->Window()) input_view_->MakeFocus(true);
}

void
MainWindow::_SwitchToSession(int idx)
{
    _SelectSession(idx);
    // See _NewSession: Select() may emit multiple notifications.
    suppress_next_select_ = 2;
    session_list_->Select(idx);
}

void
MainWindow::_SubmitPrompt()
{
    if (active_session_id_.empty()) {
        _NewSession();
    }

    // Get text from input
    BString input_text = input_view_->Text();
    input_text.Trim();
    if (input_text.Length() == 0) return;

    std::string text(input_text.String());
    input_view_->SetText("");
    input_view_->MakeFocus(true);

    chat_view_->AppendUserText(text);
    interrupt_btn_->SetEnabled(true);

    // Reset per-prompt token counters; engine_running_ flips true on first
    // MSG_STEP_STARTED, streaming_state_ goes to "thinking" then.
    last_prompt_input_ = 0;
    last_prompt_output_ = 0;
    engine_running_ = true;
    streaming_state_ = "thinking";
    current_tool_name_.clear();
    _UpdateStatusStrip();

    // Submit to engine (runs on engine thread)
    engine_->submit_prompt(active_session_id_, text);
}

void
MainWindow::_LoadHistory(const std::string& session_id)
{
    auto messages = store_.load_messages(session_id);
    for (auto& sm : messages) {
        try {
            json data = json::parse(sm.data_json);
            if (sm.type == "user_prompted") {
                std::string text = data.value("text", "");
                if (!text.empty()) chat_view_->AppendUserText(text);
            } else if (sm.type == "assistant_text") {
                std::string text = data.value("text", "");
                if (!text.empty()) {
                    chat_view_->AppendTextDelta(text);
                    chat_view_->EndStreaming();
                }
                if (data.contains("tool_calls") && data["tool_calls"].is_array()) {
                    for (auto& tc : data["tool_calls"]) {
                        std::string name = tc.value("name", "");
                        std::string input_json;
                        if (tc.contains("input")) {
                            try { input_json = tc["input"].dump(2); } catch (...) {}
                        }
                        chat_view_->AppendToolCalled(name, input_json);
                    }
                }
            } else if (sm.type == "tool_called") {
                std::string tool_name  = data.value("tool_name", "");
                std::string input_json;
                if (data.contains("input")) {
                    try { input_json = data["input"].dump(2); } catch (...) {}
                }
                chat_view_->AppendToolCalled(tool_name, input_json);
            } else if (sm.type == "tool_result") {
                std::string output  = data.value("output", "");
                bool success = data.value("success", true);
                chat_view_->AppendToolResult(output, success);
            }
        } catch (const std::exception&) {
            // Skip malformed messages
        }
    }
}

void
MainWindow::_HandleTextDelta(BMessage* msg)
{
    const char* delta = nullptr;
    if (msg->FindString("delta", &delta) == B_OK && delta) {
        chat_view_->AppendTextDelta(delta);
        if (streaming_state_ != "streaming") {
            streaming_state_ = "streaming";
            _UpdateStatusStrip();
        }
    }
}

void
MainWindow::_HandleToolCalled(BMessage* msg)
{
    const char* tool_name  = nullptr;
    const char* input_json = nullptr;
    msg->FindString("tool_name",  &tool_name);
    msg->FindString("input_json", &input_json);
    chat_view_->AppendToolCalled(tool_name  ? tool_name  : "",
                                  input_json ? input_json : "");
    current_tool_name_ = tool_name ? tool_name : "";
    streaming_state_ = "tool";
    _UpdateStatusStrip();
}

void
MainWindow::_HandleToolResult(BMessage* msg)
{
    const char* output = nullptr;
    bool success = true;
    msg->FindString("output",  &output);
    msg->FindBool("success",   &success);
    chat_view_->AppendToolResult(output ? output : "", success);
    // After a tool finishes, the engine may keep going (another tool or more
    // text). If it does, MSG_STEP_STARTED will reset state to "thinking".
    current_tool_name_.clear();
    if (engine_running_ && streaming_state_ == "tool") {
        streaming_state_ = "thinking";
        _UpdateStatusStrip();
    }
}

void
MainWindow::_HandleStepStarted()
{
    engine_running_ = true;
    streaming_state_ = "thinking";
    current_tool_name_.clear();
    _UpdateStatusStrip();
}

void
MainWindow::_HandleStepEnded(BMessage* msg)
{
    chat_view_->EndStreaming();

    int32 in_tok = 0, out_tok = 0;
    msg->FindInt32("usage_input",  &in_tok);
    msg->FindInt32("usage_output", &out_tok);
    double step_cost = 0.0;
    msg->FindDouble("cost_usd", &step_cost);
    last_prompt_input_   += in_tok;
    last_prompt_output_  += out_tok;
    session_input_total_ += in_tok;
    session_output_total_+= out_tok;
    session_cost_        += step_cost;
    // The input side reflects the full conversation size as the provider saw
    // it on this step — that's our best estimate of current context usage.
    if (in_tok > 0) current_context_tokens_ = in_tok;

    const char* finish_reason = nullptr;
    msg->FindString("finish_reason", &finish_reason);
    bool more = (finish_reason && std::string(finish_reason) == "tool_use");
    interrupt_btn_->SetEnabled(more);

    if (more) {
        // Another step will follow — keep "thinking" state.
        engine_running_ = true;
        streaming_state_ = "thinking";
    } else {
        engine_running_ = false;
        streaming_state_ = "idle";
        current_tool_name_.clear();
    }
    _UpdateStatusStrip();
}

void
MainWindow::_HandleStepFailed(BMessage* msg)
{
    chat_view_->EndStreaming();
    interrupt_btn_->SetEnabled(false);
    engine_running_ = false;
    streaming_state_ = "idle";
    current_tool_name_.clear();

    const char* error = nullptr;
    msg->FindString("error", &error);
    std::string err_text = error ? error : "Unknown error";
    chat_view_->AppendSystem("Error: " + err_text);
    _UpdateStatusStrip();
}

void
MainWindow::_HandlePermissionReq(BMessage* msg)
{
    const char* action   = nullptr;
    const char* resource = nullptr;
    const char* detail   = nullptr;
    void* promise_raw    = nullptr;

    msg->FindString("action",   &action);
    msg->FindString("resource", &resource);
    msg->FindString("detail",   &detail);
    msg->FindPointer("promise_ptr", &promise_raw);

    PermissionWindow* perm_win = new PermissionWindow(
        action   ? action   : "",
        resource ? resource : "",
        detail   ? detail   : "",
        BMessenger(this),
        promise_raw
    );
    perm_win->Show();
}

void
MainWindow::PostPermissionRequest(const std::string& action,
                                  const std::string& resource,
                                  const std::string& detail,
                                  void* promise_ptr)
{
    BMessage msg(MSG_PERMISSION_REQ);
    msg.AddString("action",   action.c_str());
    msg.AddString("resource", resource.c_str());
    msg.AddString("detail",   detail.c_str());
    msg.AddPointer("promise_ptr", promise_ptr);
    PostMessage(&msg);
}

void
MainWindow::_HandlePlanProposed(BMessage* msg)
{
    const char* plan = nullptr;
    const char* path = nullptr;
    msg->FindString("plan", &plan);
    msg->FindString("path", &path);

    PlanReviewWindow* w = new PlanReviewWindow(
        plan ? plan : "",
        path ? path : "",
        active_session_id_,
        BMessenger(this)
    );
    w->Show();
}

void
MainWindow::_HandlePlanDecision(BMessage* msg)
{
    bool approved = false;
    msg->FindBool("approved", &approved);
    const char* sid_c = nullptr;
    msg->FindString("session_id", &sid_c);
    std::string sid = sid_c ? sid_c : active_session_id_;

    if (!sid.empty() && engine_) {
        if (approved) {
            engine_->set_mode(sid, haicode::SessionMode::Build);
            if (sid == active_session_id_) {
                _RefreshModeButton();
                _UpdateStatusStrip();
                chat_view_->AppendSystem("Plan approved \xe2\x80\x94 switching to Build mode.");
                interrupt_btn_->SetEnabled(true);
                engine_running_ = true;
                streaming_state_ = "thinking";
                current_tool_name_.clear();
                _UpdateStatusStrip();
            }
            engine_->continue_session(sid);
        } else {
            if (sid == active_session_id_) {
                chat_view_->AppendSystem("Plan discarded \xe2\x80\x94 staying in Plan mode.");
            }
        }
    }
}

void
MainWindow::_HandleTodosUpdated(BMessage* msg)
{
    if (!todos_list_) {
        fprintf(stderr, "[todos] _HandleTodosUpdated: no todos_list_\n");
        return;
    }

    while (todos_list_->CountItems() > 0)
        delete todos_list_->RemoveItem((int32)0);

    int done = 0, total = 0;
    const char* content = nullptr;
    const char* active  = nullptr;
    const char* status  = nullptr;
    int32 idx = 0;
    while (true) {
        if (msg->FindString("todo_content", idx, &content) != B_OK) break;
        msg->FindString("todo_active", idx, &active);
        msg->FindString("todo_status", idx, &status);
        std::string st = status ? status : "pending";
        const char* mark = "[ ]";
        if (st == "completed")             mark = "[x]";
        else if (st == "in_progress")      mark = "[>]";

        std::string label = std::string(mark) + " " + (content ? content : "");
        if (st == "in_progress" && active && *active)
            label += std::string("  — ") + active;
        todos_list_->AddItem(new BStringItem(label.c_str()));
        fprintf(stderr, "[todos] added item %d: '%s'\n", (int)idx, label.c_str());
        if (st == "completed") ++done;
        ++total;
        ++idx;
    }
    fprintf(stderr, "[todos] _HandleTodosUpdated total=%zu items, list now has %zu items\n",
            (size_t)total, (size_t)todos_list_->CountItems());

    char hdr[64];
    snprintf(hdr, sizeof(hdr), "Todos (%d/%d done)", done, total);
    if (todos_header_) todos_header_->SetText(hdr);

    if (todos_group_) {
        if (total == 0 && !todos_group_->IsHidden()) todos_group_->Hide();
        else if (total > 0 && todos_group_->IsHidden())  todos_group_->Show();
    }
}

void
MainWindow::_RefreshTodosFromEngine()
{
    if (!engine_ || active_session_id_.empty() || !todos_list_) return;
    auto todos = engine_->get_todos(active_session_id_);

    while (todos_list_->CountItems() > 0)
        delete todos_list_->RemoveItem((int32)0);

    int done = 0;
    for (auto& t : todos) {
        const char* mark = "[ ]";
        if (t.status == "completed")        mark = "[x]";
        else if (t.status == "in_progress") mark = "[>]";

        std::string label = std::string(mark) + " " + t.content;
        if (t.status == "in_progress" && !t.active_form.empty())
            label += "  — " + t.active_form;
        todos_list_->AddItem(new BStringItem(label.c_str()));
        if (t.status == "completed") ++done;
    }

    char hdr[64];
    snprintf(hdr, sizeof(hdr), "Todos (%d/%zu done)", done, todos.size());
    if (todos_header_) todos_header_->SetText(hdr);

    if (todos_group_) {
        if (todos.empty() && !todos_group_->IsHidden()) todos_group_->Hide();
        else if (!todos.empty() && todos_group_->IsHidden()) todos_group_->Show();
    }
}

void
MainWindow::_ToggleMode()
{
    if (active_session_id_.empty() || !engine_) return;
    auto current = engine_->get_mode(active_session_id_);

    if (current == haicode::SessionMode::Plan) {
        BAlert* alert = new BAlert("Switch to Build Mode",
            "Switch from Plan mode to Build mode?\n\n"
            "Build mode allows file edits and shell commands.",
            "Cancel", "Switch to Build", nullptr,
            B_WIDTH_AS_USUAL, B_WARNING_ALERT);
        alert->SetShortcut(0, B_ESCAPE);
        int32 choice = alert->Go();
        if (choice != 1) return;
    }

    auto next = (current == haicode::SessionMode::Plan)
                ? haicode::SessionMode::Build
                : haicode::SessionMode::Plan;
    engine_->set_mode(active_session_id_, next);
    _RefreshModeButton();
    _UpdateStatusStrip();
}

void
MainWindow::_RefreshModeButton()
{
    if (!mode_btn_ || active_session_id_.empty() || !engine_) return;
    auto m = engine_->get_mode(active_session_id_);
    mode_btn_->SetLabel(m == haicode::SessionMode::Plan ? "Mode: Plan" : "Mode: Build");
}

static std::string format_tokens(int n)
{
    char buf[32];
    if (n >= 10000) snprintf(buf, sizeof(buf), "%.1fk", n / 1000.0);
    else            snprintf(buf, sizeof(buf), "%d", n);
    return buf;
}

static std::string format_cost(double usd)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "$%.4f", usd);
    return buf;
}

void
MainWindow::_UpdateStatusStrip()
{
    if (!status_strip_) return;

    // Mode badge
    std::string badge = "[BUILD]";
    if (engine_ && !active_session_id_.empty()
        && engine_->get_mode(active_session_id_) == haicode::SessionMode::Plan)
        badge = "[PLAN]";

    // State glyph + label
    std::string glyph, label;
    if (!engine_running_) {
        glyph = "\xe2\x9c\x93";  // CHECK MARK
        label = "idle";
    } else if (streaming_state_ == "streaming") {
        glyph = "\xf0\x9f\x92\xac";  // 💬
        label = "streaming\xe2\x80\xa6";
    } else if (streaming_state_ == "tool") {
        glyph = "\xf0\x9f\x94\xa7";  // 🔧
        label = "running tool";
        if (!current_tool_name_.empty()) label += ": " + current_tool_name_;
    } else {
        // thinking / before first token
        glyph = "\xf0\x9f\x92\xa1";  // 💡
        label = "thinking\xe2\x80\xa6";
    }

    std::string s = badge + " " + glyph + " " + label;

    // Token + context strip (only if we have data)
    if (last_prompt_input_ > 0 || last_prompt_output_ > 0 || session_input_total_ > 0) {
        s += "   last turn: \xe2\x86\x91" + format_tokens(last_prompt_input_)
           + " \xe2\x86\x93" + format_tokens(last_prompt_output_)
           + "   session: \xe2\x86\x91" + format_tokens(session_input_total_)
           + " \xe2\x86\x93" + format_tokens(session_output_total_);
        if (session_cost_ > 0.0)
            s += "  " + format_cost(session_cost_);
    }
    if (current_context_tokens_ > 0) {
        s += "   context: " + format_tokens(current_context_tokens_);
        if (max_context_ > 0) {
            int pct = (max_context_ > 0)
                      ? std::min(100, (int)(current_context_tokens_ * 100LL / max_context_))
                      : 0;
            char pctbuf[8];
            snprintf(pctbuf, sizeof(pctbuf), "%d%%", pct);
            s += " / " + format_tokens(max_context_) + " (" + pctbuf + ")";
        }
    }

    status_strip_->SetText(s.c_str());
}

void
MainWindow::_UpdateMaxContext()
{
    if (!engine_) { max_context_ = 0; return; }
    // default_model_ / default_provider_ are the sources of truth — see _NewSession.
    max_context_ = haicode::get_context_window(default_provider_, default_model_,
                                               engine_->config().model_contexts);
    _UpdateStatusStrip();
}

void
MainWindow::_ApplyProviderModelToActiveSession()
{
    // Provider/model dropdowns changed — patch the active session so the next
    // prompt uses the new values. Without this, the dropdown only affects
    // sessions created afterwards; users expect the switch to take effect
    // immediately on the session they're looking at.
    if (!engine_ || active_session_id_.empty()) return;
    engine_->update_provider_model(active_session_id_, default_provider_, default_model_);
}

void
MainWindow::_PersistProviderModel()
{
    // Mirror the current dropdowns into the global config so they survive
    // restart. Posted to be_app, which owns the ConfigLoader and JSON file.
    BMessage pm(MSG_PERSIST_PM);
    pm.AddString("provider", default_provider_.c_str());
    pm.AddString("model",    default_model_.c_str());
    be_app->PostMessage(&pm);
}
