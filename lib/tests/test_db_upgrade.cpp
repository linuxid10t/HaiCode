// Task 2 regression tests: numbered migrations, permission-table removal,
// and SQLite failure surfacing (DbError/DbStmt, busy_timeout).
#include <haicode/db.h>
#include <haicode/util.h>
#include "test_check.h"

#include <sqlite3.h>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>

static void build_legacy_db(const std::string& path) {
    std::remove(path.c_str());
    sqlite3* raw = nullptr;
    TEST_REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK, "open legacy");
    auto exec = [&](const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(raw, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "?";
            sqlite3_free(err);
            TEST_REQUIRE(false, "legacy exec failed: " + msg + " [" + sql + "]");
        }
    };
    // Version-0 schema: no tok_last_input column, permission table present.
    exec("CREATE TABLE session ("
         " id TEXT PRIMARY KEY, project_id TEXT NOT NULL DEFAULT '',"
         " title TEXT NOT NULL DEFAULT '', directory TEXT NOT NULL DEFAULT '',"
         " agent TEXT NOT NULL DEFAULT '', model_json TEXT NOT NULL DEFAULT '{}',"
         " cost REAL NOT NULL DEFAULT 0, tok_input INTEGER NOT NULL DEFAULT 0,"
         " tok_output INTEGER NOT NULL DEFAULT 0, tok_reasoning INTEGER NOT NULL DEFAULT 0,"
         " tok_cache_read INTEGER NOT NULL DEFAULT 0, tok_cache_write INTEGER NOT NULL DEFAULT 0,"
         " time_created INTEGER NOT NULL, time_updated INTEGER NOT NULL)");
    exec("CREATE TABLE permission ("
         " id TEXT PRIMARY KEY, project_id TEXT NOT NULL, action TEXT NOT NULL,"
         " resource TEXT NOT NULL, time_created INTEGER NOT NULL,"
         " UNIQUE(project_id, action, resource))");
    exec("CREATE TABLE session_message ("
         " id TEXT PRIMARY KEY,"
         " session_id TEXT NOT NULL REFERENCES session(id) ON DELETE CASCADE,"
         " type TEXT NOT NULL, seq INTEGER NOT NULL,"
         " data_json TEXT NOT NULL DEFAULT '{}',"
         " time_created INTEGER NOT NULL, time_updated INTEGER NOT NULL,"
         " UNIQUE(session_id, seq))");
    int64_t now = haicode::util::now_ms();
    exec(("INSERT INTO session (id, project_id, title, directory, agent, model_json,"
          " time_created, time_updated) VALUES"
          " ('ses_legacy', '/proj', 'Old title', '/proj', 'default', '{}',"
          " " + std::to_string(now) + ", " + std::to_string(now) + ")").c_str());
    exec(("INSERT INTO session_message (id, session_id, type, seq, data_json,"
          " time_created, time_updated) VALUES"
          " ('m1', 'ses_legacy', 'user_prompted', 1,"
          " '{\"role\":\"user\",\"text\":\"hello\"}',"
          " " + std::to_string(now) + ", " + std::to_string(now) + ")").c_str());
    exec(("INSERT INTO session_message (id, session_id, type, seq, data_json,"
          " time_created, time_updated) VALUES"
          " ('m2', 'ses_legacy', 'assistant_text', 2,"
          " '{\"role\":\"assistant\",\"text\":\"hi\"}',"
          " " + std::to_string(now) + ", " + std::to_string(now) + ")").c_str());
    // Migration 3's project_used backfill inputs: Chat sessions classified
    // by their transcripts (a local tool call = used; a Chat-only tool or no
    // calls = chat-only). ses_legacy ('{}', no mode = Build) counts as used.
    auto add_session = [&](const char* id, const char* model_json) {
        exec(("INSERT INTO session (id, project_id, title, directory, agent,"
              " model_json, time_created, time_updated) VALUES"
              " ('" + std::string(id) + "', '/proj', '', '/proj', 'default', '"
              + model_json + "', " + std::to_string(now) + ", "
              + std::to_string(now) + ")").c_str());
    };
    auto add_assistant = [&](const char* mid, const char* sid,
                             const char* tool) {
        exec(("INSERT INTO session_message (id, session_id, type, seq, data_json,"
              " time_created, time_updated) VALUES ('" + std::string(mid)
              + "', '" + sid + "', 'assistant_text', 1,"
              " '{\"role\":\"assistant\",\"text\":\"\",\"tool_calls\":"
              "[{\"id\":\"c1\",\"name\":\"" + tool + "\",\"input\":{}}]}',"
              " " + std::to_string(now) + ", " + std::to_string(now) + ")").c_str());
    };
    add_session("ses_chat_web", "{\"mode\":\"chat\"}");
    add_assistant("w1", "ses_chat_web", "web_search");
    add_session("ses_chat_read", "{\"mode\":\"chat\"}");
    add_assistant("r1", "ses_chat_read", "read");
    add_session("ses_chat_empty", "{\"mode\":\"chat\"}");
    add_session("ses_plan", "{\"mode\":\"plan\"}");
    exec(("INSERT INTO permission (id, project_id, action, resource, time_created)"
          " VALUES ('p1', '/proj', 'bash', '*', " + std::to_string(now) + ")").c_str());
    sqlite3_close(raw);
}

static bool table_exists(haicode::Database& db, const std::string& name) {
    haicode::DbStmt stmt(db.handle(),
        "SELECT name FROM sqlite_master WHERE type='table' AND name=?");
    stmt.bind(1, name);
    return stmt.expect_row();
}

