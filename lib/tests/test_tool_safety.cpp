#include <haicode/haicode.h>
#include <haicode/tool.h>
#include <haicode/util.h>
#include <haicode/compaction.h>
#include <haicode/engine.h>
#include <haicode/db.h>
#include <iostream>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <nlohmann/json.hpp>

// Regression tests for the crash where invalid UTF-8 in tool output made
// nlohmann's strict serializer throw inside WebExtractTool::execute, escaping
// ToolRegistry::execute and aborting the app. Covers:
//   1. util::sanitize_utf8 — byte-level validation/replacement
//   2. ToolRegistry::execute — exceptions become failed results; output is
//      sanitized so downstream .dump() can never throw
//   3. util::truncate_utf8 — boundary-safe truncation (the byte-level cut
//      that orphaned a UTF-8 lead byte and aborted the app via
//      estimate_request_tokens → type_error.316 on the runner thread)
//   4. Crash repro: assemble_messages + estimate_request_tokens over an
//      old-turn tool result whose 10 KB cut lands mid-character

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

static bool registry_offline_mode_restricts_web_tools_only() {
    using haicode::tool_available;
    using haicode::set_offline_mode;

    // Process-wide flag: start from a known online state whatever earlier
    // tests did, and restore it on every exit path below.
    set_offline_mode(false);

    haicode::ToolRegistry reg;
    haicode::register_builtin_tools(reg);
    haicode::PermissionGate gate;
    gate.set_rules({{"*", "*", haicode::PermissionEffect::Allow}});

    // ---- Online: web tools available in every mode that allows them ----
    CHECK(tool_available("web_search", haicode::SessionMode::Build), "online Build allows web_search");
    CHECK(tool_available("web_search", haicode::SessionMode::Plan),  "online Plan allows web_search");
    CHECK(tool_available("web_extract", haicode::SessionMode::Chat), "online Chat allows web_extract");
    CHECK(tool_available("write", haicode::SessionMode::Build),      "online Build allows write");

    // ---- Offline: web tools hidden, local tools and mode rules intact ----
    set_offline_mode(true);
    for (auto m : {haicode::SessionMode::Build, haicode::SessionMode::Plan,
                   haicode::SessionMode::Chat}) {
        CHECK(!tool_available("web_search", m),  "offline must hide web_search");
        CHECK(!tool_available("web_extract", m), "offline must hide web_extract");
    }
    CHECK(tool_available("write", haicode::SessionMode::Build), "offline Build still allows write");
    CHECK(tool_available("read", haicode::SessionMode::Plan),   "offline Plan still allows read");
    CHECK(!tool_available("write", haicode::SessionMode::Plan), "offline Plan keeps blocking write");
    CHECK(!tool_available("read", haicode::SessionMode::Chat),  "offline Chat keeps blocking read");

    // Direct registry execution: a valid web_search call must be refused
    // BEFORE execution (no network I/O) as a failed result, not a denial,
    // so a provider returning a hidden call can't reach the network and the
    // model can recover.
    haicode::ToolContext ctx;
    ctx.working_dir = "/tmp";
    ctx.mode = haicode::SessionMode::Build;
    auto ws = reg.execute("web_search", {{"query", "x"}, {"max_results", 1}},
                          ctx, gate);
    CHECK(!ws.success, "offline web_search must fail");
    CHECK(!ws.denied,  "offline refusal is not a permission denial");
    CHECK(ws.error.find("[offline mode]") != std::string::npos,
          "error should name offline mode, got: " + ws.error);
    auto we = reg.execute("web_extract", {{"url", "https://example.com/"}},
                          ctx, gate);
    CHECK(!we.success && we.error.find("[offline mode]") != std::string::npos,
          "offline web_extract must fail with the offline marker");

    // Local tools still execute offline (read inside the working dir takes
    // the always-allow path, which must sit after the offline check).
    const std::string rp = "/tmp/tts_offline_read.txt";
    std::remove(rp.c_str());
    { std::ofstream wf(rp); wf << "data\n"; }
    auto rd = reg.execute("read", {{"path", rp}}, ctx, gate);
    CHECK(rd.success, "offline read must still work: " + rd.error);
    std::remove(rp.c_str());

    // ---- Back online: web availability restored, mode rules unchanged ----
    set_offline_mode(false);
    CHECK(tool_available("web_search", haicode::SessionMode::Build),
          "web_search availability must return after going back online");
    CHECK(!tool_available("write", haicode::SessionMode::Chat),
          "mode restrictions must survive an offline/online cycle");

    set_offline_mode(false);  // leave the global state online for other tests
    std::cout << "[OK] registry offline mode restricts web tools only\n";
    return true;
}

// ============================================================
// Engine e2e: offline note rides system_dynamic, not the cached body
// ============================================================

#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

