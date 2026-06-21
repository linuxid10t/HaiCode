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
#include <regex>
#include <set>
#include <unistd.h>
#include <glob.h>
#include <fnmatch.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <csignal>

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

// ---- symbols tool helpers ----

static const std::set<std::string> g_source_exts = {
    ".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hxx", ".hh"
};

static bool has_source_ext(const std::string& path) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return g_source_exts.count(ext) != 0;
}

// Recursively collect source files under `root`. If `include_glob` is non-empty
// it overrides the extension filter via fnmatch. Skips paths containing /build/
// or /.git/. Uses opendir/readdir (same primitive as the rest of the file).
static void walk_source_files(const std::string& root,
                              const std::string& include_glob,
                              std::vector<std::string>& out) {
    struct stat rst{};
    if (stat(root.c_str(), &rst) != 0) return;
    // Single-file root: accept it directly (extension/glob filtered).
    if (S_ISREG(rst.st_mode)) {
        std::string base = root;
        size_t slash = base.rfind('/');
        std::string name = (slash == std::string::npos) ? base : base.substr(slash + 1);
        if (!include_glob.empty()) {
            if (fnmatch(include_glob.c_str(), name.c_str(), 0) != 0) return;
        } else if (!has_source_ext(name)) {
            return;
        }
        out.push_back(root);
        return;
    }
    DIR* dir = opendir(root.c_str());
    if (!dir) return;
    struct dirent* ent;
    std::vector<std::string> entries;
    while ((ent = readdir(dir)) != nullptr) {
        std::string n = ent->d_name;
        if (n == "." || n == "..") continue;
        entries.push_back(n);
    }
    closedir(dir);
    std::sort(entries.begin(), entries.end());
    for (const std::string& n : entries) {
        std::string full = root;
        if (full.back() != '/') full += '/';
        full += n;
        struct stat st{};
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            // Skip build dirs and version control
            if (n == "build" || n == ".git") continue;
            if (full.find("/build/") != std::string::npos) continue;
            if (full.find("/.git/") != std::string::npos) continue;
            walk_source_files(full, include_glob, out);
        } else if (S_ISREG(st.st_mode)) {
            if (!include_glob.empty()) {
                if (fnmatch(include_glob.c_str(), n.c_str(), 0) != 0) continue;
            } else if (!has_source_ext(n)) {
                continue;
            }
            out.push_back(full);
        }
    }
}

// Tokenizer state that persists across lines (block comments / strings can
// span multiple lines).
struct TokenizerState {
    bool in_block_comment = false;
    bool in_string = false;
};

