// Task 19: WebExtractTool bounds. max_chars is model-supplied and must be
// clamped to the shared 100 KB output budget; a body over the HttpClient
// download cap (10 MB) must surface truncated=true while still reporting a
// successful fetch. Served by one-shot local HTTP servers (pattern from
// test_http_client.cpp).
#include <haicode/haicode.h>
#include <haicode/tool.h>
#include <iostream>
#include <algorithm>
#include <csignal>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

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

// Drain request headers (a GET is one small packet).
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

// Serve one GET with an HTML body (Content-Length declared, chunked writes;
// a client aborting at its 10 MB cap just ends the send loop early).
static void serve_html_once(int listen_fd, const std::string& html) {
    int c = accept(listen_fd, nullptr, nullptr);
    if (c < 0) return;
    drain_request(c);
    std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                    "Content-Length: " + std::to_string(html.size())
                    + "\r\nConnection: close\r\n\r\n";
    (void)!write(c, hdr.data(), hdr.size());
    size_t sent = 0;
    while (sent < html.size()) {
        ssize_t w = send(c, html.data() + sent,
                         std::min<size_t>(64 * 1024, html.size() - sent), 0);
        if (w <= 0) break;
        sent += static_cast<size_t>(w);
    }
    close(c);
}

// HTML whose extracted text is at least min_text_bytes: repeated <p> blocks
// of plain ASCII (tags become newlines, so extracted size ≈ x count).
static std::string make_html(size_t min_text_bytes) {
    std::string html = "<html><body>";
    const std::string para(512, 'x');
    size_t text = 0;
    while (text < min_text_bytes + 1024) {
        html += "<p>" + para + "</p>";
        text += para.size();
    }
    html += "</body></html>";
    return html;
}

// Registry + rules-free gate: after Task 18 the web-tool exemption applies
// when no rule matched, so execution reaches the tool without prompting.
struct WebFixture {
    haicode::ToolRegistry reg;
    haicode::PermissionGate gate;
    haicode::ToolContext ctx;
    WebFixture() {
        haicode::register_builtin_tools(reg);
        ctx.working_dir = "/tmp";
    }
};

static bool web_extract_small_fetch_unaffected() {
    int fd = -1;
    int port = bind_ephemeral(fd);
    CHECK(port > 0, "ephemeral bind failed");
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/page";
    std::thread srv([&] {
        serve_html_once(fd, "<html><body><article><p>"
                            "Hello extractable content"
                            "</p></article></body></html>");
    });

    WebFixture fx;
    auto r = fx.reg.execute("web_extract", {{"url", url}}, fx.ctx, fx.gate);
    srv.join();
    close(fd);

    CHECK(r.success, "small fetch should succeed: " + r.error);
    auto out = nlohmann::json::parse(r.output, nullptr, false);
    CHECK(out.is_object(), "output must be a JSON object");
    CHECK(!out.value("truncated", true), "small fetch must not be truncated");
    CHECK(out.value("text", "").find("Hello extractable content")
              != std::string::npos,
          "text should contain the page content");
    std::cout << "[OK] web_extract small fetch unaffected\n";
    return true;
}

static bool web_extract_in_range_max_chars_respected() {
    int fd = -1;
    int port = bind_ephemeral(fd);
    CHECK(port > 0, "ephemeral bind failed");
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/big";
    std::thread srv([&] { serve_html_once(fd, make_html(2048)); });

    WebFixture fx;
    auto r = fx.reg.execute("web_extract",
                            {{"url", url}, {"max_chars", 100}}, fx.ctx, fx.gate);
    srv.join();
    close(fd);

    CHECK(r.success, "in-range fetch should succeed: " + r.error);
    auto out = nlohmann::json::parse(r.output, nullptr, false);
    CHECK(out.is_object(), "output must be a JSON object");
    CHECK(out.value("text", "").size() == 100,
          "in-range max_chars must be applied exactly, got "
          + std::to_string(out.value("text", "").size()));
    CHECK(out.value("truncated", false), "cut at max_chars sets truncated");
    std::cout << "[OK] web_extract in-range max_chars respected\n";
    return true;
}

// max_chars landing inside a multibyte character: the cut must back off to a
// UTF-8 boundary. A plain resize left an orphaned lead byte and the JSON dump
// threw type_error.316, failing the whole fetch.
static bool web_extract_max_chars_keeps_utf8_boundary() {
    int fd = -1;
    int port = bind_ephemeral(fd);
    CHECK(port > 0, "ephemeral bind failed");
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/vi";
    std::string para = "ab";
    for (int i = 0; i < 200; ++i) para += "\xE1\xBB\x87";  // U+1EC7 (ệ)
    std::thread srv([&] {
        serve_html_once(fd, "<html><body><p>" + para + "</p></body></html>");
    });

    WebFixture fx;
    auto r = fx.reg.execute("web_extract",
                            {{"url", url}, {"max_chars", 100}}, fx.ctx, fx.gate);
    srv.join();
    close(fd);

    CHECK(r.success, "mid-character max_chars must still succeed: " + r.error);
    auto out = nlohmann::json::parse(r.output, nullptr, false);
    CHECK(out.is_object(), "output must be a JSON object");
    std::string text = out.value("text", "");
    CHECK(!text.empty() && text.size() <= 100,
          "text must be cut at or below max_chars, got "
          + std::to_string(text.size()));
    CHECK(haicode::util::sanitize_utf8(text) == text, "text must be valid UTF-8");
    CHECK(out.value("truncated", false), "cut at max_chars sets truncated");
    std::cout << "[OK] web_extract max_chars keeps a UTF-8 boundary\n";
    return true;
}

