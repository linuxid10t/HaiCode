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
#include <Messenger.h>
#include <FilePanel.h>

#include "ChatView.h"

#include <haicode/engine.h>
#include <haicode/db.h>
#include <haicode/types.h>

#include <string>
#include <vector>
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
    // Packs action/resource/detail/promise_ptr into MSG_PERMISSION_REQ and posts to self
    void PostPermissionRequest(const std::string& action,
                               const std::string& resource,
                               const std::string& detail,
                               void* promise_ptr);

    // Called by HaiCodeApp after recreating the engine (e.g. settings change)
    void SetEngine(haicode::SessionEngine& engine) { engine_ = &engine; }

    // Mark the provider dropdown to match the given provider id ("anthropic" or "openai")
    void SelectProvider(const std::string& provider_id);

private:
    void _NewSession();
    void _SelectSession(int idx);   // loads session content (no list widget interaction)
    void _SwitchToSession(int idx); // loads content + updates list selection (suppressed)
    void _SubmitPrompt();
    void _LoadHistory(const std::string& session_id);
    void _RefreshSessionList();

    void _HandleTextDelta(BMessage* msg);
    void _HandleToolCalled(BMessage* msg);
    void _HandleToolResult(BMessage* msg);
    void _HandleStepStarted();
    void _HandleStepEnded(BMessage* msg);
    void _HandleStepFailed(BMessage* msg);
    void _HandlePermissionReq(BMessage* msg);
    void _HandlePlanProposed(BMessage* msg);
    void _HandlePlanDecision(BMessage* msg);
    void _HandleTodosUpdated(BMessage* msg);
    void _RefreshTodosFromEngine();

    void _ToggleMode();
    void _UpdateStatusStrip();
    void _UpdateMaxContext();
    void _RefreshModeButton();
    void _ApplyProviderModelToActiveSession();
    void _PersistProviderModel();

    // Engine & store (not owned — owned by HaiCodeApp)
    haicode::SessionEngine* engine_;  // pointer so HaiCodeApp can swap it on settings change
    haicode::SessionStore&  store_;
    std::string             project_dir_;
    std::string             default_model_;
    std::string             default_provider_ = "anthropic";
    std::string             active_session_id_;

    // Session list (parallel to UI list)
    std::vector<std::string> session_ids_;  // indexed to match BListView
    int suppress_next_select_ = 0;     // suppress MSG_SELECT_SESSION from programmatic Select() — counter because Select() may fire multiple notifications

    // UI widgets (owned by BLooper)
    BListView*     session_list_    = nullptr;
    BScrollView*   session_scroll_  = nullptr;
    ChatView*      chat_view_       = nullptr;
    InputTextView* input_view_      = nullptr;
    BButton*       send_btn_        = nullptr;
    BButton*       interrupt_btn_   = nullptr;
    BButton*       new_session_btn_ = nullptr;
    BButton*       dir_btn_         = nullptr;
    BButton*       mode_btn_        = nullptr;
    BFilePanel*    dir_panel_       = nullptr;
    BMenuField*    model_field_     = nullptr;
    BPopUpMenu*    model_menu_      = nullptr;
    BMenuField*    provider_field_  = nullptr;
    BPopUpMenu*    provider_menu_   = nullptr;
    BMenuBar*      menu_bar_        = nullptr;
    BStringView*   status_strip_    = nullptr;

    // Todos side panel
    BStringView*   todos_header_    = nullptr;
    BListView*     todos_list_      = nullptr;
    BScrollView*   todos_scroll_    = nullptr;

    // Engine state mirror for UI
    bool           engine_running_        = false;
    std::string    streaming_state_ = "idle";  // idle|thinking|streaming|tool
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
