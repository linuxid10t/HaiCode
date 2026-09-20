// Tests for /retry: SessionStore::delete_messages_after (store unit) and
// SessionEngine::retry_last_turn (engine e2e — happy path, skill-block
// re-emission, refuse-while-running, no-prompt no-op).
#include <haicode/engine.h>
#include <haicode/db.h>
#include <haicode/provider.h>
#include <haicode/tool.h>
#include <haicode/config.h>
#include <haicode/skills.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
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

static bool wait_for_rows(haicode::SessionStore& store, const std::string& sid,
                          size_t want) {
    for (int i = 0; i < 200; ++i) {
        if (store.load_messages(sid).size() >= want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

static bool wait_not_running(haicode::SessionEngine& engine,
                             const std::string& sid) {
    for (int i = 0; i < 200; ++i) {
        if (!engine.is_running(sid)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

static size_t count_rows_of_type(haicode::SessionStore& store,
                                 const std::string& sid,
                                 const char* type) {
    size_t n = 0;
    for (const auto& m : store.load_messages(sid))
        if (m.type == type) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// Store unit: delete_messages_after
// ---------------------------------------------------------------------------

static void test_store_delete_after() {
    std::string tmp = "/tmp/hc_test_retry_store_XXXXXX";
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", tmp.c_str());
    if (!mkdtemp(buf)) { CHECK(false, "mkdtemp"); return; }
    tmp = buf;

    haicode::Database db(tmp + "/store.db");
    db.migrate();
    haicode::SessionStore store(db);
    haicode::SessionInfo si = store.create(tmp, "build", "{}");
    std::string sid = si.id;

    for (int i = 1; i <= 4; ++i) {
        nlohmann::json d;
        d["text"] = "m" + std::to_string(i);
        store.append_message(sid, i % 2 ? "user_prompted" : "assistant_text",
                             d.dump());
    }
    CHECK(store.load_messages(sid).size() == 4, "four rows appended");

    store.delete_messages_after(sid, 2);
    auto rows = store.load_messages(sid);
    CHECK(rows.size() == 2, "delete after seq 2 leaves two rows");
    CHECK(rows.size() == 2 && rows[0].seq == 1 && rows[1].seq == 2,
          "surviving rows are seq 1 and 2");
    CHECK(rows.size() == 2 && rows[0].data_json.find("\"m1\"") != std::string::npos
          && rows[1].data_json.find("\"m2\"") != std::string::npos,
          "surviving rows keep their payloads");

    // Boundary is strict: seq == N survives, only seq > N goes.
    store.delete_messages_after(sid, 2);
    CHECK(store.load_messages(sid).size() == 2, "re-delete after 2 is a no-op");

    store.delete_messages_after(sid, 0);
    CHECK(store.load_messages(sid).empty(), "delete after seq 0 clears all");

    rm_rf(tmp);
    std::cout << "[OK] store delete_messages_after (strict seq > N)\n";
}

// ---------------------------------------------------------------------------
// Fakes
// ---------------------------------------------------------------------------

// Mirrors test_skills.cpp's FakeProvider: immediate response, captures the
// last chat request.
class FakeProvider : public haicode::Provider {
public:
    std::string id() const override { return "fake"; }
    void cancel(const std::string& stream_token = "") override {}
    std::vector<std::string> list_models(std::string&) override {
        return {"fake-model"};
    }
    int get_model_context(const std::string&) const override { return 0; }
    void stream(const haicode::LLMRequest& req,
                haicode::StreamCallbacks cb, const std::string& stream_token = "") override {
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

// Parks inside stream() until the test releases it (test_step_budget.cpp's
// BlockingProvider shape, with a rearmable gate).
class GatedProvider : public haicode::Provider {
public:
    std::string id() const override { return "gated"; }
    void cancel(const std::string& stream_token = "") override {}
    std::vector<std::string> list_models(std::string&) override {
        return {"gated-model"};
    }
    int get_model_context(const std::string&) const override { return 0; }
    void stream(const haicode::LLMRequest&, haicode::StreamCallbacks cb,
                const std::string& stream_token = "") override {
        {
            std::lock_guard<std::mutex> g(m);
            ++entered;
        }
        entered_cv.notify_all();
        {
            std::unique_lock<std::mutex> lk(m);
            release_cv.wait(lk, [&] { return release_flag; });
            ++exited;
        }
        cb.on_text_delta("t", "ok");
        cb.on_finish(haicode::FinishReason::EndTurn, {}, {});
    }

    void release() {
        std::lock_guard<std::mutex> g(m);
        release_flag = true;
        release_cv.notify_all();
    }
    void rearm() {
        std::lock_guard<std::mutex> g(m);
        release_flag = false;
    }
    bool wait_entered(int want) {
        std::unique_lock<std::mutex> lk(m);
        return entered_cv.wait_for(lk, std::chrono::seconds(5),
                                   [&] { return entered >= want; });
    }

    std::mutex m;
    std::condition_variable entered_cv;
    std::condition_variable release_cv;
    int entered = 0;
    int exited = 0;
    bool release_flag = false;
};

// ---------------------------------------------------------------------------
// Engine e2e
// ---------------------------------------------------------------------------

static void test_retry_happy_path() {
    std::string tmp = "/tmp/hc_test_retry_e2e_XXXXXX";
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", tmp.c_str());
    if (!mkdtemp(buf)) { CHECK(false, "mkdtemp"); return; }
    tmp = buf;
    mkdirs(tmp + "/proj");

    haicode::Database db(tmp + "/e2e.db");
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
        std::string sid = engine.create_session(tmp + "/proj", "build",
                                                "fake-model", "fake");

        engine.submit_prompt(sid, "hello world");
        CHECK(wait_for_rows(store, sid, 2), "first turn completes");
        CHECK(wait_not_running(engine, sid), "first turn drains");
        CHECK(provider->calls == 1, "one stream call for the first turn");

        engine.retry_last_turn(sid);
        CHECK(wait_for_rows(store, sid, 2), "retried turn completes");
        CHECK(wait_not_running(engine, sid), "retried turn drains");
        CHECK(provider->calls == 2, "retry re-ran the provider");

        // The regenerated request still carries the original user text...
        std::string reqd = FakeProvider::dump_messages(
            provider->last_chat_request.messages);
        CHECK(reqd.find("hello world") != std::string::npos,
              "retried request carries the original prompt text");

        // ...and the prompt row was reused, not duplicated: exactly one
        // user_prompted and one assistant_text row survive.
        CHECK(count_rows_of_type(store, sid, "user_prompted") == 1,
              "exactly one user_prompted row after retry");
        CHECK(count_rows_of_type(store, sid, "assistant_text") == 1,
              "exactly one assistant_text row after retry");
        CHECK(store.load_messages(sid).size() == 2,
              "session has exactly two rows after retry");
    }  // ~SessionEngine joins the loop threads

    rm_rf(tmp);
    std::cout << "[OK] retry e2e happy path (assistant row regenerated, "
                 "prompt text survives)\n";
}

static void test_retry_skill_block() {
    std::string tmp = "/tmp/hc_test_retry_skill_XXXXXX";
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", tmp.c_str());
    if (!mkdtemp(buf)) { CHECK(false, "mkdtemp"); return; }
    tmp = buf;
    std::string gdir = tmp + "/global_skills";  // empty: isolate from user's
    std::string proj = tmp + "/proj";
    mkdirs(gdir);
    mkdirs(proj + "/.haicode/skills");
    write_file(proj + "/.haicode/skills/alpha.md",
        "---\nname: Alpha\n---\nAlpha body.\n");
    setenv("HPCODE_SKILLS_DIR", gdir.c_str(), 1);

    haicode::Database db(tmp + "/e2e.db");
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
        CHECK(wait_for_rows(store, sid, 2), "skill turn completes");
        CHECK(wait_not_running(engine, sid), "skill turn drains");

        engine.retry_last_turn(sid);
        CHECK(wait_for_rows(store, sid, 2), "retried skill turn completes");
        CHECK(wait_not_running(engine, sid), "retried skill turn drains");
        CHECK(provider->calls == 2, "retry re-ran the provider");

        // The one-shot skill block rides the persisted user_prompted row, so
        // the retried request re-emits it — the row is the last user prompt
        // again after the tail was deleted.
        std::string reqd = FakeProvider::dump_messages(
            provider->last_chat_request.messages);
        CHECK(reqd.find("[skill invoked: /alpha") != std::string::npos,
              "skill invocation frame re-emitted after retry");
        CHECK(reqd.find("Alpha body.") != std::string::npos,
              "skill body re-emitted after retry");
        CHECK(reqd.find("do the thing") != std::string::npos,
              "skill args re-emitted after retry");

        // The stored row keeps the verbatim command for transcript replay.
        bool saw_raw = false;
        for (auto& r : store.load_messages(sid)) {
            if (r.type != "user_prompted") continue;
            auto d = nlohmann::json::parse(r.data_json, nullptr, false);
            if (d.is_object() && d.value("text", "") == "/alpha do the thing")
                saw_raw = true;
        }
        CHECK(saw_raw, "stored prompt row keeps the verbatim /alpha command");
    }

    unsetenv("HPCODE_SKILLS_DIR");
    rm_rf(tmp);
    std::cout << "[OK] retry re-emits one-shot skill block from the stored "
                 "row\n";
}

static void test_retry_refused_while_running() {
    std::string tmp = "/tmp/hc_test_retry_running_XXXXXX";
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", tmp.c_str());
    if (!mkdtemp(buf)) { CHECK(false, "mkdtemp"); return; }
    tmp = buf;
    mkdirs(tmp + "/proj");

    haicode::Database db(tmp + "/e2e.db");
    db.migrate();
    haicode::SessionStore store(db);
    auto provider = std::make_shared<GatedProvider>();
    haicode::ProviderRegistry registry;
    registry.register_provider(provider);
    haicode::ToolRegistry tools;
    haicode::PermissionGate perms;
    haicode::SessionEventBus bus;
    haicode::AppConfig cfg;
    cfg.model = "gated-model";
    cfg.provider = "gated";
    cfg.autoname_sessions = false;
    cfg.default_mode = "build";

    {
        haicode::SessionEngine engine(store, registry, tools, perms, bus, cfg);
        std::string sid = engine.create_session(tmp + "/proj", "build",
                                                "gated-model", "gated");

        // Turn one: park, release, drain.
        engine.submit_prompt(sid, "turn one");
        CHECK(provider->wait_entered(1), "turn one parked in stream()");
        provider->release();
        CHECK(wait_for_rows(store, sid, 2), "turn one completes");
        CHECK(wait_not_running(engine, sid), "turn one drains");

        // Turn two: park and STAY parked — the session is mid-run.
        provider->rearm();
        engine.submit_prompt(sid, "turn two");
        CHECK(provider->wait_entered(2), "turn two parked in stream()");
        CHECK(store.load_messages(sid).size() == 3,
              "three rows while turn two is parked");

        engine.retry_last_turn(sid);  // must be a no-op

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(provider->entered == 2,
              "refused retry spawned no extra agentic loop");
        CHECK(store.load_messages(sid).size() == 3,
              "refused retry deleted no rows");

        // The parked turn still completes normally afterwards.
        provider->release();
        CHECK(wait_for_rows(store, sid, 4), "turn two completes after refusal");
        CHECK(wait_not_running(engine, sid), "turn two drains");
        CHECK(provider->entered == 2 && provider->exited == 2,
              "exactly two stream calls entered and exited");
    }

    rm_rf(tmp);
    std::cout << "[OK] retry refused while session is running\n";
}

static void test_retry_no_prompt() {
    std::string tmp = "/tmp/hc_test_retry_empty_XXXXXX";
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", tmp.c_str());
    if (!mkdtemp(buf)) { CHECK(false, "mkdtemp"); return; }
    tmp = buf;
    mkdirs(tmp + "/proj");

    haicode::Database db(tmp + "/e2e.db");
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
        std::string sid = engine.create_session(tmp + "/proj", "build",
                                                "fake-model", "fake");

        engine.retry_last_turn(sid);  // no user_prompted row → no-op

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(store.load_messages(sid).empty(), "no rows appear");
        CHECK(!engine.is_running(sid), "no runner spawned");
        CHECK(provider->calls == 0, "no provider call made");
    }

    rm_rf(tmp);
    std::cout << "[OK] retry on a prompt-less session is a no-op\n";
}

int main() {
    test_store_delete_after();
    test_retry_happy_path();
    test_retry_skill_block();
    test_retry_refused_while_running();
    test_retry_no_prompt();
    if (g_fail == 0) {
        printf("test_retry: ALL PASSED\n");
        return 0;
    }
    printf("test_retry: %d FAILURES\n", g_fail);
    return 1;
}
