// write_agents_md: target selection mirrors the instructions loader (a
// populated claude.md is updated in place, never shadowed by a new
// agents.md), case variants, empty-content rejection, resource()/execute()
// agreement, Build-only mode gating. external_terminal: contract checks that
// never open a Terminal — permission action/resource, missing-command
// rejection, gate denial before execution, Plan/Chat mode refusal.
#include <haicode/haicode.h>
#include <haicode/tool.h>
#include <haicode/config.h>
#include "test_check.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace haicode;
using nlohmann::json;

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << (msg) << "\n"; return false; } } while(0)

static ToolRegistry& reg() {
    static ToolRegistry r;
    static bool done = false;
    if (!done) { register_builtin_tools(r); done = true; }
    return r;
}

static std::string make_dir() {
    char tmpl[] = "/tmp/tpt_XXXXXX";
    TEST_REQUIRE(mkdtemp(tmpl), "mkdtemp failed");
    return tmpl;
}

static void remove_dir(const std::string& d) {
    std::system(("rm -rf '" + d + "'").c_str());
}

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    TEST_REQUIRE(f.is_open(), "failed to open " + path);
    f << content;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool exists(const std::string& path) {
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0;
}

static ToolContext ctx(const std::string& dir) {
    ToolContext c;
    c.working_dir = dir;
    return c;
}

static ToolResult write_agents(const std::string& dir, const std::string& content) {
    return reg().get("write_agents_md")->execute({{"content", content}}, ctx(dir));
}

// What the next session's system prompt would carry.
static std::string loaded_instructions(const std::string& dir) {
    return load_with_layers(ConfigLayer{}, dir).agents_md;
}

// ============================================================
// write_agents_md
// ============================================================

static bool agents_creates_when_none_exist() {
    std::string d = make_dir();
    auto r = write_agents(d, "# Project\nnew\n");
    CHECK(r.success, "write failed: " + r.error);
    CHECK(read_file(d + "/agents.md") == "# Project\nnew\n", "agents.md content wrong");
    auto out = json::parse(r.output);
    CHECK(out["path"] == d + "/agents.md", "path should be the new agents.md");
    CHECK(out["note"].get<std::string>().rfind("agents.md written", 0) == 0,
          "note should name agents.md");
    CHECK(loaded_instructions(d) == "# Project\nnew\n", "loader should read it");
    remove_dir(d);
    std::cout << "[OK] write_agents_md creates agents.md when no instructions file exists\n";
    return true;
}

static bool agents_updates_populated_claude_md() {
    // The shadowing bug: a new agents.md beside a hand-maintained CLAUDE.md
    // made the loader stop reading CLAUDE.md.
    std::string d = make_dir();
    write_file(d + "/CLAUDE.md", "# Hand-maintained\n");
    auto r = write_agents(d, "# Updated\n");
    CHECK(r.success, "write failed: " + r.error);
    CHECK(!exists(d + "/agents.md"), "must not create agents.md beside CLAUDE.md");
    CHECK(read_file(d + "/CLAUDE.md") == "# Updated\n", "CLAUDE.md should be updated in place");
    auto out = json::parse(r.output);
    CHECK(out["path"] == d + "/CLAUDE.md", "path should be CLAUDE.md");
    CHECK(out["note"].get<std::string>().rfind("CLAUDE.md written", 0) == 0,
          "note should name CLAUDE.md, got: " + out["note"].get<std::string>());
    CHECK(loaded_instructions(d) == "# Updated\n", "loader should read the new content");
    remove_dir(d);
    std::cout << "[OK] write_agents_md updates a populated CLAUDE.md in place\n";
    return true;
}

static bool agents_updates_case_variant() {
    std::string d = make_dir();
    write_file(d + "/AGENTS.md", "old\n");
    auto r = write_agents(d, "new\n");
    CHECK(r.success, "write failed: " + r.error);
    CHECK(read_file(d + "/AGENTS.md") == "new\n", "AGENTS.md should be updated");
    CHECK(!exists(d + "/agents.md"), "must not create a lowercase sibling");
    remove_dir(d);
    std::cout << "[OK] write_agents_md updates an AGENTS.md case variant\n";
    return true;
}

