#include <haicode/engine.h>
#include <haicode/db.h>
#include <haicode/provider.h>
#include <haicode/tool.h>
#include <haicode/config.h>
#include <haicode/haicode.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <sqlite3.h>
#include <nlohmann/json.hpp>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << msg << "\n"; return false; } } while(0)

static bool test_above_gate() {
    // Gate for max=50 is min(10, 25) = 10; anything above 10 is empty.
    auto out40 = haicode::render_dynamic_prompt("test-model", "Haiku", "/tmp", 40, 50);
    CHECK(out40.empty(), "steps_left=40 max=50 should be empty (above gate)");
    auto out11 = haicode::render_dynamic_prompt("m", "os", "/tmp", 11, 50);
    CHECK(out11.empty(), "steps_left=11 max=50 should be empty (just above gate)");
    std::cout << "[OK] above gate suppressed (steps_left=40, 11; max=50)\n";
    return true;
}

static bool test_gate_boundary() {
    // First emitted step: threshold exactly.
    auto out = haicode::render_dynamic_prompt("test-model", "Haiku", "/tmp", 10, 50);
    CHECK(!out.empty(), "steps_left=10 max=50 should emit the block");
    CHECK(out.find("renewable per-turn step budget") != std::string::npos,
          "boundary step should contain the base sentence");
    CHECK(out.find("Budget is getting tight (10 steps left)") != std::string::npos,
          "boundary step should contain 'Budget is getting tight (10 steps left)'");
    CHECK(out.find("CRITICAL") == std::string::npos,
          "boundary step should NOT contain CRITICAL");
    std::cout << "[OK] gate boundary (steps_left=10, max=50)\n";
    return true;
}

static bool test_half_max_rule() {
    // max=16 → threshold = min(10, 8) = 8.
    auto out9 = haicode::render_dynamic_prompt("m", "os", "/tmp", 9, 16);
    CHECK(out9.empty(), "steps_left=9 max=16 should be empty (threshold is 8)");
    auto out8 = haicode::render_dynamic_prompt("m", "os", "/tmp", 8, 16);
    CHECK(!out8.empty(), "steps_left=8 max=16 should emit the block");
    CHECK(out8.find("getting tight") != std::string::npos,
          "steps_left=8 max=16 should contain 'getting tight'");
    std::cout << "[OK] half-max rule (max=16, threshold 8)\n";
    return true;
}

static bool test_small_budget() {
    // max=6 → threshold = min(10, 3) = 3. First emission lands at 3 (CRITICAL).
    auto out4 = haicode::render_dynamic_prompt("m", "os", "/tmp", 4, 6);
    CHECK(out4.empty(), "steps_left=4 max=6 should be empty (threshold is 3)");
    auto out3 = haicode::render_dynamic_prompt("m", "os", "/tmp", 3, 6);
    CHECK(!out3.empty(), "steps_left=3 max=6 should emit the block");
    CHECK(out3.find("CRITICAL") != std::string::npos,
          "steps_left=3 max=6 should be CRITICAL (<= 4)");
    std::cout << "[OK] small budget (max=6, threshold 3, CRITICAL at first emission)\n";
    return true;
}

static bool test_floor() {
    // max=1 → threshold floored to 1; the single step must still render.
    auto out = haicode::render_dynamic_prompt("m", "os", "/tmp", 1, 1);
    CHECK(!out.empty(), "steps_left=1 max=1 should emit the block (floor)");
    CHECK(out.find("CRITICAL: only 1 step(s) left") != std::string::npos,
          "steps_left=1 max=1 should contain 'CRITICAL: only 1 step(s) left'");
    std::cout << "[OK] floor (max=1, single CRITICAL step)\n";
    return true;
}

static bool test_firm_tier() {
    auto out10 = haicode::render_dynamic_prompt("test-model", "Haiku", "/tmp", 10, 50);
    CHECK(out10.find("getting tight") != std::string::npos,
          "steps_left=10 should trigger firm tier");
    CHECK(out10.find("CRITICAL") == std::string::npos,
          "steps_left=10 should NOT be CRITICAL");
    auto out5 = haicode::render_dynamic_prompt("m", "os", "/tmp", 5, 50);
    CHECK(out5.find("getting tight") != std::string::npos,
          "steps_left=5 should trigger firm tier");
    CHECK(out5.find("CRITICAL") == std::string::npos,
          "steps_left=5 should NOT be CRITICAL");
    std::cout << "[OK] firm tier within window (steps_left=10, 5)\n";
    return true;
}

static bool test_critical_tier() {
    auto out4 = haicode::render_dynamic_prompt("m", "os", "/tmp", 4, 50);
    CHECK(out4.find("CRITICAL") != std::string::npos,
          "steps_left=4 should trigger critical tier");
    auto out2 = haicode::render_dynamic_prompt("test-model", "Haiku", "/tmp", 2, 50);
    CHECK(out2.find("CRITICAL: only 2 step(s) left") != std::string::npos,
          "critical tier should contain 'CRITICAL: only 2 step(s) left'");
    CHECK(out2.find("getting tight") == std::string::npos,
          "critical tier should NOT contain 'getting tight'");
    auto out1 = haicode::render_dynamic_prompt("m", "os", "/tmp", 1, 50);
    CHECK(out1.find("CRITICAL: only 1 step(s) left") != std::string::npos,
          "steps_left=1 should contain 'CRITICAL: only 1 step(s) left'");
    std::cout << "[OK] critical tier (steps_left=4, 2, 1; max=50)\n";
    return true;
}

static bool test_placeholder_substitution() {
    auto out = haicode::render_dynamic_prompt("claude-sonnet-4-6", "Haiku R1", "/projects/foo", 8, 50);
    CHECK(out.find("claude-sonnet-4-6") == std::string::npos,
          "{{MODEL}} should be substituted, not appear literally");
    CHECK(out.find("{{STEPS_LEFT}}") == std::string::npos,
          "{{STEPS_LEFT}} should be substituted, not appear literally");
    CHECK(out.find("8 step(s) remaining") != std::string::npos,
          "{{STEPS_LEFT}} should resolve to 8");
    std::cout << "[OK] placeholder substitution in dynamic prompt\n";
    return true;
}

