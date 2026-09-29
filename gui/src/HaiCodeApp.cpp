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
#include <haicode/codex_auth.h>
#include <haicode/db.h>
#include <haicode/engine.h>
#include <haicode/events.h>
#include <haicode/provider.h>
#include <haicode/tool.h>
#include <haicode/config.h>
#include <haicode/default_prompt.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <memory>
#include <thread>
#include <cstdio>
#include <sys/stat.h>
#include <system_error>

#include <unistd.h>



static nlohmann::json
providers_json(const std::map<std::string, haicode::ProviderConfig>& providers)
{
    nlohmann::json j = nlohmann::json::object();
    for (auto& [id, p] : providers)
        j[id] = {{"type", p.type}, {"api_key", p.api_key}, {"base_url", p.base_url}};
    return j;
}

static std::unique_ptr<haicode::ProviderRegistry>
make_provider_registry(const std::map<std::string, haicode::ProviderConfig>& providers)
{
    auto registry = std::make_unique<haicode::ProviderRegistry>();
    for (auto& [id, pcfg] : providers) {
        std::string key = pcfg.api_key;
        std::string type = pcfg.type.empty()
            ? (id == "anthropic" ? "anthropic"
              : id == "chatgpt" ? "chatgpt" : "openai") : pcfg.type;
        if (key.empty() && id == "anthropic") {
            if (const char* e = std::getenv("ANTHROPIC_API_KEY"); e && *e) key = e;
        }
        if (key.empty() && id == "openai") {
            if (const char* e = std::getenv("OPENAI_API_KEY"); e && *e) key = e;
        }
        if (key.empty() && pcfg.base_url.empty() && type == "anthropic") continue;
        if (type == "chatgpt") {
            if (!haicode::codex_auth_signed_in()) continue;
            registry->register_provider(haicode::make_codex_provider(id, pcfg.base_url));
        } else if (type == "anthropic") {
            registry->register_provider(haicode::make_anthropic_provider(key, pcfg.base_url, id));
        } else if (type == "ollama" || type == "vllm" || type == "openrouter"
                   || type == "lmstudio" || type == "llamacpp") {
            registry->register_provider(
                haicode::make_openai_compat_provider(key, pcfg.base_url, id, type));
        } else {
            registry->register_provider(haicode::make_openai_provider(key, pcfg.base_url, id));
        }
    }
    return registry;
}

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
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &settings_path, true) != B_OK) {
        BAlert* alert = new BAlert("Error", "Failed to locate the settings directory.", "Quit");
        alert->Go();
        Quit();
        return;
    }
    settings_path.Append("haicode");

    std::error_code dir_error;
    std::filesystem::create_directories(settings_path.Path(), dir_error);
    if (dir_error) {
        BString err("Failed to create settings directory:\n");
        err << settings_path.Path() << "\n" << dir_error.message().c_str();
        BAlert* alert = new BAlert("Error", err.String(), "Quit");
        alert->Go();
        Quit();
        return;
    }

    BPath db_path(settings_path);
    db_path.Append("sessions.db");

    // Project plans are optional for read-only projects.
    std::filesystem::create_directories(project_dir_ + "/.haicode/plans", dir_error);

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
    providers_ = make_provider_registry(config_.providers);
    tools_     = std::make_unique<haicode::ToolRegistry>();
    perm_gate_ = std::make_unique<haicode::PermissionGate>();
    bus_       = std::make_unique<haicode::SessionEventBus>();

    // Default model unconditionally — providers may be configured later
    if (config_.model.empty())
        config_.model = "claude-opus-4-5";

    // --- 6. Register built-in tools ---
    haicode::register_builtin_tools(*tools_);

    // --- 8. Apply permission rules from config ---
    perm_gate_->set_rules(config_.permissions);

    // --- 9. Permission approval broker ---
    // The broker owns approval wait state; GUI messages carry request ids,
    // never promise pointers. Delivery posts to MainWindow (thread-safe via
    // BMessenger); a missing window fails delivery and denies.
    perm_broker_ = std::make_unique<haicode::PermissionRequestBroker>(
        perm_gate_.get());
    {
        std::shared_ptr<MainWindow*> holder = window_holder_;
        perm_broker_->set_delivery_callback(
            [holder](const haicode::PermissionRequest& req) {
                MainWindow* win = *holder;
                if (!win) return false;
                win->PostPermissionRequest(req);
                return true;
            });
    }
    // Every resolved/cancelled approval lands in the activity log (bounded,
    // in-memory) and nudges an open Permissions center to refresh. The
    // callbacks run on engine threads; be_app->PostMessage is thread-safe.
    {
        using clock = std::chrono::system_clock;
        perm_broker_->set_notify_callback(
            [this](const haicode::PermissionRequest& req,
                   const haicode::PermissionOutcome& out) {
                PermissionActivityEntry e;
                e.timestamp_ms = std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                        clock::now().time_since_epoch()).count();
                e.session_id = req.session_id;
                e.tool_name  = req.tool_name;
                e.target     = req.resource;
                e.result = out.user_decided
                    ? (out.effect == haicode::PermissionEffect::Allow
                           ? "allowed" : "denied")
                    : "cancelled";
                e.reason = out.reason;
                perm_activity_.add(e);
                be_app->PostMessage(MSG_PERM_REFRESH);
            });
    }

    // --- 10. Create SessionEngine ---
    engine_ = std::make_unique<haicode::SessionEngine>(
        *store_, *providers_, *tools_, *perm_gate_, *bus_, config_);
    engine_->set_permission_broker(perm_broker_.get());

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
        case MSG_PERMISSION_DECISION: {
            // Approval-window reply routed by request id. Unknown/stale ids
            // are refused by the broker (exactly-once), so a closed window's
            // late duplicate can never approve anything.
            const char* request_id = nullptr;
            int32 decision = 0;   // default Deny (Escape/close path)
            msg->FindString("request_id", &request_id);
            msg->FindInt32("decision", &decision);
            if (request_id && perm_broker_) {
                using PD = haicode::PermissionDecision;
                PD d = (decision == 2) ? PD::AllowForSession
                     : (decision == 1) ? PD::AllowOnce
                                       : PD::Deny;
                perm_broker_->resolve(request_id, d);
            }
            break;
        }
        case MSG_NEW_SESSION:
            // Per-session allows are scoped by session id in the gate; there
            // is no global layer left to clear here.
            break;
        case MSG_AUTO_ALLOW_EDITS: {
            int32 value = B_CONTROL_OFF;
            msg->FindInt32("be:value", &value);
            std::string sid = _TargetSession(msg);
            session_flags_[sid].auto_edits = (value == B_CONTROL_ON);
            _ApplySessionRules(sid);
            _PersistSessionFlags(sid);
            _NotifyPermissionUiChanged();
            break;
        }
        case MSG_YOLO: {
            int32 value = B_CONTROL_OFF;
            msg->FindInt32("be:value", &value);
            std::string sid = _TargetSession(msg);
            session_flags_[sid].yolo = (value == B_CONTROL_ON);
            _ApplySessionRules(sid);
            _PersistSessionFlags(sid);
            _NotifyPermissionUiChanged();
            break;
        }
        case MSG_READ_EVERYWHERE: {
            int32 value = B_CONTROL_OFF;
            msg->FindInt32("be:value", &value);
            std::string sid = _TargetSession(msg);
            session_flags_[sid].read_everywhere = (value == B_CONTROL_ON);
            _ApplySessionRules(sid);
            _PersistSessionFlags(sid);
            _NotifyPermissionUiChanged();
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
            // The new project layer may add or remove permission rules; the
            // gate keeps the previously configured set otherwise.
            perm_gate_->set_rules(config_.permissions);
            _RecreateEngine();
            break;
        }
        case MSG_ACTIVE_SESSION: {
            const char* sid = nullptr;
            if (msg->FindString("session_id", &sid) == B_OK && sid && relay_)
                relay_->set_active_session(sid);
            // Session switch: pull that session's persisted flags into the
            // gate (the old toolbar-checkbox restore path, minus the widgets).
            if (sid && *sid) {
                _SyncSessionFlags(sid);
                _NotifyPermissionUiChanged();
            }
            break;
        }
        case MSG_PERM_SYNC:
            _NotifyPermissionUiChanged();
            break;
        case MSG_FETCH_MODELS: {
            const char* pid = nullptr;
            msg->FindString("provider_id", &pid);
            std::string provider_id = pid ? pid : "anthropic";

            // Reply to whoever sent the request: a caller may attach a
            // "reply" BMessenger (e.g. SettingsWindow's model dropdown) so the
            // results don't get routed to MainWindow. Fall back to MainWindow
            // when no reply target is present or it's invalid.
            BMessenger reply_to(main_window_);
            BMessenger explicit_reply;
            if (msg->FindMessenger("reply", &explicit_reply) == B_OK
                    && explicit_reply.IsValid())
                reply_to = explicit_reply;

            // Capture shared_ptr so the provider stays alive through the thread
            auto provider = providers_->get(provider_id);
            // Callers may request a distinct reply constant (e.g. the Settings
            // vision-fallback dropdown) so the results land in their own
            // handler instead of clobbering the primary model menu.
            int32 reply_what = MSG_MODELS_LOADED;
            int32 requested_what = 0;
            if (msg->FindInt32("reply_what", &requested_what) == B_OK
                    && requested_what != 0)
                reply_what = requested_what;
            if (!provider) {
                // No key configured — send empty list so dropdown shows "(none available)"
                BMessage reply(reply_what);
                reply.AddString("provider_id", provider_id.c_str());
                reply_to.SendMessage(&reply);
                break;
            }

            BMessenger win_msgr(reply_to);
            std::thread([provider, provider_id, win_msgr, reply_what]() {
                std::string err;
                auto models = provider->list_models(err);
                BMessage reply(reply_what);
                reply.AddString("provider_id", provider_id.c_str());
                reply.AddString("error", err.c_str());
                for (auto& m : models)
                    reply.AddString("model", m.c_str());
                win_msgr.SendMessage(&reply);
            }).detach();
            break;
        }
        case MSG_SHOW_SETTINGS: {
            SettingsWindow* win = new SettingsWindow(config_, BMessenger(this),
                                                     project_dir_);
            win->Show();
            break;
        }
        case MSG_PROVIDERS_UPDATED: {
            if (!_ApplyProviders(msg)) break;
            BPath settings_path;
            if (find_directory(B_USER_SETTINGS_DIRECTORY, &settings_path) == B_OK) {
                BPath cfg_path(settings_path);
                cfg_path.Append("haicode");
                create_directory(cfg_path.Path(), 0755);
                cfg_path.Append("config.json");
                nlohmann::json j = nlohmann::json::object();
                {
                    std::ifstream f(cfg_path.Path());
                    if (f.is_open()) try { j = nlohmann::json::parse(f); } catch (...) {}
                }
                if (!j.is_object()) j = nlohmann::json::object();
                j["providers"] = providers_json(config_.providers);
                std::ofstream f(cfg_path.Path());
                if (f.is_open()) f << j.dump(2);
            }
            _RefreshProviders();
            break;
        }
        case MSG_SHOW_PERMISSIONS:
            _ShowPermissionsCenter();
            break;
        case MSG_PERMISSION_CENTER_CLOSED:
            perm_center_ = nullptr;
            break;
        case MSG_PERM_REFRESH:
            _RefreshPermissionsCenter();
            break;
        case MSG_PERM_POLICY_SAVED: {
            // A policy file was written from the Permissions center: reload
            // the effective configuration and refresh the gate's configured
            // rules. Session layers (toggles, grants) are untouched.
            haicode::ConfigLoader loader;
            config_ = loader.load(project_dir_);
            perm_gate_->set_rules(config_.permissions);
            _RefreshPermissionsCenter();
            break;
        }
        case MSG_SETTINGS_SAVED: {
            if (!_ApplyProviders(msg)) break;

            // Apply scalar settings from the settings window.
            const char* model = nullptr;
            const char* provider = nullptr;
            const char* default_mode = nullptr;
            const char* thinking_display = nullptr;
            const char* build_command = nullptr;
            const char* ws_engine = nullptr;
            int32 ws_max = 0;
            if (msg->FindString("model", &model) == B_OK && model)
                config_.model = model;
            if (msg->FindString("provider", &provider) == B_OK && provider)
                config_.provider = provider;
            if (msg->FindString("default_mode", &default_mode) == B_OK
                && default_mode && (*default_mode == '\0'
                    || *default_mode == 'p' || *default_mode == 'b')) {
                // Accept "plan"/"build"; treat empty as "leave as-is".
                if (*default_mode)
                    config_.default_mode = default_mode;
            }
            if (msg->FindString("thinking_display", &thinking_display) == B_OK
                && thinking_display
                && (std::string(thinking_display) == "off"
                    || std::string(thinking_display) == "on"
                    || std::string(thinking_display) == "on_while_thinking")) {
                config_.thinking_display = thinking_display;
            }
            if (msg->FindString("build_command", &build_command) == B_OK)
                config_.build_command = build_command ? build_command : "";
            if (msg->FindString("web_search_engine", &ws_engine) == B_OK && ws_engine
                && (std::string(ws_engine) == "ddg_lite"
                    || std::string(ws_engine) == "ddg_html"
                    || std::string(ws_engine) == "exa"
                    || std::string(ws_engine) == "zai")) {
                config_.web_search_engine = ws_engine;
            }
            if (msg->FindInt32("web_search_max_results", &ws_max) == B_OK && ws_max > 0)
                config_.web_search_max_results = ws_max;
            // Search API key: paired with its engine id. Non-empty key sets/
            // replaces it; empty key means "keep the stored key" (provider-
            // editor semantics). No message pair = nothing to change.
            const char* ws_key = nullptr;
            const char* ws_key_engine = nullptr;
            if (msg->FindString("web_search_key", &ws_key) == B_OK && ws_key
                && msg->FindString("web_search_key_engine", &ws_key_engine) == B_OK
                && ws_key_engine
                && (std::string(ws_key_engine) == "exa"
                    || std::string(ws_key_engine) == "zai")
                && *ws_key) {
                config_.web_search_api_keys[ws_key_engine] = ws_key;
            }

            // Context-window override for a model (from the Settings General tab).
            int32 context_window = 0;
            const char* context_model = nullptr;
            if (msg->FindInt32("context_window", &context_window) == B_OK
                && context_window > 0
                && msg->FindString("context_model", &context_model) == B_OK
                && context_model && *context_model) {
                config_.model_contexts[context_model] = context_window;
            }

            // Vision override for a model (from the Settings General tab).
            // Tri-state: yes/no sets the override, auto erases it so the
            // built-in table + fail-closed detection applies again.
            const char* vision_override = nullptr;
            const char* vision_model = nullptr;
            if (msg->FindString("vision_override", &vision_override) == B_OK
                && vision_override
                && msg->FindString("vision_model", &vision_model) == B_OK
                && vision_model && *vision_model) {
                std::string v(vision_override);
                if (v == "yes")
                    config_.model_vision[vision_model] = true;
                else if (v == "no")
                    config_.model_vision[vision_model] = false;
                else
                    config_.model_vision.erase(vision_model);
            }

            // Vision fallback pair (from the Settings General tab). Empty
            // provider or model turns the feature off.
            const char* fb_provider = nullptr;
            const char* fb_model = nullptr;
            if (msg->FindString("vision_fallback_provider", &fb_provider) == B_OK
                && msg->FindString("vision_fallback_model", &fb_model) == B_OK) {
                config_.vision_fallback_provider = fb_provider ? fb_provider : "";
                config_.vision_fallback_model    = fb_model    ? fb_model    : "";
            }

            // Default skills for new sessions (Settings Skills tab).
            // Repeated field: replace the whole list on every save.
            config_.default_skills.clear();
            {
                const char* def_skill = nullptr;
                for (int32 i = 0;
                        msg->FindString("default_skill", i, &def_skill) == B_OK;
                        ++i) {
                    if (def_skill && *def_skill)
                        config_.default_skills.push_back(def_skill);
                }
            }

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
                else j.erase("provider");
                if (!config_.model.empty())    j["model"]    = config_.model;
                if (!config_.default_mode.empty())
                    j["default_mode"] = config_.default_mode;
                // Thinking display: erase-when-default (and when never set)
                // so the file stays minimal for the default behavior.
                if (!config_.thinking_display.empty()
                    && config_.thinking_display != "on_while_thinking")
                    j["thinking_display"] = config_.thinking_display;
                else
                    j.erase("thinking_display");
                if (!config_.build_command.empty())
                    j["build_command"] = config_.build_command;
                else
                    j.erase("build_command");
                j["web_search"] = {
                    {"engine", config_.web_search_engine},
                    {"max_results", config_.web_search_max_results},
                };
                // Default-enabled skills; erase-when-empty so unchecking
                // every skill fully clears the persisted list.
                if (!config_.default_skills.empty()) {
                    nlohmann::json skills_j = nlohmann::json::array();
                    for (auto& s : config_.default_skills)
                        skills_j.push_back(s);
                    j["skills"] = skills_j;
                } else {
                    j.erase("skills");
                }
                // Preserve any API keys the user set by hand in config.json
                // (the Settings UI has no key-entry field; keys live in
                // config.json or env vars only).
                if (!config_.web_search_api_keys.empty()) {
                    nlohmann::json keys_j = nlohmann::json::object();
                    for (auto& [engine, key] : config_.web_search_api_keys)
                        keys_j[engine] = key;
                    j["web_search"]["api_keys"] = keys_j;
                }
                if (!config_.model_contexts.empty()) {
                    nlohmann::json models_j = nlohmann::json::object();
                    for (auto& [mid, win] : config_.model_contexts)
                        models_j[mid] = win;
                    j["models"] = models_j;
                }
                // Vision overrides use the "vision" top-level key that
                // ConfigLoader parses; erase it when fully reset to Auto so
                // stale entries don't linger.
                if (!config_.model_vision.empty()) {
                    nlohmann::json vision_j = nlohmann::json::object();
                    for (auto& [mid, vis] : config_.model_vision)
                        vision_j[mid] = vis;
                    j["vision"] = vision_j;
                } else {
                    j.erase("vision");
                }
                // Vision fallback pair; erase-when-empty so "(none)" fully
                // disables the feature in the persisted file.
                if (!config_.vision_fallback_model.empty()) {
                    j["vision_fallback"] = {
                        {"provider", config_.vision_fallback_provider},
                        {"model",    config_.vision_fallback_model},
                    };
                } else {
                    j.erase("vision_fallback");
                }
                j["providers"] = providers_json(config_.providers);

                std::ofstream f(cfg_path.Path());
                if (f.is_open()) f << j.dump(2);
            }

            _RefreshProviders();
            main_window_->PostMessage(new BMessage(MSG_SETTINGS_SAVED));

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

