#include <haicode/permission_requests.h>
#include <chrono>

namespace haicode {

using namespace std::chrono;

PermissionRequestBroker::PermissionRequestBroker(PermissionGate* gate)
    : gate_(gate)
{
}

void PermissionRequestBroker::set_delivery_callback(DeliverFn fn) {
    std::lock_guard<std::mutex> lock(mu_);
    deliver_fn_ = std::move(fn);
}

void PermissionRequestBroker::set_notify_callback(NotifyFn fn) {
    std::lock_guard<std::mutex> lock(mu_);
    notify_fn_ = std::move(fn);
}

PermissionOutcome PermissionRequestBroker::submit(PermissionRequest req) {
    // Cancellation checks and registration happen under one lock step, so a
    // cancel that lands mid-registration either sees the entry (and wakes it
    // with a Deny) or blocks the session before this submit registers.
    DeliverFn deliver;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (shutdown_) {
            PermissionOutcome out;
            out.effect = PermissionEffect::Deny;
            out.reason = shutdown_reason_.empty()
                ? "permission requests cancelled" : shutdown_reason_;
            return out;
        }
        auto blocked = blocked_sessions_.find(req.session_id);
        if (blocked != blocked_sessions_.end()) {
            PermissionOutcome out;
            out.effect = PermissionEffect::Deny;
            out.reason = blocked->second.empty()
                ? "run interrupted" : blocked->second;
            return out;
        }

        char buf[32];
        snprintf(buf, sizeof(buf), "prq_%llx",
                 static_cast<unsigned long long>(next_id_.fetch_add(1)));
        req.id = buf;
        req.created_ms = duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count();
        pending_[req.id] = Entry{req, false, {}};
        deliver = deliver_fn_;
    }

    // Delivery runs without the lock: the callback posts to a BeAPI looper
    // and must never wait on this thread while the broker is locked. No
    // delivery callback at all fails closed (deny) rather than parking the
    // worker where nobody can ever resolve it.
    if (!deliver || !deliver(req)) {
        PermissionOutcome out;
        out.effect = PermissionEffect::Deny;
        out.reason = "approval window unavailable";
        NotifyFn notify;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = pending_.find(req.id);
            if (it != pending_.end() && !it->second.resolved) {
                it->second.resolved = true;
                it->second.outcome = out;
            }
            notify = notify_fn_;
        }
        if (notify) notify(req, out);
        {
            std::lock_guard<std::mutex> lock(mu_);
            pending_.erase(req.id);
        }
        return out;
    }

    // Wait until resolved/cancelled. std::map nodes are stable, so holding
    // the entry reference across the wait is safe.
    PermissionOutcome out;
    NotifyFn notify;
    {
        std::unique_lock<std::mutex> lock(mu_);
        auto it = pending_.find(req.id);
        if (it == pending_.end()) {
            // Resolved and retired between delivery and the wait (e.g. a
            // cancel that raced registration) — the outcome is gone; deny.
            PermissionOutcome fallback;
            fallback.effect = PermissionEffect::Deny;
            fallback.reason = "approval cancelled";
            return fallback;
        }
        Entry& e = it->second;
        cv_.wait(lock, [&e] { return e.resolved; });
        out = e.outcome;
        notify = notify_fn_;
        pending_.erase(it);
    }
    if (notify) notify(req, out);
    return out;
}

bool PermissionRequestBroker::resolve(const std::string& request_id,
                                      PermissionDecision decision) {
    PermissionRequest req;
    PermissionOutcome out;
    NotifyFn notify;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = pending_.find(request_id);
        if (it == pending_.end() || it->second.resolved) return false;
        Entry& e = it->second;
        req = e.req;

        switch (decision) {
        case PermissionDecision::AllowOnce:
            out.effect = PermissionEffect::Allow;
            out.reason = "allowed once by user";
            break;
        case PermissionDecision::AllowForSession:
            out.effect = PermissionEffect::Allow;
            out.reason = "allowed for this session by user";
            break;
        case PermissionDecision::Deny:
            out.effect = PermissionEffect::Deny;
            out.reason = "denied by user";
            break;
        }
        out.user_decided = true;

        // The session grant is installed BEFORE the waiter wakes, so no
        // engine thread can observe approval without the grant already
        // covering repeat requests. Lock order is broker→gate only; the gate
        // never calls back into the broker while holding its own lock.
        if (decision == PermissionDecision::AllowForSession && gate_)
            gate_->add_exact_allow(req.session_id, req.action, req.resource);

        e.resolved = true;
        e.outcome = out;
        notify = notify_fn_;
        found = true;
        cv_.notify_all();
    }
    if (found && notify) notify(req, out);
    return found;
}

void PermissionRequestBroker::cancel_session(const std::string& session_id,
                                             const std::string& reason) {
    std::vector<std::pair<PermissionRequest, PermissionOutcome>> done;
    {
        std::lock_guard<std::mutex> lock(mu_);
        blocked_sessions_[session_id] = reason;
        for (auto& [id, e] : pending_) {
            if (e.req.session_id != session_id || e.resolved) continue;
            e.resolved = true;
            e.outcome.effect = PermissionEffect::Deny;
            e.outcome.reason = reason.empty() ? "run interrupted" : reason;
            done.push_back({e.req, e.outcome});
        }
        cv_.notify_all();
    }
    // Notifications fire after the lock is released — a notify handler may
    // inspect broker state or post to a looper without deadlocking us.
    NotifyFn notify;
    {
        std::lock_guard<std::mutex> lock(mu_);
        notify = notify_fn_;
    }
    if (notify)
        for (auto& [req, out] : done) notify(req, out);
}

void PermissionRequestBroker::cancel_all(const std::string& reason) {
    std::vector<std::pair<PermissionRequest, PermissionOutcome>> done;
    {
        std::lock_guard<std::mutex> lock(mu_);
        shutdown_ = true;
        shutdown_reason_ = reason;
        for (auto& [id, e] : pending_) {
            if (e.resolved) continue;
            e.resolved = true;
            e.outcome.effect = PermissionEffect::Deny;
            e.outcome.reason = reason.empty() ? "shutting down" : reason;
            done.push_back({e.req, e.outcome});
        }
        cv_.notify_all();
    }
    NotifyFn notify;
    {
        std::lock_guard<std::mutex> lock(mu_);
        notify = notify_fn_;
    }
    if (notify)
        for (auto& [req, out] : done) notify(req, out);
}

void PermissionRequestBroker::allow_submissions(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    blocked_sessions_.erase(session_id);
}

std::vector<PermissionRequest> PermissionRequestBroker::pending_requests() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<PermissionRequest> out;
    out.reserve(pending_.size());
    for (auto& [id, e] : pending_)
        if (!e.resolved) out.push_back(e.req);
    return out;
}

size_t PermissionRequestBroker::pending_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    size_t n = 0;
    for (auto& [id, e] : pending_)
        if (!e.resolved) ++n;
    return n;
}

} // namespace haicode
