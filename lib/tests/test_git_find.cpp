#include <haicode/haicode.h>
#include <haicode/tool.h>
#include <iostream>
#include <fstream>
#include "test_check.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <chrono>

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
    TEST_REQUIRE(t, "tool not registered: " + name);
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

// Interactive modes need a terminal the runner never provides: they are
// refused before git runs, with the patch-file route named in the error.
static bool test_git_interactive_refused() {
    auto tool = get_tool("git");
    struct Case { const char* sub; std::vector<std::string> args; };
    std::vector<Case> cases = {
        {"add", {"-p"}}, {"add", {"--patch", "README.md"}}, {"add", {"-Ap"}},
        {"add", {"-i"}}, {"add", {"--edit"}}, {"commit", {"--interactive"}},
        {"checkout", {"-p"}}, {"reset", {"-qp"}}, {"stash", {"-p"}},
        {"stash", {"push", "--patch"}}, {"rebase", {"-i", "HEAD~1"}},
    };
    for (const auto& c : cases) {
        auto r = tool->execute({{"subcommand", c.sub}, {"args", c.args}},
                               make_ctx(git_repo));
        std::string label = std::string(c.sub) + " " + c.args[0];
        CHECK(!r.success, label + " should be refused");
        CHECK(r.error.find("interactive") != std::string::npos
              && r.error.find("apply --cached") != std::string::npos,
              label + ": expected the interactive refusal, got: " + r.error);
    }
    // Non-interactive look-alikes still run: patch OUTPUT, an add whose
    // pathspec follows `--`, and stash's show/list verbs.
    auto log = tool->execute({{"subcommand", "log"}, {"args", {"-p", "-n", "1"}}},
                             make_ctx(git_repo));
    CHECK(log.success, "log -p must run: " + log.error);
    auto add = tool->execute({{"subcommand", "add"}, {"args", {"--", "-p"}}},
                             make_ctx(git_repo));
    CHECK(add.error.find("interactive") == std::string::npos,
          "a pathspec after -- is not a flag: " + add.error);
    auto show = tool->execute({{"subcommand", "stash"}, {"args", {"show", "-p"}}},
                              make_ctx(git_repo));
    CHECK(show.error.find("interactive") == std::string::npos,
          "stash show -p prints a patch: " + show.error);
    std::cout << "[OK] git interactive modes refused, look-alikes run\n";
    return true;
}

// No editor: a commit without -m fails at once (not after the 300 s
// timeout) and the error says how to pass the message.
static bool test_git_commit_without_message_fails_fast() {
    auto tool = get_tool("git");
    std::ofstream(git_repo + "/editor.txt") << "x\n";
    tool->execute({{"subcommand", "add"}, {"args", {"editor.txt"}}}, make_ctx(git_repo));
    auto t0 = std::chrono::steady_clock::now();
    auto r = tool->execute({{"subcommand", "commit"}}, make_ctx(git_repo));
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK(!r.success, "commit without a message must fail");
    CHECK(secs < 10, "commit without a message took " + std::to_string(secs) + "s");
    CHECK(r.error.find("-m") != std::string::npos,
          "expected the -m hint, got: " + r.error);
    tool->execute({{"subcommand", "reset"}, {"args", {"-q", "editor.txt"}}}, make_ctx(git_repo));
    std::remove((git_repo + "/editor.txt").c_str());
    std::cout << "[OK] git commit without -m fails fast with a hint\n";
    return true;
}

