#include <haicode/config.h>
#include <haicode/tool.h>
#include <haicode/haicode.h>
#include <iostream>
#include <fstream>
#include "test_check.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

// ---- Helpers ----

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    TEST_REQUIRE(f.is_open(), "failed to open " + path);
    f << content;
}

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << (msg) << "\n"; return false; } } while(0)

// ============================================================
// ConfigLoader::load_file
// ============================================================

static bool cfg_basic_fields() {
    const std::string p = "/tmp/tfc_basic.json";
    write_file(p, R"({"model":"claude-opus-4","provider":"anthropic","agent":"code"})");
    haicode::ConfigLoader loader;
    auto cfg = loader.load_file(p);
    CHECK(cfg.model    == "claude-opus-4",  "model mismatch");
    CHECK(cfg.provider == "anthropic",      "provider mismatch");
    CHECK(cfg.agent    == "code",           "agent mismatch");
    std::remove(p.c_str());
    std::cout << "[OK] load_file basic fields\n";
    return true;
}

static bool cfg_permissions() {
    const std::string p = "/tmp/tfc_perms.json";
    write_file(p, R"({
        "permissions": [
            {"action": "write", "resource": "/tmp/*", "effect": "allow"},
            {"action": "bash",  "resource": "*",      "effect": "deny"},
            {"action": "read",  "resource": "*"}
        ]
    })");
    haicode::ConfigLoader loader;
    auto cfg = loader.load_file(p);
    CHECK(cfg.permissions.size() == 3, "expected 3 permissions");
    CHECK(cfg.permissions[0].action   == "write",                    "perm[0] action");
    CHECK(cfg.permissions[0].resource == "/tmp/*",                   "perm[0] resource");
    CHECK(cfg.permissions[0].effect   == haicode::PermissionEffect::Allow, "perm[0] allow");
    CHECK(cfg.permissions[1].effect   == haicode::PermissionEffect::Deny,  "perm[1] deny");
    CHECK(cfg.permissions[2].effect   == haicode::PermissionEffect::Ask,   "perm[2] ask (default)");
    std::remove(p.c_str());
    std::cout << "[OK] load_file permissions\n";
    return true;
}

static bool cfg_permission_default_resource() {
    const std::string p = "/tmp/tfc_perm_default_res.json";
    write_file(p, R"({"permissions":[{"action":"bash","effect":"allow"}]})");
    haicode::ConfigLoader loader;
    auto cfg = loader.load_file(p);
    CHECK(cfg.permissions.size() == 1,    "expected 1 permission");
    CHECK(cfg.permissions[0].resource == "*", "resource should default to *");
    std::remove(p.c_str());
    std::cout << "[OK] load_file permission default resource\n";
    return true;
}

static bool cfg_build_command() {
    const std::string p = "/tmp/tfc_build.json";
    write_file(p, R"({"build_command":"make -C build -j4 2>&1"})");
    haicode::ConfigLoader loader;
    auto cfg = loader.load_file(p);
    CHECK(cfg.build_command == "make -C build -j4 2>&1", "build_command mismatch");
    std::remove(p.c_str());
    std::cout << "[OK] load_file build_command\n";
    return true;
}

static bool cfg_web_search() {
    const std::string p = "/tmp/tfc_websearch.json";
    write_file(p, R"({"web_search":{"engine":"ddg_lite","max_results":8,
        "api_keys":{"exa":"exa-key-1","zai":"zai-key-2"}}})");
    haicode::ConfigLoader loader;
    auto cfg = loader.load_file(p);
    CHECK(cfg.web_search_engine      == "ddg_lite", "web_search engine mismatch");
    CHECK(cfg.web_search_max_results == 8,           "web_search max_results mismatch");
    CHECK(cfg.web_search_api_keys.size() == 2,       "expected 2 web_search api keys");
    CHECK(cfg.web_search_api_keys["exa"] == "exa-key-1", "exa api key mismatch");
    CHECK(cfg.web_search_api_keys["zai"] == "zai-key-2", "zai api key mismatch");
    std::remove(p.c_str());
    std::cout << "[OK] load_file web_search config\n";
    return true;
}

static bool cfg_instructions() {
    const std::string p = "/tmp/tfc_instruct.json";
    write_file(p, R"({"instructions":["Be concise.","No emojis."]})");
    haicode::ConfigLoader loader;
    auto cfg = loader.load_file(p);
    CHECK(cfg.instructions.size() == 2,    "expected 2 instructions");
    CHECK(cfg.instructions[0] == "Be concise.", "instruction[0] mismatch");
    CHECK(cfg.instructions[1] == "No emojis.",  "instruction[1] mismatch");
    std::remove(p.c_str());
    std::cout << "[OK] load_file instructions\n";
    return true;
}

