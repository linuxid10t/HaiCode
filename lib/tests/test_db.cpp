#include <haicode/db.h>
#include <haicode/util.h>
#include <iostream>
#include "test_check.h"
#include <cstdio>

int main() {
    std::cout << "=== haicode Phase 1 Test ===" << std::endl;

    // Remove any leftover DB from previous run
    const char* db_path = "/tmp/test_haicode_fresh.db";
    std::remove(db_path);

    // Use temp DB
    haicode::Database db(db_path);
    db.migrate();
    std::cout << "[OK] Database created and migrated" << std::endl;

    haicode::SessionStore store(db);

    // Create a session
    auto session = store.create("/boot/home", "default", "{\"id\":\"claude-opus-4-5\",\"provider_id\":\"anthropic\"}");
    TEST_REQUIRE(!session.id.empty(), "session id empty");
    std::cout << "[OK] Session created: " << session.id << std::endl;

    // List sessions
    auto sessions = store.list();
    TEST_REQUIRE(sessions.size() == 1, "expected exactly 1 session, got " + std::to_string(sessions.size()));
    std::cout << "[OK] Listed " << sessions.size() << " session(s)" << std::endl;

    // Append a message
    store.append_message(session.id, "user_prompted",
        "{\"role\":\"user\",\"text\":\"Hello, world!\"}");
    std::cout << "[OK] Appended message" << std::endl;

    // Reload messages
    auto messages = store.load_messages(session.id);
    TEST_REQUIRE(messages.size() == 1, "expected exactly 1 message, got " + std::to_string(messages.size()));
    std::cout << "[OK] Loaded " << messages.size() << " message(s)" << std::endl;

    // Get by ID
    auto got = store.get(session.id);
    TEST_REQUIRE(got.has_value(), "get() returned nothing");
    TEST_REQUIRE(got->id == session.id, "get() id mismatch");
    std::cout << "[OK] Retrieved session by ID" << std::endl;

    // Ordering: list() and try_update_times() must both return rows newest
    // first, with id DESC as the tie-break so an in-place sidebar sort and a
    // full rebuild always agree on equal timestamps.
    {
        auto s2 = store.create("/boot/home", "default", "{}");
        auto s3 = store.create("/boot/home", "default", "{}");
        // Deterministic times: s2 oldest, s3 middle, s1 (from above) newest.
        auto set_time = [&](const std::string& id, int64_t ms) {
            haicode::DbStmt st(db.handle(),
                "UPDATE session SET time_updated=? WHERE id=?");
            st.bind(1, ms).bind(2, id).expect_done();
        };
        set_time(session.id, 3000);
        set_time(s2.id, 1000);
        set_time(s3.id, 2000);

        auto ordered = store.list();
        TEST_REQUIRE(ordered.size() == 3, "expected 3 sessions");
        TEST_REQUIRE(ordered[0].id == session.id, "newest must list first");
        TEST_REQUIRE(ordered[1].id == s3.id, "middle time second");
        TEST_REQUIRE(ordered[2].id == s2.id, "oldest last");

        std::vector<std::pair<std::string, int64_t>> times;
        TEST_REQUIRE(store.try_update_times(times, 50), "try_update_times failed");
        TEST_REQUIRE(times.size() == 3, "expected 3 time rows");
        TEST_REQUIRE(times[0].first == session.id, "times: newest first");
        TEST_REQUIRE(times[1].first == s3.id, "times: middle second");
        TEST_REQUIRE(times[2].first == s2.id, "times: oldest last");

        // Exact tie: higher id wins (both queries).
        set_time(s2.id, 2000);
        set_time(s3.id, 2000);
        ordered = store.list();
        TEST_REQUIRE(ordered.size() == 3, "expected 3 sessions (tie)");
        TEST_REQUIRE(ordered[0].id == session.id, "tie: newest still first");
        const std::string& tie_high = s2.id > s3.id ? s2.id : s3.id;
        const std::string& tie_low  = s2.id > s3.id ? s3.id : s2.id;
        TEST_REQUIRE(ordered[1].id == tie_high, "tie: higher id first");
        TEST_REQUIRE(ordered[2].id == tie_low, "tie: lower id second");
        TEST_REQUIRE(store.try_update_times(times, 50), "try_update_times (tie) failed");
        TEST_REQUIRE(times[0].first == session.id, "tie times: newest first");
        TEST_REQUIRE(times[1].first == tie_high, "tie times: higher id first");
        TEST_REQUIRE(times[2].first == tie_low, "tie times: lower id second");
        std::cout << "[OK] list()/try_update_times() order: time DESC, id DESC" << std::endl;
    }

    {
        auto older = store.create("/boot/home", "default",
            "{ \"provider_id\": \"anthropic\", \"id\": \"stored-model\" }");
        haicode::DbStmt set_time(db.handle(),
            "UPDATE session SET time_updated=1000 WHERE id=?");
        set_time.bind(1, older.id).expect_done();
        const auto original_json = store.get(older.id)->model_json;

        store.load_messages(older.id);
        store.list_complete_checkpoints(older.id);
        store.update_provider_model(older.id, "anthropic", "stored-model");
        store.update_provider_model(older.id, "", "stored-model");
        store.update_provider_model(older.id, "anthropic", "");
        store.update_provider_model(older.id, "", "");
        TEST_REQUIRE(store.get(older.id)->time_updated == 1000,
                     "opening/unchanged provider-model saves must not modify timestamp");
        TEST_REQUIRE(store.get(older.id)->model_json == original_json,
                     "unchanged saves must preserve stored JSON bytes");
        TEST_REQUIRE(store.list().back().id == older.id,
                     "opening an older session must not reorder it");

        store.update_provider_model(older.id, "anthropic", "new-model");
        TEST_REQUIRE(store.get(older.id)->time_updated > 1000,
                     "actual model change must update timestamp");
        TEST_REQUIRE(store.list().front().id == older.id,
                     "actual model change must move session to top");

        haicode::DbStmt reset_time(db.handle(),
            "UPDATE session SET time_updated=1000 WHERE id=?");
        reset_time.bind(1, older.id).expect_done();
        store.append_message(older.id, "user_prompted", "{\"text\":\"Do something\"}");
        TEST_REQUIRE(store.get(older.id)->time_updated > 1000,
                     "new message must update timestamp");
        TEST_REQUIRE(store.list().front().id == older.id,
                     "new message must move session to top");
        std::cout << "[OK] reads/unchanged saves preserve modified time; edits reorder" << std::endl;
    }

    std::cout << std::endl << "All Phase 1 tests passed!" << std::endl;
    return 0;
}
