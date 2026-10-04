// Task 29 session-cleanup tests: bulk session selection (SessionFilter),
// stored-state housekeeping (orphan sweep, checkpoint pruning, WAL/VACUUM
// reclaim), and the scratch-file sweep.
#include <haicode/db.h>
#include <haicode/util.h>
#include "test_check.h"

#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <iostream>
#include <string>
#include <vector>
#include <atomic>
#include <thread>

using haicode::Database;
using haicode::DbStmt;
using haicode::SessionFilter;
using haicode::SessionStore;

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << msg << "\n"; return false; } } while(0)

static const char* kDbPath = "/tmp/test_haicode_cleanup.db";

// Fresh migrated database + store, rebuilt per test so fixtures never leak.
struct Fixture {
    std::unique_ptr<Database> db;
    std::unique_ptr<SessionStore> store;
    void open() {
        std::remove(kDbPath);
        std::remove("/tmp/test_haicode_cleanup.db-wal");
        std::remove("/tmp/test_haicode_cleanup.db-shm");
        db = std::make_unique<Database>(kDbPath);
        db->migrate();
        store = std::make_unique<SessionStore>(*db);
    }
    // Age a row past whatever the store's own now_ms() wrote.
    void set_time(const char* table, const char* id_col, const std::string& id,
                  int64_t ms) {
        DbStmt st(db->handle(), std::string("UPDATE ") + table
            + " SET time_updated=? WHERE " + id_col + "=?");
        st.bind(1, ms).bind(2, id).expect_done();
    }
    int count(const std::string& sql) {
        DbStmt st(db->handle(), sql);
        if (!st.expect_row()) return -1;
        return st.int_col(0);
    }
};

static std::string make_session(SessionStore& store, const std::string& dir,
                                const std::string& title) {
    auto si = store.create(dir, "", "{}");
    if (!title.empty()) store.update_title(si.id, title);
    return si.id;
}

// ---- sessions_matching ----

static bool test_filters() {
    Fixture f;
    f.open();
    const int64_t now = haicode::util::now_ms();

    std::string titled_a = make_session(*f.store, "/proj/a", "Keep me");
    std::string titled_b = make_session(*f.store, "/proj/a", "Also keep");
    std::string untitled = make_session(*f.store, "/proj/b", "");
    std::string old      = make_session(*f.store, "/proj/a", "");
    f.set_time("session", "id", old, now - 40LL * 24 * 60 * 60 * 1000);

    // Give titled_a and untitled a real prompt; titled_b and old stay empty.
    f.store->append_message(titled_a, "user_prompted", "{\"text\":\"hi\"}");
    f.store->append_message(untitled, "user_prompted", "{\"text\":\"hi\"}");

    SessionFilter all;
    TEST_REQUIRE(f.store->sessions_matching(all).size() == 4,
                 "an empty filter matches every session");

    SessionFilter untitled_only;
    untitled_only.untitled_only = true;
    auto ids = f.store->sessions_matching(untitled_only);
    CHECK(ids.size() == 2, "untitled_only matches exactly the untitled rows");
    CHECK(ids[0] == untitled, "newest match comes first");
    CHECK(ids[1] == old, "second untitled session present");

    SessionFilter empty_only;
    empty_only.empty_only = true;
    ids = f.store->sessions_matching(empty_only);
    CHECK(ids.size() == 2, "empty_only matches sessions with no user_prompted row");
    for (const auto& id : ids)
        CHECK(id == titled_b || id == old, "empty_only picked the right sessions");

    const int64_t cutoff = now - 30LL * 24 * 60 * 60 * 1000;
    SessionFilter aged;
    aged.updated_before_ms = cutoff;
    ids = f.store->sessions_matching(aged);
    CHECK(ids.size() == 1 && ids[0] == old,
          "age cutoff matches only the aged session");

    SessionFilter scoped;
    scoped.directory = "/proj/a";
    ids = f.store->sessions_matching(scoped);
    CHECK(ids.size() == 3, "directory scoping excludes /proj/b");

    // Predicates combine.
    SessionFilter combo;
    combo.directory = "/proj/a";
    combo.untitled_only = true;
    combo.updated_before_ms = cutoff;
    ids = f.store->sessions_matching(combo);
    CHECK(ids.size() == 1 && ids[0] == old,
          "directory + untitled + age combine to one session");

    // A filter matching nothing returns nothing rather than everything.
    SessionFilter none;
    none.directory = "/does/not/exist";
    CHECK(f.store->sessions_matching(none).empty(),
          "unmatched directory returns no ids");

    std::cout << "[OK] sessions_matching filters\n";
    return true;
}

