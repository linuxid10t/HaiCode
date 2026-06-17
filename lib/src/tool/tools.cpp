#include <haicode/tool.h>
#include <haicode/util.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <glob.h>
#include <fnmatch.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/stat.h>

namespace haicode {

// Defined in web_tools.cpp — registered through register_builtin_tools.
void register_web_tools(ToolRegistry& registry);

static const size_t MAX_OUTPUT = 100 * 1024;

// Single-quote a string for safe shell embedding: 'value' with ' → '\''
static std::string sq(const std::string& s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'') r += "'\\''";
        else r += c;
    }
    r += "'";
    return r;
}

// Read up to MAX_OUTPUT bytes from an open pipe, stopping early when limit hit.
static std::string read_pipe(FILE* pipe) {
    std::array<char, 4096> buf;
    std::string out;
    out.reserve(4096);
    while (fgets(buf.data(), buf.size(), pipe)) {
        out += buf.data();
        if (out.size() >= MAX_OUTPUT) {
            out.resize(MAX_OUTPUT);
            out += "\n[output truncated]";
            break;
        }
    }
    return out;
}

// Resolve `path` against `working_dir` when relative. Empty path stays empty.
static std::string resolve_path(const std::string& path, const std::string& working_dir) {
    if (path.empty()) return path;
    if (path[0] == '/') return path;
    std::string base = working_dir;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return (base.empty() ? "." : base) + "/" + path;
}

// Build an error string for a missing/empty required field. If `input` is an
// empty object, the most likely cause is lost streaming arguments — say so
// so the model retries rather than guessing what it did wrong.
static std::string missing_field(const std::string& tool, const std::string& field,
                                 const nlohmann::json& input) {
    std::string err = tool + ": missing or empty '" + field + "'.";
    if (input.is_object() && input.empty())
        err += " Input is empty — the tool arguments were likely lost in streaming. Retry the tool call.";
    else
        err += " Received input: " + input.dump();
    return err;
}

// ---- BashTool ----

class BashTool : public Tool {
public:
    std::string name() const override { return "bash"; }
    std::string description() const override {
        return "Execute a bash command and return its output. "
               "Use for running code, file operations, git, etc.";
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"command", {{"type", "string"}, {"description", "The bash command to run. Runs from the project directory. stderr is merged into stdout. Output is capped at 100 KB."}}},
                {"timeout", {{"type", "integer"}, {"description", "Timeout in seconds (default 30, max enforced by the `timeout` binary). Exit code 124 = timed out."}}}
            }},
            {"required", nlohmann::json::array({"command"})}
        };
    }
    std::string resource(const nlohmann::json& input, const ToolContext& ctx) const override {
        (void)ctx;
        return input.value("command", "");
    }

    ToolResult execute(const nlohmann::json& input, const ToolContext& ctx) override {
        std::string command = input.value("command", "");
        if (command.empty())
            return {false, "", missing_field("bash", "command", input)};

        int timeout_sec = input.value("timeout", 30);
        if (timeout_sec <= 0) timeout_sec = 30;

        // Build inner command: cd to working_dir (single-quoted), then run user command
        std::string inner;
        if (!ctx.working_dir.empty())
            inner = "cd " + sq(ctx.working_dir) + " && ";
        inner += "{ " + command + "; }";

        // Wrap with timeout, run via sh, merge stderr
        std::string full_cmd = "timeout " + std::to_string(timeout_sec)
                             + " sh -c " + sq(inner) + " 2>&1";

        FILE* pipe = popen(full_cmd.c_str(), "r");
        if (!pipe) return {false, "", std::string("Failed to execute command: ") + strerror(errno)};

        std::string output = read_pipe(pipe);
        int status = pclose(pipe);
        int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

        if (rc == 124)
            return {false, output, "Command timed out after " + std::to_string(timeout_sec) + "s"};

        return {rc == 0, output, rc != 0 ? "Exit code: " + std::to_string(rc) : ""};
    }
};

// ---- ReadTool ----

static bool is_binary(std::ifstream& f) {
    char buf[8192];
    f.read(buf, sizeof(buf));
    std::streamsize n = f.gcount();
    for (std::streamsize i = 0; i < n; i++) {
        if (buf[i] == '\0') return true;
    }
    f.seekg(0);
    return false;
}

class ReadTool : public Tool {
public:
    std::string name() const override { return "read"; }
    std::string description() const override {
        return "Read a file's contents. Optionally specify line range with offset (1-based start line) and limit (line count).";
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"path",   {{"type", "string"}, {"description", "File path to read. Absolute, or relative to the project directory."}}},
                {"offset", {{"type", "integer"}, {"description", "First line to read, 1-based. Default 1 (start of file). Use this to skip into a file you've already partially read."}}},
                {"limit",  {{"type", "integer"}, {"description", "Maximum number of lines to read. Default 0 = unlimited."}}}
            }},
            {"required", nlohmann::json::array({"path"})}
        };
    }
    std::string required_permission() const override { return "read"; }
    std::string resource(const nlohmann::json& input, const ToolContext& ctx) const override {
        std::string path = input.value("path", "");
        return path.empty() ? ctx.working_dir : resolve_path(path, ctx.working_dir);
    }

    ToolResult execute(const nlohmann::json& input, const ToolContext& ctx) override {
        std::string path = input.value("path", "");
        if (path.empty())
            return {false, "", missing_field("read", "path", input)};

        // Resolve relative paths
        if (path[0] != '/') {
            std::string base = ctx.working_dir;
            while (!base.empty() && base.back() == '/') base.pop_back();
            path = (base.empty() ? "." : base) + "/" + path;
        }

        int offset = input.value("offset", 1);
        int limit  = input.value("limit", 0);
        if (offset < 1) offset = 1;

        std::ifstream f(path, std::ios::binary);
        if (!f.is_open())
            return {false, "", "Cannot open file: " + path + ": " + strerror(errno)};

        if (is_binary(f))
            return {false, "", "Binary file, cannot display: " + path};

        std::string output;
        std::string line;
        int lineno = 0;
        int printed = 0;
        while (std::getline(f, line)) {
            lineno++;
            if (lineno < offset) continue;
            output += std::to_string(lineno) + "\t" + line + "\n";
            printed++;
            if (output.size() >= MAX_OUTPUT) {
                output.resize(MAX_OUTPUT);
                output += "\n[output truncated]";
                break;
            }
            if (limit > 0 && printed >= limit) break;
        }
        return {true, output, ""};
    }
};

