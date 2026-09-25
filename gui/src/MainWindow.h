#pragma once

#include <Window.h>
#include <ListView.h>
#include <MenuBar.h>
#include <ScrollView.h>
#include <Button.h>
#include <MenuField.h>
#include <PopUpMenu.h>
#include <TextView.h>
#include <StringView.h>
#include <GroupView.h>
#include <Messenger.h>
#include <FilePanel.h>
#include <CheckBox.h>
#include <TabView.h>
#include <TextControl.h>

#include "ChatView.h"

#include <haicode/engine.h>
#include <haicode/db.h>
#include <haicode/types.h>

#include <string>
#include <array>
#include <vector>
#include <map>
#include <future>
#include <memory>

// Forward declaration
class InputTextView;

class MainWindow : public BWindow {
public:
    MainWindow(haicode::SessionEngine& engine,
               haicode::SessionStore& store,
               const std::string& project_dir,
               const std::string& default_model,
               const std::string& default_provider);

    void MessageReceived(BMessage* msg) override;
    bool QuitRequested() override;

    // Called by HaiCodeApp after construction, so GuiEventRelay knows it
    BMessenger Messenger() const { return BMessenger(this); }

    // The active session id (used by GuiEventRelay)
    std::string active_session_id() const { return active_session_id_; }

    // Called by HaiCodeApp's permission callback (from any thread via PostMessage)
    // Packs session_id/action/resource/detail/promise_ptr into
    // MSG_PERMISSION_REQ and posts to self
    void PostPermissionRequest(const std::string& session_id,
                               const std::string& action,
                               const std::string& resource,
                               const std::string& detail,
                               void* promise_ptr);

    // Called by HaiCodeApp while holding the window lock.
    void SetEngine(haicode::SessionEngine& engine);

    // Mark the provider dropdown to match the given provider id.
    void SelectProvider(const std::string& provider_id);

    // Rebuild the provider dropdown from a config providers map. Preserves the
    // current selection if its id is still present; otherwise marks the first.
    void RebuildProviderMenu(const std::map<std::string, haicode::ProviderConfig>& providers);

private:
    void _NewSession();
    void _SelectSession(int idx);   // loads session content (no list widget interaction)
    void _SwitchToSession(int idx); // loads content + updates list selection (suppressed)
    void _SubmitPrompt();
    void _HandleRetryCommand();
    void _LoadHistory(const std::string& session_id);
    void _RestoreSessionTotals(const std::string& session_id);
    void _RefreshSessionList();

    void _HandleTextDelta(BMessage* msg);
    void _HandleReasoningDelta(BMessage* msg);
    void _HandleToolCalled(BMessage* msg);
    void _HandleToolResult(BMessage* msg);
    void _HandleStepStarted();
    void _HandleStepEnded(BMessage* msg);
    void _HandleStepFailed(BMessage* msg);
    void _HandleInterrupted();
    void _HandlePermissionReq(BMessage* msg);
    void _HandlePlanProposed(BMessage* msg);
    void _HandlePlanDecision(BMessage* msg);
    void _HandleAskUserReq(BMessage* msg);
    void _HandleAskUserReply(BMessage* msg);
    void _HandleTodosUpdated(BMessage* msg);
    void _HandleBuildHookResult(BMessage* msg);
    void _HandleCompaction(BMessage* msg);
    void _HandleCompactNow();
    void _RefreshTodosFromEngine();

    void _SetMode(haicode::SessionMode next);
    void _ApplyModeCheckboxVisibility(bool reset_hidden);
    void _UpdateStatusStrip();
    void _UpdateMaxContext();
    // Push config_.thinking_display (via the engine) into ChatView.
    void _ApplyThinkingDisplay();
    void _RefreshModeButton();
    void _FetchModels();                  // reset model dropdown to (loading…) and forward to be_app
    void _ApplyProviderModelToActiveSession();
    void _PersistProviderModel();
    void _ApplyInference();       // Inference tab Apply button
    haicode::InferenceParams _LoadInference() const;
    std::string _SelectedReasoningEffort() const;
    void _RestoreInference();     // read model_json → fill inference fields
    void _RestoreInferenceFrom(const haicode::InferenceParams& p);
    std::array<std::string, 4> _InferenceFields() const;
    void _UpdateInferenceDirty();

    // Skills tab: rebuild the checkbox list from disk (list_skills) and
    // mirror the active session's enabled set onto it.
    void _RefreshSkills();

    // Attachments
    void _OpenAttachPanel();
    bool _VisionAvailable();
    void _HandleAttachRefs(BMessage* msg);
    void _NotifyAttachmentLimit(const std::string& note);
    void _RemoveAttachment(int32 index);
    void _RebuildAttachRow();

    // Per-session drafts: input text + staged attachments travel with their
    // session across switches.
    void _SaveActiveDraft();
    void _RestoreDraft(const std::string& session_id);
    struct SessionDraft {
        std::string input_text;
        std::vector<std::pair<std::string, std::string>> attachments;
    };
    std::map<std::string, SessionDraft> session_drafts_;