static bool test_no_list_cap() {
    Fixture f;
    f.open();
    // The sidebar caps at 50 (SessionStore::list(50)); cleanup must not — a
    // session that never made the list still has to be deletable.
    for (int i = 0; i < 60; ++i)
        make_session(*f.store, "/proj", "");

    CHECK(f.store->list(50).size() == 50, "list() cap still 50");

    SessionFilter untitled_only;
    untitled_only.untitled_only = true;
    CHECK(f.store->sessions_matching(untitled_only).size() == 60,
          "sessions_matching is uncapped: all 60 seeded sessions matched");

    SessionFilter empty_only;
    empty_only.empty_only = true;
    CHECK(f.store->sessions_matching(empty_only).size() == 60,
          "empty_only is uncapped too");

    std::cout << "[OK] sessions_matching has no 50-row cap\n";
    return true;
}

// ---- checkpoint housekeeping ----

static bool test_prune_stale_checkpoints() {
    Fixture f;
    f.open();
    const int64_t now = haicode::util::now_ms();
    const int64_t an_hour = 60LL * 60 * 1000;
    std::string sid = make_session(*f.store, "/proj", "ckpt host");

    std::string c1 = f.store->insert_checkpoint(sid, 5, "ctx-1", "");
    f.store->complete_checkpoint(c1, "summary one");
    std::string c2 = f.store->insert_checkpoint(sid, 9, "ctx-2", c1);
    f.store->complete_checkpoint(c2, "summary two");

    // A crashed compaction marker and a failed attempt, both long stale.
    std::string pending = f.store->insert_checkpoint(sid, 11, "ctx-p", c2);
    f.set_time("compaction_checkpoint", "id", pending, now - 6 * an_hour);
    std::string failed = f.store->insert_checkpoint(sid, 12, "ctx-f", c2);
    f.store->fail_checkpoint(failed);
    f.set_time("compaction_checkpoint", "id", failed, now - 6 * an_hour);
    // A fresh marker from a compaction that is (hypothetically) still running.
    std::string fresh = f.store->insert_checkpoint(sid, 13, "ctx-n", c2);

    int removed = f.store->prune_stale_checkpoints(now - an_hour);
    CHECK(removed == 2, "prune removes exactly the two stale non-complete rows");
    CHECK(f.count("SELECT COUNT(*) FROM compaction_checkpoint") == 3,
          "complete rows and the fresh marker survive");
    CHECK(f.count("SELECT COUNT(*) FROM compaction_checkpoint WHERE id='"
                  + pending + "'") == 0, "stale pending marker is gone");
    CHECK(f.count("SELECT COUNT(*) FROM compaction_checkpoint WHERE id='"
                  + failed + "'") == 0, "stale failed row is gone");
    CHECK(f.count("SELECT COUNT(*) FROM compaction_checkpoint WHERE id='"
                  + fresh + "'") == 1, "fresh pending marker is kept");

    // The complete rows are the transcript source and the compaction chain.
    auto cps = f.store->list_complete_checkpoints(sid);
    CHECK(cps.size() == 2, "both complete checkpoints survive pruning");
    CHECK(cps[0].id == c1 && cps[1].id == c2, "chain order preserved");
    CHECK(cps[0].summary == "summary one" && cps[1].summary == "summary two",
          "summaries intact");
    CHECK(cps[1].previous_checkpoint_id == c1, "previous_checkpoint_id intact");
    CHECK(f.store->latest_complete_checkpoint(sid)->id == c2,
          "latest_complete_checkpoint still resolves");

    std::cout << "[OK] prune_stale_checkpoints\n";
    return true;
}

