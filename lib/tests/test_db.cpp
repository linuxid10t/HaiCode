#include <haicode/db.h>
#include <haicode/util.h>
#include <iostream>
#include <cassert>
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
    assert(!session.id.empty());
    std::cout << "[OK] Session created: " << session.id << std::endl;

    // List sessions
    auto sessions = store.list();
    assert(sessions.size() == 1);
    std::cout << "[OK] Listed " << sessions.size() << " session(s)" << std::endl;

    // Append a message
    store.append_message(session.id, "user_prompted",
        "{\"role\":\"user\",\"text\":\"Hello, world!\"}");
    std::cout << "[OK] Appended message" << std::endl;

    // Reload messages
    auto messages = store.load_messages(session.id);
    assert(messages.size() == 1);
    std::cout << "[OK] Loaded " << messages.size() << " message(s)" << std::endl;

    // Get by ID
    auto got = store.get(session.id);
    assert(got.has_value());
    assert(got->id == session.id);
    std::cout << "[OK] Retrieved session by ID" << std::endl;

    std::cout << std::endl << "All Phase 1 tests passed!" << std::endl;
    return 0;
}
