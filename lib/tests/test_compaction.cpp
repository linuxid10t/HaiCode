#include <haicode/db.h>
#include <haicode/engine.h>
#include <haicode/model_info.h>
#include <haicode/config.h>
#include <iostream>
#include <cassert>
#include <cstdio>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << msg << "\n"; return false; } } while(0)

// Mirror of the engine's threshold arithmetic so the test is self-contained.
static bool should_compact(int prev_total_input, int window, int reserve,
                           double threshold_frac) {
    if (window <= 0) return false;
    int effective = window - reserve;
    int threshold = static_cast<int>(effective * threshold_frac);
    return threshold > 0 && prev_total_input >= threshold;
}

static bool test_threshold_basic() {
    // window=200000, reserve=8192, threshold=0.80 → effective=191808,
    // threshold=153446 (floor). Triggers at >= that.
    int window = 200000, reserve = 8192;
    double frac = 0.80;
    CHECK(should_compact(153446, window, reserve, frac),
          "should trigger at exactly effective*0.80");
    CHECK(should_compact(180000, window, reserve, frac),
          "should trigger well above threshold");
    CHECK(!should_compact(100000, window, reserve, frac),
          "should NOT trigger below threshold");
    std::cout << "[OK] threshold basic (153446 triggers, 100000 does not)\n";
    return true;
}

static bool test_threshold_unknown_window() {
    // window=0 → never trigger, regardless of token count.
    CHECK(!should_compact(500000, 0, 8192, 0.80),
          "window=0 must disable compaction");
    CHECK(!should_compact(500000, 0, 0, 0.80),
          "window=0 + reserve=0 must still disable compaction");
    std::cout << "[OK] threshold unknown window (window=0 disables)\n";
    return true;
}

static bool test_get_context_window_unknown() {
    haicode::AppConfig cfg;
    CHECK(haicode::get_context_window("weird", "totally-unknown-model-xyz",
                                      cfg.model_contexts) == 0,
          "unknown model should return 0 context window");
    std::cout << "[OK] get_context_window returns 0 for unknown model\n";
    return true;
}

static bool test_get_context_window_override() {
    haicode::AppConfig cfg;
    cfg.model_contexts["my-custom-model"] = 123456;
    CHECK(haicode::get_context_window("x", "my-custom-model",
                                      cfg.model_contexts) == 123456,
          "config override should take precedence");
    std::cout << "[OK] get_context_window honors config override\n";
    return true;
}

static bool test_threshold_with_estimate_fallback() {
    // When prev_total_input == 0 (first step of a turn), the engine falls back
    // to a char-based estimate (~chars/4). Simulate: a 700k-char request
    // estimates to ~175k tokens, which should trigger at the 153k threshold.
    int window = 200000, reserve = 8192;
    double frac = 0.80;
    int threshold = static_cast<int>((window - reserve) * frac); // 153446
    int estimated = 700000 / 4;  // 175000
    CHECK(estimated >= threshold,
          "estimated 175k should trigger at 153446 threshold");
    // A small 20k-char request (~5k tokens) should not.
    int small = 20000 / 4;
    CHECK(!(small >= threshold),
          "estimated 5k should NOT trigger");
    std::cout << "[OK] threshold estimate fallback (175k triggers, 5k does not)\n";
    return true;
}

