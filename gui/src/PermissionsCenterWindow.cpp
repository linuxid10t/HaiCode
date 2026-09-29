#include "PermissionsCenterWindow.h"
#include "PermissionRuleEditWindow.h"
#include "Messages.h"

#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <Menu.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Message.h>
#include <Messenger.h>
#include <PopUpMenu.h>
#include <ScrollView.h>
#include <StringItem.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>
#include <TextView.h>

#include <nlohmann/json.hpp>

#include <chrono>

namespace {

// Center-internal message ids
const uint32 kBtnRuleAdd = 'rAdd';
const uint32 kBtnRuleEdit = 'rEdt';
const uint32 kBtnRuleRemove = 'rRem';
const uint32 kBtnRuleUp = 'rUp ';
const uint32 kBtnRuleDown = 'rDwn';
const uint32 kBtnPolicySave = 'rSav';
const uint32 kBtnInspect = 'rIns';
const uint32 kPolicySourceSelected = 'rSrc';

std::string short_target(const std::string& resource, size_t max = 60) {
    if (resource.size() <= max) return resource;
    return "..." + resource.substr(resource.size() - max + 3);
}

const char* effect_label(haicode::PermissionEffect e) {
    switch (e) {
    case haicode::PermissionEffect::Allow: return "Allow";
    case haicode::PermissionEffect::Deny:  return "Deny";
    default:                               return "Ask";
    }
}

} // namespace

// ---- PermissionActivityLog ----

void PermissionActivityLog::add(const PermissionActivityEntry& e) {
    std::lock_guard<std::mutex> lock(mu_);
    auto& q = per_session_[e.session_id];
    q.push_back(e);
    while (q.size() > kMaxPerSession) q.pop_front();
}

std::vector<PermissionActivityEntry> PermissionActivityLog::entries(
        const std::string& session_id, size_t limit) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<PermissionActivityEntry> out;
    auto it = per_session_.find(session_id);
    if (it == per_session_.end()) return out;
    size_t n = 0;
    for (auto rit = it->second.rbegin();
            rit != it->second.rend() && n < limit; ++rit, ++n)
        out.push_back(*rit);
    return out;
}

size_t PermissionActivityLog::count(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = per_session_.find(session_id);
    return it == per_session_.end() ? 0 : it->second.size();
}

// ---- PermissionsCenterWindow ----

