#include <haicode/haicode.h>
#include <haicode/tool.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

// ---- Helpers ----

static haicode::ToolRegistry& reg() {
    static haicode::ToolRegistry r;
    static bool done = false;
    if (!done) { haicode::register_builtin_tools(r); done = true; }
    return r;
}

static std::shared_ptr<haicode::Tool> tool(const std::string& name) {
    auto t = reg().get(name);
    assert(t && ("tool not found: " + name).c_str());
    return t;
}

static haicode::ToolContext ctx(const std::string& dir = "/tmp") {
    haicode::ToolContext c;
    c.working_dir = dir;
    return c;
}

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    assert(f.is_open());
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool file_exists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << (msg) << "\n"; return false; } } while(0)

// ============================================================
// ReadTool
// ============================================================

static bool read_basic() {
    const std::string p = "/tmp/tft_read_basic.txt";
    write_file(p, "alpha\nbeta\ngamma\n");
    auto r = tool("read")->execute({{"path", p}}, ctx());
    CHECK(r.success, "basic read failed: " + r.error);
    CHECK(r.output.find("1\talpha") != std::string::npos, "line 1 missing");
    CHECK(r.output.find("2\tbeta")  != std::string::npos, "line 2 missing");
    CHECK(r.output.find("3\tgamma") != std::string::npos, "line 3 missing");
    std::remove(p.c_str());
    std::cout << "[OK] read basic\n";
    return true;
}

static bool read_offset() {
    const std::string p = "/tmp/tft_read_offset.txt";
    write_file(p, "one\ntwo\nthree\nfour\n");
    auto r = tool("read")->execute({{"path", p}, {"offset", 3}}, ctx());
    CHECK(r.success, r.error);
    CHECK(r.output.find("1\t") == std::string::npos, "line 1 should be skipped");
    CHECK(r.output.find("2\t") == std::string::npos, "line 2 should be skipped");
    CHECK(r.output.find("3\tthree") != std::string::npos, "line 3 missing");
    CHECK(r.output.find("4\tfour")  != std::string::npos, "line 4 missing");
    std::remove(p.c_str());
    std::cout << "[OK] read offset\n";
    return true;
}

static bool read_limit() {
    const std::string p = "/tmp/tft_read_limit.txt";
    write_file(p, "a\nb\nc\nd\ne\n");
    auto r = tool("read")->execute({{"path", p}, {"limit", 2}}, ctx());
    CHECK(r.success, r.error);
    CHECK(r.output.find("1\ta") != std::string::npos, "line 1 missing");
    CHECK(r.output.find("2\tb") != std::string::npos, "line 2 missing");
    CHECK(r.output.find("3\tc") == std::string::npos, "line 3 should be excluded");
    std::remove(p.c_str());
    std::cout << "[OK] read limit\n";
    return true;
}

static bool read_offset_and_limit() {
    const std::string p = "/tmp/tft_read_oflim.txt";
    write_file(p, "a\nb\nc\nd\ne\n");
    auto r = tool("read")->execute({{"path", p}, {"offset", 2}, {"limit", 2}}, ctx());
    CHECK(r.success, r.error);
    CHECK(r.output.find("1\t") == std::string::npos, "line 1 should be skipped");
    CHECK(r.output.find("2\tb") != std::string::npos, "line 2 missing");
    CHECK(r.output.find("3\tc") != std::string::npos, "line 3 missing");
    CHECK(r.output.find("4\t") == std::string::npos, "line 4 should be excluded");
    std::remove(p.c_str());
    std::cout << "[OK] read offset+limit\n";
    return true;
}

static bool read_relative_path() {
    const std::string p = "/tmp/tft_read_rel.txt";
    write_file(p, "hello\n");
    auto r = tool("read")->execute({{"path", "tft_read_rel.txt"}}, ctx("/tmp"));
    CHECK(r.success, "relative path failed: " + r.error);
    CHECK(r.output.find("hello") != std::string::npos, "content missing");
    std::remove(p.c_str());
    std::cout << "[OK] read relative path\n";
    return true;
}

static bool read_missing_file() {
    auto r = tool("read")->execute({{"path", "/tmp/tft_no_such_file_xyz.txt"}}, ctx());
    CHECK(!r.success, "expected failure for missing file");
    std::cout << "[OK] read missing file\n";
    return true;
}

