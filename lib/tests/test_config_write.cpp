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

// Task 6: config.json carries provider API keys — every write through this
// path must be owner-only, including over a pre-existing loose file.
static bool writes_owner_only() {
    reset_dir();
    const std::string p = std::string(kDir) + "/secret.json";
    write_file(p, "{}");
    chmod(p.c_str(), 0644);
    std::string err;
    bool ok = haicode::update_config_file(p, [](nlohmann::json& j) {
        j["providers"] = nlohmann::json::object();
    }, err);
    CHECK(ok, "update should succeed: " + err);
    struct stat st;
    CHECK(::stat(p.c_str(), &st) == 0, "stat failed");
    CHECK((st.st_mode & 07777) == 0600, "config write must be 0600, was "
          + std::to_string(st.st_mode & 07777));

    // And a fresh file (no prior loose bits to worry about).
    const std::string q = std::string(kDir) + "/fresh.json";
    ok = haicode::update_config_file(q, [](nlohmann::json& j) {
        j["model"] = "m";
    }, err);
    CHECK(ok, "fresh update should succeed: " + err);
    CHECK(::stat(q.c_str(), &st) == 0, "stat failed on fresh file");
    CHECK((st.st_mode & 07777) == 0600, "fresh config write must be 0600");
    std::cout << "[OK] update_config_file writes owner-only (0600)\n";
    return true;
}

// Task 8 regression: a Settings save (sync_global_scope) must never leak
// project-owned state into the global file. Seeded like the real setup:
// global file with providers + last_directory + permissions, project file
// with its own build_command and providers entry.
static bool settings_save_does_not_leak_project_state() {
    reset_dir();
    const std::string gp = std::string(kDir) + "/global.json";
    const std::string pp = std::string(kDir) + "/proj/.haicode/config.json";
    ::mkdir((std::string(kDir) + "/proj").c_str(), 0755);
    ::mkdir((std::string(kDir) + "/proj/.haicode").c_str(), 0755);
    write_file(gp, R"({
  "last_directory": "/boot/home/Desktop/HaiCode",
  "permissions": [{"action": "bash", "resource": "*", "effect": "ask"}],
  "providers": {"anthropic": {"type": "anthropic", "api_key": "sk-global"}}
})");
    write_file(pp, R"({
  "build_command": "make -C build -j4 2>&1",
  "providers": {"evil-proxy": {"type": "openai", "base_url": "http://localhost:9/v1"}}
})");

    // What the GUI holds as the global layer after a Settings save.
    haicode::AppConfig g = haicode::load_layer(gp).values;
    g.model = "claude-sonnet-4";

    std::string err;
    CHECK(haicode::sync_global_scope(g, gp, err), "sync should succeed: " + err);

    nlohmann::json j = nlohmann::json::parse(read_file(gp), nullptr, false);
    CHECK(!j.is_discarded(), "global file must still parse");
    CHECK(!j.contains("build_command"),
          "global file must never gain a build_command");
    CHECK(j["providers"].size() == 1 && j["providers"].contains("anthropic"),
          "global providers must be exactly the global ones (no project entries)");
    CHECK(j["model"] == "claude-sonnet-4", "saved global key applied");
    CHECK(j["last_directory"] == "/boot/home/Desktop/HaiCode",
          "unowned key (last_directory) preserved");
    CHECK(j["permissions"].is_array() && j["permissions"].size() == 1,
          "permissions preserved untouched");

    // The project file is untouched by a global save.
    nlohmann::json pj = nlohmann::json::parse(read_file(pp), nullptr, false);
    CHECK(!pj.is_discarded(), "project file must still parse");
    CHECK(pj["build_command"] == "make -C build -j4 2>&1",
          "project build_command stays in the project file");
    CHECK(pj["providers"].contains("evil-proxy"),
          "project providers stay in the project file");

    // A legacy global build_command is dropped (project-only now), and a
    // stale model is erased when the layer no longer sets it.
    write_file(gp, R"({"build_command":"make legacy","model":"old","future":1})");
    haicode::AppConfig g2;
    CHECK(haicode::sync_global_scope(g2, gp, err), "second sync: " + err);
    j = nlohmann::json::parse(read_file(gp), nullptr, false);
    CHECK(!j.contains("build_command"), "legacy global build_command dropped");
    CHECK(!j.contains("model"), "unset model erased");
    CHECK(j["future"] == 1, "unknown keys still preserved");
    std::cout << "[OK] settings save does not leak project state\n";
    return true;
}