    // Engine & store (not owned — owned by HaiCodeApp)
    haicode::SessionEngine* engine_;  // pointer so HaiCodeApp can swap it on settings change
    haicode::SessionStore&  store_;
    std::string             project_dir_;
    std::string             default_model_;
    std::string             default_provider_ = "anthropic";
    std::string             active_session_id_;
    std::string             pending_plan_path_;

    // Session list (parallel to UI list)
    std::vector<std::string> session_ids_;  // indexed to match BListView

    // UI widgets (owned by BLooper)
    BListView*     session_list_    = nullptr;
    BScrollView*   session_scroll_  = nullptr;
    ChatView*      chat_view_       = nullptr;
    InputTextView* input_view_      = nullptr;
    BButton*       send_btn_        = nullptr;
    BButton*       attach_btn_      = nullptr;
    BButton*       interrupt_btn_   = nullptr;
    BButton*       new_session_btn_ = nullptr;
    BButton*       dir_btn_         = nullptr;
    BGroupView*    dir_slot_        = nullptr;   // keeps toolbar flow stable when the button hides
    BPopUpMenu*    mode_menu_       = nullptr;
    BMenuField*    mode_field_      = nullptr;
    BButton*       compact_btn_     = nullptr;
    BCheckBox*     auto_edits_chk_  = nullptr;
    BCheckBox*     yolo_chk_        = nullptr;
    BCheckBox*     read_everywhere_chk_ = nullptr;
    // Desired-visibility mirror for mode-dependent widgets. BView::IsHidden()
    // is true for every view while the window is not yet shown, so it cannot
    // gate Hide()/Show() during the pre-Show() startup mode restore; these
    // tracked bools can. Must match the constructor's initial Hide() calls.
    bool dir_btn_visible_             = true;
    bool auto_edits_chk_visible_      = true;
    bool yolo_chk_visible_            = true;
    bool read_everywhere_chk_visible_ = false;
    void _SetWidgetVisible(BView* v, bool& tracked, bool visible);
    // Hides/shows dir_btn_ while pinning its slot's width so the rest of the
    // toolbar (Provider:/Model:/Mode:) never shifts when it disappears.
    void _SetDirBtnVisible(bool visible);
    BFilePanel*    dir_panel_       = nullptr;
    BFilePanel*    attach_panel_    = nullptr;
    BGroupView*    attach_row_      = nullptr;   // removable attachment chips
    // path + media_type of images and text files staged for the next prompt
    // (engine reads and base64-encodes the files at submit time; kind is
    // derived from the media type)
    std::vector<std::pair<std::string, std::string>> pending_attachments_;
    BMenuField*    model_field_     = nullptr;
    BPopUpMenu*    model_menu_      = nullptr;
    BMenuField*    inf_effort_field_ = nullptr;
    BPopUpMenu*    inf_effort_menu_  = nullptr;
    BMenuField*    provider_field_  = nullptr;
    BPopUpMenu*    provider_menu_   = nullptr;
    BMenuBar*      menu_bar_        = nullptr;
    BStringView*   status_strip_    = nullptr;

    // Todos side panel
    BStringView*   todos_header_    = nullptr;
    BListView*     todos_list_      = nullptr;
    BScrollView*   todos_scroll_    = nullptr;

    // Skills side panel (4th tab): BListView rows "[ ] name"/"[x] name"
    // (Todos style). Clicking a row toggles that skill for the active
    // session (persisted via store_.update_skills).
    BStringView*   skills_header_   = nullptr;
    BListView*     skills_list_     = nullptr;
    BScrollView*   skills_scroll_   = nullptr;
    std::vector<std::string> skill_ids_;      // parallel to list rows
    std::vector<std::string> skill_names_;    // display names
    std::vector<bool>        skill_enabled_;  // checkbox marks

    // Left-hand tabbed panel (Sessions / Inference / Todos)
    BTabView*      side_tabs_       = nullptr;

    // Inference tab controls
    BTextControl*  inf_max_tokens_   = nullptr;
    BTextControl*  inf_temperature_  = nullptr;
    BTextControl*  inf_top_p_        = nullptr;
    BTextControl*  inf_max_steps_    = nullptr;
    BButton*       inf_apply_btn_    = nullptr;
    std::array<std::string, 4> inf_saved_fields_;

    // Engine state mirror for UI
    bool           engine_running_        = false;
    bool           models_load_failed_    = false;  // last model fetch errored; dropdown click re-fetches
    bool           compacting_            = false;
    int            compaction_progress_   = -1;  // 0-99 while compacting; -1 = unknown
    std::string    streaming_state_ = "idle";  // idle|thinking|streaming|tool|compacting
    std::string    current_tool_name_;

    // Per-prompt / per-session token accounting
    int            last_prompt_input_     = 0;
    int            last_prompt_output_    = 0;
    int            session_input_total_   = 0;
    int            session_output_total_  = 0;
    int            current_context_tokens_ = 0;
    int            max_context_           = 0;
    double         session_cost_          = 0.0;
};