static bool cfg_providers() {
    const std::string p = "/tmp/tfc_providers.json";
    write_file(p, R"({"providers":{"anthropic":{"api_key":"sk-123","base_url":"https://api.anthropic.com"}}})");
    haicode::ConfigLoader loader;
    auto cfg = loader.load_file(p);
    CHECK(cfg.providers.count("anthropic") == 1, "anthropic provider missing");
    CHECK(cfg.providers["anthropic"].api_key  == "sk-123",                    "api_key mismatch");
    CHECK(cfg.providers["anthropic"].base_url == "https://api.anthropic.com", "base_url mismatch");
    std::remove(p.c_str());
    std::cout << "[OK] load_file providers\n";
    return true;
}

static bool cfg_missing_file() {
    haicode::ConfigLoader loader;
    auto cfg = loader.load_file("/tmp/tfc_nonexistent_xyz.json");
    CHECK(cfg.model.empty(),    "model should be empty for missing file");
    CHECK(cfg.provider.empty(), "provider should be empty for missing file");
    std::cout << "[OK] load_file missing file returns defaults\n";
    return true;
}

static bool cfg_invalid_json() {
    const std::string p = "/tmp/tfc_invalid.json";
    write_file(p, "{ this is not valid json !!!");
    haicode::ConfigLoader loader;
    auto cfg = loader.load_file(p);
    CHECK(cfg.model.empty(), "invalid JSON should return default config");
    std::remove(p.c_str());
    std::cout << "[OK] load_file invalid JSON returns defaults\n";
    return true;
}

static bool cfg_model_contexts() {
    const std::string p = "/tmp/tfc_contexts.json";
    write_file(p, R"({"models":{"claude-opus-4":200000,"claude-haiku-4":128000}})");
    haicode::ConfigLoader loader;
    auto cfg = loader.load_file(p);
    CHECK(cfg.model_contexts.count("claude-opus-4")  == 1, "opus context missing");
    CHECK(cfg.model_contexts["claude-opus-4"]  == 200000,  "opus context wrong");
    CHECK(cfg.model_contexts["claude-haiku-4"] == 128000,  "haiku context wrong");
    std::remove(p.c_str());
    std::cout << "[OK] load_file model context overrides\n";
    return true;
}

// ============================================================
// merge (ConfigLayer, presence-based)
// ============================================================

static haicode::ConfigLayer make_layer(const haicode::AppConfig& vals,
                                       std::initializer_list<std::string> keys) {
    haicode::ConfigLayer l;
    l.values = vals;
    for (auto& k : keys) l.present.insert(k);
    return l;
}

static bool merge_scalar_overlay() {
    haicode::AppConfig b, o;
    b.model    = "base-model";
    b.provider = "base-provider";
    o.model    = "overlay-model";
    auto result = haicode::merge(make_layer(b, {"model", "provider"}),
                                 make_layer(o, {"model"}));
    CHECK(result.model    == "overlay-model",   "overlay model should win");
    CHECK(result.provider == "base-provider",   "base provider should survive");
    std::cout << "[OK] merge scalar overlay wins\n";
    return true;
}

static bool merge_empty_overlay_does_not_clear() {
    haicode::AppConfig b;
    b.model    = "keep-me";
    b.provider = "keep-me-too";
    // Empty layer: nothing present — should not clear base values
    auto result = haicode::merge(make_layer(b, {"model", "provider"}),
                                 haicode::ConfigLayer{});
    CHECK(result.model    == "keep-me",     "empty overlay should not clear model");
    CHECK(result.provider == "keep-me-too", "empty overlay should not clear provider");
    std::cout << "[OK] merge empty overlay preserves base\n";
    return true;
}

static bool merge_permissions_appended() {
    haicode::AppConfig b, o;
    b.permissions.push_back({"bash", "*", haicode::PermissionEffect::Deny});
    o.permissions.push_back({"write", "/tmp/*", haicode::PermissionEffect::Allow});
    auto result = haicode::merge(make_layer(b, {"permissions"}),
                                 make_layer(o, {"permissions"}));
    CHECK(result.permissions.size() == 2, "expected 2 permissions after merge");
    CHECK(result.permissions[0].action == "bash",  "base permission should be first");
    CHECK(result.permissions[1].action == "write", "overlay permission should be appended");
    std::cout << "[OK] merge permissions appended\n";
    return true;
}

static bool merge_instructions_appended() {
    haicode::AppConfig b, o;
    b.instructions = {"base instruction"};
    o.instructions = {"overlay instruction"};
    auto result = haicode::merge(make_layer(b, {"instructions"}),
                                 make_layer(o, {"instructions"}));
    CHECK(result.instructions.size() == 2,                     "expected 2 instructions");
    CHECK(result.instructions[0] == "base instruction",    "base first");
    CHECK(result.instructions[1] == "overlay instruction", "overlay appended");
    std::cout << "[OK] merge instructions appended\n";
    return true;
}

static bool merge_build_command_overlay() {
    haicode::AppConfig b, o;
    b.build_command = "make base";
    o.build_command = "make overlay";
    auto result = haicode::merge(make_layer(b, {"build_command"}),
                                 make_layer(o, {"build_command"}));
    CHECK(result.build_command == "make overlay", "overlay build_command should win");
    std::cout << "[OK] merge build_command overlay wins\n";
    return true;
}

static bool merge_build_command_base_preserved() {
    haicode::AppConfig b;
    b.build_command = "make base";
    // overlay layer has no build_command present
    auto result = haicode::merge(make_layer(b, {"build_command"}),
                                 haicode::ConfigLayer{});
    CHECK(result.build_command == "make base", "base build_command should be preserved");
    std::cout << "[OK] merge build_command base preserved when overlay empty\n";
    return true;
}

static bool merge_providers_merged() {
    haicode::AppConfig b, o;
    haicode::ProviderConfig anthropic;
    anthropic.id = "anthropic";
    anthropic.type = "anthropic";
    anthropic.api_key = "key-a";
    b.providers["anthropic"] = anthropic;
    haicode::ProviderConfig openai;
    openai.id = "openai";
    openai.type = "openai";
    openai.api_key = "key-b";
    o.providers["openai"] = openai;
    auto result = haicode::merge(make_layer(b, {"providers/anthropic/api_key"}),
                                 make_layer(o, {"providers/openai/api_key"}));
    CHECK(result.providers.count("anthropic") == 1, "anthropic should survive");
    CHECK(result.providers.count("openai")    == 1, "openai should be added");
    std::cout << "[OK] merge providers merged\n";
    return true;
}

// Task 7 regression: a project providers entry used to be replaced
// wholesale, dropping the global api_key. Per-subkey merge keeps absent
// subkeys from the base entry.
static bool merge_providers_per_subkey() {
    const std::string gp = "/tmp/tfc_merge_global.json";
    const std::string pp = "/tmp/tfc_merge_project.json";
    write_file(gp, R"({"providers":{"anthropic":{"type":"anthropic","api_key":"sk-global"}}})");
    write_file(pp, R"({"providers":{"anthropic":{"base_url":"https://proxy.example/v1"}}})");
    auto result = haicode::merge(haicode::load_layer(gp), haicode::load_layer(pp));
    CHECK(result.providers.count("anthropic") == 1, "entry present");
    CHECK(result.providers["anthropic"].api_key == "sk-global",
          "absent api_key subkey must keep the global key");
    CHECK(result.providers["anthropic"].base_url == "https://proxy.example/v1",
          "present base_url subkey must win");
    std::remove(gp.c_str());
    std::remove(pp.c_str());
    std::cout << "[OK] merge providers per-subkey (no dropped api_key)\n";
    return true;
}

static bool merge_web_search_overlay() {
    haicode::AppConfig b, o;
    b.web_search_engine      = "exa";
    b.web_search_max_results = 5;
    b.web_search_api_keys["exa"] = "base-exa";
    b.web_search_api_keys["zai"] = "base-zai";
    o.web_search_engine      = "ddg_lite";
    o.web_search_max_results = 10;
    o.web_search_api_keys["exa"] = "overlay-exa";
    auto result = haicode::merge(
        make_layer(b, {"web_search/engine", "web_search/max_results",
                       "web_search/api_keys/exa", "web_search/api_keys/zai"}),
        make_layer(o, {"web_search/engine", "web_search/max_results",
                       "web_search/api_keys/exa"}));
    CHECK(result.web_search_engine      == "ddg_lite", "engine overlay should win");
    CHECK(result.web_search_max_results == 10,          "max_results overlay should win");
    CHECK(result.web_search_api_keys.size() == 2,       "api_keys merge should keep base keys");
    CHECK(result.web_search_api_keys["exa"] == "overlay-exa", "exa key overlay should win");
    CHECK(result.web_search_api_keys["zai"] == "base-zai",    "zai key base should survive");
    std::cout << "[OK] merge web_search overlay wins\n";
    return true;
}

static bool merge_web_search_default_overlay_preserves_base() {
    // A default-constructed overlay (project config file absent) must not
    // clobber the base engine. Presence tracking makes this structural: an
    // empty layer records nothing present.
    haicode::AppConfig b;
    b.web_search_engine = "exa";
    auto result = haicode::merge(make_layer(b, {"web_search/engine"}),
                                 haicode::ConfigLayer{});
    CHECK(result.web_search_engine == "exa",
          "default overlay should not clobber base engine");
    std::cout << "[OK] merge default overlay preserves base engine\n";
    return true;
}

// Task 7 regression: an absent project default_mode used to clobber a
// global "build" — the struct default ("plan") participated in the merge
// because "not specified" was represented as a default value.
static bool merge_absent_project_keys_preserve_global() {
    const std::string gp = "/tmp/tfc_absent_global.json";
    const std::string pp = "/tmp/tfc_absent_project.json";
    write_file(gp, R"({"default_mode":"build","web_search":{"max_results":10}})");
    write_file(pp, R"({"model":"claude-sonnet-4"})");
    auto result = haicode::merge(haicode::load_layer(gp), haicode::load_layer(pp));
    CHECK(result.default_mode == "build",
          "absent project default_mode must preserve global 'build'");
    CHECK(result.web_search_max_results == 10,
          "absent project max_results must preserve global 10");
    CHECK(result.model == "claude-sonnet-4", "present project key still wins");
    std::remove(gp.c_str());
    std::remove(pp.c_str());
    std::cout << "[OK] merge absent project keys preserve global\n";
    return true;
}

// Task 7 regression: overlay values equal to a struct default were ignored
// ("auto_compact" could be turned off but never back on; max_results == 5
// never overrode). Presence now drives the merge, so an explicit value
// always wins — in both directions.
static bool merge_explicit_default_value_still_overrides() {
    const std::string gp = "/tmp/tfc_bool_global.json";
    const std::string pp = "/tmp/tfc_bool_project.json";
    write_file(gp, R"({"auto_compact":false,"autoname_sessions":false,"web_search":{"max_results":10}})");
    write_file(pp, R"({"auto_compact":true,"web_search":{"max_results":5}})");
    auto result = haicode::merge(haicode::load_layer(gp), haicode::load_layer(pp));
    CHECK(result.auto_compact == true,
          "explicit project auto_compact:true must re-enable");
    CHECK(result.web_search_max_results == 5,
          "project max_results equal to the struct default must still override");
    CHECK(result.autoname_sessions == false,
          "absent project autoname_sessions must preserve global false");
    std::remove(gp.c_str());
    std::remove(pp.c_str());
    std::cout << "[OK] merge explicit-default values still override\n";
    return true;
}

// ============================================================
// PermissionGate
// ============================================================

static haicode::PermissionGate make_gate(std::vector<haicode::PermissionRule> rules = {}) {
    haicode::PermissionGate gate;
    gate.set_rules(rules);
    return gate;
}

static bool perm_allow_rule() {
    auto gate = make_gate({{"bash", "*", haicode::PermissionEffect::Allow}});
    auto r = gate.check("bash", "/any/path");
    CHECK(r == haicode::PermissionEffect::Allow, "expected Allow");
    std::cout << "[OK] permission allow rule\n";
    return true;
}

static bool perm_deny_rule() {
    auto gate = make_gate({{"bash", "*", haicode::PermissionEffect::Deny}});
    auto r = gate.check("bash", "/any/path");
    CHECK(r == haicode::PermissionEffect::Deny, "expected Deny");
    std::cout << "[OK] permission deny rule\n";
    return true;
}

static bool perm_no_match_returns_ask() {
    auto gate = make_gate({{"write", "*", haicode::PermissionEffect::Allow}});
    auto r = gate.check("bash", "/any/path");
    CHECK(r == haicode::PermissionEffect::Ask, "expected Ask when no rule matches");
    std::cout << "[OK] permission no match returns Ask\n";
    return true;
}

static bool perm_wildcard_action() {
    auto gate = make_gate({{"*", "*", haicode::PermissionEffect::Allow}});
    CHECK(gate.check("bash",  "/x") == haicode::PermissionEffect::Allow, "bash should match *");
    CHECK(gate.check("write", "/y") == haicode::PermissionEffect::Allow, "write should match *");
    CHECK(gate.check("read",  "/z") == haicode::PermissionEffect::Allow, "read should match *");
    std::cout << "[OK] permission wildcard action\n";
    return true;
}

static bool perm_fnmatch_resource() {
    auto gate = make_gate({{"write", "/tmp/*", haicode::PermissionEffect::Allow}});
    CHECK(gate.check("write", "/tmp/foo.txt") == haicode::PermissionEffect::Allow,
          "should allow /tmp/foo.txt");
    CHECK(gate.check("write", "/home/user/foo.txt") == haicode::PermissionEffect::Ask,
          "should not match /home/user/foo.txt");
    std::cout << "[OK] permission fnmatch resource pattern\n";
    return true;
}

static bool perm_last_rule_wins() {
    // Rules are matched in reverse order (last added wins)
    auto gate = make_gate({
        {"bash", "*", haicode::PermissionEffect::Deny},
        {"bash", "*", haicode::PermissionEffect::Allow},
    });
    auto r = gate.check("bash", "/any");
    CHECK(r == haicode::PermissionEffect::Allow, "last rule (Allow) should win");
    std::cout << "[OK] permission last rule wins\n";
    return true;
}

static bool perm_session_rules_override_config() {
    // Config says deny, session says allow — session wins
    auto gate = make_gate({{"bash", "*", haicode::PermissionEffect::Deny}});
    gate.set_session_rules({{"bash", "*", haicode::PermissionEffect::Allow}});
    auto r = gate.check("bash", "/any");
    CHECK(r == haicode::PermissionEffect::Allow, "session rule should override config deny");
    std::cout << "[OK] permission session rules override config rules\n";
    return true;
}

static bool perm_add_allow() {
    haicode::PermissionGate gate;
    gate.add_allow("write", "/tmp/*");
    CHECK(gate.check("write", "/tmp/foo") == haicode::PermissionEffect::Allow,
          "add_allow should create a session Allow rule");
    CHECK(gate.check("write", "/etc/foo") == haicode::PermissionEffect::Ask,
          "add_allow should not affect unmatched resource");
    std::cout << "[OK] permission add_allow\n";
    return true;
}

static bool perm_ask_callback_invoked() {
    haicode::PermissionGate gate;
    bool called = false;
    gate.set_ask_callback([&](const std::string& session_id,
                              const std::string& action,
                              const std::string& resource,
                              const nlohmann::json&) -> haicode::PermissionEffect {
        called = true;
        TEST_REQUIRE(session_id == "sess1", "callback session mismatch");
        TEST_REQUIRE(action   == "bash", "callback action mismatch");
        TEST_REQUIRE(resource == "/my/cmd", "callback resource mismatch");
        return haicode::PermissionEffect::Allow;
    });
    auto r = gate.check("sess1", "bash", "/my/cmd", nlohmann::json::object());
    CHECK(called, "ask callback should have been called");
    CHECK(r == haicode::PermissionEffect::Allow, "callback return value should propagate");
    std::cout << "[OK] permission ask callback invoked (session id forwarded)\n";
    return true;
}

static bool perm_ask_callback_not_invoked_when_rule_matches() {
    bool called = false;
    auto gate = make_gate({{"bash", "*", haicode::PermissionEffect::Allow}});
    gate.set_ask_callback([&](const std::string&, const std::string&,
                               const std::string&, const nlohmann::json&) -> haicode::PermissionEffect {
        called = true;
        return haicode::PermissionEffect::Deny;
    });
    auto r = gate.check("bash", "/any");
    CHECK(!called, "callback should NOT be invoked when a rule matches");
    CHECK(r == haicode::PermissionEffect::Allow, "rule result should be used");
    std::cout << "[OK] permission callback not invoked when rule matches\n";
    return true;
}

// ============================================================
// ToolRegistry::execute + gate integration
// ============================================================

static bool registry_read_inside_workdir_bypasses_gate() {
    haicode::ToolRegistry reg;
    haicode::register_builtin_tools(reg);

    // Gate denies everything — but read inside working_dir should bypass it
    haicode::PermissionGate gate;
    gate.set_rules({{"read", "*", haicode::PermissionEffect::Deny}});
    gate.set_ask_callback([](const std::string&, const std::string&,
                              const std::string&, const nlohmann::json&) {
        return haicode::PermissionEffect::Deny;
    });

    const std::string p = "/tmp/tfc_reg_bypass.txt";
    std::ofstream f(p); f << "hello\n"; f.close();

    haicode::ToolContext ctx;
    ctx.working_dir = "/tmp";
    auto r = reg.execute("read", {{"path", p}}, ctx, gate);
    CHECK(r.success,  "read inside working_dir should succeed despite deny rule");
    CHECK(!r.denied,  "denied flag should not be set");
    std::remove(p.c_str());
    std::cout << "[OK] registry read inside working_dir bypasses gate\n";
    return true;
}

static bool registry_write_denied_sets_flag() {
    haicode::ToolRegistry reg;
    haicode::register_builtin_tools(reg);

    haicode::PermissionGate gate;
    gate.set_rules({{"write", "*", haicode::PermissionEffect::Deny}});

    haicode::ToolContext ctx;
    ctx.working_dir = "/tmp";
    auto r = reg.execute("write",
                         {{"path", "/tmp/tfc_reg_denied.txt"}, {"content", "x"}},
                         ctx, gate);
    CHECK(!r.success, "denied write should not succeed");
    CHECK(r.denied,   "denied flag should be set");
    std::cout << "[OK] registry denied tool sets result.denied\n";
    return true;
}

static bool registry_unknown_tool() {
    haicode::ToolRegistry reg;
    haicode::PermissionGate gate;
    haicode::ToolContext ctx;
    ctx.working_dir = "/tmp";
    auto r = reg.execute("no_such_tool", {}, ctx, gate);
    CHECK(!r.success,                            "unknown tool should fail");
    CHECK(r.error.find("Unknown") != std::string::npos, "expected 'Unknown' in error");
    std::cout << "[OK] registry unknown tool returns error\n";
    return true;
}

// Gate that denies everything: deny rules plus a deny-callback fallback.
// glob/grep report the "read" action, so the read rule covers them.
static haicode::PermissionGate make_deny_all_gate() {
    haicode::PermissionGate gate;
    gate.set_rules({
        {"read", "*", haicode::PermissionEffect::Deny},
    });
    gate.set_ask_callback([](const std::string&, const std::string&,
                              const std::string&, const nlohmann::json&) {
        return haicode::PermissionEffect::Deny;
    });
    return gate;
}

// First existing file among candidates, or "" if none (skip the test).
static std::string first_existing(const std::vector<std::string>& candidates) {
    struct stat st;
    for (const auto& p : candidates)
        if (::stat(p.c_str(), &st) == 0) return p;
    return "";
}

static bool registry_read_system_headers_bypasses_gate() {
    std::string header = first_existing({
        "/boot/system/develop/headers/os/App.h",
        "/boot/system/develop/headers/curl/curl.h",
        "/boot/system/develop/headers/gnu/pthread.h",
        "/boot/system/documentation/BeBook/BWindow.html",
    });
    if (header.empty()) {
        std::cout << "[SKIP] registry read system headers (no haiku_devel)\n";
        return true;
    }

    haicode::ToolRegistry reg;
    haicode::register_builtin_tools(reg);
    auto gate = make_deny_all_gate();

    haicode::ToolContext ctx;
    ctx.working_dir = "/tmp";
    auto r = reg.execute("read", {{"path", header}}, ctx, gate);
    CHECK(r.success, "read of system header should bypass deny-all gate");
    CHECK(!r.denied, "denied flag should not be set for system header read");

    auto g = reg.execute("grep", {{"pattern", "include"}, {"path", header}},
                         ctx, gate);
    CHECK(g.success, "grep of system header should bypass deny-all gate");
    CHECK(!g.denied, "denied flag should not be set for system header grep");
    std::cout << "[OK] registry read/grep system headers bypass gate (" << header << ")\n";
    return true;
}

static bool registry_glob_system_headers_bypasses_gate() {
    std::string header = first_existing({
        "/boot/system/develop/headers/os/Interface2.h",
        "/boot/system/develop/headers/curl/curl.h",
        "/boot/system/develop/headers/gnu/pthread.h",
    });
    if (header.empty()) {
        std::cout << "[SKIP] registry glob system headers (no haiku_devel)\n";
        return true;
    }

    haicode::ToolRegistry reg;
    haicode::register_builtin_tools(reg);
    auto gate = make_deny_all_gate();

    haicode::ToolContext ctx;
    ctx.working_dir = "/tmp";
    auto r = reg.execute("glob", {{"pattern", header}}, ctx, gate);
    CHECK(r.success, "absolute glob under system headers should bypass deny-all gate");
    CHECK(!r.denied, "denied flag should not be set for system header glob");
    std::cout << "[OK] registry glob system headers bypasses gate\n";
    return true;
}

static bool registry_read_outside_still_denied() {
    // Hermetic negative control: the probe file lives outside both the
    // working dir and the always-readable roots, so it must stay gated.
    const std::string wd = "/tmp/tfc_reg_wd";
    const std::string outside = "/tmp/tfc_reg_outside.txt";
    ::mkdir(wd.c_str(), 0755);
    write_file(outside, "outside\n");

    haicode::ToolRegistry reg;
    haicode::register_builtin_tools(reg);
    auto gate = make_deny_all_gate();

    haicode::ToolContext ctx;
    ctx.working_dir = wd;
    auto r = reg.execute("read", {{"path", outside}}, ctx, gate);
    CHECK(!r.success, "read outside workdir and header roots should be denied");
    CHECK(r.denied,   "denied flag should be set for gated read");

    std::remove(outside.c_str());
    ::rmdir(wd.c_str());
    std::cout << "[OK] registry read outside roots still denied\n";
    return true;
}

static bool registry_read_everywhere_allows_glob_and_grep_outside_workdir() {
    // "Allow Read Everywhere" = one session rule {"read","*",Allow}. glob and
    // grep report the "read" action, so it must cover them outside the tree.
    // The ask callback denies, so a tool still reporting "glob"/"grep" fails.
    const std::string wd = "/tmp/tfc_re_wd";
    const std::string outside = "/tmp/tfc_re_outside.txt";
    ::mkdir(wd.c_str(), 0755);
    write_file(outside, "needle\n");

    haicode::ToolRegistry reg;
    haicode::register_builtin_tools(reg);

    haicode::PermissionGate gate;
    gate.set_session_rules({{"read", "*", haicode::PermissionEffect::Allow}});
    gate.set_ask_callback([](const std::string&, const std::string&,
                              const std::string&, const nlohmann::json&) {
        return haicode::PermissionEffect::Deny;
    });

    haicode::ToolContext ctx;
    ctx.working_dir = wd;
    auto g = reg.execute("glob", {{"pattern", outside}}, ctx, gate);
    CHECK(g.success, "read-everywhere should allow absolute glob outside workdir");
    CHECK(!g.denied, "denied flag should not be set for read-everywhere glob");

    auto s = reg.execute("grep", {{"pattern", "needle"}, {"path", outside}}, ctx, gate);
    CHECK(s.success, "read-everywhere should allow grep outside workdir");
    CHECK(!s.denied, "denied flag should not be set for read-everywhere grep");

    std::remove(outside.c_str());
    ::rmdir(wd.c_str());
    std::cout << "[OK] read-everywhere allows glob/grep outside workdir\n";
    return true;
}

// ============================================================
// Git invocation classifier
// ============================================================

static bool git_classifier_unit() {
    using haicode::git_invocation_is_readonly;
    auto ro = [](const char* sub, std::initializer_list<const char*> args) {
        std::vector<std::string> v;
        for (const char* a : args) v.emplace_back(a);
        return git_invocation_is_readonly(sub, v);
    };

    // Unconditionally read-only subcommands, bare and with flags.
    CHECK(ro("status", {}),           "bare git status is read-only");
    CHECK(ro("diff", {"--stat"}),     "git diff --stat is read-only");
    CHECK(ro("log", {"--oneline", "-3"}), "git log --oneline -3 is read-only");
    CHECK(ro("show", {"HEAD"}),       "git show HEAD is read-only");
    CHECK(ro("blame", {"file.cpp"}),  "git blame is read-only");
    CHECK(ro("ls-files", {}),         "git ls-files is read-only");
    CHECK(ro("shortlog", {}),         "git shortlog is read-only");
    CHECK(ro("describe", {"--tags"}), "git describe is read-only");
    CHECK(ro("rev-parse", {"--abbrev-ref", "HEAD"}), "git rev-parse is read-only");

    // --output makes even read-only subcommands write a file.
    CHECK(!ro("diff", {"--output", "/tmp/x"}),
          "git diff --output writes a file — must be gated");
    CHECK(!ro("log", {"--output=/tmp/x"}),
          "git log --output= writes a file — must be gated");

    // branch: listing forms only.
    CHECK(ro("branch", {}),           "bare git branch lists — read-only");
    CHECK(ro("branch", {"-l"}),       "git branch -l lists — read-only");
    CHECK(ro("branch", {"-a", "-v"}), "git branch -a -v lists — read-only");
    CHECK(ro("branch", {"--show-current"}), "git branch --show-current is read-only");
    CHECK(ro("branch", {"--contains=HEAD"}),
          "git branch --contains=HEAD is read-only");
    CHECK(!ro("branch", {"--contains", "HEAD"}),
          "space-separated flag value is indistinguishable from a positional "
          "(git branch foo creates) — fails closed");
    CHECK(!ro("branch", {"feature"}), "git branch <name> creates — gated");
    CHECK(!ro("branch", {"-D", "feature"}), "git branch -D deletes — gated");
    CHECK(!ro("branch", {"-d", "feature"}), "git branch -d deletes — gated");
    CHECK(!ro("branch", {"-m", "new"}), "git branch -m renames — gated");
    CHECK(!ro("branch", {"--frobnicate"}), "unknown branch flag fails closed");

    // stash: only list and show.
    CHECK(ro("stash", {"list"}),      "git stash list is read-only");
    CHECK(ro("stash", {"show"}),      "git stash show is read-only");
    CHECK(ro("stash", {"show", "-p"}), "git stash show -p is read-only");
    CHECK(!ro("stash", {}),           "bare git stash pushes — gated");
    CHECK(!ro("stash", {"pop"}),      "git stash pop mutates — gated");
    CHECK(!ro("stash", {"drop"}),     "git stash drop mutates — gated");
    CHECK(!ro("stash", {"clear"}),    "git stash clear wipes — gated");
    CHECK(!ro("stash", {"push"}),     "git stash push mutates — gated");

    // tag: listing forms only.
    CHECK(ro("tag", {}),              "bare git tag lists — read-only");
    CHECK(ro("tag", {"-l", "v*"}),    "git tag -l lists — read-only");
    CHECK(ro("tag", {"-n"}),          "git tag -n lists — read-only");
    CHECK(!ro("tag", {"v1.0"}),       "git tag <name> creates — gated");
    CHECK(!ro("tag", {"-d", "v1.0"}), "git tag -d deletes — gated");
    CHECK(!ro("tag", {"-a", "v1", "-m", "msg"}), "git tag -a creates — gated");

    // Everything else fails closed.
    CHECK(!ro("reset", {"--hard"}),   "git reset mutates — gated");
    CHECK(!ro("checkout", {"main"}),  "git checkout mutates — gated");
    CHECK(!ro("commit", {}),          "git commit mutates — gated");
    CHECK(!ro("push", {}),            "git push mutates — gated");
    CHECK(!ro("clean", {}),           "git clean mutates — gated");
    CHECK(!ro("", {}),                "empty subcommand fails closed");

    std::cout << "[OK] git_invocation_is_readonly unit cases\n";
    return true;
}

// Small self-contained git repo (pattern from test_git_find.cpp) with a
// feature branch that a wrongly-executed `git branch -D` would delete.
static std::string setup_git_repo_fixture() {
    const std::string root = "/tmp/tfc_git_gate_repo";
    system(("rm -rf " + root).c_str());
    ::mkdir(root.c_str(), 0755);
    std::ofstream(root + "/README.md") << "gate fixture\n";
    system(("cd " + root + " && "
            "git init -q -b main && "
            "git -c user.name=t -c user.email=t@t add . && "
            "git -c user.name=t -c user.email=t@t commit -q -m init && "
            "git branch feature").c_str());
    return root;
}

static bool registry_git_mutating_invocations_denied() {
    const std::string repo = setup_git_repo_fixture();

    haicode::ToolRegistry reg;
    haicode::register_builtin_tools(reg);
    auto gate = make_deny_all_gate();

    haicode::ToolContext ctx;
    ctx.working_dir = repo;

    // Bare `git branch` (listing) bypasses the deny-all gate.
    auto list = reg.execute("git", {{"subcommand", "branch"}}, ctx, gate);
    CHECK(list.success, "git branch listing should bypass deny-all gate");
    CHECK(!list.denied,  "git branch listing must not set denied flag");
    CHECK(list.output.find("feature") != std::string::npos,
          "branch listing should show the feature branch");

    // `git branch -D feature` is gated: denied, never executed.
    auto del = reg.execute("git", {{"subcommand", "branch"},
                                   {"args", {"-D", "feature"}}}, ctx, gate);
    CHECK(!del.success, "git branch -D must be denied under deny-all rules");
    CHECK(del.denied,   "git branch -D must set denied flag");

    // `git stash clear` is gated too.
    auto stash = reg.execute("git", {{"subcommand", "stash"},
                                     {"args", {"clear"}}}, ctx, gate);
    CHECK(!stash.success, "git stash clear must be denied under deny-all rules");
    CHECK(stash.denied,   "git stash clear must set denied flag");

    // The branch survived both denied calls — nothing executed.
    auto after = reg.execute("git", {{"subcommand", "branch"}}, ctx, gate);
    CHECK(after.output.find("feature") != std::string::npos,
          "feature branch must survive denied git branch -D");

    system(("rm -rf " + repo).c_str());
    std::cout << "[OK] registry denies mutating git invocations, allows listing\n";
    return true;
}

// ============================================================
// Symlink-aware containment
// ============================================================

// Deny-all gate + in-project symlink pointing outside the tree: the read
// must be gated (review #4 repro). An in-project symlink to an in-project
// file still bypasses (the trusted-tree contract is about the resolved
// target, not the link's location).
static bool registry_symlink_escape_denied() {
    const std::string wd = "/tmp/tfc_sym_wd";
    const std::string outside = "/tmp/tfc_sym_secret.txt";
    const std::string inside = wd + "/inside.txt";
    system(("rm -rf " + wd).c_str());
    ::mkdir(wd.c_str(), 0755);
    write_file(outside, "secret\n");
    write_file(inside, "public\n");

    // in-project symlink → outside file; in-project symlink → in-project file
    ::symlink(outside.c_str(), (wd + "/innocent.txt").c_str());
    ::symlink(inside.c_str(), (wd + "/alias.txt").c_str());

    haicode::ToolRegistry reg;
    haicode::register_builtin_tools(reg);
    auto gate = make_deny_all_gate();

    haicode::ToolContext ctx;
    ctx.working_dir = wd;

    auto esc = reg.execute("read", {{"path", wd + "/innocent.txt"}}, ctx, gate);
    CHECK(!esc.success, "read through symlink to outside file must be denied");
    CHECK(esc.denied,   "denied flag must be set for symlink escape read");

    auto okr = reg.execute("read", {{"path", wd + "/alias.txt"}}, ctx, gate);
    CHECK(okr.success, "read through in-project symlink to in-project file bypasses");
    CHECK(!okr.denied, "in-project symlink read must not set denied flag");
    CHECK(okr.output.find("public") != std::string::npos,
          "in-project symlink read should return the file contents");

    system(("rm -rf " + wd).c_str());
    std::remove(outside.c_str());
    std::cout << "[OK] symlink escape denied, in-project symlink still bypasses\n";
    return true;
}

// Symlinked directory inside the tree pointing outside: grep (path under the
// symlinked dir), absolute glob, and relative glob must all be gated; a
// read-everywhere session rule still allows them (positive control).
static bool registry_symlinked_dir_gates_glob_grep() {
    const std::string wd = "/tmp/tfc_symdir_wd";
    const std::string outside_dir = "/tmp/tfc_symdir_out";
    system(("rm -rf " + wd).c_str());
    system(("rm -rf " + outside_dir).c_str());
    ::mkdir(wd.c_str(), 0755);
    ::mkdir(outside_dir.c_str(), 0755);
    write_file(outside_dir + "/x.txt", "needle\n");
    ::symlink(outside_dir.c_str(), (wd + "/sub").c_str());

    haicode::ToolRegistry reg;
    haicode::register_builtin_tools(reg);

    haicode::ToolContext ctx;
    ctx.working_dir = wd;

    {
        auto gate = make_deny_all_gate();

        auto g = reg.execute("grep", {{"pattern", "needle"},
                                      {"path", wd + "/sub/x.txt"}}, ctx, gate);
        CHECK(!g.success, "grep through symlinked dir to outside must be denied");
        CHECK(g.denied,   "denied flag must be set for symlinked-dir grep");

        auto ab = reg.execute("glob", {{"pattern", wd + "/sub/*.txt"}}, ctx, gate);
        CHECK(!ab.success, "absolute glob through symlinked dir must be denied");
        CHECK(ab.denied,   "denied flag must be set for symlinked-dir absolute glob");

        auto rel = reg.execute("glob", {{"pattern", "sub/*.txt"}}, ctx, gate);
        CHECK(!rel.success, "relative glob through symlinked dir must be denied");
        CHECK(rel.denied,   "denied flag must be set for symlinked-dir relative glob");
    }

    {
        // Positive control: read-everywhere rule keeps them working.
        haicode::PermissionGate gate;
        gate.set_session_rules({{"read", "*", haicode::PermissionEffect::Allow}});
        gate.set_ask_callback([](const std::string&, const std::string&,
                                  const std::string&, const nlohmann::json&) {
            return haicode::PermissionEffect::Deny;
        });

        auto g = reg.execute("grep", {{"pattern", "needle"},
                                      {"path", wd + "/sub/x.txt"}}, ctx, gate);
        CHECK(g.success, "read-everywhere rule should allow symlinked-dir grep");

        auto rel = reg.execute("glob", {{"pattern", "sub/*.txt"}}, ctx, gate);
        CHECK(rel.success, "read-everywhere rule should allow symlinked-dir glob");
    }

    system(("rm -rf " + wd).c_str());
    system(("rm -rf " + outside_dir).c_str());
    std::cout << "[OK] symlinked dir gates grep/glob (absolute + relative)\n";
    return true;
}

// Broken symlink: nothing to leak — the read executes (not denied) and fails
// with a cannot-open error, keeping the not-gated vs failed distinction.
static bool registry_broken_symlink_not_denied() {
    const std::string wd = "/tmp/tfc_broken_wd";
    system(("rm -rf " + wd).c_str());
    ::mkdir(wd.c_str(), 0755);
    ::symlink("/tmp/tfc_broken_missing_target", (wd + "/dangling").c_str());

    haicode::ToolRegistry reg;
    haicode::register_builtin_tools(reg);
    auto gate = make_deny_all_gate();

    haicode::ToolContext ctx;
    ctx.working_dir = wd;

    auto r = reg.execute("read", {{"path", wd + "/dangling"}}, ctx, gate);
    CHECK(!r.success, "broken symlink read must fail");
    CHECK(!r.denied,  "broken symlink read executes (nothing to leak), not denied");
    CHECK(r.error.find("Cannot") != std::string::npos
            || r.error.find("open") != std::string::npos
            || r.error.find("symlink") != std::string::npos,
          "broken symlink error should mention the open failure: " + r.error);

    system(("rm -rf " + wd).c_str());
    std::cout << "[OK] broken symlink executes and fails without denied flag\n";
    return true;
}

// A symlink in the tree pointing INTO the always-readable system roots stays
// allowed (skip when the system files are absent).
static bool registry_symlink_into_system_roots_allowed() {
    std::string header = first_existing({
        "/boot/system/develop/headers/os/App.h",
        "/boot/system/develop/headers/curl/curl.h",
        "/boot/system/documentation/BeBook/BWindow.html",
    });
    if (header.empty()) {
        std::cout << "[SKIP] symlink into system roots (no haiku_devel)\n";
        return true;
    }

    const std::string wd = "/tmp/tfc_syslink_wd";
    system(("rm -rf " + wd).c_str());
    ::mkdir(wd.c_str(), 0755);
    ::symlink(header.c_str(), (wd + "/api_ref").c_str());

    haicode::ToolRegistry reg;
    haicode::register_builtin_tools(reg);
    auto gate = make_deny_all_gate();

    haicode::ToolContext ctx;
    ctx.working_dir = wd;

    auto r = reg.execute("read", {{"path", wd + "/api_ref"},
                                  {"limit", 3}}, ctx, gate);
    CHECK(r.success, "read through symlink into system roots should bypass");
    CHECK(!r.denied,  "symlink into system roots must not set denied flag");

    system(("rm -rf " + wd).c_str());
    std::cout << "[OK] symlink into system roots stays allowed\n";
    return true;
}

// ============================================================
// Per-session scoping
// ============================================================

static bool perm_session_rules_scoped() {
    haicode::PermissionGate gate;
    gate.set_session_rules("A", {{"bash", "*", haicode::PermissionEffect::Allow}});
    CHECK(gate.check("A", "bash", "/any", nlohmann::json::object())
              == haicode::PermissionEffect::Allow,
          "session A rules should apply to A");
    CHECK(gate.check("B", "bash", "/any", nlohmann::json::object())
              == haicode::PermissionEffect::Ask,
          "session A rules must not leak into B");
    std::cout << "[OK] permission session rules scoped per session\n";
    return true;
}

static bool perm_add_allow_scoped() {
    haicode::PermissionGate gate;
    gate.add_allow("A", "write", "/tmp/*");
    CHECK(gate.check("A", "write", "/tmp/foo", nlohmann::json::object())
              == haicode::PermissionEffect::Allow,
          "add_allow should grant the granting session");
    CHECK(gate.check("B", "write", "/tmp/foo", nlohmann::json::object())
              == haicode::PermissionEffect::Ask,
          "add_allow must not grant other sessions");
    std::cout << "[OK] permission add_allow scoped per session\n";
    return true;
}

static bool perm_session_rules_replaced_independently() {
    haicode::PermissionGate gate;
    gate.set_session_rules("A", {{"bash", "*", haicode::PermissionEffect::Allow}});
    gate.set_session_rules("B", {{"read", "*", haicode::PermissionEffect::Allow}});
    gate.set_session_rules("B", {});  // B's toggles changed — replacement
    CHECK(gate.check("A", "bash", "/any", nlohmann::json::object())
              == haicode::PermissionEffect::Allow,
          "replacing B's rules must leave A intact");
    CHECK(gate.check("B", "read", "/any", nlohmann::json::object())
              == haicode::PermissionEffect::Ask,
          "B's replaced rules should be gone");
    // add_allow grants survive a set_session_rules replacement (two layers)
    gate.add_allow("A", "write", "/tmp/*");
    gate.set_session_rules("A", {});
    CHECK(gate.check("A", "write", "/tmp/foo", nlohmann::json::object())
              == haicode::PermissionEffect::Allow,
          "Allow-Always grant must survive rule replacement");
    std::cout << "[OK] permission session rules replaced independently\n";
    return true;
}

static bool perm_check_thread_safety_smoke() {
    haicode::PermissionGate gate;
    gate.set_ask_callback([](const std::string&, const std::string&,
                              const std::string&, const nlohmann::json&) {
        return haicode::PermissionEffect::Ask;
    });
    bool stop = false;
    auto ok_all = true;
    std::thread writer([&]() {
        for (int i = 0; i < 20000 && !stop; ++i) {
            gate.set_session_rules("X",
                {{"bash", "*", haicode::PermissionEffect::Allow}});
            gate.set_session_rules("X", {});
        }
    });
    for (int i = 0; i < 20000; ++i) {
        auto r = gate.check("X", "bash", "/any", nlohmann::json::object());
        if (r != haicode::PermissionEffect::Allow
                && r != haicode::PermissionEffect::Ask)
            ok_all = false;
    }
    stop = true;
    writer.join();
    CHECK(ok_all, "concurrent check must only observe Allow/Ask, never crash");
    std::cout << "[OK] permission gate concurrent set/check smoke\n";
    return true;
}

// ============================================================
// Exact temporary grants + shared evaluation
// ============================================================

static bool perm_exact_grant_literal_wildcard() {
    haicode::PermissionGate gate;
    const nlohmann::json no_input = nlohmann::json::object();
    // Resource that itself contains fnmatch metacharacters: a glob grant
    // would over-authorize, an exact grant must not.
    gate.add_exact_allow("s1", "bash", "/tmp/naive*rm");
    CHECK(gate.check("s1", "bash", "/tmp/naive*rm", no_input)
              == haicode::PermissionEffect::Allow,
          "exact grant must match the literal resource");
    CHECK(gate.check("s1", "bash", "/tmp/naive-rm", no_input)
              == haicode::PermissionEffect::Ask,
          "exact grant must not glob-expand");
    CHECK(gate.check("s1", "bash", "/tmp/naiveXrm", no_input)
              == haicode::PermissionEffect::Ask,
          "exact grant must not match wildcard-shaped siblings");
    CHECK(gate.check("s2", "bash", "/tmp/naive*rm", no_input)
              == haicode::PermissionEffect::Ask,
          "exact grant must stay scoped to its session");

    // Dedup: adding the identical grant twice keeps one entry.
    gate.add_exact_allow("s1", "bash", "/tmp/naive*rm");
    auto snap = gate.snapshot("s1");
    CHECK(snap.temporary_exact.size() == 1, "identical exact grants deduplicate");

    // Revocation removes exactly the named pair.
    CHECK(gate.revoke_exact_allow("s1", "bash", "/tmp/naive*rm"),
          "revoking an existing exact grant reports success");
    CHECK(gate.check("s1", "bash", "/tmp/naive*rm", no_input)
              == haicode::PermissionEffect::Ask,
          "revoked exact grant no longer authorizes");
    CHECK(!gate.revoke_exact_allow("s1", "bash", "/tmp/naive*rm"),
          "revoking a missing grant reports failure");

    gate.add_exact_allow("s1", "bash", "/tmp/a");
    gate.add_allow("s1", "bash", "/tmp/b*");
    gate.revoke_temporary_allows("s1");
    CHECK(gate.check("s1", "bash", "/tmp/a", no_input)
              == haicode::PermissionEffect::Ask
       && gate.check("s1", "bash", "/tmp/bx", no_input)
              == haicode::PermissionEffect::Ask,
          "revoke_temporary_allows clears exact and pattern grants");
    std::cout << "[OK] exact temporary grants literal matching, dedup, revoke\n";
    return true;
}

static bool perm_evaluate_reports_source() {
    auto gate = make_gate({{"read", "/safe/*", haicode::PermissionEffect::Allow}});
    gate.set_session_rules("s1", {{"write", "*", haicode::PermissionEffect::Deny}});

    auto d = gate.evaluate("s1", "read", "/safe/file");
    CHECK(d.effect == haicode::PermissionEffect::Allow && d.source == "configuration"
          && d.rule_index == 0, "configured rule decision reports source+index");

    d = gate.evaluate("s1", "write", "/x");
    CHECK(d.effect == haicode::PermissionEffect::Deny && d.source == "session",
          "session toggle decision reports session source");

    d = gate.evaluate("s1", "bash", "/x");
    CHECK(d.effect == haicode::PermissionEffect::Ask && d.source == "prompt"
          && !d.blocked, "unmatched request reports prompt source, not blocked");

    gate.add_allow("s1", "bash", "/cmd");
    d = gate.evaluate("s1", "bash", "/cmd");
    CHECK(d.effect == haicode::PermissionEffect::Allow
          && d.source == "temporary_pattern",
          "legacy allow-always grant reports temporary_pattern source");

    // Session layers override configured denies (existing precedence kept).
    gate.set_session_rules("s1", {{"bash", "*", haicode::PermissionEffect::Allow}});
    gate.set_rules({{"bash", "*", haicode::PermissionEffect::Deny}});
    d = gate.evaluate("s1", "bash", "/x");
    CHECK(d.effect == haicode::PermissionEffect::Allow,
          "session layer still overrides configured deny");
    std::cout << "[OK] evaluate reports effect, source, and rule identity\n";
    return true;
}

static bool registry_inspect_matches_execution() {
    haicode::ToolRegistry reg;
    haicode::register_builtin_tools(reg);
    haicode::PermissionGate gate;
    gate.set_rules({{"write", "*", haicode::PermissionEffect::Deny},
                    {"write", "/tmp/tfc_inspect_ok.txt",
                     haicode::PermissionEffect::Allow}});
    gate.set_ask_callback([](const std::string&, const std::string&,
                              const std::string&, const nlohmann::json&) {
        return haicode::PermissionEffect::Deny;
    });

    haicode::ToolContext ctx;
    ctx.session_id = "s1";
    ctx.working_dir = "/tmp";

    const std::string allowed_path = "/tmp/tfc_inspect_ok.txt";
    const std::string denied_path  = "/tmp/tfc_inspect_no.txt";
    nlohmann::json in = {{"path", ""}, {"content", "x"}};

    in["path"] = allowed_path;
    auto d = reg.evaluate("write", in, ctx, gate);
    CHECK(d.effect == haicode::PermissionEffect::Allow && !d.blocked,
          "inspector: allowed write");
    std::remove(allowed_path.c_str());

    in["path"] = denied_path;
    d = reg.evaluate("write", in, ctx, gate);
    CHECK(d.effect == haicode::PermissionEffect::Deny && !d.blocked,
          "inspector: denied write");

    // Execution must agree with inspection...
    auto r = reg.execute("write", in, ctx, gate);
    CHECK(r.denied && !r.success, "execution agrees: denied");
    struct stat st;
    CHECK(::stat(denied_path.c_str(), &st) != 0,
          "inspector must not execute the tool (no file created)");

    // ...and for the allowed case too.
    in["path"] = allowed_path;
    r = reg.execute("write", in, ctx, gate);
    CHECK(r.success && !r.denied, "execution agrees: allowed");
    std::remove(allowed_path.c_str());

    // Builtin exemption + mode block agreement.
    d = reg.evaluate("read", {{"path", "/tmp"}}, ctx, gate);
    CHECK(d.effect == haicode::PermissionEffect::Allow && d.source == "builtin",
          "inspector reports builtin read exemption");
    ctx.mode = haicode::SessionMode::Chat;
    d = reg.evaluate("read", {{"path", "/tmp"}}, ctx, gate);
    CHECK(d.blocked && d.source == "mode",
          "inspector reports mode block with blocked flag");
    std::cout << "[OK] ToolRegistry::evaluate agrees with execute, no side effects\n";
    return true;
}

// ============================================================
// Source-aware permission policy documents
// ============================================================

static bool policy_load_missing_and_present() {
    const std::string missing = "/tmp/tfc_policy_missing.json";
    std::remove(missing.c_str());
    auto doc = haicode::load_permission_document(missing);
    CHECK(!doc.exists && doc.rules.empty(), "missing policy file: exists=false, no rules");
    CHECK(doc.path == missing, "document carries its path");

    const std::string p = "/tmp/tfc_policy_present.json";
    write_file(p, R"({
        "model": "claude-opus-4",
        "permissions": [
            {"action": "bash", "resource": "/tmp/*", "effect": "allow"},
            {"action": "write", "resource": "*"}
        ]
    })");
    doc = haicode::load_permission_document(p);
    CHECK(doc.exists, "present policy file: exists=true");
    CHECK(doc.rules.size() == 2, "policy rules parsed");
    CHECK(doc.rules[0].action == "bash"
       && doc.rules[0].effect == haicode::PermissionEffect::Allow,
          "policy rule content parsed");
    CHECK(doc.rules[1].effect == haicode::PermissionEffect::Ask,
          "missing effect defaults to ask");
    CHECK(!doc.fingerprint.empty(), "fingerprint produced");
    std::remove(p.c_str());
    std::cout << "[OK] load_permission_document missing + present files\n";
    return true;
}

static bool policy_save_preserves_unrelated_keys() {
    const std::string p = "/tmp/tfc_policy_save.json";
    write_file(p, R"({
        "model": "claude-opus-4",
        "unknown_future_key": {"nested": [1, 2, 3]},
        "permissions": [{"action": "bash", "resource": "*", "effect": "deny"}]
    })");
    auto doc = haicode::load_permission_document(p);

    std::vector<haicode::PermissionRule> next = {
        {"write", "/boot/home/*", haicode::PermissionEffect::Allow},
        {"read", "*", haicode::PermissionEffect::Ask},
    };
    std::string err;
    CHECK(haicode::save_permission_document(p, next, doc.fingerprint, err),
          "policy save succeeds: " + err);

    // Only permissions replaced; unrelated keys intact.
    std::ifstream f(p);
    std::stringstream ss; ss << f.rdbuf();
    auto j = nlohmann::json::parse(ss.str(), nullptr, false);
    CHECK(!j.is_discarded() && j.contains("model")
       && j["model"] == "claude-opus-4", "unrelated scalar key preserved");
    CHECK(j.contains("unknown_future_key"), "unknown key preserved");
    CHECK(j.contains("permissions") && j["permissions"].size() == 2,
          "permissions array replaced");
    CHECK(j["permissions"][0]["resource"] == "/boot/home/*", "new rule[0] written");

    auto reloaded = haicode::load_permission_document(p);
    CHECK(reloaded.rules.size() == 2 && reloaded.rules[0].action == "write",
          "saved policy reloads identically");
    std::remove(p.c_str());
    std::cout << "[OK] save_permission_document replaces only permissions\n";
    return true;
}

