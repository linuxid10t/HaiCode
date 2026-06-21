// main.cpp — haicode TUI entry point
//
// Boot sequence:
//   1. Locate user settings directory via Haiku find_directory()
//   2. Open SQLite database + run migrations
//   3. Build all registry / config objects
//   4. Register Anthropic provider and built-in tools
//   5. Wire PermissionGate ask-callback → promise → bus event → TuiApp
//   6. Create SessionEngine + TuiApp, then call run()
//
// Pure POSIX + Haiku kernel APIs; no BeAPI (BApplication etc.)

#include "TuiApp.h"

#include <haicode/haicode.h>
#include <haicode/db.h>
#include <haicode/config.h>
#include <haicode/default_prompt.h>
#include <haicode/events.h>
#include <haicode/tool.h>
#include <haicode/provider.h>

// Haiku POSIX-compatible find_directory (C interface, no BApplication needed)
#include <FindDirectory.h>
#include <StorageDefs.h>   // B_PATH_NAME_LENGTH
#include <SupportDefs.h>   // status_t, B_OK

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace haicode;
using json = nlohmann::json;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helper: resolve the DB path under B_USER_SETTINGS_DIRECTORY
// ---------------------------------------------------------------------------

static std::string resolve_db_path() {
    char buf[B_PATH_NAME_LENGTH];
    status_t st = ::find_directory(B_USER_SETTINGS_DIRECTORY,
                                   /*volume=*/(dev_t)-1,
                                   /*createIt=*/true,
                                   buf, B_PATH_NAME_LENGTH);
    std::string base;
    if (st == B_OK) {
        base = buf;
    } else {
        const char* home = std::getenv("HOME");
        base = std::string(home ? home : "/boot/home") + "/.config";
    }
    fs::path dir = fs::path(base) / "haicode";
    std::error_code ec;
    fs::create_directories(dir, ec); // ignore error; DB open will catch it
    return (dir / "sessions.db").string();
}

