// HttpClient::post_sse failure reporting + provider-level empty-success fix
// (review #8). A one-shot local TCP server serves canned responses; a closed
// port stands in for an unreachable endpoint.
#include <haicode/util.h>
#include <haicode/haicode.h>
#include <haicode/provider.h>
#include <memory>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << (msg) << "\n"; return false; } } while(0)

// Bind 127.0.0.1:0, return the chosen port (fd stays open in `listen_fd`).
static int bind_ephemeral(int& listen_fd) {
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0;
    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) != 0) return -1;
    if (listen(listen_fd, 1) != 0) return -1;
    socklen_t len = sizeof(addr);
    if (getsockname(listen_fd, (sockaddr*)&addr, &len) != 0) return -1;
    return ntohs(addr.sin_port);
}

// Accept one connection, drain the request, write `response`, close.
static void serve_once(int listen_fd, const std::string& response) {
    int c = accept(listen_fd, nullptr, nullptr);
    if (c < 0) return;
    // Don't hang forever on a misbehaving client.
    timeval tv{2, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[4097];
    while (true) {
        ssize_t n = recv(c, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        // drain until EOF or timeout — curl sends one small request
        if (strstr(buf, "Content-Length") != nullptr
                && strstr(buf, "\r\n\r\n") != nullptr)
            break;
    }
    (void)!write(c, response.data(), response.size());
    close(c);
}

// Drain request headers (works for GET and small POST bodies alike — one
// packet carries headers, and usually the whole request).
static void drain_request(int c) {
    timeval tv{2, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[4097];
    while (true) {
        ssize_t n = recv(c, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        if (strstr(buf, "\r\n\r\n") != nullptr) break;
    }
}

static bool test_transport_failure_reports_minus_one() {
    int fd = -1;
    int port = bind_ephemeral(fd);
    CHECK(port > 0, "ephemeral bind failed");
    close(fd);  // nothing listens now

    haicode::HttpClient http;
    long code = 42;
    std::string terr = "unset";
    int events = 0;
    http.post_sse("http://127.0.0.1:" + std::to_string(port),
                  {}, "{}",
                  [&](const haicode::SSEEvent&) { events++; return true; },
                  &code, &terr);
    CHECK(code == -1, "closed port must report response_code -1");
    CHECK(!terr.empty(), "transport error text should be set");
    CHECK(events == 0, "no SSE events should be delivered on transport failure");
    std::cout << "[OK] post_sse transport failure -> -1, no events\n";
    return true;
}

static bool test_http_500_reports_code_and_no_events() {
    int fd = -1;
    int port = bind_ephemeral(fd);
    CHECK(port > 0, "ephemeral bind failed");
    std::thread t([&]() {
        serve_once(fd, "HTTP/1.1 500 Internal Server Error\r\n"
                       "Content-Type: application/json\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "{\"error\":{\"message\":\"boom\"}}");
    });

    haicode::HttpClient http;
    long code = 0;
    std::string terr;
    int events = 0;
    http.post_sse("http://127.0.0.1:" + std::to_string(port),
                  {}, "{}",
                  [&](const haicode::SSEEvent&) { events++; return true; },
                  &code, &terr);
    t.join();
    close(fd);
    CHECK(code == 500, "HTTP 500 must be reported as the response code");
    CHECK(events == 0, "error document must not fire the SSE callback");
    CHECK(terr.find("boom") != std::string::npos,
          "body excerpt should surface the server's message: " + terr);
    std::cout << "[OK] post_sse HTTP 500 -> code 500 + body excerpt\n";
    return true;
}

static bool test_clean_sse_stream_delivers_events() {
    int fd = -1;
    int port = bind_ephemeral(fd);
    CHECK(port > 0, "ephemeral bind failed");
    std::thread t([&]() {
        serve_once(fd, "HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/event-stream\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "event: one\n"
                       "data: first\n"
                       "\n"
                       "event: two\n"
                       "data: second\n"
                       "\n");
    });

    haicode::HttpClient http;
    long code = 0;
    std::string terr;
    std::vector<std::string> datas;
    http.post_sse("http://127.0.0.1:" + std::to_string(port),
                  {}, "{}",
                  [&](const haicode::SSEEvent& ev) {
                      datas.push_back(ev.data);
                      return true;
                  },
                  &code, &terr);
    t.join();
    close(fd);
    CHECK(code == 200, "successful stream should report 200");
    CHECK(terr.empty(), "no transport error on success");
    CHECK(datas.size() == 2 && datas[0] == "first" && datas[1] == "second",
          "events must arrive complete and in order");
    std::cout << "[OK] post_sse clean stream -> events in order, code 200\n";
    return true;
}

// End-to-end proof of the empty-success fix: an Anthropic provider pointed
// at a dead endpoint must call on_error and NEVER on_finish.
static bool test_anthropic_provider_dead_endpoint() {
    int fd = -1;
    int port = bind_ephemeral(fd);
    CHECK(port > 0, "ephemeral bind failed");
    close(fd);

    auto provider = haicode::make_anthropic_provider(
        "k", "http://127.0.0.1:" + std::to_string(port));

    haicode::LLMRequest req;
    req.model_id = "claude-x";
    req.messages = {nlohmann::json{{"role", "user"}, {"content", "hi"}}};

    bool got_error = false, got_finish = false;
    haicode::StreamCallbacks cbs;
    cbs.on_error = [&](const std::string& e) {
        got_error = true;
        std::cout << "  (on_error: " << e << ")\n";
    };
    cbs.on_finish = [&](haicode::FinishReason, haicode::TokenUsage,
                        std::vector<haicode::ToolCall>) {
        got_finish = true;
    };
    provider->stream(req, cbs);
    CHECK(got_error, "dead endpoint must produce on_error");
    CHECK(!got_finish, "dead endpoint must NOT produce an empty on_finish");
    std::cout << "[OK] anthropic provider reports failure, no empty finish\n";
    return true;
}

// Two concurrent post_sse calls on ONE HttpClient (each provider owns one
// client, and ProviderRegistry hands that provider to every session thread):
// each callback must receive exactly its own server's events, in order. The
// per-client SSE parse state this guards against made session A's tokens land
// in session B's callback and raced on the shared buffer. Looped — a single
// pass can pass by luck.
static bool test_concurrent_post_sse_streams_isolated() {
    for (int iter = 0; iter < 10; iter++) {
        int fd_a = -1, fd_b = -1;
        int port_a = bind_ephemeral(fd_a);
        int port_b = bind_ephemeral(fd_b);
        CHECK(port_a > 0 && port_b > 0, "ephemeral binds failed");

        const std::string hdr =
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n";
        const std::string body_a = hdr +
            "event: a\ndata: a1\n\nevent: a\ndata: a2\n\nevent: a\ndata: a3\n\n";
        const std::string body_b = hdr +
            "event: b\ndata: b1\n\nevent: b\ndata: b2\n\nevent: b\ndata: b3\n\n";

        std::thread srv_a([&]() { serve_once(fd_a, body_a); });
        std::thread srv_b([&]() { serve_once(fd_b, body_b); });

        haicode::HttpClient http;  // deliberately ONE client, two requests
        std::vector<std::string> got_a, got_b;
        long code_a = 0, code_b = 0;

        std::thread req_a([&]() {
            std::string terr;
            http.post_sse("http://127.0.0.1:" + std::to_string(port_a), {}, "{}",
                          [&](const haicode::SSEEvent& ev) {
                              got_a.push_back(ev.event + ":" + ev.data);
                              return true;
                          }, &code_a, &terr);
        });
        std::thread req_b([&]() {
            std::string terr;
            http.post_sse("http://127.0.0.1:" + std::to_string(port_b), {}, "{}",
                          [&](const haicode::SSEEvent& ev) {
                              got_b.push_back(ev.event + ":" + ev.data);
                              return true;
                          }, &code_b, &terr);
        });
        req_a.join();
        req_b.join();
        srv_a.join();
        srv_b.join();
        close(fd_a);
        close(fd_b);

        const std::string it = " (iter " + std::to_string(iter) + ")";
        CHECK(code_a == 200 && code_b == 200,
              "both concurrent requests should report 200" + it);
        CHECK(got_a.size() == 3, "A should see exactly 3 events" + it + ", got " +
              std::to_string(got_a.size()));
        CHECK(got_b.size() == 3, "B should see exactly 3 events" + it + ", got " +
              std::to_string(got_b.size()));
        CHECK(got_a == std::vector<std::string>({"a:a1", "a:a2", "a:a3"}),
              "A's stream must contain only A's events in order" + it);
        CHECK(got_b == std::vector<std::string>({"b:b1", "b:b2", "b:b3"}),
              "B's stream must contain only B's events in order" + it);
    }
    std::cout << "[OK] concurrent post_sse on one client: streams isolated (10 iters)\n";
    return true;
}

// A callback returning false is a deliberate early stop (consumer cancel,
// OpenAI [DONE]): not a transport failure — exactly the events before the
// stop are delivered and the HTTP status is still reported.
static bool test_callback_false_stops_stream_cleanly() {
    int fd = -1;
    int port = bind_ephemeral(fd);
    CHECK(port > 0, "ephemeral bind failed");
    std::thread t([&]() {
        serve_once(fd,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Connection: close\r\n"
            "\r\n"
            "event: one\ndata: first\n\n"
            "event: two\ndata: second\n\n"
            "event: three\ndata: third\n\n");
    });

    haicode::HttpClient http;
    long code = 0;
    std::string terr = "unset";
    std::vector<std::string> datas;
    http.post_sse("http://127.0.0.1:" + std::to_string(port),
                  {}, "{}",
                  [&](const haicode::SSEEvent& ev) {
                      datas.push_back(ev.data);
                      return false;  // stop after the first event
                  },
                  &code, &terr);
    t.join();
    close(fd);
    CHECK(datas.size() == 1 && datas[0] == "first",
          "exactly the first event before the stop, got " +
          std::to_string(datas.size()));
    CHECK(code == 200,
          "deliberate stop is not a transport failure — code stays 200, got " +
          std::to_string(code));
    CHECK(terr.empty(), "no transport error on deliberate stop, got: " + terr);
    std::cout << "[OK] callback-false stop: 1 event, code 200, no error\n";
    return true;
}

// Serve each accepted connection with responses[i] in order (redirect hops
// need multiple requests on one listener). accept() is deadline-bounded so
// a client that never sends the next hop can never hang the join.
static void serve_responses(int listen_fd, const std::vector<std::string>& responses) {
    for (const auto& r : responses) {
        timeval tv{10, 0};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listen_fd, &fds);
        int ready = select(listen_fd + 1, &fds, nullptr, nullptr, &tv);
        if (ready <= 0) return;
        int c = accept(listen_fd, nullptr, nullptr);
        if (c < 0) return;
        drain_request(c);
        (void)!write(c, r.data(), r.size());
        close(c);
    }
}

// State shared with the client thread, which may outlive this function on
// a regression (cancel fails, thread detached) — shared_ptr ownership keeps
// a detached thread from dangling against this stack frame.
struct CancelWaitState {
    haicode::HttpClient http;
    std::atomic<bool> done{false};
    long code = 42;
    std::string terr = "unset";
};

// Cancel while the transfer is silent: the server accepts, drains the
// request, optionally writes headers, then goes quiet. write_cb never fires
// in this phase — regression for cancels that used to ride data arrival and
// therefore did nothing until the 60 s low-speed / 300 s timeout.
static bool test_cancel_aborts_silent_wait(bool send_headers) {
    int fd = -1;
    int port = bind_ephemeral(fd);
    CHECK(port > 0, "ephemeral bind failed");

    auto st = std::make_shared<CancelWaitState>();
    std::atomic<bool> request_seen{false};

    std::thread srv([&] {
        int c = accept(fd, nullptr, nullptr);
        if (c < 0) return;
        drain_request(c);
        if (send_headers)
            (void)!write(c, "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/event-stream\r\n\r\n", 56);
        request_seen = true;
        for (int i = 0; i < 100 && !st->done; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        close(c);
    });

    std::thread client([st, port] {
        st->http.post_sse("http://127.0.0.1:" + std::to_string(port), {}, "{}",
                          [](const haicode::SSEEvent&) { return true; },
                          &st->code, &st->terr);
        st->done = true;
    });

    // Wait until the request is registered (server drained it), then cancel.
    for (int i = 0; i < 50 && !request_seen; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto begin = std::chrono::steady_clock::now();
    st->http.cancel();
    for (int i = 0; i < 50 && !st->done; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bool fast = std::chrono::steady_clock::now() - begin < std::chrono::seconds(5);
    if (st->done) client.join();
    else          client.detach();  // safe: state owned via shared_ptr
    srv.join();
    close(fd);

    CHECK(st->done, "post_sse must return after cancel() during a silent wait");
    CHECK(fast, "cancel must abort within ~1 s, not ride the low-speed timeout");
    CHECK(st->code == -1, "cancelled silent wait reports transport code -1");
    CHECK(st->terr.empty(), "cancel is silent — no transport error text, got: "
          + st->terr);
    std::cout << "[OK] cancel aborts silent wait (headers="
              << (send_headers ? "sent" : "none") << ") in <5s, silent\n";
    return true;
}

// Bodies over the 10 MB cap: the transfer aborts, the truncated prefix is
// returned with the real HTTP code, and *truncated is set.
static bool test_get_body_cap() {
    int fd = -1;
    int port = bind_ephemeral(fd);
    CHECK(port > 0, "ephemeral bind failed");
    const size_t total = 10 * 1024 * 1024 + 4096;  // just over the cap

    std::thread srv([&] {
        int c = accept(fd, nullptr, nullptr);
        if (c < 0) return;
        drain_request(c);
        std::string hdr = "HTTP/1.1 200 OK\r\nContent-Length: "
                        + std::to_string(total)
                        + "\r\nConnection: close\r\n\r\n";
        (void)!write(c, hdr.data(), hdr.size());
        std::string chunk(64 * 1024, 'x');
        size_t sent = 0;
        while (sent < total) {
            ssize_t w = send(c, chunk.data(), std::min(chunk.size(), total - sent), 0);
            if (w <= 0) break;  // client aborted at the cap — expected
            sent += static_cast<size_t>(w);
        }
        close(c);
    });

    haicode::HttpClient http;
    long code = 0;
    bool truncated = false;
    std::string body = http.get("http://127.0.0.1:" + std::to_string(port) + "/",
                                {}, 30, &code, &truncated);
    srv.join();
    close(fd);
    CHECK(code == 200, "over-cap body still reports the real HTTP code");
    CHECK(truncated, "over-cap body must set truncated");
    CHECK(body.size() == 10 * 1024 * 1024,
          "exactly the 10 MB cap returned, got " + std::to_string(body.size()));
    std::cout << "[OK] get() over-cap body: code 200 + truncated + capped prefix\n";
    return true;
}

// SSE spec: consecutive data: lines join with \n into ONE event payload —
// they are not separate events and the last must not drop the earlier ones.
static bool test_multiline_data_joins() {
    int fd = -1;
    int port = bind_ephemeral(fd);
    CHECK(port > 0, "ephemeral bind failed");
    std::thread t([&] {
        serve_once(fd,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Connection: close\r\n"
            "\r\n"
            "data: first\n"
            "data: second\n"
            "data: third\n"
            "\n");
    });

    haicode::HttpClient http;
    long code = 0;
    std::string terr;
    std::vector<std::string> datas;
    http.post_sse("http://127.0.0.1:" + std::to_string(port), {}, "{}",
                  [&](const haicode::SSEEvent& ev) {
                      datas.push_back(ev.data);
                      return true;
                  }, &code, &terr);
    t.join();
    close(fd);
    CHECK(code == 200, "stream should report 200");
    CHECK(datas.size() == 1, "three data: lines are ONE event, got " +
          std::to_string(datas.size()));
    CHECK(!datas.empty() && datas[0] == "first\nsecond\nthird",
          "data lines join with \\n, got: '" + (datas.empty() ? "" : datas[0]) + "'");
    std::cout << "[OK] multi-line data: joins into one \\n-joined event\n";
    return true;
}

// get() follows same-host redirects (path change) manually...
static bool test_get_same_host_redirect_followed() {
    int fd = -1;
    int port = bind_ephemeral(fd);
    CHECK(port > 0, "ephemeral bind failed");
    std::thread t([&] {
        serve_responses(fd, {
            "HTTP/1.1 302 Found\r\nLocation: /final\r\nContent-Length: 0\r\n"
            "Connection: close\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nFINAL",
        });
    });

    haicode::HttpClient http;
    long code = 0;
    std::string body = http.get("http://127.0.0.1:" + std::to_string(port) + "/start",
                                {}, 20, &code);
    t.join();
    close(fd);
    CHECK(code == 200, "same-host 302 must be followed to 200, got " +
          std::to_string(code));
    CHECK(body == "FINAL", "final hop body must be returned, got: " + body);
    std::cout << "[OK] get() follows same-host 302 (manual hop loop)\n";
    return true;
}

// ...but never a cross-host redirect: the 3xx is returned as-is instead of
// re-sending headers (credentials) to a host the server names.
static bool test_get_cross_host_redirect_not_followed() {
    int fd = -1;
    int port = bind_ephemeral(fd);
    CHECK(port > 0, "ephemeral bind failed");
    std::thread t([&] {
        serve_responses(fd, {
            "HTTP/1.1 302 Found\r\nLocation: http://localhost:1/steal\r\n"
            "Content-Length: 4\r\nConnection: close\r\n\r\nBAIT",
        });
    });

    haicode::HttpClient http;
    long code = 0;
    std::string body = http.get("http://127.0.0.1:" + std::to_string(port) + "/",
                                {}, 20, &code);
    t.join();
    close(fd);
    CHECK(code == 302, "cross-host 302 must be returned as-is, got " +
          std::to_string(code));
    CHECK(body == "BAIT", "redirect body itself is the response");
    std::cout << "[OK] get() returns cross-host 302 as-is (no header re-send)\n";
    return true;
}

// post_sse carries credentials: never follows redirects, same host or not.
static bool test_post_sse_never_follows_redirect() {
    int fd = -1;
    int port = bind_ephemeral(fd);
    CHECK(port > 0, "ephemeral bind failed");
    std::thread t([&] {
        serve_responses(fd, {
            "HTTP/1.1 302 Found\r\nLocation: /elsewhere\r\nContent-Length: 0\r\n"
            "Connection: close\r\n\r\n",
        });
    });

    haicode::HttpClient http;
    long code = 0;
    std::string terr;
    int events = 0;
    http.post_sse("http://127.0.0.1:" + std::to_string(port), {}, "{}",
                  [&](const haicode::SSEEvent&) { events++; return true; },
                  &code, &terr);
    t.join();
    close(fd);
    CHECK(code == 302, "post_sse must report the 302 itself, got " +
          std::to_string(code));
    CHECK(events == 0, "no events from a redirect response");
    std::cout << "[OK] post_sse never follows redirects\n";
    return true;
}

// Retry-After capture (Task 25): seconds-form header lands in the out-param
// and is appended to the HTTP-level error text; HTTP-dates are ignored.
static bool test_retry_after_captured() {
    int fd = -1;
    int port = bind_ephemeral(fd);
    CHECK(port > 0, "ephemeral bind failed");
    std::thread t([&]() {
        serve_once(fd, "HTTP/1.1 429 Too Many Requests\r\n"
                       "Retry-After: 7\r\n"
                       "Content-Type: application/json\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "{\"error\":{\"message\":\"rate limited\"}}");
    });

    haicode::HttpClient http;
    long code = 0;
    std::string terr, ra;
    http.post_sse("http://127.0.0.1:" + std::to_string(port),
                  {}, "{}", [](const haicode::SSEEvent&) { return true; },
                  &code, &terr, &ra);
    t.join();
    CHECK(code == 429, "429 reported");
    CHECK(ra == "7", "seconds-form Retry-After captured");
    CHECK(terr.find("[retry-after: 7]") != std::string::npos,
          "marker appended to the error text");

    // HTTP-date form: ignored, out-param stays empty.
    int fd2 = -1;
    int port2 = bind_ephemeral(fd2);
    CHECK(port2 > 0, "second ephemeral bind failed");
    std::thread t2([&]() {
        serve_once(fd2, "HTTP/1.1 503 Service Unavailable\r\n"
                        "Retry-After: Wed, 21 Oct 2026 07:28:00 GMT\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                        "{\"error\":{\"message\":\"down\"}}");
    });
    long code2 = 0;
    std::string terr2, ra2;
    http.post_sse("http://127.0.0.1:" + std::to_string(port2),
                  {}, "{}", [](const haicode::SSEEvent&) { return true; },
                  &code2, &terr2, &ra2);
    t2.join();
    CHECK(code2 == 503, "503 reported");
    CHECK(ra2.empty(), "HTTP-date Retry-After ignored");
    std::cout << "[OK] post_sse Retry-After: seconds captured, dates ignored\n";
    return true;
}

// Read one full HTTP request (headers + Content-Length body) into `out`.
// The provider's POST body can span several packets, unlike drain_request's
// single-recv assumption.
static void read_full_request(int c, std::string& out) {
    timeval tv{5, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[4096];
    while (true) {
        size_t hdr_end = out.find("\r\n\r\n");
        if (hdr_end != std::string::npos) {
            size_t cl = 0;
            size_t p = out.find("Content-Length:");
            if (p == std::string::npos) p = out.find("content-length:");
            if (p != std::string::npos && p < hdr_end)
                cl = std::strtoul(out.c_str() + p + 15, nullptr, 10);
            if (out.size() >= hdr_end + 4 + cl) return;
        }
        ssize_t n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0) return;
        out.append(buf, n);
    }
}

struct StallState {
    haicode::HttpClient http;
    std::atomic<bool> done{false};
    long code = 42;
    std::string terr;
    std::vector<std::string> datas;
};

// The stall limit is per-client and honored both ways: a short limit aborts
// a silent server (transport error, code -1), and a disabled limit (<= 0, as
// a stand-in for the 30-minute local-server limit) rides out a silence
// longer than the short limit's abort time, then delivers the events — the
// llama.cpp prefill case that the fixed 60 s limit killed.
static bool test_stall_timeout_configurable() {
    auto run = [](long stall_s, int silent_ms, std::shared_ptr<StallState> st,
                  double& elapsed_s) -> bool {
        int fd = -1;
        int port = bind_ephemeral(fd);
        if (port <= 0) return false;
        st->http.set_stall_timeout(stall_s);
        std::thread srv([fd, silent_ms, st] {
            int c = accept(fd, nullptr, nullptr);
            if (c < 0) return;
            drain_request(c);
            for (int waited = 0; waited < silent_ms && !st->done; waited += 50)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::string r = "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/event-stream\r\n"
                            "Connection: close\r\n\r\n"
                            "data: late\n\n";
            if (!st->done) (void)!write(c, r.data(), r.size());
            close(c);
        });
        auto begin = std::chrono::steady_clock::now();
        st->http.post_sse("http://127.0.0.1:" + std::to_string(port), {}, "{}",
                          [st](const haicode::SSEEvent& ev) {
                              st->datas.push_back(ev.data);
                              return true;
                          },
                          &st->code, &st->terr);
        st->done = true;
        elapsed_s = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - begin).count();
        srv.join();
        close(fd);
        return true;
    };

    CHECK(haicode::HttpClient().stall_timeout()
              == haicode::HttpClient::kDefaultStallTimeoutSec,
          "a fresh client starts at the 60 s default");

    // Short limit vs a 25 s silence: aborts well before the server speaks.
    auto shortst = std::make_shared<StallState>();
    double short_s = 0;
    CHECK(run(1, 25000, shortst, short_s), "short-limit server setup failed");
    std::cout << "  (1 s stall limit aborted after " << short_s << " s: "
              << shortst->terr << ")\n";
    CHECK(shortst->code == -1, "stalled stream reports transport code -1");
    CHECK(!shortst->terr.empty(), "stall abort carries a transport error");
    CHECK(shortst->datas.empty(), "no events from a stalled stream");
    CHECK(short_s < 20, "short stall limit must fire long before 25 s");

    // Disabled limit vs a silence longer than the short limit took to abort.
    auto longst = std::make_shared<StallState>();
    double long_s = 0;
    int silence_ms = static_cast<int>(short_s * 1000) + 2000;
    CHECK(run(0, silence_ms, longst, long_s), "no-limit server setup failed");
    CHECK(longst->code == 200, "silent-then-streaming server reports 200, got "
          + std::to_string(longst->code) + " / " + longst->terr);
    CHECK(longst->terr.empty(), "no transport error when the limit is off");
    CHECK(longst->datas.size() == 1 && longst->datas[0] == "late",
          "events after a long silence are delivered");
    std::cout << "[OK] stall limit: 1 s aborts a silent server; disabled rides out "
              << silence_ms / 1000.0 << " s of silence\n";
    return true;
}

// llama.cpp flavor end to end: the request asks for return_progress, and the
// prompt_progress chunks streamed during prefill (role-only delta, null
// content) produce no text — only the real content arrives, then a clean
// finish with usage.
static bool test_llamacpp_progress_chunks_ignored() {
    int fd = -1;
    int port = bind_ephemeral(fd);
    CHECK(port > 0, "ephemeral bind failed");

    auto chunk = [](const std::string& extra_delta, const std::string& tail) {
        return "data: {\"choices\":[{\"finish_reason\":null,\"index\":0,"
               "\"delta\":{" + extra_delta + "}}],"
               "\"object\":\"chat.completion.chunk\"" + tail + "}\n\n";
    };
    std::string progress_delta = "\"role\":\"assistant\",\"content\":null";
    std::string response =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Connection: close\r\n\r\n"
        + chunk(progress_delta, ",\"prompt_progress\":{\"total\":4096,"
                "\"cache\":0,\"processed\":0,\"time_ms\":0}")
        + chunk(progress_delta, ",\"prompt_progress\":{\"total\":4096,"
                "\"cache\":0,\"processed\":2048,\"time_ms\":6800}")
        + chunk(progress_delta, ",\"prompt_progress\":{\"total\":4096,"
                "\"cache\":0,\"processed\":4096,\"time_ms\":13600}")
        + chunk("\"content\":\"Hello\"", "")
        + "data: {\"choices\":[{\"finish_reason\":\"stop\",\"index\":0,"
          "\"delta\":{}}]}\n\n"
        + "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":4096,"
          "\"completion_tokens\":1}}\n\n"
        + "data: [DONE]\n\n";

    std::string request;
    std::thread srv([&] {
        int c = accept(fd, nullptr, nullptr);
        if (c < 0) return;
        read_full_request(c, request);
        (void)!write(c, response.data(), response.size());
        close(c);
    });

    auto provider = haicode::make_openai_compat_provider(
        "", "http://127.0.0.1:" + std::to_string(port) + "/v1", "llama",
        "llamacpp");
    haicode::LLMRequest req;
    req.model_id = "qwen3-32b";
    req.messages = {nlohmann::json{{"role", "user"}, {"content", "hi"}}};

    std::string text, err;
    bool finished = false;
    haicode::TokenUsage usage;
    haicode::FinishReason reason = haicode::FinishReason::ToolUse;
    haicode::StreamCallbacks cbs;
    cbs.on_text_delta = [&](const std::string&, const std::string& d) { text += d; };
    cbs.on_error = [&](const std::string& e) { err = e; };
    cbs.on_finish = [&](haicode::FinishReason fr, haicode::TokenUsage u,
                        std::vector<haicode::ToolCall>) {
        finished = true; reason = fr; usage = u;
    };
    provider->stream(req, cbs);
    srv.join();
    close(fd);

    size_t body_at = request.find("\r\n\r\n");
    auto body = nlohmann::json::parse(
        body_at == std::string::npos ? "" : request.substr(body_at + 4),
        nullptr, false);
    CHECK(!body.is_discarded(), "server should receive a JSON request body");
    CHECK(body.value("return_progress", false),
          "llama.cpp request carries return_progress:true");
    CHECK(err.empty(), "progress chunks must not raise an error, got: " + err);
    CHECK(finished, "stream finishes normally after progress chunks");
    CHECK(text == "Hello", "only real content becomes text, got: '" + text + "'");
    CHECK(reason == haicode::FinishReason::EndTurn, "finish_reason stop -> EndTurn");
    CHECK(usage.output == 1, "usage chunk still parsed after progress chunks");
    std::cout << "[OK] llama.cpp: return_progress sent, prompt_progress chunks skipped\n";
    return true;
}

int main() {
    std::cout.setf(std::ios::unitbuf);
    std::cout << "=== HttpClient post_sse failure reporting ===\n\n";
    bool ok = true;
    ok &= test_transport_failure_reports_minus_one();
    ok &= test_http_500_reports_code_and_no_events();
    ok &= test_clean_sse_stream_delivers_events();
    ok &= test_anthropic_provider_dead_endpoint();
    ok &= test_concurrent_post_sse_streams_isolated();
    ok &= test_callback_false_stops_stream_cleanly();
    ok &= test_cancel_aborts_silent_wait(false);
    ok &= test_cancel_aborts_silent_wait(true);
    ok &= test_get_body_cap();
    ok &= test_multiline_data_joins();
    ok &= test_get_same_host_redirect_followed();
    ok &= test_get_cross_host_redirect_not_followed();
    ok &= test_post_sse_never_follows_redirect();
    ok &= test_retry_after_captured();
    ok &= test_stall_timeout_configurable();
    ok &= test_llamacpp_progress_chunks_ignored();
    std::cout << (ok ? "\nAll http client tests passed!\n"
                     : "\nSome tests FAILED.\n");
    return ok ? 0 : 1;
}
