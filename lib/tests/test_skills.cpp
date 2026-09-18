// Unit tests for skills: discovery, frontmatter parsing, prompt-block
// assembly, config parse/merge, and the update_skills store round-trip.
// All scratch files live under /tmp and are cleaned up.
#include <haicode/skills.h>
#include <haicode/config.h>
#include <haicode/db.h>
#include <haicode/engine.h>
#include <haicode/provider.h>
#include <haicode/tool.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
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

    // Directory skills (agentskills.io pack layout): SKILL.md inside a
    // subdirectory, discovered recursively. Id = parent dir name.
    // (mkdirs first — write_file/ofstream does not create parents.)
    mkdirs(gdir + "/pack/skills/dirskill");
    write_file(gdir + "/pack/skills/dirskill/SKILL.md",
        "---\nname: DirSkill\ndescription: >\n  Folded description line one\n"
        "  and line two.\n---\nDirSkill body.\n");
    // Duplicate dir-skill name deeper in a pack: nearest-to-root wins.
    mkdirs(gdir + "/pack/plugins/x/skills/dirskill");
    write_file(gdir + "/pack/plugins/x/skills/dirskill/SKILL.md",
        "deep duplicate; must lose\n");
    // Nested non-SKILL.md docs are not skills.
    mkdirs(gdir + "/pack/skills/other");
    write_file(gdir + "/pack/skills/other/README.md", "doc; not a skill\n");

    setenv("HPCODE_SKILLS_DIR", gdir.c_str(), 1);
    auto skills = haicode::list_skills(proj);
    CHECK(skills.size() == 5);  // alpha, beta, gamma, shadowed, dirskill
    // Case-sensitive sort by name: capitals sort before lowercase, and the
    // no-frontmatter fallback is the lowercase filename stem.
    CHECK(skills[0].name == "Alpha");
    CHECK(skills[0].description == "First skill");
    CHECK(skills[1].name == "DirSkill");
    CHECK(skills[1].id == "dirskill");
    CHECK(skills[1].description ==
          "Folded description line one and line two.");
    CHECK(skills[1].path.find("/pack/skills/dirskill/SKILL.md")
          != std::string::npos);  // canonical, not the plugins/ duplicate
    CHECK(skills[2].name == "Gamma");
    CHECK(skills[2].description == "quoted-single");
    CHECK(skills[3].name == "Shadow");         // project shadows global
    CHECK(skills[3].path == proj + "/.haicode/skills/shadowed.md");
    CHECK(skills[4].name == "beta");           // stem fallback
    CHECK(skills[4].description.empty());

    auto block = haicode::build_skills_block(
        proj, {"alpha.md", "shadowed.md", "dirskill", "missing.md"});
    CHECK(block.find("# Skills") != std::string::npos);
    // Directive framing: the block must command application, not just list.
    CHECK(block.find("active operating instructions") != std::string::npos);
    CHECK(block.find("## Alpha (alpha.md)") != std::string::npos);
    CHECK(block.find("Alpha body.") != std::string::npos);
    CHECK(block.find("When to use: First skill") != std::string::npos);
    CHECK(block.find("## Shadow (shadowed.md)") != std::string::npos);
    CHECK(block.find("project version") != std::string::npos);
    CHECK(block.find("global version") == std::string::npos);
    CHECK(block.find("name: Alpha") == std::string::npos);  // fm stripped
    CHECK(block.find("## DirSkill (dirskill)") != std::string::npos);
    CHECK(block.find("DirSkill body.") != std::string::npos);
    // Folded frontmatter description is now emitted as a when-to-use line.
    CHECK(block.find("When to use: Folded description line one and line two.")
          != std::string::npos);
    CHECK(block.find("deep duplicate") == std::string::npos);
    CHECK(block.find("missing.md") == std::string::npos);   // skipped id

    // Location line: absolute file path plus the skill directory, so
    // relative resources resolve against the skill, not the project.
    CHECK(block.find("Location: " + gdir + "/alpha.md")
          != std::string::npos);
    CHECK(block.find("skill directory: " + gdir) != std::string::npos);
    CHECK(block.find("Location: " + gdir + "/pack/skills/dirskill/SKILL.md")
          != std::string::npos);
    CHECK(block.find("skill directory: " + gdir + "/pack/skills/dirskill")
          != std::string::npos);
    CHECK(block.find("Location: " + proj + "/.haicode/skills/shadowed.md")
          != std::string::npos);                     // project wins shadowing

    // Mode capability notes appear only in tool-stripped modes.
    auto chat_block = haicode::build_skills_block(proj, {"alpha.md"}, "chat");
    CHECK(chat_block.find("Chat mode") != std::string::npos);
    auto plan_block = haicode::build_skills_block(proj, {"alpha.md"}, "plan");
    CHECK(plan_block.find("Plan mode") != std::string::npos);
    CHECK(block.find("Chat mode") == std::string::npos);  // default = build
    CHECK(block.find("Plan mode") == std::string::npos);

    CHECK(haicode::build_skills_block(proj, {}).empty());

    // Oversized skill: omitted individually with a marker; a smaller skill
    // enabled after it still lands (no truncate-all).
    std::string big(80 * 1024, 'x');
    write_file(gdir + "/huge.md", big);
    auto trunc = haicode::build_skills_block(proj, {"huge.md", "beta.md"});
    CHECK(trunc.find("[skill 'huge.md' omitted: exceeds the skills block "
                     "size cap]") != std::string::npos);
    CHECK(trunc.find("Beta body; no frontmatter.") != std::string::npos);
    CHECK(trunc.size() < 80 * 1024);

    // Two skills that together exceed the cap: first fits, second is
    // omitted with the cap-reached marker.
    std::string half(40 * 1024, 'y');
    write_file(gdir + "/half1.md", half);
    write_file(gdir + "/half2.md", half);
    auto capped = haicode::build_skills_block(proj, {"half1.md", "half2.md"});
    CHECK(capped.find(std::string(100, 'y')) != std::string::npos);
    CHECK(capped.find("[skill 'half2.md' omitted: skills block size cap "
                      "reached]") != std::string::npos);
    CHECK(capped.find(std::string(40 * 1024 + 100, 'y')) == std::string::npos);

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

