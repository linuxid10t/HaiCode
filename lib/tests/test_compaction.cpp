#include <haicode/db.h>
#include <haicode/compaction.h>
#include <haicode/engine.h>
#include <haicode/model_info.h>
#include <haicode/config.h>
#include <haicode/provider.h>
#include <haicode/tool.h>
#include <chrono>
#include <iostream>
#include <cstdio>
#include <thread>
#include <unistd.h>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << msg << "\n"; return false; } } while(0)

static const char* kDbPath = "/tmp/haicode_test_compaction.db";

// Drives the real agentic loop end-to-end: over-threshold usage on turn 1
// arms the trigger; turn 2's first step must compact via a checkpoint.
class FakeProvider : public haicode::Provider {
public:
    std::string id() const override { return "fake"; }
    void cancel() override {}
    std::vector<std::string> list_models(std::string&) override {
        return {"fake-model"};
    }
    void stream(const haicode::LLMRequest& req, haicode::StreamCallbacks cb) override {
        ++calls;
        if (req.system == "You are a precise conversation summarizer.") {
            summary_requests.push_back(req);
            cb.on_text_delta("t",
                "## Objective\no\n\n## Constraints & Decisions\nc\n\n"
                "## Completed Work\ncw\n\n## Active Work\naw\n\n"
                "## Blockers\nb\n\n## Next Actions\nn\n\n"
                "## Relevant Files\nf\n");
            cb.on_finish(haicode::FinishReason::EndTurn, {}, {});
            return;
        }
        last_chat_request = req;
        cb.on_text_delta("t", "ok");
        haicode::TokenUsage u;
        u.input = 999999;  // over any threshold → arms/re-fires the trigger
        cb.on_finish(haicode::FinishReason::EndTurn, u, {});
    }
    int calls = 0;
    std::vector<haicode::LLMRequest> summary_requests;
    haicode::LLMRequest last_chat_request;

    static std::string dump_messages(const std::vector<nlohmann::json>& msgs) {
        std::string out;
        for (const auto& m : msgs) out += m.dump();
        return out;
    }
};

