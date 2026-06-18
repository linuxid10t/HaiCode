#include "HaiCodeApp.h"
#include "MainWindow.h"
#include "GuiEventRelay.h"
#include "Messages.h"
#include "SettingsWindow.h"

#include <Application.h>
#include <Alert.h>
#include <FindDirectory.h>
#include <Path.h>
#include <StorageDefs.h>
#include <Directory.h>
#include <Entry.h>

#include <haicode/haicode.h>
#include <haicode/db.h>
#include <haicode/engine.h>
#include <haicode/events.h>
#include <haicode/provider.h>
#include <haicode/tool.h>
#include <haicode/config.h>
#include <haicode/default_prompt.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <string>
#include <memory>
#include <future>
#include <thread>
#include <cstdio>
#include <sys/stat.h>

#include <unistd.h>



HaiCodeApp::HaiCodeApp(int argc, char* argv[])
    : BApplication("application/x-vnd.haicode")
    , window_holder_(std::make_shared<MainWindow*>(nullptr))
{
    // Determine project directory: command-line arg wins, then saved setting, then CWD
    if (argc > 1) {
        project_dir_ = argv[1];
    } else {
        // Try to load saved directory from settings
        BPath settings_path;
        if (find_directory(B_USER_SETTINGS_DIRECTORY, &settings_path) == B_OK) {
            BPath cfg(settings_path);
            cfg.Append("haicode/config.json");
            std::ifstream f(cfg.Path());
            if (f.is_open()) {
                try {
                    nlohmann::json j = nlohmann::json::parse(f);
                    if (j.contains("last_directory") && j["last_directory"].is_string()) {
                        std::string saved = j["last_directory"].get<std::string>();
                        BEntry entry(saved.c_str());
                        if (entry.Exists() && entry.IsDirectory())
                            project_dir_ = saved;
                    }
                } catch (...) {}
            }
        }
        if (project_dir_.empty()) {
            char buf[B_PATH_NAME_LENGTH] = {};
            project_dir_ = getcwd(buf, sizeof(buf)) ? buf : "/boot/home";
        }
    }
}

