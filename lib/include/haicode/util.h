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

// Base64-encode raw bytes (standard alphabet with padding).
std::string base64_encode(const std::string& raw);

// Reverse of base64_encode (standard alphabet). Decoding stops at '='
// padding or the first invalid character, returning the best-effort prefix.
std::string base64_decode(const std::string& in);

// Return a copy of s that is guaranteed to be valid UTF-8: every invalid or
// truncated byte sequence is replaced with U+FFFD. Valid input passes through
// unchanged. Used on external content (web pages, command output) before it
// is embedded in JSON — nlohmann's strict serializer throws otherwise.
std::string sanitize_utf8(const std::string& s);

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

    // Plain JSON POST (not SSE). Used for native endpoints like Ollama's
    // /api/show. response_code follows the same convention as get().
    std::string post_json(const std::string& url,
                          const std::map<std::string, std::string>& headers,
                          const std::string& body,
                          long timeout_seconds = 60,
                          long* response_code = nullptr);

    void cancel();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace haicode