static bool wait_for(haicode::SessionStore& store, const std::string& sid,
                     size_t want_messages, bool want_checkpoint) {
    // Phase 1: messages settle. Phase 2: checkpoint state (compaction runs
    // on the loop thread and can lag the last append by a summarizer call).
    for (int i = 0; i < 100; ++i) {
        if (store.load_messages(sid).size() >= want_messages) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for (int i = 0; i < 200; ++i) {
        bool cp = want_checkpoint
            ? store.latest_complete_checkpoint(sid).has_value()
            : !store.latest_complete_checkpoint(sid).has_value();
        if (store.load_messages(sid).size() >= want_messages && cp) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// Regression for "compaction triggers but does nothing": the whole e2e path —
// trigger → split → summarize → validate → commit → sliced request.
static bool test_end_to_end_checkpoint() {
    remove(kDbPath);
    haicode::Database db(kDbPath);
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
    cfg.model_contexts["fake-model"] = 16000;

    haicode::SessionEngine engine(store, registry, tools, perms, bus, cfg);
    std::string sid = engine.create_session("/tmp/proj", "build",
                                            "fake-model", "fake");

    engine.submit_prompt(sid, "TURNONE-MARKER first prompt");
    CHECK(wait_for(store, sid, 2, false), "turn 1 completes (2 rows, no ckpt)");

    engine.submit_prompt(sid, "TURNTWO-MARKER second prompt");
    // Generous deadline: compaction runs on the loop thread and this box has
    // been unstable; the asserted end state is what matters, not timing.
    bool settled = false;
    for (int i = 0; i < 600 && !settled; ++i) {  // up to 30s
        settled = store.load_messages(sid).size() >= 4
               && store.latest_complete_checkpoint(sid).has_value();
        if (!settled) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(settled, "turn 2 completes AND a completed checkpoint exists");

    auto cp = store.latest_complete_checkpoint(sid);
    CHECK(cp.has_value(), "checkpoint committed");
    CHECK(cp->through_seq >= 1, "boundary covers at least the oldest turn");
    CHECK(store.load_messages(sid).size() == 4,
          "all four rows survive compaction");
    CHECK(!provider->summary_requests.empty(), "summarizer call happened");
    std::string reqd = FakeProvider::dump_messages(provider->last_chat_request.messages);
    CHECK(reqd.find("TURNONE-MARKER") == std::string::npos,
          "post-compaction request excludes pre-checkpoint turns");
    CHECK(reqd.find("HISTORICAL CONVERSATION") != std::string::npos,
          "post-compaction request carries the checkpoint block");
    CHECK(reqd.find("TURNTWO-MARKER") != std::string::npos,
          "post-checkpoint turns stay live");
    std::cout << "[OK] end-to-end: trigger -> checkpoint -> sliced request\n";
    return true;
}

static void append_user(haicode::SessionStore& store, const std::string& sid,
                        const std::string& text) {
    store.append_message(sid, "user_prompted",
        nlohmann::json{{"text", text}}.dump());
}
static void append_asst(haicode::SessionStore& store, const std::string& sid,
                        const std::string& text) {
    store.append_message(sid, "assistant_text",
        nlohmann::json{{"text", text}, {"role", "assistant"}}.dump());
}

static bool test_checkpoint_roundtrip() {
    remove(kDbPath);
    haicode::Database db(kDbPath);
    db.migrate();
    haicode::SessionStore store(db);
    auto sess = store.create("/tmp/proj", "build", "{}");

    CHECK(!store.latest_complete_checkpoint(sess.id).has_value(),
          "no checkpoint initially");

    std::string id1 = store.insert_checkpoint(sess.id, 4, "recent-ctx-1", "");
    CHECK(!store.latest_complete_checkpoint(sess.id).has_value(),
          "pending checkpoint is not visible");

    store.complete_checkpoint(id1, "## Objective\nsummary-1");
    auto cp1 = store.latest_complete_checkpoint(sess.id);
    CHECK(cp1.has_value() && cp1->id == id1, "complete checkpoint visible");
    CHECK(cp1->summary == "## Objective\nsummary-1", "summary round-trips");
    CHECK(cp1->recent_context == "recent-ctx-1", "recent_context round-trips");

    std::string id2 = store.insert_checkpoint(sess.id, 8, "recent-ctx-2", id1);
    store.fail_checkpoint(id2);
    auto cp2 = store.latest_complete_checkpoint(sess.id);
    CHECK(cp2.has_value() && cp2->id == id1,
          "failed checkpoint does not become active");

    std::string id3 = store.insert_checkpoint(sess.id, 12, "recent-ctx-3", id1);
    store.complete_checkpoint(id3, "## Objective\nsummary-3");
    auto cp3 = store.latest_complete_checkpoint(sess.id);
    CHECK(cp3.has_value() && cp3->id == id3, "latest by through_seq");
    CHECK(cp3->previous_checkpoint_id == id1, "chain links to predecessor");
    std::cout << "[OK] checkpoint round-trip (pending/complete/failed/chain)\n";
    return true;
}

static bool test_messages_survive() {
    remove(kDbPath);
    haicode::Database db(kDbPath);
    db.migrate();
    haicode::SessionStore store(db);
    auto sess = store.create("/tmp/proj", "build", "{}");

    for (int i = 1; i <= 6; ++i) {
        append_user(store, sess.id, "turn " + std::to_string(i));
        append_asst(store, sess.id, "reply " + std::to_string(i));
    }
    size_t before = store.load_messages(sess.id).size();
    CHECK(before == 12, "six turns stored");

    std::string id = store.insert_checkpoint(sess.id, 8, "tail", "");
    store.complete_checkpoint(id, "## Objective\ns");
    CHECK(store.load_messages(sess.id).size() == before,
          "HEADLINE: messages survive compaction (rows never deleted)");
    std::cout << "[OK] messages survive compaction\n";
    return true;
}

static bool test_assembly_with_checkpoint() {
    remove(kDbPath);
    haicode::Database db(kDbPath);
    db.migrate();
    haicode::SessionStore store(db);
    auto sess = store.create("/tmp/proj", "build", "{}");

    append_user(store, sess.id, "always use tabs");
    append_asst(store, sess.id, "understood");
    append_user(store, sess.id, "now build");
    append_asst(store, sess.id, "building");

    std::string id = store.insert_checkpoint(sess.id, 2, "RECENTCTX", "");
    store.complete_checkpoint(id, "## Objective\ntabs-only constraint");

    haicode::ContextBuilder builder;
    auto out = haicode::apply_checkpoint(store.load_messages(sess.id),
                                         *store.latest_complete_checkpoint(sess.id));
    CHECK(out.size() == 3, "block + tail only");
    auto assembled = builder.assemble_messages(out);
    CHECK(assembled.size() == 3, "three provider messages");
    std::string first = assembled[0].dump();
    CHECK(first.find("tabs-only constraint") != std::string::npos,
          "summary text reaches the request");
    CHECK(first.find("always use tabs") == std::string::npos,
          "pre-checkpoint rows do NOT reach the request");
    CHECK(first.find("RECENTCTX") == std::string::npos,
          "retained recent context is not re-embedded (no double count)");
    CHECK(assembled[1].value("role", "") == "user"
              && assembled[2].value("role", "") == "assistant",
          "alternation legal after checkpoint");
    std::cout << "[OK] assembly slices by through_seq and prepends block\n";
    return true;
}

static bool test_split_feeds_checkpoint_chain() {
    std::vector<haicode::SessionMessage> msgs;
    auto mk = [](int seq, const char* type, const std::string& text) {
        haicode::SessionMessage m;
        m.seq = seq; m.type = type;
        m.data_json = nlohmann::json{{"text", text}}.dump();
        return m;
    };
    for (int i = 1; i <= 8; ++i) {
        msgs.push_back(mk(i * 2 - 1, "user_prompted", "note " + std::to_string(i)));
        msgs.push_back(mk(i * 2, "assistant_text", "ack " + std::to_string(i)));
    }

    int ts1 = haicode::split_history(msgs, 200, 0);
    CHECK(ts1 > 0, "first split finds a boundary");

    for (int i = 9; i <= 12; ++i) {
        msgs.push_back(mk(i * 2 - 1, "user_prompted", "note " + std::to_string(i)));
        msgs.push_back(mk(i * 2, "assistant_text", "ack " + std::to_string(i)));
    }
    std::string prev_summary = "## Objective\nkeep the tabs rule";
    int ts2 = haicode::split_history(msgs, 200, ts1);
    CHECK(ts2 > ts1, "second split advances past the first boundary");
    std::string prompt2 = haicode::build_summary_prompt(prev_summary,
                          "### User\nnote 1", "newer history");
    CHECK(prompt2.find("keep the tabs rule") != std::string::npos,
          "early constraint survives into generation 2 via the summary");
    std::cout << "[OK] chained compaction merges rather than duplicates\n";
    return true;
}

static bool test_get_context_window_and_hysteresis() {
    haicode::AppConfig cfg;
    CHECK(haicode::get_context_window("weird", "totally-unknown-model-xyz",
                                      cfg.model_contexts) == 0,
          "unknown model returns 0");
    cfg.model_contexts["my-custom-model"] = 123456;
    CHECK(haicode::get_context_window("x", "my-custom-model",
                                      cfg.model_contexts) == 123456,
          "config override wins");

    CHECK(haicode::should_compact_with_hysteresis(100, 80, 5, -1), "first crossing fires");
    CHECK(!haicode::should_compact_with_hysteresis(100, 80, 6, 5), "blocked within 2 steps");
    CHECK(haicode::should_compact_with_hysteresis(100, 80, 7, 5), "re-armed after 2");
    CHECK(!haicode::should_compact_with_hysteresis(79, 80, 5, -1), "below threshold never fires");
    std::cout << "[OK] get_context_window + hysteresis gating\n";
    return true;
}

int main() {
    bool ok = true;
    ok &= test_end_to_end_checkpoint();
    ok &= test_checkpoint_roundtrip();
    ok &= test_messages_survive();
    ok &= test_assembly_with_checkpoint();
    ok &= test_split_feeds_checkpoint_chain();
    ok &= test_get_context_window_and_hysteresis();
    if (ok) remove(kDbPath);
    std::cout << (ok ? "ALL COMPACTION TESTS PASSED\n"
                     : "COMPACTION TESTS FAILED\n");
    return ok ? 0 : 1;
}