static void test_parse_invocation() {
    std::string tmp = "/tmp/hc_test_skills_inv_XXXXXX";
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", tmp.c_str());
    if (!mkdtemp(buf)) { CHECK(false); return; }
    tmp = buf;
    std::string gdir = tmp + "/global_skills";
    std::string proj = tmp + "/proj";
    mkdirs(gdir);
    mkdirs(proj + "/.haicode/skills");
    write_file(gdir + "/alpha.md",
        "---\nname: Alpha\ndescription: \"First\"\n---\nAlpha body.\n");
    write_file(gdir + "/beta.md", "Beta body.\n");
    mkdirs(gdir + "/pack/skills/dirskill");
    write_file(gdir + "/pack/skills/dirskill/SKILL.md", "DirSkill body.\n");
    write_file(proj + "/.haicode/skills/gamma.md", "Gamma body.\n");

    setenv("HPCODE_SKILLS_DIR", gdir.c_str(), 1);

    haicode::SkillInfo sk;
    std::string args;

    CHECK(parse_skill_invocation(proj, "/dirskill make it so", sk, args));
    CHECK(sk.id == "dirskill");
    CHECK(args == "make it so");

    CHECK(parse_skill_invocation(proj, "/alpha.md fix this", sk, args));
    CHECK(sk.id == "alpha.md");            // exact-id tier (filename is the id)
    CHECK(args == "fix this");

    CHECK(parse_skill_invocation(proj, "/alpha fix this", sk, args));
    CHECK(sk.id == "alpha.md");            // stem tier
    CHECK(args == "fix this");

    CHECK(parse_skill_invocation(proj, "/gamma hi", sk, args));
    CHECK(sk.id == "gamma.md");            // project skill
    CHECK(sk.path == proj + "/.haicode/skills/gamma.md");
    CHECK(args == "hi");

    // Command only: match, empty args.
    CHECK(parse_skill_invocation(proj, "/alpha", sk, args));
    CHECK(sk.id == "alpha.md");
    CHECK(args.empty());

    // Leading whitespace tolerated.
    CHECK(parse_skill_invocation(proj, "  /alpha   spaced  ", sk, args));
    CHECK(sk.id == "alpha.md");
    CHECK(args == "spaced");

    // Non-matches pass through untouched.
    CHECK(!parse_skill_invocation(proj, "/nope hi", sk, args));
    CHECK(!parse_skill_invocation(proj, "hello /alpha", sk, args));
    CHECK(!parse_skill_invocation(proj, "/boot/home/README.md is the file",
                                  sk, args));
    CHECK(!parse_skill_invocation(proj, "/", sk, args));      // empty command
    CHECK(!parse_skill_invocation(proj, "", sk, args));
    CHECK(!parse_skill_invocation(proj, "/Alpha fix", sk, args)); // case-sensitive

    unsetenv("HPCODE_SKILLS_DIR");
    rm_rf(tmp);
}

