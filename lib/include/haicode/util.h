#pragma once
#include <string>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>

namespace haicode {
namespace util {

// Generate unique IDs with given prefix, descending time order
std::string make_id(const std::string& prefix);

// Current time in milliseconds since epoch
int64_t now_ms();

} // namespace util

// libcurl-based HTTP client with SSE support
struct SSEEvent {
    std::string event;
    std::string data;
};

class HttpClient {
public:
    using SSECallback = std::function<bool(const SSEEvent&)>;

    HttpClient();
    ~HttpClient();

    // POST with streaming SSE response
    void post_sse(const std::string& url,
                  const std::map<std::string, std::string>& headers,
                  const std::string& body,
                  SSECallback callback);

    // Simple GET. timeout_seconds caps the whole transfer (default 60s).
    // response_code (out, optional): HTTP status, or -1 on transport failure.
    std::string get(const std::string& url,
                    const std::map<std::string, std::string>& headers,
                    long timeout_seconds = 60,
                    long* response_code = nullptr);

    void cancel();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace haicode