// ---------------------------------------------------------------------------
// Helper: pick Anthropic API key (config > env var)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    // If not running inside a terminal (e.g. double-clicked in Tracker),
    // re-exec ourselves inside a Haiku Terminal window. execl replaces
    // this process so no fork is needed; if Terminal isn't found we fall
    // through and attempt to run normally.
    if (!isatty(STDIN_FILENO)) {
        std::string cmd = "'";
        for (char c : std::string(argv[0])) {
            if (c == '\'') cmd += "'\\''"; else cmd += c;
        }
        cmd += "'";
        for (int i = 1; i < argc; ++i) {
            cmd += " '";
            for (char c : std::string(argv[i])) {
                if (c == '\'') cmd += "'\\''"; else cmd += c;
            }
            cmd += "'";
        }
        execl("/boot/system/apps/Terminal", "Terminal",
              "/bin/sh", "-c", cmd.c_str(), nullptr);
    }

    // Project directory: first CLI arg, otherwise cwd
    std::string project_dir;
    if (argc >= 2 && argv[1][0] != '-') {
        project_dir = argv[1];
    } else {
        project_dir = fs::current_path().string();
    }

    // ------------------------------------------------------------------
    // 1. Database
    // ------------------------------------------------------------------
    std::string db_path = resolve_db_path();
    Database db(db_path);
    try {
        db.migrate();
    } catch (const std::exception& e) {
        std::cerr << "haicode: database migration failed: " << e.what() << "\n";
        return 1;
    }

    // ------------------------------------------------------------------
    // 2. Config
    // ------------------------------------------------------------------
    ConfigLoader config_loader;
    AppConfig config = config_loader.load(project_dir);

    // Offer to create agents.md if neither agents.md nor claude.md is present.
    {
        fs::path agents_md = fs::path(project_dir) / kAgentsMdFilename;
        fs::path claude_md = fs::path(project_dir) / kClaudeMdFilename;
        std::error_code ec;
        if (!fs::exists(agents_md, ec) && !fs::exists(claude_md, ec)) {
            std::cout << "No agents.md or claude.md found in " << project_dir
                      << ". Create agents.md with a starter template? [y/N] ";
            std::cout.flush();
            std::string line;
            if (!std::getline(std::cin, line)) line.clear();
            char c = line.empty() ? 'n' : static_cast<char>(std::tolower(line[0]));
            if (c == 'y') {
                std::ofstream out(agents_md.string());
                if (out.is_open()) {
                    out << kAgentsMdStarterTemplate;
                    out.close();
                    config.agents_md = kAgentsMdStarterTemplate;
                } else {
                    std::cerr << "haicode: could not write " << agents_md.string() << "\n";
                }
            }
        }
    }

    // Apply sane defaults
    if (config.model.empty()) config.model = "claude-opus-4-5";
    if (config.agent.empty()) config.agent = "default";

    // ------------------------------------------------------------------
    // 3. Core objects
    // ------------------------------------------------------------------
    SessionStore     store(db);
    ProviderRegistry providers;
    ToolRegistry     tools;
    SessionEventBus  bus;

    // ------------------------------------------------------------------
    // 4. Providers (generic: supports any number of Anthropic and
    //    OpenAI-compatible endpoints, distinguished by config `type`).
    // ------------------------------------------------------------------
    for (auto& [id, pcfg] : config.providers) {
        std::string key = pcfg.api_key;
        std::string type = pcfg.type.empty()
            ? (id == "anthropic" ? "anthropic" : "openai") : pcfg.type;
        if (key.empty() && id == "anthropic") {
            if (const char* e = std::getenv("ANTHROPIC_API_KEY"); e && *e) key = e;
        }
        if (key.empty() && id == "openai") {
            if (const char* e = std::getenv("OPENAI_API_KEY"); e && *e) key = e;
        }
        if (key.empty() && pcfg.base_url.empty() && type == "anthropic") continue;
        if (type == "anthropic") {
            providers.register_provider(make_anthropic_provider(key, pcfg.base_url, id));
        } else if (type == "ollama" || type == "vllm" || type == "openrouter"
                   || type == "lmstudio" || type == "llamacpp") {
            providers.register_provider(
                make_openai_compat_provider(key, pcfg.base_url, id, type));
        } else {
            providers.register_provider(make_openai_provider(key, pcfg.base_url, id));
        }
    }

    // Default model unconditionally — providers may be added later via config
    if (config.model.empty())
        config.model = "claude-opus-4-5";

    // ------------------------------------------------------------------
    // 5. Built-in tools
    // ------------------------------------------------------------------
    register_builtin_tools(tools);

    // ------------------------------------------------------------------
    // 6. Permission gate
    //
    //    The ask-callback is invoked on engine background threads.
    //    Strategy:
    //      a) Allocate a heap PendingPermission with a std::promise.
    //      b) Publish a PermissionRequested event on the bus, carrying the
    //         raw pointer to the PendingPermission as a JSON uintptr_t.
    //      c) The TuiApp's bus subscription (see subscribe_events) decodes
    //         the pointer and calls push_engine_event(PermissionReq), which
    //         wakes the main ncurses loop via the wake pipe.
    //      d) The main loop renders the overlay and resolves the promise.
    //      e) This thread unblocks on future.get() and returns the effect.
    //
    //    We store a raw pointer to the TuiApp so the callback can call
    //    push_engine_event directly as a fallback if the bus path fails.
    //    The pointer is set to null initially and updated after TuiApp is
    //    constructed.  A shared_ptr<TuiApp*> gives a stable address.
    // ------------------------------------------------------------------
    PermissionGate perm_gate;
    perm_gate.set_rules(config.permissions);

    // Stable, shared, nullable pointer to TuiApp (set after construction)
    auto tui_holder = std::make_shared<tui::TuiApp*>(nullptr);

    perm_gate.set_ask_callback(
        [tui_holder](const std::string& action,
                     const std::string& resource,
                     const json& input) -> PermissionEffect
        {
            tui::TuiApp* app = *tui_holder;
            if (!app) {
                // TUI not yet running — deny to avoid deadlock
                return PermissionEffect::Deny;
            }

            // Build the PendingPermission on the heap.  It lives until the
            // main thread resolves the promise (future.get() below unblocks
            // after that, and we delete it here).
            auto perm = std::make_unique<tui::PendingPermission>();
            perm->action   = action;
            perm->resource = resource;
            perm->detail   = input.dump(2);

            std::promise<PermissionEffect> promise;
            auto future = promise.get_future();
            perm->promise = &promise;

            // Post the event directly via the now-public push_engine_event.
            // This writes to the wake pipe and is safe to call from any thread.
            {
                tui::EngineEvent ev;
                ev.kind = tui::EngineEventKind::PermissionReq;
                ev.perm = perm.get();
                app->push_engine_event(std::move(ev));
            }

            // Block until the main loop resolves the permission
            PermissionEffect effect = future.get();

            // perm is deleted here; promise is on our stack (already fulfilled)
            return effect;
        }
    );

    // ------------------------------------------------------------------
    // 7. Session engine
    // ------------------------------------------------------------------
    SessionEngine engine(store, providers, tools, perm_gate, bus, config);

    // ------------------------------------------------------------------
    // 8. TUI application
    // ------------------------------------------------------------------
    tui::TuiApp app(engine, store, bus, config_loader, config, project_dir);

    // Activate the permission callback path
    *tui_holder = &app;

    // Blocks until Ctrl+Q
    app.run();

    *tui_holder = nullptr;
    return 0;
}