// web_search API snippets (Exa highlights, Z.ai content) are capped at 400
// bytes. A Vietnamese snippet whose byte 400 falls inside a character used to
// keep the orphaned lead byte; dumping the result array then threw
// "type_error.316 invalid UTF-8 byte at index 400: 0xE2" (repro from a Z.ai
// search for Vietnamese sources).
static bool web_search_snippet_cap_keeps_utf8_boundary() {
    // 399 ASCII bytes, then "…" (E2 80 A6) straddling byte 400.
    std::string content(399, 'x');
    for (int i = 0; i < 50; ++i) content += "\xE2\x80\xA6";
    std::string highlight = "a";
    for (int i = 0; i < 300; ++i) highlight += "\xE1\xBB\x87";  // ệ

    nlohmann::json zai = {{"search_result", {
        {{"title", "t"}, {"link", "https://example.vn"}, {"content", content}}}}};
    nlohmann::json exa = {{"results", {
        {{"title", "t"}, {"url", "https://example.vn"},
         {"highlights", {highlight}}}}}};

    for (auto& [engine, resp] : {std::pair{std::string("zai"), zai},
                                 std::pair{std::string("exa"), exa}}) {
        nlohmann::json arr;
        std::string dumped;
        try {
            arr = haicode::parse_search_api_results(engine, resp, 5);
            dumped = arr.dump(2);
        } catch (const std::exception& e) {
            CHECK(false, engine + " snippet dump threw: " + e.what());
        }
        CHECK(arr.size() == 1, engine + " should yield one result");
        std::string snip = arr[0].value("snippet", "");
        const std::string ell = "\xE2\x80\xA6";
        CHECK(snip.size() > ell.size()
                  && snip.compare(snip.size() - ell.size(), ell.size(), ell) == 0,
              engine + " capped snippet ends with the ellipsis marker");
        CHECK(snip.size() - ell.size() <= 400,
              engine + " snippet body must stay within 400 bytes");
        CHECK(haicode::util::sanitize_utf8(snip) == snip,
              engine + " snippet must be valid UTF-8");
    }
    std::cout << "[OK] web_search snippet cap keeps a UTF-8 boundary\n";
    return true;
}

// Over-cap request: max_chars is clamped to the shared 100 KB budget. The
// serialized JSON then crosses MAX_OUTPUT itself, so the observable is the
// capped output plus the marker (the JSON is deliberately cut there).
static bool web_extract_over_cap_max_chars_clamped() {
    int fd = -1;
    int port = bind_ephemeral(fd);
    CHECK(port > 0, "ephemeral bind failed");
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/cap";
    std::thread srv([&] { serve_html_once(fd, make_html(150 * 1024)); });

    WebFixture fx;
    auto r = fx.reg.execute("web_extract",
                            {{"url", url}, {"max_chars", 100000000}},
                            fx.ctx, fx.gate);
    srv.join();
    close(fd);

    CHECK(r.success, "over-cap fetch should succeed: " + r.error);
    static const size_t kBudget = 100 * 1024;
    const std::string marker = "\n[output truncated]";
    CHECK(r.output.size() == kBudget + marker.size(),
          "over-cap max_chars must clamp the result to the 100 KB budget, got "
          + std::to_string(r.output.size()));
    CHECK(r.output.size() >= marker.size()
              && r.output.compare(r.output.size() - marker.size(),
                                  marker.size(), marker) == 0,
          "output must end with the truncation marker");
    std::cout << "[OK] web_extract over-cap max_chars clamped to 100 KB\n";
    return true;
}

// Body over the HttpClient 10 MB download cap: the transfer aborts at the
// cap with the real HTTP code, the tool still succeeds, and the output's
// truncated flag is true (text itself cut at the 8000-char default).
static bool web_extract_download_cap_flags_truncated() {
    int fd = -1;
    int port = bind_ephemeral(fd);
    CHECK(port > 0, "ephemeral bind failed");
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/huge";
    std::thread srv([&] { serve_html_once(fd, make_html(10 * 1024 * 1024 + 8192)); });

    WebFixture fx;
    auto r = fx.reg.execute("web_extract", {{"url", url}}, fx.ctx, fx.gate);
    srv.join();
    close(fd);

    CHECK(r.success, "over-download-cap fetch still reports success: " + r.error);
    auto out = nlohmann::json::parse(r.output, nullptr, false);
    CHECK(out.is_object(), "output must be a JSON object");
    CHECK(out.value("truncated", false),
          "download-cap abort must surface truncated=true");
    CHECK(out.value("text", "").size() == 8000,
          "text cut at the 8000-char default, got "
          + std::to_string(out.value("text", "").size()));
    std::cout << "[OK] web_extract download-cap body flags truncated\n";
    return true;
}

int main() {
    std::cout.setf(std::ios::unitbuf);
    std::cout << "=== Web tool bounds tests ===\n";
    // The in-process server keeps send()ing past the client's download-cap
    // abort; the resulting EPIPE would kill the whole test process.
    signal(SIGPIPE, SIG_IGN);
    haicode::set_offline_mode(false);

    bool ok = true;
    ok &= web_extract_small_fetch_unaffected();
    ok &= web_extract_in_range_max_chars_respected();
    ok &= web_extract_over_cap_max_chars_clamped();
    ok &= web_extract_download_cap_flags_truncated();
    ok &= web_extract_max_chars_keeps_utf8_boundary();
    ok &= web_search_snippet_cap_keeps_utf8_boundary();

    std::cout << (ok ? "\nAll web tool tests passed!\n"
                     : "\nSome tests FAILED.\n");
    return ok ? 0 : 1;
}
