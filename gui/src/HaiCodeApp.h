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
};