bool
HaiCodeApp::_ApplyProviders(const BMessage* msg)
{
    const char* text = nullptr;
    if (msg->FindString("providers", &text) != B_OK || !text) return false;
    try {
        auto j = nlohmann::json::parse(text);
        if (!j.is_object()) return false;
        std::map<std::string, haicode::ProviderConfig> updated;
        for (auto& [id, v] : j.items()) {
            haicode::ProviderConfig p;
            p.id = id;
            p.type = v.value("type", "");
            if (p.type.empty())
                p.type = id == "anthropic" ? "anthropic"
                    : id == "chatgpt" ? "chatgpt" : "openai";
            p.api_key = v.value("api_key", "");
            p.base_url = v.value("base_url", "");
            updated[id] = std::move(p);
        }
        config_.providers = std::move(updated);
        return true;
    } catch (...) {
        return false;
    }
}

void
HaiCodeApp::_RefreshProviders()
{
    auto next_providers = make_provider_registry(config_.providers);
    _RecreateEngine(std::move(next_providers));
    if (main_window_) {
        main_window_->Lock();
        main_window_->RebuildProviderMenu(config_.providers);
        main_window_->Unlock();
    }
}

void
HaiCodeApp::_RecreateEngine(std::unique_ptr<haicode::ProviderRegistry> next_providers)
{
    std::string sid;
    if (main_window_) {
        main_window_->Lock();
        sid = main_window_->active_session_id();
        main_window_->Unlock();
    }
    if (engine_) {
        engine_->cancel_pending_asks();
        if (!sid.empty()) engine_->interrupt(sid);
        engine_->shutdown();
    }

    // The retiring engine's shutdown() closed the broker (cancel_all); the
    // shared broker must serve the new engine, so lift the block before the
    // replacement starts running. Waits denied by the shutdown stay denied.
    if (perm_broker_)
        perm_broker_->reopen();

    auto old_engine = std::move(engine_);
    auto old_providers = next_providers ? std::move(providers_) : nullptr;
    if (next_providers) providers_ = std::move(next_providers);
    engine_ = std::make_unique<haicode::SessionEngine>(
        *store_, *providers_, *tools_, *perm_gate_, *bus_, config_);
    if (main_window_) {
        main_window_->Lock();
        main_window_->SetEngine(*engine_);
        main_window_->Unlock();
    }
    old_engine.reset();
}

