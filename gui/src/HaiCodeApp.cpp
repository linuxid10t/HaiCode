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
#include <haicode/util.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <memory>
#include <thread>
#include <cstdio>
#include <sys/stat.h>
#include <system_error>

#include <unistd.h>



// Every config save funnels through here: read the existing document, apply
// the mutation, write back atomically. An unparseable file is refused, not
// overwritten — the old truncating ofstream silently replaced a hand-edited
// file (typo included) with the few keys being saved, losing the rest.
// Failures are surfaced to the user instead of swallowed.
static void
save_config_at(const std::string& path, const char* what,
               const std::function<void(nlohmann::json&)>& mutate)
{
    std::string err;
    if (haicode::update_config_file(path, mutate, err))
        return;  // saved
    BString text;
    text << "Could not save " << what << ":\n\n" << err.c_str();
    BAlert* alert = new BAlert("Config save failed", text.String(), "OK");
    alert->Go();
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

    // Tighten secret files that earlier releases or other editors may have
    // left group/world-readable: config.json carries provider API keys,
    // openai-auth.json the OAuth tokens. Best-effort — every later save
    // rewrites them 0600 anyway.
    {
        BPath cfg(settings_path);
        cfg.Append("config.json");
        (void)haicode::util::ensure_owner_only(cfg.Path());
        BPath auth(settings_path);
        auth.Append("openai-auth.json");
        (void)haicode::util::ensure_owner_only(auth.Path());
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
    global_layer_ = haicode::load_layer(haicode::global_config_path());
    _SyncMergedConfig();  // fills config_ and last_trust_ (gate not yet up)

    // --- 4. Create core objects ---
    store_     = std::make_unique<haicode::SessionStore>(*db_);
    providers_ = make_provider_registry(config_.providers);
    tools_     = std::make_unique<haicode::ToolRegistry>();
    perm_gate_ = std::make_unique<haicode::PermissionGate>();
    bus_       = std::make_unique<haicode::SessionEventBus>();

    // No hardcoded default model: the startup MSG_FETCH_MODELS path marks
    // the first listed model and propagates it; a failed fetch leaves the
    // placeholder for the user to pick manually (Task 26).

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

    // --- 13b. Project trust: if the project's config carries gated keys
    // (permission rules, a build command, agent overrides, search API keys)
    // without a matching trust record, ask now. Declining leaves the merged
    // config as loaded — stripped — so the fail-closed state is already set.
    _MaybePromptProjectTrust();

    // --- 14. Kick off initial model fetch for the provider that the session
    // (loaded in the constructor via _SwitchToSession) already set. Posting
    // MSG_FETCH_MODELS to MainWindow lets it read its own marked provider item
    // rather than overriding it from config (which may differ from the session).
    main_window_->PostMessage(new BMessage(MSG_FETCH_MODELS));
}

void
HaiCodeApp::ArgvReceived(int32 argc, char** argv)
{
    // First launch: argv arrives while still launching and was already
    // consumed by the constructor. Only a second launch of the
    // B_SINGLE_LAUNCH app reaches here with the window up.
    if (IsLaunching())
        return;
    for (int32 i = 1; i < argc; ++i) {
        if (!argv[i])
            continue;
        BEntry entry(argv[i]);
        if (entry.InitCheck() == B_OK && entry.Exists() && entry.IsDirectory()) {
            entry_ref ref;
            if (entry.GetRef(&ref) == B_OK && main_window_) {
                BMessage fwd(B_REFS_RECEIVED);
                fwd.AddRef("refs", &ref);
                main_window_->PostMessage(&fwd);
            }
            return;
        }
    }
}

void
HaiCodeApp::RefsReceived(BMessage* msg)
{
    // A second launch invoked via Tracker ("Open With", drag-onto-icon).
    // MainWindow's own B_REFS_RECEIVED handler switches the project dir and
    // posts MSG_DIR_CHANGED; on the first launch the window does not exist
    // yet, and the constructor's argv handling covers that path.
    if (main_window_)
        main_window_->PostMessage(msg);
}

bool
HaiCodeApp::QuitRequested()
{
    // Any still-running confirmation already happened in MainWindow's
    // QuitRequested (the window is queried before B_QUIT_REQUESTED reaches
    // the app; vetoing there vetoed the quit).
    if (relay_) bus_->unsubscribe_all();

    // Deletion workers reference engine_ and store_; join them before the
    // engine shuts down so no worker outlives what it touches.
    _JoinLifecycleWorkers();

    // Orderly shutdown with a hard bound: the engine's workers are now
    // cancellable (subprocess runner + HTTP abort-on-cancel), so shutdown()
    // normally joins within ~1 s. On the normal path returning true lets
    // Run() unwind — members destruct in safe order and ~Database closes
    // SQLite cleanly. A shutdown wedged past 10 s falls back to _exit(0)
    // (no static destructors) rather than hanging the quit.
    std::atomic<bool> done{false};
    std::thread shut([&] {
        if (engine_) engine_->shutdown();
        done = true;
    });
    for (int i = 0; i < 100 && !done; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (done) {
        shut.join();
        return true;
    }
    _exit(0);
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
            // A session never activated this run (e.g. picked in the
            // Permissions center) has no entry yet: load its persisted flags
            // first, or the other two would default to off and the persist
            // below would overwrite them.
            if (!session_flags_.count(sid)) _SyncSessionFlags(sid);
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
            if (!session_flags_.count(sid)) _SyncSessionFlags(sid);
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
            if (!session_flags_.count(sid)) _SyncSessionFlags(sid);
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
            // Keep the global layer in sync: a later Settings save writes
            // provider/model from it and must not resurrect a stale pick.
            if (provider) global_layer_.values.provider = provider;
            if (model)    global_layer_.values.model    = model;

            std::string p = config_.provider, m = config_.model;
            save_config_at(haicode::global_config_path(), "the provider selection",
                [p, m](nlohmann::json& j) {
                    if (!p.empty()) j["provider"] = p;
                    if (!m.empty()) j["model"]    = m;
                });
            break;
        }
        case MSG_DIR_PROPOSED: {
            // Window-side selection arrived. Nothing has mutated yet; the
            // app owns acceptance. On confirm, the window applies its half
            // (label, skills, DB row) and replies MSG_DIR_CHANGED, whose
            // handler does the app-side reload + recreate. On cancel the
            // silence IS the veto — no state moved anywhere.
            const char* path = nullptr;
            if (msg->FindString("path", &path) != B_OK || !path) break;
            if (!_ConfirmDisruptiveChange("Changing the project directory")) break;
            if (main_window_) {
                BMessage apply(MSG_DIR_APPLY);
                apply.AddString("path", path);
                main_window_->PostMessage(&apply);
            }
            break;
        }
        case MSG_DIR_CHANGED: {
            const char* path = nullptr;
            if (msg->FindString("path", &path) != B_OK || !path) break;

            std::string dir = path;
            save_config_at(haicode::global_config_path(), "the project directory",
                [dir](nlohmann::json& j) { j["last_directory"] = dir; });

            // Reload project-specific config (picks up agents.md/claude.md in the new dir).
            project_dir_ = path;
            global_layer_ = haicode::load_layer(haicode::global_config_path());
            _SyncMergedConfig();
            _MaybePromptProjectTrust();
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
            if (!_ConfirmDisruptiveChange("Updating providers")) break;
            if (!_ApplyProviders(msg)) break;
            auto providers = haicode::providers_to_json(
                global_layer_.values.providers);
            save_config_at(haicode::global_config_path(), "providers",
                [providers](nlohmann::json& j) { j["providers"] = providers; });
            _RefreshProviders();
            break;
        }
        case MSG_DELETE_SESSION_CONFIRMED: {
            const char* sid_c = nullptr;
            if (msg->FindString("session_id", &sid_c) != B_OK || !sid_c) break;
            std::string sid = sid_c;
            // Retiring, joining, and the DB delete happen off the looper so
            // the GUI stays responsive while a running session winds down.
            // The worker only touches engine_/store_ (thread-safe) and posts
            // the completion message; all UI work happens in the handler.
            std::lock_guard<std::mutex> lock(lifecycle_mu_);
            lifecycle_workers_.emplace_back([this, sid]() {
                std::string err;
                bool ok = engine_ ? engine_->delete_session(sid, err) : false;
                BMessage done(MSG_SESSION_DELETED);
                done.AddString("session_id", sid.c_str());
                done.AddBool("ok", ok);
                if (!err.empty())
                    done.AddString("error", err.c_str());
                PostMessage(&done);
            });
            break;
        }
        case MSG_SESSION_DELETED:
            if (main_window_)
                main_window_->PostMessage(msg);
            break;
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
            global_layer_ = haicode::load_layer(haicode::global_config_path());
            _SyncMergedConfig();
            _RefreshPermissionsCenter();
            break;
        }
        case MSG_SETTINGS_SAVED: {
            if (!_ConfirmDisruptiveChange("Applying new settings")) break;
            if (!_ApplyProviders(msg)) break;

            // Settings edits the GLOBAL layer; the merged config_ is
            // re-derived from it below. Applying here (not to config_) is
            // what stops project-owned values from leaking into the global
            // file on save.
            haicode::AppConfig& g = global_layer_.values;

            // Apply scalar settings from the settings window.
            const char* model = nullptr;
            const char* provider = nullptr;
            const char* default_mode = nullptr;
            const char* thinking_display = nullptr;
            const char* build_command = nullptr;
            const char* ws_engine = nullptr;
            int32 ws_max = 0;
            if (msg->FindString("model", &model) == B_OK && model)
                g.model = model;
            if (msg->FindString("provider", &provider) == B_OK && provider)
                g.provider = provider;
            if (msg->FindString("default_mode", &default_mode) == B_OK
                && default_mode && (*default_mode == '\0'
                    || *default_mode == 'p' || *default_mode == 'b')) {
                // Accept "plan"/"build"; treat empty as "leave as-is".
                if (*default_mode)
                    g.default_mode = default_mode;
            }
            if (msg->FindString("thinking_display", &thinking_display) == B_OK
                && thinking_display
                && (std::string(thinking_display) == "off"
                    || std::string(thinking_display) == "on"
                    || std::string(thinking_display) == "on_while_thinking")) {
                g.thinking_display = thinking_display;
            }
            // build_command is project-scoped: captured here, written to
            // <project>/.haicode/config.json below — never the global file.
            std::string project_build_command;
            if (msg->FindString("build_command", &build_command) == B_OK)
                project_build_command = build_command ? build_command : "";
            if (msg->FindString("web_search_engine", &ws_engine) == B_OK && ws_engine
                && (std::string(ws_engine) == "ddg_lite"
                    || std::string(ws_engine) == "ddg_html"
                    || std::string(ws_engine) == "exa"
                    || std::string(ws_engine) == "zai")) {
                g.web_search_engine = ws_engine;
            }
            if (msg->FindInt32("web_search_max_results", &ws_max) == B_OK && ws_max > 0)
                g.web_search_max_results = ws_max;
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
                g.web_search_api_keys[ws_key_engine] = ws_key;
            }

            // Context-window override for a model (from the Settings General tab).
            int32 context_window = 0;
            const char* context_model = nullptr;
            if (msg->FindInt32("context_window", &context_window) == B_OK
                && context_window > 0
                && msg->FindString("context_model", &context_model) == B_OK
                && context_model && *context_model) {
                g.model_contexts[context_model] = context_window;
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
                    g.model_vision[vision_model] = true;
                else if (v == "no")
                    g.model_vision[vision_model] = false;
                else
                    g.model_vision.erase(vision_model);
            }

            // Vision fallback pair (from the Settings General tab). Empty
            // provider or model turns the feature off.
            const char* fb_provider = nullptr;
            const char* fb_model = nullptr;
            if (msg->FindString("vision_fallback_provider", &fb_provider) == B_OK
                && msg->FindString("vision_fallback_model", &fb_model) == B_OK) {
                g.vision_fallback_provider = fb_provider ? fb_provider : "";
                g.vision_fallback_model    = fb_model    ? fb_model    : "";
            }

            // Default skills for new sessions (Settings Skills tab).
            // Repeated field: replace the whole list on every save.
            g.default_skills.clear();
            {
                const char* def_skill = nullptr;
                for (int32 i = 0;
                        msg->FindString("default_skill", i, &def_skill) == B_OK;
                        ++i) {
                    if (def_skill && *def_skill)
                        g.default_skills.push_back(def_skill);
                }
            }

            // Persist the global-scope keys (and only those) atomically;
            // permissions/last_directory/... in the file are preserved, and a
            // legacy global build_command is dropped (project-only now).
            {
                std::string err;
                if (!haicode::sync_global_scope(g, haicode::global_config_path(),
                                                err)) {
                    BString text;
                    text << "Could not save settings:\n\n" << err.c_str();
                    BAlert* alert = new BAlert("Config save failed",
                                               text.String(), "OK");
                    alert->Go();
                }
            }
            // The build hook is project-scoped: it lives in the project's own
            // .haicode/config.json (erase-when-empty), never the global file.
            save_config_at(haicode::project_config_path(project_dir_),
                "the build command",
                [project_build_command](nlohmann::json& j) {
                    if (!project_build_command.empty())
                        j["build_command"] = project_build_command;
                    else
                        j.erase("build_command");
                });

            // Re-derive the merged config the engine/UI read.
            _SyncMergedConfig();
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

void
HaiCodeApp::_SyncMergedConfig()
{
    config_ = haicode::load_with_layers(global_layer_, project_dir_,
                                        &last_trust_);
    // The project layer may add or remove permission rules; the gate keeps
    // the previously configured set otherwise.
    if (perm_gate_)
        perm_gate_->set_rules(config_.permissions);
}

void
HaiCodeApp::_MaybePromptProjectTrust()
{
    if (!last_trust_.needed || last_trust_.granted)
        return;
    // Fail-closed prompt: "Don't Trust" is the default button and Escape —
    // the merged config already reflects the stripped layer, so declining
    // is a no-op.
    BString text;
    text << "The configuration in this project:\n\n"
         << project_dir_.c_str()
         << "\n\nwants to enable:\n\n"
         << last_trust_.summary.c_str()
         << "\nUntil the project is trusted, these settings are ignored.";
    BAlert* alert = new BAlert("Project trust", text.String(),
                               "Don't Trust", "Trust", nullptr,
                               B_WIDTH_AS_USUAL, B_WARNING_ALERT);
    alert->SetShortcut(0, B_ESCAPE);
    if (alert->Go() != 1)
        return;
    std::string err;
    if (!haicode::store_trust_record("", project_dir_,
                                     last_trust_.fingerprint, err)) {
        BString failure;
        failure << "Could not record trust for this project:\n\n"
                << err.c_str();
        BAlert* a = new BAlert("Trust failed", failure.String(), "OK");
        a->Go();
        return;
    }
    // Record matches the current gated content: re-merge so the gated keys
    // (permissions, build command, ...) take effect.
    _SyncMergedConfig();
}

bool
HaiCodeApp::_ConfirmDisruptiveChange(const char* what)
{
    if (!engine_) return true;
    auto running = engine_->running_sessions();
    if (running.empty()) return true;

    // Name every running session — background ones included, not just the
    // active window's. Titles over raw ids so the user recognizes their work.
    std::string names;
    int shown = 0;
    for (const auto& sid : running) {
        if (shown == 6) {
            names += "…and " + std::to_string(running.size() - shown) + " more\n";
            break;
        }
        auto si = store_->get(sid);
        std::string title = (si && !si->title.empty()) ? si->title : sid;
        if (!names.empty()) names += "\n";
        names += "• " + title;
        ++shown;
    }

    BString text;
    text << what << " stops every running session:\n\n"
         << names.c_str()
         << "\n\nInterrupt them and apply anyway?";
    // Cancel is the safe choice: Escape and the default button leave the app,
    // the window, the DB, and the config files completely unchanged.
    BAlert* alert = new BAlert("Running sessions", text.String(),
                               "Cancel", "Interrupt and Apply", nullptr,
                               B_WIDTH_AS_USUAL, B_WARNING_ALERT);
    alert->SetShortcut(0, B_ESCAPE);
    return alert->Go() == 1;
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
        // Providers are global-scope: edit the global layer, then re-merge
        // so config_ (engine + UI) reflects global + project as loaded.
        global_layer_.values.providers = std::move(updated);
        _SyncMergedConfig();
        return true;
    } catch (...) {
        return false;
    }
}

