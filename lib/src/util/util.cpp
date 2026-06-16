#include <haicode/util.h>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <random>
#include <curl/curl.h>
#include <cstring>

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

} // namespace util

// ---- HttpClient ----

struct HttpClient::State {
    CURL* curl = nullptr;
    std::string buffer;
    SSECallback callback;
    bool cancelled = false;

    static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* s = static_cast<State*>(userdata);
        if (s->cancelled) return 0;

        s->buffer.append(ptr, size * nmemb);

        // Parse SSE lines
        size_t pos = 0;
        std::string event_type;
        std::string event_data;

        while (true) {
            size_t nl = s->buffer.find('\n', pos);
            if (nl == std::string::npos) break;

            std::string line = s->buffer.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            pos = nl + 1;

            if (line.empty()) {
                // End of SSE event
                if (!event_data.empty()) {
                    SSEEvent ev;
                    ev.event = event_type;
                    ev.data  = event_data;
                    if (!s->callback(ev)) {
                        s->cancelled = true;
                        s->buffer.erase(0, pos);
                        return 0;
                    }
                }
                event_type.clear();
                event_data.clear();
            } else if (line.rfind("event:", 0) == 0) {
                event_type = line.substr(6);
                if (!event_type.empty() && event_type[0] == ' ')
                    event_type = event_type.substr(1);
            } else if (line.rfind("data:", 0) == 0) {
                event_data = line.substr(5);
                if (!event_data.empty() && event_data[0] == ' ')
                    event_data = event_data.substr(1);
            }
        }

        s->buffer.erase(0, pos);
        return size * nmemb;
    }
};

HttpClient::HttpClient() : state_(std::make_unique<State>()) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    state_->curl = curl_easy_init();
}

HttpClient::~HttpClient() {
    if (state_->curl) curl_easy_cleanup(state_->curl);
}

void HttpClient::cancel() {
    state_->cancelled = true;
}

void HttpClient::post_sse(const std::string& url,
                           const std::map<std::string, std::string>& headers,
                           const std::string& body,
                           SSECallback callback) {
    CURL* curl = state_->curl;
    state_->callback = callback;
    state_->cancelled = false;
    state_->buffer.clear();

    curl_easy_reset(curl);
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
}

std::string HttpClient::get(const std::string& url,
                             const std::map<std::string, std::string>& headers) {
    CURL* curl = state_->curl;
    state_->cancelled = false;
    std::string result;

    auto write_fn = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        auto* s = static_cast<std::string*>(userdata);
        s->append(ptr, size * nmemb);
        return size * nmemb;
    };

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +write_fn);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    struct curl_slist* hlist = nullptr;
    for (auto& [k, v] : headers)
        hlist = curl_slist_append(hlist, (k + ": " + v).c_str());
    if (hlist) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);

    curl_easy_perform(curl);
    if (hlist) curl_slist_free_all(hlist);
    return result;
}

} // namespace haicode