// ---- WriteTool ----

static bool make_parent_dirs(const std::string& path) {
    size_t pos = path.rfind('/');
    if (pos == std::string::npos || pos == 0) return true;
    std::string dir = path.substr(0, pos);

    // Walk the path and create each missing component
    for (size_t i = 1; i <= dir.size(); i++) {
        if (i == dir.size() || dir[i] == '/') {
            std::string component = dir.substr(0, i);
            struct stat st{};
            if (stat(component.c_str(), &st) != 0) {
                if (mkdir(component.c_str(), 0755) != 0 && errno != EEXIST)
                    return false;
            }
        }
    }
    return true;
}

class WriteTool : public Tool {
public:
    std::string name() const override { return "write"; }
    std::string description() const override {
        return "Write content to a file, creating or overwriting it. Parent directories are created automatically.";
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"path",    {{"type", "string"}, {"description", "File to write. Absolute, or relative to the project directory. Missing parent directories are created. Overwrites the existing file entirely."}}},
                {"content", {{"type", "string"}, {"description", "Full new contents of the file. The entire file becomes this string."}}}
            }},
            {"required", nlohmann::json::array({"path", "content"})}
        };
    }
    std::string required_permission() const override { return "write"; }
    std::string resource(const nlohmann::json& input, const ToolContext& ctx) const override {
        std::string path = input.value("path", "");
        return path.empty() ? ctx.working_dir : resolve_path(path, ctx.working_dir);
    }

    ToolResult execute(const nlohmann::json& input, const ToolContext& ctx) override {
        std::string path = input.value("path", "");
        std::string content = input.value("content", "");
        if (path.empty())
            return {false, "", missing_field("write", "path", input)};
        // content may be legitimately empty (writing an empty file) — but if
        // path is also missing and input is otherwise empty, the streaming
        // layer dropped the args. missing_field() above already flags that.

        // Resolve relative paths
        if (path[0] != '/') {
            std::string base = ctx.working_dir;
            while (!base.empty() && base.back() == '/') base.pop_back();
            path = (base.empty() ? "." : base) + "/" + path;
        }

        if (!make_parent_dirs(path))
            return {false, "", "Cannot create parent directories for: " + path + ": " + strerror(errno)};

        // Atomic write: write to tmp, then rename
        std::string tmp_path = path + ".tmp_write";
        {
            std::ofstream f(tmp_path, std::ios::out | std::ios::trunc | std::ios::binary);
            if (!f.is_open())
                return {false, "", std::string("Cannot open temp file for write: ") + strerror(errno)};
            f.write(content.data(), static_cast<std::streamsize>(content.size()));
            if (!f.good()) {
                f.close();
                std::remove(tmp_path.c_str());
                return {false, "", "Write error: " + std::string(strerror(errno))};
            }
            f.close();
            if (!f.good()) {
                std::remove(tmp_path.c_str());
                return {false, "", "Flush error: " + std::string(strerror(errno))};
            }
        }

        if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
            std::remove(tmp_path.c_str());
            return {false, "", "Cannot rename to target: " + path + ": " + strerror(errno)};
        }

        return {true, "Written " + std::to_string(content.size()) + " bytes to " + path, ""};
    }
};

// ---- GlobTool ----

class GlobTool : public Tool {
public:
    std::string name() const override { return "glob"; }
    std::string description() const override {
        return "Find files matching a glob pattern. Note: '**' recursive matching is not supported; use grep or bash find for recursive searches.";
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"pattern", {{"type", "string"}, {"description", "POSIX glob pattern (e.g. '*.cpp', 'src/*.h'). Patterns starting with '/' are treated as absolute. NOTE: '**' recursive matching is NOT supported — use grep or `bash find` for recursive searches."}}},
                {"path",    {{"type", "string"}, {"description", "Base directory to search. Default: the project directory."}}}
            }},
            {"required", nlohmann::json::array({"pattern"})}
        };
    }
    std::string resource(const nlohmann::json& input, const ToolContext& ctx) const override {
        (void)ctx;
        return input.value("pattern", "");
    }

    ToolResult execute(const nlohmann::json& input, const ToolContext& ctx) override {
        std::string pattern = input.value("pattern", "");
        if (pattern.empty())
            return {false, "", missing_field("glob", "pattern", input)};

        std::string full_pattern;
        if (pattern[0] == '/') {
            // Absolute pattern: use as-is
            full_pattern = pattern;
        } else {
            std::string base = input.value("path", ctx.working_dir);
            if (base.empty()) base = ".";
            while (base.size() > 1 && base.back() == '/') base.pop_back();
            full_pattern = base + "/" + pattern;
        }

        glob_t g{};
        // No GLOB_MARK to avoid trailing slash confusion
        int rc = glob(full_pattern.c_str(), GLOB_TILDE, nullptr, &g);

        std::string output;
        if (rc == 0) {
            for (size_t i = 0; i < g.gl_pathc; i++) {
                output += g.gl_pathv[i];
                output += '\n';
                if (output.size() >= MAX_OUTPUT) {
                    output.resize(MAX_OUTPUT);
                    output += "\n[output truncated]";
                    break;
                }
            }
        }
        // globfree is always safe to call regardless of rc
        globfree(&g);

        if (rc != 0 && rc != GLOB_NOMATCH)
            return {false, "", "Glob error (code " + std::to_string(rc) + ")"};

        return {true, output.empty() ? "(no matches)" : output, ""};
    }
};

