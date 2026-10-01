#pragma once

#include <Application.h>

#include <haicode/haicode.h>
#include <haicode/db.h>
#include <haicode/engine.h>
#include <haicode/events.h>
#include <haicode/provider.h>
#include <haicode/tool.h>
#include <haicode/config.h>
#include <haicode/permission_requests.h>

#include "MainWindow.h"
#include "GuiEventRelay.h"
#include "PermissionsCenterWindow.h"

#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class HaiCodeApp : public BApplication {
public:
    HaiCodeApp(int argc, char* argv[]);

    void ReadyToRun() override;
    void MessageReceived(BMessage* msg) override;
    bool QuitRequested() override;
    // B_SINGLE_LAUNCH routes a second launch's argv/refs to the running
    // instance instead of starting a new process. Forward a directory
    // argument (or a Tracker "Open With" ref) to MainWindow's existing
    // B_REFS_RECEIVED handler, which switches the project directory.
    void ArgvReceived(int32 argc, char** argv) override;
    void RefsReceived(BMessage* msg) override;

private:
    // Owned haicode objects
    std::unique_ptr<haicode::Database>        db_;
    std::unique_ptr<haicode::SessionStore>    store_;
    std::unique_ptr<haicode::ProviderRegistry> providers_;
    std::unique_ptr<haicode::ToolRegistry>    tools_;
    std::unique_ptr<haicode::PermissionGate>  perm_gate_;
    std::unique_ptr<haicode::PermissionRequestBroker> perm_broker_;
    std::unique_ptr<haicode::SessionEventBus> bus_;
    std::unique_ptr<haicode::SessionEngine>   engine_;
    std::unique_ptr<GuiEventRelay>            relay_;

    // Not owned (owned by BLooper after Show())
    MainWindow* main_window_ = nullptr;
    // One reusable Permissions center; pointer cleared on close.
    PermissionsCenterWindow* perm_center_ = nullptr;
    // Authorization outcomes since launch (bounded, in-memory).
    PermissionActivityLog perm_activity_;
    // Session-deletion workers (tracked; joined in QuitRequested and by
    // _JoinLifecycleWorkers before any engine shutdown).
    std::mutex lifecycle_mu_;
    std::vector<std::thread> lifecycle_workers_;

    haicode::AppConfig config_;
    // The global config LAYER (values + key presence), kept separate from the
    // merged `config_` the engine/UI read. Settings saves edit this layer and
    // persist only its global-scope keys to the global file — the merged
    // config's project-owned values (build_command, project providers, ...)
    // must never leak into it.
    haicode::ConfigLayer global_layer_;
    // Trust state of the open project's gated config keys, as of the last
    // _SyncMergedConfig() (pre-strip). Drives the trust prompt.
    haicode::ProjectTrust last_trust_;
    // Re-merge global_layer_ + the project layer into config_ and push the
    // refreshed permission rules to the gate.
    void _SyncMergedConfig();
    // If the open project carries gated config keys (permissions, build
    // command, agents, search API keys) without a matching trust record,
    // prompt; "Trust" stores the record and re-merges so they take effect.
    void _MaybePromptProjectTrust();
    std::string project_dir_;

    // Shared holder so the broker's delivery callback can capture MainWindow*
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
    // Load a session's persisted toggle flags into session_flags_ and apply
    // them (used on session switch; replaces the checkbox-restore posts).
    void _SyncSessionFlags(const std::string& session_id);
    // Mirror a session's toggle flags into its model_json so they survive
    // restarts (same format the old toolbar checkboxes wrote).
    void _PersistSessionFlags(const std::string& session_id);
    // Push permission status (flags + pending counts) to MainWindow and
    // refresh an open Permissions center.
    void _NotifyPermissionUiChanged();
    void _ShowPermissionsCenter();
    void _RefreshPermissionsCenter();
    bool _ApplyProviders(const BMessage* msg);
    void _RefreshProviders();
    // Warn-and-ask before a change that would interrupt every running
    // session (settings save, provider update, directory switch). Lists ALL
    // running sessions, including background ones. Returns true only for the
    // explicit "Interrupt and apply" choice; Cancel leaves app, window, DB,
    // and config files untouched.
    bool _ConfirmDisruptiveChange(const char* what);

    // Retiring/join/delete work for session deletion runs on tracked worker
    // threads (never the looper — joins can wait on network teardown) and
    // posts MSG_SESSION_DELETED back. The workers reference engine_, so they
    // are joined before any engine shutdown/replacement.
    void _JoinLifecycleWorkers();

    // Replace the engine after handing the window its new pointer. When supplied,
    // keep the old provider registry alive until the retiring engine is destroyed.
    void _RecreateEngine(std::unique_ptr<haicode::ProviderRegistry> next_providers = nullptr);
};
