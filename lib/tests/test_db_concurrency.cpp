// Task 3 regression tests: concurrent-append sequencing and transaction
// isolation on the single shared sqlite connection.
#include <haicode/db.h>
#include <haicode/util.h>
#include "test_check.h"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

int main() {
    std::cout << "=== test_db_concurrency ===" << std::endl;

    const std::string path = "/tmp/test_haicode_concurrency.db";

    // --- Concurrent appends to one session: no lost rows, no duplicate seqs ---
    // Looped to shake out races; any iteration failing the invariants fails
    // the whole test with a line number pointing at the iteration.
    const int kAppendIters = 20;
    const int kAppendsPerThread = 50;
    for (int iter = 0; iter < kAppendIters; ++iter) {
        std::remove(path.c_str());
        haicode::Database db(path);
        db.migrate();
        haicode::SessionStore store(db);
        auto session = store.create("/proj", "default", "{}");

        auto append_worker = [&](int tag) {
            for (int i = 0; i < kAppendsPerThread; ++i)
                store.append_message(session.id, "user_prompted",
                    "{\"role\":\"user\",\"text\":\"t" + std::to_string(tag)
                        + "-" + std::to_string(i) + "\"}");
        };
        std::thread a(append_worker, 0);
        std::thread b(append_worker, 1);
        a.join();
        b.join();

        auto msgs = store.load_messages(session.id);
        TEST_REQUIRE(msgs.size() == (size_t)(2 * kAppendsPerThread),
            "iter " + std::to_string(iter) + ": expected "
            + std::to_string(2 * kAppendsPerThread) + " rows, got "
            + std::to_string(msgs.size()));
        std::set<int> seqs;
        for (auto& m : msgs) seqs.insert(m.seq);
        TEST_REQUIRE(seqs.size() == msgs.size(),
            "iter " + std::to_string(iter) + ": duplicate seqs ("
            + std::to_string(msgs.size()) + " rows, "
            + std::to_string(seqs.size()) + " distinct seqs)");
        TEST_REQUIRE(*seqs.begin() == 1 && *seqs.rbegin() == (int)msgs.size(),
            "iter " + std::to_string(iter) + ": seqs not a 1..N permutation");
    }
    std::cout << "[OK] " << kAppendIters << " iterations: 2 threads x "
              << kAppendsPerThread << " appends, no losses, seqs unique 1..N"
              << std::endl;

    // --- Concurrent replace_todos on two sessions: no cross-session absorption ---
    // Before the store-level mutex, thread B's BEGIN failed against thread
    // A's open transaction, so B's writes landed inside A's commit and the
    // two sessions could end up with each other's (or missing) rows.
    const int kTodoIters = 50;
    const int kWritesPerThread = 20;
    for (int iter = 0; iter < kTodoIters; ++iter) {
        std::remove(path.c_str());
        haicode::Database db(path);
        db.migrate();
        haicode::SessionStore store(db);
        auto s1 = store.create("/proj1", "default", "{}");
        auto s2 = store.create("/proj2", "default", "{}");

        auto todo_worker = [&](const std::string& sid, const std::string& tag) {
            for (int i = 0; i < kWritesPerThread; ++i) {
                std::vector<haicode::Todo> list;
                for (int j = 0; j <= i % 3; ++j) {
                    haicode::Todo t;
                    t.content = tag + "-" + std::to_string(i) + "-" + std::to_string(j);
                    t.active_form = t.content;
                    t.status = "pending";
                    list.push_back(t);
                }
                store.replace_todos(sid, list);
            }
        };
        std::thread t1(todo_worker, s1.id, "one");
        std::thread t2(todo_worker, s2.id, "two");
        t1.join();
        t2.join();

        // Each session's final list must be exactly its own last write
        // (tag "one" -> 1 + (kWritesPerThread-1)%3 items, all prefixed "one").
        auto check = [&](const std::string& sid, const std::string& tag) {
            auto todos = store.load_todos(sid);
            size_t expect_n = 1 + (size_t)((kWritesPerThread - 1) % 3);
            TEST_REQUIRE(todos.size() == expect_n,
                "iter " + std::to_string(iter) + " session " + tag + ": expected "
                + std::to_string(expect_n) + " todos, got "
                + std::to_string(todos.size()));
            for (size_t j = 0; j < todos.size(); ++j) {
                TEST_REQUIRE(todos[j].content.rfind(tag + "-", 0) == 0,
                    "iter " + std::to_string(iter) + " session " + tag
                    + ": foreign row '" + todos[j].content + "'");
                TEST_REQUIRE(todos[j].content ==
                    tag + "-" + std::to_string(kWritesPerThread - 1) + "-"
                    + std::to_string(j),
                    "iter " + std::to_string(iter) + " session " + tag
                    + ": stale row '" + todos[j].content + "'");
            }
        };
        check(s1.id, "one");
        check(s2.id, "two");
    }
    std::cout << "[OK] " << kTodoIters << " iterations: concurrent todo replaces"
              << " keep each session's final list exact (no absorption)" << std::endl;

    // --- Appends racing a todo transaction on the shared connection ---
    {
        std::remove(path.c_str());
        haicode::Database db(path);
        db.migrate();
        haicode::SessionStore store(db);
        auto s1 = store.create("/proj1", "default", "{}");
        auto s2 = store.create("/proj2", "default", "{}");

        std::thread appender([&]() {
            for (int i = 0; i < 100; ++i)
                store.append_message(s2.id, "assistant_text",
                                     "{\"role\":\"assistant\",\"text\":\"x\"}");
        });
        std::thread todoer([&]() {
            for (int i = 0; i < 100; ++i) {
                std::vector<haicode::Todo> list(1);
                list[0].content = "t" + std::to_string(i);
                list[0].active_form = list[0].content;
                list[0].status = "pending";
                store.replace_todos(s1.id, list);
            }
        });
        appender.join();
        todoer.join();

        TEST_REQUIRE(store.load_messages(s2.id).size() == 100,
                     "appends lost while todos transaction raced");
        TEST_REQUIRE(store.load_todos(s1.id).size() == 1,
                     "todo replace corrupted while appends raced");
        TEST_REQUIRE(store.load_todos(s1.id)[0].content == "t99",
                     "todo final row is not the last write");
        std::cout << "[OK] append vs todo-transaction race: both intact" << std::endl;
    }

    std::cout << std::endl << "All test_db_concurrency tests passed!" << std::endl;
    return 0;
}
