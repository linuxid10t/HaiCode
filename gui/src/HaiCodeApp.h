#pragma once

#include <Application.h>

#include <haicode/haicode.h>
#include <haicode/db.h>
#include <haicode/engine.h>
#include <haicode/events.h>
#include <haicode/provider.h>
#include <haicode/tool.h>
#include <haicode/config.h>

#include "MainWindow.h"
#include "GuiEventRelay.h"

#include <string>
#include <map>
#include <memory>

class HaiCodeApp : public BApplication {
public:
    HaiCodeApp(int argc, char* argv[]);

    void ReadyToRun() override;
    void MessageReceived(BMessage* msg) override;
    bool QuitRequested() override;

private:
    // Owned haicode objects
    std::unique_ptr<haicode::Database>        db_;
    std::unique_ptr<haicode::SessionStore>    store_;
    std::unique_ptr<haicode::ProviderRegistry> providers_;
    std::unique_ptr<haicode::ToolRegistry>    tools_;
    std::unique_ptr<haicode::PermissionGate>  perm_gate_;
    std::unique_ptr<haicode::SessionEventBus> bus_;
    std::unique_ptr<haicode::SessionEngine>   engine_;
    std::unique_ptr<GuiEventRelay>            relay_;

    // Not owned (owned by BLooper after Show())
    MainWindow* main_window_ = nullptr;

    haicode::AppConfig config_;
    std::string project_dir_;

    // Shared holder so the permission callback can capture MainWindow*
    // safely even if the callback outlives ReadyToRun scope
    std::shared_ptr<MainWindow*> window_holder_;

    // Session-scoped permission state. Each session's toolbar-toggle flags
    // are tracked independently and pushed into the PermissionGate under
    // that session's id, so switching the selected session never changes
    // the rules a background session runs under. Allow-Always grants live
    // in the gate's per-session store, scoped by the same id.
    struct SessionFlags {
        bool auto_edits       = false;
        bool yolo             = false;
        bool read_everywhere  = false;
    };
    std::map<std::string, SessionFlags> session_flags_;
    // Resolve the session a permission-related message targets: the message's
    // "session_id" when present, else the currently selected session.
    std::string _TargetSession(const BMessage* msg) const;
    void _ApplySessionRules(const std::string& session_id);

    // Stop the current engine (interrupt + cancel pending asks + destroy,
    // which joins the agentic-loop threads) and construct a fresh one from
    // the current config_. Returns the new engine. Used by MSG_SETTINGS_SAVED
    // and MSG_DIR_CHANGED; must run before providers_ is mutated.
    haicode::SessionEngine* _RecreateEngine();
};
