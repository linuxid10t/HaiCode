#include <haicode/haicode.h>
#include <haicode/tool.h>
#include <haicode/util.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include "test_check.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <fs_attr.h>
#include <FindDirectory.h>
#include <Path.h>

// ---- Helpers ----

static haicode::ToolRegistry& reg() {
    static haicode::ToolRegistry r;
    static bool done = false;
    if (!done) { haicode::register_builtin_tools(r); done = true; }
    return r;
}

static std::shared_ptr<haicode::Tool> tool(const std::string& name) {
    auto t = reg().get(name);
    TEST_REQUIRE(t, "tool not found: " + name);
    return t;
}

static haicode::ToolContext ctx(const std::string& dir = "/tmp") {
    haicode::ToolContext c;
    c.working_dir = dir;
    return c;
}

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    TEST_REQUIRE(f.is_open(), "failed to open " + path);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// Probe writability of a location: true = write succeeded (caller decides
// what that means); false = not writable. Never throws or aborts.
static bool write_file_quietly(const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f << "x";
    f.close();
    if (!f) {
        std::remove(path.c_str());
        return false;
    }
    std::remove(path.c_str());
    return true;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Count entries in `dir` whose name starts with `prefix`. Temp names are
// random now (mkstemp), so leftovers must be detected by scan, not by a
// fixed filename.
static int count_files_with_prefix(const std::string& dir, const std::string& prefix) {
    DIR* d = opendir(dir.c_str());
    if (!d) return -1;
    int n = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0)
            ++n;
    }
    closedir(d);
    return n;
}

// Resolve the system temp directory exactly as DiffTool does, so leftover
// scans look where the tool actually creates its scratch file.
static std::string system_temp_dir() {
    BPath temp_path;
    if (find_directory(B_SYSTEM_TEMP_DIRECTORY, &temp_path) == B_OK
            && temp_path.Path())
        return temp_path.Path();
    return "/tmp";
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
    const std::string p = "/tmp/tft_write_notmp.txt";
    std::remove(p.c_str());
    auto r = tool("write")->execute({{"path", p}, {"content", "clean\n"}}, ctx());
    CHECK(r.success, r.error);
    CHECK(count_files_with_prefix("/tmp", "tft_write_notmp.txt.tmp_write_") == 0,
          "temp file left behind after successful write");
    std::remove(p.c_str());
    std::cout << "[OK] write leaves no stray temp file\n";
    return true;
}

// Regression (review #5): a write call with only `path` used to default
// content to "" and empty the target file.
static bool write_missing_content_param_leaves_file_intact() {
    const std::string p = "/tmp/tft_write_nocontent.txt";
    write_file(p, "precious data\n");
    auto r = tool("write")->execute({{"path", p}}, ctx());
    CHECK(!r.success, "write without content must fail");
    CHECK(r.error.find("content") != std::string::npos,
          "error must name the missing field, got: " + r.error);
    CHECK(read_file(p) == "precious data\n",
          "file must be intact after rejected write");
    std::remove(p.c_str());
    std::cout << "[OK] write without content param leaves file intact\n";
    return true;
}

// Regression (review #9): atomic replacement dropped the execute bits.
static bool write_preserves_mode_0755() {
    const std::string p = "/tmp/tft_write_mode.sh";
    write_file(p, "#!/bin/sh\n");
    chmod(p.c_str(), 0755);
    auto r = tool("write")->execute({{"path", p}, {"content", "#!/bin/sh\nexit 0\n"}}, ctx());
    CHECK(r.success, r.error);
    struct stat st{};
    CHECK(stat(p.c_str(), &st) == 0, "stat failed");
    CHECK((st.st_mode & 07777) == 0755, "write must preserve 0755 mode");
    std::remove(p.c_str());
    std::cout << "[OK] write preserves 0755 mode\n";
    return true;
}

// Task 6: util::atomic_write_file mode semantics — an explicit mode wins
// over the preserved bits of a pre-existing target (secrets must tighten,
// never inherit 0644/0755), while the default keeps preserving them.
static bool util_explicit_mode_beats_preserved() {
    const std::string p = "/tmp/tft_atomic_secret.json";
    write_file(p, "{}");
    chmod(p.c_str(), 0755);
    std::string err = haicode::util::atomic_write_file(p, "{\"a\":1}\n", 0600);
    CHECK(err.empty(), ("atomic_write_file: " + err).c_str());
    struct stat st{};
    CHECK(stat(p.c_str(), &st) == 0, "stat failed");
    CHECK((st.st_mode & 07777) == 0600, "explicit 0600 must beat preserved 0755");
    std::remove(p.c_str());
    std::cout << "[OK] atomic_write_file explicit mode beats preserved bits\n";
    return true;
}