// ---- GrepTool ----

class GrepTool : public Tool {
public:
    std::string name() const override { return "grep"; }
    std::string description() const override {
        return "Search for a pattern in files using grep (recursive by default).";
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"pattern",      {{"type", "string"}, {"description", "Regular expression (basic grep syntax) to search for. Passed to /bin/grep -e."}}},
                {"path",         {{"type", "string"}, {"description", "File or directory to search. Absolute, or relative to the project directory. Default: project directory."}}},
                {"include",      {{"type", "string"}, {"description", "Filename glob filter, e.g. '*.cpp' or '*.{h,cpp}'. Restricts which files are scanned."}}},
                {"line_numbers", {{"type", "boolean"}, {"description", "Prefix each match with `lineno:` (default true)."}}}
            }},
            {"required", nlohmann::json::array({"pattern"})}
        };
    }
    // Resource is the search path (where grep scans), not the regex — that's
    // what users typically want to scope with permission rules.
    std::string resource(const nlohmann::json& input, const ToolContext& ctx) const override {
        if (input.contains("path") && input["path"].is_string()) {
            std::string p = input["path"].get<std::string>();
            if (!p.empty()) return resolve_path(p, ctx.working_dir);
        }
        return ctx.working_dir.empty() ? "." : ctx.working_dir;
    }

    ToolResult execute(const nlohmann::json& input, const ToolContext& ctx) override {
        std::string pattern = input.value("pattern", "");
        if (pattern.empty())
            return {false, "", missing_field("grep", "pattern", input)};

        // Resolve search path
        std::string path;
        if (input.contains("path") && input["path"].is_string()) {
            path = input["path"].get<std::string>();
            if (!path.empty() && path[0] != '/') {
                std::string base = ctx.working_dir;
                while (!base.empty() && base.back() == '/') base.pop_back();
                path = (base.empty() ? "." : base) + "/" + path;
            }
        } else {
            path = ctx.working_dir.empty() ? "." : ctx.working_dir;
        }

        bool line_numbers = input.value("line_numbers", true);
        std::string include = input.value("include", "");

        // Build command with all arguments properly single-quoted
        std::string cmd = "/bin/grep -r --color=never";
        if (line_numbers) cmd += " -n";
        if (!include.empty()) cmd += " --include=" + sq(include);
        cmd += " -e " + sq(pattern);
        cmd += " " + sq(path);
        cmd += " 2>&1";

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe)
            return {false, "", std::string("Failed to run grep: ") + strerror(errno)};

        std::string output = read_pipe(pipe);
        int status = pclose(pipe);
        int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

        // grep exit codes: 0 = found, 1 = not found, 2 = error
        if (rc == 2)
            return {false, output, "grep error"};

        return {true, output.empty() ? "(no matches)" : output, ""};
    }
};

// Atomic write: tmp file + rename. Creates parent dirs. Returns "" on success,
// an error message otherwise. The tmp file is always cleaned up on failure.
static std::string atomic_write(const std::string& path, const std::string& content) {
    if (!make_parent_dirs(path))
        return std::string("Cannot create parent directories for: ") + path + ": " + strerror(errno);

    std::string tmp_path = path + ".tmp_write";
    {
        std::ofstream f(tmp_path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!f.is_open())
            return std::string("Cannot open temp file for write: ") + strerror(errno);
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!f.good()) {
            f.close();
            std::remove(tmp_path.c_str());
            return "Write error: " + std::string(strerror(errno));
        }
        f.close();
        if (!f.good()) {
            std::remove(tmp_path.c_str());
            return "Flush error: " + std::string(strerror(errno));
        }
    }

    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        std::remove(tmp_path.c_str());
        return "Cannot rename to target: " + path + ": " + strerror(errno);
    }
    return "";
}

// True if the buffer looks binary (null byte in first 8 KB).
static bool looks_binary(const std::string& s) {
    size_t scan = std::min<size_t>(s.size(), 8192);
    for (size_t i = 0; i < scan; i++)
        if (s[i] == '\0') return true;
    return false;
}

// ---- EditTool ----

