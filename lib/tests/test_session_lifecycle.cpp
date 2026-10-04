#include <haicode/engine.h>
#include <haicode/haicode.h>
#include <haicode/permission_requests.h>
#include "test_check.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

using namespace haicode;
using json = nlohmann::json;

static void wait_idle(SessionEngine& engine, const std::string& sid) {
    for (int i = 0; i < 500 && engine.is_running(sid); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    TEST_REQUIRE(!engine.is_running(sid), "bounded worker completion");
}

struct Gate {
    std::mutex m;
    std::condition_variable cv;
    bool open = false;
    void wait() {
        std::unique_lock<std::mutex> lk(m);
        TEST_REQUIRE(cv.wait_for(lk, std::chrono::seconds(5), [&] { return open; }),
            "bounded gate wait");
    }
    void open_gate() {
        std::lock_guard<std::mutex> lk(m);
        open = true;
        cv.notify_all();
    }
};

// Scripts by the final user message: "PARK" → parktool call, "ASK" → an
// ask_user call, anything else ends the turn. Title requests park on their
// own gate and are released by scoped cancel (token prefix "t:").
class ParkingProvider : public Provider {
public:
    std::string id() const override { return "parking"; }
    void cancel(const std::string& token = "") override {
        if (token.empty() || token.rfind("t:", 0) == 0) title_gate->open_gate();
    }
    std::vector<std::string> list_models(std::string&) override { return {"p-model"}; }

    Gate title_entered;
    std::shared_ptr<Gate> title_gate = std::make_shared<Gate>();
    std::atomic<int> calls{0};
    std::atomic<int> title_calls{0};

    void stream(const LLMRequest& req, StreamCallbacks cb, const std::string& = "") override {
        if (req.system.find("generate short descriptive titles") != std::string::npos) {
            ++title_calls;
            title_entered.open_gate();
            title_gate->wait();
            cb.on_finish(FinishReason::EndTurn, {}, {});
            return;
        }
        ++calls;
        const nlohmann::json* tail = req.messages.empty() ? nullptr
                                                          : &req.messages.back();
        std::string text;
        bool is_user = tail && tail->value("role", "") == "user";
        if (is_user && (*tail)["content"].is_string())
            text = (*tail)["content"].get<std::string>();
        if (is_user && text.find("PARK") != std::string::npos) {
            ToolCall tc;
            tc.id = "park1";
            tc.name = "parktool";
            tc.input = json::object();
            cb.on_finish(FinishReason::ToolUse, {}, {tc});
            return;
        }
        if (is_user && text.find("ASK") != std::string::npos) {
            ToolCall tc;
            tc.id = "ask1";
            tc.name = "ask_user";
            tc.input = {{"question", "Pick one"},
                        {"options", json::array({"a", "b"})}};
            cb.on_finish(FinishReason::ToolUse, {}, {tc});
            return;
        }
        cb.on_text_delta("t", "done");
        cb.on_finish(FinishReason::EndTurn, {}, {});
    }
};

class ParkingTool : public Tool {
public:
    Gate entered;
    std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    std::atomic<int> executions{0};
    std::string name() const override { return "parktool"; }
    std::string description() const override { return "parks until released"; }
    json input_schema() const override { return json::object(); }
    ToolResult execute(const json&, const ToolContext& ctx) override {
        ++executions;
        entered.open_gate();
        std::unique_lock<std::mutex> lk(gate->m);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!gate->open && !(ctx.interrupt && ctx.interrupt->load())) {
            TEST_REQUIRE(std::chrono::steady_clock::now() < deadline,
                "bounded parked-tool wait");
            gate->cv.wait_for(lk, std::chrono::milliseconds(10));
        }
        if (gate->open) return {true, "parked ok", ""};
        return {false, "", "park interrupted"};
    }
};

struct Fixture {
    Database db{":memory:"};
    SessionStore store{db};
    std::shared_ptr<ParkingProvider> provider = std::make_shared<ParkingProvider>();
    ProviderRegistry registry;
    ToolRegistry tools;
    std::shared_ptr<ParkingTool> park = std::make_shared<ParkingTool>();
    PermissionGate gate;
    SessionEventBus bus;
    AppConfig config;
    std::unique_ptr<SessionEngine> engine;
    std::string sid;