PermissionsCenterWindow::PermissionsCenterWindow(
        haicode::PermissionGate& gate,
        haicode::PermissionRequestBroker& broker,
        haicode::ToolRegistry& tools,
        haicode::SessionStore& store,
        PermissionActivityLog& activity,
        std::function<haicode::SessionEngine*()> engine_of,
        const std::string& initial_session,
        const std::string& global_policy_path,
        const std::string& project_policy_path,
        const std::string& project_dir,
        BMessenger notify_target)
    : BWindow(BRect(120, 120, 780, 640), "Permissions",
              B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS)
    , gate_(gate)
    , broker_(broker)
    , tools_(tools)
    , store_(store)
    , activity_(activity)
    , engine_of_(std::move(engine_of))
    , global_policy_path_(global_policy_path)
    , project_policy_path_(project_policy_path)
    , project_dir_(project_dir)
    , notify_target_(notify_target)
    , selected_session_(initial_session)
{
    // ---- Session tab ----
    session_menu_ = new BPopUpMenu("session");
    session_menu_->SetRadioMode(true);
    session_menu_->SetLabelFromMarked(true);
    session_field_ = new BMenuField("session_field", "Session:", session_menu_);

    mode_view_ = new BStringView("mode", "Mode: -");
    dir_view_ = new BStringView("dir", "Directory: -");
    mode_view_->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    dir_view_->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

    auto_writes_chk_ = new BCheckBox("auto_writes",
        "Automatically allow writes",
        new BMessage(MSG_PERM_TOGGLE_AUTO));
    read_outside_chk_ = new BCheckBox("read_outside",
        "Allow reads outside trusted roots",
        new BMessage(MSG_PERM_TOGGLE_READ));
    bypass_chk_ = new BCheckBox("bypass",
        "Bypass permission prompts",
        new BMessage(MSG_PERM_TOGGLE_BYPASS));
    toggle_note_ = new BStringView("toggle_note",
        "These apply to this session only and are restored when it reopens.\n"
        "Bypass is not a sandbox: mode and offline restrictions still apply.");
    BFont small(*be_plain_font);
    small.SetSize(10.0f);
    toggle_note_->SetFont(&small);
    toggle_note_->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

    pending_list_ = new BListView("pending", B_SINGLE_SELECTION_LIST);
    auto* pending_scroll = new BScrollView("pending_scroll", pending_list_,
                                           0, false, true, B_FANCY_BORDER);
    pending_allow_btn_ = new BButton("pending_allow", "Approve",
                                     new BMessage(MSG_PERM_PENDING_ALLOW));
    pending_deny_btn_ = new BButton("pending_deny", "Deny",
                                    new BMessage(MSG_PERM_PENDING_DENY));

    grant_list_ = new BListView("grants", B_SINGLE_SELECTION_LIST);
    auto* grant_scroll = new BScrollView("grant_scroll", grant_list_,
                                         0, false, true, B_FANCY_BORDER);
    revoke_btn_ = new BButton("revoke", "Revoke",
                              new BMessage(MSG_PERM_REVOKE));
    revoke_all_btn_ = new BButton("revoke_all", "Revoke All",
                                  new BMessage(MSG_PERM_REVOKE_ALL));
    grant_note_ = new BStringView("grant_note",
        "Temporary grants last until revoked or HaiCode exits.");
    grant_note_->SetFont(&small);
    grant_note_->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

    auto* session_tab = new BView("session_tab", B_SUPPORTS_LAYOUT);
    BLayoutBuilder::Group<>(session_tab, B_VERTICAL, B_USE_SMALL_SPACING)
        .SetInsets(B_USE_SMALL_INSETS)
        .Add(session_field_)
        .Add(mode_view_)
        .Add(dir_view_)
        .AddStrut(B_USE_SMALL_SPACING)
        .Add(auto_writes_chk_)
        .Add(read_outside_chk_)
        .Add(bypass_chk_)
        .Add(toggle_note_)
        .AddStrut(B_USE_SMALL_SPACING)
        .Add(new BStringView("pending_hdr", "Waiting for approval:"))
        .Add(pending_scroll)
        .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
            .Add(pending_allow_btn_)
            .Add(pending_deny_btn_)
            .AddGlue()
        .End()
        .AddStrut(B_USE_SMALL_SPACING)
        .Add(new BStringView("grant_hdr", "Temporary grants (this session):"))
        .Add(grant_scroll)
        .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
            .Add(revoke_btn_)
            .Add(revoke_all_btn_)
            .AddGlue()
        .End()
        .Add(grant_note_)
    .End();

    // ---- Policies tab ----
    policy_source_menu_ = new BPopUpMenu("source");
    policy_source_menu_->SetRadioMode(true);
    policy_source_menu_->SetLabelFromMarked(true);
    {
        auto* g = new BMenuItem("Global configuration",
                                new BMessage(kPolicySourceSelected));
        g->Message()->AddBool("project", false);
        auto* p = new BMenuItem("Loaded project configuration",
                                new BMessage(kPolicySourceSelected));
        p->Message()->AddBool("project", true);
        policy_source_menu_->AddItem(g);
        policy_source_menu_->AddItem(p);
        g->SetMarked(true);
    }
    policy_source_field_ = new BMenuField("source_field", "Source:",
                                          policy_source_menu_);

    policy_path_view_ = new BStringView("policy_path", "");
    policy_path_view_->SetFont(&small);
    policy_path_view_->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

    rules_list_ = new BListView("rules", B_SINGLE_SELECTION_LIST);
    auto* rules_scroll = new BScrollView("rules_scroll", rules_list_,
                                         0, false, true, B_FANCY_BORDER);
    rule_add_btn_ = new BButton("rule_add", "Add", new BMessage(kBtnRuleAdd));
    rule_edit_btn_ = new BButton("rule_edit", "Edit", new BMessage(kBtnRuleEdit));
    rule_remove_btn_ = new BButton("rule_remove", "Remove",
                                   new BMessage(kBtnRuleRemove));
    rule_up_btn_ = new BButton("rule_up", "Move Up", new BMessage(kBtnRuleUp));
    rule_down_btn_ = new BButton("rule_down", "Move Down",
                                 new BMessage(kBtnRuleDown));
    policy_save_btn_ = new BButton("policy_save", "Save",
                                   new BMessage(kBtnPolicySave));

    policy_note_ = new BStringView("policy_note",
        "Precedence (highest first): temporary grants, session toggles,\n"
        "configured rules. Within one source the LAST matching rule wins;\n"
        "Ask falls through to prompting. Global rules load first, project\n"
        "rules are appended after them.");
    policy_note_->SetFont(&small);
    policy_note_->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

    inspect_tool_ = new BTextControl("inspect_tool", "Tool:", "bash", nullptr);
    inspect_input_ = new BTextControl("inspect_input", "Input JSON:",
                                      R"({"command": "/bin/true"})", nullptr);
    inspect_result_ = new BTextView("inspect_result");
    inspect_result_->MakeEditable(false);
    inspect_result_->MakeSelectable(true);
    inspect_result_->SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    inspect_result_->SetLowColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    inspect_result_->SetExplicitMinSize(BSize(B_SIZE_UNSET, 44));
    auto* inspect_btn = new BButton("inspect", "Inspect",
                                    new BMessage(kBtnInspect));

    auto* policy_tab = new BView("policy_tab", B_SUPPORTS_LAYOUT);
    BLayoutBuilder::Group<>(policy_tab, B_VERTICAL, B_USE_SMALL_SPACING)
        .SetInsets(B_USE_SMALL_INSETS)
        .Add(policy_source_field_)
        .Add(policy_path_view_)
        .Add(rules_scroll)
        .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
            .Add(rule_add_btn_)
            .Add(rule_edit_btn_)
            .Add(rule_remove_btn_)
            .Add(rule_up_btn_)
            .Add(rule_down_btn_)
            .AddGlue()
            .Add(policy_save_btn_)
        .End()
        .Add(policy_note_)
        .AddStrut(B_USE_SMALL_SPACING)
        .Add(new BStringView("inspect_hdr",
                             "Operation inspector (no execution, no prompt):"))
        .Add(inspect_tool_)
        .Add(inspect_input_)
        .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
            .Add(inspect_btn)
            .AddGlue()
        .End()
        .Add(inspect_result_)
    .End();

    // ---- Activity tab ----
    auto* activity_list = new BListView("activity", B_SINGLE_SELECTION_LIST);
    activity_list_ = activity_list;
    auto* activity_scroll = new BScrollView("activity_scroll", activity_list,
                                            0, false, true, B_FANCY_BORDER);
    auto* activity_note = new BStringView("activity_note",
        "Authorization outcomes since HaiCode started (most recent first).\n"
        "Authorized means the tool was permitted to run, not that it succeeded.");
    activity_note->SetFont(&small);
    activity_note->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

    auto* activity_tab = new BView("activity_tab", B_SUPPORTS_LAYOUT);
    BLayoutBuilder::Group<>(activity_tab, B_VERTICAL, B_USE_SMALL_SPACING)
        .SetInsets(B_USE_SMALL_INSETS)
        .Add(activity_note)
        .Add(activity_scroll)
    .End();

    auto* tabs = new BTabView("tabs", B_WIDTH_FROM_WIDEST);
    tabs->AddTab(session_tab);
    tabs->AddTab(policy_tab);
    tabs->AddTab(activity_tab);
    tabs->TabAt(0)->SetLabel("Session");
    tabs->TabAt(1)->SetLabel("Policies");
    tabs->TabAt(2)->SetLabel("Activity");

    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(tabs)
    .End();

    Refresh();
}

