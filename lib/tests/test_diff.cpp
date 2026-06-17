#include <haicode/haicode.h>
#include <haicode/tool.h>
#include <iostream>
#include <fstream>
#include <cassert>
#include <cstdio>
#include <string>

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    assert(f.is_open());
    f << content;
}

static haicode::ToolContext make_ctx(const std::string& working_dir) {
    haicode::ToolContext ctx;
    ctx.working_dir = working_dir;
    return ctx;
}

static std::shared_ptr<haicode::Tool> get_diff_tool() {
    static haicode::ToolRegistry registry;
    static bool registered = false;
    if (!registered) {
        haicode::register_builtin_tools(registry);
        registered = true;
    }
    auto tool = registry.get("diff");
    assert(tool && "diff tool not registered");
    return tool;
}

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << msg << "\n"; return false; } } while(0)

static bool test_no_differences() {
    const std::string path = "/tmp/test_diff_same.txt";
    const std::string content = "line one\nline two\nline three\n";
    write_file(path, content);

    auto tool = get_diff_tool();
    auto result = tool->execute({{"path", path}, {"content", content}}, make_ctx("/tmp"));

    CHECK(result.success, "expected success for identical content");
    CHECK(result.output == "(no differences)", "expected '(no differences)', got: " + result.output);
    std::remove(path.c_str());
    std::cout << "[OK] no differences\n";
    return true;
}

static bool test_added_lines() {
    const std::string path = "/tmp/test_diff_add.txt";
    write_file(path, "line one\nline two\n");

    auto tool = get_diff_tool();
    auto result = tool->execute(
        {{"path", path}, {"content", "line one\nline two\nline three\n"}},
        make_ctx("/tmp"));

    CHECK(result.success, "expected success");
    CHECK(result.output.find("+line three") != std::string::npos,
          "expected '+line three' in diff output, got: " + result.output);
    std::remove(path.c_str());
    std::cout << "[OK] added lines\n";
    return true;
}

static bool test_removed_lines() {
    const std::string path = "/tmp/test_diff_del.txt";
    write_file(path, "line one\nline two\nline three\n");

    auto tool = get_diff_tool();
    auto result = tool->execute(
        {{"path", path}, {"content", "line one\nline three\n"}},
        make_ctx("/tmp"));

    CHECK(result.success, "expected success");
    CHECK(result.output.find("-line two") != std::string::npos,
          "expected '-line two' in diff output, got: " + result.output);
    std::remove(path.c_str());
    std::cout << "[OK] removed lines\n";
    return true;
}

static bool test_modified_line() {
    const std::string path = "/tmp/test_diff_mod.txt";
    write_file(path, "hello world\n");

    auto tool = get_diff_tool();
    auto result = tool->execute(
        {{"path", path}, {"content", "hello haiku\n"}},
        make_ctx("/tmp"));

    CHECK(result.success, "expected success");
    CHECK(result.output.find("-hello world") != std::string::npos,
          "expected '-hello world' in diff");
    CHECK(result.output.find("+hello haiku") != std::string::npos,
          "expected '+hello haiku' in diff");
    std::remove(path.c_str());
    std::cout << "[OK] modified line\n";
    return true;
}

static bool test_replace_all_content() {
    const std::string path = "/tmp/test_diff_replace.txt";
    write_file(path, "old content\n");

    auto tool = get_diff_tool();
    auto result = tool->execute(
        {{"path", path}, {"content", "new content\n"}},
        make_ctx("/tmp"));

    CHECK(result.success, "expected success");
    CHECK(result.output.find("-old content") != std::string::npos, "expected '-old content'");
    CHECK(result.output.find("+new content") != std::string::npos, "expected '+new content'");
    std::remove(path.c_str());
    std::cout << "[OK] replace all content\n";
    return true;
}

static bool test_missing_file() {
    auto tool = get_diff_tool();
    auto result = tool->execute(
        {{"path", "/tmp/nonexistent_haicode_diff_test.txt"}, {"content", "anything\n"}},
        make_ctx("/tmp"));

    CHECK(!result.success, "expected failure for missing file");
    std::cout << "[OK] missing file returns error\n";
    return true;
}

static bool test_missing_path_param() {
    auto tool = get_diff_tool();
    auto result = tool->execute({{"content", "something"}}, make_ctx("/tmp"));

    CHECK(!result.success, "expected failure when path is missing");
    std::cout << "[OK] missing path param returns error\n";
    return true;
}

static bool test_relative_path() {
    const std::string path = "/tmp/test_diff_rel.txt";
    write_file(path, "alpha\nbeta\n");

    auto tool = get_diff_tool();
    auto result = tool->execute(
        {{"path", "test_diff_rel.txt"}, {"content", "alpha\nbeta\ngamma\n"}},
        make_ctx("/tmp"));

    CHECK(result.success, "expected success with relative path");
    CHECK(result.output.find("+gamma") != std::string::npos, "expected '+gamma'");
    std::remove(path.c_str());
    std::cout << "[OK] relative path resolved against working_dir\n";
    return true;
}

int main() {
    std::cout << "=== DiffTool Tests ===\n";

    bool ok = true;
    ok &= test_no_differences();
    ok &= test_added_lines();
    ok &= test_removed_lines();
    ok &= test_modified_line();
    ok &= test_replace_all_content();
    ok &= test_missing_file();
    ok &= test_missing_path_param();
    ok &= test_relative_path();

    if (ok) {
        std::cout << "\nAll DiffTool tests passed!\n";
        return 0;
    } else {
        std::cerr << "\nSome tests FAILED.\n";
        return 1;
    }
}