static bool test_clear_stale_checkpoint_contexts() {
    Fixture f;
    f.open();
    std::string a = make_session(*f.store, "/proj", "session a");
    std::string b = make_session(*f.store, "/proj", "session b");

    // Session a: three compactions. Session b: one. Only each session's
    // highest-through_seq row is ever read again by compact_history.
    std::string a1 = f.store->insert_checkpoint(a, 4, "a-ctx-1", "");
    f.store->complete_checkpoint(a1, "a summary 1");
    std::string a2 = f.store->insert_checkpoint(a, 8, "a-ctx-2", a1);
    f.store->complete_checkpoint(a2, "a summary 2");
    std::string a3 = f.store->insert_checkpoint(a, 12, "a-ctx-3", a2);
    f.store->complete_checkpoint(a3, "a summary 3");
    std::string b1 = f.store->insert_checkpoint(b, 3, "b-ctx-1", "");
    f.store->complete_checkpoint(b1, "b summary 1");
    // A pending row is not a compaction the next round could resume from; it
    // must not be touched by this sweep either.
    std::string a4 = f.store->insert_checkpoint(a, 14, "a-ctx-pending", a3);

    int cleared = f.store->clear_stale_checkpoint_contexts();
    CHECK(cleared == 2, "only the superseded complete rows are cleared");
    (void)a4;

    auto cps = f.store->list_complete_checkpoints(a);
    CHECK(cps.size() == 3, "all complete rows survive the sweep");
    CHECK(cps[0].recent_context.empty() && cps[1].recent_context.empty(),
          "superseded rows lost their retained tail");
    CHECK(cps[2].recent_context == "a-ctx-3",
          "the newest complete row keeps its retained tail");
    CHECK(cps[0].summary == "a summary 1" && cps[2].summary == "a summary 3",
          "every summary survives");
    CHECK(cps[1].previous_checkpoint_id == a1 && cps[2].previous_checkpoint_id == a2,
          "the chain is intact");
    CHECK(f.store->latest_complete_checkpoint(a)->recent_context == "a-ctx-3",
          "compact_history's read path still gets a retained tail");

    auto b_cps = f.store->list_complete_checkpoints(b);
    CHECK(b_cps.size() == 1 && b_cps[0].recent_context == "b-ctx-1",
          "a session's only checkpoint keeps its context");

    {
        DbStmt pend(f.db->handle(),
            "SELECT recent_context FROM compaction_checkpoint WHERE id='" + a4 + "'");
        TEST_REQUIRE(pend.expect_row(), "pending checkpoint row present");
        CHECK(pend.text(0) == "a-ctx-pending", "pending row untouched");
    }

    // Idempotent: a second sweep clears nothing further.
    CHECK(f.store->clear_stale_checkpoint_contexts() == 0,
          "re-running the sweep is a no-op");

    std::cout << "[OK] clear_stale_checkpoint_contexts\n";
    return true;
}

// ---- orphan sweep ----

static bool test_prune_orphans() {
    Fixture f;
    f.open();
    std::string live = make_session(*f.store, "/proj", "live session");
    f.store->append_message(live, "user_prompted", "{\"text\":\"keep\"}");
    std::vector<haicode::Todo> todos = {{"live todo", "active", "pending"}};
    f.store->replace_todos(live, todos);
    std::string cp = f.store->insert_checkpoint(live, 1, "ctx", "");
    f.store->complete_checkpoint(cp, "live summary");

    // Second connection with FK enforcement off: exactly what a pre-FK
    // database or a hand-edit leaves behind.
    {
        sqlite3* raw = nullptr;
        TEST_REQUIRE(sqlite3_open(kDbPath, &raw) == SQLITE_OK, "open raw");
        sqlite3_exec(raw, "PRAGMA foreign_keys=OFF;", nullptr, nullptr, nullptr);
        auto exec = [&](const char* sql) {
            char* err = nullptr;
            if (sqlite3_exec(raw, sql, nullptr, nullptr, &err) != SQLITE_OK) {
                std::string msg = err ? err : "?";
                sqlite3_free(err);
                sqlite3_close(raw);
                TEST_REQUIRE(false, "orphan fixture insert failed: " + msg);
            }
        };
        exec("INSERT INTO session_message (id, session_id, type, seq, data_json,"
             " time_created, time_updated) VALUES ('orphan-msg','gone-session',"
             " 'assistant_text', 1, '{}', 0, 0)");
        exec("INSERT INTO session_todo (session_id, position, content, active_form,"
             " status, time_created, time_updated) VALUES ('gone-session', 0,"
             " 'stale todo', '', 'pending', 0, 0)");
        exec("INSERT INTO compaction_checkpoint (id, session_id, through_seq,"
             " summary, recent_context, previous_checkpoint_id, status,"
             " time_created, time_updated) VALUES ('orphan-cp','gone-session',1,"
             " 'orphan summary','orphan ctx','', 'complete', 0, 0)");
        sqlite3_close(raw);
    }

    std::string err;
    int removed = f.db->prune_orphans(err);
    CHECK(removed == 3, "all three orphan rows removed");
    CHECK(err.empty(), "no error reported: " + err);
    CHECK(f.count("SELECT COUNT(*) FROM session_message WHERE session_id='gone-session'") == 0,
          "orphan message gone");
    CHECK(f.count("SELECT COUNT(*) FROM session_todo WHERE session_id='gone-session'") == 0,
          "orphan todo gone");
    CHECK(f.count("SELECT COUNT(*) FROM compaction_checkpoint WHERE session_id='gone-session'") == 0,
          "orphan checkpoint gone");

    CHECK(f.count("SELECT COUNT(*) FROM session_message WHERE session_id='" + live + "'") == 1,
          "live session's message untouched");
    CHECK(f.count("SELECT COUNT(*) FROM session_todo WHERE session_id='" + live + "'") == 1,
          "live session's todo untouched");
    CHECK(f.count("SELECT COUNT(*) FROM compaction_checkpoint WHERE session_id='" + live + "'") == 1,
          "live session's checkpoint untouched");

    // Nothing left dangling, and a repeat sweep finds nothing to do.
    DbStmt check(f.db->handle(), "PRAGMA foreign_key_check;");
    int violations = 0;
    while (check.expect_row()) ++violations;
    CHECK(violations == 0, "foreign_key_check clean after the sweep");
    std::string err2;
    CHECK(f.db->prune_orphans(err2) == 0, "re-running the sweep removes nothing");

    std::cout << "[OK] prune_orphans\n";
    return true;
}