void
HaiCodeApp::ReadyToRun()
{
    // --- 1. Locate settings directory for DB ---
    BPath settings_path;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &settings_path) != B_OK) {
        settings_path.SetTo("/boot/home/config/settings");
    }
    settings_path.Append("haicode");

    // Create directory if needed
    BDirectory dir;
    if (dir.SetTo(settings_path.Path()) != B_OK) {
        create_directory(settings_path.Path(), 0755);
    }

    BPath db_path(settings_path);
    db_path.Append("sessions.db");

    // --- 2. Open database & run migrations ---
    try {
        db_ = std::make_unique<haicode::Database>(db_path.Path());
        db_->migrate();
    } catch (const std::exception& e) {
        BString err("Failed to open database:\n");
        err << e.what();
        BAlert* alert = new BAlert("Error", err.String(), "Quit");
        alert->Go();
        Quit();
        return;
    }

    // --- 3. Load config ---
    haicode::ConfigLoader loader;
    config_ = loader.load(project_dir_);

    // --- 4. Create core objects ---
    store_     = std::make_unique<haicode::SessionStore>(*db_);
    providers_ = std::make_unique<haicode::ProviderRegistry>();
    tools_     = std::make_unique<haicode::ToolRegistry>();
    perm_gate_ = std::make_unique<haicode::PermissionGate>();
    bus_       = std::make_unique<haicode::SessionEventBus>();

    // --- 5. Register providers (generic: supports any number of Anthropic
    // and OpenAI-compatible endpoints, distinguished by config `type`). ---
    for (auto& [id, pcfg] : config_.providers) {
        std::string key = pcfg.api_key;
        std::string type = pcfg.type.empty()
            ? (id == "anthropic" ? "anthropic" : "openai") : pcfg.type;
        if (key.empty() && id == "anthropic") {
            if (const char* e = std::getenv("ANTHROPIC_API_KEY"); e && *e) key = e;
        }
        if (key.empty() && id == "openai") {
            if (const char* e = std::getenv("OPENAI_API_KEY"); e && *e) key = e;
        }
        // Skip Anthropic with neither key nor base_url; OpenAI-compatible can
        // run keyless against a local endpoint (Ollama, LM Studio).
        if (key.empty() && pcfg.base_url.empty() && type == "anthropic") continue;
        if (type == "anthropic")
            providers_->register_provider(
                haicode::make_anthropic_provider(key, pcfg.base_url, id));
        else
            providers_->register_provider(
                haicode::make_openai_provider(key, pcfg.base_url, id));
    }

    // Default model unconditionally — providers may be configured later
    if (config_.model.empty())
        config_.model = "claude-opus-4-5";

    // --- 6. Register built-in tools ---
    haicode::register_builtin_tools(*tools_);

    // --- 8. Apply permission rules from config ---
    perm_gate_->set_rules(config_.permissions);

    // --- 9. Set up PermissionGate ask callback ---
    // Capture window_holder_ by value so the lambda can access MainWindow* safely.
    std::shared_ptr<MainWindow*> holder = window_holder_;

    perm_gate_->set_ask_callback(
        [holder](const std::string& action,
                 const std::string& resource,
                 const nlohmann::json& input) -> haicode::PermissionEffect
        {
            MainWindow* win = *holder;
            if (!win) {
                // No window yet — deny
                return haicode::PermissionEffect::Deny;
            }

            // Build detail string from input JSON
            std::string detail;
            try { detail = input.dump(2); } catch (...) {}

            // Allocate a promise on the heap; engine thread blocks on the future
            auto* promise = new std::promise<haicode::PermissionEffect>();
            std::future<haicode::PermissionEffect> fut = promise->get_future();

            // Post to MainWindow (thread-safe via BMessenger)
            win->PostPermissionRequest(action, resource, detail,
                                       static_cast<void*>(promise));

            // Block engine thread until user responds
            return fut.get();
        }
    );

    // --- 10. Create SessionEngine ---
    engine_ = std::make_unique<haicode::SessionEngine>(
        *store_, *providers_, *tools_, *perm_gate_, *bus_, config_);

    // --- 11. Create MainWindow ---
    main_window_ = new MainWindow(*engine_, *store_, project_dir_, config_.model, config_.provider);
    *window_holder_ = main_window_;
    // Populate the provider dropdown from config now that MainWindow exists.
    main_window_->RebuildProviderMenu(config_.providers);

    // --- 12. Create GuiEventRelay and attach to bus ---
    relay_ = std::make_unique<GuiEventRelay>(
        main_window_->Messenger(),
        *bus_,
        main_window_->active_session_id());
    relay_->attach();

    // --- 13. Show the window ---
    main_window_->Show();

    // --- 14. Kick off initial model fetch for the provider that the session
    // (loaded in the constructor via _SwitchToSession) already set. Posting
    // MSG_FETCH_MODELS to MainWindow lets it read its own marked provider item
    // rather than overriding it from config (which may differ from the session).
    main_window_->PostMessage(new BMessage(MSG_FETCH_MODELS));
}

bool
HaiCodeApp::QuitRequested()
{
    if (relay_) bus_->unsubscribe_all();
    // Engine threads may be blocked in network I/O; joining them would hang.
    // Use exit() so the OS cleans up all threads immediately.
    std::exit(0);
    return true; // unreachable
}

