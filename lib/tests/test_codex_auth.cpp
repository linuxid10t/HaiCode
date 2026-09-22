#include <haicode/codex_auth.h>
#include <haicode/util.h>
#include <iostream>
#include <cassert>
#include <cstdio>
#include <string>
#include <unistd.h>

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::cerr << "[FAIL] " << msg << "\n"; return false; } } while(0)

using namespace haicode;

// Build a JWT-shaped string: header.payload.signature, base64url-encoded JSON.
static std::string make_jwt(const std::string& claims_json) {
    return codex_b64url_encode(R"({"alg":"RS256","typ":"JWT"})")
         + "." + codex_b64url_encode(claims_json) + "." + "sig";
}

static bool test_b64url_roundtrip() {
    // All byte values: verifies +/ → -_ mapping and = stripping both ways.
    std::string raw(256, '\0');
    for (int i = 0; i < 256; ++i) raw[i] = static_cast<char>(i);
    std::string enc = codex_b64url_encode(raw);
    CHECK(enc.find('+') == std::string::npos, "encoded must not contain '+'");
    CHECK(enc.find('/') == std::string::npos, "encoded must not contain '/'");
    CHECK(enc.find('=') == std::string::npos, "encoded must not contain '='");
    std::string dec = codex_b64url_decode(enc);
    CHECK(dec == raw, "256-byte roundtrip must be byte-identical");

    // Empty and odd-length inputs.
    CHECK(codex_b64url_encode("").empty(), "empty encodes to empty");
    CHECK(codex_b64url_decode("").empty(), "empty decodes to empty");
    std::string one = codex_b64url_encode("a");
    std::string odd = codex_b64url_decode(one);
    CHECK(odd == "a", "single-byte roundtrip");
    std::cout << "[OK] base64url roundtrip\n";
    return true;
}

static bool test_parse_account_id_top_level() {
    std::string jwt = make_jwt(R"({"email":"a@b.c","chatgpt_account_id":"acc-top"})");
    CHECK(codex_parse_account_id(jwt) == "acc-top",
          "top-level chatgpt_account_id must win");
    std::cout << "[OK] account id: top-level claim\n";
    return true;
}

