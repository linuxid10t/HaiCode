#pragma once
#include <atomic>
#include <map>
#include <string>

namespace haicode {
namespace util {

struct SubprocessResult {
    int exit_code = 0;        // WEXITSTATUS, or -1 when killed by signal / failed to spawn
    bool timed_out = false;   // deadline hit while the direct child lived
    bool interrupted = false; // *interrupt flipped while the direct child lived
    bool truncated = false;   // output exceeded output_cap (kept draining, discarded the rest)
    std::string output;       // sanitized UTF-8, at most output_cap bytes
};

// Run `command` via /bin/sh -c in its own process group with working_dir as
// cwd, /dev/null as stdin, and stdout+stderr merged into one captured pipe.
// The child inherits NO other file descriptors (everything >= 3 is closed
// after the dup2s), so database/socket handles never leak into commands.
//
// The poll loop (100 ms tick) watches three things while the direct child
// lives: *interrupt (kills the whole process group, sets interrupted), the
// deadline (same kill, sets timed_out), and the pipe (appended into output
// up to output_cap; past the cap bytes are read and discarded so a chatty
// child is never SIGPIPEd — truncated reports the overflow instead of
// turning a success into exit -1).
//
// Orphan semantics: once the direct child is reaped, a backgrounded
// descendant (`server &`) may still hold the pipe. The loop then drains for
// a bounded grace window (~500 ms, ending early on EOF or when output
// already hit the cap) and returns the child's real exit code — a
// backgrounded command returns promptly and stays alive by design. Timeout
// and interrupt only fire while the direct child itself lives.
SubprocessResult run_subprocess(const std::string& command,
                                const std::string& working_dir,
                                int timeout_sec,
                                const std::atomic<bool>* interrupt,
                                const std::map<std::string, std::string>& extra_env = {},
                                size_t output_cap = 100 * 1024);

} // namespace util
} // namespace haicode
