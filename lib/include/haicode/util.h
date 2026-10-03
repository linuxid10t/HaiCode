#pragma once
#include <string>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <sys/stat.h>

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
// is fsynced, then renamed over the target. Returns "" on success, error
// text otherwise. Parent-directory creation is the caller's responsibility.
//
// Mode selection: with the default `preserve_mode`, a pre-existing target's
// permission bits are kept (an 0755 script keeps its execute bits) and new
// files are created 0644. An explicit `mode` (e.g. 0600 for secrets) always
// wins, INCLUDING over a pre-existing target's bits — secrets must tighten,
// never inherit a group/world-readable 0644.
//
// Haiku semantics: BFS attributes (MIME type, Tracker metadata, ...) are
// copied from the existing target onto the replacement — a bare rename
// would drop them. If `path` is a symlink, the write goes through to the
// resolved target and the link itself is preserved; a symlink that cannot
// be resolved (dangling, loop) is refused with an error and nothing is
// written.
std::string atomic_write_file(const std::string& path, const std::string& content,
                              mode_t mode = (mode_t)0);

// Tighten an existing file to owner-only (0600) when it currently carries
// group/world bits. A no-op for already-0600 (or stricter) files and for
// missing files. Returns "" on success, error text otherwise. Applied at
// startup to secret files (config/token stores) that earlier releases or
// other editors may have created world-readable.
std::string ensure_owner_only(const std::string& path);

// Lowercase hex SHA-256 of `data`. Used for config trust fingerprints.
std::string sha256_hex(const std::string& data);

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
    //
    // retry_after (out, optional): the server's Retry-After header value when
    // present and seconds-form (HTTP-dates are ignored — callers can't act
    // on them portably). For HTTP-level failures the same value is appended
    // to *transport_error as " [retry-after: N]" so it survives the
    // provider→engine error-string path.
    //
    // Cancellation/liveness: cancel() aborts the transfer from any phase
    // (including connect and silent header waits) via the progress callback.
    // There is no total timeout; instead a connect gets 30 s and a transfer
    // sustained below 1 byte/s for stall_timeout() seconds (default 60)
    // aborts (CURLE_OPERATION_TIMEDOUT) — long generations legitimately run
    // for minutes. Redirects are never
    // followed: the request carries credentials that must not be re-sent
    // cross-host. The unparsed stream buffer is capped at 2 MB — a larger
    // single line/stream aborts with transport_error "response line/stream
    // exceeded size cap".
    void post_sse(const std::string& url,
                  const std::map<std::string, std::string>& headers,
                  const std::string& body,
                  SSECallback callback,
                  long* response_code = nullptr,
                  std::string* transport_error = nullptr,
                  std::string* retry_after = nullptr);

    // Simple GET. timeout_seconds caps the whole transfer (default 60s).
    // Redirects are followed SAME-HOST ONLY (<= 5 hops; hostname compared,
    // port ignored so http->https upgrades still work). A cross-host 3xx is
    // returned as-is. Bodies are capped at 10 MB: an over-cap transfer is
    // aborted, the truncated prefix returned, and *truncated set true.
    // response_code (out, optional): HTTP status, or -1 on transport failure.
    std::string get(const std::string& url,
                    const std::map<std::string, std::string>& headers,
                    long timeout_seconds = 60,
                    long* response_code = nullptr,
                    bool* truncated = nullptr);

    // Plain JSON POST (not SSE). Used for native endpoints like Ollama's
    // /api/show. Redirects are never followed (credential-bearing). Body
    // cap and *truncated follow get(); response_code follows its convention.
    std::string post_json(const std::string& url,
                          const std::map<std::string, std::string>& headers,
                          const std::string& body,
                          long timeout_seconds = 60,
                          long* response_code = nullptr,
                          bool* truncated = nullptr);

    void cancel();

    // Silent-stream limit for post_sse(): seconds below 1 byte/s before the
    // transfer aborts. Default kDefaultStallTimeoutSec suits hosted APIs,
    // which send keep-alives; local servers (llama.cpp, Ollama, ...) stay
    // silent for the whole prompt prefill, which on a large prompt runs for
    // minutes, so their providers raise it. <= 0 disables the check (cancel()
    // still works). Applies to requests started after the call.
    static constexpr long kDefaultStallTimeoutSec = 60;
    void set_stall_timeout(long seconds);
    long stall_timeout() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace haicode