static bool test_compact_messages_roundtrip() {
    const char* db_path = "/tmp/test_haicode_compact.db";
    std::remove(db_path);
    haicode::Database db(db_path);
    db.migrate();
    haicode::SessionStore store(db);

    auto session = store.create("/tmp", "default", "{}");

    // Build: [head: 4 msgs seq 1-4][tail: last user_prompted + 2 more]
    store.append_message(session.id, "user_prompted",  "{\"role\":\"user\",\"text\":\"old1\"}");
    store.append_message(session.id, "assistant_text", "{\"role\":\"assistant\",\"text\":\"old2\"}");
    store.append_message(session.id, "tool_result",    "{\"call_id\":\"c1\",\"output\":\"old3\"}");
    store.append_message(session.id, "assistant_text", "{\"role\":\"assistant\",\"text\":\"old4\"}");
    // tail begins here (seq 5)
    store.append_message(session.id, "user_prompted",  "{\"role\":\"user\",\"text\":\"current\"}");
    store.append_message(session.id, "assistant_text", "{\"role\":\"assistant\",\"text\":\"resp\"}");
    store.append_message(session.id, "tool_result",    "{\"call_id\":\"c2\",\"output\":\"cur-out\"}");

    auto before = store.load_messages(session.id);
    CHECK(before.size() == 7, "expected 7 messages before compaction");

    int tail_start_seq = 5;  // the last user_prompted
    std::string summary_json = "{\"role\":\"user\",\"text\":\"[Conversation summary]\\nstuff\"}";
    store.compact_messages(session.id, tail_start_seq, summary_json);

    auto after = store.load_messages(session.id);
    // head (4 rows) replaced by 1 summary → 7 - 4 + 1 = 4
    CHECK(after.size() == 4, "expected 4 messages after compaction, got "
          + std::to_string(after.size()));

    // First row must be the summary at seq 0.
    CHECK(after[0].type == "compaction_summary",
          "first message should be compaction_summary, got " + after[0].type);
    CHECK(after[0].seq == 0, "summary should be at seq 0");
    CHECK(after[0].data_json == summary_json, "summary data_json should match");

    // Tail must be intact: seqs 5,6,7 with original types.
    CHECK(after[1].seq == 5 && after[1].type == "user_prompted",
          "tail[0] should be seq 5 user_prompted");
    CHECK(after[2].seq == 6 && after[2].type == "assistant_text",
          "tail[1] should be seq 6 assistant_text");
    CHECK(after[3].seq == 7 && after[3].type == "tool_result",
          "tail[2] should be seq 7 tool_result");

    std::cout << "[OK] compact_messages DB round-trip (7->4, head replaced, tail intact)\n";
    return true;
}

static bool test_summary_emitted_as_assistant_role() {
    // Regression for the "two consecutive user messages" bug. A persisted
    // compaction_summary must be emitted as role=assistant so the alternation
    // with the tail's user_prompted is legal.
    std::vector<haicode::SessionMessage> msgs;
    {
        haicode::SessionMessage m;
        m.seq = 0; m.type = "compaction_summary";
        m.data_json = "{\"role\":\"assistant\",\"text\":\"prior summary content\"}";
        msgs.push_back(m);
    }
    {
        haicode::SessionMessage m;
        m.seq = 1; m.type = "user_prompted";
        m.data_json = "{\"role\":\"user\",\"text\":\"next question\"}";
        msgs.push_back(m);
    }

    haicode::ContextBuilder cb;
    auto out = cb.assemble_messages(msgs);
    CHECK(out.size() == 2, "expected 2 assembled messages, got " + std::to_string(out.size()));
    CHECK(out[0].value("role", "") == "assistant",
          "summary must assemble as assistant, got " + out[0].value("role","<missing>"));
    CHECK(out[1].value("role", "") == "user",
          "user_prompted must follow as user, got " + out[1].value("role","<missing>"));
    // Lead-in is present so the model knows what it's reading, and is flagged
    // as a system-injected compaction (not normal assistant speech).
    std::string c0 = out[0].value("content", "");
    CHECK(c0.find("compacted earlier turns") != std::string::npos,
          "summary content should carry the compaction flag");
    CHECK(c0.find("Summary of the prior conversation") != std::string::npos,
          "summary content should carry the lead-in");
    CHECK(c0.find("prior summary content") != std::string::npos,
          "summary content should retain the stored text");
    std::cout << "[OK] summary emitted as assistant role (legal alternation)\n";
    return true;
}

static bool test_choose_tail_multi_turn() {
    // Multi-turn case: should pick the seq of the last user_prompted.
    std::vector<haicode::SessionMessage> msgs;
    auto mk = [&](int seq, const std::string& type, const std::string& body) {
        haicode::SessionMessage m; m.seq = seq; m.type = type; m.data_json = body;
        msgs.push_back(m);
    };
    mk(1, "user_prompted",  "{\"text\":\"first\"}");
    mk(2, "assistant_text", "{\"text\":\"ack\"}");
    mk(3, "user_prompted",  "{\"text\":\"second\"}");  // tail starts here
    mk(4, "assistant_text", "{\"text\":\"ok\"}");
    int tail = haicode::choose_tail_start_seq(msgs, 4);
    CHECK(tail == 3, "multi-turn: expected tail=3, got " + std::to_string(tail));
    std::cout << "[OK] choose_tail_start_seq multi-turn picks last user_prompted\n";
    return true;
}