std::string
HaiCodeApp::_TargetSession(const BMessage* msg) const
{
    const char* sid = nullptr;
    if (msg->FindString("session_id", &sid) == B_OK && sid && *sid)
        return sid;
    if (main_window_)
        return main_window_->active_session_id();
    return "";
}

void
HaiCodeApp::_ShowPermissionsCenter()
{
    if (perm_center_) {
        // Activate() must run on the center's own loop.
        perm_center_->PostMessage(MSG_PERM_ACTIVATE);
        return;
    }
    std::string initial_session;
    if (main_window_) {
        main_window_->Lock();
        initial_session = main_window_->active_session_id();
        main_window_->Unlock();
    }
    perm_center_ = new PermissionsCenterWindow(
        *perm_gate_, *perm_broker_, *tools_, *store_, perm_activity_,
        [this]() -> haicode::SessionEngine* { return engine_.get(); },
        initial_session,
        haicode::global_config_path(),
        haicode::project_config_path(project_dir_),
        project_dir_,
        BMessenger(this));
    perm_center_->Show();
}

void
HaiCodeApp::_RefreshPermissionsCenter()
{
    // PostMessage is thread-safe and runs Refresh() on the center's loop;
    // Lock() from here could deadlock against the center posting back.
    if (perm_center_) perm_center_->PostMessage(MSG_PERM_REFRESH);
}