int main() {
    std::cout << "=== test_db_upgrade ===" << std::endl;

    // --- Legacy database migrates 0 -> 3 with data intact ---
    {
        const std::string path = "/tmp/test_haicode_upgrade_legacy.db";
        build_legacy_db(path);
        haicode::Database db(path);
        TEST_REQUIRE(db.user_version() == 0, "legacy starts at version 0");
        db.migrate();

        TEST_REQUIRE(db.user_version() == 3, "migrated to version 3");
        TEST_REQUIRE(!table_exists(db, "permission"),
                     "permission table dropped by migration 2");
        std::cout << "[OK] legacy DB migrated to v3, permission dropped" << std::endl;

        haicode::SessionStore store(db);
        auto sess = store.get("ses_legacy");
        TEST_REQUIRE(sess.has_value(), "legacy session row survived");
        TEST_REQUIRE(sess->title == "Old title", "legacy title survived");
        // get() SELECTs tok_last_input explicitly — the column must exist now.
        TEST_REQUIRE(sess->last_input_tokens == 0, "tok_last_input added, default 0");
        auto msgs = store.load_messages("ses_legacy");
        TEST_REQUIRE(msgs.size() == 2, "both legacy messages survived");
        TEST_REQUIRE(msgs[0].seq == 1 && msgs[1].seq == 2, "seq order preserved");
        std::cout << "[OK] legacy rows intact, tok_last_input added" << std::endl;

        // Migration 3: project_used backfilled from mode + transcript.
        TEST_REQUIRE(sess->project_used, "no-mode (Build) session is used");
        TEST_REQUIRE(store.get("ses_plan")->project_used, "Plan session is used");
        TEST_REQUIRE(store.get("ses_chat_read")->project_used,
                     "Chat session with a local tool call is used");
        TEST_REQUIRE(!store.get("ses_chat_web")->project_used,
                     "Chat session with only Chat tools is chat-only");
        TEST_REQUIRE(!store.get("ses_chat_empty")->project_used,
                     "Chat session with no calls is chat-only");
        bool listed = false;
        for (const auto& si : store.list())
            if (si.id == "ses_chat_read") listed = si.project_used;
        TEST_REQUIRE(listed, "list() reads project_used too");
        std::cout << "[OK] project_used backfilled" << std::endl;

        // Idempotent: a second migrate() is a no-op at version 3.
        db.migrate();
        TEST_REQUIRE(db.user_version() == 3, "re-migrate stays at version 3");
        TEST_REQUIRE(!store.get("ses_chat_web")->project_used,
                     "re-migrate keeps the backfill");
        TEST_REQUIRE(store.load_messages("ses_legacy").size() == 2,
                     "re-migrate preserves rows");
        std::cout << "[OK] migrate() idempotent" << std::endl;
    }

    // --- Fresh database lands at version 3 without a permission table ---
    {
        const std::string path = "/tmp/test_haicode_upgrade_fresh.db";
        std::remove(path.c_str());
        haicode::Database db(path);
        db.migrate();
        TEST_REQUIRE(db.user_version() == 3, "fresh DB at version 3");
        TEST_REQUIRE(!table_exists(db, "permission"), "no permission table");
        haicode::SessionStore store(db);
        auto s = store.create("/proj", "default", "{}");
        store.append_message(s.id, "user_prompted", "{\"role\":\"user\",\"text\":\"x\"}");
        TEST_REQUIRE(store.load_messages(s.id).size() == 1, "fresh round-trip");
        TEST_REQUIRE(!store.get(s.id)->project_used, "new session not yet used");
        std::cout << "[OK] fresh DB created at v3" << std::endl;
    }

    // --- Failure surfacing ---
    {
        const std::string path = "/tmp/test_haicode_upgrade_lock.db";
        std::remove(path.c_str());
        haicode::Database db(path);
        db.migrate();

        // Invalid SQL throws DbError from the RAII wrapper.
        bool threw = false;
        try {
            haicode::DbStmt bad(db.handle(), "SELECT * FROM no_such_table");
            (void)bad;
        } catch (const haicode::DbError&) {
            threw = true;
        }
        TEST_REQUIRE(threw, "DbStmt on invalid SQL throws DbError");
        std::cout << "[OK] DbStmt throws DbError on invalid SQL" << std::endl;

        // A write that cannot acquire the lock throws promptly instead of
        // silently succeeding.
        haicode::Database locker(path);
        locker.exec("BEGIN EXCLUSIVE;");
        db.set_busy_timeout(100);
        haicode::SessionStore store(db);
        threw = false;
        auto t0 = std::chrono::steady_clock::now();
        try {
            store.create("/proj", "default", "{}");
        } catch (const haicode::DbError&) {
            threw = true;
        }
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        TEST_REQUIRE(threw, "locked-out create throws DbError");
        TEST_REQUIRE(elapsed_ms < 2000, "failed fast, took " + std::to_string(elapsed_ms) + "ms");
        std::cout << "[OK] locked write throws DbError after " << elapsed_ms << "ms"
                  << std::endl;

        // Releasing the lock restores writes.
        locker.exec("ROLLBACK;");
        auto s = store.create("/proj", "default", "{}");
        TEST_REQUIRE(!s.id.empty(), "write succeeds after lock released");
        std::cout << "[OK] writes recover after lock release" << std::endl;
    }

    std::cout << std::endl << "All test_db_upgrade tests passed!" << std::endl;
    return 0;
}