static bool agents_prefers_agents_when_both_populated() {
    std::string d = make_dir();
    write_file(d + "/agents.md", "agents\n");
    write_file(d + "/claude.md", "claude\n");
    auto r = write_agents(d, "updated\n");
    CHECK(r.success, "write failed: " + r.error);
    CHECK(read_file(d + "/agents.md") == "updated\n", "agents.md should be written");
    CHECK(read_file(d + "/claude.md") == "claude\n", "claude.md must be untouched");
    CHECK(loaded_instructions(d) == "updated\n", "loader reads agents.md");
    remove_dir(d);
    std::cout << "[OK] write_agents_md writes agents.md when both have content\n";
    return true;
}

static bool agents_empty_agents_falls_through_to_claude() {
    // The loader skips an empty agents.md, so claude.md is the file in use.
    std::string d = make_dir();
    write_file(d + "/agents.md", "");
    write_file(d + "/claude.md", "claude\n");
    auto r = write_agents(d, "updated\n");
    CHECK(r.success, "write failed: " + r.error);
    CHECK(read_file(d + "/claude.md") == "updated\n", "claude.md should be written");
    CHECK(read_file(d + "/agents.md").empty(), "empty agents.md must stay empty");
    CHECK(loaded_instructions(d) == "updated\n", "loader should read the new content");
    remove_dir(d);
    std::cout << "[OK] write_agents_md follows the loader past an empty agents.md\n";
    return true;
}

static bool agents_empty_files_prefer_agents() {
    std::string d = make_dir();
    write_file(d + "/agents.md", "");
    write_file(d + "/claude.md", "");
    CHECK(project_instructions_write_target(d) == d + "/agents.md",
          "both empty → agents.md");
    remove_dir(d);
    d = make_dir();
    write_file(d + "/claude.md", "");
    CHECK(project_instructions_write_target(d) == d + "/claude.md",
          "only an empty claude.md → claude.md (no new agents.md)");
    remove_dir(d);
    std::cout << "[OK] write target with empty instructions files\n";
    return true;
}

static bool agents_empty_content_rejected() {
    std::string d = make_dir();
    write_file(d + "/CLAUDE.md", "keep\n");
    auto r = write_agents(d, "");
    CHECK(!r.success, "empty content must be rejected");
    CHECK(read_file(d + "/CLAUDE.md") == "keep\n", "existing file must be untouched");
    CHECK(!exists(d + "/agents.md"), "nothing may be created");
    remove_dir(d);
    std::cout << "[OK] write_agents_md rejects empty content without touching files\n";
    return true;
}

static bool agents_resource_matches_execute() {
    std::string d = make_dir();
    write_file(d + "/CLAUDE.md", "x\n");
    auto t = reg().get("write_agents_md");
    CHECK(t->required_permission() == "write", "permission action should be write");
    std::string res = t->resource({{"content", "y"}}, ctx(d + "/"));
    CHECK(res == d + "/CLAUDE.md", "resource should name CLAUDE.md, got: " + res);
    auto r = t->execute({{"content", "y\n"}}, ctx(d + "/"));
    CHECK(r.success, "write failed: " + r.error);
    CHECK(json::parse(r.output)["path"] == res, "execute must write the resource path");
    remove_dir(d);
    std::cout << "[OK] write_agents_md resource() names the file execute() writes\n";
    return true;
}

static bool agents_gated_and_build_only() {
    std::string d = make_dir();
    PermissionGate gate;  // no rules, no callback: unresolved Ask
    auto r = reg().execute("write_agents_md", {{"content", "x\n"}}, ctx(d), gate);
    CHECK(!r.success && r.denied, "write_agents_md must go through the gate");
    CHECK(!exists(d + "/agents.md"), "denied call must not write");

    PermissionGate allow;
    allow.set_rules({{"*", "*", PermissionEffect::Allow}});
    for (SessionMode m : {SessionMode::Plan, SessionMode::Chat}) {
        ToolContext c = ctx(d);
        c.mode = m;
        auto p = reg().execute("write_agents_md", {{"content", "x\n"}}, c, allow);
        CHECK(!p.success, "write_agents_md must not run outside Build mode");
        CHECK(p.error.find("[mode restriction]") != std::string::npos,
              "expected a mode restriction, got: " + p.error);
    }
    CHECK(!exists(d + "/agents.md"), "mode-restricted calls must not write");
    remove_dir(d);
    std::cout << "[OK] write_agents_md is gated and Build-only\n";
    return true;
}

