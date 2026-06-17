#include <haicode/haicode.h>
#include <haicode/tool.h>
#include <iostream>
#include <fstream>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

static haicode::ToolContext make_ctx(const std::string& working_dir) {
    haicode::ToolContext ctx;
    ctx.working_dir = working_dir;
    return ctx;
}

static haicode::ToolRegistry& registry() {
    static haicode::ToolRegistry reg;
    static bool registered = false;
    if (!registered) {
        haicode::register_builtin_tools(reg);
        registered = true;
    }
    return reg;
}

static std::shared_ptr<haicode::Tool> get_tool(const std::string& name) {
    auto t = registry().get(name);
    assert(t && ("tool not registered: " + name).c_str());
    return t;
}

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << msg << "\n"; return false; } } while(0)

// ---- git tests ----

static std::string git_repo;  // set up once in main

static bool test_git_status() {
    auto tool = get_tool("git");
    auto result = tool->execute({{"subcommand", "status"}}, make_ctx(git_repo));
    CHECK(result.success, "git status failed: " + result.error);
    CHECK(result.output.find("branch") != std::string::npos ||
          result.output.find("HEAD") != std::string::npos,
          "git status output looks wrong: " + result.output);
    std::cout << "[OK] git status\n";
    return true;
}

static bool test_git_log() {
    auto tool = get_tool("git");
    auto result = tool->execute(
        {{"subcommand", "log"}, {"args", {"--oneline", "-3"}}},
        make_ctx(git_repo));
    CHECK(result.success, "git log failed: " + result.error);
    std::cout << "[OK] git log --oneline -3\n";
    return true;
}

static bool test_git_branch() {
    auto tool = get_tool("git");
    auto result = tool->execute({{"subcommand", "branch"}}, make_ctx(git_repo));
    CHECK(result.success, "git branch failed: " + result.error);
    std::cout << "[OK] git branch\n";
    return true;
}

static bool test_git_rev_parse() {
    auto tool = get_tool("git");
    auto result = tool->execute(
        {{"subcommand", "rev-parse"}, {"args", {"--abbrev-ref", "HEAD"}}},
        make_ctx(git_repo));
    CHECK(result.success, "git rev-parse failed: " + result.error);
    CHECK(!result.output.empty(), "rev-parse output is empty");
    std::cout << "[OK] git rev-parse --abbrev-ref HEAD\n";
    return true;
}

static bool test_git_disallowed_subcommand() {
    auto tool = get_tool("git");
    auto result = tool->execute({{"subcommand", "bisect"}}, make_ctx(git_repo));
    CHECK(!result.success, "expected failure for disallowed subcommand");
    CHECK(result.error.find("not allowed") != std::string::npos,
          "expected 'not allowed' in error, got: " + result.error);
    std::cout << "[OK] git disallowed subcommand rejected\n";
    return true;
}

static bool test_git_missing_subcommand() {
    auto tool = get_tool("git");
    auto result = tool->execute({{"args", {"--oneline"}}}, make_ctx(git_repo));
    CHECK(!result.success, "expected failure when subcommand is missing");
    std::cout << "[OK] git missing subcommand returns error\n";
    return true;
}

static bool test_git_ls_files() {
    auto tool = get_tool("git");
    auto result = tool->execute({{"subcommand", "ls-files"}}, make_ctx(git_repo));
    CHECK(result.success, "git ls-files failed: " + result.error);
    CHECK(result.output.find("CMakeLists") != std::string::npos ||
          !result.output.empty(), "expected some tracked files");
    std::cout << "[OK] git ls-files\n";
    return true;
}

// ---- find tests ----

static std::string find_root;  // temp dir set up in main

static bool test_find_all() {
    auto tool = get_tool("find");
    auto result = tool->execute({{"path", find_root}}, make_ctx(find_root));
    CHECK(result.success, "find failed: " + result.error);
    CHECK(result.output.find(find_root) != std::string::npos, "expected path in output");
    std::cout << "[OK] find all\n";
    return true;
}

static bool test_find_by_name() {
    auto tool = get_tool("find");
    auto result = tool->execute(
        {{"path", find_root}, {"name", "*.txt"}},
        make_ctx(find_root));
    CHECK(result.success, "find -name failed: " + result.error);
    CHECK(result.output.find(".txt") != std::string::npos, "expected .txt files");
    CHECK(result.output.find(".cpp") == std::string::npos, "should not include .cpp files");
    std::cout << "[OK] find -name *.txt\n";
    return true;
}

