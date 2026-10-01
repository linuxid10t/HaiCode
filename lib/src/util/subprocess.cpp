#include <haicode/subprocess.h>
#include <haicode/util.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace haicode {
namespace util {

namespace {
// How long to keep draining after the direct child is reaped, waiting for a
// backgrounded descendant's last output before returning.
constexpr auto kGraceDrain = std::chrono::milliseconds(500);
constexpr auto kTick = std::chrono::milliseconds(100);
} // namespace

SubprocessResult run_subprocess(const std::string& command,
                                const std::string& working_dir,
                                int timeout_sec,
                                const std::atomic<bool>* interrupt,
                                const std::map<std::string, std::string>& extra_env,
                                size_t output_cap) {
    SubprocessResult result;
    if (timeout_sec <= 0) timeout_sec = 30;

    int fds[2];
    if (pipe(fds) != 0) {
        result.exit_code = -1;
        result.output = std::string("pipe failed: ") + strerror(errno);
        return result;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        result.exit_code = -1;
        result.output = std::string("fork failed: ") + strerror(errno);
        return result;
    }
    if (pid == 0) {
        // Child: own process group, detached stdin, merged stdout+stderr.
        setpgid(0, 0);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        // Close everything else: the parent's SQLite/socket/engine handles
        // must not leak into arbitrary commands. Bounded at a high fd number
        // so an RLIM_INFINITY ceiling can't make this loop unbounded.
        struct rlimit rl;
        rlim_t max_fd = 1024;
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0
                && rl.rlim_max != RLIM_INFINITY && rl.rlim_max < 65536)
            max_fd = rl.rlim_max;
        for (rlim_t f = 3; f < max_fd; f++) close(static_cast<int>(f));
        if (!working_dir.empty() && chdir(working_dir.c_str()) != 0) _exit(127);
        for (const auto& [name, value] : extra_env)
            setenv(name.c_str(), value.c_str(), 1);
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(fds[1]);
    // Race-safe group setup: the child may already have exec'd.
    setpgid(pid, pid);
    int flags = fcntl(fds[0], F_GETFL);
    if (flags >= 0) fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

    std::string output;
    bool pipe_open = true;
    bool child_done = false;
    int status = 0;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(timeout_sec);
    // max() until the direct child is reaped; then the bounded grace window.
    auto grace_deadline = std::chrono::steady_clock::time_point::max();

    // Read everything currently buffered; stops at EAGAIN (nothing more now).
    // Returns false when the pipe hit EOF or a hard error (caller closes it).
    auto drain_available = [&]() -> bool {
        char buf[4096];
        for (;;) {
            ssize_t n = read(fds[0], buf, sizeof(buf));
            if (n > 0) {
                if (output.size() < output_cap) {
                    size_t take = std::min(static_cast<size_t>(n),
                                           output_cap - output.size());
                    output.append(buf, take);
                    if (take < static_cast<size_t>(n)) result.truncated = true;
                } else {
                    result.truncated = true;
                }
                continue;
            }
            if (n == 0) return false;
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            return false;
        }
    };

    while (!child_done || (pipe_open && !result.truncated
                           && std::chrono::steady_clock::now() < grace_deadline)) {
        if (!child_done) {
            pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid || (waited < 0 && errno == ECHILD)) {
                child_done = true;
                grace_deadline = std::chrono::steady_clock::now() + kGraceDrain;
            } else {
                bool interrupted_now = interrupt && interrupt->load();
                if (interrupted_now
                        || std::chrono::steady_clock::now() >= deadline) {
                    result.interrupted = interrupted_now;
                    result.timed_out = !interrupted_now;
                    kill(-pid, SIGKILL);
                    kill(pid, SIGKILL);  // fallback if the group setup raced
                    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                    child_done = true;
                    if (pipe_open && drain_available()) {
                        struct pollfd pfd {fds[0], POLLIN | POLLHUP, 0};
                        if (poll(&pfd, 1, 0) > 0) drain_available();
                    }
                    break;
                }
            }
        }
        if (pipe_open) {
            struct pollfd pfd {fds[0], POLLIN | POLLHUP, 0};
            int ready = poll(&pfd, 1, 100);
            if (ready > 0) {
                if (!drain_available()) pipe_open = false;
            } else if (ready < 0 && errno != EINTR) {
                pipe_open = false;
            }
        } else if (!child_done) {
            // Child alive with its output ends closed: wait for exit/deadline.
            std::this_thread::sleep_for(kTick);
        }
    }
    close(fds[0]);
    if (!child_done) {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }

    output = sanitize_utf8(output);
    if (output.size() > output_cap) output = truncate_utf8(output, output_cap);
    result.output = std::move(output);
    if (!result.timed_out && !result.interrupted)
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    else
        result.exit_code = -1;
    return result;
}

} // namespace util
} // namespace haicode