// thinking_display default is "off": an absent key parses to the empty
// "unset" sentinel (consumers fall back to collapsed), explicit values
// parse unchanged, and the save path erases the key from the global file
// when the layer leaves it unset (the default is never written).
static bool thinking_display_defaults_to_off() {
    reset_dir();
    const std::string p = std::string(kDir) + "/thinking.json";

    // Absent key → empty (unset); explicit values survive the parser.
    write_file(p, R"({"model":"m"})");
    auto cfg = haicode::ConfigLoader().load_file(p);
    CHECK(cfg.thinking_display.empty(),
          "absent thinking_display must stay unset (empty)");
    write_file(p, R"({"thinking_display":"on_while_thinking"})");
    cfg = haicode::ConfigLoader().load_file(p);
    CHECK(cfg.thinking_display == "on_while_thinking",
          "explicit saved preference must not be coerced to the default");
    write_file(p, R"({"thinking_display":"off"})");
    cfg = haicode::ConfigLoader().load_file(p);
    CHECK(cfg.thinking_display == "off", "explicit off survives the parser");

    // Serialization: default (empty) omits the key; explicit values emit it.
    haicode::AppConfig g;
    CHECK(!haicode::global_scope_json(g).contains("thinking_display"),
          "default thinking_display must not be serialized");
    g.thinking_display = "on";
    CHECK(haicode::global_scope_json(g)["thinking_display"] == "on",
          "explicit on must be serialized");
    g.thinking_display = "on_while_thinking";
    CHECK(haicode::global_scope_json(g)["thinking_display"] == "on_while_thinking",
          "explicit on_while_thinking must be serialized (no longer the default)");

    // Sync hygiene: a file holding a stale explicit value is erased when
    // the layer is back to the default, and re-written when set to "on".
    const std::string gp = std::string(kDir) + "/global.json";
    write_file(gp, R"({"thinking_display":"on_while_thinking","future":1})");
    haicode::AppConfig gl = haicode::load_layer(gp).values;
    std::string err;
    CHECK(haicode::sync_global_scope(gl, gp, err), "sync: " + err);
    nlohmann::json j = nlohmann::json::parse(read_file(gp), nullptr, false);
    CHECK(j.contains("thinking_display"), "explicit layer value round-trips");
    haicode::AppConfig gdef;  // everything at default
    CHECK(haicode::sync_global_scope(gdef, gp, err), "default sync: " + err);
    j = nlohmann::json::parse(read_file(gp), nullptr, false);
    CHECK(!j.contains("thinking_display"),
          "default (off) must be erased from the global file");
    CHECK(j["future"] == 1, "unknown keys still preserved");
    gdef.thinking_display = "on";
    CHECK(haicode::sync_global_scope(gdef, gp, err), "on sync: " + err);
    j = nlohmann::json::parse(read_file(gp), nullptr, false);
    CHECK(j.contains("thinking_display")
            && j["thinking_display"] == "on",
          "explicit on written back to the global file");
    std::cout << "[OK] thinking_display defaults to off\n";
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
    ok &= writes_owner_only();
    ok &= settings_save_does_not_leak_project_state();
    ok &= thinking_display_defaults_to_off();
    if (ok) {
        std::cout << "\nAll config-write tests passed!\n";
        return 0;
    }
    std::cerr << "\nSome config-write tests FAILED.\n";
    return 1;
}