static bool util_default_mode_still_preserves() {
    const std::string p = "/tmp/tft_atomic_preserve.sh";
    write_file(p, "#!/bin/sh\n");
    chmod(p.c_str(), 0755);
    std::string err = haicode::util::atomic_write_file(p, "#!/bin/sh\nexit 0\n");
    CHECK(err.empty(), ("atomic_write_file: " + err).c_str());
    struct stat st{};
    CHECK(stat(p.c_str(), &st) == 0, "stat failed");
    CHECK((st.st_mode & 07777) == 0755, "default write must still preserve 0755");
    std::remove(p.c_str());

    // New file via the default path lands at 0644.
    const std::string q = "/tmp/tft_atomic_new.json";
    std::remove(q.c_str());
    err = haicode::util::atomic_write_file(q, "{}\n");
    CHECK(err.empty(), ("atomic_write_file: " + err).c_str());
    CHECK(stat(q.c_str(), &st) == 0, "stat failed");
    CHECK((st.st_mode & 07777) == 0644, "new file via default mode is 0644");
    std::remove(q.c_str());
    std::cout << "[OK] atomic_write_file default mode still preserves\n";
    return true;
}

// Task 6: startup tightening helper — chmod to owner-only only when
// group/world bits are present; no-op for already-tight and missing files.
static bool util_ensure_owner_only() {
    const std::string p = "/tmp/tft_ensure_owner.json";

    write_file(p, "{}");
    chmod(p.c_str(), 0644);
    CHECK(haicode::util::ensure_owner_only(p).empty(), "ensure_owner_only 0644");
    struct stat st{};
    CHECK(stat(p.c_str(), &st) == 0, "stat failed");
    CHECK((st.st_mode & 07777) == 0600, "0644 must tighten to 0600");

    chmod(p.c_str(), 0600);
    CHECK(haicode::util::ensure_owner_only(p).empty(), "ensure_owner_only 0600");
    CHECK(stat(p.c_str(), &st) == 0, "stat failed");
    CHECK((st.st_mode & 07777) == 0600, "0600 stays 0600 (no churn)");

    chmod(p.c_str(), 0666);
    CHECK(haicode::util::ensure_owner_only(p).empty(), "ensure_owner_only 0666");
    CHECK(stat(p.c_str(), &st) == 0, "stat failed");
    CHECK((st.st_mode & 07777) == 0600, "0666 tightens to 0600");

    std::remove(p.c_str());
    CHECK(haicode::util::ensure_owner_only(p).empty(),
          "missing file is a silent no-op");
    std::cout << "[OK] util::ensure_owner_only tightens only when needed\n";
    return true;
}

// Task 21: a rename-based replacement drops every BFS attribute (MIME type,
// Tracker metadata) — atomic_write_file must copy them onto the new file.
static bool atomic_write_preserves_bfs_attributes() {
    const std::string p = "/tmp/tft_atomic_attrs.txt";
    write_file(p, "v1\n");

    // Plant a typed attribute like Tracker would: BEOS:TYPE holds a
    // NUL-terminated MIME string with type code 'MIMS'.
    const uint32_t kMime = 0x4D494D53;  // "MIMS"
    const std::string mime = "text/x-haicode-test";
    int fd = open(p.c_str(), O_RDONLY);
    CHECK(fd >= 0, "open for attr write failed");
    ssize_t aw = fs_write_attr(fd, "BEOS:TYPE", kMime, 0, mime.c_str(),
                               mime.size() + 1);
    close(fd);
    CHECK(aw == (ssize_t)(mime.size() + 1), "planting BEOS:TYPE failed");

    std::string err = haicode::util::atomic_write_file(p, "v2\n");
    CHECK(err.empty(), ("atomic_write_file: " + err).c_str());
    CHECK(read_file(p) == "v2\n", "data must be the new content");

    fd = open(p.c_str(), O_RDONLY);
    CHECK(fd >= 0, "open for attr read failed");
    attr_info info{};
    CHECK(fs_stat_attr(fd, "BEOS:TYPE", &info) == 0,
          "BEOS:TYPE must survive the rewrite");
    CHECK(info.type == kMime, "attribute type must be preserved");
    char buf[64] = {0};
    ssize_t n = fs_read_attr(fd, "BEOS:TYPE", info.type, 0, buf, sizeof(buf));
    close(fd);
    CHECK(n == (ssize_t)(mime.size() + 1), "attribute size must be preserved");
    CHECK(std::string(buf, n) == std::string(mime.c_str(), mime.size() + 1),
          "attribute bytes must be preserved");

    std::remove(p.c_str());
    std::cout << "[OK] atomic_write_file preserves BFS attributes\n";
    return true;
}

