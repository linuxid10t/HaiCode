#include <haicode/config.h>
#include <haicode/tool.h>
#include <haicode/haicode.h>
#include <iostream>
#include <fstream>
#include <cassert>
#include <cstdio>
#include <string>

// ---- Helpers ----

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    assert(f.is_open());
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
    write_file(p, R"({"web_search":{"engine":"ddg_lite","max_results":8}})");
    haicode::ConfigLoader loader;
    auto cfg = loader.load_file(p);
    CHECK(cfg.web_search_engine      == "ddg_lite", "web_search engine mismatch");
    CHECK(cfg.web_search_max_results == 8,           "web_search max_results mismatch");
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
// ConfigLoader::merge
// ============================================================

static bool merge_scalar_overlay() {
    haicode::AppConfig base, overlay;
    base.model    = "base-model";
    base.provider = "base-provider";
    overlay.model = "overlay-model";
    haicode::ConfigLoader loader_; auto result = loader_.merge(base, overlay);
    CHECK(result.model    == "overlay-model",   "overlay model should win");
    CHECK(result.provider == "base-provider",   "base provider should survive");
    std::cout << "[OK] merge scalar overlay wins\n";
    return true;
}

static bool merge_empty_overlay_does_not_clear() {
    haicode::AppConfig base, overlay;
    base.model    = "keep-me";
    base.provider = "keep-me-too";
    // overlay has empty strings — should not clear base values
    haicode::ConfigLoader loader_; auto result = loader_.merge(base, overlay);
    CHECK(result.model    == "keep-me",     "empty overlay should not clear model");
    CHECK(result.provider == "keep-me-too", "empty overlay should not clear provider");
    std::cout << "[OK] merge empty overlay preserves base\n";
    return true;
}

static bool merge_permissions_appended() {
    haicode::AppConfig base, overlay;
    base.permissions.push_back({"bash", "*", haicode::PermissionEffect::Deny});
    overlay.permissions.push_back({"write", "/tmp/*", haicode::PermissionEffect::Allow});
    haicode::ConfigLoader loader_; auto result = loader_.merge(base, overlay);
    CHECK(result.permissions.size() == 2, "expected 2 permissions after merge");
    CHECK(result.permissions[0].action == "bash",  "base permission should be first");
    CHECK(result.permissions[1].action == "write", "overlay permission should be appended");
    std::cout << "[OK] merge permissions appended\n";
    return true;
}

static bool merge_instructions_appended() {
    haicode::AppConfig base, overlay;
    base.instructions    = {"base instruction"};
    overlay.instructions = {"overlay instruction"};
    haicode::ConfigLoader loader_; auto result = loader_.merge(base, overlay);
    CHECK(result.instructions.size() == 2,                     "expected 2 instructions");
    CHECK(result.instructions[0] == "base instruction",    "base first");
    CHECK(result.instructions[1] == "overlay instruction", "overlay appended");
    std::cout << "[OK] merge instructions appended\n";
    return true;
}

static bool merge_build_command_overlay() {
    haicode::AppConfig base, overlay;
    base.build_command    = "make base";
    overlay.build_command = "make overlay";
    haicode::ConfigLoader loader_; auto result = loader_.merge(base, overlay);
    CHECK(result.build_command == "make overlay", "overlay build_command should win");
    std::cout << "[OK] merge build_command overlay wins\n";
    return true;
}

static bool merge_build_command_base_preserved() {
    haicode::AppConfig base, overlay;
    base.build_command = "make base";
    // overlay has no build_command
    haicode::ConfigLoader loader_; auto result = loader_.merge(base, overlay);
    CHECK(result.build_command == "make base", "base build_command should be preserved");
    std::cout << "[OK] merge build_command base preserved when overlay empty\n";
    return true;
}

static bool merge_providers_merged() {
    haicode::AppConfig base, overlay;
    base.providers["anthropic"]  = {"anthropic", "key-a", ""};
    overlay.providers["openai"]  = {"openai",    "key-b", ""};
    haicode::ConfigLoader loader_; auto result = loader_.merge(base, overlay);
    CHECK(result.providers.count("anthropic") == 1, "anthropic should survive");
    CHECK(result.providers.count("openai")    == 1, "openai should be added");
    std::cout << "[OK] merge providers merged\n";
    return true;
}

static bool merge_web_search_overlay() {
    haicode::AppConfig base, overlay;
    base.web_search_engine      = "mojeek";
    base.web_search_max_results = 5;
    overlay.web_search_engine   = "ddg_lite";
    overlay.web_search_max_results = 10;
    haicode::ConfigLoader loader_; auto result = loader_.merge(base, overlay);
    CHECK(result.web_search_engine      == "ddg_lite", "engine overlay should win");
    CHECK(result.web_search_max_results == 10,          "max_results overlay should win");
    std::cout << "[OK] merge web_search overlay wins\n";
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
    gate.set_ask_callback([&](const std::string& action, const std::string& resource,
                               const nlohmann::json&) -> haicode::PermissionEffect {
        called = true;
        assert(action   == "bash"    && "callback action mismatch");
        assert(resource == "/my/cmd" && "callback resource mismatch");
        return haicode::PermissionEffect::Allow;
    });
    auto r = gate.check("bash", "/my/cmd");
    CHECK(called, "ask callback should have been called");
    CHECK(r == haicode::PermissionEffect::Allow, "callback return value should propagate");
    std::cout << "[OK] permission ask callback invoked\n";
    return true;
}

static bool perm_ask_callback_not_invoked_when_rule_matches() {
    bool called = false;
    auto gate = make_gate({{"bash", "*", haicode::PermissionEffect::Allow}});
    gate.set_ask_callback([&](const std::string&, const std::string&,
                               const nlohmann::json&) -> haicode::PermissionEffect {
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
                              const nlohmann::json&) {
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
    ok &= merge_web_search_overlay();

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

    std::cout << "\n-- ToolRegistry + gate integration --\n";
    ok &= registry_read_inside_workdir_bypasses_gate();
    ok &= registry_write_denied_sets_flag();
    ok &= registry_unknown_tool();

    if (ok) {
        std::cout << "\nAll config + permission tests passed!\n";
        return 0;
    } else {
        std::cerr << "\nSome tests FAILED.\n";
        return 1;
    }
}