void
HaiCodeApp::_SyncSessionFlags(const std::string& session_id)
{
    if (session_id.empty() || !store_) return;
    auto si = store_->get(session_id);
    if (!si) return;
    bool auto_edits = false, yolo = false, read_everywhere = false;
    try {
        auto mj = nlohmann::json::parse(si->model_json, nullptr, false);
        if (!mj.is_discarded() && mj.is_object()) {
            auto_edits = mj.value("auto_edits", false);
            yolo = mj.value("yolo", false);
            read_everywhere = mj.value("allow_read_everywhere", false);
        }
    } catch (...) {}
    session_flags_[session_id] = {auto_edits, yolo, read_everywhere};
    _ApplySessionRules(session_id);
}

void
HaiCodeApp::_PersistSessionFlags(const std::string& session_id)
{
    if (session_id.empty() || !store_) return;
    auto it = session_flags_.find(session_id);
    if (it == session_flags_.end()) return;
    store_->update_permission_flags(session_id, it->second.auto_edits,
                                    it->second.yolo,
                                    it->second.read_everywhere);
}

void
HaiCodeApp::_NotifyPermissionUiChanged()
{
    // Status summary for the active session + per-session pending counts.
    // Mode-scoped: write presets surface only in Build (Plan is
    // non-destructive, Chat has no local access); Plan reports its read
    // scope instead. The hard boundary is the engine's tool allowlist —
    // this only keeps the summary honest about it.
    std::string status = "Standard";
    std::string active;
    if (main_window_) {
        main_window_->Lock();
        active = main_window_->active_session_id();
        main_window_->Unlock();
    }
    haicode::SessionMode mode = haicode::SessionMode::Build;
    if (engine_ && !active.empty()) mode = engine_->get_mode(active);
    // Convergence point: every flag/mode/session change funnels through
    // here, so re-derive the active session's armed rules from flags + the
    // CURRENT mode. _ApplySessionRules is mode-gated and idempotent — this
    // is what disarms dormant write rules when a session leaves Build and
    // re-arms them when it returns.
    if (!active.empty()) _ApplySessionRules(active);
    bool read_on = false;
    // Write presets exist only in Build; "" outside Build converges the
    // dropdown's mark to Standard regardless of the flag entry.
    std::string preset = (mode == haicode::SessionMode::Build) ? "standard" : "";
    if (!active.empty()) {
        auto it = session_flags_.find(active);
        if (it != session_flags_.end()) {
            read_on = it->second.read_everywhere;
            if (mode == haicode::SessionMode::Build) {
                if (it->second.yolo) {
                    status = "Unrestricted";
                    preset = "unrestricted";
                } else if (it->second.auto_edits) {
                    status = "Auto-write";
                    preset = "auto-write";
                }
            }
        }
    }
    if (mode == haicode::SessionMode::Plan)
        status = read_on ? "Plan reads: everywhere" : "Plan reads: project";
    else if (mode == haicode::SessionMode::Chat)
        status = "No local access";

    size_t pending = perm_broker_ ? perm_broker_->pending_count() : 0;
    if (pending > 0)
        status = std::to_string(pending) + " waiting";

    BMessage m(MSG_PERM_STATUS);
    m.AddString("status", status.c_str());
    m.AddBool("read_everywhere", read_on);
    m.AddString("preset", preset.c_str());
    m.AddString("mode", mode == haicode::SessionMode::Plan ? "plan"
              : mode == haicode::SessionMode::Chat ? "chat" : "build");
    if (perm_broker_) {
        for (const auto& req : perm_broker_->pending_requests())
            m.AddString("pend_session", req.session_id.c_str());
    }
    if (main_window_) main_window_->PostMessage(&m);
    _RefreshPermissionsCenter();
}

void
HaiCodeApp::_ApplySessionRules(const std::string& session_id)
{
    // Permissions are mode-scoped, mirroring the engine's tool allowlist
    // (ToolRegistry::evaluate checks tool_allowed_in_mode before any gate
    // rule, so this is consistency, not the security boundary): write/bypass
    // rules arm only in Build — Plan is non-destructive and Chat has no
    // local access — and the reads-outside rule is Plan-only.
    haicode::SessionMode mode = haicode::SessionMode::Build;
    if (engine_) mode = engine_->get_mode(session_id);
    bool build = (mode == haicode::SessionMode::Build);
    bool plan  = (mode == haicode::SessionMode::Plan);

    std::vector<haicode::PermissionRule> rules;
    auto it = session_flags_.find(session_id);
    if (it != session_flags_.end()) {
        if (build && it->second.auto_edits)
            rules.push_back({"write", "*", haicode::PermissionEffect::Allow});
        if (build && it->second.yolo)
            rules.push_back({"*", "*", haicode::PermissionEffect::Allow});
        if (plan && it->second.read_everywhere)
            rules.push_back({"read", "*", haicode::PermissionEffect::Allow});
    }
    perm_gate_->set_session_rules(session_id, rules);
}
