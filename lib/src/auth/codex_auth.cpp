#include <haicode/codex_auth.h>
#include <haicode/util.h>
#include <nlohmann/json.hpp>

#include <FindDirectory.h>
#include <Directory.h>
#include <Path.h>
#include <Url.h>

#include <openssl/sha.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace haicode {

namespace {

// Wire constants for the Codex CLI's published public OAuth client.
// Verified against codex-rs (login/src/server.rs) and independent clients
// (sac-cli, rs_ai, opencode); see agents.md "chatgpt provider" notes.
constexpr const char* kAuthorizeUrl = "https://auth.openai.com/oauth/authorize";
constexpr const char* kTokenUrl     = "https://auth.openai.com/oauth/token";
constexpr const char* kClientId     = "app_EMoamEEZ73f0CkXaXp7hrann";
constexpr const char* kScope        = "openid profile email offline_access";
constexpr const char* kOriginator   = "codex_cli_rs";
constexpr int   kCallbackPort       = 1455;
constexpr int   kFallbackPort       = 1457;
constexpr int64_t kLoginTimeoutMs   = 300 * 1000;  // 5 min
constexpr int64_t kRefreshSkewMs    = 300 * 1000;  // refresh 5 min early
constexpr uint64_t kDefaultExpiresIn = 3600;

std::mutex g_auth_mu;
std::string g_store_path_override;

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9') || c == '-' || c == '_'
                || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex_val(s[i + 1]), lo = hex_val(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// Random bytes from /dev/urandom; returns false on failure.
bool random_bytes(unsigned char* buf, size_t n) {
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    return got == n;
}

// --- loopback callback server --------------------------------------------

struct CallbackResult {
    bool ok = false;
    std::string code;
    std::string error;
};

// Parse "GET /auth/callback?code=..&state=.. HTTP/1.1" query params.
void parse_query(const std::string& url,
                 std::map<std::string, std::string>& out) {
    size_t q = url.find('?');
    if (q == std::string::npos) return;
    std::string query = url.substr(q + 1);
    size_t end = query.find(' ');
    if (end != std::string::npos) query = query.substr(0, end);
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        std::string pair = query.substr(pos,
            amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos)
            out[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
}

void send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
}

void respond(int fd, int status, const std::string& status_text,
             const std::string& body) {
    char header[128];
    snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\n"
             "Content-Length: %zu\r\nConnection: close\r\n\r\n",
             status, status_text, body.size());
    send_all(fd, std::string(header) + body);
}

// Bind 127.0.0.1 on kCallbackPort, falling back to kFallbackPort.
int bind_loopback(int* port_out) {
    for (int port : {kCallbackPort, kFallbackPort}) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0
                && listen(fd, 4) == 0) {
            *port_out = port;
            return fd;
        }
        close(fd);
    }
    return -1;
}

// Accept requests until a /auth/callback with matching state arrives or the
// overall deadline passes. Non-callback requests get a 404 and the loop goes
// on (browsers request /favicon.ico etc.).
CallbackResult wait_for_callback(int listen_fd, const std::string& expected_state) {
    CallbackResult result;
    int64_t deadline = util::now_ms() + kLoginTimeoutMs;

    while (util::now_ms() < deadline) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);
        int64_t remaining = deadline - util::now_ms();
        timeval tv{};
        tv.tv_sec = static_cast<long>(remaining / 1000);
        tv.tv_usec = static_cast<long>((remaining % 1000) * 1000);
        int ready = ::select(listen_fd + 1, &readfds, nullptr, nullptr, &tv);
        if (ready <= 0) {
            if (ready == 0) break;  // timed out
            result.error = "select() failed on callback server";
            return result;
        }

        int conn = ::accept(listen_fd, nullptr, nullptr);
        if (conn < 0) continue;

        // Read the request head (the request line carries the query).
        std::string req;
        char buf[2048];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
            timeval rtv{1, 0};
            setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
            ssize_t n = ::recv(conn, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.append(buf, static_cast<size_t>(n));
        }

        if (req.rfind("GET /auth/callback", 0) == 0) {
            std::map<std::string, std::string> params;
            parse_query(req, params);
            auto err_it = params.find("error");
            if (err_it != params.end()) {
                std::string desc = params.count("error_description")
                    ? params["error_description"] : "";
                respond(conn, 400, "Bad Request",
                    "<html><body><h2>Sign-in failed</h2><p>" + err_it->second
                    + ": " + desc + "</p></body></html>");
                close(conn);
                result.error = "OAuth error: " + err_it->second
                    + (desc.empty() ? "" : (" — " + desc));
                result.ok = false;
                return result;
            }
            auto state_it = params.find("state");
            if (state_it == params.end() || state_it->second != expected_state) {
                // Stale/mismatched tab — reject and keep waiting.
                respond(conn, 400, "Bad Request",
                    "<html><body><p>Stale sign-in attempt; please retry.</p></body></html>");
                close(conn);
                continue;
            }
            auto code_it = params.find("code");
            if (code_it == params.end()) {
                respond(conn, 400, "Bad Request",
                    "<html><body><p>Missing authorization code.</p></body></html>");
                close(conn);
                result.error = "callback missing code parameter";
                return result;
            }
            respond(conn, 200, "OK",
                "<html><body><h2>Sign-in successful</h2>"
                "<p>You can close this tab and return to HaiCode.</p></body></html>");
            close(conn);
            result.code = code_it->second;
            result.ok = true;
            return result;
        }

        respond(conn, 404, "Not Found", "<html><body></body></html>");
        close(conn);
    }

    result.error = "timed out waiting for sign-in to complete";
    return result;
}

