#include <haicode/engine.h>
#include <haicode/haicode.h>
#include <haicode/permission_requests.h>
#include <haicode/util.h>
#include "test_check.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>

namespace haicode {
std::vector<nlohmann::json> translate_messages(const std::string&, const std::string&,
    const std::vector<nlohmann::json>&);
}
using namespace haicode;
using json = nlohmann::json;

static SessionMessage row(const std::string& type, const json& data) {
    SessionMessage m;
    m.type = type;
    m.data_json = data.dump();
    return m;
}

static void check_exchange(const std::vector<json>& messages) {
    std::set<std::string> pending;
    for (const auto& m : messages) {
        if (m["content"].is_array()) {
            bool results = false;
            for (const auto& b : m["content"]) {
                if (b.value("type", "") == "tool_result") {
                    results = true;
                    TEST_REQUIRE(pending.erase(b.value("tool_use_id", "")) == 1,
                        "result belongs to outstanding call");
                }
            }
            if (results) {
                TEST_REQUIRE(pending.empty(), "entire batch answered together");
                continue;
            }
        }
        TEST_REQUIRE(pending.empty(), "no ordinary message before results");
        if (m["content"].is_array())
            for (const auto& b : m["content"])
                if (b.value("type", "") == "tool_use")
                    TEST_REQUIRE(pending.insert(b.value("id", "")).second,
                        "unique call in batch");
    }
    TEST_REQUIRE(pending.empty(), "no unanswered calls");
    pending.clear();
    for (const auto& m : translate_messages("", "", messages)) {
        if (m.value("role", "") == "tool") {
            TEST_REQUIRE(pending.erase(m.value("tool_call_id", "")) == 1,
                "OpenAI matching response");
        } else {
            TEST_REQUIRE(pending.empty(), "OpenAI batch complete before ordinary message");
            if (m.contains("tool_calls"))
                for (const auto& c : m["tool_calls"])
                    pending.insert(c.value("id", ""));
        }
    }
    TEST_REQUIRE(pending.empty(), "OpenAI batch complete");
}

static void test_repair() {
    ContextBuilder builder;
    std::vector<SessionMessage> rows = {
        row("user_prompted", {{"text", "first"}}),
        row("assistant_text", {{"tool_calls", json::array({
            {{"id", "a"}, {"name", "test"}}, {{"id", "b"}, {"name", "test"}}})}}),
        row("tool_result", {{"call_id", "a"}, {"success", false}, {"output", "diagnostic"}}),
        row("user_prompted", {{"text", "interleaved"}}),
        row("tool_result", {{"call_id", "b"}, {"output", "late real result"}}),
        row("tool_result", {{"call_id", "b"}, {"output", "duplicate"}}),
        row("tool_result", {{"call_id", "orphan"}, {"output", "orphan"}})};
    auto out = builder.assemble_messages(rows);
    check_exchange(out);
    TEST_REQUIRE(out.size() == 4, "one grouped result message");
    TEST_REQUIRE(out[2]["content"].size() == 2, "two responses grouped");
    TEST_REQUIRE(out[2]["content"][0]["is_error"] == true, "failed result marked");
    TEST_REQUIRE(out[2]["content"][1]["content"] == "late real result", "late output retained");
    rows.resize(4);
    out = builder.assemble_messages(rows);
    check_exchange(out);
    TEST_REQUIRE(out[2]["content"][1]["is_error"] == true, "missing result repaired");
}

// ============================================================
// Task 14: FIFO prompt scheduling + independent title maintenance
// ============================================================

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

static void wait_idle(SessionEngine& engine, const std::string& sid);

// Provider whose normal calls are scripted by the last string user message:
// a prompt containing "PARK" gets a parktool call, everything else ends the
// turn. Title-refinement and compaction-summarizer calls are recognized by
// their system prompts and can be parked on dedicated gates, so tests block
// exactly the stream they care about — no timing guesses.
class ParkingProvider : public Provider {
public:
    std::string id() const override { return "parking"; }
    void cancel(const std::string& token = "") override {
        if (title_gate && (token.empty() || token.rfind("t:", 0) == 0))
            title_gate->open_gate();
        if (summarize_gate && (token.empty() || token.rfind("s:", 0) == 0))
            summarize_gate->open_gate();
    }
    std::vector<std::string> list_models(std::string&) override { return {"p-model"}; }

