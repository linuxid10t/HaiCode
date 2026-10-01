#include <haicode/engine.h>
#include <haicode/haicode.h>
#include "test_check.h"
#include <chrono>
#include <thread>
#include <iostream>

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
    std::cout << "turn integrity tests passed\n";
}