static bool policy_save_empty_removes_key_and_creates_file() {
    // Existing file with rules: empty list removes the key entirely.
    const std::string p = "/tmp/tfc_policy_empty.json";
    write_file(p, R"({"permissions": [{"action": "bash", "resource": "*", "effect": "allow"}],
                      "provider": "anthropic"})");
    auto doc = haicode::load_permission_document(p);
    std::string err;
    CHECK(haicode::save_permission_document(p, {}, doc.fingerprint, err),
          "clearing rules succeeds: " + err);
    std::ifstream f(p);
    std::stringstream ss; ss << f.rdbuf();
    auto j = nlohmann::json::parse(ss.str(), nullptr, false);
    CHECK(!j.contains("permissions"), "empty rule list removes permissions key");
    CHECK(j.contains("provider"), "provider key survives clearing");

    // New file (would-be-created source): starts from empty fingerprint.
    std::remove(p.c_str());
    const std::string fresh_dir = "/tmp/tfc_policy_newdir";
    const std::string fresh_path = fresh_dir + "/config.json";
    auto fresh_doc = haicode::load_permission_document(fresh_path);
    CHECK(!fresh_doc.exists && fresh_doc.fingerprint == "[]",
          "fresh source fingerprint is the empty list");
    CHECK(haicode::save_permission_document(
              fresh_path,
              {{"bash", "*", haicode::PermissionEffect::Deny}},
              fresh_doc.fingerprint, err),
          "save creates the missing file: " + err);
    auto created = haicode::load_permission_document(fresh_path);
    CHECK(created.exists && created.rules.size() == 1
       && created.rules[0].effect == haicode::PermissionEffect::Deny,
          "created file parses back with the saved rule");
    std::remove(fresh_path.c_str());
    std::remove(fresh_dir.c_str());
    std::remove("/tmp/tfc_policy_newdir");
    std::cout << "[OK] policy save removes empty key, creates missing file\n";
    return true;
}