class EditTool : public Tool {
public:
    std::string name() const override { return "edit"; }
    std::string description() const override {
        return "Replace a unique string in a file with a new string. Use this for "
               "surgical edits instead of rewriting the whole file with `write`. "
               "Read the file first; never edit what you haven't read. "
               "old_string must match exactly (including whitespace and indentation) "
               "and must be unique unless replace_all=true.";
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"path",        {{"type", "string"}, {"description", "File to edit. Absolute, or relative to the project directory."}}},
                {"old_string",  {{"type", "string"}, {"description", "Exact text to find. Copy it verbatim from the file (indentation, newlines, quotes). Include enough surrounding context to make the match unique."}}},
                {"new_string",  {{"type", "string"}, {"description", "Replacement text. Empty string deletes old_string."}}},
                {"replace_all", {{"type", "boolean"}, {"description", "Replace every occurrence instead of requiring a unique match. Default false."}}}
            }},
            {"required", nlohmann::json::array({"path", "old_string", "new_string"})}
        };
    }
    std::string required_permission() const override { return "write"; }
    std::string resource(const nlohmann::json& input, const ToolContext& ctx) const override {
        std::string path = input.value("path", "");
        return path.empty() ? ctx.working_dir : resolve_path(path, ctx.working_dir);
    }

    ToolResult execute(const nlohmann::json& input, const ToolContext& ctx) override {
        std::string path = input.value("path", "");
        std::string old_string = input.value("old_string", "");
        std::string new_string = input.value("new_string", "");
        bool replace_all = input.value("replace_all", false);

        if (path.empty())
            return {false, "", missing_field("edit", "path", input)};
        if (old_string.empty())
            return {false, "", "edit: old_string is empty; nothing to find. "
                               "To delete text, provide both old_string and an empty new_string."};

        path = resolve_path(path, ctx.working_dir);

        // Read whole file
        std::ifstream fin(path, std::ios::binary);
        if (!fin.is_open())
            return {false, "", "Cannot open file: " + path + ": " + strerror(errno)};
        std::ostringstream ss;
        ss << fin.rdbuf();
        std::string content = ss.str();
        fin.close();

        if (looks_binary(content))
            return {false, "", "Binary file, cannot edit: " + path};

        // Count occurrences
        size_t count = 0;
        for (size_t pos = 0; (pos = content.find(old_string, pos)) != std::string::npos; count++)
            pos += old_string.size();

        if (count == 0)
            return {false, "", "old_string not found in " + path + ". "
                               "Read the file and copy the exact text (including whitespace)."};
        if (!replace_all && count > 1)
            return {false, "", "old_string appears " + std::to_string(count) + " times in " + path + ". "
                               "Include more surrounding context to make it unique, or set replace_all=true."};

        // Perform replacement
        std::string new_content;
        if (replace_all) {
            new_content.reserve(content.size() + new_string.size());
            size_t last = 0, pos = 0;
            while ((pos = content.find(old_string, last)) != std::string::npos) {
                new_content.append(content, last, pos - last);
                new_content.append(new_string);
                last = pos + old_string.size();
            }
            new_content.append(content, last, std::string::npos);
        } else {
            size_t pos = content.find(old_string);
            new_content = content.substr(0, pos) + new_string + content.substr(pos + old_string.size());
        }

        std::string err = atomic_write(path, new_content);
        if (!err.empty())
            return {false, "", err};

        std::string summary = replace_all
            ? "Replaced " + std::to_string(count) + " occurrences in " + path
            : "Edited " + path;
        return {true, summary, ""};
    }
};

// ---- LsTool ----

class LsTool : public Tool {
public:
    std::string name() const override { return "ls"; }
    std::string description() const override {
        return "List the contents of a directory. Returns one entry per line, "
               "alphabetically sorted, with a trailing '/' on directories. "
               "Prefer this over `bash ls` for quick codebase exploration.";
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}, {"description", "Directory to list. Absolute, or relative to the project directory. Default: the project directory."}}}
            }},
            {"required", nlohmann::json::array()}
        };
    }
    std::string required_permission() const override { return "read"; }
    std::string resource(const nlohmann::json& input, const ToolContext& ctx) const override {
        if (input.contains("path") && input["path"].is_string() && !input["path"].get<std::string>().empty())
            return resolve_path(input["path"].get<std::string>(), ctx.working_dir);
        return ctx.working_dir.empty() ? "." : ctx.working_dir;
    }

    ToolResult execute(const nlohmann::json& input, const ToolContext& ctx) override {
        std::string path;
        if (input.contains("path") && input["path"].is_string() && !input["path"].get<std::string>().empty())
            path = resolve_path(input["path"].get<std::string>(), ctx.working_dir);
        else
            path = ctx.working_dir.empty() ? "." : ctx.working_dir;

        DIR* dir = opendir(path.c_str());
        if (!dir)
            return {false, "", "Cannot open directory: " + path + ": " + strerror(errno)};

        std::vector<std::string> entries;
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            std::string name = ent->d_name;
            if (name == "." || name == "..") continue;

            std::string suffix;
            struct stat st{};
            std::string full = path + "/" + name;
            if (stat(full.c_str(), &st) == 0) {
                if (S_ISDIR(st.st_mode))         suffix = "/";
                else if (S_ISLNK(st.st_mode))    suffix = "@";
            }
            entries.push_back(name + suffix);
        }
        closedir(dir);

        std::sort(entries.begin(), entries.end());

        std::string output;
        for (auto& e : entries) {
            output += e;
            output += '\n';
            if (output.size() >= MAX_OUTPUT) {
                output.resize(MAX_OUTPUT);
                output += "\n[output truncated]";
                break;
            }
        }
        return {true, output.empty() ? "(empty directory)" : output, ""};
    }
};

// ---- ExternalTerminalTool ----