// Task 21: writing through a symlink must update the target and keep the
// link — the old rename replaced the link with a regular file.
static bool atomic_write_follows_symlink() {
    const std::string real = "/tmp/tft_atomic_real.txt";
    const std::string link = "/tmp/tft_atomic_link.txt";
    std::remove(link.c_str());
    std::remove(real.c_str());
    write_file(real, "old\n");
    CHECK(symlink(real.c_str(), link.c_str()) == 0, "symlink failed");

    std::string err = haicode::util::atomic_write_file(link, "through the link\n");
    CHECK(err.empty(), ("atomic_write_file: " + err).c_str());
    CHECK(read_file(real) == "through the link\n",
          "write through symlink must land in the target");
    struct stat st{};
    CHECK(lstat(link.c_str(), &st) == 0, "lstat on link failed");
    CHECK(S_ISLNK(st.st_mode), "symlink must survive the write");

    std::remove(link.c_str());
    std::remove(real.c_str());
    std::cout << "[OK] atomic_write_file writes through symlinks\n";
    return true;
}

// Task 21: a dangling symlink must be refused, not silently destroyed (the
// old rename replaced the broken link with a regular file at the link path).
static bool atomic_write_broken_symlink_refused() {
    const std::string target = "/tmp/tft_atomic_missing_target.txt";
    const std::string link = "/tmp/tft_atomic_broken.txt";
    std::remove(link.c_str());
    std::remove(target.c_str());
    CHECK(symlink(target.c_str(), link.c_str()) == 0, "symlink failed");

    std::string err = haicode::util::atomic_write_file(link, "must not land\n");
    CHECK(!err.empty(), "broken symlink must be refused with an error");

    struct stat st{};
    CHECK(lstat(link.c_str(), &st) == 0, "lstat on link failed");
    CHECK(S_ISLNK(st.st_mode), "broken symlink must stay a symlink");
    CHECK(stat(target.c_str(), &st) != 0, "dangling target must stay absent");

    std::remove(link.c_str());
    std::remove(target.c_str());  // paranoia: nothing may have appeared
    std::cout << "[OK] atomic_write_file refuses broken symlinks\n";
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
    const std::string p = "/tmp/tft_edit_notmp.txt";
    write_file(p, "original\n");
    auto r = tool("edit")->execute(
        {{"path", p}, {"old_string", "original"}, {"new_string", "replaced"}},
        ctx());
    CHECK(r.success, r.error);
    CHECK(count_files_with_prefix("/tmp", "tft_edit_notmp.txt.tmp_write_") == 0,
          "temp file left behind after successful edit");
    std::remove(p.c_str());
    std::cout << "[OK] edit leaves no stray temp file\n";
    return true;
}

// Regression (review #5): missing new_string used to default to "" — deletion.
static bool edit_missing_new_string_param_leaves_file_intact() {
    const std::string p = "/tmp/tft_edit_nonew.txt";
    write_file(p, "keep this line\n");
    auto r = tool("edit")->execute({{"path", p}, {"old_string", "keep"}}, ctx());
    CHECK(!r.success, "edit without new_string must fail");
    CHECK(r.error.find("new_string") != std::string::npos,
          "error must name the missing field, got: " + r.error);
    CHECK(read_file(p) == "keep this line\n",
          "file must be unchanged after rejected edit");
    std::remove(p.c_str());
    std::cout << "[OK] edit without new_string param leaves file unchanged\n";
    return true;
}

