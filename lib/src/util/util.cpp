#include <haicode/util.h>
#include <atomic>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <random>
#include <curl/curl.h>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>

namespace haicode {
namespace util {

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string make_id(const std::string& prefix) {
    // Descending sort: use (MAX_INT64 - now_ms) as base, plus random suffix
    int64_t desc = INT64_MAX - now_ms();
    std::mt19937_64 rng(std::random_device{}());
    uint64_t rnd = rng();

    std::ostringstream ss;
    ss << prefix << "_"
       << std::hex << std::setw(16) << std::setfill('0') << (uint64_t)desc
       << std::hex << std::setw(8)  << std::setfill('0') << (uint32_t)(rnd & 0xFFFFFFFF);
    return ss.str();
}

std::string base64_encode(const std::string& raw) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((raw.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < raw.size()) {
        uint32_t n = (uint8_t)raw[i] << 16 | (uint8_t)raw[i + 1] << 8
                   | (uint8_t)raw[i + 2];
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += table[(n >> 6) & 63];
        out += table[n & 63];
        i += 3;
    }
    size_t rem = raw.size() - i;
    if (rem == 1) {
        uint32_t n = (uint8_t)raw[i] << 16;
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += "==";
    } else if (rem == 2) {
        uint32_t n = (uint8_t)raw[i] << 16 | (uint8_t)raw[i + 1] << 8;
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += table[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

std::string base64_decode(const std::string& in) {
    // Reverse of base64_encode: standard alphabet, '=' padding terminates the
    // final group, any other character stops decoding (best-effort prefix).
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    out.reserve(in.size() / 4 * 3 + 3);
    uint32_t n = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r') break;
        int v = val(c);
        if (v < 0) break;
        n = (n << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((n >> bits) & 0xFF);
        }
    }
    return out;
}

std::string sanitize_utf8(const std::string& s) {
    static const char kReplacement[] = "\xEF\xBF\xBD"; // U+FFFD
    std::string out;
    out.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        uint8_t b = (uint8_t)s[i];
        if (b < 0x80) { // ASCII
            out += (char)b;
            ++i;
            continue;
        }
        // Determine expected sequence length from the leading byte.
        int len = 0;
        uint32_t cp = 0;
        if ((b & 0xE0) == 0xC0)      { len = 2; cp = b & 0x1F; }
        else if ((b & 0xF0) == 0xE0) { len = 3; cp = b & 0x0F; }
        else if ((b & 0xF8) == 0xF0) { len = 4; cp = b & 0x07; }
        // Stray continuation byte or invalid leading byte (C0/C1 overlong,
        // F5-FF): replace this one byte and rescan.
        if (len == 0 || i + len > n) {
            out += kReplacement;
            ++i;
            continue;
        }
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            uint8_t c = (uint8_t)s[i + k];
            if ((c & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (c & 0x3F);
        }
        // Reject overlong encodings, UTF-16 surrogates, and out-of-range.
        if (ok && len == 2 && cp < 0x80) ok = false;
        if (ok && len == 3 && cp < 0x800) ok = false;
        if (ok && len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ok = false;
        if (ok && cp >= 0xD800 && cp <= 0xDFFF) ok = false; // UTF-16 surrogates
        if (!ok) {
            out += kReplacement;
            ++i; // rescan from the next byte — may start a valid sequence
            continue;
        }
        out.append(s, i, len);
        i += len;
    }
    return out;
}

std::string atomic_write_file(const std::string& path, const std::string& content) {
    // Remember the target's permission bits so replacement preserves them
    // (an 0755 script keeps its execute bits). 0 = target doesn't exist yet.
    mode_t mode = 0;
    struct stat st{};
    if (stat(path.c_str(), &st) == 0)
        mode = st.st_mode & 07777;

    // mkstemp demands trailing X's and creates with O_EXCL semantics — a
    // pre-existing file of a similar name can never be clobbered.
    std::string tmpl = path + ".tmp_write_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = mkstemp(buf.data());
    if (fd < 0)
        return "Cannot create temp file for " + path + ": " + strerror(errno);
    std::string tmp_path(buf.data());

    if (fchmod(fd, mode != 0 ? mode : (mode_t)0644) != 0) {
        std::string err = "Cannot set mode on temp file: " + std::string(strerror(errno));
        close(fd);
        unlink(tmp_path.c_str());
        return err;
    }

    // Write loop: ::write may transfer less than requested.
    const char* data = content.data();
    size_t remaining = content.size();
    while (remaining > 0) {
        ssize_t n = ::write(fd, data, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::string err = "Write error on " + tmp_path + ": " + strerror(errno);
            close(fd);
            unlink(tmp_path.c_str());
            return err;
        }
        data += n;
        remaining -= static_cast<size_t>(n);
    }

    if (fsync(fd) != 0) {
        std::string err = "Flush error on " + tmp_path + ": " + strerror(errno);
        close(fd);
        unlink(tmp_path.c_str());
        return err;
    }
    if (close(fd) != 0) {
        std::string err = "Close error on " + tmp_path + ": " + strerror(errno);
        unlink(tmp_path.c_str());
        return err;
    }

    if (rename(tmp_path.c_str(), path.c_str()) != 0) {
        std::string err = "Cannot rename to target: " + path + ": " + strerror(errno);
        unlink(tmp_path.c_str());
        return err;
    }
    return "";
}

int make_secure_temp(const std::string& tmpl_prefix, std::string& out_path) {
    std::string tmpl = tmpl_prefix + "XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = mkstemp(buf.data());
    if (fd >= 0)
        out_path.assign(buf.data());
    return fd;
}

} // namespace util

// ---- HttpClient ----

struct HttpClient::State {
    // Set from cancel() on the UI thread, read from write_cb on the request
    // thread — atomic to avoid a torn flag.
    std::atomic<bool> cancelled{false};
    // SSE parse state lives per client (not per request): a single SSE event
    // (event:/data:/blank line) can straddle libcurl chunk boundaries when the
    // data line is large (e.g. propose_plan markdown). Local vars here
    // silently dropped in-progress events, truncating tool input JSON.
    std::string buffer;
    std::string event_type;
    std::string event_data;
    SSECallback callback;

    static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* s = static_cast<State*>(userdata);
        if (s->cancelled) return 0;

        s->buffer.append(ptr, size * nmemb);

        // Parse SSE lines
        size_t pos = 0;

        while (true) {
            size_t nl = s->buffer.find('\n', pos);
            if (nl == std::string::npos) break;

            std::string line = s->buffer.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            pos = nl + 1;

            if (line.empty()) {
                // End of SSE event
                if (!s->event_data.empty()) {
                    SSEEvent ev;
                    ev.event = s->event_type;
                    ev.data  = s->event_data;
                    if (!s->callback(ev)) {
                        s->cancelled = true;
                        s->buffer.erase(0, pos);
                        return 0;
                    }
                }
                s->event_type.clear();
                s->event_data.clear();
            } else if (line.rfind("event:", 0) == 0) {
                s->event_type = line.substr(6);
                if (!s->event_type.empty() && s->event_type[0] == ' ')
                    s->event_type = s->event_type.substr(1);
            } else if (line.rfind("data:", 0) == 0) {
                s->event_data = line.substr(5);
                if (!s->event_data.empty() && s->event_data[0] == ' ')
                    s->event_data = s->event_data.substr(1);
            }
        }

        s->buffer.erase(0, pos);
        return size * nmemb;
    }
};

HttpClient::HttpClient() : state_(std::make_unique<State>()) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

HttpClient::~HttpClient() = default;

void HttpClient::cancel() {
    state_->cancelled = true;
}

// Easy handles are not thread-safe, and a single provider client can serve
// concurrent requests (e.g. the settings-save flow fires list_models on a
// detached thread while the UI thread fetches the model context). A shared
// handle raced inside Curl_checkheaders and crashed the app — so each request
// gets its own handle, initialized and cleaned up around perform.
static CURL* fresh_handle() {
    return curl_easy_init();
}

void HttpClient::post_sse(const std::string& url,
                           const std::map<std::string, std::string>& headers,
                           const std::string& body,
                           SSECallback callback) {
    state_->callback = callback;
    state_->cancelled = false;
    state_->buffer.clear();
    state_->event_type.clear();
    state_->event_data.clear();

    CURL* curl = fresh_handle();
    if (!curl) return;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, State::write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, state_.get());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);