static haicode::SessionMessage make_msg(int seq, const std::string& data) {
    haicode::SessionMessage m;
    m.type = "user_prompted";
    m.seq = seq;
    m.data_json = data;
    return m;
}

static const char* kSkillRow =
    "{\"role\":\"user\",\"text\":\"/alpha do the thing\","
    "\"skill\":\"alpha.md\",\"skill_args\":\"do the thing\","
    "\"skill_block\":\"\\n\\n# Skills\\n\\n## Alpha (alpha.md)\\n\\n"
    "Alpha body.\\n\"}";

static void test_assemble_skill_rows() {
    haicode::ContextBuilder builder;

    // Current-turn row carries the framed body; a past-turn copy of the
    // same row collapses to the compact marker (one-shot drop-off).
    std::vector<haicode::SessionMessage> msgs;
    msgs.push_back(make_msg(1, kSkillRow));
    msgs.push_back(make_msg(2, kSkillRow));
    auto out = builder.assemble_messages(msgs, true);
    CHECK(out.size() == 2);
    std::string past = out[0]["content"].get<std::string>();
    std::string cur  = out[1]["content"].get<std::string>();
    CHECK(past.find("no longer apply") != std::string::npos);
    CHECK(past.find("Alpha body.") == std::string::npos);
    CHECK(past.find("do the thing") != std::string::npos);   // args stay
    CHECK(cur.find("[skill invoked: /alpha") != std::string::npos);
    CHECK(cur.find("Alpha body.") != std::string::npos);
    CHECK(cur.find("do the thing") != std::string::npos);
    CHECK(cur.find("/alpha do the thing") == std::string::npos); // no raw cmd

    // Already active via the Skills tab: short note, no body.
    msgs.clear();
    msgs.push_back(make_msg(1,
        "{\"role\":\"user\",\"text\":\"/alpha go\",\"skill\":\"alpha.md\","
        "\"skill_args\":\"go\",\"skill_active\":true}"));
    out = builder.assemble_messages(msgs, true);
    cur = out[0]["content"].get<std::string>();
    CHECK(cur.find("already active this session") != std::string::npos);
    CHECK(cur.find("Alpha body.") == std::string::npos);
    CHECK(cur.find("go") != std::string::npos);

    // mode_notice rides first, skill block second, args last.
    std::string with_notice = std::string(kSkillRow);
    with_notice.insert(with_notice.size() - 1,
                       ",\"mode_notice\":\"[mode changed to plan]\"");
    msgs.clear();
    msgs.push_back(make_msg(1, with_notice));
    out = builder.assemble_messages(msgs, true);
    cur = out[0]["content"].get<std::string>();
    auto pos_notice = cur.find("[mode changed to plan]");
    auto pos_skill  = cur.find("[skill invoked: /alpha");
    auto pos_args   = cur.find("do the thing");
    CHECK(pos_notice != std::string::npos && pos_skill != std::string::npos
          && pos_args != std::string::npos);
    CHECK(pos_notice < pos_skill && pos_skill < pos_args);

    // Attachment path: skill text block + args block + image block.
    msgs.clear();
    msgs.push_back(make_msg(1,
        "{\"role\":\"user\",\"text\":\"/alpha look\",\"skill\":\"alpha.md\","
        "\"skill_args\":\"look\","
        "\"skill_block\":\"\\n\\n# Skills\\n\\n## Alpha (alpha.md)\\n\\n"
        "Alpha body.\\n\","
        "\"attachments\":[{\"media_type\":\"image/png\",\"path\":\"x.png\","
        "\"data_b64\":\"Zm9v\"}]}"));
    out = builder.assemble_messages(msgs, true);
    CHECK(out[0]["content"].is_array());
    auto& blocks = out[0]["content"];
    CHECK(blocks.size() == 3);
    CHECK(blocks[0].value("type", "") == "text");
    CHECK(blocks[0].value("text", "").find("[skill invoked: /alpha")
          != std::string::npos);
    CHECK(blocks[1].value("type", "") == "text");
    CHECK(blocks[1].value("text", "") == "look");
    CHECK(blocks[2].value("type", "") == "image");

    // Plain rows are untouched.
    msgs.clear();
    msgs.push_back(make_msg(1, "{\"role\":\"user\",\"text\":\"plain\"}"));
    out = builder.assemble_messages(msgs, true);
    CHECK(out[0]["content"].get<std::string>() == "plain");
}