// Return the CODE-only substring of `line` — comments and string/char literals
// are blanked out (replaced with spaces so column offsets are preserved). The
// `state` struct carries block-comment / string state across lines.
static std::string strip_non_code(const std::string& line, TokenizerState& state) {
    std::string out = line;
    for (size_t i = 0; i < out.size(); i++) {
        if (state.in_block_comment) {
            if (i + 1 < out.size() && out[i] == '*' && out[i + 1] == '/') {
                out[i] = ' ';
                out[i + 1] = ' ';
                state.in_block_comment = false;
                i++;  // skip the '/'
            } else {
                out[i] = ' ';
            }
            continue;
        }
        if (state.in_string) {
            if (out[i] == '\\' && i + 1 < out.size()) {
                // escape: blank both chars
                out[i] = ' ';
                out[i + 1] = ' ';
                i++;
                continue;
            }
            if (out[i] == '"') {
                out[i] = ' ';
                state.in_string = false;
            } else {
                out[i] = ' ';
            }
            continue;
        }
        // Not in block comment or string.
        if (out[i] == '/' && i + 1 < out.size() && out[i + 1] == '/') {
            // line comment — blank to EOL
            for (size_t j = i; j < out.size(); j++) out[j] = ' ';
            return out;
        }
        if (out[i] == '/' && i + 1 < out.size() && out[i + 1] == '*') {
            out[i] = ' ';
            out[i + 1] = ' ';
            state.in_block_comment = true;
            i++;
            continue;
        }
        if (out[i] == '"') {
            out[i] = ' ';
            state.in_string = true;
            continue;
        }
        if (out[i] == '\'') {
            // char literal — blank until closing unescaped quote
            out[i] = ' ';
            size_t j = i + 1;
            while (j < out.size()) {
                if (out[j] == '\\' && j + 1 < out.size()) {
                    out[j] = ' ';
                    out[j + 1] = ' ';
                    j += 2;
                    continue;
                }
                if (out[j] == '\'') {
                    out[j] = ' ';
                    break;
                }
                out[j] = ' ';
                j++;
            }
            i = j;
            continue;
        }
    }
    return out;
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
    f.clear();   // read() past EOF sets eofbit; clear it before seeking back
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
                {"pattern",      {{"type", "string"}, {"description", "Regular expression (ERE/extended syntax) to search for. Passed to /bin/grep -E -e. Supports alternation (A|B), +, ?, {n,m}."}}},
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
        std::string cmd = "/bin/grep -r -E --color=never";
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

// ---- AskUserTool ----
//
// Asks the user a focused disambiguation question with 2-5 preset
// options. The tool itself is synchronous and only records intent as
// JSON output — the engine detects it by name, publishes AskUserRequested,
// ends the turn, and blocks until the UI replies. The real answer appears
// as a subsequent tool_result replacement.

class AskUserTool : public Tool {
public:
    std::string name() const override { return "ask_user"; }
    std::string description() const override {
        return "Ask the user a focused disambiguation question when needed to "
               "resolve ambiguity. Supply 2-5 preset options; the user may also "
               "type a custom reply. The answer flows back as a tool result.";
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"question", {{"type", "string"}, {"description", "The question text."}}},
                {"options", {
                    {"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"description", "2-5 preset choices."}
                }}
            }},
            {"required", nlohmann::json::array({"question", "options"})}
        };
    }
    std::string required_permission() const override { return "ask_user"; }
    std::string resource(const nlohmann::json& /*input*/, const ToolContext& ctx) const override {
        return ctx.working_dir.empty() ? "." : ctx.working_dir;
    }

    ToolResult execute(const nlohmann::json& input, const ToolContext& /*ctx*/) override {
        std::string question = input.value("question", "");
        if (question.empty())
            return {false, "", missing_field("ask_user", "question", input)};

        if (!input.contains("options") || !input["options"].is_array())
            return {false, "", "ask_user: 'options' must be an array of strings."};

        std::vector<std::string> options;
        int idx = 0;
        for (const auto& o : input["options"]) {
            if (!o.is_string())
                return {false, "", "ask_user: options[" + std::to_string(idx)
                                  + "] must be a string."};
            std::string s = o.get<std::string>();
            if (s.empty())
                return {false, "", "ask_user: options[" + std::to_string(idx)
                                  + "] is empty."};
            options.push_back(s);
            ++idx;
        }
        if (options.size() < 2)
            return {false, "", "ask_user: 'options' must contain at least 2 choices."};
        if (options.size() > 5)
            return {false, "", "ask_user: 'options' may have at most 5 choices."};

        // Echo the request so the engine has the structured payload for the
        // AskUserRequested event. The real answer comes via tool_result
        // replacement after the UI replies.
        nlohmann::json out = {
            {"question", question},
            {"options",  options},
            {"status",   "asking"}
        };
        return {true, out.dump(2), ""};
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

// ---- SymbolsTool ----

namespace {

struct SymbolHit {
    enum class Kind { Definition, Call, MemberAccess, Declaration, Mention };
    Kind kind;
    std::string file;
    int line;
    std::string text;            // trimmed CODE-only line
    std::string cls;             // enclosing class, or "<global>"
    const char* matched_pattern; // which branch in classify() fired
};

const char* kind_str(SymbolHit::Kind k) {
    switch (k) {
        case SymbolHit::Kind::Definition:     return "definition";
        case SymbolHit::Kind::Call:           return "call";
        case SymbolHit::Kind::MemberAccess:   return "member_access";
        case SymbolHit::Kind::Declaration:    return "declaration";
        case SymbolHit::Kind::Mention:        return "mention";
    }
    return "mention";
}

// Trim leading/trailing whitespace.
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Does a CODE-only line declare/define `name` as a class/struct?
bool is_class_def(const std::string& code, const std::string& name) {
    static const std::regex re(R"(^(class|struct)\s+)" + name + R"(\b)");
    return std::regex_search(code, re);
}

// Does a CODE-only line define `name` as a method on `cls`?
bool is_method_def(const std::string& code, const std::string& name, const std::string& cls) {
    if (cls == "<global>") return false;
    std::string esc = std::regex_replace(name + "::" + name, std::regex(R"(::)"), "::");
    (void)esc;
    // Match  ClassName::name(  possibly with whitespace.
    static const std::regex id_re(R"([\w:&*<>,\s]+)");
    std::string pat = R"(^\w[\w:&*<>,\s]*\s+)" + cls + "::" + name + R"(\s*\()" ;
    try {
        std::regex re(pat);
        return std::regex_search(code, re);
    } catch (...) { return false; }
}

// Does a CODE-only line define `name` as a free function (depth 0, no class)?
// Requires a real return type (an identifier char before whitespace+name) and
// rejects qualified names (`::name`) — those are calls, not definitions.
bool is_free_func_def(const std::string& code, const std::string& name) {
    // Must start with an identifier char (return type), not whitespace/punct.
    std::string pat = R"(^\w[\w:&*<>,\s]*\s+)" + name + R"(\s*\()" ;
    try {
        std::regex re(pat);
        if (!std::regex_search(code, re)) return false;
    } catch (...) { return false; }
    // Reject if the name is qualified (Class::name) — that's a call/def handled
    // by is_method_def, not a free function.
    std::string qual = "::" + name + R"(\s*\()" ;
    try {
        std::regex qre(qual);
        if (std::regex_search(code, qre)) return false;
    } catch (...) {}
    return true;
}

// Does a CODE-only line declare `name` as a member field of a class?
bool is_member_field(const std::string& code, const std::string& name) {
    std::string pat = R"(^\s+[\w:&*<>\[\]]+\s+)" + name + R"(\s*[;=])" ;
    try {
        std::regex re(pat);
        return std::regex_search(code, re);
    } catch (...) { return false; }
}

// Does a CODE-only line declare `name` as a typedef/using?
bool is_typedef_def(const std::string& code, const std::string& name) {
    static const std::regex re(R"(^\s*(typedef|using)\s+)" + name + R"(\b)");
    return std::regex_search(code, re);
}

// Does a CODE-only line define `name` as a method (ClassName::name) at any
// scope? Detects structurally by the qualified-name pattern.
bool is_qualified_method_def(const std::string& code, const std::string& name) {
    // ^ <return-type> ClassName::name(  — return type optional but if present
    // must start with a word char (not pure whitespace, which would be a call).
    std::string pat = std::string(R"(^\w[\w:&*<>,\s]*\s+)") + R"(\w+::)" + name + R"(\s*\()" ;
    try {
        std::regex re(pat);
        if (std::regex_search(code, re)) return true;
    } catch (...) {}
    // also: ClassName::name( with no return type (ctor/dtor/operator) at col 0
    pat = R"(^\w+::)" + name + R"(\s*\()" ;
    try {
        std::regex re(pat);
        return std::regex_search(code, re);
    } catch (...) { return false; }
}

// Does a CODE-only line declare `name` as a method prototype inside a class
// body? Pattern: indented RetType name( ... ) ;  (no ::, no { body).
bool is_method_decl(const std::string& code, const std::string& name) {
    std::string pat = R"(^\s+\w[\w:&*<>,\s]*\s+)" + name + R"(\s*\()" ;
    try {
        std::regex re(pat);
        if (!std::regex_search(code, re)) return false;
    } catch (...) { return false; }
    // Must end with ';' (prototype), not '{' (inline def, which is a def).
    std::string trimmed = code;
    while (!trimmed.empty() && isspace(trimmed.back())) trimmed.pop_back();
    if (trimmed.empty() || trimmed.back() != ';') return false;
    // Reject qualified names — those are out-of-class definitions.
    try {
        std::regex qre("::" + name + R"(\s*\()");
        if (std::regex_search(code, qre)) return false;
    } catch (...) {}
    return true;
}

// Determine the kind of a hit on `name` in a CODE-only line.
// Returns {kind, pattern_name} where pattern_name identifies which branch fired.
std::pair<SymbolHit::Kind, const char*> classify(const std::string& code,
                                                   const std::string& name,
                                                   const std::string& cls) {
    if (is_class_def(code, name))            return {SymbolHit::Kind::Definition, "class_def"};
    if (is_qualified_method_def(code, name)) return {SymbolHit::Kind::Definition, "qualified_method_def"};
    if (is_method_def(code, name, cls))      return {SymbolHit::Kind::Definition, "method_def"};
    if (cls == "<global>" && is_free_func_def(code, name))
                                             return {SymbolHit::Kind::Definition, "free_func_def"};
    if (is_typedef_def(code, name))          return {SymbolHit::Kind::Definition, "typedef_def"};
    if (cls != "<global>" && is_member_field(code, name))
                                             return {SymbolHit::Kind::Declaration, "member_field"};
    if (cls != "<global>" && is_method_decl(code, name))
                                             return {SymbolHit::Kind::Declaration, "method_decl"};
    // usage detection
    // call: name( possibly preceded by :: . -> or start/space
    static const std::regex call_re(R"((::|\.|->|^|[\s(,.]))" + name + R"(\s*\()");
    if (std::regex_search(code, call_re))    return {SymbolHit::Kind::Call, "call_re"};
    // member access via explicit operator: .name ->name ::name
    static const std::regex mem_re(R"((\.|->|::))" + name + R"(\b)");
    if (std::regex_search(code, mem_re))     return {SymbolHit::Kind::MemberAccess, "mem_re"};
    // name used as an object: name. or name-> (accessing a sub-member)
    try {
        std::regex obj_re("\\b" + name + R"(\s*(\.|->))");
        if (std::regex_search(code, obj_re)) return {SymbolHit::Kind::MemberAccess, "obj_re"};
    } catch (...) {}
    return {SymbolHit::Kind::Mention, "mention_fallthrough"};
}

// Find the enclosing method name for the (in Foo::bar) annotation by scanning
// backward for a method-definition header that opens the scope containing idx.
// Conservative: only returns qualified method names (Class::method), since free
// function headers are too easily confused with calls/control flow. Returns ""
// when no confident enclosing method is found.
std::string enclosing_method(const std::vector<std::string>& lines, int idx) {
    int depth = 0;  // net unclosed '{' between idx and the current scan line
    for (int i = idx; i >= 0; i--) {
        const std::string& l = lines[i];
        int opens = 0, closes = 0;
        for (char c : l) {
            if (c == '{') opens++;
            else if (c == '}') closes++;
        }
        // A method definition header that opens a scope covering idx: it has
        // more '{' than '}' and, scanning back from idx, we haven't closed it.
        static const std::regex method_hdr(
            R"(^\w[\w:&*<>,\s]*\s+(\w+)::(\w+)\s*\([^)]*\)\s*(const)?\s*\{?)");
        std::smatch m;
        if (std::regex_search(l, m, method_hdr) && opens > closes &&
            depth - (opens - closes) < 0) {
            return std::string(m[1]) + "::" + std::string(m[2]);
        }
        depth += opens - closes;
    }
    return "";
}

} // namespace

class SymbolsTool : public Tool {
public:
    std::string name() const override { return "symbols"; }
    std::string description() const override {
        return "Find C/C++ symbol definitions and references. Skips comments/strings "
               "and classifies hits. Faster than grep for tracing fields and functions. "
               "query: \"definition\", \"references\", or \"callers\".";
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"name",    {{"type", "string"}, {"description", "Symbol name to search for."}}},
                {"query",   {{"type", "string"}, {"enum", {"definition", "references", "callers"}},
                             {"description", "\"definition\": find where name is defined. \"references\": every occurrence with classification. \"callers\": usage sites only (call + member_access)."}}},
                {"path",    {{"type", "string"}, {"description", "File or directory to scope the search. Absolute, or relative to the project directory. Default: project directory."}}},
                {"include", {{"type", "string"}, {"description", "Filename glob filter overriding the default source-extension set, e.g. '*.cpp'."}}},
                {"verbose", {{"type", "boolean"}, {"description", "When true, appends [matched: <pattern>] to each hit line showing which regex branch produced the classification. Useful for debugging misclassifications."}}}
            }},
            {"required", nlohmann::json::array({"name", "query"})}
        };
    }
    std::string required_permission() const override { return "read"; }
    std::string resource(const nlohmann::json& input, const ToolContext& ctx) const override {
        if (input.contains("path") && input["path"].is_string()) {
            std::string p = input["path"].get<std::string>();
            if (!p.empty()) return resolve_path(p, ctx.working_dir);
        }
        return ctx.working_dir.empty() ? "." : ctx.working_dir;
    }

    ToolResult execute(const nlohmann::json& input, const ToolContext& ctx) override {
        std::string sym = input.value("name", "");
        if (sym.empty())
            return {false, "", missing_field("symbols", "name", input)};
        std::string query = input.value("query", "");
        if (query != "definition" && query != "references" && query != "callers")
            return {false, "", missing_field("symbols", "query", input)};
        bool verbose = input.value("verbose", false);

        std::string root;
        if (input.contains("path") && input["path"].is_string()) {
            std::string p = input["path"].get<std::string>();
            root = p.empty() ? ctx.working_dir : resolve_path(p, ctx.working_dir);
        } else {
            root = ctx.working_dir.empty() ? "." : ctx.working_dir;
        }
        std::string include_glob = input.value("include", "");

        std::vector<std::string> files;
        walk_source_files(root, include_glob, files);

        const size_t MAX_HITS = 200;
        std::vector<SymbolHit> hits;
        std::regex name_re("\\b" + sym + "\\b");

        for (const std::string& file : files) {
            std::ifstream f(file);
            if (!f.is_open()) continue;
            std::vector<std::string> lines;
            std::string l;
            while (std::getline(f, l)) lines.push_back(l);

            TokenizerState tstate;
            int brace_depth = 0;
            std::vector<std::pair<int, std::string>> class_stack;  // (depth, name)

            for (size_t i = 0; i < lines.size(); i++) {
                std::string code = strip_non_code(lines[i], tstate);
                std::string cls = class_stack.empty() ? "<global>" : class_stack.back().second;

                // Track class/struct scope entry.
                std::smatch cm;
                static const std::regex class_open(R"(^(class|struct)\s+(\w+))");
                if (std::regex_search(code, cm, class_open)) {
                    std::string cname = cm[2];
                    if (cm[1] == "sym" && cm[2] == sym) {
                        // handled below as a hit
                    }
                    class_stack.push_back({brace_depth, cname});
                }

                // Count braces in CODE-only text for scope tracking.
                if (!std::regex_search(code, name_re)) {
                    for (char c : code) {
                        if (c == '{') brace_depth++;
                        else if (c == '}') {
                            brace_depth--;
                            while (!class_stack.empty() && class_stack.back().first >= brace_depth)
                                class_stack.pop_back();
                        }
                    }
                    continue;
                }

                // We have a hit — classify it.
                auto [k, kpat] = classify(code, sym, cls);

                // For definition queries keep definitions + declarations; for
                // callers keep only call + member_access.
                if (query == "definition" &&
                    k != SymbolHit::Kind::Definition &&
                    k != SymbolHit::Kind::Declaration)
                    k = SymbolHit::Kind::Mention;  // dropped below
                if (query == "callers" &&
                    k != SymbolHit::Kind::Call &&
                    k != SymbolHit::Kind::MemberAccess) {
                    // still need brace tracking; skip recording
                    for (char c : code) {
                        if (c == '{') brace_depth++;
                        else if (c == '}') {
                            brace_depth--;
                            while (!class_stack.empty() && class_stack.back().first >= brace_depth)
                                class_stack.pop_back();
                        }
                    }
                    continue;
                }
                if (query == "definition" && k == SymbolHit::Kind::Mention) {
                    for (char c : code) {
                        if (c == '{') brace_depth++;
                        else if (c == '}') {
                            brace_depth--;
                            while (!class_stack.empty() && class_stack.back().first >= brace_depth)
                                class_stack.pop_back();
                        }
                    }
                    continue;
                }

                SymbolHit h;
                h.kind = k;
                h.matched_pattern = kpat;
                h.file = file;
                h.line = static_cast<int>(i + 1);
                h.text = trim(code);
                h.cls  = cls;
                if (k == SymbolHit::Kind::Call || k == SymbolHit::Kind::MemberAccess) {
                    std::string m = enclosing_method(lines, static_cast<int>(i));
                    if (!m.empty()) h.cls = m;  // render as (in method)
                }
                hits.push_back(h);
                if (hits.size() >= MAX_HITS) break;

                for (char c : code) {
                    if (c == '{') brace_depth++;
                    else if (c == '}') {
                        brace_depth--;
                        while (!class_stack.empty() && class_stack.back().first >= brace_depth)
                            class_stack.pop_back();
                    }
                }
            }
            if (hits.size() >= MAX_HITS) break;
        }

        // Sort by (file, line).
        std::sort(hits.begin(), hits.end(), [](const SymbolHit& a, const SymbolHit& b) {
            if (a.file != b.file) return a.file < b.file;
            return a.line < b.line;
        });

        std::set<std::string> files_hit;
        std::string out;
        for (const auto& h : hits) {
            files_hit.insert(h.file);
            out += h.file + ":" + std::to_string(h.line);
            // pad kind column
            std::string k = kind_str(h.kind);
            while (k.size() < 12) k += ' ';
            out += "   " + k;
            if (h.kind == SymbolHit::Kind::Definition) {
                out += "   " + h.text;
            } else if (h.kind == SymbolHit::Kind::Call || h.kind == SymbolHit::Kind::MemberAccess) {
                if (!h.cls.empty() && h.cls != "<global>") out += "   (in " + h.cls + ")";
            }
            if (verbose) out += "   [matched: " + std::string(h.matched_pattern) + "]";
            out += "\n";
        }
        if (hits.size() >= MAX_HITS)
            out += "[truncated]\n";
        out += std::to_string(hits.size()) + " hits in " +
               std::to_string(files_hit.size()) + " files";

        if (hits.empty())
            return {true, "No symbols found for '" + sym + "' (" + query + ")", ""};
        return {true, out, ""};
    }
};