void
HaiCodeApp::_JoinLifecycleWorkers()
{
    // Swap under the mutex, join outside it: a finishing worker's last act
    // is PostMessage (no app lock needed), so this cannot deadlock.
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mu_);
        workers.swap(lifecycle_workers_);
    }
    for (auto& t : workers)
        if (t.joinable()) t.join();
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
    // Deletion workers reference engine_; they must exit before it does.
    _JoinLifecycleWorkers();
    if (engine_) {
        engine_->cancel_pending_asks();
        // Accepted replacement stops every run deliberately — background
        // sessions too, matching what the confirmation dialog promised.
        for (auto& sid : engine_->running_sessions())
            engine_->interrupt(sid);
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
                    status = "YOLO";
                    preset = "unrestricted";
                } else if (it->second.auto_edits) {
                    status = "Auto-write";
                    preset = "auto-write";
                }
            }
        }
    }
    if (mode == haicode::SessionMode::Plan)
        status = read_on ? "All reads" : "Standard";
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
    // local access — and the reads-everywhere rule arms in Build and Plan
    // (outside-project reads gate and prompt in both; Chat has no read
    // tool).
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
        if ((build || plan) && it->second.read_everywhere)
            rules.push_back({"read", "*", haicode::PermissionEffect::Allow});
    }
    perm_gate_->set_session_rules(session_id, rules);
}
