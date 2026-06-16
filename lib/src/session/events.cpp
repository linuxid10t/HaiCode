#include <haicode/events.h>

namespace haicode {

void SessionEventBus::subscribe(events::EventType type, Handler handler) {
    std::lock_guard<std::mutex> lock(mu_);
    handlers_[type].push_back(std::move(handler));
}

void SessionEventBus::unsubscribe_all() {
    std::lock_guard<std::mutex> lock(mu_);
    handlers_.clear();
}

void SessionEventBus::publish(events::EventType type, const nlohmann::json& data) {
    std::vector<Handler> handlers_copy;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = handlers_.find(type);
        if (it != handlers_.end())
            handlers_copy = it->second;
    }
    for (auto& h : handlers_copy)
        h(data);
}

} // namespace haicode