static bool policy_save_conflict_and_validation() {
    const std::string p = "/tmp/tfc_policy_conflict.json";
    write_file(p, R"({"permissions": [{"action": "bash", "resource": "*", "effect": "deny"}]})");
    auto doc = haicode::load_permission_document(p);

    // Concurrent edit after load: stale fingerprint must fail without writing.
    write_file(p, R"({"permissions": [{"action": "bash", "resource": "*", "effect": "ask"}]})");
    std::string err;
    CHECK(!haicode::save_permission_document(
              p, {{"write", "*", haicode::PermissionEffect::Allow}},
              doc.fingerprint, err),
          "conflicting edit rejected");
    CHECK(err.find("changed since") != std::string::npos,
          "conflict error explains the cause");

    // Reloaded fingerprint matches the new content → save proceeds.
    auto reloaded = haicode::load_permission_document(p);
    CHECK(haicode::save_permission_document(
              p, {{"write", "*", haicode::PermissionEffect::Allow}},
              reloaded.fingerprint, err),
          "fresh fingerprint saves: " + err);

    // Cosmetic reformatting of the same rules is NOT a conflict: the file now
    // holds write/allow, so compare against the fingerprint of that content.
    auto saved = haicode::load_permission_document(p);
    write_file(p, "{\"permissions\":[{\"resource\":\"*\",\"action\":\"write\","
                  "\"effect\":\"allow\"}]}");
    CHECK(haicode::save_permission_document(
              p, {{"read", "/tmp", haicode::PermissionEffect::Deny}},
              saved.fingerprint, err),
          "cosmetic reformat is not a conflict: " + err);

    // Malformed JSON document refuses to save rather than clobbering.
    write_file(p, "{ this is not json");
    CHECK(!haicode::save_permission_document(
              p, {{"read", "*", haicode::PermissionEffect::Deny}},
              reloaded.fingerprint, err),
          "malformed document rejected");
    CHECK(err.find("valid JSON") != std::string::npos,
          "malformed error explains the cause");
    std::remove(p.c_str());
    std::cout << "[OK] policy save conflict detection + validation\n";
    return true;
}