// ---- VACUUM / WAL reclaim ----

static bool test_reclaim_space() {
    Fixture f;
    f.open();

    // Fill it, then delete most of it, so there is free space to reclaim.
    std::vector<std::string> ids;
    for (int i = 0; i < 12; ++i) {
        std::string sid = make_session(*f.store, "/proj", "filler " + std::to_string(i));
        std::string blob(4000, 'x');
        for (int m = 0; m < 8; ++m)
            f.store->append_message(sid, "assistant_text",
                                    "{\"text\":\"" + blob + "\"}");
        ids.push_back(sid);
    }
    for (size_t i = 0; i + 2 < ids.size(); ++i)
        f.store->delete_session(ids[i]);

    std::string err;
    int64_t before = 0, after = 0;
    int free_pages = f.count("PRAGMA freelist_count;");
    CHECK(free_pages > 0, "deletions left reclaimable pages");
    CHECK(f.db->reclaim_space(err, &before, &after, 1024LL * 1024),
          "automatic reclaim threshold check succeeds");
    CHECK(before == after && f.count("PRAGMA freelist_count;") == free_pages,
          "less than 1 MiB of free pages skips VACUUM");
    bool ok = f.db->reclaim_space(err, &before, &after);
    CHECK(ok, "reclaim_space succeeded: " + err);
    CHECK(err.empty(), "no error text on success");
    CHECK(before > 0 && after > 0, "sizes reported on both sides of VACUUM");
    CHECK(after < before, "the file shrank: " + std::to_string(before)
                          + " -> " + std::to_string(after));
    struct stat main_stat{}, wal_stat{};
    CHECK(stat(kDbPath, &main_stat) == 0 && main_stat.st_size == after,
          "the main file shrinks while the connection remains open");
    CHECK(stat("/tmp/test_haicode_cleanup.db-wal", &wal_stat) == 0
          && wal_stat.st_size == 0, "VACUUM's WAL is checkpointed and truncated");

    // Still a readable WAL database afterwards.
    {
        DbStmt jm(f.db->handle(), "PRAGMA journal_mode;");
        TEST_REQUIRE(jm.expect_row(), "journal_mode returned no row");
        CHECK(jm.text(0) == "wal", "journal_mode is still wal after VACUUM");
    }

    auto survivors = f.store->list(50);
    CHECK(survivors.size() == 2, "surviving sessions still readable");
    CHECK(f.store->load_messages(survivors[0].id).size() == 8,
          "surviving session's messages intact");

    std::string err2;
    CHECK(f.db->reclaim_space(err2), "reclaim on an already-compact DB succeeds");
    CHECK(err2.empty(), "no spurious error on the repeat reclaim");

    std::cout << "[OK] reclaim_space (WAL checkpoint + VACUUM)\n";
    return true;
}