PermissionsCenterWindow::~PermissionsCenterWindow()
{
}

void
PermissionsCenterWindow::Refresh()
{
    _RebuildSessionMenu();
    _RefreshSessionTab();
    _ReloadPolicyDocs();
    _RefreshRulesList();
    _RefreshActivityList();
}

// ---- Session tab ----

void
PermissionsCenterWindow::_RebuildSessionMenu()
{
    while (session_menu_->CountItems() > 0)
        delete session_menu_->RemoveItem(int32(0));
    auto sessions = store_.list(50);
    bool marked = false;
    for (auto& si : sessions) {
        std::string label = si.title.empty()
            ? (si.id.size() > 8 ? si.id.substr(si.id.size() - 8) : si.id)
            : si.title;
        auto* msg = new BMessage(MSG_PERM_SESSION_SELECTED);
        msg->AddString("session_id", si.id.c_str());
        auto* item = new BMenuItem(label.c_str(), msg);
        session_menu_->AddItem(item);
        if (si.id == selected_session_) {
            item->SetMarked(true);
            marked = true;
        }
    }
    if (!marked) {
        selected_session_.clear();
        if (session_menu_->ItemAt(0)) {
            session_menu_->ItemAt(0)->SetMarked(true);
            const char* sid = nullptr;
            if (session_menu_->ItemAt(0)->Message()
                    && session_menu_->ItemAt(0)->Message()
                            ->FindString("session_id", &sid) == B_OK && sid)
                selected_session_ = sid;
        }
    }
}

