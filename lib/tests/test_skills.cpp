// Unit tests for skills: discovery, frontmatter parsing, prompt-block
// assembly, config parse/merge, and the update_skills store round-trip.
// All scratch files live under /tmp and are cleaned up.
#include <haicode/skills.h>
#include <haicode/config.h>
#include <haicode/db.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_fail; \
    } \
} while (0)

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

static void mkdirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/' && !cur.empty()) ::mkdir(cur.c_str(), 0755);
        cur += path[i];
    }
    ::mkdir(path.c_str(), 0755);
}

static void rm_rf(const std::string& path) {
    DIR* d = opendir(path.c_str());
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
            std::string child = path + "/" + ent->d_name;
            struct stat st;
            if (lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                rm_rf(child);
            else
                ::unlink(child.c_str());
        }
        closedir(d);
    }
    ::rmdir(path.c_str());
}

static void test_discovery() {
    std::string tmp = "/tmp/hc_test_skills_XXXXXX";
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", tmp.c_str());
    if (!mkdtemp(buf)) { CHECK(false); return; }
    tmp = buf;
    std::string gdir = tmp + "/global_skills";
    std::string proj = tmp + "/proj";
    mkdirs(gdir);
    mkdirs(proj + "/.haicode/skills");

    write_file(gdir + "/alpha.md",
        "---\nname: Alpha\ndescription: \"First skill\"\n---\nAlpha body.\n");
    write_file(gdir + "/beta.md", "Beta body; no frontmatter.\n");
    write_file(gdir + "/shadowed.md", "global version\n");
    write_file(gdir + "/ignored.txt", "not a skill\n");
    write_file(proj + "/.haicode/skills/shadowed.md",
        "---\nname: Shadow\n---\nproject version\n");
    write_file(proj + "/.haicode/skills/gamma.md",
        "---\nname: 'Gamma'\ndescription: quoted-single\n---\nGamma body.\n");

    setenv("HPCODE_SKILLS_DIR", gdir.c_str(), 1);
    auto skills = haicode::list_skills(proj);
    CHECK(skills.size() == 4);  // alpha, beta, gamma, shadowed (.txt skipped)
    // Case-sensitive sort by name: capitals sort before lowercase, and the
    // no-frontmatter fallback is the lowercase filename stem.
    CHECK(skills[0].name == "Alpha");
    CHECK(skills[0].description == "First skill");
    CHECK(skills[1].name == "Gamma");
    CHECK(skills[1].description == "quoted-single");
    CHECK(skills[2].name == "Shadow");         // project shadows global
    CHECK(skills[2].path == proj + "/.haicode/skills/shadowed.md");
    CHECK(skills[3].name == "beta");           // stem fallback
    CHECK(skills[3].description.empty());

    auto block = haicode::build_skills_block(
        proj, {"alpha.md", "shadowed.md", "missing.md"});
    CHECK(block.find("# Skills") != std::string::npos);
    CHECK(block.find("## Alpha (alpha.md)") != std::string::npos);
    CHECK(block.find("Alpha body.") != std::string::npos);
    CHECK(block.find("## Shadow (shadowed.md)") != std::string::npos);
    CHECK(block.find("project version") != std::string::npos);
    CHECK(block.find("global version") == std::string::npos);
    CHECK(block.find("name: Alpha") == std::string::npos);  // fm stripped
    CHECK(block.find("missing.md") == std::string::npos);   // skipped id

    CHECK(haicode::build_skills_block(proj, {}).empty());

    // Truncation: a skill bigger than the cap gets a marker.
    std::string big(80 * 1024, 'x');
    write_file(gdir + "/huge.md", big);
    auto trunc = haicode::build_skills_block(proj, {"huge.md"});
    CHECK(trunc.find("[skills truncated]") != std::string::npos);
    CHECK(trunc.size() < 80 * 1024);

    unsetenv("HPCODE_SKILLS_DIR");
    rm_rf(tmp);
}

static void test_config() {
    std::string tmp = "/tmp/hc_test_skills_cfg_XXXXXX";
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", tmp.c_str());
    if (!mkdtemp(buf)) { CHECK(false); return; }
    tmp = buf;
    std::string path = tmp + "/config.json";
    write_file(path,
        "{\"skills\": [\"a.md\", \"b.md\"], \"model\": \"m\"}");

    haicode::ConfigLoader loader;
    auto cfg = loader.load_file(path);
    CHECK(cfg.default_skills.size() == 2);
    CHECK(cfg.default_skills[0] == "a.md");
    CHECK(cfg.default_skills[1] == "b.md");

    haicode::AppConfig overlay;
    overlay.default_skills = {"b.md", "c.md"};
    auto merged = loader.merge(cfg, overlay);
    CHECK(merged.default_skills.size() == 3);  // dedup on "b.md"
    CHECK(merged.default_skills[2] == "c.md");

    rm_rf(tmp);
}

static void test_store() {
    haicode::Database db(":memory:");
    db.migrate();
    haicode::SessionStore store(db);
    auto s = store.create("/tmp/proj", "main",
                          "{\"id\":\"m\",\"provider_id\":\"anthropic\","
                          "\"mode\":\"plan\"}");
    std::vector<std::string> skills = {"one.md", "two.md"};
    store.update_skills(s.id, skills);
    auto got = store.get(s.id);
    CHECK(got.has_value());
    // model_json still parses and keeps the other fields.
    CHECK(got->model_json.find("\"mode\":\"plan\"") != std::string::npos);
    CHECK(got->model_json.find("one.md") != std::string::npos);
    CHECK(got->model_json.find("two.md") != std::string::npos);

    // Replace with an empty list: key becomes [].
    store.update_skills(s.id, {});
    got = store.get(s.id);
    CHECK(got->model_json.find("\"skills\":[]") != std::string::npos);
    CHECK(got->model_json.find("\"mode\":\"plan\"") != std::string::npos);

    // Nonexistent session: no-op, no crash.
    store.update_skills("nope", skills);
}

int main() {
    test_discovery();
    test_config();
    test_store();
    if (g_fail == 0) {
        printf("test_skills: ALL PASSED\n");
        return 0;
    }
    printf("test_skills: %d FAILURES\n", g_fail);
    return 1;
}