// apply --cached stages exactly the hunks in a hand-made patch: the
// non-interactive replacement for add -p.
static bool test_git_apply_cached_stages_one_hunk() {
    auto tool = get_tool("git");
    std::string f = git_repo + "/hunks.txt";
    {
        std::ofstream o(f);
        for (int i = 1; i <= 20; ++i) o << "line " << i << "\n";
    }
    tool->execute({{"subcommand", "add"}, {"args", {"hunks.txt"}}}, make_ctx(git_repo));
    tool->execute({{"subcommand", "commit"}, {"args", {"-q", "-m", "hunks"}}},
                  make_ctx(git_repo));
    {
        std::ofstream o(f);
        for (int i = 1; i <= 20; ++i)
            o << (i == 2 ? "line two" : i == 19 ? "line nineteen"
                         : "line " + std::to_string(i)) << "\n";
    }
    auto diff = tool->execute({{"subcommand", "diff"}, {"args", {"hunks.txt"}}},
                              make_ctx(git_repo));
    CHECK(diff.success, "diff failed: " + diff.error);
    size_t second = diff.output.find("\n@@", diff.output.find("@@") + 2);
    CHECK(second != std::string::npos, "expected two hunks: " + diff.output);
    std::string patch_path = "/tmp/test_git_haicode_hunk.patch";
    std::ofstream(patch_path) << diff.output.substr(0, second + 1);

    auto apply = tool->execute(
        {{"subcommand", "apply"}, {"args", {"--cached", patch_path}}},
        make_ctx(git_repo));
    std::remove(patch_path.c_str());
    CHECK(apply.success, "apply --cached failed: " + apply.error);
    auto staged = tool->execute({{"subcommand", "diff"}, {"args", {"--cached"}}},
                                make_ctx(git_repo));
    auto unstaged = tool->execute({{"subcommand", "diff"}}, make_ctx(git_repo));
    CHECK(staged.output.find("+line two") != std::string::npos
          && staged.output.find("nineteen") == std::string::npos,
          "only the first hunk should be staged: " + staged.output);
    CHECK(unstaged.output.find("+line nineteen") != std::string::npos
          && unstaged.output.find("line two") == std::string::npos,
          "the second hunk should stay unstaged: " + unstaged.output);

    auto unsafe = tool->execute(
        {{"subcommand", "apply"}, {"args", {"--unsafe-p", "x.patch"}}},
        make_ctx(git_repo));
    CHECK(!unsafe.success && unsafe.error.find("unsafe-paths") != std::string::npos,
          "apply --unsafe-paths (abbreviated) must be refused: " + unsafe.error);
    std::cout << "[OK] git apply --cached stages one hunk; --unsafe-paths refused\n";
    return true;
}

// ---- grep tests ----

static std::string grep_root;  // temp dir set up in main

static bool test_grep_skips_binary_and_git() {
    auto tool = get_tool("grep");
    auto r = tool->execute({{"pattern", "needle"}}, make_ctx(grep_root));
    CHECK(r.success, "grep failed: " + r.error);
    CHECK(r.output.find("inary file") == std::string::npos,
          "binary files must be skipped: " + r.output);
    CHECK(r.output.find("/.git/") == std::string::npos, ".git must be skipped: " + r.output);
    CHECK(r.output.find("build/notes.txt") != std::string::npos,
          "text under build/ is searched by default: " + r.output);
    auto ex = tool->execute({{"pattern", "needle"}, {"exclude_dirs", {"build"}}},
                            make_ctx(grep_root));
    CHECK(ex.success && ex.output.find("build/") == std::string::npos,
          "exclude_dirs must skip build/: " + ex.output);
    std::cout << "[OK] grep skips binaries and .git; exclude_dirs works\n";
    return true;
}

static bool test_grep_output_modes() {
    auto tool = get_tool("grep");
    auto files = tool->execute({{"pattern", "needle"}, {"output_mode", "files"},
                                {"exclude_dirs", {"build"}}}, make_ctx(grep_root));
    CHECK(files.success, "files mode failed: " + files.error);
    CHECK(files.output.find("a.txt\n") != std::string::npos
          && files.output.find("needle") == std::string::npos,
          "files mode lists paths only: " + files.output);

    auto count = tool->execute({{"pattern", "needle"}, {"output_mode", "count"},
                                {"exclude_dirs", {"build"}}}, make_ctx(grep_root));
    CHECK(count.success, "count mode failed: " + count.error);
    CHECK(count.output.find("a.txt:2\n") != std::string::npos,
          "a.txt has two matching lines: " + count.output);
    CHECK(count.output.find(":0") == std::string::npos
          && count.output.find("plain.txt") == std::string::npos,
          "files with no match are dropped: " + count.output);

    auto one = tool->execute({{"pattern", "needle"}, {"output_mode", "count"},
                              {"path", "a.txt"}}, make_ctx(grep_root));
    CHECK(one.output.find("a.txt:2") != std::string::npos,
          "single-file count keeps the path: " + one.output);
    auto none = tool->execute({{"pattern", "needle"}, {"output_mode", "count"},
                               {"path", "plain.txt"}}, make_ctx(grep_root));
    CHECK(none.output == "(no matches)", "zero count is no match: " + none.output);

    auto only = tool->execute({{"pattern", "need[a-z]+"}, {"only_matching", true},
                               {"line_numbers", false}, {"path", "a.txt"}},
                              make_ctx(grep_root));
    CHECK(only.output == "needle\nneedle\n", "only_matching prints the match: " + only.output);

    auto bad = tool->execute({{"pattern", "x"}, {"output_mode", "lines"}}, make_ctx(grep_root));
    CHECK(!bad.success, "unknown output_mode must fail");
    std::cout << "[OK] grep files/count/only_matching modes\n";
    return true;
}