void
PermissionsCenterWindow::_SelectSession(const std::string& session_id)
{
    if (selected_session_ == session_id) return;
    selected_session_ = session_id;
    _RefreshSessionTab();
    _RefreshActivityList();
}

void
PermissionsCenterWindow::_RefreshSessionTab()
{
    if (selected_session_.empty()) {
        mode_view_->SetText("Mode: -");
        dir_view_->SetText("Directory: -");
        auto_writes_chk_->SetValue(B_CONTROL_OFF);
        read_outside_chk_->SetValue(B_CONTROL_OFF);
        bypass_chk_->SetValue(B_CONTROL_OFF);
        auto_writes_chk_->SetEnabled(false);
        read_outside_chk_->SetEnabled(false);
        bypass_chk_->SetEnabled(false);
        _RefreshPendingList();
        _RefreshGrantList();
        return;
    }

    auto si = store_.get(selected_session_);
    bool auto_edits = false, yolo = false, read_everywhere = false;
    std::string mode_str = "build", dir;
    if (si) {
        dir = si->directory;
        try {
            auto mj = nlohmann::json::parse(si->model_json, nullptr, false);
            if (!mj.is_discarded() && mj.is_object()) {
                auto_edits = mj.value("auto_edits", false);
                yolo = mj.value("yolo", false);
                read_everywhere = mj.value("allow_read_everywhere", false);
                mode_str = mj.value("mode", "build");
            }
        } catch (...) {}
    }
    if (auto* engine = engine_of_()) {
        auto m = engine->get_mode(selected_session_);
        mode_str = m == haicode::SessionMode::Plan ? "plan"
                 : m == haicode::SessionMode::Chat ? "chat" : "build";
    }

    mode_view_->SetText((std::string("Mode: ") + mode_str).c_str());
    dir_view_->SetText(("Directory: " + (dir.empty() ? "-" : dir)).c_str());

    // Keep the widgets from re-posting while restoring state.
    auto_writes_chk_->SetValue(auto_edits ? B_CONTROL_ON : B_CONTROL_OFF);
    read_outside_chk_->SetValue(read_everywhere ? B_CONTROL_ON : B_CONTROL_OFF);
    bypass_chk_->SetValue(yolo ? B_CONTROL_ON : B_CONTROL_OFF);

    // Mode-inapplicable controls stay visible but disabled with an
    // explanation (Build: all available; Plan: writes unavailable;
    // Chat: everything local unavailable).
    bool plan = mode_str == "plan";
    bool chat = mode_str == "chat";
    auto_writes_chk_->SetEnabled(!plan && !chat);
    read_outside_chk_->SetEnabled(plan);
    bypass_chk_->SetEnabled(!plan && !chat);
    auto_writes_chk_->SetToolTip((plan || chat)
        ? "Writes are unavailable in this mode" : "");
    read_outside_chk_->SetToolTip(!plan
        ? "Applies to Plan mode sessions" : "");
    bypass_chk_->SetToolTip((plan || chat)
        ? "Writes are unavailable in this mode" : "");

    _RefreshPendingList();
    _RefreshGrantList();
}

void
PermissionsCenterWindow::_RefreshPendingList()
{
    pending_reqs_ = broker_.pending_requests();
    pending_list_->MakeEmpty();
    for (const auto& req : pending_reqs_) {
        if (req.session_id != selected_session_) continue;
        std::string label = req.tool_name + "  " + short_target(req.resource);
        pending_list_->AddItem(new BStringItem(label.c_str()));
    }
    bool has_sel = pending_list_->CurrentSelection() >= 0;
    pending_allow_btn_->SetEnabled(has_sel);
    pending_deny_btn_->SetEnabled(has_sel);
}