class ExternalTerminalTool : public Tool {
public:
    std::string name() const override { return "external_terminal"; }
    std::string description() const override {
        return "Open a command in a new Haiku Terminal window. Use this for "
               "interactive full-screen terminal programs (editors like vim, "
               "ncurses apps, REPLs, shells) that cannot run inside the `bash` "
               "tool because `bash` merges stderr and reads output through a "
               "pipe. Works regardless of which HaiCode frontend (GUI or TUI) "
               "is in use. Returns immediately; the window closes when the "
               "command exits.";
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"command",     {{"type", "string"}, {"description", "Command to run in the new Terminal window. Interactive programs are fine — output is not captured."}}},
                {"working_dir", {{"type", "string"}, {"description", "Working directory for the new window. Absolute, or relative to the project directory. Default: the project directory."}}}
            }},
            {"required", nlohmann::json::array({"command"})}
        };
    }
    // Distinct permission action from bash so the prompt makes clear which tool
    // is asking. Users who want a single rule covering both can use action "*".
    std::string required_permission() const override { return "external_terminal"; }
    std::string resource(const nlohmann::json& input, const ToolContext& ctx) const override {
        (void)ctx;
        return input.value("command", "");
    }

    ToolResult execute(const nlohmann::json& input, const ToolContext& ctx) override {
        std::string command = input.value("command", "");
        if (command.empty())
            return {false, "", missing_field("external_terminal", "command", input)};

        std::string dir;
        if (input.contains("working_dir") && input["working_dir"].is_string()
                && !input["working_dir"].get<std::string>().empty())
            dir = resolve_path(input["working_dir"].get<std::string>(), ctx.working_dir);
        else
            dir = ctx.working_dir.empty() ? "." : ctx.working_dir;

        // Run just the command — Terminal closes the window when it exits.
        std::string inner = command;

        pid_t pid = fork();
        if (pid < 0)
            return {false, "", std::string("fork failed: ") + strerror(errno)};

        if (pid == 0) {
            // First child: new session, drop the engine's stdio fds.
            setsid();
            close(0); close(1); close(2);

            // Double-fork so the engine doesn't have to reap the grandchild.
            pid_t grand = fork();
            if (grand < 0)  _exit(127);
            if (grand > 0)  _exit(0);

            // Grandchild: replace self with Terminal.
            execl("/boot/system/apps/Terminal", "Terminal",
                  "-w", dir.c_str(),
                  "/bin/sh", "-c", inner.c_str(),
                  (char*)nullptr);
            _exit(127);  // only reached if execl failed
        }

        // Parent: reap the intermediate child (exits immediately after fork).
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}

        return {true, "Opened in new Terminal window (cwd=" + dir + ")", ""};
    }
};

// ---- ProposePlanTool ----
//
// Only meaningful in Plan mode (the engine filters it out of tool_defs in
// Build mode). Writes the plan markdown to <project_dir>/.haicode/plans/
// and returns the path so the engine can surface it in the PlanProposed
// event. The engine ends the turn after this tool runs.

class ProposePlanTool : public Tool {
public:
    std::string name() const override { return "propose_plan"; }
    std::string description() const override {
        return "Submit an implementation plan for user approval. Only available "
               "in Plan mode. After calling this, stop and wait for the user "
               "to approve before making any changes.";
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"plan", {{"type", "string"}, {"description", "Full plan as markdown. Cover context, recommended approach, files to modify (with paths), existing utilities to reuse (with paths), and verification steps."}}}
            }},
            {"required", nlohmann::json::array({"plan"})}
        };
    }
    std::string required_permission() const override { return "propose_plan"; }
    std::string resource(const nlohmann::json& /*input*/, const ToolContext& ctx) const override {
        // Fixed destination under the project's .haicode/plans/ — no user-controlled
        // path component to abuse, so we surface the directory as the resource.
        return ctx.working_dir.empty() ? "." : ctx.working_dir;
    }

    ToolResult execute(const nlohmann::json& input, const ToolContext& ctx) override {
        std::string plan = input.value("plan", "");
        if (plan.empty())
            return {false, "", "propose_plan: missing or empty 'plan'."};

        // Build <project_dir>/.haicode/plans/plan_YYYYMMDD_HHMMSS_<rand>.md
        std::string base = ctx.working_dir.empty() ? "." : ctx.working_dir;
        // Strip trailing slash from base for clean concatenation
        while (!base.empty() && base.back() == '/') base.pop_back();
        std::string plans_dir = base + "/.haicode/plans";

        char ts[32];
        time_t now_t = time(nullptr);
        struct tm tmv;
        localtime_r(&now_t, &tmv);
        strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tmv);

        // 4 hex chars of randomness to avoid collisions when two plans land
        // inside the same second.
        char rand_suffix[8];
        std::snprintf(rand_suffix, sizeof(rand_suffix), "%04x",
                      static_cast<unsigned>(util::now_ms() & 0xFFFF));

        std::string path = plans_dir + "/plan_" + ts + "_" + rand_suffix + ".md";

        std::string err = atomic_write(path,
            "<!-- haicode-status: active -->\n" + plan);
        if (!err.empty())
            return {false, "", err};

        nlohmann::json out = {
            {"path",          path},
            {"bytes_written", plan.size()}
        };
        return {true, out.dump(2), ""};
    }
};

// ---- WriteAgentsMdTool ----
//
// Creates or overwrites agents.md in the project root. The file is
// appended verbatim to HaiCode's system prompt for every session in the
// project, so the agent can self-document the project description, build
// commands, and conventions. Changes take effect from the next session.