    explicit Fixture(bool autotitle = false) {
        db.migrate();
        registry.register_provider(provider);
        register_builtin_tools(tools);
        tools.register_tool(park);
        config.default_mode = "build";
        config.autoname_sessions = autotitle;
        config.autoname_llm_refine = autotitle;
        engine = std::make_unique<SessionEngine>(store, registry, tools, gate,
                                                 bus, config);
        sid = create();
    }

    std::string create() {
        auto s = engine->create_session("/tmp", "build", "p-model", "parking");
        TEST_REQUIRE(!s.empty(), "session created");
        return s;
    }

    size_t message_count(const std::string& s) {
        return store.load_messages(s).size();
    }
};

// Idle deletion: rows gone, engine state cleared, gate layers erased for
// that session only, repeated delete safe.
static void test_delete_idle() {
    Fixture fx;
    std::string other = fx.create();
    fx.gate.set_session_rules(other, {{"*", "*", PermissionEffect::Allow}});
    fx.gate.add_allow(other, "bash", "/bin/true");
    // Both grant kinds for the doomed session: pattern and exact.
    fx.gate.add_allow(fx.sid, "bash", "/bin/pattern");
    fx.gate.add_exact_allow(fx.sid, "bash", "/bin/exact");
    const json no_input = json::object();
    TEST_REQUIRE(fx.gate.check(fx.sid, "bash", "/bin/pattern", no_input)
        == PermissionEffect::Allow, "pattern grant active before delete");
    TEST_REQUIRE(fx.gate.check(fx.sid, "bash", "/bin/exact", no_input)
        == PermissionEffect::Allow, "exact grant active before delete");

    std::string err;
    TEST_REQUIRE(fx.engine->delete_session(fx.sid, err) && err.empty(),
        "idle delete succeeds");
    TEST_REQUIRE(!fx.store.get(fx.sid).has_value(), "rows removed");
    TEST_REQUIRE(fx.engine->queued_prompt_count(fx.sid) == 0, "queue cleared");
    TEST_REQUIRE(fx.gate.check(fx.sid, "bash", "/bin/pattern", no_input)
        == PermissionEffect::Ask, "deleted session's pattern grant erased");
    TEST_REQUIRE(fx.gate.check(fx.sid, "bash", "/bin/exact", no_input)
        == PermissionEffect::Ask, "deleted session's exact grant erased");
    TEST_REQUIRE(fx.gate.check(other, "bash", "/bin/true", no_input)
        == PermissionEffect::Allow, "other session's rules and grant intact");
    TEST_REQUIRE(fx.store.get(other).has_value(), "other session's rows intact");

    TEST_REQUIRE(fx.engine->delete_session(fx.sid, err),
        "repeated delete is a safe no-op");
}