static bool test_housekeeping_concurrency() {
    Fixture f;
    f.open();
    SessionStore second(*f.db);
    std::string sid = make_session(*f.store, "/proj", "keep");
    std::atomic<bool> start{false};
    std::atomic<int> failures{0};
    std::thread writer([&] {
        while (!start.load()) std::this_thread::yield();
        for (int i = 0; i < 200; ++i) {
            try {
                f.store->append_message(sid, "user_prompted", "{}");
                second.replace_todos(sid, {{"keep", "keeping", "pending"}});
            } catch (...) { ++failures; }
        }
    });
    std::thread cleaner([&] {
        while (!start.load()) std::this_thread::yield();
        for (int i = 0; i < 200; ++i) {
            try {
                std::string error;
                f.db->prune_orphans(error);
                if (!error.empty()) ++failures;
                if (i % 20 == 0 && !f.db->reclaim_space(error)) ++failures;
            } catch (...) { ++failures; }
        }
    });
    start = true;
    writer.join();
    cleaner.join();
    CHECK(failures == 0, "housekeeping and both stores share connection serialization");
    CHECK(f.store->load_messages(sid).size() == 200, "all concurrent messages survive");
    CHECK(second.load_todos(sid).size() == 1, "concurrent todo list survives");
    return true;
}

