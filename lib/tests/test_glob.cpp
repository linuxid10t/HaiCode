// GlobTool: `**` globstar expansion (POSIX glob() reads `**` as a plain `*`,
// so `src/**/*.cpp` used to match only src/<one dir>/*.cpp with no error),
// hidden/symlinked directory handling, unchanged single-segment patterns,
// interrupt, and the in-project gate bypass for globstar patterns.
#include <haicode/haicode.h>
#include <haicode/tool.h>
#include "test_check.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
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

static std::string root;     // project tree
static std::string outside;  // target of the in-project symlink

static void touch(const std::string& path) {
    std::ofstream f(path);
    TEST_REQUIRE(f.is_open(), "failed to create " + path);
    f << "x\n";
}

static void mkdir_p(const std::string& path) {
    TEST_REQUIRE(::mkdir(path.c_str(), 0755) == 0, "mkdir failed: " + path);
}

// root/
//   a.cpp  CMakeLists.txt
//   src/b.cpp  src/.dotfile.cpp
//   src/deep/c.cpp  src/deep/d.h  src/deep/CMakeLists.txt
//   .hidden/e.cpp
//   link -> outside/   (outside/f.cpp)
static void build_tree() {
    char tmpl[] = "/tmp/tglob_XXXXXX";
    TEST_REQUIRE(mkdtemp(tmpl), "mkdtemp failed");
    root = tmpl;
    char tmpl2[] = "/tmp/tglob_out_XXXXXX";
    TEST_REQUIRE(mkdtemp(tmpl2), "mkdtemp failed");
    outside = tmpl2;

    touch(root + "/a.cpp");
    touch(root + "/CMakeLists.txt");
    mkdir_p(root + "/src");
    touch(root + "/src/b.cpp");
    touch(root + "/src/.dotfile.cpp");
    mkdir_p(root + "/src/deep");
    touch(root + "/src/deep/c.cpp");
    touch(root + "/src/deep/d.h");
    touch(root + "/src/deep/CMakeLists.txt");
    mkdir_p(root + "/.hidden");
    touch(root + "/.hidden/e.cpp");
    touch(outside + "/f.cpp");
    TEST_REQUIRE(::symlink(outside.c_str(), (root + "/link").c_str()) == 0,
                 "symlink failed");
}

static void remove_tree() {
    std::system(("rm -rf '" + root + "' '" + outside + "'").c_str());
}

static ToolContext ctx() {
    ToolContext c;
    c.working_dir = root;
    return c;
}

static std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line))
        if (!line.empty()) out.push_back(line);
    return out;
}

static std::string show(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += "\n  " + x;
    return s.empty() ? " (none)" : s;
}

// Run glob and compare against `expected` (root-relative, sorted).
static bool expect(const json& input, std::vector<std::string> expected,
                   const std::string& label) {
    auto r = reg().get("glob")->execute(input, ctx());
    CHECK(r.success, label + ": glob failed: " + r.error);
    for (auto& e : expected) e = root + "/" + e;
    auto got = lines(r.output);
    if (expected.empty()) {
        CHECK(r.output == "(no matches)", label + ": expected no matches, got:" + show(got));
    } else {
        CHECK(got == expected, label + ": expected:" + show(expected) + "\ngot:" + show(got));
    }
    std::cout << "[OK] " << label << "\n";
    return true;
}

static bool globstar_any_depth() {
    // The regression: zero-directory matches (a.cpp, src/b.cpp) were missed.
    return expect({{"pattern", "**/*.cpp"}},
                  {"a.cpp", "src/b.cpp", "src/deep/c.cpp"},
                  "**/*.cpp matches every depth, skips hidden + symlinked dirs");
}

static bool globstar_under_prefix() {
    return expect({{"pattern", "src/**/*.cpp"}},
                  {"src/b.cpp", "src/deep/c.cpp"},
                  "src/**/*.cpp includes src/ itself (zero directories)");
}

static bool globstar_literal_tail() {
    return expect({{"pattern", "**/CMakeLists.txt"}},
                  {"CMakeLists.txt", "src/deep/CMakeLists.txt"},
                  "**/CMakeLists.txt finds the literal name at every depth");
}

static bool globstar_trailing() {
    return expect({{"pattern", "src/**"}},
                  {"src/b.cpp", "src/deep", "src/deep/CMakeLists.txt",
                   "src/deep/c.cpp", "src/deep/d.h"},
                  "trailing ** lists everything below, no dotfiles");
}