static bool test_choose_tail_single_long_turn() {
    // Single-turn fallback: one user_prompted + 6 assistant/tool round-trips.
    // K=4 means the tail begins at the 4th-from-last assistant_text.
    std::vector<haicode::SessionMessage> msgs;
    auto mk = [&](int seq, const std::string& type, const std::string& body) {
        haicode::SessionMessage m; m.seq = seq; m.type = type; m.data_json = body;
        msgs.push_back(m);
    };
    mk(1, "user_prompted", "{\"text\":\"go\"}");
    int seq = 2;
    for (int i = 0; i < 6; ++i) {
        mk(seq++, "assistant_text",
           "{\"text\":\"t\",\"tool_calls\":[{\"id\":\"c\",\"name\":\"read\",\"input\":{}}]}");
        mk(seq++, "tool_result", "{\"call_id\":\"c\",\"output\":\"x\"}");
    }
    // Assistants are at seqs 2,4,6,8,10,12. K=4 keeps last 4 → starts at seq 6.
    int tail = haicode::choose_tail_start_seq(msgs, 4);
    CHECK(tail == 6, "single-turn: expected tail=6, got " + std::to_string(tail));
    std::cout << "[OK] choose_tail_start_seq single-turn K=4 picks 4th-from-last assistant\n";
    return true;
}

static bool test_choose_tail_not_enough_round_trips() {
    // Single-turn with fewer than K round-trips → no compaction.
    std::vector<haicode::SessionMessage> msgs;
    auto mk = [&](int seq, const std::string& type, const std::string& body) {
        haicode::SessionMessage m; m.seq = seq; m.type = type; m.data_json = body;
        msgs.push_back(m);
    };
    mk(1, "user_prompted", "{\"text\":\"go\"}");
    for (int i = 0; i < 2; ++i) {
        mk(2 + i*2, "assistant_text",
           "{\"text\":\"t\",\"tool_calls\":[{\"id\":\"c\",\"name\":\"read\",\"input\":{}}]}");
        mk(3 + i*2, "tool_result", "{\"call_id\":\"c\",\"output\":\"x\"}");
    }
    int tail = haicode::choose_tail_start_seq(msgs, 4);
    CHECK(tail == -1, "expected -1 (fewer than K round-trips), got " + std::to_string(tail));
    std::cout << "[OK] choose_tail_start_seq returns -1 when round-trips < K\n";
    return true;
}

static bool test_summary_inserts_user_stub_before_assistant() {
    // Single-turn fallback puts assistant_text right after the summary.
    // assemble_messages must insert a stub user turn to keep alternation legal.
    std::vector<haicode::SessionMessage> msgs;
    {
        haicode::SessionMessage m;
        m.seq = 0; m.type = "compaction_summary";
        m.data_json = "{\"role\":\"assistant\",\"text\":\"prior\"}";
        msgs.push_back(m);
    }
    {
        haicode::SessionMessage m;
        m.seq = 2; m.type = "assistant_text";
        m.data_json = "{\"text\":\"resume\",\"tool_calls\":[{\"id\":\"c\",\"name\":\"read\",\"input\":{}}]}";
        msgs.push_back(m);
    }
    haicode::ContextBuilder cb;
    auto out = cb.assemble_messages(msgs);
    CHECK(out.size() == 3, "expected summary + stub + assistant, got "
          + std::to_string(out.size()));
    CHECK(out[0].value("role","") == "assistant", "out[0] should be assistant (summary)");
    CHECK(out[1].value("role","") == "user", "out[1] should be the stub user");
    CHECK(out[2].value("role","") == "assistant", "out[2] should be the resume assistant");
    std::cout << "[OK] assemble_messages inserts user stub before assistant follower\n";
    return true;
}