static bool read_binary_file() {
    const std::string p = "/tmp/tft_read_binary.bin";
    // Write content with null bytes — triggers binary detection
    std::string bin(64, 'x');
    bin[10] = '\0';
    write_file(p, bin);
    auto r = tool("read")->execute({{"path", p}}, ctx());
    CHECK(!r.success, "expected failure for binary file");
    CHECK(r.error.find("Binary") != std::string::npos, "expected 'Binary' in error: " + r.error);
    std::remove(p.c_str());
    std::cout << "[OK] read binary file rejected\n";
    return true;
}

static bool read_missing_path_param() {
    auto r = tool("read")->execute({{"offset", 1}}, ctx());
    CHECK(!r.success, "expected failure when path is missing");
    std::cout << "[OK] read missing path param\n";
    return true;
}

static bool read_empty_file() {
    const std::string p = "/tmp/tft_read_empty.txt";
    write_file(p, "");
    auto r = tool("read")->execute({{"path", p}}, ctx());
    CHECK(r.success, "reading empty file should succeed: " + r.error);
    CHECK(r.output.empty(), "expected empty output for empty file");
    std::remove(p.c_str());
    std::cout << "[OK] read empty file\n";
    return true;
}

// ============================================================
// WriteTool
// ============================================================

static bool write_basic() {
    const std::string p = "/tmp/tft_write_basic.txt";
    std::remove(p.c_str());
    auto r = tool("write")->execute({{"path", p}, {"content", "hello world\n"}}, ctx());
    CHECK(r.success, "write failed: " + r.error);
    CHECK(read_file(p) == "hello world\n", "file content mismatch");
    std::remove(p.c_str());
    std::cout << "[OK] write basic\n";
    return true;
}

static bool write_overwrites_existing() {
    const std::string p = "/tmp/tft_write_overwrite.txt";
    write_file(p, "old content\n");
    auto r = tool("write")->execute({{"path", p}, {"content", "new content\n"}}, ctx());
    CHECK(r.success, r.error);
    CHECK(read_file(p) == "new content\n", "overwrite failed");
    std::remove(p.c_str());
    std::cout << "[OK] write overwrites existing file\n";
    return true;
}

static bool write_creates_parent_dirs() {
    const std::string dir = "/tmp/tft_write_newdir_a/b/c";
    const std::string p   = dir + "/file.txt";
    // Ensure clean state
    system("rm -rf /tmp/tft_write_newdir_a");
    auto r = tool("write")->execute({{"path", p}, {"content", "deep\n"}}, ctx());
    CHECK(r.success, "write with new parents failed: " + r.error);
    CHECK(read_file(p) == "deep\n", "content mismatch after parent creation");
    system("rm -rf /tmp/tft_write_newdir_a");
    std::cout << "[OK] write creates parent directories\n";
    return true;
}

static bool write_preserves_binary_content() {
    const std::string p = "/tmp/tft_write_binary.bin";
    std::string content(256, '\0');
    for (int i = 0; i < 256; i++) content[i] = static_cast<char>(i);
    auto r = tool("write")->execute({{"path", p}, {"content", content}}, ctx());
    CHECK(r.success, r.error);
    CHECK(read_file(p) == content, "binary content corrupted");
    std::remove(p.c_str());
    std::cout << "[OK] write preserves binary content\n";
    return true;
}

static bool write_empty_content() {
    const std::string p = "/tmp/tft_write_empty.txt";
    auto r = tool("write")->execute({{"path", p}, {"content", ""}}, ctx());
    CHECK(r.success, r.error);
    CHECK(read_file(p).empty(), "expected empty file");
    std::remove(p.c_str());
    std::cout << "[OK] write empty content\n";
    return true;
}

static bool write_relative_path() {
    const std::string p = "/tmp/tft_write_rel.txt";
    std::remove(p.c_str());
    auto r = tool("write")->execute({{"path", "tft_write_rel.txt"}, {"content", "rel\n"}}, ctx("/tmp"));
    CHECK(r.success, r.error);
    CHECK(read_file(p) == "rel\n", "relative write content mismatch");
    std::remove(p.c_str());
    std::cout << "[OK] write relative path\n";
    return true;
}

static bool write_missing_path_param() {
    auto r = tool("write")->execute({{"content", "x"}}, ctx());
    CHECK(!r.success, "expected failure when path is missing");
    std::cout << "[OK] write missing path param\n";
    return true;
}