// Deleting a running session parked in a tool: interrupt releases it, the
// join completes, queued work never restarts, and no rows appear afterwards.
static void test_delete_running_parked_tool() {
    Fixture fx;
    fx.gate.set_rules({{"*", "*", PermissionEffect::Allow}});
    std::string other = fx.create();
    fx.engine->submit_prompt(fx.sid, "PARK here");
    fx.park->entered.wait();
    fx.engine->submit_prompt(fx.sid, "never runs");
    TEST_REQUIRE(fx.engine->queued_prompt_count(fx.sid) == 1, "prompt queued");
    fx.engine->submit_prompt(other, "PARK too");
    for (int i = 0; i < 500 && fx.park->executions != 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    TEST_REQUIRE(fx.park->executions == 2, "both sessions reached the tool");

    std::string err;
    TEST_REQUIRE(fx.engine->delete_session(fx.sid, err) && err.empty(),
        "running delete succeeds");
    TEST_REQUIRE(fx.park->executions == 2, "deleted session's tool not re-run");
    TEST_REQUIRE(!fx.engine->is_running(fx.sid), "deleted session idle");
    TEST_REQUIRE(fx.engine->is_running(other), "other session still running");
    const int calls_after = fx.provider->calls.load();
    size_t rows = fx.message_count(fx.sid);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    TEST_REQUIRE(fx.message_count(fx.sid) == rows, "no rows after deletion");
    TEST_REQUIRE(fx.provider->calls.load() == calls_after,
        "no provider calls after deletion");
    size_t user_rows = 0;
    for (const auto& r : fx.store.load_messages(fx.sid))
        if (r.type == "user_prompted") ++user_rows;
    TEST_REQUIRE(user_rows == 0,
        "rows fully removed; queued prompt never persisted or run");
    fx.engine->submit_prompt(fx.sid, "rejected");
    TEST_REQUIRE(fx.message_count(fx.sid) == rows,
        "submissions refused after deletion");
    fx.park->gate->open_gate();
    wait_idle(*fx.engine, other);
    fx.engine->shutdown();
}

// Deleting while parked on a permission approval: the wait is denied, the
// join completes, and the stale approval can never grant anything.
static void test_delete_parked_approval() {
    Fixture fx;
    PermissionRequestBroker broker(&fx.gate);
    Gate delivered;
    broker.set_delivery_callback([&](const PermissionRequest&) {
        delivered.open_gate();
        return true;
    });
    fx.gate.set_rules({});
    fx.engine->set_permission_broker(&broker);
    fx.engine->submit_prompt(fx.sid, "PARK approval");
    delivered.wait();
    auto pending = broker.pending_requests();
    TEST_REQUIRE(pending.size() == 1, "one parked approval");
    TEST_REQUIRE(pending[0].session_id == fx.sid, "approval belongs to session");

    std::string err;
    TEST_REQUIRE(fx.engine->delete_session(fx.sid, err) && err.empty(),
        "delete while parked on approval succeeds");
    TEST_REQUIRE(!broker.resolve(pending[0].id, PermissionDecision::AllowForSession),
        "stale approval refused");
    const json no_input = json::object();
    TEST_REQUIRE(fx.gate.check(fx.sid, "parktool", "/tmp", no_input)
        == PermissionEffect::Ask, "stale approval installed no grant");
    TEST_REQUIRE(!fx.store.get(fx.sid).has_value(), "rows removed");
    fx.engine->shutdown();
}

// Deleting while parked on ask_user: the wait is answered "(interrupted)",
// the runner exits, and no further rows are written.
static void test_delete_parked_ask() {
    Fixture fx;
    fx.gate.set_rules({{"*", "*", PermissionEffect::Allow}});
    Gate asked;
    fx.bus.subscribe(events::EventType::AskUserRequested, [&](const json&) {
        asked.open_gate();
    });
    fx.engine->submit_prompt(fx.sid, "ASK me");
    asked.wait();

    std::string err;
    TEST_REQUIRE(fx.engine->delete_session(fx.sid, err) && err.empty(),
        "delete while parked on ask_user succeeds");
    TEST_REQUIRE(!fx.store.get(fx.sid).has_value(), "rows removed");
    size_t rows = 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    rows = fx.message_count(fx.sid);
    TEST_REQUIRE(rows == 0, "no rows after deletion");
    fx.engine->shutdown();
}

// A parked title-refinement job is cancelled and joined; a late stale write
// can never rename the (now deleted) session.
static void test_delete_with_pending_title() {
    Fixture fx(/*autotitle=*/true);
    fx.gate.set_rules({{"*", "*", PermissionEffect::Allow}});
    fx.engine->submit_prompt(fx.sid, "first prompt");
    fx.provider->title_entered.wait();
    wait_idle(*fx.engine, fx.sid);
    const std::string baseline = fx.store.get(fx.sid)->title;
    TEST_REQUIRE(!baseline.empty(), "heuristic title applied");
    const int title_calls = fx.provider->title_calls.load();

    std::string err;
    TEST_REQUIRE(fx.engine->delete_session(fx.sid, err) && err.empty(),
        "delete with pending title job succeeds");
    TEST_REQUIRE(!fx.store.get(fx.sid).has_value(), "rows removed");
    TEST_REQUIRE(fx.provider->title_calls.load() == title_calls,
        "title job cancelled, not completed");
    bool renamed = false;
    fx.bus.subscribe(events::EventType::SessionRenamed, [&](const json& data) {
        if (data.value("session_id", "") == fx.sid) renamed = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    TEST_REQUIRE(!renamed, "no title event after deletion");
    fx.engine->shutdown();
}

// The DB delete runs only after the workers exited; a failure surfaces
// through `error`, rolls the retirement back, and leaves the session usable.
static void test_delete_db_failure() {
    const char* path = "/tmp/haicode_test_lifecycle.db";
    remove(path);
    Database db(path);
    db.migrate();
    SessionStore store(db);
    auto provider = std::make_shared<ParkingProvider>();
    ProviderRegistry registry;
    registry.register_provider(provider);
    ToolRegistry tools;
    PermissionGate gate;
    SessionEventBus bus;
    AppConfig config;
    config.default_mode = "build";
    config.autoname_sessions = false;
    SessionEngine engine(store, registry, tools, gate, bus, config);    std::string sid = engine.create_session("/tmp", "build", "p-model", "parking");
    TEST_REQUIRE(!sid.empty(), "session created");
    engine.submit_prompt(sid, "hello");
    wait_idle(engine, sid);
    const size_t rows = store.load_messages(sid).size();
    TEST_REQUIRE(rows >= 2, "turn completed before the failure test");

    // Second connection holds the database exclusive; the engine's short
    // busy timeout turns the delete into a prompt DbError, not a hang.
    db.set_busy_timeout(100);
    Database blocker(path);
    blocker.exec("BEGIN EXCLUSIVE;");

    std::string err;
    TEST_REQUIRE(!engine.delete_session(sid, err), "delete fails politely");
    TEST_REQUIRE(!err.empty(), "failure explained");
    blocker.exec("ROLLBACK;");

    TEST_REQUIRE(store.get(sid).has_value(), "session survives the failure");
    TEST_REQUIRE(!engine.is_running(sid), "not running after rollback");
    engine.submit_prompt(sid, "still works");
    wait_idle(engine, sid);
    TEST_REQUIRE(store.load_messages(sid).size() > rows,
        "session usable after the failed delete");
    std::string err2;
    TEST_REQUIRE(engine.delete_session(sid, err2) && err2.empty(),
        "delete succeeds once the DB is free");
    TEST_REQUIRE(!store.get(sid).has_value(), "rows removed on retry");
    engine.shutdown();
}

// Bulk cleanup is a loop over the same retirement path (HaiCodeApp::_RunCleanup).
// One running session in the matched set must not disturb a session the filter
// did not match: it keeps running, keeps its grants, and its rows survive.
static void test_bulk_delete_loop() {
    Fixture fx;
    fx.gate.set_rules({});
    std::string running = fx.create();     // matched, parked in a tool
    std::string survivor = fx.create();    // NOT matched, keeps running

    // parktool runs under a per-session grant rather than a global allow-all,
    // so the grant-erasure assertions below can tell the two apart.
    fx.gate.add_allow(fx.sid, "parktool", "/tmp");
    fx.gate.add_allow(running, "parktool", "/tmp");
    fx.gate.add_allow(survivor, "parktool", "/tmp");

    fx.engine->submit_prompt(fx.sid, "PARK doomed");
    fx.park->entered.wait();
    fx.engine->submit_prompt(survivor, "PARK survivor");
    for (int i = 0; i < 500 && fx.park->executions != 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    TEST_REQUIRE(fx.park->executions == 2, "both sessions reached the tool");

    // Give the doomed sessions grants that must disappear with them, and the
    // survivor grants that must not.
    const json no_input = json::object();
    fx.gate.add_allow(fx.sid, "bash", "/bin/doomed");
    fx.gate.add_allow(running, "bash", "/bin/doomed");
    fx.gate.add_allow(survivor, "bash", "/bin/keep");
    TEST_REQUIRE(fx.gate.check(running, "bash", "/bin/doomed", no_input)
        == PermissionEffect::Allow, "doomed grants active before deletion");

    SessionFilter filter;
    filter.directory = "/tmp";
    auto matched = fx.store.sessions_matching(filter);
    TEST_REQUIRE(matched.size() == 3, "filter matches the doomed sessions and the survivor");
    matched.erase(std::remove(matched.begin(), matched.end(), survivor), matched.end());
    TEST_REQUIRE(matched.size() == 2, "two sessions targeted");

    int deleted = 0, failed = 0;
    for (const std::string& sid : matched) {
        std::string err;
        if (fx.engine->delete_session(sid, err)) ++deleted; else ++failed;
    }
    TEST_REQUIRE(deleted == 2 && failed == 0, "bulk loop deleted both");

    TEST_REQUIRE(!fx.store.get(fx.sid).has_value(), "first doomed session gone");
    TEST_REQUIRE(!fx.store.get(running).has_value(), "second doomed session gone");
    TEST_REQUIRE(!fx.engine->is_running(fx.sid) && !fx.engine->is_running(running),
        "retired sessions no longer running");
    TEST_REQUIRE(fx.park->executions == 2, "no parked tool re-ran after retirement");

    // The untouched session is unaffected by the loop.
    TEST_REQUIRE(fx.engine->is_running(survivor), "survivor still running");
    TEST_REQUIRE(fx.gate.check(survivor, "bash", "/bin/keep", no_input)
        == PermissionEffect::Allow, "survivor's grant intact");
    TEST_REQUIRE(fx.gate.check(fx.sid, "bash", "/bin/doomed", no_input)
        == PermissionEffect::Ask, "doomed grants erased");

    fx.park->gate->open_gate();
    wait_idle(*fx.engine, survivor);
    TEST_REQUIRE(fx.store.get(survivor).has_value(), "survivor's rows intact");
    fx.engine->shutdown();
}

static void test_idle_maintenance() {
    Fixture fx;
    fx.gate.set_rules({{"*", "*", PermissionEffect::Allow}});
    bool called = false;
    TEST_REQUIRE(fx.engine->run_when_idle([&] {
        called = true;
        TEST_REQUIRE(fx.store.get(fx.sid).has_value(), "maintenance may read the store");
    }), "idle maintenance may access the store");
    TEST_REQUIRE(called, "idle callback ran");

    fx.engine->submit_prompt(fx.sid, "PARK foreground");
    fx.park->entered.wait();
    called = false;
    TEST_REQUIRE(!fx.engine->run_when_idle([&] { called = true; }),
        "foreground work defers maintenance");
    TEST_REQUIRE(!called, "busy callback never ran");
    fx.park->gate->open_gate();
    wait_idle(*fx.engine, fx.sid);
    TEST_REQUIRE(fx.engine->run_when_idle([&] { called = true; }),
        "maintenance retries after foreground completion");
    TEST_REQUIRE(called, "retry ran");
    fx.engine->shutdown();
    TEST_REQUIRE(!fx.engine->run_when_idle([] {}), "shutdown refuses maintenance");

    Fixture titled(true);
    titled.gate.set_rules({{"*", "*", PermissionEffect::Allow}});
    titled.engine->submit_prompt(titled.sid, "finish and name this session");
    titled.provider->title_entered.wait();
    wait_idle(*titled.engine, titled.sid);
    TEST_REQUIRE(!titled.engine->run_when_idle([] {}),
        "title maintenance also defers database maintenance");
    titled.provider->title_gate->open_gate();
    bool ran = false;
    for (int i = 0; i < 500 && !ran; ++i) {
        ran = titled.engine->run_when_idle([] {});
        if (!ran) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    TEST_REQUIRE(ran, "maintenance retries after title completion");
    titled.engine->shutdown();
}

int main() {
    test_idle_maintenance();
    test_delete_idle();
    test_delete_running_parked_tool();
    test_delete_parked_approval();
    test_delete_parked_ask();
    test_delete_with_pending_title();
    test_delete_db_failure();
    test_bulk_delete_loop();
    std::puts("session lifecycle tests passed");
}
