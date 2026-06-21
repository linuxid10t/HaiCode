#include <haicode/db.h>
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
    ok &= test_compact_messages_noop();
    if (ok) {
        std::cout << "\nAll compaction tests passed!\n";
        return 0;
    }
    std::cerr << "\nSome compaction tests FAILED.\n";
    return 1;
}
