#pragma once
#include <string>
#include <cstdint>

namespace haicode {

// "Sign in with ChatGPT" (Codex-style OAuth) credentials.
struct CodexAuth {
    std::string access;
    std::string refresh;
    std::string account_id;
    int64_t expires_at_ms = 0;
};

// Run the interactive browser login: binds a loopback callback server on
// 127.0.0.1:1455 (fallback 1457), opens the system browser at
// auth.openai.com, waits (up to ~5 min) for the OAuth redirect, exchanges
// the authorization code with PKCE, parses the id_token for the ChatGPT
// account id, and persists the token store. `auth_url_out` (optional)
// receives the authorize URL so callers can surface a manual-open link
// when no browser launched.
bool codex_run_browser_login(CodexAuth& out, std::string& err,
                             std::string* auth_url_out = nullptr);

// Return a live access token (+ ChatGPT account id), transparently
// refreshing when within the 5-minute expiry skew. Thread-safe
// (single-flight under a mutex).
bool codex_auth_get_access(std::string& token, std::string& account_id,
                           std::string& err);

// Refresh now, ignoring skew. Used by the 401-retry path. Thread-safe.
bool codex_auth_force_refresh(std::string& err);

bool codex_auth_signed_in();

// Sign out: deletes the token store (no server-side revocation).
void codex_auth_clear();

// --- internals exposed for unit tests ------------------------------------
std::string codex_parse_account_id(const std::string& id_token_jwt);
std::string codex_b64url_encode(const std::string& raw);
std::string codex_b64url_decode(const std::string& in);
std::string codex_build_authorize_url(const std::string& redirect_uri,
                                      const std::string& code_challenge,
                                      const std::string& state);
std::string codex_auth_store_path();
void codex_auth_set_store_path_for_testing(const std::string& path);
bool codex_auth_save(const CodexAuth& auth, std::string& err);
bool codex_auth_load(CodexAuth& out);

} // namespace haicode
