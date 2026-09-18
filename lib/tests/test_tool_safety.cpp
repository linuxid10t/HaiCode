#include <haicode/haicode.h>
#include <haicode/tool.h>
#include <haicode/util.h>
#include <iostream>
#include <fstream>
#include <cstdio>
#include <string>
#include <stdexcept>
#include <nlohmann/json.hpp>

// Regression tests for the crash where invalid UTF-8 in tool output made
// nlohmann's strict serializer throw inside WebExtractTool::execute, escaping
// ToolRegistry::execute and aborting the app. Covers:
//   1. util::sanitize_utf8 — byte-level validation/replacement
//   2. ToolRegistry::execute — exceptions become failed results; output is
//      sanitized so downstream .dump() can never throw

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << (msg) << "\n"; return false; } } while(0)

static const char kR[] = "\xEF\xBF\xBD"; // U+FFFD replacement character

// ============================================================
// util::sanitize_utf8
// ============================================================

static bool sanitize_ascii_unchanged() {
    std::string s = "hello world 123\n\t\"quotes\" & <tags>";
    CHECK(haicode::util::sanitize_utf8(s) == s, "ASCII must pass through unchanged");
    CHECK(haicode::util::sanitize_utf8("").empty(), "empty string stays empty");
    std::cout << "[OK] sanitize_ascii_unchanged\n";
    return true;
}

static bool sanitize_valid_multibyte_unchanged() {
    // é (2-byte), 中 (3-byte CJK), 🚀 (4-byte emoji)
    std::string s = "caf\xc3\xa9 \xe4\xb8\xad\xf0\x9f\x9a\x80";
    CHECK(haicode::util::sanitize_utf8(s) == s, "valid 2/3/4-byte sequences must pass through");
    std::cout << "[OK] sanitize_valid_multibyte_unchanged\n";
    return true;
}

static bool sanitize_lone_continuation() {
    // 0x80 is a continuation byte with no leading byte. (Adjacent literals —
    // "\x80b" would parse as the single escape \x80b.)
    CHECK(haicode::util::sanitize_utf8("a\x80" "b") == std::string("a") + kR + "b",
          "lone continuation byte replaced with U+FFFD");
    std::cout << "[OK] sanitize_lone_continuation\n";
    return true;
}

static bool sanitize_truncated_at_end() {
    // Trailing 2-byte sequence missing its second byte.
    CHECK(haicode::util::sanitize_utf8("\xc3") == kR, "truncated 2-byte seq replaced");
    // Trailing 3-byte sequence with one continuation byte: each invalid byte
    // is replaced individually (same per-byte behaviour as nlohmann's replace handler).
    CHECK(haicode::util::sanitize_utf8("x\xe4\xb8") == std::string("x") + kR + kR,
          "truncated 3-byte seq bytes replaced");
    std::cout << "[OK] sanitize_truncated_at_end\n";
    return true;
}

static bool sanitize_overlong() {
    // C0 80 is an overlong encoding of NUL — invalid.
    CHECK(haicode::util::sanitize_utf8("\xc0\x80") == std::string(kR) + kR,
          "overlong encoding rejected");
    std::cout << "[OK] sanitize_overlong\n";
    return true;
}

static bool sanitize_surrogate() {
    // ED A0 80 encodes U+D800 — a UTF-16 surrogate, invalid in UTF-8.
    CHECK(haicode::util::sanitize_utf8("\xed\xa0\x80") == std::string(kR) + kR + kR,
          "surrogate codepoint rejected");
    std::cout << "[OK] sanitize_surrogate\n";
    return true;
}

static bool sanitize_mixed() {
    // Valid é, then two invalid bytes (FF, FE), then valid text.
    std::string in  = "ok\xc3\xa9\xff\xfe bad";
    std::string out = haicode::util::sanitize_utf8(in);
    CHECK(out == std::string("ok\xc3\xa9") + kR + kR + " bad",
          "mixed valid/invalid input sanitized byte-accurately");
    std::cout << "[OK] sanitize_mixed\n";
    return true;
}

