#include <haicode/tool.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <array>
#include <memory>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <glob.h>
#include <fnmatch.h>
#include <sys/wait.h>
#include <sys/stat.h>

namespace haicode {

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
                {"command", {{"type", "string"}, {"description", "The bash command to run"}}},
                {"timeout", {{"type", "integer"}, {"description", "Timeout in seconds (default 30)"}}}
            }},
            {"required", nlohmann::json::array({"command"})}
        };
    }

    ToolResult execute(const nlohmann::json& input, const ToolContext& ctx) override {
        std::string command = input.value("command", "");
        if (command.empty())
            return {false, "", "No command provided"};

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
                {"path",   {{"type", "string"}}},
                {"offset", {{"type", "integer"}, {"description", "First line to read (1-based, default 1)"}}},
                {"limit",  {{"type", "integer"}, {"description", "Max lines to read (default unlimited)"}}}
            }},
            {"required", nlohmann::json::array({"path"})}
        };
    }
    std::string required_permission() const override { return "read"; }

    ToolResult execute(const nlohmann::json& input, const ToolContext& ctx) override {
        std::string path = input.value("path", "");
        if (path.empty())
            return {false, "", "No path provided"};

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
                {"path",    {{"type", "string"}}},
                {"content", {{"type", "string"}}}
            }},
            {"required", nlohmann::json::array({"path", "content"})}
        };
    }
    std::string required_permission() const override { return "write"; }

    ToolResult execute(const nlohmann::json& input, const ToolContext& ctx) override {
        std::string path = input.value("path", "");
        std::string content = input.value("content", "");
        if (path.empty())
            return {false, "", "No path provided"};

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
                {"pattern", {{"type", "string"}, {"description", "Glob pattern (no ** support)"}}},
                {"path",    {{"type", "string"}, {"description", "Base directory (default: working dir)"}}}
            }},
            {"required", nlohmann::json::array({"pattern"})}
        };
    }

    ToolResult execute(const nlohmann::json& input, const ToolContext& ctx) override {
        std::string pattern = input.value("pattern", "");
        if (pattern.empty())
            return {false, "", "No pattern provided"};

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
                {"pattern",      {{"type", "string"}}},
                {"path",         {{"type", "string"}}},
                {"include",      {{"type", "string"}, {"description", "Filename glob filter, e.g. '*.cpp'"}}},
                {"line_numbers", {{"type", "boolean"}, {"description", "Show line numbers (default true)"}}}
            }},
            {"required", nlohmann::json::array({"pattern"})}
        };
    }

    ToolResult execute(const nlohmann::json& input, const ToolContext& ctx) override {
        std::string pattern = input.value("pattern", "");
        if (pattern.empty())
            return {false, "", "No pattern provided"};

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

// Registration function
void register_builtin_tools(ToolRegistry& registry) {
    registry.register_tool(std::make_shared<BashTool>());
    registry.register_tool(std::make_shared<ReadTool>());
    registry.register_tool(std::make_shared<WriteTool>());
    registry.register_tool(std::make_shared<GlobTool>());
    registry.register_tool(std::make_shared<GrepTool>());
}

} // namespace haicode
