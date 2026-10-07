// session.project_used: set by the agentic loop on the first Build/Plan step
// so the GUI can tell a chat-only session (directory hidden in the sidebar)
// from one that has worked in its project. Store unit (sticky, idempotent,
// no time_updated bump, independent of model_json patches) and engine e2e
// (Chat turns leave it unset, a Build turn sets it, switching back to Chat
// keeps it). The migration-3 backfill is covered by test_db_upgrade.
#include <haicode/engine.h>
#include <haicode/db.h>
#include <haicode/provider.h>
#include <haicode/tool.h>
#include <haicode/config.h>
#include "test_check.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

class FakeProvider : public haicode::Provider {
public:
    std::string id() const override { return "fake"; }
    void cancel(const std::string& = "") override {}
    std::vector<std::string> list_models(std::string&) override {
        return {"fake-model"};
    }
    int get_model_context(const std::string&) const override { return 0; }
    void stream(const haicode::LLMRequest&, haicode::StreamCallbacks cb,
                const std::string& = "") override {
        cb.on_text_delta("t", "ok");
        cb.on_finish(haicode::FinishReason::EndTurn, {}, {});
    }
};

static std::string make_tmp(const char* tmpl) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", tmpl);
    TEST_REQUIRE(mkdtemp(buf) != nullptr, "mkdtemp");
    return buf;
}

static void rm_tmp(const std::string& dir) {
    std::string cmd = "rm -rf '" + dir + "'";
    if (std::system(cmd.c_str()) != 0)
        std::cerr << "warning: could not remove " << dir << std::endl;
}

static bool wait_turn(haicode::SessionEngine& engine, haicode::SessionStore& store,
                      const std::string& sid, size_t rows) {
    for (int i = 0; i < 200; ++i) {
        if (store.load_messages(sid).size() >= rows && !engine.is_running(sid))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

static void test_store() {
    std::string tmp = make_tmp("/tmp/hc_test_project_used_XXXXXX");
    {
        haicode::Database db(tmp + "/store.db");
        db.migrate();
        haicode::SessionStore store(db);
        auto s = store.create(tmp, "build", "{\"mode\":\"chat\"}");
        TEST_REQUIRE(!store.get(s.id)->project_used, "new session unset");

        int64_t before = store.get(s.id)->time_updated;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        store.mark_project_used(s.id);
        auto after = store.get(s.id);
        TEST_REQUIRE(after->project_used, "mark sets the flag");
        TEST_REQUIRE(after->time_updated == before,
                     "mark does not bump time_updated (no sidebar reorder)");
        store.mark_project_used(s.id);
        TEST_REQUIRE(store.get(s.id)->project_used, "mark is idempotent");

        // model_json patches don't touch the column.
        store.update_mode(s.id, "chat");
        TEST_REQUIRE(store.get(s.id)->project_used, "survives a mode patch");

        bool listed = false;
        for (const auto& si : store.list())
            if (si.id == s.id) listed = si.project_used;
        TEST_REQUIRE(listed, "list() carries the flag");

        // Unknown id: a silent no-op, like the other patch methods.
        store.mark_project_used("ses_missing");
    }
    rm_tmp(tmp);
    std::cout << "[OK] store: sticky, idempotent, no time_updated bump" << std::endl;
}

static void test_engine() {
    std::string tmp = make_tmp("/tmp/hc_test_project_used_e2e_XXXXXX");
    ::mkdir((tmp + "/proj").c_str(), 0755);
    {
        haicode::Database db(tmp + "/e2e.db");
        db.migrate();
        haicode::SessionStore store(db);
        auto provider = std::make_shared<FakeProvider>();
        haicode::ProviderRegistry registry;
        registry.register_provider(provider);
        haicode::ToolRegistry tools;
        haicode::PermissionGate perms;
        haicode::SessionEventBus bus;
        haicode::AppConfig cfg;
        cfg.model = "fake-model";
        cfg.provider = "fake";
        cfg.autoname_sessions = false;
        cfg.default_mode = "build";

        haicode::SessionEngine engine(store, registry, tools, perms, bus, cfg);
        std::string sid = engine.create_session(tmp + "/proj", "build",
                                                "fake-model", "fake");
        TEST_REQUIRE(!sid.empty(), "session created");
        // Created in Build, switched to Chat before anything ran: nothing
        // has touched the project yet.
        engine.set_mode(sid, haicode::SessionMode::Chat);
        TEST_REQUIRE(!store.get(sid)->project_used, "unset before any turn");

        engine.submit_prompt(sid, "just chatting");
        TEST_REQUIRE(wait_turn(engine, store, sid, 2), "chat turn completes");
        TEST_REQUIRE(!store.get(sid)->project_used,
                     "a Chat turn leaves the session chat-only");

        engine.set_mode(sid, haicode::SessionMode::Build);
        engine.submit_prompt(sid, "now build");
        TEST_REQUIRE(wait_turn(engine, store, sid, 4), "build turn completes");
        TEST_REQUIRE(store.get(sid)->project_used, "a Build turn sets the flag");

        engine.set_mode(sid, haicode::SessionMode::Chat);
        engine.submit_prompt(sid, "back to chat");
        TEST_REQUIRE(wait_turn(engine, store, sid, 6), "second chat turn completes");
        TEST_REQUIRE(store.get(sid)->project_used,
                     "switching back to Chat keeps the flag");

        // Plan counts as project use too.
        std::string plan_sid = engine.create_session(tmp + "/proj", "build",
                                                     "fake-model", "fake");
        engine.set_mode(plan_sid, haicode::SessionMode::Plan);
        engine.submit_prompt(plan_sid, "plan something");
        TEST_REQUIRE(wait_turn(engine, store, plan_sid, 2), "plan turn completes");
        TEST_REQUIRE(store.get(plan_sid)->project_used, "a Plan turn sets the flag");
    }  // ~SessionEngine joins the loop threads
    rm_tmp(tmp);
    std::cout << "[OK] engine: Chat turns stay chat-only, Build/Plan turns mark"
              << std::endl;
}

int main() {
    std::cout << "=== test_project_used ===" << std::endl;
    test_store();
    test_engine();
    std::cout << "\nAll test_project_used tests passed!" << std::endl;
    return 0;
}
