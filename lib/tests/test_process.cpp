#include <haicode/haicode.h>
#include <haicode/tool.h>
#include <iostream>
#include <cassert>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <csignal>

static haicode::ToolContext make_ctx() {
    haicode::ToolContext ctx;
    ctx.working_dir = "/tmp";
    return ctx;
}

static std::shared_ptr<haicode::Tool> get_process_tool() {
    static haicode::ToolRegistry registry;
    static bool registered = false;
    if (!registered) {
        haicode::register_builtin_tools(registry);
        registered = true;
    }
    auto t = registry.get("process");
    assert(t && "process tool not registered");
    return t;
}

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << msg << "\n"; return false; } } while(0)

static bool test_list_all() {
    auto tool = get_process_tool();
    auto result = tool->execute({{"action", "list"}}, make_ctx());
    CHECK(result.success, "list failed: " + result.error);
    CHECK(!result.output.empty(), "expected non-empty process list");
    std::cout << "[OK] list all processes\n";
    return true;
}

static bool test_list_filter_match() {
    auto tool = get_process_tool();
    // "sshd" is reliably running on this machine (confirmed via netstat)
    auto result = tool->execute({{"action", "list"}, {"filter", "sshd"}}, make_ctx());
    CHECK(result.success, "list with filter failed: " + result.error);
    CHECK(result.output.find("sshd") != std::string::npos,
          "expected sshd in filtered output, got: " + result.output);
    std::cout << "[OK] list filter matches running process\n";
    return true;
}

static bool test_list_filter_no_match() {
    auto tool = get_process_tool();
    auto result = tool->execute(
        {{"action", "list"}, {"filter", "zzz_no_such_process_xyz"}},
        make_ctx());
    CHECK(result.success, "list should succeed even with no match");
    CHECK(result.output == "(no matching processes)",
          "expected '(no matching processes)', got: " + result.output);
    std::cout << "[OK] list filter no match\n";
    return true;
}

static bool test_kill_self_with_zero() {
    // SIGKILL with pid=self would kill our process, so use signal 0 as a
    // probe — it checks that the pid exists without actually sending a signal.
    // Our tool doesn't support signal 0 directly, so instead kill a known
    // subprocess we spawn ourselves.
    auto tool = get_process_tool();

    // Spawn a sleep process we can safely kill
    pid_t pid = fork();
    if (pid == 0) {
        // child: sleep for a long time
        ::sleep(60);
        _exit(0);
    }
    assert(pid > 0);

    auto result = tool->execute(
        {{"action", "kill"}, {"pid", (int)pid}, {"signal", "TERM"}},
        make_ctx());
    CHECK(result.success, "kill TERM failed: " + result.error);
    CHECK(result.output.find("TERM") != std::string::npos, "expected TERM in output");

    // Reap the child
    int status;
    ::waitpid(pid, &status, 0);
    std::cout << "[OK] kill TERM sends signal to subprocess\n";
    return true;
}

static bool test_kill_bad_pid() {
    auto tool = get_process_tool();
    // PID 0x7fffffff is very unlikely to exist
    auto result = tool->execute(
        {{"action", "kill"}, {"pid", 2147483647}, {"signal", "TERM"}},
        make_ctx());
    CHECK(!result.success, "expected failure for nonexistent pid");
    std::cout << "[OK] kill nonexistent pid returns error\n";
    return true;
}

static bool test_kill_unknown_signal() {
    auto tool = get_process_tool();
    auto result = tool->execute(
        {{"action", "kill"}, {"pid", (int)getpid()}, {"signal", "BOGUS"}},
        make_ctx());
    CHECK(!result.success, "expected failure for unknown signal");
    CHECK(result.error.find("unknown signal") != std::string::npos,
          "expected 'unknown signal' in error, got: " + result.error);
    std::cout << "[OK] kill unknown signal returns error\n";
    return true;
}

static bool test_kill_missing_pid() {
    auto tool = get_process_tool();
    auto result = tool->execute({{"action", "kill"}}, make_ctx());
    CHECK(!result.success, "expected failure when pid is missing");
    std::cout << "[OK] kill missing pid returns error\n";
    return true;
}

static bool test_check_port_listening() {
    auto tool = get_process_tool();
    // Port 22 (sshd) is confirmed listening
    auto result = tool->execute({{"action", "check_port"}, {"port", 22}}, make_ctx());
    CHECK(result.success, "check_port failed: " + result.error);
    CHECK(result.output.find(":22") != std::string::npos,
          "expected ':22' in output, got: " + result.output);
    std::cout << "[OK] check_port finds listening port\n";
    return true;
}

static bool test_check_port_not_listening() {
    auto tool = get_process_tool();
    // Port 19999 is very unlikely to be in use
    auto result = tool->execute({{"action", "check_port"}, {"port", 19999}}, make_ctx());
    CHECK(result.success, "check_port should succeed even for unused port");
    CHECK(result.output.find("Nothing listening") != std::string::npos,
          "expected 'Nothing listening' message, got: " + result.output);
    std::cout << "[OK] check_port reports nothing on unused port\n";
    return true;
}

static bool test_check_port_missing_port() {
    auto tool = get_process_tool();
    auto result = tool->execute({{"action", "check_port"}}, make_ctx());
    CHECK(!result.success, "expected failure when port is missing");
    std::cout << "[OK] check_port missing port returns error\n";
    return true;
}

static bool test_missing_action() {
    auto tool = get_process_tool();
    auto result = tool->execute({{"filter", "something"}}, make_ctx());
    CHECK(!result.success, "expected failure when action is missing");
    std::cout << "[OK] missing action returns error\n";
    return true;
}

static bool test_unknown_action() {
    auto tool = get_process_tool();
    auto result = tool->execute({{"action", "explode"}}, make_ctx());
    CHECK(!result.success, "expected failure for unknown action");
    std::cout << "[OK] unknown action returns error\n";
    return true;
}

int main() {
    std::cout << "=== ProcessTool Tests ===\n\n";

    bool ok = true;
    ok &= test_list_all();
    ok &= test_list_filter_match();
    ok &= test_list_filter_no_match();
    ok &= test_kill_self_with_zero();
    ok &= test_kill_bad_pid();
    ok &= test_kill_unknown_signal();
    ok &= test_kill_missing_pid();
    ok &= test_check_port_listening();
    ok &= test_check_port_not_listening();
    ok &= test_check_port_missing_port();
    ok &= test_missing_action();
    ok &= test_unknown_action();

    if (ok) {
        std::cout << "\nAll ProcessTool tests passed!\n";
        return 0;
    } else {
        std::cerr << "\nSome tests FAILED.\n";
        return 1;
    }
}
