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

// Longest prefix of s not exceeding max_bytes bytes that does not split a
// UTF-8 sequence: a cut would otherwise orphan a lead byte and make every
// downstream .dump() throw (type_error.316). Returns s unchanged when it
// already fits. Input is assumed valid UTF-8; use sanitize_utf8 first on
// external content.
std::string truncate_utf8(const std::string& s, size_t max_bytes);

// Atomically replace `path` with `content`: the temp file is created via
// mkstemp (O_CREAT|O_EXCL — it can never clobber a pre-existing sibling),
// inherits the target's previous permission bits when the target existed
// (an 0755 script keeps its execute bits), is fsynced, then renamed over
// the target. Returns "" on success, error text otherwise. Parent-directory
// creation is the caller's responsibility.
std::string atomic_write_file(const std::string& path, const std::string& content);

// Create a securely-named scratch file via mkstemp on tmpl_prefix +
// "XXXXXX". For temp files that are deleted when done (not renamed over a
// target). Returns the open fd (caller closes and unlinks), or -1 on
// failure; out_path receives the created name.
int make_secure_temp(const std::string& tmpl_prefix, std::string& out_path);

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

    // POST with streaming SSE response. Out-params follow the get()/
    // post_json() convention: response_code is the HTTP status, or -1 on
    // transport failure; transport_error then carries the curl error text
    // (transport failures only). A stream stopped early because the callback
    // returned false (OpenAI [DONE], consumer cancel) is NOT a transport
    // failure — the response code is still reported.
    void post_sse(const std::string& url,
                  const std::map<std::string, std::string>& headers,
                  const std::string& body,
                  SSECallback callback,
                  long* response_code = nullptr,
                  std::string* transport_error = nullptr);

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