class WriteAgentsMdTool : public Tool {
public:
    std::string name() const override { return "write_agents_md"; }
    std::string description() const override {
        return "Create or overwrite agents.md in the project root. "
               "agents.md is appended verbatim to HaiCode's system prompt for "
               "every session in this project — use it to record the project "
               "description, build/run commands, and coding conventions so "
               "future sessions start with full context. "
               "The file takes effect starting from the next session.";
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"content", {
                    {"type", "string"},
                    {"description",
                     "Full markdown content for agents.md. Should include at "
                     "minimum: a # Project section (what the project is), a "
                     "# Build & run section (how to build, test, run), and a "
                     "# Conventions section (style, naming, workflow rules)."}
                }}
            }},
            {"required", nlohmann::json::array({"content"})}
        };
    }
    std::string required_permission() const override { return "write"; }
    std::string resource(const nlohmann::json& /*input*/,
                         const ToolContext& ctx) const override {
        std::string base = ctx.working_dir.empty() ? "." : ctx.working_dir;
        while (!base.empty() && base.back() == '/') base.pop_back();
        return base + "/agents.md";
    }

    ToolResult execute(const nlohmann::json& input,
                       const ToolContext& ctx) override {
        std::string content = input.value("content", "");
        if (content.empty())
            return {false, "", "write_agents_md: content must not be empty."};

        std::string base = ctx.working_dir.empty() ? "." : ctx.working_dir;
        while (!base.empty() && base.back() == '/') base.pop_back();
        std::string path = base + "/agents.md";

        std::string err = atomic_write(path, content);
        if (!err.empty())
            return {false, "", err};

        nlohmann::json out = {
            {"path",          path},
            {"bytes_written", static_cast<int>(content.size())},
            {"note", "agents.md written. It will be included in the system "
                     "prompt starting from the next session."}
        };
        return {true, out.dump(2), ""};
    }
};

// ---- DiscardPlanTool ----
//
// Marks the most recent active plan as discarded so it is no longer
// injected into the system prompt. Use when abandoning a plan without
// implementing it, or after the plan has been fully implemented.

class DiscardPlanTool : public Tool {
public:
    std::string name() const override { return "discard_plan"; }
    std::string description() const override {
        return "Retire the most recent active plan. Call this when the plan "
               "has been fully implemented or when the user wants to abandon "
               "it. Retired plans are no longer injected into future sessions.";
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"reason", {
                    {"type", "string"},
                    {"description", "Brief reason: 'implemented' or 'abandoned'."}
                }}
            }},
            {"required", nlohmann::json::array({"reason"})}
        };
    }
    std::string required_permission() const override { return "write"; }
    std::string resource(const nlohmann::json& /*input*/,
                         const ToolContext& ctx) const override {
        std::string base = ctx.working_dir.empty() ? "." : ctx.working_dir;
        while (!base.empty() && base.back() == '/') base.pop_back();
        return base + "/.haicode/plans/";
    }

    ToolResult execute(const nlohmann::json& input,
                       const ToolContext& ctx) override {
        std::string reason = input.value("reason", "discarded");

        std::string base = ctx.working_dir.empty() ? "." : ctx.working_dir;
        while (!base.empty() && base.back() == '/') base.pop_back();
        std::string plans_dir = base + "/.haicode/plans";

        // Find the latest active plan (lexicographic = chronological).
        DIR* d = opendir(plans_dir.c_str());
        if (!d)
            return {false, "", "discard_plan: no plans directory found."};

        std::string latest_name;
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            std::string n = ent->d_name;
            if (n.size() < 4 || n.substr(n.size() - 3) != ".md") continue;
            if (n <= latest_name) continue;
            // Peek at the status header.
            std::string path = plans_dir + "/" + n;
            std::ifstream f(path);
            std::string first_line;
            if (std::getline(f, first_line) &&
                first_line.find("haicode-status: active") != std::string::npos) {
                latest_name = n;
            }
        }
        closedir(d);

        if (latest_name.empty())
            return {false, "", "discard_plan: no active plan found."};

        std::string path = plans_dir + "/" + latest_name;

        // Read the file, swap the status line, rewrite atomically.
        std::ifstream f(path);
        if (!f.is_open())
            return {false, "", "discard_plan: cannot read " + path};
        std::ostringstream ss;
        ss << f.rdbuf();
        f.close();
        std::string content = ss.str();

        const std::string old_header = "<!-- haicode-status: active -->";
        const std::string new_header = "<!-- haicode-status: " + reason + " -->";
        auto pos = content.find(old_header);
        if (pos != std::string::npos)
            content.replace(pos, old_header.size(), new_header);

        std::string err = atomic_write(path, content);
        if (!err.empty())
            return {false, "", err};

        nlohmann::json out = {{"path", path}, {"status", reason}};
        return {true, out.dump(2), ""};
    }
};

// ---- TodoWriteTool ----
//
// Whole-list replace. The engine detects this tool by name and writes
// the parsed todos to the session_todo table + publishes a TodoUpdated
// event. The tool itself only validates and echoes the input so the
// engine has a structured result to consume.