static bool test_grep_long_lines_capped() {
    auto tool = get_tool("grep");
    auto r = tool->execute({{"pattern", "needle"}, {"path", "long.txt"},
                            {"line_numbers", false}}, make_ctx(grep_root));
    CHECK(r.success, "grep failed: " + r.error);
    CHECK(r.output.find(" [... +") != std::string::npos,
          "a 5000-byte line is cut by default");
    CHECK(r.output.size() < 1100, "cut line too long: " + std::to_string(r.output.size()));

    auto small = tool->execute({{"pattern", "needle"}, {"path", "long.txt"},
                                {"line_numbers", false}, {"max_line_length", 10}},
                               make_ctx(grep_root));
    // 9 ASCII bytes, then the 3-byte "é" would straddle the cut: back off.
    CHECK(small.output.rfind("123456789 [... +", 0) == 0,
          "cut backs off to a UTF-8 boundary: " + small.output);

    auto full = tool->execute({{"pattern", "needle"}, {"path", "long.txt"},
                               {"line_numbers", false}, {"max_line_length", 0}},
                              make_ctx(grep_root));
    CHECK(full.output.find("[... +") == std::string::npos && full.output.size() > 5000,
          "max_line_length 0 keeps the whole line");
    std::cout << "[OK] grep caps long lines at a UTF-8 boundary\n";
    return true;
}

static std::string setup_grep_tree() {
    std::string root = "/tmp/test_grep_haicode";
    system(("rm -rf " + root).c_str());
    system(("mkdir -p " + root + "/build " + root + "/.git").c_str());
    std::ofstream(root + "/a.txt") << "one needle\ntwo\nthree needle\n";
    std::ofstream(root + "/plain.txt") << "nothing here\n";
    std::ofstream(root + "/build/notes.txt") << "needle in build\n";
    std::ofstream(root + "/.git/config") << "needle in git\n";
    {
        std::ofstream bin(root + "/build/prog.o", std::ios::binary);
        bin << std::string("\x7f" "ELF\0\0needle\0", 13);
    }
    std::ofstream(root + "/long.txt") << "123456789\xc3\xa9" << std::string(5000, 'x')
                                      << " needle\n";
    return root;
}

// Initialize a small self-contained git repo for the GitTool tests so they
// don't depend on the location of the haicode checkout.
static std::string setup_git_repo() {
    std::string root = "/tmp/test_git_haicode";
    system(("rm -rf " + root).c_str());
    system(("mkdir -p " + root).c_str());
    std::ofstream(root + "/README.md") << "test repo\n";
    std::ofstream(root + "/CMakeLists.txt") << "# placeholder\n";
    // -c flags supply identity locally so the test doesn't depend on a
    // configured global user.name/user.email.
    std::string init_and_commit =
        "cd " + root + " && "
        "git init -q -b main && "
        "git -c user.name=test -c user.email=t@t add . && "
        "git -c user.name=test -c user.email=t@t commit -q -m 'init' >/dev/null";
    system(init_and_commit.c_str());
    return root;
}

int main() {
    std::cout << "=== GitTool + FindTool + GrepTool Tests ===\n\n";

    git_repo = setup_git_repo();
    find_root = setup_find_tree();
    grep_root = setup_grep_tree();
    // Commits made through the tool need an identity; the child inherits it.
    setenv("GIT_AUTHOR_NAME", "test", 1);
    setenv("GIT_AUTHOR_EMAIL", "t@t", 1);
    setenv("GIT_COMMITTER_NAME", "test", 1);
    setenv("GIT_COMMITTER_EMAIL", "t@t", 1);

    bool ok = true;

    std::cout << "-- git --\n";
    ok &= test_git_status();
    ok &= test_git_log();
    ok &= test_git_branch();
    ok &= test_git_rev_parse();
    ok &= test_git_ls_files();
    ok &= test_git_disallowed_subcommand();
    ok &= test_git_missing_subcommand();
    ok &= test_git_interactive_refused();
    ok &= test_git_commit_without_message_fails_fast();
    ok &= test_git_apply_cached_stages_one_hunk();

    std::cout << "\n-- find --\n";
    ok &= test_find_all();
    ok &= test_find_by_name();
    ok &= test_find_by_type_f();
    ok &= test_find_by_type_d();
    ok &= test_find_maxdepth();
    ok &= test_find_no_matches();
    ok &= test_find_relative_path();
    ok &= test_find_default_path();

    std::cout << "\n-- grep --\n";
    ok &= test_grep_skips_binary_and_git();
    ok &= test_grep_output_modes();
    ok &= test_grep_long_lines_capped();

    system(("rm -rf " + find_root).c_str());
    system(("rm -rf " + grep_root).c_str());
    system(("rm -rf " + git_repo).c_str());

    if (ok) {
        std::cout << "\nAll GitTool + FindTool + GrepTool tests passed!\n";
        return 0;
    } else {
        std::cerr << "\nSome tests FAILED.\n";
        return 1;
    }
}
