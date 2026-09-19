// HttpClient::post_sse failure reporting + provider-level empty-success fix
// (review #8). A one-shot local TCP server serves canned responses; a closed
// port stands in for an unreachable endpoint.
#include <haicode/util.h>
#include <haicode/haicode.h>
#include <haicode/provider.h>
#include <iostream>
#include <string>
#include <vector>
#include <cstring>
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
    char buf[4096];
    while (recv(c, buf, sizeof(buf), 0) > 0) {
        // drain until EOF or timeout — curl sends one small request
        if (strstr(buf, "Content-Length") != nullptr
                && strstr(buf, "\r\n\r\n") != nullptr)
            break;
    }
    (void)!write(c, response.data(), response.size());
    close(c);
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

int main() {
    std::cout << "=== HttpClient post_sse failure reporting ===\n\n";
    bool ok = true;
    ok &= test_transport_failure_reports_minus_one();
    ok &= test_http_500_reports_code_and_no_events();
    ok &= test_clean_sse_stream_delivers_events();
    ok &= test_anthropic_provider_dead_endpoint();
    std::cout << (ok ? "\nAll http client tests passed!\n"
                     : "\nSome tests FAILED.\n");
    return ok ? 0 : 1;
}
