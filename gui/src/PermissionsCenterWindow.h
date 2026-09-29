#pragma once

#include <Window.h>
#include <Messenger.h>

#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <haicode/config.h>
#include <haicode/engine.h>
#include <haicode/permission_requests.h>
#include <haicode/tool.h>

class BButton;
class BCheckBox;
class BListView;
class BMenuField;
class BPopUpMenu;
class BStringView;
class BTabView;
class BTextControl;
class BTextView;
class BScrollView;

// One authorization outcome for the activity list. Deliberately carries no
// raw input payloads or file contents — just what happened and why.
struct PermissionActivityEntry {
    int64_t timestamp_ms = 0;
    std::string session_id;
    std::string tool_name;
    std::string target;       // short resource summary
    std::string result;       // "allowed", "denied", "pending", "cancelled"
    std::string reason;       // rule source / user decision / block cause
};

// Bounded in-memory recorder of authorization outcomes, fed by the broker's
// notify callback (any thread) and read by the Permissions center. Entries
// exist only since application launch.
class PermissionActivityLog {
public:
    void add(const PermissionActivityEntry& e);
    std::vector<PermissionActivityEntry> entries(const std::string& session_id,
                                                 size_t limit = 200) const;
    size_t count(const std::string& session_id) const;

private:
    static constexpr size_t kMaxPerSession = 200;
    mutable std::mutex mu_;
    std::map<std::string, std::deque<PermissionActivityEntry>> per_session_;
};

// Modeless, resizable Permissions center: Session (controls, pending queue,
// grants), Policies (persistent rule editor + operation inspector),
// Activity. One instance per application; HaiCodeApp owns everything it
// references for the whole app lifetime (engine accessor is a stable
// function since the engine object is recreatable).
class PermissionsCenterWindow : public BWindow {
public:
    PermissionsCenterWindow(haicode::PermissionGate& gate,
                            haicode::PermissionRequestBroker& broker,
                            haicode::ToolRegistry& tools,
                            haicode::SessionStore& store,
                            PermissionActivityLog& activity,
                            std::function<haicode::SessionEngine*()> engine_of,
                            const std::string& initial_session,
                            const std::string& global_policy_path,
                            const std::string& project_policy_path,
                            const std::string& project_dir,
                            BMessenger notify_target);
    ~PermissionsCenterWindow();

    void MessageReceived(BMessage* msg) override;
    bool QuitRequested() override;

    // Re-read everything (session list, grants, policy, activity). Invoked
    // via MSG_PERM_REFRESH from HaiCodeApp; never called cross-thread.
    void Refresh();

private:
    // Session tab
    void _RebuildSessionMenu();
    void _SelectSession(const std::string& session_id);
    void _RefreshSessionTab();
    void _RefreshPendingList();
    void _RefreshGrantList();
    void _OnToggle(uint32 what, bool on);
    void _ApprovePending(bool allow);
    void _RevokeSelected();
    void _RevokeAll();

    // Policies tab
    void _ReloadPolicyDocs();   // disk → working copy (kept when dirty)
    void _RefreshRulesList();   // working copy → list UI
    void _SavePolicy();
    void _OpenRuleEditor(int index);   // -1 = append
    void _ApplyRuleEdit(BMessage* msg);
    void _RunInspector();

    // Activity tab
    void _RefreshActivityList();

    haicode::PermissionGate& gate_;
    haicode::PermissionRequestBroker& broker_;
    haicode::ToolRegistry& tools_;
    haicode::SessionStore& store_;
    PermissionActivityLog& activity_;
    std::function<haicode::SessionEngine*()> engine_of_;
    std::string global_policy_path_;
    std::string project_policy_path_;
    std::string project_dir_;
    BMessenger notify_target_;

    std::string selected_session_;
    bool project_source_ = false;   // Policies tab: which source is shown

    // Session tab widgets
    BMenuField* session_field_ = nullptr;
    BPopUpMenu* session_menu_ = nullptr;
    BStringView* mode_view_ = nullptr;
    BStringView* dir_view_ = nullptr;
    BCheckBox* auto_writes_chk_ = nullptr;
    BCheckBox* read_outside_chk_ = nullptr;
    BCheckBox* bypass_chk_ = nullptr;
    BStringView* toggle_note_ = nullptr;
    BListView* pending_list_ = nullptr;
    BButton* pending_allow_btn_ = nullptr;
    BButton* pending_deny_btn_ = nullptr;
    BListView* grant_list_ = nullptr;
    BButton* revoke_btn_ = nullptr;
    BButton* revoke_all_btn_ = nullptr;
    BStringView* grant_note_ = nullptr;
    BListView* activity_list_ = nullptr;

    // Policies tab widgets
    BMenuField* policy_source_field_ = nullptr;
    BPopUpMenu* policy_source_menu_ = nullptr;
    BListView* rules_list_ = nullptr;
    BStringView* policy_path_view_ = nullptr;
    BStringView* policy_note_ = nullptr;
    BButton* rule_add_btn_ = nullptr;
    BButton* rule_edit_btn_ = nullptr;
    BButton* rule_remove_btn_ = nullptr;
    BButton* rule_up_btn_ = nullptr;
    BButton* rule_down_btn_ = nullptr;
    BButton* policy_save_btn_ = nullptr;
    BTextControl* inspect_tool_ = nullptr;
    BTextControl* inspect_input_ = nullptr;
    BTextView* inspect_result_ = nullptr;

    // Loaded policy documents (source-separated) for conflict detection.
    haicode::PermissionPolicyDocument global_doc_;
    haicode::PermissionPolicyDocument project_doc_;

    // Parallel to grant_list_ rows: the grant each row revokes.
    struct GrantRef {
        std::string session_id;
        std::string action;
        std::string resource;
        bool exact;   // true: exact grant; false: legacy pattern grant
    };
    std::vector<GrantRef> grant_refs_;
    // Parallel to pending_list_ rows.
    std::vector<haicode::PermissionRequest> pending_reqs_;
    // Parallel to rules_list_ rows (current source only).
    std::vector<haicode::PermissionRule> rules_shown_;
    // True while the working copy has unsaved edits: Refresh() then keeps
    // the edits instead of clobbering them from disk.
    bool rules_dirty_ = false;
};