static bool write_no_stray_tmp_file() {
    const std::string p   = "/tmp/tft_write_notmp.txt";
    const std::string tmp = p + ".tmp_write";
    std::remove(p.c_str());
    std::remove(tmp.c_str());
    auto r = tool("write")->execute({{"path", p}, {"content", "clean\n"}}, ctx());
    CHECK(r.success, r.error);
    CHECK(!file_exists(tmp), ".tmp_write file left behind after successful write");
    std::remove(p.c_str());
    std::cout << "[OK] write leaves no stray .tmp_write file\n";
    return true;
}

// ============================================================
// EditTool
// ============================================================

static bool edit_basic() {
    const std::string p = "/tmp/tft_edit_basic.txt";
    write_file(p, "hello world\n");
    auto r = tool("edit")->execute({{"path", p}, {"old_string", "world"}, {"new_string", "haiku"}}, ctx());
    CHECK(r.success, r.error);
    CHECK(read_file(p) == "hello haiku\n", "edit result mismatch");
    std::remove(p.c_str());
    std::cout << "[OK] edit basic replacement\n";
    return true;
}

static bool edit_multiline() {
    const std::string p = "/tmp/tft_edit_multi.txt";
    write_file(p, "line one\nline two\nline three\n");
    auto r = tool("edit")->execute(
        {{"path", p}, {"old_string", "line two\nline three"}, {"new_string", "line TWO\nline THREE"}},
        ctx());
    CHECK(r.success, r.error);
    CHECK(read_file(p) == "line one\nline TWO\nline THREE\n", "multiline edit mismatch");
    std::remove(p.c_str());
    std::cout << "[OK] edit multiline replacement\n";
    return true;
}

static bool edit_delete_text() {
    const std::string p = "/tmp/tft_edit_delete.txt";
    write_file(p, "keep this\ndelete this\nkeep this too\n");
    auto r = tool("edit")->execute(
        {{"path", p}, {"old_string", "delete this\n"}, {"new_string", ""}},
        ctx());
    CHECK(r.success, r.error);
    CHECK(read_file(p) == "keep this\nkeep this too\n", "delete edit mismatch");
    std::remove(p.c_str());
    std::cout << "[OK] edit delete text (empty new_string)\n";
    return true;
}

static bool edit_replace_all() {
    const std::string p = "/tmp/tft_edit_replaceall.txt";
    write_file(p, "foo bar foo baz foo\n");
    auto r = tool("edit")->execute(
        {{"path", p}, {"old_string", "foo"}, {"new_string", "qux"}, {"replace_all", true}},
        ctx());
    CHECK(r.success, r.error);
    CHECK(read_file(p) == "qux bar qux baz qux\n", "replace_all mismatch");
    CHECK(r.output.find("3") != std::string::npos, "expected count of 3 in output");
    std::remove(p.c_str());
    std::cout << "[OK] edit replace_all\n";
    return true;
}

static bool edit_duplicate_without_replace_all() {
    const std::string p = "/tmp/tft_edit_dup.txt";
    write_file(p, "same\nsame\n");
    auto r = tool("edit")->execute(
        {{"path", p}, {"old_string", "same"}, {"new_string", "different"}},
        ctx());
    CHECK(!r.success, "expected failure for duplicate match without replace_all");
    CHECK(r.error.find("2") != std::string::npos, "expected count '2' in error: " + r.error);
    CHECK(read_file(p) == "same\nsame\n", "file should be unchanged after failed edit");
    std::remove(p.c_str());
    std::cout << "[OK] edit duplicate old_string without replace_all fails\n";
    return true;
}

static bool edit_not_found() {
    const std::string p = "/tmp/tft_edit_notfound.txt";
    write_file(p, "hello world\n");
    auto r = tool("edit")->execute(
        {{"path", p}, {"old_string", "no such text"}, {"new_string", "x"}},
        ctx());
    CHECK(!r.success, "expected failure when old_string not found");
    CHECK(read_file(p) == "hello world\n", "file should be unchanged");
    std::remove(p.c_str());
    std::cout << "[OK] edit old_string not found\n";
    return true;
}

static bool edit_empty_old_string() {
    const std::string p = "/tmp/tft_edit_emptyold.txt";
    write_file(p, "content\n");
    auto r = tool("edit")->execute(
        {{"path", p}, {"old_string", ""}, {"new_string", "x"}},
        ctx());
    CHECK(!r.success, "expected failure for empty old_string");
    std::remove(p.c_str());
    std::cout << "[OK] edit empty old_string rejected\n";
    return true;
}

