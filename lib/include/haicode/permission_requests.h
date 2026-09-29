#pragma once
#include "tool.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace haicode {

// What the user picked in the approval window.
enum class PermissionDecision {
    Deny,
    AllowOnce,
    AllowForSession,   // installs an exact (action, resource) grant for the session
};

// One approval request as submitted by the engine's gate callback. Carries
// everything the approval window and activity log need; ids are assigned by
// the broker and are immutable afterwards.
struct PermissionRequest {
    std::string id;
    std::string session_id;
    std::string call_id;
    std::string tool_name;
    std::string action;       // permission category (Tool::required_permission)
    std::string resource;     // exact resource string the gate matched on
    std::string working_dir;
    nlohmann::json input;
    int64_t created_ms = 0;
};

// How a request ended up resolving.
struct PermissionOutcome {
    PermissionEffect effect = PermissionEffect::Deny;
    std::string reason;
    bool user_decided = false;   // true only for an explicit window decision
};

// Owns the wait state for pending permission approvals. The engine worker
// blocks in submit(); the GUI resolves by request id — no promise pointers
// cross threads. Every request resolves exactly once: duplicate, stale, or
// post-cancellation replies can never approve anything.
//
// Lock ordering: the broker never calls into PermissionGate while holding an
// engine-side gate check (the gate invokes its ask callback without its own
// mutex held), so resolve() taking broker-then-gate locks is safe.
class PermissionRequestBroker {
public:
    // Returns true when the request was handed to a UI, false when delivery
    // failed (caller denies the request).
    using DeliverFn = std::function<bool(const PermissionRequest&)>;
    // Fired once per request, after the waiter has been woken and the entry
    // retired — never while any broker lock is held.
    using NotifyFn = std::function<void(const PermissionRequest&,
                                        const PermissionOutcome&)>;

    explicit PermissionRequestBroker(PermissionGate* gate = nullptr);

    void set_delivery_callback(DeliverFn fn);
    void set_notify_callback(NotifyFn fn);

    // Blocks the calling engine worker until the request is resolved,
    // cancelled, or fails delivery. Never prompts for a session whose run was
    // interrupted (see cancel_session) — those deny immediately.
    PermissionOutcome submit(PermissionRequest req);

    // GUI thread. Exactly-once: returns false for unknown, already-resolved,
    // or cancelled requests without side effects. AllowForSession installs
    // the exact grant in the gate BEFORE waking the waiter, so approval and
    // grant can never be observed out of order.
    bool resolve(const std::string& request_id, PermissionDecision decision);

    // Wake every pending wait for the session with a Deny and auto-deny new
    // submissions for it until allow_submissions() re-arms it (next run).
    void cancel_session(const std::string& session_id, const std::string& reason);
    // Wake ALL pending waits and reject every future submission (shutdown).
    void cancel_all(const std::string& reason);
    void allow_submissions(const std::string& session_id);

    // Snapshot for the Permissions center (still-waiting requests only).
    std::vector<PermissionRequest> pending_requests() const;
    size_t pending_count() const;

private:
    struct Entry {
        PermissionRequest req;
        bool resolved = false;
        PermissionOutcome outcome;
    };

    PermissionGate* gate_ = nullptr;   // optional; installs session grants
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, Entry> pending_;
    std::map<std::string, std::string> blocked_sessions_;  // sid -> reason
    bool shutdown_ = false;
    std::string shutdown_reason_;
    DeliverFn deliver_fn_;
    NotifyFn notify_fn_;
    std::atomic<uint64_t> next_id_{1};
};

} // namespace haicode