// The original crash: strict .dump() throws on invalid UTF-8. Prove the raw
// string still triggers it (documenting the failure mode) and that the
// sanitized version serializes cleanly.
static bool sanitize_prevents_dump_throw() {
    std::string bad = "page text \xff\xfe with latin1 bytes";

    bool threw = false;
    try {
        nlohmann::json j{{"text", bad}};
        (void)j.dump(2);
    } catch (const nlohmann::json::exception&) {
        threw = true;
    }
    CHECK(threw, "strict dump of raw invalid UTF-8 must throw (the original crash)");

    std::string good = haicode::util::sanitize_utf8(bad);
    try {
        nlohmann::json j{{"text", good}};
        std::string dumped = j.dump(2);
        CHECK(dumped.find(kR) != std::string::npos, "sanitized dump contains U+FFFD");
    } catch (const std::exception& e) {
        CHECK(false, std::string("dump of sanitized text must not throw: ") + e.what());
    }
    std::cout << "[OK] sanitize_prevents_dump_throw\n";
    return true;
}

// ============================================================
// ToolRegistry::execute chokepoint
// ============================================================

class ThrowingTool : public haicode::Tool {
public:
    std::string name() const override { return "fake_throw"; }
    std::string description() const override { return "test tool that throws"; }
    nlohmann::json input_schema() const override { return {}; }
    haicode::ToolResult execute(const nlohmann::json&, const haicode::ToolContext&) override {
        throw std::runtime_error("boom");
    }
};

class BadUtf8Tool : public haicode::Tool {
public:
    std::string name() const override { return "fake_bad_utf8"; }
    std::string description() const override { return "test tool with invalid UTF-8 output"; }
    nlohmann::json input_schema() const override { return {}; }
    haicode::ToolResult execute(const nlohmann::json&, const haicode::ToolContext&) override {
        haicode::ToolResult r;
        r.success = true;
        r.output  = "ok\xff" "bad"; // 0xFF is not valid UTF-8 (split literal: \xffb would be one escape)
        return r;
    }
};

static bool registry_catches_tool_exception() {
    haicode::ToolRegistry reg;
    reg.register_tool(std::make_shared<ThrowingTool>());

    haicode::PermissionGate gate;
    gate.set_rules({{"fake_throw", "*", haicode::PermissionEffect::Allow}});

    haicode::ToolContext ctx;
    haicode::ToolResult r;
    try {
        r = reg.execute("fake_throw", {}, ctx, gate);
    } catch (const std::exception& e) {
        CHECK(false, std::string("registry must not propagate tool exceptions: ") + e.what());
    }
    CHECK(!r.success, "throwing tool should yield a failed result");
    CHECK(r.error.find("boom") != std::string::npos, "error should carry the exception message");
    CHECK(r.error.find("fake_throw") != std::string::npos, "error should name the tool");
    std::cout << "[OK] registry_catches_tool_exception\n";
    return true;
}

static bool registry_sanitizes_invalid_output() {
    haicode::ToolRegistry reg;
    reg.register_tool(std::make_shared<BadUtf8Tool>());

    haicode::PermissionGate gate;
    gate.set_rules({{"fake_bad_utf8", "*", haicode::PermissionEffect::Allow}});

    haicode::ToolContext ctx;
    auto r = reg.execute("fake_bad_utf8", {}, ctx, gate);
    CHECK(r.success, "tool itself succeeded");
    CHECK(r.output == std::string("ok") + kR + "bad",
          "invalid bytes in tool output must be sanitized to U+FFFD");

    // And the result must now serialize without throwing — the exact call that
    // crashed the app (engine.cpp persists tool results via data.dump()).
    try {
        nlohmann::json j{{"output", r.output}};
        (void)j.dump();
    } catch (const std::exception& e) {
        CHECK(false, std::string("sanitized result must serialize: ") + e.what());
    }
    std::cout << "[OK] registry_sanitizes_invalid_output\n";
    return true;
}