void
PermissionsCenterWindow::_RefreshGrantList()
{
    grant_list_->MakeEmpty();
    grant_refs_.clear();
    if (selected_session_.empty()) {
        grant_note_->SetText("Temporary grants last until revoked or "
                             "HaiCode exits.");
        return;
    }
    auto snap = gate_.snapshot(selected_session_);
    for (const auto& r : snap.temporary_exact)
        grant_refs_.push_back({selected_session_, r.action, r.resource, true});
    for (const auto& r : snap.temporary_patterns)
        grant_refs_.push_back({selected_session_, r.action, r.resource, false});

    for (const auto& g : grant_refs_) {
        std::string label = g.action + "  " + short_target(g.resource)
                          + (g.exact ? "  [exact]" : "  [pattern]");
        grant_list_->AddItem(new BStringItem(label.c_str()));
    }
    if (grant_refs_.empty())
        grant_note_->SetText("No temporary grants for this session.");
    else
        grant_note_->SetText("Temporary grants last until revoked or "
                             "HaiCode exits. Revoking does not affect "
                             "operations already running.");
}

void
PermissionsCenterWindow::_OnToggle(uint32 what, bool on)
{
    if (selected_session_.empty()) return;

    if (what == MSG_PERM_TOGGLE_BYPASS && on) {
        BAlert* confirm = new BAlert("Bypass permissions",
            "Allow every tool call in this session without asking?\n"
            "This is not a sandbox: mode and offline restrictions still "
            "apply, and file writes, shell commands, and network access "
            "will all run unprompted.",
            "Cancel", "Enable", nullptr, B_WIDTH_AS_USUAL,
            B_WARNING_ALERT);
        confirm->SetShortcut(0, B_ESCAPE);
        if (confirm->Go() == 0) {
            bypass_chk_->SetValue(B_CONTROL_OFF);
            return;
        }
    }

    // Persist via the store, then let be_app re-apply that session's rules —
    // identical to the old toolbar toggles, so persistence format and
    // MainWindow's restore path keep working. The message carries the
    // session id so a background session's rules are never retargeted to
    // the selected one.
    auto si = store_.get(selected_session_);
    bool auto_edits = false, yolo = false, read_everywhere = false;
    if (si) {
        try {
            auto mj = nlohmann::json::parse(si->model_json, nullptr, false);
            if (!mj.is_discarded() && mj.is_object()) {
                auto_edits = mj.value("auto_edits", false);
                yolo = mj.value("yolo", false);
                read_everywhere = mj.value("allow_read_everywhere", false);
            }
        } catch (...) {}
    }
    if (what == MSG_PERM_TOGGLE_AUTO)   auto_edits = on;
    if (what == MSG_PERM_TOGGLE_BYPASS) yolo = on;
    if (what == MSG_PERM_TOGGLE_READ)   read_everywhere = on;
    store_.update_permission_flags(selected_session_, auto_edits, yolo,
                                   read_everywhere);

    // Mirror to be_app on the toggle's own message constant (it applies the
    // session rules); MainWindow's checkboxes pick the state up on the next
    // session switch.
    uint32 app_msg = what == MSG_PERM_TOGGLE_AUTO  ? MSG_AUTO_ALLOW_EDITS
                   : what == MSG_PERM_TOGGLE_BYPASS ? MSG_YOLO
                                                    : MSG_READ_EVERYWHERE;
    BMessage m(app_msg);
    m.AddInt32("be:value", on ? B_CONTROL_ON : B_CONTROL_OFF);
    m.AddString("session_id", selected_session_.c_str());
    be_app->PostMessage(&m);
}

void
PermissionsCenterWindow::_ApprovePending(bool allow)
{
    int sel = pending_list_->CurrentSelection();
    if (sel < 0 || size_t(sel) >= pending_reqs_.size()) return;
    const auto& req = pending_reqs_[size_t(sel)];
    broker_.resolve(req.id, allow
        ? haicode::PermissionDecision::AllowOnce
        : haicode::PermissionDecision::Deny);
    _RefreshPendingList();
}