static bool test_select_summary_prompt() {
    // First-pass head (no prior summary) → first prompt.
    std::vector<haicode::SessionMessage> head1;
    {
        haicode::SessionMessage m;
        m.seq = 1; m.type = "user_prompted";
        m.data_json = "{\"text\":\"go\"}";
        head1.push_back(m);
    }
    const char* p1 = haicode::select_summary_prompt(head1);
    CHECK(p1 != nullptr, "first-pass prompt must not be null");
    CHECK(std::string(p1).find("Segment 1") == std::string::npos,
          "first-pass prompt should not mention Segment 1");
    CHECK(std::string(p1).find("Summarize the following conversation") != std::string::npos,
          "first-pass prompt should be the first-pass text");

    // Resegment head (starts with prior summary) → resegment prompt.
    std::vector<haicode::SessionMessage> head2;
    {
        haicode::SessionMessage m;
        m.seq = 0; m.type = "compaction_summary";
        m.data_json = "{\"role\":\"assistant\",\"text\":\"prior summary\"}";
        head2.push_back(m);
    }
    {
        haicode::SessionMessage m;
        m.seq = 1; m.type = "user_prompted";
        m.data_json = "{\"text\":\"new turn\"}";
        head2.push_back(m);
    }
    const char* p2 = haicode::select_summary_prompt(head2);
    CHECK(p2 != nullptr, "resegment prompt must not be null");
    CHECK(std::string(p2).find("Segment 1") != std::string::npos,
          "resegment prompt should mention Segment 1");
    CHECK(std::string(p2).find("Segment 2") != std::string::npos,
          "resegment prompt should mention Segment 2");
    CHECK(std::string(p2).find("VERBATIM") != std::string::npos,
          "resegment prompt should require verbatim preservation");
    std::cout << "[OK] select_summary_prompt picks resegment when head[0]==compaction_summary\n";
    return true;
}

static bool test_hysteresis_blocks_immediate_recompact() {
    const int threshold = 100000;
    const int over = 110000;       // safely above threshold
    const int below = 50000;       // safely below threshold

    // Never compacted → fires when over threshold.
    CHECK(haicode::should_compact_with_hysteresis(over, threshold, 5, -1),
          "first compaction should fire when above threshold and lcs<0");
    // Same step right after compaction → blocked (gap < 2).
    CHECK(!haicode::should_compact_with_hysteresis(over, threshold, 6, 5),
          "step+1 should block (gap=1 < 2)");
    // Step+2 → gap satisfied, allowed when still above threshold.
    CHECK(haicode::should_compact_with_hysteresis(over, threshold, 7, 5),
          "step+2 should re-allow when above threshold");
    // Below threshold → never fires regardless of lcs.
    CHECK(!haicode::should_compact_with_hysteresis(below, threshold, 9, 5),
          "below threshold should not fire even with sufficient gap");
    CHECK(!haicode::should_compact_with_hysteresis(below, threshold, 0, -1),
          "below threshold + never compacted should not fire");
    // Disabled threshold (window=0 path produces threshold<=0).
    CHECK(!haicode::should_compact_with_hysteresis(over, 0, 0, -1),
          "threshold<=0 must disable");
    std::cout << "[OK] hysteresis blocks immediate recompaction (2-step gap)\n";
    return true;
}

static bool test_compact_messages_noop() {
    const char* db_path = "/tmp/test_haicode_compact2.db";
    std::remove(db_path);
    haicode::Database db(db_path);
    db.migrate();
    haicode::SessionStore store(db);

    auto session = store.create("/tmp", "default", "{}");
    store.append_message(session.id, "user_prompted", "{\"role\":\"user\",\"text\":\"hi\"}");
    store.append_message(session.id, "assistant_text", "{\"role\":\"assistant\",\"text\":\"yo\"}");

    // keep_from_seq <= 1 is a no-op (nothing before the tail).
    store.compact_messages(session.id, 1, "{\"role\":\"user\",\"text\":\"summary\"}");

    auto msgs = store.load_messages(session.id);
    CHECK(msgs.size() == 2, "no-op compaction should leave messages untouched");
    CHECK(msgs[0].type != "compaction_summary",
          "no summary should be inserted on no-op");
    std::cout << "[OK] compact_messages no-op when keep_from_seq <= 1\n";
    return true;
}

int main() {
    std::cout << "=== Compaction Tests ===\n";
    bool ok = true;
    ok &= test_threshold_basic();
    ok &= test_threshold_unknown_window();
    ok &= test_threshold_with_estimate_fallback();
    ok &= test_get_context_window_unknown();
    ok &= test_get_context_window_override();
    ok &= test_compact_messages_roundtrip();
    ok &= test_summary_emitted_as_assistant_role();
    ok &= test_choose_tail_multi_turn();
    ok &= test_choose_tail_single_long_turn();
    ok &= test_choose_tail_not_enough_round_trips();
    ok &= test_summary_inserts_user_stub_before_assistant();
    ok &= test_select_summary_prompt();
    ok &= test_hysteresis_blocks_immediate_recompact();
    ok &= test_compact_messages_noop();
    if (ok) {
        std::cout << "\nAll compaction tests passed!\n";
        return 0;
    }
    std::cerr << "\nSome compaction tests FAILED.\n";
    return 1;
}