// --- token endpoint -------------------------------------------------------

struct TokenResponse {
    std::string access_token;
    std::string refresh_token;
    std::string id_token;
    uint64_t expires_in = kDefaultExpiresIn;
};

bool parse_token_response(const std::string& body, TokenResponse& out,
                          std::string& err) {
    auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        err = "invalid response from token endpoint";
        return false;
    }
    if (j.contains("error")) {
        err = "token endpoint rejected request: "
            + j.value("error", std::string("unknown"));
        if (j.contains("error_description")
                && j["error_description"].is_string())
            err += " — " + j["error_description"].get<std::string>();
        return false;
    }
    if (!j.contains("access_token") || !j["access_token"].is_string()) {
        err = "token response missing access_token";
        return false;
    }
    out.access_token = j["access_token"].get<std::string>();
    if (j.contains("refresh_token") && j["refresh_token"].is_string())
        out.refresh_token = j["refresh_token"].get<std::string>();
    if (j.contains("id_token") && j["id_token"].is_string())
        out.id_token = j["id_token"].get<std::string>();
    if (j.contains("expires_in") && j["expires_in"].is_number_unsigned())
        out.expires_in = j["expires_in"].get<uint64_t>();
    else if (j.contains("expires_in") && j["expires_in"].is_number())
        out.expires_in = static_cast<uint64_t>(j["expires_in"].get<int64_t>());
    return true;
}

bool post_token(const std::vector<std::pair<std::string, std::string>>& form,
                TokenResponse& out, std::string& err) {
    std::string body;
    for (size_t i = 0; i < form.size(); ++i) {
        if (i) body += '&';
        body += url_encode(form[i].first) + '=' + url_encode(form[i].second);
    }
    HttpClient http;
    long code = 0;
    std::string resp = http.post_json(kTokenUrl,
        {{"Content-Type", "application/x-www-form-urlencoded"},
         {"Accept", "application/json"}},
        body, 30, &code);
    if (code == -1) {
        err = "network error contacting auth.openai.com";
        return false;
    }
    if (code != 200) {
        err = "token endpoint returned HTTP " + std::to_string(code);
        if (!resp.empty()) {
            TokenResponse tmp;
            std::string sub;
            if (!parse_token_response(resp, tmp, sub))
                err += ": " + sub;
        }
        return false;
    }
    return parse_token_response(resp, out, err);
}

} // namespace

// --- base64url helpers ----------------------------------------------------

std::string codex_b64url_encode(const std::string& raw) {
    std::string s = util::base64_encode(raw);
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '+') out += '-';
        else if (c == '/') out += '_';
        else if (c == '=') break;
        else out += c;
    }
    return out;
}

std::string codex_b64url_decode(const std::string& in) {
    // Map back to the standard alphabet and re-pad, then reuse the strict
    // decoder which stops at '='.
    std::string s;
    s.reserve(in.size() + 4);
    for (char c : in) {
        if (c == '-') s += '+';
        else if (c == '_') s += '/';
        else if (c == '=') break;
        else s += c;
    }
    while (s.size() % 4 != 0) s += '=';
    return util::base64_decode(s);
}