// ============================================================
// external_terminal (never launches a Terminal)
// ============================================================

static bool terminal_permission_contract() {
    auto t = reg().get("external_terminal");
    CHECK(t, "external_terminal not registered");
    CHECK(t->required_permission() == "external_terminal",
          "external_terminal must use its own permission action");
    CHECK(t->resource({{"command", "vim notes.txt"}}, ctx("/tmp")) == "vim notes.txt",
          "resource should be the command");
    std::cout << "[OK] external_terminal permission action + resource\n";
    return true;
}

static bool terminal_missing_command_rejected() {
    auto t = reg().get("external_terminal");
    auto r = t->execute(json::object(), ctx("/tmp"));
    CHECK(!r.success, "missing command must fail");
    CHECK(r.error.find("command") != std::string::npos,
          "error should name the missing field, got: " + r.error);
    r = t->execute({{"command", ""}}, ctx("/tmp"));
    CHECK(!r.success, "empty command must fail");
    std::cout << "[OK] external_terminal rejects a missing/empty command\n";
    return true;
}

static bool terminal_gated_never_exempt() {
    // No built-in exemption: with no rule and no callback the call is denied
    // before execute() could fork anything. A deny callback also denies.
    PermissionGate gate;
    auto r = reg().execute("external_terminal", {{"command", "true"}}, ctx("/tmp"), gate);
    CHECK(!r.success && r.denied, "unresolved Ask must deny external_terminal");

    PermissionGate deny;
    deny.set_ask_callback([](const std::string&, const std::string& action,
                             const std::string&, const json&) {
        TEST_REQUIRE(action == "external_terminal", "ask should name external_terminal");
        return PermissionEffect::Deny;
    });
    r = reg().execute("external_terminal", {{"command", "true"}}, ctx("/tmp"), deny);
    CHECK(!r.success && r.denied, "deny callback must deny external_terminal");

    // A bash Allow rule doesn't cover it — distinct action.
    PermissionGate bash_only;
    bash_only.set_rules({{"bash", "*", PermissionEffect::Allow}});
    auto ev = reg().evaluate("external_terminal", {{"command", "true"}}, ctx("/tmp"), bash_only);
    CHECK(ev.effect != PermissionEffect::Allow, "bash Allow must not authorize external_terminal");
    std::cout << "[OK] external_terminal always goes through the gate\n";
    return true;
}

static bool terminal_build_only() {
    PermissionGate allow;
    allow.set_rules({{"*", "*", PermissionEffect::Allow}});
    for (SessionMode m : {SessionMode::Plan, SessionMode::Chat}) {
        ToolContext c = ctx("/tmp");
        c.mode = m;
        auto ev = reg().evaluate("external_terminal", {{"command", "true"}}, c, allow);
        CHECK(ev.effect == PermissionEffect::Deny && ev.source == "mode",
              "external_terminal must be mode-blocked outside Build");
        auto r = reg().execute("external_terminal", {{"command", "true"}}, c, allow);
        CHECK(!r.success && r.error.find("[mode restriction]") != std::string::npos,
              "expected a mode restriction, got: " + r.error);
    }
    std::cout << "[OK] external_terminal is refused in Plan and Chat mode\n";
    return true;
}

int main() {
    std::cout << "=== write_agents_md / external_terminal Tests ===\n\n";
    bool ok = true;
    std::cout << "-- write_agents_md --\n";
    ok &= agents_creates_when_none_exist();
    ok &= agents_updates_populated_claude_md();
    ok &= agents_updates_case_variant();
    ok &= agents_prefers_agents_when_both_populated();
    ok &= agents_empty_agents_falls_through_to_claude();
    ok &= agents_empty_files_prefer_agents();
    ok &= agents_empty_content_rejected();
    ok &= agents_resource_matches_execute();
    ok &= agents_gated_and_build_only();
    std::cout << "\n-- external_terminal --\n";
    ok &= terminal_permission_contract();
    ok &= terminal_missing_command_rejected();
    ok &= terminal_gated_never_exempt();
    ok &= terminal_build_only();
    if (ok) {
        std::cout << "\nAll project tool tests passed!\n";
        return 0;
    }
    std::cerr << "\nSome tests FAILED.\n";
    return 1;
}