void
PermissionsCenterWindow::_RevokeSelected()
{
    int sel = grant_list_->CurrentSelection();
    if (sel < 0 || size_t(sel) >= grant_refs_.size()) return;
    const auto& g = grant_refs_[size_t(sel)];
    if (g.exact)
        gate_.revoke_exact_allow(g.session_id, g.action, g.resource);
    else
        gate_.revoke_pattern_allow(g.session_id, g.action, g.resource);
    _RefreshGrantList();
}

void
PermissionsCenterWindow::_RevokeAll()
{
    if (selected_session_.empty()) return;
    gate_.revoke_temporary_allows(selected_session_);
    _RefreshGrantList();
}

// ---- Policies tab ----

void
PermissionsCenterWindow::_ReloadPolicyDocs()
{
    // Unsaved edits win: never clobber the working copy from disk.
    if (rules_dirty_) return;
    global_doc_ = haicode::load_permission_document(global_policy_path_);
    project_doc_ = haicode::load_permission_document(project_policy_path_);
}

void
PermissionsCenterWindow::_RefreshRulesList()
{
    const auto& doc = project_source_ ? project_doc_ : global_doc_;
    if (!rules_dirty_) rules_shown_ = doc.rules;

    policy_path_view_->SetText(doc.path.c_str());
    rules_list_->MakeEmpty();
    int i = 1;
    for (const auto& r : rules_shown_) {
        std::string label = std::to_string(i++) + ". "
                          + r.action + "  " + short_target(r.resource, 44)
                          + "  " + effect_label(r.effect);
        rules_list_->AddItem(new BStringItem(label.c_str()));
    }
}

void
PermissionsCenterWindow::_SavePolicy()
{
    const auto& doc = project_source_ ? project_doc_ : global_doc_;
    std::string err;
    if (!haicode::save_permission_document(doc.path, rules_shown_,
                                           doc.fingerprint, err)) {
        BAlert* a = new BAlert("Save failed", err.c_str(), "OK");
        a->Go();
        return;
    }
    rules_dirty_ = false;
    // The app reloads config + gate rules from disk (MSG_PERM_POLICY_SAVED);
    // refresh our documents so the new fingerprints are current.
    notify_target_.SendMessage(MSG_PERM_POLICY_SAVED);
    _ReloadPolicyDocs();
    _RefreshRulesList();
}

void
PermissionsCenterWindow::_OpenRuleEditor(int index)
{
    haicode::PermissionRule rule{"", "*",
                                 haicode::PermissionEffect::Ask};
    if (index >= 0 && size_t(index) < rules_shown_.size())
        rule = rules_shown_[size_t(index)];
    auto* win = new PermissionRuleEditWindow(BMessenger(this), rule, index);
    win->Show();
}

void
PermissionsCenterWindow::_ApplyRuleEdit(BMessage* msg)
{
    const char* action = nullptr;
    const char* resource = nullptr;
    const char* effect = nullptr;
    int32 index = -1;
    msg->FindString("action", &action);
    msg->FindString("resource", &resource);
    msg->FindString("effect", &effect);
    msg->FindInt32("index", &index);
    if (!action || !*action || !resource) return;

    haicode::PermissionRule rule{action, resource,
                                 haicode::PermissionEffect::Ask};
    if (effect && std::string(effect) == "allow")
        rule.effect = haicode::PermissionEffect::Allow;
    else if (effect && std::string(effect) == "deny")
        rule.effect = haicode::PermissionEffect::Deny;

    if (index >= 0 && size_t(index) < rules_shown_.size())
        rules_shown_[size_t(index)] = rule;
    else
        rules_shown_.push_back(rule);
    rules_dirty_ = true;
    _RefreshRulesList();
}

void
PermissionsCenterWindow::_RunInspector()
{
    std::string tool = inspect_tool_->Text() ? inspect_tool_->Text() : "";
    std::string input_s = inspect_input_->Text() ? inspect_input_->Text() : "";
    auto input = nlohmann::json::parse(input_s, nullptr, false);

    haicode::ToolContext ctx;
    ctx.session_id = selected_session_;
    ctx.tool_name = tool;
    ctx.working_dir = project_dir_;
    if (auto* engine = engine_of_())
        ctx.mode = engine->get_mode(selected_session_);

    auto d = tools_.evaluate(tool, input.is_discarded()
            ? nlohmann::json::object() : input, ctx, gate_);
    std::string out = std::string("Effect: ")
        + (d.effect == haicode::PermissionEffect::Allow ? "Allow"
         : d.effect == haicode::PermissionEffect::Deny  ? "Deny" : "Ask")
        + "\nSource: " + d.source
        + (d.rule_index >= 0
            ? " (rule " + std::to_string(d.rule_index + 1) + ")" : "")
        + "\nReason: " + d.reason;
    inspect_result_->SetText(out.c_str());
}