// --- JWT / authorize URL --------------------------------------------------

std::string codex_parse_account_id(const std::string& id_token_jwt) {
    // Claims only — no signature verification. The account id is used solely
    // as an outbound request header, same trust model as codex-rs.
    size_t dot1 = id_token_jwt.find('.');
    size_t dot2 = dot1 == std::string::npos
        ? std::string::npos : id_token_jwt.find('.', dot1 + 1);
    if (dot1 == std::string::npos || dot2 == std::string::npos) return "";
    std::string payload = codex_b64url_decode(
        id_token_jwt.substr(dot1 + 1, dot2 - dot1 - 1));
    auto j = nlohmann::json::parse(payload, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return "";
    if (j.contains("chatgpt_account_id") && j["chatgpt_account_id"].is_string())
        return j["chatgpt_account_id"].get<std::string>();
    constexpr const char* kNs = "https://api.openai.com/auth";
    if (j.contains(kNs) && j[kNs].is_object()
            && j[kNs].contains("chatgpt_account_id")
            && j[kNs]["chatgpt_account_id"].is_string())
        return j[kNs]["chatgpt_account_id"].get<std::string>();
    if (j.contains("organizations") && j["organizations"].is_array()
            && !j["organizations"].empty()
            && j["organizations"][0].is_object()
            && j["organizations"][0].contains("id")
            && j["organizations"][0]["id"].is_string())
        return j["organizations"][0]["id"].get<std::string>();
    return "";
}

std::string codex_build_authorize_url(const std::string& redirect_uri,
                                      const std::string& code_challenge,
                                      const std::string& state) {
    std::vector<std::pair<std::string, std::string>> params = {
        {"response_type", "code"},
        {"client_id", kClientId},
        {"redirect_uri", redirect_uri},
        {"scope", kScope},
        {"code_challenge", code_challenge},
        {"code_challenge_method", "S256"},
        {"id_token_add_organizations", "true"},
        {"codex_cli_simplified_flow", "true"},
        {"state", state},
        {"originator", kOriginator},
    };
    std::string url = std::string(kAuthorizeUrl) + "?";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) url += '&';
        url += url_encode(params[i].first) + '=' + url_encode(params[i].second);
    }
    return url;
}

// --- store ----------------------------------------------------------------

std::string codex_auth_store_path() {
    if (!g_store_path_override.empty()) return g_store_path_override;
    BPath settings;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &settings) != B_OK)
        return "";
    BPath p(settings);
    p.Append("haicode/openai-auth.json");
    return p.Path();
}

void codex_auth_set_store_path_for_testing(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_auth_mu);
    g_store_path_override = path;
}

bool codex_auth_save(const CodexAuth& auth, std::string& err) {
    std::string path = codex_auth_store_path();
    if (path.empty()) {
        err = "cannot determine settings directory";
        return false;
    }
    std::string dir = path.substr(0, path.rfind('/'));
    create_directory(dir.c_str(), 0755);
    nlohmann::json j = {
        {"type", "chatgpt-codex"},
        {"access", auth.access},
        {"refresh", auth.refresh},
        {"expires_at_ms", auth.expires_at_ms},
        {"account_id", auth.account_id},
    };
    err = util::atomic_write_file(path, j.dump(2));
    return err.empty();
}