    struct curl_slist* hlist = nullptr;
    for (auto& [k, v] : headers)
        hlist = curl_slist_append(hlist, (k + ": " + v).c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);

    curl_easy_perform(curl);
    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);
}

std::string HttpClient::get(const std::string& url,
                             const std::map<std::string, std::string>& headers,
                             long timeout_seconds,
                             long* response_code) {
    std::string result;

    auto write_fn = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        auto* s = static_cast<std::string*>(userdata);
        s->append(ptr, size * nmemb);
        return size * nmemb;
    };

    CURL* curl = fresh_handle();
    if (!curl) {
        if (response_code) *response_code = -1;
        return result;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +write_fn);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);

    struct curl_slist* hlist = nullptr;
    for (auto& [k, v] : headers)
        hlist = curl_slist_append(hlist, (k + ": " + v).c_str());
    if (hlist) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);

    CURLcode res = curl_easy_perform(curl);
    if (response_code) {
        if (res != CURLE_OK) {
            *response_code = -1;  // transport-level failure (DNS, conn, timeout)
        } else {
            long code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            *response_code = code;
        }
    }
    if (hlist) curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);
    return result;
}

std::string HttpClient::post_json(const std::string& url,
                                  const std::map<std::string, std::string>& headers,
                                  const std::string& body,
                                  long timeout_seconds,
                                  long* response_code) {
    std::string result;

    auto write_fn = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        auto* s = static_cast<std::string*>(userdata);
        s->append(ptr, size * nmemb);
        return size * nmemb;
    };

    CURL* curl = fresh_handle();
    if (!curl) {
        if (response_code) *response_code = -1;
        return result;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +write_fn);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);

    struct curl_slist* hlist = nullptr;
    for (auto& [k, v] : headers)
        hlist = curl_slist_append(hlist, (k + ": " + v).c_str());
    if (hlist) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);

    CURLcode res = curl_easy_perform(curl);
    if (response_code) {
        if (res != CURLE_OK) {
            *response_code = -1;
        } else {
            long code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            *response_code = code;
        }
    }
    if (hlist) curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);
    return result;
}

} // namespace haicode