static bool test_parse_account_id_namespaced() {
    std::string jwt = make_jwt(
        R"({"https://api.openai.com/auth":{"chatgpt_account_id":"acc-ns"}})");
    CHECK(codex_parse_account_id(jwt) == "acc-ns",
          "namespaced chatgpt_account_id must be found");
    std::cout << "[OK] account id: namespaced claim\n";
    return true;
}

static bool test_parse_account_id_organizations() {
    std::string jwt = make_jwt(
        R"({"organizations":[{"id":"org-1"},{"id":"org-2"}]})");
    CHECK(codex_parse_account_id(jwt) == "org-1",
          "first organization id is the fallback");
    // Top-level outranks organizations.
    std::string both = make_jwt(
        R"({"chatgpt_account_id":"acc","organizations":[{"id":"org-1"}]})");
    CHECK(codex_parse_account_id(both) == "acc",
          "chatgpt_account_id outranks organizations");
    std::cout << "[OK] account id: organizations fallback + precedence\n";
    return true;
}

static bool test_parse_account_id_invalid() {
    CHECK(codex_parse_account_id("").empty(), "empty jwt → empty");
    CHECK(codex_parse_account_id("not-a-jwt").empty(), "no dots → empty");
    CHECK(codex_parse_account_id("a.b").empty(), "one dot → empty");
    CHECK(codex_parse_account_id("a.###.c").empty(), "undecodable payload → empty");
    CHECK(codex_parse_account_id(make_jwt(R"({"email":"a@b.c"})")).empty(),
          "no account claims → empty");
    std::cout << "[OK] account id: invalid inputs\n";
    return true;
}

static bool test_authorize_url() {
    std::string url = codex_build_authorize_url(
        "http://localhost:1455/auth/callback", "CHALLENGE", "STATE");
    CHECK(url.rfind("https://auth.openai.com/oauth/authorize?", 0) == 0,
          "must hit the authorize endpoint");
    CHECK(url.find("client_id=app_EMoamEEZ73f0CkXaXp7hrann") != std::string::npos,
          "client id present");
    CHECK(url.find("response_type=code") != std::string::npos,
          "response_type present");
    CHECK(url.find("code_challenge_method=S256") != std::string::npos,
          "S256 method present");
    CHECK(url.find("code_challenge=CHALLENGE") != std::string::npos,
          "challenge present");
    CHECK(url.find("state=STATE") != std::string::npos, "state present");
    CHECK(url.find("scope=openid%20profile%20email%20offline_access")
              != std::string::npos, "scope present and url-encoded");
    CHECK(url.find("redirect_uri=http%3A%2F%2Flocalhost%3A1455%2Fauth%2Fcallback")
              != std::string::npos, "redirect uri url-encoded");
    CHECK(url.find("originator=codex_cli_rs") != std::string::npos,
          "originator present");
    CHECK(url.find("id_token_add_organizations=true") != std::string::npos,
          "org claim flag present");
    CHECK(url.find("codex_cli_simplified_flow=true") != std::string::npos,
          "simplified flow flag present");
    std::cout << "[OK] authorize URL construction\n";
    return true;
}

static bool test_store_roundtrip() {
    std::string tmpl = "/tmp/haicode-test-auth-XXXXXX";
    std::string path;
    int fd = util::make_secure_temp(tmpl, path);
    CHECK(fd >= 0, "temp file created");
    ::close(fd);
    ::remove(path.c_str());  // save should recreate it
    codex_auth_set_store_path_for_testing(path);

    CodexAuth auth;
    auth.access = "acc-tok";
    auth.refresh = "ref-tok";
    auth.account_id = "acc-123";
    auth.expires_at_ms = 123456789;

    std::string err;
    CHECK(codex_auth_save(auth, err), ("save failed: " + err).c_str());

    CodexAuth loaded;
    CHECK(codex_auth_load(loaded), "load failed after save");
    CHECK(loaded.access == "acc-tok", "access roundtrip");
    CHECK(loaded.refresh == "ref-tok", "refresh roundtrip");
    CHECK(loaded.account_id == "acc-123", "account id roundtrip");
    CHECK(loaded.expires_at_ms == 123456789, "expiry roundtrip");

    // Overwrite: second save replaces rather than merges.
    CodexAuth auth2;
    auth2.access = "acc-2";
    CHECK(codex_auth_save(auth2, err), "second save ok");
    CodexAuth loaded2;
    CHECK(codex_auth_load(loaded2), "second load ok");
    CHECK(loaded2.access == "acc-2", "overwrite wins");
    CHECK(loaded2.refresh.empty(), "fields absent in new save are empty");

    ::remove(path.c_str());
    codex_auth_set_store_path_for_testing("");
    std::cout << "[OK] store save/load roundtrip\n";
    return true;
}

static bool test_signed_in_follows_store() {
    std::string tmpl = "/tmp/haicode-test-signed-XXXXXX";
    std::string path;
    int fd = util::make_secure_temp(tmpl, path);
    CHECK(fd >= 0, "temp file created");
    ::close(fd);
    ::remove(path.c_str());
    codex_auth_set_store_path_for_testing(path);

    CHECK(!codex_auth_signed_in(), "absent store → not signed in");

    CodexAuth auth;
    auth.access = "tok";
    auth.refresh = "ref";
    auth.expires_at_ms = util::now_ms() + 3600 * 1000;
    std::string err;
    CHECK(codex_auth_save(auth, err), "save ok");
    CHECK(codex_auth_signed_in(), "present store → signed in");

    codex_auth_clear();
    CHECK(!codex_auth_signed_in(), "clear removes store");

    codex_auth_set_store_path_for_testing("");
    std::cout << "[OK] signed_in / clear follow the store\n";
    return true;
}

int main() {
    bool ok = true;
    ok &= test_b64url_roundtrip();
    ok &= test_parse_account_id_top_level();
    ok &= test_parse_account_id_namespaced();
    ok &= test_parse_account_id_organizations();
    ok &= test_parse_account_id_invalid();
    ok &= test_authorize_url();
    ok &= test_store_roundtrip();
    ok &= test_signed_in_follows_store();
    std::cout << (ok ? "ALL PASS\n" : "FAILURES\n");
    return ok ? 0 : 1;
}