// ============================================================
// Gate tail: explicit-Allow requirement (review #10) and
// execution-time mode enforcement (review #1)
// ============================================================

static bool registry_unresolved_ask_denied() {
    // No rules, no ask callback: check() returns Ask. Must NOT execute.
    haicode::ToolRegistry reg;
    haicode::register_builtin_tools(reg);
    haicode::PermissionGate gate;
    haicode::ToolContext ctx;
    ctx.working_dir = "/tmp";

    const std::string p = "/tmp/tts_unresolved.txt";
    std::remove(p.c_str());
    auto r = reg.execute("write", {{"path", p}, {"content", "x"}}, ctx, gate);
    CHECK(!r.success, "unresolved Ask must not execute the tool");
    CHECK(r.denied,   "denied flag should be set for unresolved Ask");
    CHECK(r.error.find("not granted") != std::string::npos,
          "error should say 'not granted', got: " + r.error);
    std::ifstream f(p);
    CHECK(!f.is_open(), "target file must not be created");
    std::remove(p.c_str());
    std::cout << "[OK] registry unresolved Ask is denied, not executed\n";
    return true;
}

static bool registry_ask_callback_allow_executes() {
    haicode::ToolRegistry reg;
    haicode::register_builtin_tools(reg);
    haicode::PermissionGate gate;
    gate.set_ask_callback([](const std::string&, const std::string&,
                               const std::string&, const nlohmann::json&) {
        return haicode::PermissionEffect::Allow;
    });
    haicode::ToolContext ctx;
    ctx.working_dir = "/tmp";

    const std::string p = "/tmp/tts_ask_allow.txt";
    std::remove(p.c_str());
    auto r = reg.execute("write", {{"path", p}, {"content", "x"}}, ctx, gate);
    CHECK(r.success, "Ask resolved to Allow by callback must execute: " + r.error);
    std::ifstream f(p);
    CHECK(f.is_open(), "file should exist after allowed write");
    std::remove(p.c_str());
    std::cout << "[OK] registry Ask resolved to Allow executes\n";
    return true;
}

static bool registry_explicit_deny_still_denied() {
    haicode::ToolRegistry reg;
    haicode::register_builtin_tools(reg);
    haicode::PermissionGate gate;
    gate.set_rules({{"write", "*", haicode::PermissionEffect::Deny}});
    haicode::ToolContext ctx;
    ctx.working_dir = "/tmp";

    const std::string p = "/tmp/tts_deny.txt";
    std::remove(p.c_str());
    auto r = reg.execute("write", {{"path", p}, {"content", "x"}}, ctx, gate);
    CHECK(!r.success, "explicit Deny must block execution");
    CHECK(r.denied,   "denied flag should be set");
    CHECK(r.error.find("Permission denied") != std::string::npos,
          "error should say 'Permission denied', got: " + r.error);
    std::remove(p.c_str());
    std::cout << "[OK] registry explicit Deny still denied\n";
    return true;
}