// ---- Activity tab ----

void
PermissionsCenterWindow::_RefreshActivityList()
{
    activity_list_->MakeEmpty();
    if (selected_session_.empty()) {
        activity_list_->AddItem(new BStringItem("(no session selected)"));
        return;
    }
    auto es = activity_.entries(selected_session_);
    if (es.empty()) {
        activity_list_->AddItem(
            new BStringItem("(nothing recorded since HaiCode started)"));
        return;
    }
    for (const auto& e : es) {
        time_t sec = time_t(e.timestamp_ms / 1000);
        struct tm tmv;
        localtime_r(&sec, &tmv);
        char when[16];
        strftime(when, sizeof(when), "%H:%M:%S", &tmv);
        std::string label = std::string(when) + "  " + e.result
                          + "  " + e.tool_name + "  "
                          + short_target(e.target, 40)
                          + "  — " + e.reason;
        activity_list_->AddItem(new BStringItem(label.c_str()));
    }
}

// ---- message dispatch ----

void
PermissionsCenterWindow::MessageReceived(BMessage* msg)
{
    switch (msg->what) {
        case MSG_PERM_SESSION_SELECTED: {
            const char* sid = nullptr;
            if (msg->FindString("session_id", &sid) == B_OK && sid)
                _SelectSession(sid);
            break;
        }
        case MSG_PERM_TOGGLE_AUTO:
        case MSG_PERM_TOGGLE_READ:
        case MSG_PERM_TOGGLE_BYPASS:
            _OnToggle(msg->what, msg->GetInt32("be:value", 0) == B_CONTROL_ON);
            break;
        case MSG_PERM_PENDING_ALLOW:
            _ApprovePending(true);
            break;
        case MSG_PERM_PENDING_DENY:
            _ApprovePending(false);
            break;
        case MSG_PERM_REVOKE:
            _RevokeSelected();
            break;
        case MSG_PERM_REVOKE_ALL:
            _RevokeAll();
            break;
        case MSG_PERM_REFRESH:
            Refresh();
            break;
        case MSG_PERM_ACTIVATE:
            Activate();
            break;
        case kPolicySourceSelected: {
            bool project = false;
            msg->FindBool("project", &project);
            project_source_ = project;
            _RefreshRulesList();
            break;
        }
        case kBtnRuleAdd:
            _OpenRuleEditor(-1);
            break;
        case kBtnRuleEdit: {
            int sel = rules_list_->CurrentSelection();
            if (sel >= 0) _OpenRuleEditor(sel);
            break;
        }
        case kBtnRuleRemove: {
            int sel = rules_list_->CurrentSelection();
            if (sel >= 0 && size_t(sel) < rules_shown_.size()) {
                rules_shown_.erase(rules_shown_.begin() + sel);
                rules_dirty_ = true;
                _RefreshRulesList();
            }
            break;
        }
        case kBtnRuleUp:
        case kBtnRuleDown: {
            int sel = rules_list_->CurrentSelection();
            if (sel < 0) break;
            int target = (msg->what == kBtnRuleUp) ? sel - 1 : sel + 1;
            if (target < 0 || size_t(target) >= rules_shown_.size()) break;
            std::swap(rules_shown_[size_t(sel)], rules_shown_[size_t(target)]);
            rules_dirty_ = true;
            _RefreshRulesList();
            rules_list_->Select(target);
            break;
        }
        case kBtnPolicySave:
            _SavePolicy();
            break;
        case MSG_PERM_RULE_DONE:
            _ApplyRuleEdit(msg);
            break;
        case kBtnInspect:
            _RunInspector();
            break;
        default:
            BWindow::MessageReceived(msg);
            break;
    }
}

bool
PermissionsCenterWindow::QuitRequested()
{
    notify_target_.SendMessage(MSG_PERMISSION_CENTER_CLOSED);
    return true;
}