// Regression for the settings-save crash: HaiCodeApp used to destroy the
// engine while its agentic-loop thread was mid-run; ~SessionEngine() detached
// the worker and freed mu_/config_/maps under it, so the worker's next
// lock_guard(mu_) hit the pthread "mutex->owner == -1" assertion. The
// destructor now joins the workers. Here stream() blocks on a gate the test
// controls (simulating an in-flight HTTP request that ignores cancel());
// we destroy the engine while the call is blocked and release the gate from
// a helper thread only after destruction has begun. Assertions: the
// destructor blocks until the worker drains (proving the join), and the
// worker fully exited stream() before the destructor returned.
class BlockingProvider : public haicode::Provider {
public:
    std::string id() const override { return "blocking"; }
    void cancel(const std::string& stream_token = "") override {}
    std::vector<std::string> list_models(std::string&) override {
        return {"blocking-model"};
    }
    void stream(const haicode::LLMRequest&, haicode::StreamCallbacks cb, const std::string& stream_token = "") override {
        {
            std::lock_guard<std::mutex> g(m);
            ++entered;
        }
        entered_cv.notify_all();
        {
            std::unique_lock<std::mutex> lk(m);
            release_cv.wait(lk, [&] { return release_flag; });
            ++exited;
        }
        cb.on_text_delta("t", "ok");
        cb.on_finish(haicode::FinishReason::EndTurn, {}, {});
    }
    std::mutex m;
    std::condition_variable entered_cv;
    std::condition_variable release_cv;
    int entered = 0;
    int exited = 0;
    bool release_flag = false;
};

static bool test_destructor_joins_running_loop() {
    static const char* kDb = "/tmp/haicode_test_destructor_join.db";
    remove(kDb);
    haicode::Database db(kDb);
    db.migrate();
    haicode::SessionStore store(db);
    auto provider = std::make_shared<BlockingProvider>();
    haicode::ProviderRegistry registry;
    registry.register_provider(provider);
    haicode::ToolRegistry tools;
    haicode::PermissionGate perms;
    haicode::SessionEventBus bus;
    haicode::AppConfig cfg;
    cfg.model = "blocking-model";
    cfg.provider = "blocking";
    cfg.autoname_sessions = false;

    std::thread releaser;
    auto t0 = std::chrono::steady_clock::now();
    {
        haicode::SessionEngine engine(store, registry, tools, perms, bus, cfg);
        std::string sid = engine.create_session("/tmp/proj", "build",
                                                "blocking-model", "blocking");
        engine.submit_prompt(sid, "hello");

        // Wait until the worker is parked inside stream().
        bool entered_stream = false;
        {
            std::unique_lock<std::mutex> lk(provider->m);
            entered_stream = provider->entered_cv.wait_for(
                lk, std::chrono::seconds(5),
                [&] { return provider->entered > 0; });
        }
        CHECK(entered_stream, "worker reached the blocked stream() call");

        // Flip the release gate 300ms from now — by then scope exit below has
        // started the destructor, which (with the join-based dtor) is blocked
        // waiting for this very flip. With the old detach behavior the dtor
        // returned immediately and the released worker ran into freed memory.
        releaser = std::thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            std::lock_guard<std::mutex> g(provider->m);
            provider->release_flag = true;
            provider->release_cv.notify_all();
        });

        t0 = std::chrono::steady_clock::now();
    }  // ~SessionEngine(): flags set, providers cancelled, workers joined.

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    releaser.join();

    CHECK(elapsed.count() >= 100,
          "destructor blocked until the worker drained (join, not detach)");
    CHECK(provider->entered == 1 && provider->exited == 1,
          "worker entered and exited stream() exactly once");

    std::cout << "[OK] destructor joins running loop (blocked "
              << elapsed.count() << "ms for the in-flight call, no crash)\n";
    return true;
}

// ============================================================
// Engine e2e: execution-time mode gate + failure-output persistence
// ============================================================

// First stream() emits the configured tool call; every later call ends the
// turn. Drives the full engine loop without a real LLM.
class ToolCallProvider : public haicode::Provider {
public:
    std::string id() const override { return "toolcall"; }
    void cancel(const std::string& stream_token = "") override {}
    std::vector<std::string> list_models(std::string&) override {
        return {"tc-model"};
    }
    void stream(const haicode::LLMRequest&, haicode::StreamCallbacks cb, const std::string& stream_token = "") override {
        ++calls;
        if (calls == 1) {
            std::vector<haicode::ToolCall> tcs{pending};
            cb.on_finish(haicode::FinishReason::ToolUse, {}, tcs);
            return;
        }
        cb.on_text_delta("t", "done");
        cb.on_finish(haicode::FinishReason::EndTurn, {}, {});
    }
    int calls = 0;
    haicode::ToolCall pending;
};