static bool test_find_by_type_f() {
    auto tool = get_tool("find");
    auto result = tool->execute(
        {{"path", find_root}, {"type", "f"}},
        make_ctx(find_root));
    CHECK(result.success, "find -type f failed: " + result.error);
    std::cout << "[OK] find -type f\n";
    return true;
}

static bool test_find_by_type_d() {
    auto tool = get_tool("find");
    auto result = tool->execute(
        {{"path", find_root}, {"type", "d"}},
        make_ctx(find_root));
    CHECK(result.success, "find -type d failed: " + result.error);
    CHECK(result.output.find("subdir") != std::string::npos, "expected subdir in output");
    std::cout << "[OK] find -type d\n";
    return true;
}

static bool test_find_maxdepth() {
    auto tool = get_tool("find");
    // With maxdepth 1 we should NOT see the nested file
    auto result = tool->execute(
        {{"path", find_root}, {"maxdepth", 1}, {"type", "f"}},
        make_ctx(find_root));
    CHECK(result.success, "find -maxdepth 1 failed: " + result.error);
    CHECK(result.output.find("nested") == std::string::npos,
          "maxdepth 1 should not show nested file");
    std::cout << "[OK] find -maxdepth 1 excludes nested files\n";
    return true;
}

static bool test_find_no_matches() {
    auto tool = get_tool("find");
    auto result = tool->execute(
        {{"path", find_root}, {"name", "*.nonexistent_xyz"}},
        make_ctx(find_root));
    CHECK(result.success, "find should succeed even with no matches");
    CHECK(result.output == "(no matches)", "expected '(no matches)', got: " + result.output);
    std::cout << "[OK] find no matches\n";
    return true;
}

static bool test_find_relative_path() {
    auto tool = get_tool("find");
    // Use a relative path — should resolve against working_dir
    auto result = tool->execute(
        {{"path", "subdir"}, {"type", "f"}},
        make_ctx(find_root));
    CHECK(result.success, "find with relative path failed: " + result.error);
    CHECK(result.output.find("nested") != std::string::npos, "expected nested file");
    std::cout << "[OK] find relative path\n";
    return true;
}

static bool test_find_default_path() {
    auto tool = get_tool("find");
    // No path — should default to working_dir
    auto result = tool->execute({{"type", "f"}}, make_ctx(find_root));
    CHECK(result.success, "find without path failed: " + result.error);
    CHECK(!result.output.empty(), "expected some files");
    std::cout << "[OK] find defaults to working_dir\n";
    return true;
}

// Create a small directory tree for find tests
static std::string setup_find_tree() {
    std::string root = "/tmp/test_find_haicode";
    system(("rm -rf " + root).c_str());
    system(("mkdir -p " + root + "/subdir").c_str());
    auto write = [](const std::string& p, const std::string& c) {
        std::ofstream f(p); f << c;
    };
    write(root + "/a.txt", "hello");
    write(root + "/b.txt", "world");
    write(root + "/c.cpp", "int main(){}");
    write(root + "/subdir/nested.txt", "deep");
    return root;
}

int main() {
    std::cout << "=== GitTool + FindTool Tests ===\n\n";

    // Use the haicode repo itself as the git test directory
    git_repo = "/boot/home/haicode";
    find_root = setup_find_tree();

    bool ok = true;

    std::cout << "-- git --\n";
    ok &= test_git_status();
    ok &= test_git_log();
    ok &= test_git_branch();
    ok &= test_git_rev_parse();
    ok &= test_git_ls_files();
    ok &= test_git_disallowed_subcommand();
    ok &= test_git_missing_subcommand();

    std::cout << "\n-- find --\n";
    ok &= test_find_all();
    ok &= test_find_by_name();
    ok &= test_find_by_type_f();
    ok &= test_find_by_type_d();
    ok &= test_find_maxdepth();
    ok &= test_find_no_matches();
    ok &= test_find_relative_path();
    ok &= test_find_default_path();

    system(("rm -rf " + find_root).c_str());

    if (ok) {
        std::cout << "\nAll GitTool + FindTool tests passed!\n";
        return 0;
    } else {
        std::cerr << "\nSome tests FAILED.\n";
        return 1;
    }
}
