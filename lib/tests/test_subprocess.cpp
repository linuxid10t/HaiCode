// util::run_subprocess — the shared cancellable subprocess runner behind
// bash/grep/diff/git/find/process/screenshot and the engine build hook.
// Covers: interrupt (pre-set and mid-run), timeout, orphaned background
// children returning promptly, output-cap truncation without exit-code
// corruption, extra_env propagation, exit codes, and fd hygiene.
#include <haicode/subprocess.h>
#include <haicode/haicode.h>
#include <haicode/tool.h>
#include <haicode/util.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << (msg) << "\n"; return false; } } while(0)
// CHECK for use inside void lambdas: fail fast instead of unwinding.
#define CHECK_EXIT(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << (msg) << "\n"; std::exit(1); } } while(0)

using haicode::util::run_subprocess;
using haicode::util::SubprocessResult;

static bool returns_fast(const std::function<void()>& fn, int seconds) {
    auto begin = std::chrono::steady_clock::now();
    fn();
    return std::chrono::steady_clock::now() - begin < std::chrono::seconds(seconds);
}

// Interrupt already set when the call begins: the child is killed almost
// immediately and the result reports `interrupted`.
static bool test_interrupt_preset() {
    std::atomic<bool> flag{true};
    bool fast = returns_fast([&] {
        SubprocessResult r = run_subprocess("sleep 60", "/tmp", 30, &flag);
        CHECK_EXIT(r.interrupted, "preset interrupt must set interrupted");
        CHECK_EXIT(!r.timed_out, "preset interrupt is not a timeout");
    }, 3);
    CHECK(fast, "preset interrupt must return within 3 s");
    std::cout << "[OK] preset interrupt kills sleep 60 promptly\n";
    return true;
}

// Interrupt lands while the child is running: same contract.
static bool test_interrupt_midrun() {
    std::atomic<bool> flag{false};
    std::thread setter([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        flag.store(true);
    });
    bool fast = returns_fast([&] {
        SubprocessResult r = run_subprocess("sleep 60", "/tmp", 30, &flag);
        CHECK_EXIT(r.interrupted, "mid-run interrupt must set interrupted");
    }, 3);
    setter.join();
    CHECK(fast, "mid-run interrupt must return within 3 s");
    std::cout << "[OK] mid-run interrupt kills sleep 60 promptly\n";
    return true;
}

// A backgrounded child keeps the pipe open but the direct child exits:
// the call must return promptly with the shell's real exit code.
static bool test_orphan_returns_promptly() {
    bool fast = returns_fast([&] {
        SubprocessResult r = run_subprocess(
            "sh -c 'echo STARTED; sleep 60' & exit 0", "/tmp", 30, nullptr);
        CHECK_EXIT(r.exit_code == 0, "backgrounded child: direct child exits 0, got " +
              std::to_string(r.exit_code));
        CHECK_EXIT(!r.timed_out && !r.interrupted, "no timeout/interrupt expected");
        CHECK_EXIT(r.output.find("STARTED") != std::string::npos,
              "orphan's early output must be captured");
    }, 5);
    CHECK(fast, "orphan case must return within 5 s, not wait the 30 s timeout");
    std::cout << "[OK] backgrounded child returns promptly with exit 0\n";
    return true;
}

// 200 KB of output against a 100 KB default cap: exit status must stay 0
// (the old popen path SIGPIPEd the child into -1) and truncation reported.
static bool test_output_cap_truncation() {
    SubprocessResult r = run_subprocess(
        "dd if=/dev/zero bs=1024 count=200 2>/dev/null", "/tmp", 30, nullptr);
    CHECK(r.exit_code == 0, "truncated success must keep exit 0, got " +
          std::to_string(r.exit_code));
    CHECK(r.truncated, "200 KB output must set truncated");
    CHECK(r.output.size() == 100 * 1024, "output must be exactly the cap, got " +
          std::to_string(r.output.size()));
    std::cout << "[OK] over-cap output: exit 0 + truncated + capped bytes\n";
    return true;
}

// Deadline expiry while the child lives.
static bool test_timeout() {
    bool fast = returns_fast([&] {
        SubprocessResult r = run_subprocess("sleep 30", "/tmp", 1, nullptr);
        CHECK_EXIT(r.timed_out, "deadline must set timed_out");
        CHECK_EXIT(!r.interrupted, "timeout is not an interrupt");
    }, 4);
    CHECK(fast, "timeout must return promptly after the 1 s deadline");
    std::cout << "[OK] timeout kills sleep 30 after 1 s\n";
    return true;
}

