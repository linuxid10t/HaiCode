// Task 5: update_config_file — the centralized, non-destructive config
// write path. Every config save in the GUI goes through it: the existing
// document is read, an unparseable file is refused (never overwritten),
// the mutate lambda touches only its keys, and the result lands via
// util::atomic_write_file (mkstemp + fsync + rename).
#include <haicode/config.h>
#include "test_check.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    TEST_REQUIRE(f.is_open(), "failed to open " + path);
    f << content;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string body((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    return body;
}

// Any leftover atomic-write scratch siblings (".tmp_write_XXXXXX") in dir?
static bool has_tmp_write_leftovers(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    struct dirent* ent;
    bool found = false;
    while ((ent = readdir(d)) != nullptr) {
        if (strstr(ent->d_name, ".tmp_write_")) found = true;
    }
    closedir(d);
    return found;
}

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << (msg) << "\n"; return false; } } while(0)

static const char* kDir = "/tmp/tcw_dir";

static void reset_dir() {
    std::string cmd = "rm -rf ";
    cmd += kDir;
    (void)system(cmd.c_str());
    ::mkdir(kDir, 0755);
}

// A missing file is created from {} and the mutation lands in it.
static bool missing_file_created() {
    reset_dir();
    const std::string p = std::string(kDir) + "/created.json";
    std::string err;
    bool ok = haicode::update_config_file(p, [](nlohmann::json& j) {
        j["model"] = "claude-sonnet-4";
    }, err);
    CHECK(ok, "update on missing file should succeed: " + err);
    auto cfg = haicode::ConfigLoader().load_file(p);
    CHECK(cfg.model == "claude-sonnet-4", "mutated key should be parsed back");
    CHECK(!has_tmp_write_leftovers(kDir), "no .tmp_write_ leftovers");
    std::cout << "[OK] update_config_file creates missing file\n";
    return true;
}

// Keys the mutation does not touch — including unknown-to-the-app ones —
// survive byte-for-byte in the rewritten document.
static bool unrelated_keys_preserved() {
    reset_dir();
    const std::string p = std::string(kDir) + "/preserve.json";
    write_file(p, R"({
  "model": "old-model",
  "provider": "anthropic",
  "future_key": {"nested": [1, 2, 3]},
  "last_directory": "/boot/home"
})");
    std::string err;
    bool ok = haicode::update_config_file(p, [](nlohmann::json& j) {
        j["model"] = "new-model";
    }, err);
    CHECK(ok, "update should succeed: " + err);
    nlohmann::json j = nlohmann::json::parse(read_file(p), nullptr, false);
    CHECK(!j.is_discarded(), "file must still parse");
    CHECK(j["model"] == "new-model", "mutated key applied");
    CHECK(j["provider"] == "anthropic", "known unrelated key preserved");
    CHECK(j["last_directory"] == "/boot/home", "unrelated string preserved");
    CHECK(j["future_key"]["nested"].size() == 3, "unknown key preserved");
    CHECK(!has_tmp_write_leftovers(kDir), "no .tmp_write_ leftovers");
    std::cout << "[OK] update_config_file preserves unrelated keys\n";
    return true;
}

// The regression this task exists for: a hand-edited typo must make the
// update fail AND leave the file byte-identical — never truncate it.
static bool invalid_json_refused_untouched() {
    reset_dir();
    const std::string p = std::string(kDir) + "/broken.json";
    const std::string broken = "{\n  \"model\": \"claude-sonnet-4\"  // no comma\n";
    write_file(p, broken);
    std::string err;
    bool ok = haicode::update_config_file(p, [](nlohmann::json& j) {
        j["provider"] = "anthropic";
    }, err);
    CHECK(!ok, "update on unparseable file must fail");
    CHECK(err.find("not valid JSON") != std::string::npos,
          "error should say the file is not valid JSON: " + err);
    CHECK(err.find(p) != std::string::npos, "error should name the path: " + err);
    CHECK(read_file(p) == broken, "file must be byte-identical after refusal");
    CHECK(!has_tmp_write_leftovers(kDir), "no .tmp_write_ leftovers");
    std::cout << "[OK] update_config_file refuses invalid JSON, file untouched\n";
    return true;
}

// Erasing a key inside the lambda removes it; nested objects merge by hand
// (the lambda owns the shape of its own subtree).
static bool mutate_can_erase_keys() {
    reset_dir();
    const std::string p = std::string(kDir) + "/erase.json";
    write_file(p, R"({"model":"m","build_command":"make all"})");
    std::string err;
    bool ok = haicode::update_config_file(p, [](nlohmann::json& j) {
        j.erase("build_command");
    }, err);
    CHECK(ok, "update should succeed: " + err);
    auto body = read_file(p);
    CHECK(body.find("build_command") == std::string::npos,
          "erased key must be gone");
    CHECK(body.find("\"model\"") != std::string::npos, "other key kept");
    std::cout << "[OK] update_config_file honors key erasure\n";
    return true;
}

// Parent directory of the config file may not exist yet (fresh project
// .haicode/ dir); the helper creates it.
static bool creates_parent_dirs() {
    reset_dir();
    const std::string p = std::string(kDir) + "/nested/deeper/config.json";
    std::string err;
    bool ok = haicode::update_config_file(p, [](nlohmann::json& j) {
        j["build_command"] = "make -C build -j4 2>&1";
    }, err);
    CHECK(ok, "update with missing parent dir should succeed: " + err);
    struct stat st;
    CHECK(::stat(p.c_str(), &st) == 0, "file created at nested path");
    auto cfg = haicode::ConfigLoader().load_file(p);
    CHECK(cfg.build_command == "make -C build -j4 2>&1", "value round-trips");
    std::cout << "[OK] update_config_file creates parent directories\n";
    return true;
}

// An empty path is a caller bug, not a silent success.
static bool empty_path_fails() {
    std::string err;
    CHECK(!haicode::update_config_file("", [](nlohmann::json&) {}, err),
          "empty path must fail");
    CHECK(!err.empty(), "error message set");
    std::cout << "[OK] update_config_file rejects empty path\n";
    return true;
}

int main() {
    bool ok = true;
    ok &= missing_file_created();
    ok &= unrelated_keys_preserved();
    ok &= invalid_json_refused_untouched();
    ok &= mutate_can_erase_keys();
    ok &= creates_parent_dirs();
    ok &= empty_path_fails();
    if (ok) {
        std::cout << "\nAll config-write tests passed!\n";
        return 0;
    }
    std::cerr << "\nSome config-write tests FAILED.\n";
    return 1;
}