static bool wait_for_rows(haicode::SessionStore& store, const std::string& sid,
                          size_t want) {
    for (int i = 0; i < 200; ++i) {
        if (store.load_messages(sid).size() >= want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return store.load_messages(sid).size() >= want;
}

static std::string first_tool_result_output(haicode::SessionStore& store,
                                            const std::string& sid) {
    for (const auto& m : store.load_messages(sid)) {
        if (m.type != "tool_result") continue;
        auto j = nlohmann::json::parse(m.data_json, nullptr, false);
        if (j.is_object()) return j.value("output", "");
    }
    return "";
}

// Review #1 e2e: Chat mode + a provider that returns a `write` call anyway.
// The wire filter already hides write; the execution-time check must refuse
// it even though the permission rules allow everything. The failed
// tool_result persists (denied=false → turn continues) and the file is
// never created.
static bool test_chat_mode_write_blocked_e2e() {
    std::string tmpl = "/tmp/hc_test_chat_e2e_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data())) { CHECK(false, "mkdtemp failed"); return false; }
    std::string tmp(buf.data());
    std::string proj = tmp + "/proj";
    mkdir(proj.c_str(), 0755);

    haicode::Database db(tmp + "/e2e.db");
    db.migrate();
    haicode::SessionStore store(db);
    auto provider = std::make_shared<ToolCallProvider>();
    provider->pending.id = "c1";
    provider->pending.name = "write";
    provider->pending.input = {{"path", proj + "/target.txt"},
                               {"content", "must not land"}};
    haicode::ProviderRegistry registry;
    registry.register_provider(provider);
    haicode::ToolRegistry tools;
    haicode::register_builtin_tools(tools);
    haicode::PermissionGate perms;
    perms.set_rules({{"*", "*", haicode::PermissionEffect::Allow}});
    haicode::SessionEventBus bus;
    haicode::AppConfig cfg;
    cfg.model = "tc-model";
    cfg.provider = "toolcall";
    cfg.autoname_sessions = false;

    {
        haicode::SessionEngine engine(store, registry, tools, perms, bus, cfg);
        std::string sid = engine.create_session(proj, "build", "tc-model",
                                                "toolcall");
        engine.set_mode(sid, haicode::SessionMode::Chat);
        engine.submit_prompt(sid, "make a file");

        CHECK(wait_for_rows(store, sid, 4),
              "turn must complete: user + assistant(tool_calls) + tool_result"
              " + final assistant text");
        std::ifstream f(proj + "/target.txt");
        CHECK(!f.is_open(), "Chat mode must not create the target file");
        std::string out = first_tool_result_output(store, sid);
        CHECK(out.find("mode restriction") != std::string::npos,
              "tool_result should carry the mode-restriction error, got: " + out);
        CHECK(out.find("write") != std::string::npos,
              "error should name the blocked tool");
    }
    std::string rm = "rm -rf " + tmp;
    system(rm.c_str());
    std::cout << "[OK] e2e Chat mode blocks a provider-emitted write\n";
    return true;
}

// Build-mode control for the above: identical provider + rules, Build mode —
// the write executes and lands on disk.
static bool test_build_mode_write_executes_e2e() {
    std::string tmpl = "/tmp/hc_test_build_e2e_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data())) { CHECK(false, "mkdtemp failed"); return false; }
    std::string tmp(buf.data());
    std::string proj = tmp + "/proj";
    mkdir(proj.c_str(), 0755);

    haicode::Database db(tmp + "/e2e.db");
    db.migrate();
    haicode::SessionStore store(db);
    auto provider = std::make_shared<ToolCallProvider>();
    provider->pending.id = "c1";
    provider->pending.name = "write";
    provider->pending.input = {{"path", proj + "/target.txt"},
                               {"content", "landed"}};
    haicode::ProviderRegistry registry;
    registry.register_provider(provider);
    haicode::ToolRegistry tools;
    haicode::register_builtin_tools(tools);
    haicode::PermissionGate perms;
    perms.set_rules({{"*", "*", haicode::PermissionEffect::Allow}});
    haicode::SessionEventBus bus;
    haicode::AppConfig cfg;
    cfg.model = "tc-model";
    cfg.provider = "toolcall";
    cfg.autoname_sessions = false;
    cfg.default_mode = "build";  // AppConfig defaults to plan; this is the Build control

    {
        haicode::SessionEngine engine(store, registry, tools, perms, bus, cfg);
        std::string sid = engine.create_session(proj, "build", "tc-model",
                                                "toolcall");
        engine.submit_prompt(sid, "make a file");

        CHECK(wait_for_rows(store, sid, 4), "turn must complete in Build mode");
        std::ifstream f(proj + "/target.txt");
        CHECK(f.is_open(), "Build mode must execute the write");
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        CHECK(content == "landed", "file content mismatch: " + content);
    }
    std::string rm = "rm -rf " + tmp;
    system(rm.c_str());
    std::cout << "[OK] e2e Build mode control executes the write\n";
    return true;
}

// Review #6 e2e: a failing bash command must persist its captured output
// alongside the exit status, so the model can read the diagnostics.
static bool test_bash_failure_output_persisted_e2e() {
    std::string tmpl = "/tmp/hc_test_bashfail_e2e_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data())) { CHECK(false, "mkdtemp failed"); return false; }
    std::string tmp(buf.data());
    std::string proj = tmp + "/proj";
    mkdir(proj.c_str(), 0755);

    haicode::Database db(tmp + "/e2e.db");
    db.migrate();
    haicode::SessionStore store(db);
    auto provider = std::make_shared<ToolCallProvider>();
    provider->pending.id = "c1";
    provider->pending.name = "bash";
    provider->pending.input = {{"command", "echo boom; exit 3"}};
    haicode::ProviderRegistry registry;
    registry.register_provider(provider);
    haicode::ToolRegistry tools;
    haicode::register_builtin_tools(tools);
    haicode::PermissionGate perms;
    perms.set_rules({{"*", "*", haicode::PermissionEffect::Allow}});
    haicode::SessionEventBus bus;
    haicode::AppConfig cfg;
    cfg.model = "tc-model";
    cfg.provider = "toolcall";
    cfg.autoname_sessions = false;
    cfg.default_mode = "build";  // bash is not in the plan allowlist

    {
        haicode::SessionEngine engine(store, registry, tools, perms, bus, cfg);
        std::string sid = engine.create_session(proj, "build", "tc-model",
                                                "toolcall");
        engine.submit_prompt(sid, "run it");

        CHECK(wait_for_rows(store, sid, 4), "turn must complete");
        std::string out = first_tool_result_output(store, sid);
        CHECK(out.find("Exit code: 3") != std::string::npos,
              "persisted tool_result must carry the exit status, got: " + out);
        CHECK(out.find("boom") != std::string::npos,
              "persisted tool_result must carry the command output, got: " + out);
    }
    std::string rm = "rm -rf " + tmp;
    system(rm.c_str());
    std::cout << "[OK] e2e bash failure persists exit status + output\n";
    return true;
}

// Review #7 e2e: flipping the session's provider mid-run must re-fetch the
// provider object (connection, cancel map), not just update the strings.
// prov-a's first stream() flips the session to prov-b inside the callback —
// deterministic, it lands before step 2's re-read — and emits a todo_write
// call so the loop continues. prov-b records what it was sent.
class SwitchingProvider : public haicode::Provider {
public:
    std::string id() const override { return "prov-a"; }
    void cancel(const std::string& stream_token = "") override {}
    std::vector<std::string> list_models(std::string&) override {
        return {"model-a"};
    }
    void stream(const haicode::LLMRequest&, haicode::StreamCallbacks cb, const std::string& stream_token = "") override {
        ++calls;
        if (calls == 1) {
            store->update_provider_model(sid, "prov-b", "model-b");
            haicode::ToolCall tc;
            tc.id = "sw1";
            tc.name = "todo_write";
            tc.input = {{"todos", nlohmann::json::array({
                {{"content", "step"}, {"activeForm", "stepping"},
                 {"status", "completed"}}})}};
            cb.on_finish(haicode::FinishReason::ToolUse, {}, {tc});
            return;
        }
        cb.on_text_delta("t", "a-done");
        cb.on_finish(haicode::FinishReason::EndTurn, {}, {});
    }
    haicode::SessionStore* store = nullptr;
    std::string sid;
    int calls = 0;
};

class RecordingProvider : public haicode::Provider {
public:
    std::string id() const override { return "prov-b"; }
    void cancel(const std::string& stream_token = "") override {}
    std::vector<std::string> list_models(std::string&) override {
        return {"model-b"};
    }
    void stream(const haicode::LLMRequest& req,
                haicode::StreamCallbacks cb, const std::string& stream_token = "") override {
        ++calls;
        last_model = req.model_id;
        cb.on_text_delta("t", "b-done");
        cb.on_finish(haicode::FinishReason::EndTurn, {}, {});
    }
    int calls = 0;
    std::string last_model;
};

static bool test_provider_switch_mid_loop_e2e() {
    std::string tmpl = "/tmp/hc_test_provswitch_e2e_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data())) { CHECK(false, "mkdtemp failed"); return false; }
    std::string tmp(buf.data());
    std::string proj = tmp + "/proj";
    mkdir(proj.c_str(), 0755);

    haicode::Database db(tmp + "/e2e.db");
    db.migrate();
    haicode::SessionStore store(db);
    auto a = std::make_shared<SwitchingProvider>();
    auto b = std::make_shared<RecordingProvider>();
    haicode::ProviderRegistry registry;
    registry.register_provider(a);
    registry.register_provider(b);
    haicode::ToolRegistry tools;
    haicode::register_builtin_tools(tools);
    haicode::PermissionGate perms;
    perms.set_rules({{"*", "*", haicode::PermissionEffect::Allow}});
    haicode::SessionEventBus bus;
    haicode::AppConfig cfg;
    cfg.model = "model-a";
    cfg.provider = "prov-a";
    cfg.autoname_sessions = false;
    cfg.default_mode = "build";  // todo_write alone wouldn't need Build, but
                                 // the flip must hold for any tool-bearing turn

    {
        haicode::SessionEngine engine(store, registry, tools, perms, bus, cfg);
        std::string sid = engine.create_session(proj, "build", "model-a",
                                                "prov-a");
        a->store = &store;
        a->sid = sid;
        engine.submit_prompt(sid, "switch us");

        CHECK(wait_for_rows(store, sid, 4),
              "turn must complete: user + assistant(tc) + tool_result + "
              "final assistant text");
        CHECK(a->calls == 1, "prov-a should have been called exactly once");
        CHECK(b->calls == 1, "prov-b must take over after the mid-loop switch");
        CHECK(b->last_model == "model-b",
              "prov-b must receive the updated model_id, got: " + b->last_model);
    }
    std::string rm = "rm -rf " + tmp;
    system(rm.c_str());
    std::cout << "[OK] e2e mid-loop provider switch re-fetches provider object\n";
    return true;
}

// Inspection item e2e: interrupt() must release a worker blocked in the
// ask_user wait. The provider emits an ask_user call; once the question is
// pending, the test interrupts the session. Without the fix the worker
// deadlocks in asking_cv_.wait until engine destruction.
static bool test_interrupt_releases_ask_wait_e2e() {
    std::string tmpl = "/tmp/hc_test_askint_e2e_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data())) { CHECK(false, "mkdtemp failed"); return false; }
    std::string tmp(buf.data());
    std::string proj = tmp + "/proj";
    mkdir(proj.c_str(), 0755);

    haicode::Database db(tmp + "/e2e.db");
    db.migrate();
    haicode::SessionStore store(db);
    auto provider = std::make_shared<ToolCallProvider>();
    provider->pending.id = "ask1";
    provider->pending.name = "ask_user";
    provider->pending.input = {{"question", "Continue?"},
                               {"options", nlohmann::json::array({"yes", "no"})}};
    haicode::ProviderRegistry registry;
    registry.register_provider(provider);
    haicode::ToolRegistry tools;
    haicode::register_builtin_tools(tools);
    haicode::PermissionGate perms;
    perms.set_rules({{"*", "*", haicode::PermissionEffect::Allow}});
    haicode::SessionEventBus bus;
    haicode::AppConfig cfg;
    cfg.model = "tc-model";
    cfg.provider = "toolcall";
    cfg.autoname_sessions = false;
    cfg.default_mode = "build";

    {
        haicode::SessionEngine engine(store, registry, tools, perms, bus, cfg);
        std::string sid = engine.create_session(proj, "build", "tc-model",
                                                "toolcall");

        std::atomic<bool> asked{false};
        bus.subscribe(haicode::events::EventType::AskUserRequested,
                      [&](const nlohmann::json&) { asked = true; });

        engine.submit_prompt(sid, "ask me something");

        // Wait for the question to be pending, then interrupt from the main
        // thread (mirrors the GUI's Stop button).
        bool saw_ask = false;
        for (int i = 0; i < 100 && !saw_ask; ++i) {
            if (asked.load()) saw_ask = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        CHECK(saw_ask, "AskUserRequested must arrive");

        engine.interrupt(sid);

        bool ended = false;
        for (int i = 0; i < 100 && !ended; ++i) {
            if (!engine.is_running(sid)) ended = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        CHECK(ended, "interrupt() must release the ask_user wait and end the loop");
        CHECK(provider->calls == 1,
              "provider must not be called again after the interrupt");

        // The placeholder row was overwritten with the interrupted answer.
        bool saw_interrupted_row = false;
        for (const auto& m : store.load_messages(sid)) {
            if (m.type != "tool_result") continue;
            auto j = nlohmann::json::parse(m.data_json, nullptr, false);
            if (j.is_object() && j.value("call_id", "") == "ask1"
                    && j.value("output", "").find("(interrupted)")
                           != std::string::npos)
                saw_interrupted_row = true;
        }
        CHECK(saw_interrupted_row,
              "tool_result row for the ask must carry the (interrupted) answer");
    }
    std::string rm = "rm -rf " + tmp;
    system(rm.c_str());
    std::cout << "[OK] e2e interrupt() releases the ask_user wait\n";
    return true;
}

// Inspection item e2e: the build hook must run with cwd = the session's
// project directory (not the app cwd), under a timeout, with capped output.
// The failing case (`pwd; echo marker; exit 3`) proves all three visible
// properties at once: the pwd output names the session dir, the marker and
// exit status land in the tool result, and success flips to false with the
// [build_hook] prefix. The success case proves the hook stays silent on
// green builds (output not appended, BuildHookResult success=true).
static bool test_build_hook_cwd_and_failure_e2e() {
    std::string tmpl = "/tmp/hc_test_buildhook_e2e_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data())) { CHECK(false, "mkdtemp failed"); return false; }
    std::string tmp(buf.data());
    std::string proj = tmp + "/proj";
    mkdir(proj.c_str(), 0755);

    // -- failing hook: cwd + exit status + marker propagation --
    {
        haicode::Database db(tmp + "/fail.db");
        db.migrate();
        haicode::SessionStore store(db);
        auto provider = std::make_shared<ToolCallProvider>();
        provider->pending.id = "w1";
        provider->pending.name = "write";
        provider->pending.input = {{"path", proj + "/out.txt"},
                                   {"content", "x"}};
        haicode::ProviderRegistry registry;
        registry.register_provider(provider);
        haicode::ToolRegistry tools;
        haicode::register_builtin_tools(tools);
        haicode::PermissionGate perms;
        perms.set_rules({{"*", "*", haicode::PermissionEffect::Allow}});
        haicode::SessionEventBus bus;
        haicode::AppConfig cfg;
        cfg.model = "tc-model";
        cfg.provider = "toolcall";
        cfg.autoname_sessions = false;
        cfg.default_mode = "build";
        cfg.build_command = "pwd; echo HOOKMARK; exit 3";

        std::atomic<bool> hook_event{false};
        std::atomic<int> hook_exit{0};
        bus.subscribe(haicode::events::EventType::BuildHookResult,
                      [&](const nlohmann::json& d) {
                          hook_event = true;
                          hook_exit = d.value("exit_code", -1);
                      });

        std::string sid;
        {
            haicode::SessionEngine engine(store, registry, tools, perms, bus, cfg);
            sid = engine.create_session(proj, "build", "tc-model",
                                        "toolcall");
            engine.submit_prompt(sid, "write it");
            CHECK(wait_for_rows(store, sid, 4), "turn must complete");
        }
        CHECK(hook_event.load(), "BuildHookResult must be published");
        CHECK(hook_exit.load() == 3, "hook exit code must be 3");

        std::string out = first_tool_result_output(store, sid);
        CHECK(out.find(proj) != std::string::npos,
              "hook output must contain pwd of the session dir, got: " + out);
        CHECK(out.find("HOOKMARK") != std::string::npos,
              "hook output must be captured, got: " + out);
        CHECK(out.find("[build_hook]") != std::string::npos,
              "failure must carry the [build_hook] marker, got: " + out);
        CHECK(out.find("exit 3") != std::string::npos,
              "failure must name the exit status, got: " + out);
    }

    // -- succeeding hook: silent on green builds --
    {
        haicode::Database db(tmp + "/ok.db");
        db.migrate();
        haicode::SessionStore store(db);
        auto provider = std::make_shared<ToolCallProvider>();
        provider->pending.id = "w2";
        provider->pending.name = "write";
        provider->pending.input = {{"path", proj + "/ok.txt"},
                                   {"content", "y"}};
        haicode::ProviderRegistry registry;
        registry.register_provider(provider);
        haicode::ToolRegistry tools;
        haicode::register_builtin_tools(tools);
        haicode::PermissionGate perms;
        perms.set_rules({{"*", "*", haicode::PermissionEffect::Allow}});
        haicode::SessionEventBus bus;
        haicode::AppConfig cfg;
        cfg.model = "tc-model";
        cfg.provider = "toolcall";
        cfg.autoname_sessions = false;
        cfg.default_mode = "build";
        cfg.build_command = "pwd";

        std::atomic<bool> hook_ok{false};
        bus.subscribe(haicode::events::EventType::BuildHookResult,
                      [&](const nlohmann::json& d) {
                          hook_ok = d.value("success", false);
                      });

        std::string sid;
        {
            haicode::SessionEngine engine(store, registry, tools, perms, bus, cfg);
            sid = engine.create_session(proj, "build", "tc-model", "toolcall");
            engine.submit_prompt(sid, "write it");
            CHECK(wait_for_rows(store, sid, 4), "turn must complete");
        }
        CHECK(hook_ok.load(), "successful hook must publish success=true");
        std::string out = first_tool_result_output(store, sid);
        CHECK(out.find("[build_hook]") == std::string::npos,
              "green build must not pollute the tool result, got: " + out);
    }

    std::string rm = "rm -rf " + tmp;
    system(rm.c_str());
    std::cout << "[OK] e2e build hook runs in session dir, reports failure\n";
    return true;
}

// ============================================================
// Per-session cancel scoping (stream token)
// ============================================================

// Two sessions share ONE provider object (ProviderRegistry hands the same
// shared_ptr to every session). interrupt() must cancel only the interrupted
// session's in-flight stream: Provider::stream/cancel carry a per-run token
// and the engine cancels exactly that token. Regression: the old unscoped
// Provider::cancel() aborted BOTH sessions' transfers.
class SharedTokenProvider : public haicode::Provider {
public:
    std::string id() const override { return "shared"; }
    std::vector<std::string> list_models(std::string&) override {
        return {"shared-model"};
    }
    void stream(const haicode::LLMRequest&, haicode::StreamCallbacks cb,
                const std::string& stream_token = "") override {
        {
            std::lock_guard<std::mutex> g(m);
            ++entered[stream_token];
        }
        entered_cv.notify_all();
        {
            std::unique_lock<std::mutex> lk(m);
            release_cv.wait_for(lk, std::chrono::seconds(10),
                                [&] { return released.count(stream_token) != 0; });
        }
        bool was_cancelled;
        {
            std::lock_guard<std::mutex> g(m);
            was_cancelled = cancelled_tokens.count(stream_token) != 0;
            exited.push_back(stream_token);
            if (!was_cancelled) finished.push_back(stream_token);
        }
        // Mirror the real providers: an interrupted stream returns silently
        // — no on_error, no on_finish — so the loop breaks on its own flag.
        if (!was_cancelled && cb.on_finish)
            cb.on_finish(haicode::FinishReason::Stopped, {}, {});
    }
    void cancel(const std::string& stream_token = "") override {
        std::lock_guard<std::mutex> g(m);
        for (auto& [tok, _] : entered) {
            if (stream_token.empty() || tok == stream_token) {
                cancelled_tokens.insert(tok);
                released.insert(tok);
            }
        }
        release_cv.notify_all();
    }
    std::mutex m;
    std::condition_variable entered_cv;
    std::condition_variable release_cv;
    std::map<std::string, int> entered;
    std::set<std::string> released;
    std::set<std::string> cancelled_tokens;
    std::vector<std::string> exited;       // every stream() exit, cancelled or not
    std::vector<std::string> finished;     // streams that reached on_finish
};

static bool test_interrupt_scoped_to_session_token() {
    static const char* kDb = "/tmp/haicode_test_cancel_scope.db";
    remove(kDb);
    haicode::Database db(kDb);
    db.migrate();
    haicode::SessionStore store(db);
    auto provider = std::make_shared<SharedTokenProvider>();
    haicode::ProviderRegistry registry;
    registry.register_provider(provider);
    haicode::ToolRegistry tools;
    haicode::PermissionGate perms;
    haicode::SessionEventBus bus;
    haicode::AppConfig cfg;
    cfg.model = "shared-model";
    cfg.provider = "shared";
    cfg.autoname_sessions = false;
    cfg.default_mode = "build";

    haicode::SessionEngine engine(store, registry, tools, perms, bus, cfg);
    std::string sid_a = engine.create_session("/tmp/projA", "build",
                                              "shared-model", "shared");
    std::string sid_b = engine.create_session("/tmp/projB", "build",
                                              "shared-model", "shared");
    engine.submit_prompt(sid_a, "a");
    engine.submit_prompt(sid_b, "b");

    bool both_in = false;
    {
        std::unique_lock<std::mutex> lk(provider->m);
        both_in = provider->entered_cv.wait_for(
            lk, std::chrono::seconds(5),
            [&] { return provider->entered.size() >= 2; });
    }
    CHECK(both_in, "both sessions must be parked in stream() on the SHARED provider");

    // Recover the two run tokens ("s:<session_id>:r<seq>", seq is an
    // engine-wide counter) by the session id embedded in each.
    std::string tok_a, tok_b;
    {
        std::lock_guard<std::mutex> g(provider->m);
        for (auto& [tok, n] : provider->entered) {
            if (tok.find(sid_a) != std::string::npos) tok_a = tok;
            if (tok.find(sid_b) != std::string::npos) tok_b = tok;
        }
    }
    CHECK(!tok_a.empty() && !tok_b.empty() && tok_a != tok_b,
          "each session must stream under its own distinct run token: " +
          tok_a + " / " + tok_b);

    engine.interrupt(sid_a);

    bool a_cancelled = false, b_untouched = false;
    {
        std::unique_lock<std::mutex> lk(provider->m);
        a_cancelled = provider->release_cv.wait_for(
            lk, std::chrono::seconds(2),
            [&] { return provider->cancelled_tokens.count(tok_a) != 0; });
        b_untouched = provider->cancelled_tokens.count(tok_b) == 0;
    }
    CHECK(a_cancelled, "interrupt() cancelled the interrupted session's token");
    CHECK(b_untouched, "interrupt() must not cancel the concurrent session's token");

    {
        std::lock_guard<std::mutex> g(provider->m);
        CHECK(provider->exited.size() <= 1 && provider->finished.empty(),
              "the other session's stream must still be running");
    }

    {
        std::lock_guard<std::mutex> g(provider->m);
        provider->released.insert(tok_b);
    }
    provider->release_cv.notify_all();
    bool b_finished = false;
    {
        std::unique_lock<std::mutex> lk(provider->m);
        b_finished = provider->release_cv.wait_for(
            lk, std::chrono::seconds(5),
            [&] { return !provider->finished.empty(); });
    }
    CHECK(b_finished, "the concurrent session completed after the interrupt");
    {
        std::lock_guard<std::mutex> g(provider->m);
        CHECK(provider->exited.size() == 2,
              "both streams eventually exit: " +
              std::to_string(provider->exited.size()));
        CHECK(provider->finished.size() == 1 && provider->finished[0] == tok_b,
              "only the non-interrupted session's stream reaches on_finish");
    }

    std::remove(kDb);
    std::cout << "[OK] interrupt() is scoped to the session's stream token\n";
    return true;
}

// Inspection item: concurrent sessions must run their loops in parallel.
// Two sessions, two independent blocking providers — both workers must be
// parked inside stream() AT THE SAME TIME. With the old whole-body
// shutdown_mu_ the second runner serialized behind the first and this test
// timed out. Also pins the SQLite serialized-mode assumption the parallel
// loops rely on (they share one connection).
class IdBlockingProvider : public BlockingProvider {
public:
    explicit IdBlockingProvider(std::string pid) : pid_(std::move(pid)) {}
    std::string id() const override { return pid_; }
private:
    std::string pid_;
};

static bool test_concurrent_sessions_run_in_parallel() {
    CHECK(sqlite3_threadsafe() == 1,
          "SQLite must be built serialized (threadsafe==1) for concurrent "
          "sessions sharing one connection");

    static const char* kDb = "/tmp/haicode_test_parallel.db";
    remove(kDb);
    haicode::Database db(kDb);
    db.migrate();
    haicode::SessionStore store(db);
    auto pa = std::make_shared<IdBlockingProvider>("par-a");
    auto pb = std::make_shared<IdBlockingProvider>("par-b");
    haicode::ProviderRegistry registry;
    registry.register_provider(pa);
    registry.register_provider(pb);
    haicode::ToolRegistry tools;
    haicode::PermissionGate perms;
    haicode::SessionEventBus bus;
    haicode::AppConfig cfg;
    cfg.model = "blocking-model";
    cfg.provider = "par-a";
    cfg.autoname_sessions = false;
    cfg.default_mode = "build";

    {
        haicode::SessionEngine engine(store, registry, tools, perms, bus, cfg);
        std::string sid_a = engine.create_session("/tmp/projA", "build",
                                                  "blocking-model", "par-a");
        std::string sid_b = engine.create_session("/tmp/projB", "build",
                                                  "blocking-model", "par-b");
        engine.submit_prompt(sid_a, "a");
        engine.submit_prompt(sid_b, "b");

        // Wait for BOTH workers to be parked in stream() simultaneously.
        // Sequential waits suffice: under the old global mutex session B's
        // runner could not even enter stream() while A was parked, so the
        // second wait times out.
        bool a_in = false, b_in = false;
        {
            std::unique_lock<std::mutex> lk(pa->m);
            a_in = pa->entered_cv.wait_for(lk, std::chrono::seconds(5),
                                           [&] { return pa->entered > 0; });
        }
        {
            std::unique_lock<std::mutex> lk(pb->m);
            b_in = pb->entered_cv.wait_for(lk, std::chrono::seconds(5),
                                           [&] { return pb->entered > 0; });
        }
        CHECK(a_in && b_in, "both sessions must run their loops in parallel");

        // Release both; scope exit runs ~SessionEngine, which joins both.
        {
            std::lock_guard<std::mutex> g(pa->m);
            pa->release_flag = true;
            pa->release_cv.notify_all();
        }
        {
            std::lock_guard<std::mutex> g(pb->m);
            pb->release_flag = true;
            pb->release_cv.notify_all();
        }
    }
    CHECK(pa->entered == 1 && pb->entered == 1,
          "each provider entered stream() exactly once");
    CHECK(pa->exited == 1 && pb->exited == 1,
          "each provider exited stream() before the dtor returned");

    std::remove(kDb);
    std::cout << "[OK] concurrent sessions run their loops in parallel\n";
    return true;
}

// ============================================================
// Renewable step budget e2e
// ============================================================

// Drives the full engine loop from a script: a generator invoked once per
// stream() call with the call index. Returning an empty vector ends the
// turn; otherwise the calls are emitted with ToolUse. Generalizes
// ToolCallProvider to multi-step scenarios (and unbounded ones — a
// generator that never returns empty keeps the loop going forever, which
// is exactly what the exhaustion tests need).
class ScriptedProvider : public haicode::Provider {
public:
    using Script = std::function<std::vector<haicode::ToolCall>(int)>;
    explicit ScriptedProvider(Script s) : script_(std::move(s)) {}
    std::string id() const override { return "scripted"; }
    void cancel(const std::string& stream_token = "") override {}
    std::vector<std::string> list_models(std::string&) override {
        return {"sc-model"};
    }
    void stream(const haicode::LLMRequest&, haicode::StreamCallbacks cb, const std::string& stream_token = "") override {
        int idx = calls++;
        auto tcs = script_(idx);
        if (tcs.empty()) {
            cb.on_text_delta("t", "done");
            cb.on_finish(haicode::FinishReason::EndTurn, {}, {});
            return;
        }
        for (size_t i = 0; i < tcs.size(); ++i)
            tcs[i].id = "sc" + std::to_string(idx) + "_" + std::to_string(i);
        cb.on_finish(haicode::FinishReason::ToolUse, {}, tcs);
    }
    Script script_;
    int calls = 0;
};

static nlohmann::json todo_list_json(
        const std::vector<std::pair<std::string, std::string>>& items) {
    // items: (content, status)
    auto arr = nlohmann::json::array();
    for (auto& [content, status] : items)
        arr.push_back({{"content", content},
                       {"activeForm", "working on " + content},
                       {"status", status}});
    return {{"todos", arr}};
}

static haicode::ToolCall todo_call(const std::vector<std::pair<std::string,
                                                        std::string>>& items) {
    haicode::ToolCall tc;
    tc.name = "todo_write";
    tc.input = todo_list_json(items);
    return tc;
}

static haicode::ToolCall bash_call(const std::string& cmd) {
    haicode::ToolCall tc;
    tc.name = "bash";
    tc.input = {{"command", cmd}};
    return tc;
}

// Captures StepFailed error strings (published from worker threads).
struct ErrorRecorder {
    std::mutex m;
    std::vector<std::string> errors;
    void subscribe(haicode::SessionEventBus& bus) {
        bus.subscribe(haicode::events::EventType::StepFailed,
                      [this](const nlohmann::json& d) {
                          std::lock_guard<std::mutex> g(m);
                          errors.push_back(d.value("error", ""));
                      });
    }
    bool has_containing(const std::string& needle) {
        std::lock_guard<std::mutex> g(m);
        for (auto& e : errors)
            if (e.find(needle) != std::string::npos) return true;
        return false;
    }
    size_t count() {
        std::lock_guard<std::mutex> g(m);
        return errors.size();
    }
};

static bool wait_for_errors(ErrorRecorder& rec, size_t want) {
    for (int i = 0; i < 200; ++i) {
        if (rec.count() >= want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return rec.count() >= want;
}

// Shared rig: temp project, scripted provider, allow-all rules, an agent
// with the given max_steps.
struct BudgetRig {
    std::string tmp;
    std::string proj;
    std::unique_ptr<haicode::Database> db;
    std::unique_ptr<haicode::SessionStore> store;
    std::shared_ptr<ScriptedProvider> provider;
    haicode::ProviderRegistry registry;
    haicode::ToolRegistry tools;
    haicode::PermissionGate perms;
    haicode::SessionEventBus bus;
    haicode::AppConfig cfg;
    ErrorRecorder errors;

    BudgetRig(ScriptedProvider::Script script, int max_steps) {
        std::string tmpl = "/tmp/hc_test_budget_XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (!mkdtemp(buf.data())) return;
        tmp = buf.data();
        proj = tmp + "/proj";
        mkdir(proj.c_str(), 0755);
        db = std::make_unique<haicode::Database>(tmp + "/e2e.db");
        db->migrate();
        store = std::make_unique<haicode::SessionStore>(*db);
        provider = std::make_shared<ScriptedProvider>(std::move(script));
        registry.register_provider(provider);
        haicode::register_builtin_tools(tools);
        perms.set_rules({{"*", "*", haicode::PermissionEffect::Allow}});
        cfg.model = "sc-model";
        cfg.provider = "scripted";
        cfg.autoname_sessions = false;
        cfg.default_mode = "build";
        cfg.agents["build"].max_steps = max_steps;
        errors.subscribe(bus);
    }
    ~BudgetRig() {
        store.reset();
        db.reset();
        if (!tmp.empty()) {
            std::string rm = "rm -rf " + tmp;
            system(rm.c_str());
        }
    }
    bool ok() const { return db != nullptr; }
};

// 1. Renewal works: max_steps=3, four seeded todos completed one per
// todo_write with filler bash steps between. Without renewal the turn
// would die at iteration 3; with it, all 8 scripted iterations run and
// the turn ends normally.
static bool test_budget_renews_on_todo_completion_e2e() {
    BudgetRig rig([](int idx) -> std::vector<haicode::ToolCall> {
        if (idx >= 7) return {};  // end turn after 4 todo_writes + 3 fillers
        if (idx % 2 == 0) {
            int done = idx / 2 + 1;  // progressively complete item 1..4
            std::vector<std::pair<std::string, std::string>> items;
            for (int k = 1; k <= 4; ++k)
                items.emplace_back("item" + std::to_string(k),
                                   k <= done ? "completed" : "pending");
            return {todo_call(items)};
        }
        return {bash_call("echo filler")};
    }, 3);
    CHECK(rig.ok(), "mkdtemp failed");

    {
        haicode::SessionEngine engine(*rig.store, rig.registry, rig.tools,
                                      rig.perms, rig.bus, rig.cfg);
        std::string sid = engine.create_session(rig.proj, "build",
                                                "sc-model", "scripted");
        std::vector<haicode::Todo> seed;
        for (int k = 1; k <= 4; ++k) {
            haicode::Todo td;
            td.content = "item" + std::to_string(k);
            td.active_form = "working on item" + std::to_string(k);
            seed.push_back(td);
        }
        engine.seed_todos(sid, seed);

        engine.submit_prompt(sid, "do the work");
        // user + 7*(assistant+tool_result) + final assistant text
        CHECK(wait_for_rows(*rig.store, sid, 16),
              "turn must run all 8 iterations and end normally");
    }
    CHECK(rig.provider->calls == 8,
          "provider must be called 8 times (got "
          + std::to_string(rig.provider->calls) + ")");
    CHECK(!rig.errors.has_containing("budget exhausted")
              && !rig.errors.has_containing("ceiling reached"),
          "no exhaustion error may fire when todos keep completing");
    std::cout << "[OK] e2e budget renews on each new todo completion\n";
    return true;
}

// 2. No renewal without new completions: two todos were already completed
// in a prior turn (seeded), so the loop's high-water starts at 2. The
// script re-completes the same two forever — the turn must stop at the
// window budget with the exhausted message.
static bool test_no_renewal_without_new_completions_e2e() {
    BudgetRig rig([](int idx) -> std::vector<haicode::ToolCall> {
        if (idx % 2 == 0)
            return {todo_call({{"a", "completed"}, {"b", "completed"}})};
        return {bash_call("echo spin")};
    }, 3);
    CHECK(rig.ok(), "mkdtemp failed");

    {
        haicode::SessionEngine engine(*rig.store, rig.registry, rig.tools,
                                      rig.perms, rig.bus, rig.cfg);
        std::string sid = engine.create_session(rig.proj, "build",
                                                "sc-model", "scripted");
        std::vector<haicode::Todo> seed;
        for (const char* name : {"a", "b"}) {
            haicode::Todo td;
            td.content = name;
            td.active_form = std::string("working on ") + name;
            td.status = "completed";
            seed.push_back(td);
        }
        engine.seed_todos(sid, seed);

        engine.submit_prompt(sid, "loop forever");
        CHECK(wait_for_rows(*rig.store, sid, 7),
              "3 iterations must persist user + 3*(assistant+tool_result)");
    }
    CHECK(rig.provider->calls == 3,
          "re-completing already-completed todos must not renew: exactly "
          "max_steps=3 calls expected (got "
          + std::to_string(rig.provider->calls) + ")");
    CHECK(wait_for_errors(rig.errors, 1), "exhaustion StepFailed must fire");
    CHECK(rig.errors.has_containing("Turn step budget exhausted (3 steps"),
          "error must be the budget-exhausted message");
    std::cout << "[OK] e2e re-completing old todos never renews the budget\n";
    return true;
}

// 3. Flip-flop guard: completing an item renews once; flipping it
// completed→pending→completed never exceeds the high-water mark again, so
// the turn stops at the window budget.
static bool test_flipflop_does_not_renew_e2e() {
    BudgetRig rig([](int idx) -> std::vector<haicode::ToolCall> {
        return {todo_call({{"only", idx % 2 == 0 ? "completed" : "pending"}})};
    }, 3);
    CHECK(rig.ok(), "mkdtemp failed");

    {
        haicode::SessionEngine engine(*rig.store, rig.registry, rig.tools,
                                      rig.perms, rig.bus, rig.cfg);
        std::string sid = engine.create_session(rig.proj, "build",
                                                "sc-model", "scripted");
        haicode::Todo td;
        td.content = "only";
        td.active_form = "working on only";
        engine.seed_todos(sid, {td});

        engine.submit_prompt(sid, "flip flop");
        CHECK(wait_for_rows(*rig.store, sid, 7),
              "3 iterations must persist before the budget runs out");
    }
    CHECK(rig.provider->calls == 3,
          "status flip-flopping must not renew past the first completion "
          "(got " + std::to_string(rig.provider->calls) + " calls)");
    CHECK(wait_for_errors(rig.errors, 1), "exhaustion StepFailed must fire");
    CHECK(rig.errors.has_containing("Turn step budget exhausted (3 steps"),
          "error must be the budget-exhausted message");
    std::cout << "[OK] e2e completed→pending→completed flip-flop never renews\n";
    return true;
}

// 4. Hard ceiling: the script invents and completes one MORE todo every
// step (idx+1 completed items), so every iteration legitimately renews
// the window — yet the turn must still terminate at
// kStepCeilingMultiplier * max_steps = 8 iterations.
static bool test_hard_ceiling_terminates_e2e() {
    BudgetRig rig([](int idx) -> std::vector<haicode::ToolCall> {
        std::vector<std::pair<std::string, std::string>> items;
        for (int k = 0; k <= idx; ++k)
            items.emplace_back("trivial" + std::to_string(k), "completed");
        return {todo_call(items)};
    }, 2);
    CHECK(rig.ok(), "mkdtemp failed");

    {
        haicode::SessionEngine engine(*rig.store, rig.registry, rig.tools,
                                      rig.perms, rig.bus, rig.cfg);
        std::string sid = engine.create_session(rig.proj, "build",
                                                "sc-model", "scripted");
        engine.submit_prompt(sid, "farm renewals forever");
        // user + 8*(assistant+tool_result)
        CHECK(wait_for_rows(*rig.store, sid, 17),
              "exactly 8 iterations must run before the ceiling");
    }
    CHECK(rig.provider->calls == 8,
          "ceiling must cap the turn at 4*max_steps=8 calls (got "
          + std::to_string(rig.provider->calls) + ")");
    CHECK(wait_for_errors(rig.errors, 1), "ceiling StepFailed must fire");
    CHECK(rig.errors.has_containing("Hard step ceiling reached (8 steps"),
          "error must be the hard-ceiling message");
    std::cout << "[OK] e2e hard ceiling terminates endless renewal farming\n";
    return true;
}

int main() {
    std::cout << "=== Step Budget Gate + Escalation Tests ===\n";

    bool ok = true;
    ok &= test_above_gate();
    ok &= test_gate_boundary();
    ok &= test_half_max_rule();
    ok &= test_small_budget();
    ok &= test_floor();
    ok &= test_firm_tier();
    ok &= test_critical_tier();
    ok &= test_placeholder_substitution();
    ok &= test_destructor_joins_running_loop();
    ok &= test_chat_mode_write_blocked_e2e();
    ok &= test_build_mode_write_executes_e2e();
    ok &= test_bash_failure_output_persisted_e2e();
    ok &= test_provider_switch_mid_loop_e2e();
    ok &= test_interrupt_releases_ask_wait_e2e();
    ok &= test_build_hook_cwd_and_failure_e2e();
    ok &= test_interrupt_scoped_to_session_token();
    ok &= test_concurrent_sessions_run_in_parallel();
    ok &= test_budget_renews_on_todo_completion_e2e();
    ok &= test_no_renewal_without_new_completions_e2e();
    ok &= test_flipflop_does_not_renew_e2e();
    ok &= test_hard_ceiling_terminates_e2e();

    if (ok) {
        std::cout << "\nAll step budget gate tests passed!\n";
        return 0;
    } else {
        std::cerr << "\nSome tests FAILED.\n";
        return 1;
    }
}