    Gate title_entered;
    Gate summarize_entered;
    std::shared_ptr<Gate> title_gate;
    std::shared_ptr<Gate> summarize_gate;
    std::atomic<int> calls{0};        // normal conversation calls
    std::atomic<int> title_calls{0};
    std::atomic<int> summarize_calls{0};

    void stream(const LLMRequest& req, StreamCallbacks cb, const std::string& = "") override {
        const std::string& sys = req.system;
        if (sys.find("generate short descriptive titles") != std::string::npos) {
            ++title_calls;
            title_entered.open_gate();
            if (title_gate) title_gate->wait();
            cb.on_text_delta("t", "Refined Session Title");
            cb.on_finish(FinishReason::EndTurn, {}, {});
            return;
        }
        if (sys.find("precise conversation summarizer") != std::string::npos) {
            ++summarize_calls;
            summarize_entered.open_gate();
            if (summarize_gate) summarize_gate->wait();
            cb.on_text_delta("t",
                "## Objective\no\n## Constraints & Decisions\nc\n## Completed Work\nw\n"
                "## Active Work\na\n## Blockers\nb\n## Next Actions\nn\n## Relevant Files\nf\n");
            cb.on_finish(FinishReason::EndTurn, {}, {});
            return;
        }
        ++calls;
        // Script from the FINAL message only: a plain-string user tail names
        // this turn's prompt; a tool-result tail (array content) means the
        // park exchange already ran — end the turn.
        const nlohmann::json* tail = req.messages.empty() ? nullptr
                                                          : &req.messages.back();
        bool tail_is_park = tail && tail->value("role", "") == "user"
            && (*tail)["content"].is_string()
            && (*tail)["content"].get<std::string>().find("PARK")
                   != std::string::npos;
        if (tail_is_park) {
            std::vector<ToolCall> tcs;
            ToolCall tc;
            tc.id = "park1";
            tc.name = "parktool";
            tc.input = json::object();
            tcs.push_back(tc);
            cb.on_finish(FinishReason::ToolUse, {}, tcs);
            return;
        }
        cb.on_text_delta("t", "done");
        cb.on_finish(FinishReason::EndTurn, {}, {});
    }
};

// Tool that parks inside execute() until its gate opens or the run's
// interrupt flag is set (so shutdown/interrupt always release it).
class ParkingTool : public Tool {
public:
    std::shared_ptr<Gate> gate;
    Gate entered;
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

struct QueueFixture {
    Database db;
    SessionStore store;
    std::shared_ptr<ParkingProvider> provider;
    ProviderRegistry registry;
    ToolRegistry tools;
    std::shared_ptr<ParkingTool> park;
    PermissionGate gate;
    SessionEventBus bus;
    AppConfig config;
    std::unique_ptr<SessionEngine> engine;
    std::string sid;