static bool edit_preserves_mode_0755() {
    const std::string p = "/tmp/tft_edit_mode.sh";
    write_file(p, "#!/bin/sh\necho v1\n");
    chmod(p.c_str(), 0755);
    auto r = tool("edit")->execute(
        {{"path", p}, {"old_string", "v1"}, {"new_string", "v2"}}, ctx());
    CHECK(r.success, r.error);
    struct stat st{};
    CHECK(stat(p.c_str(), &st) == 0, "stat failed");
    CHECK((st.st_mode & 07777) == 0755, "edit must preserve 0755 mode");
    std::remove(p.c_str());
    std::cout << "[OK] edit preserves 0755 mode\n";
    return true;
}

// Regression (review #2): DiffTool used the predictable name <path>.tmp_diff
// and unlinked it — a read-only preview destroyed a pre-existing sibling.
static bool diff_preserves_sibling_tmp_diff_file() {
    const std::string p        = "/tmp/tft_diff_sib.txt";
    const std::string sentinel = p + ".tmp_diff";
    write_file(p, "original line\n");
    write_file(sentinel, "sentinel payload\n");
    auto r = tool("diff")->execute({{"path", p}, {"content", "changed line\n"}}, ctx());
    CHECK(r.success, r.error);
    CHECK(r.output.find("+changed line") != std::string::npos, "diff output expected");
    CHECK(read_file(sentinel) == "sentinel payload\n",
          "pre-existing .tmp_diff sibling must survive a diff preview byte-identical");
    CHECK(count_files_with_prefix("/tmp", "tft_diff_sib.txt.tmp_diff_") == 0,
          "diff must leave no scratch files behind");
    CHECK(count_files_with_prefix(system_temp_dir(), "haicode_diff_") == 0,
          "diff must leave no haicode_diff_ scratch in the temp dir");
    std::remove(p.c_str());
    std::remove(sentinel.c_str());
    std::cout << "[OK] diff preserves pre-existing .tmp_diff sibling\n";
    return true;
}

// Task 22: the scratch file must live in the system temp directory, never
// beside the target — diff previews run against read-only locations (Plan
// mode over the packagefs-mounted system headers). chmod cannot simulate
// this (the default Haiku user is uid 0 and bypasses permission bits), so
// the test targets the genuinely read-only packagefs.
static bool diff_scratch_in_readonly_dir() {
    // Probe: if this location is somehow writable, the test cannot assert
    // anything meaningful — fail loudly rather than pass vacuously.
    const std::string probe = "/boot/system/develop/headers/os/kernel/.tft_ro_probe";
    if (write_file_quietly(probe)) {
        std::cerr << "[FAIL] " << probe << " is writable; read-only premise broken\n";
        return false;
    }

    const std::string ro_file = "/boot/system/develop/headers/os/kernel/fs_attr.h";
    auto r = tool("diff")->execute(
        {{"path", ro_file}, {"content", "totally different\n"}}, ctx());
    CHECK(r.success,
          "diff against a read-only location must succeed, got: " + r.error);
    CHECK(r.output.find("+totally different") != std::string::npos,
          "diff output expected");

    // No scratch beside the read-only target, none left in the temp dir.
    CHECK(count_files_with_prefix("/boot/system/develop/headers/os/kernel",
                                  "fs_attr.h.tmp_diff_") == 0,
          "no scratch may be created beside the target");
    CHECK(count_files_with_prefix(system_temp_dir(), "haicode_diff_") == 0,
          "diff must leave no haicode_diff_ scratch in the temp dir");
    std::cout << "[OK] diff scratch lives in the temp dir (read-only target works)\n";
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
    ok &= write_missing_content_param_leaves_file_intact();
    ok &= write_preserves_mode_0755();

    std::cout << "\n-- atomic write modes (util) --\n";
    ok &= util_explicit_mode_beats_preserved();
    ok &= util_default_mode_still_preserves();
    ok &= util_ensure_owner_only();
    ok &= atomic_write_preserves_bfs_attributes();
    ok &= atomic_write_follows_symlink();
    ok &= atomic_write_broken_symlink_refused();

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
    ok &= edit_missing_new_string_param_leaves_file_intact();
    ok &= edit_preserves_mode_0755();
    ok &= diff_preserves_sibling_tmp_diff_file();
    ok &= diff_scratch_in_readonly_dir();
    ok &= edit_whitespace_must_match_exactly();

    if (ok) {
        std::cout << "\nAll file tool tests passed!\n";
        return 0;
    } else {
        std::cerr << "\nSome tests FAILED.\n";
        return 1;
    }
}