class OfflineE2EProvider : public haicode::Provider {
public:
    std::string id() const override { return "fake"; }
    void cancel(const std::string& = "") override {}
    std::vector<std::string> list_models(std::string&) override {
        return {"fake-model"};
    }
    int get_model_context(const std::string&) const override { return 0; }
    void stream(const haicode::LLMRequest& req,
                haicode::StreamCallbacks cb, const std::string& = "") override {
        ++calls;
        last_system = req.system;
        last_dynamic = req.system_dynamic;
        has_web_tools = false;
        for (const auto& t : req.tools)
            if (t.name == "web_search" || t.name == "web_extract")
                has_web_tools = true;
        cb.on_text_delta("t", "ok");
        cb.on_finish(haicode::FinishReason::EndTurn, {}, {});
    }
    int calls = 0;
    std::string last_system;
    std::string last_dynamic;
    bool has_web_tools = false;
};

static void rm_rf_dir(const std::string& path) {
    DIR* d = opendir(path.c_str());
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
            std::string child = path + "/" + ent->d_name;
            struct stat st;
            if (lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                rm_rf_dir(child);
            else
                ::unlink(child.c_str());
        }
        closedir(d);
    }
    ::rmdir(path.c_str());
}

static bool engine_offline_note_rides_dynamic_block() {
    using haicode::set_offline_mode;
    char buf[] = "/tmp/hc_tts_offline_e2e_XXXXXX";
    if (!mkdtemp(buf)) { CHECK(false, "mkdtemp"); return false; }
    std::string tmp = buf;
    set_offline_mode(false);

    haicode::Database db(tmp + "/e2e.db");
    db.migrate();
    haicode::SessionStore store(db);
    auto provider = std::make_shared<OfflineE2EProvider>();
    haicode::ProviderRegistry registry;
    registry.register_provider(provider);
    haicode::ToolRegistry tools;
    haicode::register_builtin_tools(tools);
    haicode::PermissionGate perms;
    haicode::SessionEventBus bus;
    haicode::AppConfig cfg;
    cfg.model = "fake-model";
    cfg.provider = "fake";
    cfg.autoname_sessions = false;
    cfg.default_mode = "build";

    {
        haicode::SessionEngine engine(store, registry, tools, perms, bus, cfg);
        std::string sid = engine.create_session(tmp, "build",
                                                "fake-model", "fake");

        auto turn = [&](int want_calls) {
            for (int i = 0; i < 200; ++i) {
                if (provider->calls >= want_calls && !engine.is_running(sid))
                    return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            return false;
        };

        // Online: no note anywhere, web tools on the wire.
        engine.submit_prompt(sid, "hello");
        CHECK(turn(1), "first turn completes");
        CHECK(provider->last_dynamic.find("Offline mode") == std::string::npos,
              "online: dynamic block must not mention offline mode");
        CHECK(provider->last_system.find("Offline mode") == std::string::npos,
              "online: stable system prompt must not mention offline mode");
        CHECK(provider->has_web_tools, "online: web tools must be on the wire");

        // Offline: note in system_dynamic only; web tools filtered.
        set_offline_mode(true);
        engine.submit_prompt(sid, "hello again");
        CHECK(turn(2), "offline turn completes");
        CHECK(provider->last_dynamic.find("# Offline mode") != std::string::npos,
              "offline: dynamic block must carry the offline note");
        CHECK(provider->last_dynamic.find("web_search") != std::string::npos,
              "offline note should name the unavailable tools");
        CHECK(provider->last_system.find("Offline mode") == std::string::npos,
              "offline: cached stable body must stay byte-stable");
        CHECK(!provider->has_web_tools, "offline: web tools must be filtered");

        // Back online: note gone, web tools restored.
        set_offline_mode(false);
        engine.submit_prompt(sid, "hello once more");
        CHECK(turn(3), "return-to-online turn completes");
        CHECK(provider->last_dynamic.find("Offline mode") == std::string::npos,
              "back online: note must disappear");
        CHECK(provider->has_web_tools,
              "back online: web tools must return to the wire");
    }  // ~SessionEngine joins the loop threads

    set_offline_mode(false);
    rm_rf_dir(tmp);
    std::cout << "[OK] engine offline note rides system_dynamic only\n";
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
// util::truncate_utf8
// ============================================================

static bool truncate_passes_short_and_exact_fit() {
    using haicode::util::truncate_utf8;
    CHECK(truncate_utf8("hello", 10) == "hello", "short string unchanged");
    CHECK(truncate_utf8("hello", 5) == "hello", "exact fit unchanged");
    CHECK(truncate_utf8("", 0).empty(), "empty stays empty");
    CHECK(truncate_utf8("abc", 0).empty(), "zero cap yields empty");
    // Cut exactly at a character boundary keeps the character.
    std::string cjk = "\xe4\xb8\xad"; // 中, 3 bytes
    CHECK(truncate_utf8("ab" + cjk, 5) == "ab" + cjk,
          "boundary cut keeps complete character");
    std::cout << "[OK] truncate_passes_short_and_exact_fit\n";
    return true;
}

static bool truncate_mid_sequence_is_valid_utf8() {
    using haicode::util::truncate_utf8;
    using haicode::util::sanitize_utf8;
    const std::string two   = "\xc3\xa9";        // é
    const std::string three = "\xe4\xb8\xad";    // 中
    const std::string four  = "\xf0\x9f\x9a\x80"; // 🚀
    for (size_t cap = 1; cap <= 4; ++cap) {
        std::string out = truncate_utf8("x" + two, 1 + cap);
        CHECK(out.size() <= 1 + cap, "2-byte case respects cap");
        CHECK(sanitize_utf8(out) == out, "2-byte case output is valid UTF-8");
        out = truncate_utf8("x" + three, 1 + cap);
        CHECK(out.size() <= 1 + cap, "3-byte case respects cap");
        CHECK(sanitize_utf8(out) == out, "3-byte case output is valid UTF-8");
        out = truncate_utf8("x" + four, 1 + cap);
        CHECK(out.size() <= 1 + cap, "4-byte case respects cap");
        CHECK(sanitize_utf8(out) == out, "4-byte case output is valid UTF-8");
    }
    // Lead byte sitting exactly at the cut: the whole sequence is dropped.
    CHECK(truncate_utf8("a" + three, 2) == "a", "lead at cut dropped");
    // Exhaustive-ish sweep: every cut position of a mixed string.
    std::string mixed;
    for (int i = 0; i < 200; ++i) mixed += "a" + two + three + four + "\n";
    for (size_t cap = 0; cap <= mixed.size(); cap += 7) {
        std::string out = truncate_utf8(mixed, cap);
        CHECK(out.size() <= cap, "sweep respects cap");
        CHECK(sanitize_utf8(out) == out, "sweep output is valid UTF-8");
    }
    std::cout << "[OK] truncate_mid_sequence_is_valid_utf8\n";
    return true;
}

// ============================================================
// Crash repro: the abort() from the Codex-provider report
// ============================================================

// Reproduces the shipped crash: assemble_messages byte-cut an old-turn tool
// result mid-character, then estimate_request_tokens dumped it — strict
// serializer threw type_error.316 on the runner thread → std::terminate.
static bool crash_repro_old_tool_result_truncation() {
    using haicode::SessionMessage;
    // 10239 ASCII bytes, then a 3-byte 中 starting exactly at byte 10240
    // (0-based index 10239) — the old resize(10240) orphaned its lead byte.
    std::string big = std::string(10 * 1024 - 1, 'a')
                    + "\xe4\xb8\xad"
                    + std::string(600, 'b');

    SessionMessage tr;
    tr.seq = 1; tr.type = "tool_result";
    tr.data_json = nlohmann::json{
        {"call_id", "tc1"}, {"success", true}, {"output", big}}.dump();

    SessionMessage up;
    up.seq = 2; up.type = "user_prompted";
    up.data_json = nlohmann::json{{"text", "next turn"}}.dump();

    haicode::ContextBuilder builder;
    auto assembled = builder.assemble_messages({tr, up});
    CHECK(assembled.size() == 2, "two provider messages assembled");

    // The truncated tool result must be valid UTF-8 with the marker intact.
    std::string out = assembled[0]["content"][0]["content"].get<std::string>();
    CHECK(haicode::util::sanitize_utf8(out) == out,
          "truncated old tool result must be valid UTF-8");
    CHECK(out.find("[truncated: 603 more bytes]") != std::string::npos,
          "honest dropped-byte marker (603 = 3 + 600)");
    CHECK(out.find("\xe4\xb8\xad") == std::string::npos,
          "split character must not survive into the output");

    // The exact crash frame: this used to throw type_error.316.
    bool threw = false;
    std::string what;
    try {
        haicode::estimate_request_tokens("sys", "dyn", assembled, {});
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    CHECK(!threw, "estimate_request_tokens must not throw: " + what);

    // Also serialize the way providers do — same strict default handler.
    try {
        auto dumped = assembled[0].dump();
        (void)dumped;
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    CHECK(!threw, "strict dump of assembled message must not throw: " + what);
    std::cout << "[OK] crash_repro_old_tool_result_truncation\n";
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

    std::cout << "\n-- util::truncate_utf8 --\n";
    ok &= truncate_passes_short_and_exact_fit();
    ok &= truncate_mid_sequence_is_valid_utf8();

    std::cout << "\n-- Crash repro (abort via estimate_request_tokens) --\n";
    ok &= crash_repro_old_tool_result_truncation();

    std::cout << "\n-- ToolRegistry chokepoint --\n";
    ok &= registry_catches_tool_exception();
    ok &= registry_sanitizes_invalid_output();

    std::cout << "\n-- Gate tail + mode enforcement --\n";
    ok &= registry_unresolved_ask_denied();
    ok &= registry_ask_callback_allow_executes();
    ok &= registry_explicit_deny_still_denied();
    ok &= registry_chat_mode_blocks_tools();
    ok &= registry_offline_mode_restricts_web_tools_only();
    ok &= engine_offline_note_rides_dynamic_block();
    ok &= registry_plan_mode_blocks_write_allows_read();

    if (ok) {
        std::cout << "\nAll tool safety tests passed!\n";
        return 0;
    } else {
        std::cerr << "\nSome tests FAILED.\n";
        return 1;
    }
}