class TodoWriteTool : public Tool {
public:
    std::string name() const override { return "todo_write"; }
    std::string description() const override {
        return "Replace the task list atomically. Use for multi-step work: "
               "decompose the task, mark exactly one item in_progress at a "
               "time, flip items to completed as you finish them. Send the "
               "FULL list every call — not a delta. An empty list clears "
               "everything.";
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"todos", {
                    {"type", "array"},
                    {"description", "Full todo list. Order is preserved as displayed."},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"content",    {{"type", "string"},  {"description", "Imperative form of the task, e.g. 'Add foo.cpp'."}}},
                            {"activeForm", {{"type", "string"},  {"description", "Present-continuous form for the spinner, e.g. 'Adding foo.cpp'."}}},
                            {"status",     {{"type", "string"},  {"enum", {"pending", "in_progress", "completed"}}}}
                        }},
                        {"required", nlohmann::json::array({"content", "activeForm", "status"})}
                    }}
                }}
            }},
            {"required", nlohmann::json::array({"todos"})}
        };
    }
    std::string required_permission() const override { return "todo_write"; }
    std::string resource(const nlohmann::json& /*input*/, const ToolContext& ctx) const override {
        return ctx.working_dir.empty() ? "." : ctx.working_dir;
    }

    ToolResult execute(const nlohmann::json& input, const ToolContext& /*ctx*/) override {
        if (!input.contains("todos") || !input["todos"].is_array())
            return {false, "", "todo_write: 'todos' must be an array."};

        nlohmann::json echoed = nlohmann::json::array();
        int completed = 0;
        int in_progress = 0;
        int idx = 0;
        for (auto& t : input["todos"]) {
            if (!t.is_object())
                return {false, "", "todo_write: todos[" + std::to_string(idx)
                                  + "] must be an object."};
            std::string status = t.value("status", "");
            if (status != "pending" && status != "in_progress" && status != "completed")
                return {false, "", "todo_write: todos[" + std::to_string(idx)
                                  + "].status must be 'pending', 'in_progress', "
                                  "or 'completed' (got '" + status + "')."};
            std::string content = t.value("content", "");
            if (content.empty())
                return {false, "", "todo_write: todos[" + std::to_string(idx)
                                  + "].content must be non-empty."};

            echoed.push_back({
                {"content",    content},
                {"activeForm", t.value("activeForm", "")},
                {"status",     status}
            });
            if (status == "completed")   ++completed;
            else if (status == "in_progress") ++in_progress;
            ++idx;
        }

        nlohmann::json out = {
            {"todos",     echoed},
            {"count",     echoed.size()},
            {"completed", completed}
        };
        // Surface a single warning line if more than one item is in_progress —
        // the schema allows it but the spec calls for exactly one.
        if (in_progress > 1)
            out["warning"] = "Multiple in_progress items; the spec calls for one at a time.";

        return {true, out.dump(2), ""};
    }
};

// ---- DiffTool ----

class DiffTool : public Tool {
public:
    std::string name() const override { return "diff"; }
    std::string description() const override {
        return "Show a unified diff between the current contents of a file and proposed "
               "new content. Useful for previewing edits before applying them.";
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"path",    {{"type", "string"}, {"description", "File to compare. Absolute or relative to the project directory."}}},
                {"content", {{"type", "string"}, {"description", "Proposed new file contents."}}}
            }},
            {"required", nlohmann::json::array({"path", "content"})}
        };
    }
    std::string required_permission() const override { return "read"; }
    std::string resource(const nlohmann::json& input, const ToolContext& ctx) const override {
        std::string path = input.value("path", "");
        return path.empty() ? ctx.working_dir : resolve_path(path, ctx.working_dir);
    }

    ToolResult execute(const nlohmann::json& input, const ToolContext& ctx) override {
        std::string path = input.value("path", "");
        std::string content = input.value("content", "");
        if (path.empty())
            return {false, "", missing_field("diff", "path", input)};

        path = resolve_path(path, ctx.working_dir);

        // Write proposed content to a temp file
        std::string tmp = path + ".tmp_diff";
        {
            std::ofstream fout(tmp, std::ios::binary);
            if (!fout.is_open())
                return {false, "", "Cannot write temp file: " + tmp + ": " + strerror(errno)};
            fout.write(content.data(), static_cast<std::streamsize>(content.size()));
            if (!fout)
                return {false, "", "Write failed: " + tmp};
        }

        std::string cmd = "diff -u " + sq(path) + " " + sq(tmp) + " 2>&1";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            ::unlink(tmp.c_str());
            return {false, "", "popen failed: " + std::string(strerror(errno))};
        }
        std::string output = read_pipe(pipe);
        int rc = pclose(pipe);
        ::unlink(tmp.c_str());

        int exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
        if (exit_code == 2)
            return {false, "", "diff error: " + output};

        return {true, output.empty() ? "(no differences)" : output, ""};
    }
};

// ---- GitTool ----

class GitTool : public Tool {
    static constexpr const char* kAllowed[] = {
        "status", "diff", "log", "show", "branch", "blame",
        "stash", "add", "commit", "checkout", "reset", "remote",
        "merge", "rebase", "pull", "push", "fetch", "tag",
        "shortlog", "describe", "rev-parse", "ls-files",
    };