// ============================================================

int main() {
    std::cout << "=== Config + PermissionGate Tests ===\n\n";

    bool ok = true;

    std::cout << "-- load_file --\n";
    ok &= cfg_basic_fields();
    ok &= cfg_permissions();
    ok &= cfg_permission_default_resource();
    ok &= cfg_build_command();
    ok &= cfg_web_search();
    ok &= cfg_instructions();
    ok &= cfg_providers();
    ok &= cfg_missing_file();
    ok &= cfg_invalid_json();
    ok &= cfg_model_contexts();

    std::cout << "\n-- merge --\n";
    ok &= merge_scalar_overlay();
    ok &= merge_empty_overlay_does_not_clear();
    ok &= merge_permissions_appended();
    ok &= merge_instructions_appended();
    ok &= merge_build_command_overlay();
    ok &= merge_build_command_base_preserved();
    ok &= merge_providers_merged();
    ok &= merge_providers_per_subkey();
    ok &= merge_web_search_overlay();
    ok &= merge_web_search_default_overlay_preserves_base();
    ok &= merge_absent_project_keys_preserve_global();
    ok &= merge_explicit_default_value_still_overrides();

    std::cout << "\n-- PermissionGate --\n";
    ok &= perm_allow_rule();
    ok &= perm_deny_rule();
    ok &= perm_no_match_returns_ask();
    ok &= perm_wildcard_action();
    ok &= perm_fnmatch_resource();
    ok &= perm_last_rule_wins();
    ok &= perm_session_rules_override_config();
    ok &= perm_add_allow();
    ok &= perm_ask_callback_invoked();
    ok &= perm_ask_callback_not_invoked_when_rule_matches();

    std::cout << "\n-- PermissionGate session scoping --\n";
    ok &= perm_session_rules_scoped();
    ok &= perm_add_allow_scoped();
    ok &= perm_session_rules_replaced_independently();
    ok &= perm_check_thread_safety_smoke();

    std::cout << "\n-- PermissionGate exact grants + evaluation --\n";
    ok &= perm_exact_grant_literal_wildcard();
    ok &= perm_evaluate_reports_source();
    ok &= registry_inspect_matches_execution();

    std::cout << "\n-- Source-aware policy documents --\n";
    ok &= policy_load_missing_and_present();
    ok &= policy_save_preserves_unrelated_keys();
    ok &= policy_save_empty_removes_key_and_creates_file();
    ok &= policy_save_conflict_and_validation();

    std::cout << "\n-- ToolRegistry + gate integration --\n";
    ok &= registry_read_inside_workdir_bypasses_gate();
    ok &= registry_read_system_headers_bypasses_gate();
    ok &= registry_glob_system_headers_bypasses_gate();
    ok &= registry_read_outside_still_denied();
    ok &= registry_read_everywhere_allows_glob_and_grep_outside_workdir();
    ok &= registry_write_denied_sets_flag();
    ok &= registry_unknown_tool();

    std::cout << "\n-- git invocation classifier --\n";
    ok &= git_classifier_unit();
    ok &= registry_git_mutating_invocations_denied();

    std::cout << "\n-- symlink containment --\n";
    ok &= registry_symlink_escape_denied();
    ok &= registry_symlinked_dir_gates_glob_grep();
    ok &= registry_broken_symlink_not_denied();
    ok &= registry_symlink_into_system_roots_allowed();

    if (ok) {
        std::cout << "\nAll config + permission tests passed!\n";
        return 0;
    } else {
        std::cerr << "\nSome tests FAILED.\n";
        return 1;
    }
}