// Mirrors test_compaction.cpp's FakeProvider: captures the last chat request.
class FakeProvider : public haicode::Provider {
public:
    std::string id() const override { return "fake"; }
    void cancel() override {}
    std::vector<std::string> list_models(std::string&) override {
        return {"fake-model"};
    }
    int get_model_context(const std::string&) const override { return 0; }
    void stream(const haicode::LLMRequest& req,
                haicode::StreamCallbacks cb) override {
        ++calls;
        last_chat_request = req;
        cb.on_text_delta("t", "ok");
        cb.on_finish(haicode::FinishReason::EndTurn, {}, {});
    }
    int calls = 0;
    haicode::LLMRequest last_chat_request;

    static std::string dump_messages(
            const std::vector<nlohmann::json>& msgs) {
        std::string out;
        for (const auto& m : msgs) out += m.dump();
        return out;
    }
};

static void test_engine_slash_e2e() {
    std::string tmp = "/tmp/hc_test_skills_e2e_XXXXXX";
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", tmp.c_str());
    if (!mkdtemp(buf)) { CHECK(false); return; }
    tmp = buf;
    std::string gdir = tmp + "/global_skills";  // empty: isolate from user's
    std::string proj = tmp + "/proj";
    mkdirs(gdir);
    mkdirs(proj + "/.haicode/skills");
    write_file(proj + "/.haicode/skills/alpha.md",
        "---\nname: Alpha\n---\nAlpha body.\n");
    setenv("HPCODE_SKILLS_DIR", gdir.c_str(), 1);

    std::string dbp = tmp + "/e2e.db";
    haicode::Database db(dbp);
    db.migrate();
    haicode::SessionStore store(db);
    auto provider = std::make_shared<FakeProvider>();
    haicode::ProviderRegistry registry;
    registry.register_provider(provider);
    haicode::ToolRegistry tools;
    haicode::PermissionGate perms;
    haicode::SessionEventBus bus;
    haicode::AppConfig cfg;
    cfg.model = "fake-model";
    cfg.provider = "fake";
    cfg.autoname_sessions = false;
    cfg.default_mode = "build";

    {
        haicode::SessionEngine engine(store, registry, tools, perms, bus, cfg);
        std::string sid = engine.create_session(proj, "build",
                                                "fake-model", "fake");

        engine.submit_prompt(sid, "/alpha do the thing");
        for (int i = 0; i < 100; ++i) {
            if (store.load_messages(sid).size() >= 2) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        CHECK(store.load_messages(sid).size() >= 2);
        std::string reqd = FakeProvider::dump_messages(
            provider->last_chat_request.messages);
        CHECK(reqd.find("[skill invoked: /alpha") != std::string::npos);
        CHECK(reqd.find("Alpha body.") != std::string::npos);
        // One-shot block carries the skill's location (submit_prompt →
        // ContextBuilder end to end).
        CHECK(reqd.find("Location: " + proj + "/.haicode/skills/alpha.md")
              != std::string::npos);
        CHECK(reqd.find("do the thing") != std::string::npos);
        CHECK(reqd.find("/alpha do the thing") == std::string::npos);

        // Second, plain turn: body drops off; only the compact marker
        // references the earlier invocation.
        engine.submit_prompt(sid, "plain second prompt");
        for (int i = 0; i < 100; ++i) {
            if (store.load_messages(sid).size() >= 4) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        CHECK(store.load_messages(sid).size() >= 4);
        std::string reqd2 = FakeProvider::dump_messages(
            provider->last_chat_request.messages);
        CHECK(reqd2.find("plain second prompt") != std::string::npos);
        CHECK(reqd2.find("Alpha body.") == std::string::npos);
        CHECK(reqd2.find("[skill invoked:") == std::string::npos);
        CHECK(reqd2.find("no longer apply") != std::string::npos);

        // The stored row keeps the verbatim command for transcript replay.
        auto rows = store.load_messages(sid);
        bool saw_raw = false;
        for (auto& r : rows) {
            if (r.type != "user_prompted") continue;
            auto d = nlohmann::json::parse(r.data_json, nullptr, false);
            if (d.is_object() && d.value("text", "") == "/alpha do the thing")
                saw_raw = true;
        }
        CHECK(saw_raw);
    }  // ~SessionEngine joins the loop threads

    unsetenv("HPCODE_SKILLS_DIR");
    rm_rf(tmp);
}

int main() {
    test_discovery();
    test_config();
    test_store();
    test_parse_invocation();
    test_assemble_skill_rows();
    test_engine_slash_e2e();
    if (g_fail == 0) {
        printf("test_skills: ALL PASSED\n");
        return 0;
    }
    printf("test_skills: %d FAILURES\n", g_fail);
    return 1;
}