// extra_env entries must be visible to the command.
static bool test_extra_env() {
    SubprocessResult r = run_subprocess("echo $HAICODE_TEST_VAR", "/tmp", 10,
                                        nullptr,
                                        {{"HAICODE_TEST_VAR", "hello-481516"}});
    CHECK(r.exit_code == 0, "echo must exit 0");
    CHECK(r.output.find("hello-481516") != std::string::npos,
          "extra_env must reach the child environment: '" + r.output + "'");
    std::cout << "[OK] extra_env visible in child\n";
    return true;
}

// Plain exit-code passthrough.
static bool test_exit_codes() {
    CHECK(run_subprocess("exit 7", "/tmp", 10, nullptr).exit_code == 7,
          "exit 7 must pass through");
    CHECK(run_subprocess("exit 0", "/tmp", 10, nullptr).exit_code == 0,
          "exit 0 must pass through");
    CHECK(run_subprocess("exit 42", "/tmp", 10, nullptr).exit_code == 42,
          "exit 42 must pass through");
    std::cout << "[OK] exit codes pass through\n";
    return true;
}

// The child must not inherit the parent's file descriptors: open a real fd
// in the parent, then have the child attempt a write to that fd number. A
// closed fd makes the shell redirection fail (prints CLOSED); an inherited
// one would succeed (prints OPEN). (Haiku has no /dev/fd to list.)
static bool test_no_fd_leak() {
    int probe = open("/dev/null", O_WRONLY);
    CHECK(probe >= 3, "probe open failed or landed on a std fd");
    SubprocessResult r = run_subprocess(
        "echo x >&" + std::to_string(probe) + " 2>/dev/null && echo OPEN || echo CLOSED",
        "/tmp", 10, nullptr);
    close(probe);
    CHECK(r.exit_code == 0, "probe command must succeed: " + r.output);
    CHECK(r.output.find("CLOSED") != std::string::npos,
          "fd " + std::to_string(probe) + " must be closed in the child, got: "
          + r.output);
    std::cout << "[OK] child inherits no fds >= 3\n";
    return true;
}

// The whole stack through the tool registry: BashTool must report success
// with the truncation marker (regression: read_pipe's pclose SIGPIPE turned
// this into exit -1 / failure).
static bool test_bash_tool_truncation_via_registry() {
    haicode::ToolRegistry registry;
    haicode::register_builtin_tools(registry);
    haicode::PermissionGate gate;
    gate.set_rules({{"*", "*", haicode::PermissionEffect::Allow}});
    haicode::ToolContext ctx;
    ctx.working_dir = "/tmp";
    ctx.tool_name = "bash";
    ctx.interrupt = nullptr;

    haicode::ToolResult result = registry.execute(
        "bash", nlohmann::json{{"command",
            "dd if=/dev/zero bs=1024 count=200 2>/dev/null"}},
        ctx, gate);
    CHECK(result.success, "over-output bash must still succeed: " + result.error);
    CHECK(result.output.find("[output truncated]") != std::string::npos,
          "output must carry the truncation marker");
    std::cout << "[OK] BashTool via registry: success + truncation marker\n";
    return true;
}

// BashTool timeout path: timed_out maps to a failed result with a clear
// error, not exit code 124.
static bool test_bash_tool_timeout() {
    haicode::ToolRegistry registry;
    haicode::register_builtin_tools(registry);
    haicode::PermissionGate gate;
    gate.set_rules({{"*", "*", haicode::PermissionEffect::Allow}});
    haicode::ToolContext ctx;
    ctx.working_dir = "/tmp";
    ctx.tool_name = "bash";

    haicode::ToolResult result = registry.execute(
        "bash", nlohmann::json{{"command", "sleep 30"}, {"timeout", 1}},
        ctx, gate);
    CHECK(!result.success, "timed-out bash must fail");
    CHECK(result.error.find("timed out") != std::string::npos,
          "error must say timed out: " + result.error);
    std::cout << "[OK] BashTool timeout: failed result with timeout error\n";
    return true;
}

int main() {
    std::cout << "=== run_subprocess: cancellable subprocess runner ===\n\n";
    bool ok = true;
    ok &= test_interrupt_preset();
    ok &= test_interrupt_midrun();
    ok &= test_orphan_returns_promptly();
    ok &= test_output_cap_truncation();
    ok &= test_timeout();
    ok &= test_extra_env();
    ok &= test_exit_codes();
    ok &= test_no_fd_leak();
    ok &= test_bash_tool_truncation_via_registry();
    ok &= test_bash_tool_timeout();
    std::cout << (ok ? "\nAll subprocess tests passed!\n"
                     : "\nSome tests FAILED.\n");
    return ok ? 0 : 1;
}
