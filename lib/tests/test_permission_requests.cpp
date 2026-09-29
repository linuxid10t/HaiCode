#include <haicode/permission_requests.h>
#include <haicode/tool.h>
#include <haicode/engine.h>
#include <haicode/db.h>
#include <haicode/provider.h>
#include <haicode/config.h>
#include <haicode/haicode.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << (msg) << "\n"; return false; } } while(0)

using namespace haicode;

// NOTE: CHECK contains `return false`, so it must only be used on the main
// test thread. Worker lambdas below only capture outcomes; every assertion
// runs after join(). (A lambda containing CHECK deduces a bool return type,
// and its success path then falls off the end of a non-void lambda — a trap.)

static PermissionRequest make_req(const std::string& sid,
                                  const std::string& action = "bash",
                                  const std::string& resource = "/bin/true") {
    PermissionRequest r;
    r.session_id = sid;
    r.call_id = "call_1";
    r.tool_name = "bash";
    r.action = action;
    r.resource = resource;
    r.working_dir = "/tmp";
    r.input = nlohmann::json{{"command", "/bin/true"}};
    return r;
}

// Basic approve/deny round-trip on one session.
static bool approve_and_deny_roundtrip() {
    PermissionGate gate;
    PermissionRequestBroker broker(&gate);
    broker.set_delivery_callback([](const PermissionRequest&) { return true; });

    PermissionOutcome out{};
    std::thread worker([&] {
        out = broker.submit(make_req("s1"));
    });

    // Wait for the request to appear, then approve it once-for-session.
    while (broker.pending_count() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto pending = broker.pending_requests();
    CHECK(pending.size() == 1, "one pending request");
    CHECK(pending[0].tool_name == "bash" && pending[0].input.is_object(),
          "request carries structured context");
    CHECK(broker.resolve(pending[0].id, PermissionDecision::AllowForSession),
          "resolve succeeds on first reply");

    worker.join();
    CHECK(out.effect == PermissionEffect::Allow, "worker saw Allow");
    CHECK(out.user_decided, "outcome marked user_decided");

    // The session grant must already be installed (before the waiter woke).
    const auto no_input = nlohmann::json::object();
    CHECK(gate.check("s1", "bash", "/bin/true", no_input) == PermissionEffect::Allow,
          "session grant installed before waiter wake");
    CHECK(gate.check("s1", "bash", "/bin/other", no_input) == PermissionEffect::Ask,
          "grant scoped to the exact resource");
    CHECK(gate.check("s2", "bash", "/bin/true", no_input) == PermissionEffect::Ask,
          "grant scoped to the requesting session");
    std::cout << "[OK] approve/deny roundtrip + grant-before-wake\n";
    return true;
}

// Duplicate and stale replies must never approve anything.
static bool duplicate_and_stale_replies() {
    PermissionRequestBroker broker;
    broker.set_delivery_callback([](const PermissionRequest&) { return true; });

    PermissionOutcome out{};
    std::thread worker([&] {
        out = broker.submit(make_req("s1"));
    });
    while (broker.pending_count() == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto pending = broker.pending_requests();

    // Simulate Escape/close: deny.
    CHECK(broker.resolve(pending[0].id, PermissionDecision::Deny),
          "first reply (deny) accepted");
    // Duplicate reply arrives late (window raced a queued message): refused.
    CHECK(!broker.resolve(pending[0].id, PermissionDecision::AllowOnce),
          "duplicate reply refused");
    // Stale id entirely: refused.
    CHECK(!broker.resolve("prq_999999", PermissionDecision::AllowForSession),
          "stale id refused");
    worker.join();
    CHECK(out.effect == PermissionEffect::Deny && out.user_decided,
          "worker denied after close");
    CHECK(broker.pending_count() == 0, "no requests remain pending");
    std::cout << "[OK] duplicate/stale replies cannot approve\n";
    return true;
}

// Failed GUI delivery denies the request instead of hanging the worker.
static bool failed_delivery_denies() {
    PermissionRequestBroker broker;
    broker.set_delivery_callback([](const PermissionRequest&) { return false; });

    auto out = broker.submit(make_req("s1"));
    CHECK(out.effect == PermissionEffect::Deny, "failed delivery denies");
    CHECK(out.reason.find("unavailable") != std::string::npos,
          "delivery failure reason reported");
    CHECK(broker.pending_count() == 0, "nothing left pending");
    std::cout << "[OK] failed delivery denies without hanging\n";
    return true;
}

// Two concurrent sessions: cancelling one leaves the other approvable.
static bool concurrent_session_isolation() {
    PermissionGate gate;
    PermissionRequestBroker broker(&gate);
    broker.set_delivery_callback([](const PermissionRequest&) { return true; });

    PermissionOutcome out1{}, out2{};
    std::thread w1([&] {
        out1 = broker.submit(make_req("s1", "bash", "/cmd1"));
    });
    std::thread w2([&] {
        out2 = broker.submit(make_req("s2", "bash", "/cmd2"));
    });

    while (broker.pending_count() < 2)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    // Interrupt session 1's run; approve session 2.
    broker.cancel_session("s1", "run interrupted");
    auto pending = broker.pending_requests();
    std::string s2_id;
    for (auto& p : pending)
        if (p.session_id == "s2") s2_id = p.id;
    CHECK(!s2_id.empty(), "s2 request still pending after s1 cancel");
    CHECK(broker.resolve(s2_id, PermissionDecision::AllowOnce),
          "s2 approvable while s1 cancelled");

    w1.join(); w2.join();
    CHECK(out1.effect == PermissionEffect::Deny
       && out1.reason == "run interrupted", "s1 denied with interrupt reason");
    CHECK(!out1.user_decided, "s1 denial not marked as a user decision");
    CHECK(out2.effect == PermissionEffect::Allow && out2.user_decided,
          "s2 user-approved");

    // Submissions for the cancelled run stay denied until re-armed.
    auto again = broker.submit(make_req("s1", "bash", "/cmd1"));
    CHECK(again.effect == PermissionEffect::Deny, "blocked session auto-denied");
    broker.allow_submissions("s1");
    std::cout << "[OK] concurrent sessions isolated under cancel\n";
    return true;
}

// cancel-all during shutdown wakes every waiter and rejects new submits.
static bool shutdown_wakes_all() {
    PermissionRequestBroker broker;
    broker.set_delivery_callback([](const PermissionRequest&) { return true; });

    PermissionOutcome out1{}, out2{};
    std::thread w1([&] { out1 = broker.submit(make_req("s1")); });
    std::thread w2([&] { out2 = broker.submit(make_req("s2")); });
    while (broker.pending_count() < 2)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    broker.cancel_all("engine shutdown");
    w1.join(); w2.join();
    CHECK(out1.effect == PermissionEffect::Deny && out2.effect == PermissionEffect::Deny,
          "both waiters woken with deny");
    CHECK(out1.reason == "engine shutdown", "shutdown reason reported");

    auto after = broker.submit(make_req("s3"));
    CHECK(after.effect == PermissionEffect::Deny, "post-shutdown submit rejected");
    std::cout << "[OK] cancel_all wakes waiters, rejects new submits\n";
    return true;
}

// A resolve that races a cancel: exactly one of the two wins, and an
// approval can never land on a cancelled request.
static bool resolve_vs_cancel_race() {
    for (int i = 0; i < 200; ++i) {
        PermissionRequestBroker broker;
        broker.set_delivery_callback([](const PermissionRequest&) { return true; });

        PermissionOutcome out{};
        std::thread worker([&] {
            out = broker.submit(make_req("s1"));
        });

        // Spin until registration is visible.
        while (broker.pending_count() == 0)
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        std::string req_id = broker.pending_requests()[0].id;

        // Fire resolve and cancel "simultaneously" from two threads.
        std::thread resolver([&] {
            broker.resolve(req_id, PermissionDecision::AllowOnce);
        });
        broker.cancel_session("s1", "run interrupted");
        resolver.join();
        worker.join();

        // Whatever interleaving occurred, the worker saw one definitive
        // answer, and an allow only survives when resolve won the race
        // BEFORE the cancel marked the entry.
        CHECK(out.effect == PermissionEffect::Allow
            || out.effect == PermissionEffect::Deny,
            "definitive outcome in every iteration");
        if (out.effect == PermissionEffect::Allow)
            CHECK(out.reason.find("allowed once") != std::string::npos,
                  "allow outcome came from the resolve, not the cancel");
    }
    std::cout << "[OK] resolve-vs-cancel race resolves exactly once (200 iterations)\n";
    return true;
}

// Registration racing shutdown: submit must not hang or prompt.
static bool registration_vs_shutdown_race() {
    for (int i = 0; i < 200; ++i) {
        PermissionRequestBroker broker;
        std::atomic<bool> delivery_called{false};
        broker.set_delivery_callback([&](const PermissionRequest&) {
            delivery_called = true;
            return true;
        });

        std::thread canceller([&] {
            broker.cancel_all("engine shutdown");
        });
        auto out = broker.submit(make_req("s1"));
        canceller.join();
        // Whether it prompted or was rejected outright, it must not hang and
        // must end denied (either pre-registered then cancelled, or blocked).
        CHECK(out.effect == PermissionEffect::Deny, "racing submit ends denied");
        (void)delivery_called;
    }
    std::cout << "[OK] registration-vs-shutdown race never hangs (200 iterations)\n";
    return true;
}

// Pending snapshot only shows still-waiting requests.
static bool pending_snapshot_accuracy() {
    PermissionRequestBroker broker;
    broker.set_delivery_callback([](const PermissionRequest&) { return true; });

    std::vector<PermissionOutcome> outs(3);
    std::thread workers[3];
    for (int i = 0; i < 3; ++i) {
        workers[i] = std::thread([&broker, &outs, i] {
            outs[size_t(i)] = broker.submit(make_req("s" + std::to_string(i)));
        });
    }
    while (broker.pending_count() < 3)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    auto pending = broker.pending_requests();
    CHECK(pending.size() == 3, "three pending visible");
    broker.resolve(pending[0].id, PermissionDecision::Deny);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(broker.pending_count() == 2, "resolved request leaves the snapshot");
    for (auto& p : broker.pending_requests())
        CHECK(p.id != pending[0].id, "resolved id absent from snapshot");

    broker.cancel_all("done");
    for (auto& w : workers) w.join();
    CHECK(broker.pending_count() == 0, "all clear after cancel_all");
    CHECK(outs[0].effect == PermissionEffect::Deny && outs[0].user_decided,
          "resolved-by-user outcome delivered");
    CHECK(outs[1].effect == PermissionEffect::Deny && !outs[1].user_decided,
          "cancelled outcome delivered");
    std::cout << "[OK] pending snapshot tracks waiting requests only\n";
    return true;
}

// ============================================================
// Engine integration: interrupt/shutdown release approval waits
// ============================================================

// Provider whose first stream() returns a bash tool call, then plain text.
class ToolCallProvider : public haicode::Provider {
public:
    std::string id() const override { return "toolfake"; }
    void cancel(const std::string& = "") override {}
    std::vector<std::string> list_models(std::string&) override {
        return {"toolfake-model"};
    }
    int get_model_context(const std::string&) const override { return 0; }
    void stream(const haicode::LLMRequest&, haicode::StreamCallbacks cb,
                const std::string& = "") override {
        if (step++ == 0) {
            haicode::ToolCall tc;
            tc.id = "call_1";
            tc.name = "bash";
            tc.input = nlohmann::json{{"command", "/bin/true"}};
            cb.on_finish(haicode::FinishReason::ToolUse, {}, {tc});
        } else {
            cb.on_text_delta("t", "done");
            cb.on_finish(haicode::FinishReason::EndTurn, {}, {});
        }
    }
    int step = 0;
};

static bool engine_interrupt_releases_pending_approval() {
    const char* kDb = "/tmp/hc_tpr_engine_int.db";
    std::remove(kDb);
    haicode::Database db(kDb);
    db.migrate();
    haicode::SessionStore store(db);
    auto provider = std::make_shared<ToolCallProvider>();
    haicode::ProviderRegistry registry;
    registry.register_provider(provider);
    haicode::ToolRegistry tools;
    haicode::register_builtin_tools(tools);
    haicode::PermissionGate gate;   // no rules: bash must ask
    haicode::SessionEventBus bus;
    haicode::AppConfig cfg;
    cfg.model = "toolfake-model";
    cfg.provider = "toolfake";
    cfg.autoname_sessions = false;
    cfg.default_mode = "build";   // bash is Plan-mode-blocked otherwise

    haicode::PermissionRequestBroker broker(&gate);
    broker.set_delivery_callback([](const haicode::PermissionRequest&) {
        return true;  // pretend a window opened
    });

    {
        haicode::SessionEngine engine(store, registry, tools, gate, bus, cfg);
        engine.set_permission_broker(&broker);
        std::string sid = engine.create_session("/tmp", "build",
                                                "toolfake-model", "toolfake");
        engine.submit_prompt(sid, "run the command");

        // Park: worker reaches the bash approval and blocks in the broker.
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::seconds(5);
        while (broker.pending_count() == 0
                && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(broker.pending_count() == 1,
              "worker parked on the bash approval");

        // Interrupt must wake it (no join here — that's the point).
        engine.interrupt(sid);
    }  // ~SessionEngine: joins the runner; hangs forever without the cancel

    CHECK(broker.pending_count() == 0, "no approvals remain pending");
    std::remove(kDb);
    std::cout << "[OK] engine interrupt releases pending approval; dtor joins\n";
    return true;
}

static bool engine_shutdown_with_pending_approval() {
    const char* kDb = "/tmp/hc_tpr_engine_shut.db";
    std::remove(kDb);
    haicode::Database db(kDb);
    db.migrate();
    haicode::SessionStore store(db);
    auto provider = std::make_shared<ToolCallProvider>();
    haicode::ProviderRegistry registry;
    registry.register_provider(provider);
    haicode::ToolRegistry tools;
    haicode::register_builtin_tools(tools);
    haicode::PermissionGate gate;
    haicode::SessionEventBus bus;
    haicode::AppConfig cfg;
    cfg.model = "toolfake-model";
    cfg.provider = "toolfake";
    cfg.autoname_sessions = false;
    cfg.default_mode = "build";

    haicode::PermissionRequestBroker broker(&gate);
    broker.set_delivery_callback([](const haicode::PermissionRequest&) {
        return true;
    });

    auto t0 = std::chrono::steady_clock::now();
    {
        haicode::SessionEngine engine(store, registry, tools, gate, bus, cfg);
        engine.set_permission_broker(&broker);
        std::string sid = engine.create_session("/tmp", "build",
                                                "toolfake-model", "toolfake");
        engine.submit_prompt(sid, "run the command");

        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::seconds(5);
        while (broker.pending_count() == 0
                && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(broker.pending_count() == 1, "approval pending before shutdown");
    }  // dtor → shutdown(): must cancel_all BEFORE joining workers

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t0);
    CHECK(elapsed < std::chrono::seconds(10),
          "shutdown joined the parked worker promptly");
    std::remove(kDb);
    std::cout << "[OK] engine shutdown cancels approvals before joining\n";
    return true;
}

static bool engine_rerun_rearms_submissions() {
    const char* kDb = "/tmp/hc_tpr_engine_rearm.db";
    std::remove(kDb);
    haicode::Database db(kDb);
    db.migrate();
    haicode::SessionStore store(db);
    auto provider = std::make_shared<ToolCallProvider>();
    haicode::ProviderRegistry registry;
    registry.register_provider(provider);
    haicode::ToolRegistry tools;
    haicode::register_builtin_tools(tools);
    haicode::PermissionGate gate;
    haicode::SessionEventBus bus;
    haicode::AppConfig cfg;
    cfg.model = "toolfake-model";
    cfg.provider = "toolfake";
    cfg.autoname_sessions = false;
    cfg.default_mode = "build";

    haicode::PermissionRequestBroker broker(&gate);
    broker.set_delivery_callback([](const haicode::PermissionRequest&) {
        return true;
    });

    haicode::SessionEngine engine(store, registry, tools, gate, bus, cfg);
    engine.set_permission_broker(&broker);
    std::string sid = engine.create_session("/tmp", "build",
                                            "toolfake-model", "toolfake");

    // Run 1: park on the approval, then interrupt (blocks submissions).
    engine.submit_prompt(sid, "run the command");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (broker.pending_count() == 0
            && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    engine.interrupt(sid);
    while (engine.is_running(sid))
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

    // Run 2: the new runner re-arms submissions; the worker parks again.
    // Reset the provider so run 2's first stream() emits the tool call too.
    provider->step = 0;
    engine.submit_prompt(sid, "again");
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (broker.pending_count() == 0
            && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(broker.pending_count() == 1,
          "second run prompts again (submissions re-armed)");

    // Approve it this time; the run completes.
    auto pending = broker.pending_requests();
    CHECK(broker.resolve(pending[0].id,
                         haicode::PermissionDecision::AllowOnce),
          "approval accepted on the re-armed run");
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (engine.is_running(sid)
            && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(!engine.is_running(sid), "approved run completes");
    std::remove(kDb);
    std::cout << "[OK] new run re-arms permission submissions\n";
    return true;
}

// Post-approval interrupt check: an interrupt landing while the user decided
// must stop the call even though approval came back Allow.
static bool registry_post_approval_interrupt_blocks() {
    haicode::ToolRegistry reg;
    haicode::register_builtin_tools(reg);
    haicode::PermissionGate gate;

    haicode::ToolContext ctx;
    ctx.session_id = "s1";
    ctx.call_id = "c1";
    ctx.tool_name = "bash";
    ctx.working_dir = "/tmp";
    std::atomic<bool> interrupted{true};   // set BEFORE the call resolves
    ctx.interrupt = &interrupted;

    // Gate ask resolves to Allow (as if the user clicked Allow)...
    int asks = 0;
    gate.set_ask_callback_ex([&](const haicode::ToolContext&,
                                 const std::string&, const std::string&,
                                 const std::string&, const nlohmann::json&) {
        ++asks;
        return haicode::PermissionEffect::Allow;
    });

    const std::string target = "/tmp/hc_tpr_post_interrupt.txt";
    std::remove(target.c_str());
    auto r = reg.execute("write", {{"path", target}, {"content", "x"}},
                         ctx, gate);
    CHECK(asks == 1, "ask callback ran and approved");
    CHECK(!r.success && r.denied, "interrupted call not executed");
    CHECK(r.error.find("[interrupted]") != std::string::npos,
          "error names the interrupt");
    struct stat st;
    CHECK(::stat(target.c_str(), &st) != 0, "no file written");
    std::cout << "[OK] post-approval interrupt check blocks execution\n";
    return true;
}

int main() {
    std::cout << "=== Permission Request Broker Tests ===\n";
    bool ok = true;
    ok &= approve_and_deny_roundtrip();
    ok &= duplicate_and_stale_replies();
    ok &= failed_delivery_denies();
    ok &= concurrent_session_isolation();
    ok &= shutdown_wakes_all();
    ok &= resolve_vs_cancel_race();
    ok &= registration_vs_shutdown_race();
    ok &= pending_snapshot_accuracy();

    std::cout << "\n-- Engine integration --\n";
    ok &= engine_interrupt_releases_pending_approval();
    ok &= engine_shutdown_with_pending_approval();
    ok &= engine_rerun_rearms_submissions();
    ok &= registry_post_approval_interrupt_blocks();
    if (ok) {
        std::cout << "\nAll broker tests passed!\n";
        return 0;
    }
    std::cerr << "\nSome broker tests FAILED.\n";
    return 1;
}