    static bool is_allowed(const std::string& sub) {
        for (auto* s : kAllowed) if (sub == s) return true;
        return false;
    }

public:
    std::string name() const override { return "git"; }
    std::string description() const override {
        return "Run a git subcommand (status, diff, log, add, commit, branch, blame, "
               "stash, checkout, reset, remote, merge, rebase, pull, push, fetch, tag, "
               "shortlog, describe, rev-parse, ls-files) in the project directory. "
               "Pass extra flags via the args array.";
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"subcommand", {{"type", "string"}, {"description", "Git subcommand to run."}}},
                {"args",       {{"type", "array"},  {"items", {{"type", "string"}}},
                                {"description", "Additional arguments and flags passed to git after the subcommand."}}}
            }},
            {"required", nlohmann::json::array({"subcommand"})}
        };
    }
    std::string required_permission() const override { return "git"; }
    std::string resource(const nlohmann::json& input, const ToolContext&) const override {
        return input.value("subcommand", "");
    }

    ToolResult execute(const nlohmann::json& input, const ToolContext& ctx) override {
        std::string sub = input.value("subcommand", "");
        if (sub.empty())
            return {false, "", missing_field("git", "subcommand", input)};
        if (!is_allowed(sub))
            return {false, "", "git: subcommand not allowed: " + sub};

        std::string cmd = "git -C " + sq(ctx.working_dir) + " " + sub;
        if (input.contains("args") && input["args"].is_array()) {
            for (const auto& a : input["args"]) {
                if (a.is_string()) cmd += " " + sq(a.get<std::string>());
            }
        }
        cmd += " 2>&1";

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe)
            return {false, "", "popen failed: " + std::string(strerror(errno))};
        std::string output = read_pipe(pipe);
        int rc = pclose(pipe);
        int exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;

        if (exit_code != 0 && output.empty())
            return {false, "", "git exited with code " + std::to_string(exit_code)};

        return {exit_code == 0, output, exit_code == 0 ? "" : output};
    }
};

// ---- FindTool ----

class FindTool : public Tool {
public:
    std::string name() const override { return "find"; }
    std::string description() const override {
        return "Recursively search for files and directories. Supports filtering by name "
               "pattern, type (f/d/l), max depth, modification time (mtime), and size. "
               "Use this instead of glob when you need recursive matching or time/size filters.";
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"path",     {{"type", "string"}, {"description", "Directory to search. Defaults to the project directory."}}},
                {"name",     {{"type", "string"}, {"description", "Filename glob pattern, e.g. \"*.cpp\"."}}},
                {"type",     {{"type", "string"}, {"enum", {"f", "d", "l"}},
                              {"description", "Limit to files (f), directories (d), or symlinks (l)."}}},
                {"maxdepth", {{"type", "integer"}, {"description", "Maximum directory depth to descend."}}},
                {"mtime",    {{"type", "string"}, {"description", "Filter by modification time in days, e.g. \"-1\" = modified in last day, \"+7\" = older than 7 days."}}},
                {"size",     {{"type", "string"}, {"description", "Filter by size, e.g. \"+1M\" = larger than 1 MB, \"-100k\" = smaller than 100 KB."}}}
            }},
            {"required", nlohmann::json::array()}
        };
    }
    std::string required_permission() const override { return "read"; }
    std::string resource(const nlohmann::json& input, const ToolContext& ctx) const override {
        std::string path = input.value("path", "");
        return path.empty() ? ctx.working_dir : resolve_path(path, ctx.working_dir);
    }

    ToolResult execute(const nlohmann::json& input, const ToolContext& ctx) override {
        std::string path = input.value("path", "");
        path = path.empty() ? ctx.working_dir : resolve_path(path, ctx.working_dir);

        std::string cmd = "find " + sq(path);

        if (input.contains("maxdepth") && input["maxdepth"].is_number_integer())
            cmd += " -maxdepth " + std::to_string(input["maxdepth"].get<int>());

        if (input.contains("type") && input["type"].is_string()) {
            std::string t = input["type"].get<std::string>();
            if (t == "f" || t == "d" || t == "l")
                cmd += " -type " + t;
        }

        if (input.contains("name") && input["name"].is_string())
            cmd += " -name " + sq(input["name"].get<std::string>());

        if (input.contains("mtime") && input["mtime"].is_string())
            cmd += " -mtime " + sq(input["mtime"].get<std::string>());

        if (input.contains("size") && input["size"].is_string())
            cmd += " -size " + sq(input["size"].get<std::string>());

        cmd += " 2>&1";

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe)
            return {false, "", "popen failed: " + std::string(strerror(errno))};
        std::string output = read_pipe(pipe);
        int rc = pclose(pipe);
        int exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;

        if (exit_code != 0)
            return {false, "", "find error: " + output};

        return {true, output.empty() ? "(no matches)" : output, ""};
    }
};

// Registration function
void register_builtin_tools(ToolRegistry& registry) {
    registry.register_tool(std::make_shared<BashTool>());
    registry.register_tool(std::make_shared<ReadTool>());
    registry.register_tool(std::make_shared<WriteTool>());
    registry.register_tool(std::make_shared<EditTool>());
    registry.register_tool(std::make_shared<GlobTool>());
    registry.register_tool(std::make_shared<GrepTool>());
    registry.register_tool(std::make_shared<LsTool>());
    registry.register_tool(std::make_shared<ExternalTerminalTool>());
    registry.register_tool(std::make_shared<ProposePlanTool>());
    registry.register_tool(std::make_shared<DiscardPlanTool>());
    registry.register_tool(std::make_shared<WriteAgentsMdTool>());
    registry.register_tool(std::make_shared<TodoWriteTool>());
    registry.register_tool(std::make_shared<DiffTool>());
    registry.register_tool(std::make_shared<GitTool>());
    registry.register_tool(std::make_shared<FindTool>());
    // web_search and web_extract live in web_tools.cpp; pull them in through
    // their own registration entry point so this file doesn't need to know
    // about HttpClient.
    register_web_tools(registry);
}

} // namespace haicode