bool codex_auth_load(CodexAuth& out) {
    std::string path = codex_auth_store_path();
    if (path.empty()) return false;
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::string body((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    if (!j.contains("access") || !j["access"].is_string()) return false;
    out = CodexAuth{};
    out.access = j["access"].get<std::string>();
    if (j.contains("refresh") && j["refresh"].is_string())
        out.refresh = j["refresh"].get<std::string>();
    if (j.contains("account_id") && j["account_id"].is_string())
        out.account_id = j["account_id"].get<std::string>();
    if (j.contains("expires_at_ms") && j["expires_at_ms"].is_number())
        out.expires_at_ms = j["expires_at_ms"].get<int64_t>();
    return true;
}

// --- login ----------------------------------------------------------------

bool codex_run_browser_login(CodexAuth& out, std::string& err,
                             std::string* auth_url_out) {
    unsigned char verifier_seed[64], state_seed[32];
    if (!random_bytes(verifier_seed, sizeof(verifier_seed))
            || !random_bytes(state_seed, sizeof(state_seed))) {
        err = "cannot read /dev/urandom";
        return false;
    }
    std::string verifier = codex_b64url_encode(
        std::string(reinterpret_cast<char*>(verifier_seed), sizeof(verifier_seed)));
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(verifier.data()),
           verifier.size(), digest);
    std::string challenge = codex_b64url_encode(
        std::string(reinterpret_cast<char*>(digest), sizeof(digest)));
    std::string state = codex_b64url_encode(
        std::string(reinterpret_cast<char*>(state_seed), sizeof(state_seed)));

    int port = 0;
    int listen_fd = bind_loopback(&port);
    if (listen_fd < 0) {
        err = "cannot bind loopback callback server on ports 1455/1457";
        return false;
    }

    std::string redirect_uri = "http://localhost:" + std::to_string(port)
        + "/auth/callback";
    std::string auth_url = codex_build_authorize_url(redirect_uri, challenge, state);
    if (auth_url_out) *auth_url_out = auth_url;

    // Non-fatal: the GUI surfaces the URL for manual open when this fails.
    BUrl url(auth_url.c_str(), true);
    if (url.IsValid())
        url.OpenWithPreferredApplication(true);

    CallbackResult cb = wait_for_callback(listen_fd, state);
    close(listen_fd);
    if (!cb.ok) {
        err = cb.error.empty() ? "login did not complete" : cb.error;
        return false;
    }

    TokenResponse tokens;
    if (!post_token({
            {"grant_type", "authorization_code"},
            {"code", cb.code},
            {"redirect_uri", redirect_uri},
            {"client_id", kClientId},
            {"code_verifier", verifier},
        }, tokens, err)) {
        return false;
    }

    CodexAuth auth;
    auth.access = tokens.access_token;
    auth.refresh = tokens.refresh_token;
    auth.expires_at_ms = util::now_ms()
        + static_cast<int64_t>(tokens.expires_in) * 1000;
    if (!tokens.id_token.empty())
        auth.account_id = codex_parse_account_id(tokens.id_token);

    {
        std::lock_guard<std::mutex> lock(g_auth_mu);
        if (!codex_auth_save(auth, err)) return false;
    }
    out = auth;
    return true;
}

// --- refresh --------------------------------------------------------------

namespace {

// Caller holds g_auth_mu. Refreshes the stored token and saves the store.
bool refresh_locked(CodexAuth& auth, std::string& err) {
    if (auth.refresh.empty()) {
        err = "not signed in (no refresh token) — sign in with ChatGPT again";
        return false;
    }
    TokenResponse tokens;
    if (!post_token({
            {"grant_type", "refresh_token"},
            {"client_id", kClientId},
            {"refresh_token", auth.refresh},
        }, tokens, err)) {
        return false;
    }
    auth.access = tokens.access_token;
    // Refresh-token rotation: keep the old one when the endpoint omits it.
    if (!tokens.refresh_token.empty())
        auth.refresh = tokens.refresh_token;
    auth.expires_at_ms = util::now_ms()
        + static_cast<int64_t>(tokens.expires_in) * 1000;
    if (!codex_auth_save(auth, err)) return false;
    return true;
}

} // namespace

bool codex_auth_get_access(std::string& token, std::string& account_id,
                           std::string& err) {
    std::lock_guard<std::mutex> lock(g_auth_mu);
    CodexAuth auth;
    if (!codex_auth_load(auth)) {
        err = "not signed in — use Sign in with ChatGPT in Settings";
        return false;
    }
    if (auth.expires_at_ms - kRefreshSkewMs <= util::now_ms()) {
        if (!refresh_locked(auth, err)) return false;
    }
    token = auth.access;
    account_id = auth.account_id;
    return true;
}

bool codex_auth_force_refresh(std::string& err) {
    std::lock_guard<std::mutex> lock(g_auth_mu);
    CodexAuth auth;
    if (!codex_auth_load(auth)) {
        err = "not signed in";
        return false;
    }
    return refresh_locked(auth, err);
}

bool codex_auth_signed_in() {
    std::lock_guard<std::mutex> lock(g_auth_mu);
    CodexAuth auth;
    return codex_auth_load(auth) && !auth.access.empty();
}

void codex_auth_clear() {
    std::lock_guard<std::mutex> lock(g_auth_mu);
    std::string path = codex_auth_store_path();
    if (!path.empty()) ::remove(path.c_str());
}

} // namespace haicode