static bool globstar_absolute() {
    return expect({{"pattern", root + "/**/*.h"}}, {"src/deep/d.h"},
                  "absolute **/*.h");
}

static bool globstar_repeated_no_duplicates() {
    return expect({{"pattern", "**/**/*.h"}}, {"src/deep/d.h"},
                  "**/** collapses, no duplicate results");
}

static bool globstar_explicit_hidden_prefix() {
    return expect({{"pattern", ".hidden/**/*.cpp"}}, {".hidden/e.cpp"},
                  "hidden directory named in the literal prefix is searched");
}

static bool globstar_path_param() {
    auto r = reg().get("glob")->execute({{"pattern", "**/*.h"},
                                         {"path", root + "/src"}}, ctx());
    CHECK(r.success, "glob with path failed: " + r.error);
    CHECK(lines(r.output) == std::vector<std::string>{root + "/src/deep/d.h"},
          "path param should be the base, got:" + show(lines(r.output)));
    std::cout << "[OK] **/*.h relative to the path parameter\n";
    return true;
}

static bool globstar_no_matches() {
    return expect({{"pattern", "**/*.nothing"}}, {}, "no matches reported");
}

static bool globstar_missing_base() {
    return expect({{"pattern", "nope/**/*.cpp"}}, {}, "missing literal base → no matches");
}

static bool single_segment_patterns_unchanged() {
    if (!expect({{"pattern", "*.cpp"}}, {"a.cpp"}, "*.cpp stays one level"))
        return false;
    if (!expect({{"pattern", "src/*.cpp"}}, {"src/b.cpp"}, "src/*.cpp stays one level"))
        return false;
    // `**` inside a segment is not globstar — it behaves like `*`.
    return expect({{"pattern", "src/de**"}}, {"src/deep"}, "mid-segment ** is a plain *");
}

static bool globstar_interrupted() {
    std::atomic<bool> stop{true};
    ToolContext c = ctx();
    c.interrupt = &stop;
    auto r = reg().get("glob")->execute({{"pattern", "**/*.cpp"}}, c);
    CHECK(!r.success, "preset interrupt must fail the glob");
    CHECK(r.error.find("interrupted") != std::string::npos,
          "error should say interrupted, got: " + r.error);
    std::cout << "[OK] preset interrupt stops a globstar walk\n";
    return true;
}

static bool globstar_in_project_bypasses_gate() {
    // A deny-everything ask callback: only the built-in in-project read
    // exemption can let this through, so a `**` pattern must still resolve
    // its base to the project directory.
    PermissionGate gate;
    gate.set_ask_callback([](const std::string&, const std::string&,
                             const std::string&, const json&) {
        return PermissionEffect::Deny;
    });
    auto r = reg().execute("glob", {{"pattern", "**/*.h"}}, ctx(), gate);
    CHECK(r.success && !r.denied, "in-project ** glob should bypass the gate: " + r.error);
    CHECK(lines(r.output) == std::vector<std::string>{root + "/src/deep/d.h"},
          "bypassed glob output wrong:" + show(lines(r.output)));

    auto out = reg().execute("glob", {{"pattern", outside + "/**/*.cpp"}}, ctx(), gate);
    CHECK(!out.success && out.denied, "outside-project ** glob must stay gated");
    std::cout << "[OK] ** glob: in-project bypass, outside gated\n";
    return true;
}

int main() {
    std::cout << "=== GlobTool Tests ===\n\n";
    build_tree();
    bool ok = true;
    ok &= globstar_any_depth();
    ok &= globstar_under_prefix();
    ok &= globstar_literal_tail();
    ok &= globstar_trailing();
    ok &= globstar_absolute();
    ok &= globstar_repeated_no_duplicates();
    ok &= globstar_explicit_hidden_prefix();
    ok &= globstar_path_param();
    ok &= globstar_no_matches();
    ok &= globstar_missing_base();
    ok &= single_segment_patterns_unchanged();
    ok &= globstar_interrupted();
    ok &= globstar_in_project_bypasses_gate();
    remove_tree();
    if (ok) {
        std::cout << "\nAll glob tests passed!\n";
        return 0;
    }
    std::cerr << "\nSome tests FAILED.\n";
    return 1;
}