static bool registry_chat_mode_blocks_tools() {
    // Rules allow everything — the mode restriction alone must block.
    haicode::ToolRegistry reg;
    haicode::register_builtin_tools(reg);
    haicode::PermissionGate gate;
    gate.set_rules({{"*", "*", haicode::PermissionEffect::Allow}});
    haicode::ToolContext ctx;
    ctx.working_dir = "/tmp";
    ctx.mode = haicode::SessionMode::Chat;

    const std::string p = "/tmp/tts_chat_write.txt";
    std::remove(p.c_str());
    auto w = reg.execute("write", {{"path", p}, {"content", "x"}}, ctx, gate);
    CHECK(!w.success, "Chat mode must block write even when rules allow");
    CHECK(!w.denied,   "mode restriction is not a permission denial");
    CHECK(w.error.find("mode restriction") != std::string::npos,
          "error should mention the mode restriction, got: " + w.error);
    CHECK(w.error.find("chat") != std::string::npos, "error should name chat mode");
    std::ifstream f(p);
    CHECK(!f.is_open(), "file must not be created in Chat mode");

    // Chat blocks even read (before the working-dir bypass).
    const std::string rp = "/tmp/tts_chat_read.txt";
    std::ofstream wf(rp); wf << "data\n"; wf.close();
    auto rd = reg.execute("read", {{"path", rp}}, ctx, gate);
    CHECK(!rd.success, "Chat mode must block read too");
    CHECK(rd.error.find("mode restriction") != std::string::npos,
          "read error should mention the mode restriction");
    std::remove(p.c_str());
    std::remove(rp.c_str());

    // Allowed Chat tools still pass the mode gate (web_search needs no net
    // access to prove the gate lets it through to execution).
    auto ws = reg.execute("web_search", {{"query", "x"}, {"max_results", 1}}, ctx, gate);
    CHECK(ws.error.find("mode restriction") == std::string::npos,
          "web_search must not hit the mode restriction");

    std::cout << "[OK] registry Chat mode blocks write/read, passes web_search\n";
    return true;
}

static bool registry_plan_mode_blocks_write_allows_read() {
    haicode::ToolRegistry reg;
    haicode::register_builtin_tools(reg);
    haicode::PermissionGate gate;
    gate.set_rules({{"*", "*", haicode::PermissionEffect::Allow}});
    haicode::ToolContext ctx;
    ctx.working_dir = "/tmp";
    ctx.mode = haicode::SessionMode::Plan;

    const std::string p = "/tmp/tts_plan_write.txt";
    std::remove(p.c_str());
    auto w = reg.execute("write", {{"path", p}, {"content", "x"}}, ctx, gate);
    CHECK(!w.success, "Plan mode must block write");
    CHECK(w.error.find("plan") != std::string::npos,
          "error should name plan mode, got: " + w.error);
    std::ifstream f(p);
    CHECK(!f.is_open(), "file must not be created in Plan mode");
    std::remove(p.c_str());

    const std::string rp = "/tmp/tts_plan_read.txt";
    std::ofstream wf(rp); wf << "data\n"; wf.close();
    auto rd = reg.execute("read", {{"path", rp}}, ctx, gate);
    CHECK(rd.success, "Plan mode must still allow read: " + rd.error);
    std::remove(rp.c_str());
    std::cout << "[OK] registry Plan mode blocks write, allows read\n";
    return true;
}

// ============================================================

int main() {
    std::cout << "=== Tool Safety Tests ===\n\n";

    bool ok = true;

    std::cout << "-- util::sanitize_utf8 --\n";
    ok &= sanitize_ascii_unchanged();
    ok &= sanitize_valid_multibyte_unchanged();
    ok &= sanitize_lone_continuation();
    ok &= sanitize_truncated_at_end();
    ok &= sanitize_overlong();
    ok &= sanitize_surrogate();
    ok &= sanitize_mixed();
    ok &= sanitize_prevents_dump_throw();

    std::cout << "\n-- ToolRegistry chokepoint --\n";
    ok &= registry_catches_tool_exception();
    ok &= registry_sanitizes_invalid_output();

    std::cout << "\n-- Gate tail + mode enforcement --\n";
    ok &= registry_unresolved_ask_denied();
    ok &= registry_ask_callback_allow_executes();
    ok &= registry_explicit_deny_still_denied();
    ok &= registry_chat_mode_blocks_tools();
    ok &= registry_plan_mode_blocks_write_allows_read();

    if (ok) {
        std::cout << "\nAll tool safety tests passed!\n";
        return 0;
    } else {
        std::cerr << "\nSome tests FAILED.\n";
        return 1;
    }
}