static bool test_housekeeping_lock_failures() {
    Fixture f;
    f.open();
    f.db->set_busy_timeout(30);
    std::string sid = make_session(*f.store, "/proj", "keep");
    sqlite3* other = nullptr;
    TEST_REQUIRE(sqlite3_open(kDbPath, &other) == SQLITE_OK, "open competing connection");
    TEST_REQUIRE(sqlite3_exec(other, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr)
        == SQLITE_OK, "hold competing write lock");
    std::string error;
    CHECK(f.db->prune_orphans(error) == 0 && !error.empty(),
          "orphan failures are returned without throwing");
    CHECK(!f.db->reclaim_space(error) && !error.empty(), "reclaim failure is reported");
    TEST_REQUIRE(sqlite3_exec(other, "ROLLBACK; BEGIN; SELECT count(*) FROM session;",
        nullptr, nullptr, nullptr) == SQLITE_OK, "hold a reader snapshot");
    f.store->append_message(sid, "user_prompted", "{}");
    CHECK(!f.db->reclaim_space(error) && error.find("busy") != std::string::npos,
          "a busy WAL checkpoint is not reported as success");
    TEST_REQUIRE(sqlite3_exec(other, "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK,
        "release reader snapshot");
    sqlite3_close(other);
    CHECK(f.db->reclaim_space(error) && error.empty(), "reclaim retries after lock release");
    CHECK(f.store->load_messages(sid).size() == 1, "failed housekeeping preserves history");
    return true;
}

static bool test_automatic_reclaim_threshold() {
    Fixture f;
    f.open();
    std::string keep = make_session(*f.store, "/proj", "keep");
    std::string filler = make_session(*f.store, "/proj", "filler");
    for (int i = 0; i < 16; ++i)
        f.store->append_message(filler, "assistant_text", std::string(128 * 1024, 'x'));
    f.store->delete_session(filler);
    int64_t before = 0, after = 0;
    std::string error;
    CHECK(f.db->reclaim_space(error, &before, &after, 1024LL * 1024),
          "large automatic reclaim succeeds: " + error);
    CHECK(before - after >= 1024LL * 1024, "large free space triggers automatic VACUUM");
    CHECK(f.store->get(keep).has_value(), "automatic reclaim preserves remaining sessions");
    return true;
}

// ---- scratch-file sweep ----

static const char* kFixDir = "/tmp/haicode_cleanup_fixture";

static void touch(const std::string& path, int64_t mtime_ms) {
    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    TEST_REQUIRE(fd >= 0, "fixture create failed: " + path);
    close(fd);
    struct timespec ts[2];
    ts[0].tv_sec  = (time_t)(mtime_ms / 1000);
    ts[0].tv_nsec = 0;
    ts[1].tv_sec  = (time_t)(mtime_ms / 1000);
    ts[1].tv_nsec = 0;
    TEST_REQUIRE(utimensat(AT_FDCWD, path.c_str(), ts, 0) == 0,
                 "fixture utimensat failed: " + path);
}

static bool exists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

static bool test_sweep_scratch_files() {
    system(("rm -rf " + std::string(kFixDir)).c_str());
    TEST_REQUIRE(mkdir(kFixDir, 0755) == 0, "fixture dir create failed");

    const int64_t now = haicode::util::now_ms();
    const int64_t aged = now - 3 * 24 * 60 * 60 * 1000;   // 3 days old
    const int64_t fresh = now - 60 * 1000;                // 1 minute old

    std::string shot_aged  = std::string(kFixDir) + "/haicode_shot_aged.png";
    std::string shot_fresh = std::string(kFixDir) + "/haicode_shot_fresh.png";
    std::string diff_aged  = std::string(kFixDir) + "/haicode_diff_aged";
    std::string diff_fresh = std::string(kFixDir) + "/haicode_diff_fresh";
    std::string unrelated  = std::string(kFixDir) + "/other_shot_1.png";
    std::string near_miss  = std::string(kFixDir) + "/notour_shot_1.png";
    std::string sub        = std::string(kFixDir) + "/haicode_shot_subdir";
    std::string sub_file   = sub + "/haicode_shot_inside.png";

    touch(shot_aged, aged);
    touch(diff_aged, aged);
    touch(shot_fresh, fresh);
    touch(diff_fresh, fresh);
    touch(unrelated, aged);
    touch(near_miss, aged);
    TEST_REQUIRE(mkdir(sub.c_str(), 0755) == 0, "fixture subdir create failed");
    touch(sub_file, aged);

    std::string err;
    int removed = haicode::util::sweep_scratch_files(
        kFixDir, {"haicode_shot_", "haicode_diff_"}, 24LL * 60 * 60 * 1000, err);

    CHECK(removed == 2, "removed exactly the two aged scratch files");
    CHECK(err.empty(), "no error reported: " + err);
    CHECK(!exists(shot_aged), "aged screenshot scratch removed");
    CHECK(!exists(diff_aged), "aged diff scratch removed");
    CHECK(exists(shot_fresh), "in-flight screenshot scratch kept");
    CHECK(exists(diff_fresh), "live diff scratch kept");
    CHECK(exists(unrelated), "unrelated name left alone");
    CHECK(exists(near_miss), "near-miss name left alone");
    CHECK(exists(sub), "directory with a matching name untouched");
    CHECK(exists(sub_file), "contents of that directory untouched");

    // An empty prefix list sweeps nothing and never errors.
    std::string err2;
    CHECK(haicode::util::sweep_scratch_files(kFixDir, {}, 0, err2) == 0,
          "no prefixes means no removal");
    // A missing directory reports rather than crashing.
    std::string err3;
    CHECK(haicode::util::sweep_scratch_files(std::string(kFixDir) + "/nope",
             {"haicode_shot_"}, 0, err3) == 0, "missing dir removes nothing");
    CHECK(!err3.empty(), "missing dir reports an error");

    system(("rm -rf " + std::string(kFixDir)).c_str());
    std::cout << "[OK] sweep_scratch_files\n";
    return true;
}

int main() {
    int failures = 0;
    auto run = [&](const char* name, bool (*fn)()) {
        std::cout << "--- " << name << "\n";
        if (!fn()) ++failures;
    };
    run("filters", test_filters);
    run("no_list_cap", test_no_list_cap);
    run("prune_stale_checkpoints", test_prune_stale_checkpoints);
    run("clear_stale_checkpoint_contexts", test_clear_stale_checkpoint_contexts);
    run("prune_orphans", test_prune_orphans);
    run("reclaim_space", test_reclaim_space);
    run("automatic_reclaim_threshold", test_automatic_reclaim_threshold);
    run("housekeeping_concurrency", test_housekeeping_concurrency);
    run("housekeeping_lock_failures", test_housekeeping_lock_failures);
    run("sweep_scratch_files", test_sweep_scratch_files);

    std::remove(kDbPath);
    std::remove("/tmp/test_haicode_cleanup.db-wal");
    std::remove("/tmp/test_haicode_cleanup.db-shm");

    if (failures) {
        std::cerr << failures << " cleanup test group(s) failed\n";
        return 1;
    }
    std::cout << "all session cleanup tests passed\n";
    return 0;
}