void
HaiCodeApp::MessageReceived(BMessage* msg)
{
    switch (msg->what) {
        case MSG_ADD_PERMISSION: {
            const char* action   = nullptr;
            const char* resource = nullptr;
            msg->FindString("action",   &action);
            msg->FindString("resource", &resource);
            if (action && resource) {
                always_rules_.push_back(
                    {action, resource, haicode::PermissionEffect::Allow});
                _ApplySessionRules();
            }
            break;
        }
        case MSG_NEW_SESSION:
            always_rules_.clear();
            _ApplySessionRules();
            break;
        case MSG_AUTO_ALLOW_EDITS: {
            int32 value = B_CONTROL_OFF;
            msg->FindInt32("be:value", &value);
            auto_edits_on_ = (value == B_CONTROL_ON);
            _ApplySessionRules();
            break;
        }
        case MSG_YOLO: {
            int32 value = B_CONTROL_OFF;
            msg->FindInt32("be:value", &value);
            yolo_on_ = (value == B_CONTROL_ON);
            _ApplySessionRules();
            break;
        }
        case MSG_PERSIST_PM: {
            const char* provider = nullptr;
            const char* model    = nullptr;
            msg->FindString("provider", &provider);
            msg->FindString("model",    &model);
            if (provider) config_.provider = provider;
            if (model)    config_.model    = model;

            BPath settings_path;
            if (find_directory(B_USER_SETTINGS_DIRECTORY, &settings_path) == B_OK) {
                BPath cfg_path(settings_path);
                cfg_path.Append("haicode");
                create_directory(cfg_path.Path(), 0755);
                cfg_path.Append("config.json");

                nlohmann::json j;
                {
                    std::ifstream f(cfg_path.Path());
                    if (f.is_open()) try { j = nlohmann::json::parse(f); } catch (...) {}
                }
                if (!config_.provider.empty()) j["provider"] = config_.provider;
                if (!config_.model.empty())    j["model"]    = config_.model;
                std::ofstream f(cfg_path.Path());
                if (f.is_open()) f << j.dump(2);
            }
            break;
        }
        case MSG_DIR_CHANGED: {
            const char* path = nullptr;
            if (msg->FindString("path", &path) != B_OK || !path) break;

            BPath settings_path;
            if (find_directory(B_USER_SETTINGS_DIRECTORY, &settings_path) != B_OK) break;
            BPath cfg_path(settings_path);
            cfg_path.Append("haicode");
            create_directory(cfg_path.Path(), 0755);
            cfg_path.Append("config.json");

            // Load existing config, update last_directory, write back
            nlohmann::json j;
            {
                std::ifstream f(cfg_path.Path());
                if (f.is_open()) try { j = nlohmann::json::parse(f); } catch (...) {}
            }
            j["last_directory"] = path;
            std::ofstream f(cfg_path.Path());
            if (f.is_open()) f << j.dump(2);

            // Reload project-specific config (picks up agents.md/claude.md in the new dir).
            project_dir_ = path;
            haicode::ConfigLoader loader2;
            config_ = loader2.load(project_dir_);
            // Recreate the engine so config_.agents_md takes effect for new sessions.
            engine_ = std::make_unique<haicode::SessionEngine>(
                *store_, *providers_, *tools_, *perm_gate_, *bus_, config_);
            if (main_window_) main_window_->SetEngine(*engine_);
            break;
        }
        case MSG_ACTIVE_SESSION: {
            const char* sid = nullptr;
            if (msg->FindString("session_id", &sid) == B_OK && sid && relay_)
                relay_->set_active_session(sid);
            break;
        }
        case MSG_FETCH_MODELS: {
            const char* pid = nullptr;
            msg->FindString("provider_id", &pid);
            std::string provider_id = pid ? pid : "anthropic";

            // Capture shared_ptr so the provider stays alive through the thread
            auto provider = providers_->get(provider_id);
            if (!provider) {
                // No key configured — send empty list so dropdown shows "(none available)"
                BMessage reply(MSG_MODELS_LOADED);
                reply.AddString("provider_id", provider_id.c_str());
                main_window_->PostMessage(&reply);
                break;
            }

            BMessenger win_msgr(main_window_);
            std::thread([provider, provider_id, win_msgr]() {
                auto models = provider->list_models();
                BMessage reply(MSG_MODELS_LOADED);
                reply.AddString("provider_id", provider_id.c_str());
                for (auto& m : models)
                    reply.AddString("model", m.c_str());
                win_msgr.SendMessage(&reply);
            }).detach();
            break;
        }
        case MSG_SHOW_SETTINGS: {
            SettingsWindow* win = new SettingsWindow(config_.providers, BMessenger(this));
            win->Show();
            break;
        }
        case MSG_SETTINGS_SAVED: {
            // Replace the in-memory providers map wholesale from the JSON
            // the settings window sent us.
            const char* providers_json = nullptr;
            if (msg->FindString("providers", &providers_json) != B_OK || !providers_json)
                break;
            try {
                auto pj = nlohmann::json::parse(providers_json, nullptr, false);
                if (!pj.is_discarded() && pj.is_object()) {
                    config_.providers.clear();
                    for (auto& [k, v] : pj.items()) {
                        haicode::ProviderConfig p;
                        p.id = k;
                        p.type = v.value("type", "");
                        if (p.type.empty())
                            p.type = (k == "anthropic") ? "anthropic" : "openai";
                        p.api_key  = v.value("api_key", "");
                        p.base_url = v.value("base_url", "");
                        config_.providers[k] = std::move(p);
                    }
                }
            } catch (...) {}

            // Persist the full providers map, preserving other top-level keys.
            BPath settings_path;
            if (find_directory(B_USER_SETTINGS_DIRECTORY, &settings_path) == B_OK) {
                BPath cfg_path(settings_path);
                cfg_path.Append("haicode");
                create_directory(cfg_path.Path(), 0755);
                cfg_path.Append("config.json");

                nlohmann::json j;
                {
                    std::ifstream f(cfg_path.Path());
                    if (f.is_open()) try { j = nlohmann::json::parse(f); } catch (...) {}
                }
                if (!config_.provider.empty()) j["provider"] = config_.provider;
                if (!config_.model.empty())    j["model"]    = config_.model;
                nlohmann::json providers_j = nlohmann::json::object();
                for (auto& [id, p] : config_.providers) {
                    providers_j[id] = {
                        {"type",     p.type},
                        {"api_key",  p.api_key},
                        {"base_url", p.base_url},
                    };
                }
                j["providers"] = providers_j;

                std::ofstream f(cfg_path.Path());
                if (f.is_open()) f << j.dump(2);
            }

            // Re-register providers from the updated config (generic loop).
            providers_ = std::make_unique<haicode::ProviderRegistry>();
            for (auto& [id, pcfg] : config_.providers) {
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
                if (type == "anthropic")
                    providers_->register_provider(
                        haicode::make_anthropic_provider(key, pcfg.base_url, id));
                else
                    providers_->register_provider(
                        haicode::make_openai_provider(key, pcfg.base_url, id));
            }

            // Recreate engine with updated provider registry and refresh the UI.
            engine_ = std::make_unique<haicode::SessionEngine>(
                *store_, *providers_, *tools_, *perm_gate_, *bus_, config_);
            main_window_->SetEngine(*engine_);
            main_window_->RebuildProviderMenu(config_.providers);

            // Re-fetch models for the currently selected provider.
            std::string refresh_id = config_.provider;
            if (refresh_id.empty() && !config_.providers.empty())
                refresh_id = config_.providers.begin()->first;
            if (!refresh_id.empty()) {
                BMessage fetch(MSG_FETCH_MODELS);
                fetch.AddString("provider_id", refresh_id.c_str());
                PostMessage(&fetch);
            }
            break;
        }
        default:
            BApplication::MessageReceived(msg);
    }
}

void
HaiCodeApp::_ApplySessionRules()
{
    std::vector<haicode::PermissionRule> rules = always_rules_;
    if (auto_edits_on_)
        rules.push_back({"write", "*", haicode::PermissionEffect::Allow});
    if (yolo_on_)
        rules.push_back({"*", "*", haicode::PermissionEffect::Allow});
    perm_gate_->set_session_rules(rules);
}