// ---- ProcessTool ----

class ProcessTool : public Tool {
    static int parse_signal(const std::string& name) {
        if (name == "TERM" || name == "15") return SIGTERM;
        if (name == "KILL" || name == "9")  return SIGKILL;
        if (name == "HUP"  || name == "1")  return SIGHUP;
        if (name == "INT"  || name == "2")  return SIGINT;
        if (name == "USR1" || name == "10") return SIGUSR1;
        if (name == "USR2" || name == "12") return SIGUSR2;
        if (name == "STOP" || name == "19") return SIGSTOP;
        if (name == "CONT" || name == "18") return SIGCONT;
        return -1;
    }

public:
    std::string name() const override { return "process"; }
    std::string description() const override {
        return "Inspect and manage running processes. "
               "Actions: \"list\" — show running processes (optional \"filter\" substring match on name); "
               "\"kill\" — send a signal to a process by \"pid\" (default signal: TERM; "
               "supported: TERM, KILL, HUP, INT, USR1, USR2, STOP, CONT); "
               "\"check_port\" — show which process (if any) is listening on \"port\".";
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"action", {{"type", "string"}, {"enum", {"list", "kill", "check_port"}},
                            {"description", "Action to perform."}}},
                {"filter", {{"type", "string"}, {"description", "Substring filter on process name (list only)."}}},
                {"pid",    {{"type", "integer"}, {"description", "Process ID to signal (kill only)."}}},
                {"signal", {{"type", "string"}, {"description", "Signal name or number (kill only, default: TERM)."}}},
                {"port",   {{"type", "integer"}, {"description", "TCP/UDP port number (check_port only)."}}}
            }},
            {"required", nlohmann::json::array({"action"})}
        };
    }
    std::string required_permission() const override { return "process"; }
    std::string resource(const nlohmann::json& input, const ToolContext&) const override {
        std::string action = input.value("action", "");
        if (action == "kill" && input.contains("pid"))
            return "pid:" + std::to_string(input["pid"].get<int>());
        if (action == "check_port" && input.contains("port"))
            return "port:" + std::to_string(input["port"].get<int>());
        return action;
    }

    ToolResult execute(const nlohmann::json& input, const ToolContext& ctx) override {
        std::string action = input.value("action", "");
        if (action.empty())
            return {false, "", missing_field("process", "action", input)};

        if (action == "list") {
            std::string filter = input.value("filter", "");
            FILE* pipe = popen("ps 2>&1", "r");
            if (!pipe)
                return {false, "", "popen failed: " + std::string(strerror(errno))};
            std::string raw = read_pipe(pipe);
            pclose(pipe);

            if (filter.empty())
                return {true, raw, ""};

            // Keep the header line plus any line containing the filter string
            std::string header, out;
            std::istringstream ss(raw);
            std::string line;
            bool first = true;
            while (std::getline(ss, line)) {
                if (first) { header = line + "\n"; first = false; continue; }
                if (line.find(filter) != std::string::npos)
                    out += line + "\n";
            }
            return {true, out.empty() ? "(no matching processes)" : header + out, ""};
        }

        if (action == "kill") {
            if (!input.contains("pid") || !input["pid"].is_number_integer())
                return {false, "", "process kill: \"pid\" (integer) is required"};
            int pid = input["pid"].get<int>();
            std::string sig_name = input.value("signal", "TERM");
            int sig = parse_signal(sig_name);
            if (sig < 0)
                return {false, "", "process kill: unknown signal: " + sig_name};
            if (::kill(static_cast<pid_t>(pid), sig) != 0)
                return {false, "", "kill(" + std::to_string(pid) + ", " + sig_name + ") failed: " + strerror(errno)};
            return {true, "Signal " + sig_name + " sent to pid " + std::to_string(pid), ""};
        }

        if (action == "check_port") {
            if (!input.contains("port") || !input["port"].is_number_integer())
                return {false, "", "process check_port: \"port\" (integer) is required"};
            int port = input["port"].get<int>();
            FILE* pipe = popen("netstat -n 2>&1", "r");
            if (!pipe)
                return {false, "", "popen failed: " + std::string(strerror(errno))};
            std::string raw = read_pipe(pipe);
            pclose(pipe);

            std::string port_str = ":" + std::to_string(port);
            std::string out;
            std::istringstream ss(raw);
            std::string line;
            bool first = true;
            while (std::getline(ss, line)) {
                if (first) { out += line + "\n"; first = false; continue; }
                if (line.find(port_str) != std::string::npos)
                    out += line + "\n";
            }
            return {true, out.size() <= (out.find('\n') + 1) ? "Nothing listening on port " + std::to_string(port) : out, ""};
        }

        return {false, "", "process: unknown action: " + action};
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
    registry.register_tool(std::make_shared<AskUserTool>());
    registry.register_tool(std::make_shared<DiffTool>());
    registry.register_tool(std::make_shared<GitTool>());
    registry.register_tool(std::make_shared<FindTool>());
    registry.register_tool(std::make_shared<SymbolsTool>());
    registry.register_tool(std::make_shared<ProcessTool>());
    // web_search and web_extract live in web_tools.cpp; pull them in through
    // their own registration entry point so this file doesn't need to know
    // about HttpClient.
    register_web_tools(registry);
}

} // namespace haicode