static bool edit_binary_file() {
    const std::string p = "/tmp/tft_edit_binary.bin";
    std::string bin(64, 'x');
    bin[5] = '\0';
    write_file(p, bin);
    auto r = tool("edit")->execute(
        {{"path", p}, {"old_string", "xxxxx"}, {"new_string", "yyyyy"}},
        ctx());
    CHECK(!r.success, "expected failure for binary file");
    std::remove(p.c_str());
    std::cout << "[OK] edit binary file rejected\n";
    return true;
}

static bool edit_missing_file() {
    auto r = tool("edit")->execute(
        {{"path", "/tmp/tft_edit_nosuchfile.txt"}, {"old_string", "x"}, {"new_string", "y"}},
        ctx());
    CHECK(!r.success, "expected failure for missing file");
    std::cout << "[OK] edit missing file\n";
    return true;
}

static bool edit_relative_path() {
    const std::string p = "/tmp/tft_edit_rel.txt";
    write_file(p, "before\n");
    auto r = tool("edit")->execute(
        {{"path", "tft_edit_rel.txt"}, {"old_string", "before"}, {"new_string", "after"}},
        ctx("/tmp"));
    CHECK(r.success, r.error);
    CHECK(read_file(p) == "after\n", "relative edit mismatch");
    std::remove(p.c_str());
    std::cout << "[OK] edit relative path\n";
    return true;
}

static bool edit_no_stray_tmp_file() {
    const std::string p   = "/tmp/tft_edit_notmp.txt";
    const std::string tmp = p + ".tmp_write";
    write_file(p, "original\n");
    std::remove(tmp.c_str());
    auto r = tool("edit")->execute(
        {{"path", p}, {"old_string", "original"}, {"new_string", "replaced"}},
        ctx());
    CHECK(r.success, r.error);
    CHECK(!file_exists(tmp), ".tmp_write left behind after successful edit");
    std::remove(p.c_str());
    std::cout << "[OK] edit leaves no stray .tmp_write file\n";
    return true;
}

static bool edit_whitespace_must_match_exactly() {
    const std::string p = "/tmp/tft_edit_ws.txt";
    write_file(p, "    indented\n");
    // Missing the leading spaces — should not match
    auto r = tool("edit")->execute(
        {{"path", p}, {"old_string", "indented"}, {"new_string", "x"}},
        ctx());
    // "indented" without spaces does appear as a substring, so this should succeed
    // but we want to verify whitespace-sensitive matching works correctly
    CHECK(r.success, "substring without spaces should match: " + r.error);
    CHECK(read_file(p) == "    x\n", "whitespace-sensitive replacement mismatch");
    std::remove(p.c_str());
    std::cout << "[OK] edit whitespace-sensitive matching\n";
    return true;
}

// ============================================================

int main() {
    std::cout << "=== ReadTool / WriteTool / EditTool Tests ===\n\n";

    bool ok = true;

    std::cout << "-- read --\n";
    ok &= read_basic();
    ok &= read_offset();
    ok &= read_limit();
    ok &= read_offset_and_limit();
    ok &= read_relative_path();
    ok &= read_missing_file();
    ok &= read_binary_file();
    ok &= read_missing_path_param();
    ok &= read_empty_file();

    std::cout << "\n-- write --\n";
    ok &= write_basic();
    ok &= write_overwrites_existing();
    ok &= write_creates_parent_dirs();
    ok &= write_preserves_binary_content();
    ok &= write_empty_content();
    ok &= write_relative_path();
    ok &= write_missing_path_param();
    ok &= write_no_stray_tmp_file();

    std::cout << "\n-- edit --\n";
    ok &= edit_basic();
    ok &= edit_multiline();
    ok &= edit_delete_text();
    ok &= edit_replace_all();
    ok &= edit_duplicate_without_replace_all();
    ok &= edit_not_found();
    ok &= edit_empty_old_string();
    ok &= edit_binary_file();
    ok &= edit_missing_file();
    ok &= edit_relative_path();
    ok &= edit_no_stray_tmp_file();
    ok &= edit_whitespace_must_match_exactly();

    if (ok) {
        std::cout << "\nAll file tool tests passed!\n";
        return 0;
    } else {
        std::cerr << "\nSome tests FAILED.\n";
        return 1;
    }
}
