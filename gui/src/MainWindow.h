#pragma once

#include <Window.h>
#include <ListView.h>
#include <MenuBar.h>
#include <ScrollView.h>
#include <Button.h>
#include <MenuField.h>
#include <PopUpMenu.h>
#include <TextView.h>
#include <Messenger.h>
#include <FilePanel.h>

#include "ChatView.h"

#include <haicode/engine.h>
#include <haicode/db.h>

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
               const std::string& default_model);

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
    void _HandleStepEnded(BMessage* msg);
    void _HandleStepFailed(BMessage* msg);
    void _HandlePermissionReq(BMessage* msg);

    // Engine & store (not owned — owned by HaiCodeApp)
    haicode::SessionEngine* engine_;  // pointer so HaiCodeApp can swap it on settings change
    haicode::SessionStore&  store_;
    std::string             project_dir_;
    std::string             default_model_;
    std::string             active_session_id_;

    // Session list (parallel to UI list)
    std::vector<std::string> session_ids_;  // indexed to match BListView
    bool suppress_next_select_ = false;     // suppress MSG_SELECT_SESSION from programmatic Select()

    // UI widgets (owned by BLooper)
    BListView*     session_list_    = nullptr;
    BScrollView*   session_scroll_  = nullptr;
    ChatView*      chat_view_       = nullptr;
    InputTextView* input_view_      = nullptr;
    BButton*       send_btn_        = nullptr;
    BButton*       interrupt_btn_   = nullptr;
    BButton*       new_session_btn_ = nullptr;
    BButton*       dir_btn_         = nullptr;
    BFilePanel*    dir_panel_       = nullptr;
    BMenuField*    model_field_     = nullptr;
    BPopUpMenu*    model_menu_      = nullptr;
    BMenuField*    provider_field_  = nullptr;
    BPopUpMenu*    provider_menu_   = nullptr;
    BMenuBar*      menu_bar_        = nullptr;
};