    explicit QueueFixture(bool autotitle = false)
        : db(":memory:")
        , store(db)
        , provider(std::make_shared<ParkingProvider>())
        , park(std::make_shared<ParkingTool>())
    {
        db.migrate();
        park->gate = std::make_shared<Gate>();
        registry.register_provider(provider);
        register_builtin_tools(tools);
        tools.register_tool(park);
        gate.set_rules({{"*", "*", PermissionEffect::Allow}});
        config.default_mode = "build";
        config.autoname_sessions = autotitle;
        config.autoname_llm_refine = autotitle;
        engine = std::make_unique<SessionEngine>(store, registry, tools, gate,
                                                 bus, config);
        sid = engine->create_session("/tmp", "build", "p-model", "parking");
    }
};

static void wait_running(SessionEngine& engine, const std::string& sid) {
    for (int i = 0; i < 500 && !engine.is_running(sid); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    TEST_REQUIRE(engine.is_running(sid), "session must be running");
}

static void wait_title(SessionStore& store, const std::string& sid,
                       const std::string& expect) {
    for (int i = 0; i < 500; ++i) {
        auto s = store.get(sid);
        if (s && s->title.find(expect) != std::string::npos) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto s = store.get(sid);
    TEST_REQUIRE(false, "title never became '" + expect + "', is '" +
                (s ? s->title : "<none>") + "'");
}

// Submit while parked in a tool: the prompt queues, never lands between the
// tool_use row and its result, and later runs as its OWN turn (FIFO).
static void test_queue_during_tool() {
    QueueFixture fx;
    fx.engine->submit_prompt(fx.sid, "PARK here");
    wait_running(*fx.engine, fx.sid);
    fx.park->entered.wait();
    TEST_REQUIRE(fx.park->executions == 1, "parked tool is executing");

    // Two prompts arrive while the tool is parked.
    fx.engine->submit_prompt(fx.sid, "second prompt");
    fx.engine->submit_prompt(fx.sid, "third prompt");
    TEST_REQUIRE(fx.engine->queued_prompt_count(fx.sid) == 2,
                 "both prompts queued while the turn runs");
    // Not persisted yet: rows are written only when the turn starts.
    {
        auto rows = fx.store.load_messages(fx.sid);
        int users = 0;
        for (auto& r : rows) if (r.type == "user_prompted") ++users;
        TEST_REQUIRE(users == 1, "queued prompts are not persisted early");
    }

    fx.park->gate->open_gate();
    wait_idle(*fx.engine, fx.sid);

    // Exactly one response per submitted prompt, in FIFO order: each queued
    // prompt runs as its own turn only after the previous turn fully closed
    // (its tool exchange result persisted before the next user row).
    std::vector<std::string> sig;
    auto rows0 = fx.store.load_messages(fx.sid);
    for (auto& r : rows0) sig.push_back(r.type);
    std::vector<std::string> want{
        "user_prompted", "assistant_text", "tool_result", "assistant_text",
        "user_prompted", "assistant_text",
        "user_prompted", "assistant_text"};
    TEST_REQUIRE(sig == want, "FIFO turn sequence, one response per prompt");
    std::vector<std::string> prompts;
    for (const auto& r : rows0)
        if (r.type == "user_prompted") prompts.push_back(json::parse(r.data_json)["text"]);
    TEST_REQUIRE(prompts == std::vector<std::string>({"PARK here", "second prompt", "third prompt"}),
        "FIFO prompt order");
    TEST_REQUIRE(fx.park->executions == 1, "parked tool ran exactly once");
    TEST_REQUIRE(fx.provider->calls == 4,
                 "one stream per step: park-call, turn1 end, turn2 end, turn3 end");
    TEST_REQUIRE(fx.engine->queued_prompt_count(fx.sid) == 0, "queue drained");
    check_exchange(ContextBuilder().assemble_messages(
        fx.store.load_messages(fx.sid)));
    std::cout << "[OK] queue during tool: FIFO, one response each, "
                 "no mid-exchange rows\n";
}

// Submit immediately after interrupt: the queued prompt still runs (with
// fresh interruption state) and gets exactly one response.
static void test_submit_after_interrupt() {
    QueueFixture fx;
    fx.engine->submit_prompt(fx.sid, "PARK here");
    fx.park->entered.wait();

    fx.engine->interrupt(fx.sid);
    fx.engine->submit_prompt(fx.sid, "after interrupt");
    // The parked tool released via the interrupt flag; the queued prompt
    // must still run as its own turn with a fresh interrupt state.
    wait_idle(*fx.engine, fx.sid);

    auto rows = fx.store.load_messages(fx.sid);
    int users = 0, assistants = 0;
    for (auto& r : rows) {
        if (r.type == "user_prompted") ++users;
        if (r.type == "assistant_text") ++assistants;
    }
    TEST_REQUIRE(users == 2, "both prompts persisted");
    TEST_REQUIRE(assistants == 2, "interrupted call and exactly one queued response");
    TEST_REQUIRE(fx.provider->calls == 2, "queued prompt streamed exactly once");
    // The interrupted exchange kept its integrity: result row for the call.
    bool has_result = false;
    for (auto& r : rows) if (r.type == "tool_result") has_result = true;
    TEST_REQUIRE(has_result, "interrupted tool call still has a result row");
    TEST_REQUIRE(fx.engine->queued_prompt_count(fx.sid) == 0, "queue empty");
    TEST_REQUIRE(!fx.engine->is_running(fx.sid), "idle at the end");
    check_exchange(ContextBuilder().assemble_messages(rows));
    std::cout << "[OK] submit after interrupt: queued prompt runs with "
                 "fresh interruption state\n";
}

// A submission racing the runner's exit (from the TurnEnded notification)
// can never be stranded: it queues and the drain re-check picks it up.
static void test_submit_at_runner_exit() {
    QueueFixture fx;
    std::atomic<bool> submitted{false};
    fx.bus.subscribe(events::EventType::TurnEnded, [&](const json&) {
        if (submitted.exchange(true)) return;
        fx.engine->submit_prompt(fx.sid, "from TurnEnded");
    });
    fx.engine->submit_prompt(fx.sid, "first prompt");
    wait_idle(*fx.engine, fx.sid);
    TEST_REQUIRE(submitted.load(), "TurnEnded fired");
    // The raced-in prompt must have been persisted and answered, not lost.
    wait_idle(*fx.engine, fx.sid);
    auto rows = fx.store.load_messages(fx.sid);
    int users = 0;
    for (auto& r : rows) if (r.type == "user_prompted") ++users;
    TEST_REQUIRE(users == 2, "raced-in prompt persisted");
    TEST_REQUIRE(fx.provider->calls == 2 && rows.size() == 4,
        "exactly one response per runner-exit prompt");
    TEST_REQUIRE(!fx.engine->is_running(fx.sid), "idle at the end");
    TEST_REQUIRE(fx.engine->queued_prompt_count(fx.sid) == 0, "queue empty");
    check_exchange(ContextBuilder().assemble_messages(rows));
    std::cout << "[OK] submit at runner exit: never stranded\n";
}

// Same, but submitted from the final StepEnded (inside the loop body).
static void test_submit_from_final_step() {
    QueueFixture fx;
    std::atomic<bool> submitted{false};
    fx.bus.subscribe(events::EventType::StepEnded, [&](const json& data) {
        if (data.value("finish_reason", "") != "end_turn") return;
        if (submitted.exchange(true)) return;
        fx.engine->submit_prompt(fx.sid, "from StepEnded");
    });
    fx.engine->submit_prompt(fx.sid, "first prompt");
    wait_idle(*fx.engine, fx.sid);
    TEST_REQUIRE(submitted.load(), "final StepEnded fired");
    auto rows = fx.store.load_messages(fx.sid);
    int users = 0;
    for (auto& r : rows) if (r.type == "user_prompted") ++users;
    TEST_REQUIRE(users == 2, "prompt from StepEnded persisted");
    TEST_REQUIRE(fx.provider->calls == 2 && rows.size() == 4,
        "exactly one response per final-step prompt");
    TEST_REQUIRE(!fx.engine->is_running(fx.sid), "idle at the end");
    check_exchange(ContextBuilder().assemble_messages(rows));
    std::cout << "[OK] submit from final step: queued, one response\n";
}

// Title refinement is maintenance work: a blocked title request must not
// keep the session's foreground state busy — the next prompt starts and
// completes while the title stream is still parked.
static void test_title_decoupled_and_interrupt_skip() {
    {
        QueueFixture fx(/*autotitle=*/true);
        fx.provider->title_gate = std::make_shared<Gate>();
        fx.engine->submit_prompt(fx.sid, "first prompt");   // turn 1: refine fires
        fx.provider->title_entered.wait();
        TEST_REQUIRE(fx.provider->title_calls == 1, "title job started");
        wait_idle(*fx.engine, fx.sid);

        // New foreground work completes while the title stream stays parked.
        fx.engine->submit_prompt(fx.sid, "second prompt");
        wait_idle(*fx.engine, fx.sid);
        TEST_REQUIRE(fx.provider->title_calls == 1,
                     "turn 2 does not spawn another refinement (6-turn cadence)");
        TEST_REQUIRE(fx.provider->calls == 2, "both turns streamed");

        fx.provider->title_gate->open_gate();
        wait_title(fx.store, fx.sid, "Refined Session Title");
        // Engine destruction joins the (now finished) title worker cleanly.
        fx.engine.reset();
        std::cout << "[OK] title refinement decoupled from foreground state\n";
    }
    {
        // Interrupted runs skip refinement entirely.
        QueueFixture fx(/*autotitle=*/true);
        fx.provider->title_gate = std::make_shared<Gate>();
        fx.bus.subscribe(events::EventType::StepStarted, [&](const json&) {
            fx.engine->interrupt(fx.sid);
        });
        fx.engine->submit_prompt(fx.sid, "PARK here");
        wait_idle(*fx.engine, fx.sid);
        TEST_REQUIRE(fx.provider->title_calls == 0,
                     "no title refinement after an interrupted run");
        std::cout << "[OK] interrupted run skips title refinement\n";
    }
}

// A prompt submitted while manual compaction runs queues behind it and runs
// as its own turn afterwards (the compaction worker shares the drain).
static void test_queue_during_compaction() {
    QueueFixture fx;
    fx.provider->summarize_gate = std::make_shared<Gate>();
    fx.engine->submit_prompt(fx.sid, "first prompt");
    wait_idle(*fx.engine, fx.sid);
    fx.engine->submit_prompt(fx.sid, "second prompt");
    wait_idle(*fx.engine, fx.sid);

    fx.engine->compact_now(fx.sid);
    fx.provider->summarize_entered.wait();
    TEST_REQUIRE(fx.provider->summarize_calls == 1, "summarizer started");

    fx.engine->submit_prompt(fx.sid, "during compaction");
    TEST_REQUIRE(fx.engine->queued_prompt_count(fx.sid) == 1,
                 "prompt queued behind the compaction");
    fx.provider->summarize_gate->open_gate();
    wait_idle(*fx.engine, fx.sid);

    auto rows = fx.store.load_messages(fx.sid);
    int users = 0;
    for (auto& r : rows) if (r.type == "user_prompted") ++users;
    TEST_REQUIRE(users == 3, "queued prompt persisted after compaction");
    TEST_REQUIRE(!fx.engine->is_running(fx.sid), "idle at the end");
    check_exchange(ContextBuilder().assemble_messages(rows));
    std::cout << "[OK] queue during manual compaction drains afterwards\n";
}

// Shutdown discards queued prompts: no restart, no hang, no late rows.
static void test_shutdown_discards_queue() {
    QueueFixture fx;
    fx.engine->submit_prompt(fx.sid, "PARK here");
    fx.park->entered.wait();
    fx.engine->submit_prompt(fx.sid, "never runs");
    TEST_REQUIRE(fx.engine->queued_prompt_count(fx.sid) == 1, "queued");
    int calls = fx.provider->calls.load();

    fx.engine->shutdown();   // sets interrupt flags; the parked tool releases
    TEST_REQUIRE(fx.park->executions == 1, "parked tool released by shutdown");
    TEST_REQUIRE(fx.provider->calls.load() == calls,
                 "queued prompt never started after shutdown began");
    auto rows = fx.store.load_messages(fx.sid);
    int users = 0;
    for (auto& r : rows) if (r.type == "user_prompted") ++users;
    TEST_REQUIRE(users == 1, "queued prompt was never persisted");
    fx.engine->submit_prompt(fx.sid, "rejected after shutdown");
    TEST_REQUIRE(fx.store.load_messages(fx.sid).size() == rows.size(), "shutdown rejects new work");
    fx.engine->interrupt(fx.sid);
    std::cout << "[OK] shutdown discards queued prompts without restart\n";
}

// Two sessions sharing one provider run independently: parking one session's
// tool does not block the other session's turn.
static void test_concurrent_sessions() {
    QueueFixture fx;
    std::string a = fx.sid;
    std::string b = fx.engine->create_session("/tmp", "build", "p-model",
                                              "parking");
    fx.engine->submit_prompt(a, "PARK here");
    fx.park->entered.wait();

    fx.engine->submit_prompt(b, "quick turn");
    wait_idle(*fx.engine, b);
    TEST_REQUIRE(fx.engine->is_running(a), "session A still parked");
    TEST_REQUIRE(!fx.engine->is_running(b), "session B finished independently");

    fx.park->gate->open_gate();
    wait_idle(*fx.engine, a);
    TEST_REQUIRE(!fx.engine->is_running(a), "session A drained after release");
    check_exchange(ContextBuilder().assemble_messages(
        fx.store.load_messages(a)));
    check_exchange(ContextBuilder().assemble_messages(
        fx.store.load_messages(b)));
    std::cout << "[OK] concurrent sessions run independently\n";
}

static void test_queue_during_approval() {
    QueueFixture fx;
    PermissionRequestBroker broker(&fx.gate);
    Gate delivered;
    broker.set_delivery_callback([&](const PermissionRequest&) {
        delivered.open_gate();
        return true;
    });
    fx.gate.set_rules({});
    fx.engine->set_permission_broker(&broker);
    fx.park->gate->open_gate();
    fx.engine->submit_prompt(fx.sid, "PARK approval");
    delivered.wait();
    fx.engine->submit_prompt(fx.sid, "after approval");
    auto pending = broker.pending_requests();
    TEST_REQUIRE(pending.size() == 1, "one parked approval");
    TEST_REQUIRE(fx.engine->queued_prompt_count(fx.sid) == 1, "queued behind approval");
    TEST_REQUIRE(broker.resolve(pending[0].id, PermissionDecision::AllowOnce), "approve");
    wait_idle(*fx.engine, fx.sid);
    TEST_REQUIRE(fx.provider->calls == 3, "approved exchange and distinct queued response");
    check_exchange(ContextBuilder().assemble_messages(fx.store.load_messages(fx.sid)));
    fx.engine.reset();
}

static void test_prepared_queue() {
    QueueFixture fx;
    fx.engine->submit_prompt(fx.sid, "PARK here");
    fx.park->entered.wait();
    std::vector<Attachment> attachments(1);
    attachments[0].kind = "text";
    attachments[0].media_type = "text/plain";
    attachments[0].data_b64 = util::base64_encode("captured payload");
    fx.engine->submit_prompt(fx.sid, "prepared", attachments);
    attachments[0].data_b64 = util::base64_encode("changed payload");
    fx.park->gate->open_gate();
    wait_idle(*fx.engine, fx.sid);
    bool found = false;
    for (const auto& r : fx.store.load_messages(fx.sid)) {
        if (r.type != "user_prompted") continue;
        auto data = json::parse(r.data_json);
        if (data.value("text", "") != "prepared") continue;
        found = true;
        TEST_REQUIRE(util::base64_decode(data["attachments"][0]["data_b64"])
            == "captured payload", "queue owns prepared payload");
    }
    TEST_REQUIRE(found, "prepared prompt persisted");
}

static void test_title_cancel_and_stale() {
    for (bool cancel : {false, true}) {
        QueueFixture fx(true);
        fx.provider->title_gate = std::make_shared<Gate>();
        fx.engine->submit_prompt(fx.sid, "first prompt");
        fx.provider->title_entered.wait();
        wait_idle(*fx.engine, fx.sid);
        const auto baseline = fx.store.get(fx.sid)->title;
        if (cancel) fx.engine->interrupt(fx.sid);
        else fx.store.update_title(fx.sid, "Newer title");
        fx.provider->title_gate->open_gate();
        fx.engine->shutdown();
        TEST_REQUIRE(fx.store.get(fx.sid)->title == (cancel ? baseline : "Newer title"),
            "cancelled or stale title cannot overwrite current title");
    }
    QueueFixture fx(true);
    fx.provider->title_gate = std::make_shared<Gate>();
    fx.engine->submit_prompt(fx.sid, "first prompt");
    fx.provider->title_entered.wait();
    wait_idle(*fx.engine, fx.sid);
    fx.store.update_title(fx.sid, "Newer title");
    TEST_REQUIRE(!fx.store.update_title_if_current(fx.sid, "Stale title", "first prompt"),
        "atomic stale-title guard");
    fx.engine->shutdown();
}

class BatchProvider : public Provider {
public:
    std::string id() const override { return "batch"; }
    void cancel(const std::string& = "") override {}
    std::vector<std::string> list_models(std::string&) override { return {}; }
    void stream(const LLMRequest& req, StreamCallbacks cb, const std::string& = "") override {
        check_exchange(req.messages);
        if (count++ == 0) cb.on_finish(FinishReason::ToolUse, {}, calls);
        else {
            cb.on_text_delta("text", "done");
            cb.on_finish(FinishReason::EndTurn, {}, {});
        }
    }
    int count = 0;
    std::vector<ToolCall> calls;
};

class CountTool : public Tool {
public:
    std::string name() const override { return "count"; }
    std::string description() const override { return "test"; }
    json input_schema() const override { return json::object(); }
    ToolResult execute(const json&, const ToolContext&) override { ++executed; return {}; }
    std::atomic<int> executed{0};
};

class PlanTool : public CountTool {
public:
    std::string name() const override { return "propose_plan"; }
};

static void wait_idle(SessionEngine& engine, const std::string& sid) {
    for (int i = 0; i < 500 && engine.is_running(sid); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    TEST_REQUIRE(!engine.is_running(sid), "bounded worker completion");
}

static void test_stop(const std::string& reason) {
    Database db(":memory:");
    db.migrate();
    SessionStore store(db);
    ProviderRegistry providers;
    auto provider = std::make_shared<BatchProvider>();
    providers.register_provider(provider);
    ToolRegistry tools;
    register_builtin_tools(tools);
    auto counter = std::make_shared<CountTool>();
    tools.register_tool(counter);
    tools.register_tool(std::make_shared<PlanTool>());
    PermissionGate gate;
    gate.set_rules({{"*", "*", reason == "permission denied"
        ? PermissionEffect::Deny : PermissionEffect::Allow}});
    SessionEventBus bus;
    AppConfig config;
    config.default_mode = "build";
    config.autoname_sessions = false;
    SessionEngine engine(store, providers, tools, gate, bus, config);
    auto sid = engine.create_session("/tmp", "build", "test", "batch");
    provider->calls = {{"a", reason == "plan proposed" ? "propose_plan" : "count",
        {{"plan", "test"}}, false, {}}, {"b", "count", json::object(), false, {}}};
    if (reason == "plan proposed") engine.set_mode(sid, SessionMode::Plan);
    if (reason == "interrupted")
        bus.subscribe(events::EventType::StepEnded, [&](const json&) { engine.interrupt(sid); });
    if (reason == "before execution")
        bus.subscribe(events::EventType::ToolCalled, [&](const json&) { engine.interrupt(sid); });
    if (reason == "during batch")
        bus.subscribe(events::EventType::ToolSuccess, [&](const json&) { engine.interrupt(sid); });
    engine.submit_prompt(sid, "first");
    wait_idle(engine, sid);
    auto rows = store.load_messages(sid);
    int results = 0;
    for (const auto& m : rows) if (m.type == "tool_result") {
        auto data = json::parse(m.data_json);
        ++results;
        if (data["call_id"] == "b") {
            std::string expected = reason == "during batch" || reason == "before execution"
                ? "interrupted" : reason;
            TEST_REQUIRE(data["success"] == false, "skipped call fails");
            TEST_REQUIRE(data.value("output", "").find("not run: " + expected)
                != std::string::npos, "explicit skip reason");
        }
    }
    TEST_REQUIRE(results == 2, "every call persisted a result");
    TEST_REQUIRE(counter->executed == (reason == "during batch" ? 1 : 0),
        "skipped calls have no side effects");
    check_exchange(ContextBuilder().assemble_messages(rows));
    bus.unsubscribe_all();
    engine.submit_prompt(sid, "next");
    wait_idle(engine, sid);
    TEST_REQUIRE(provider->count == 2, "next request succeeds");
}

int main() {
    test_repair();
    test_stop("permission denied");
    test_stop("plan proposed");
    test_stop("interrupted");
    test_stop("before execution");
    test_stop("during batch");
    test_queue_during_tool();
    test_submit_after_interrupt();
    test_submit_at_runner_exit();
    test_submit_from_final_step();
    test_title_decoupled_and_interrupt_skip();
    test_queue_during_compaction();
    test_shutdown_discards_queue();
    test_concurrent_sessions();
    test_queue_during_approval();
    test_prepared_queue();
    test_title_cancel_and_stale();
    std::cout << "turn integrity tests passed\n";
}
