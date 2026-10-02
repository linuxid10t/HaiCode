#pragma once
#include <string>

namespace haicode {

enum class ProviderErrorKind { Transient, Overflow, Fatal };

// Classifies a provider/stream error message for retry policy.
//   Transient — overload/timeout/connection/5xx/rate limits (incl. 429 and
//   Anthropic's 529): retried with backoff when nothing streamed.
//   Overflow — provider-specific context-length markers: retried once via
//   compaction. Deliberately narrow; bare "context"/"exceeds" matches (e.g.
//   "exceeds your quota") must NOT trigger compaction.
//   Fatal — anything else.
ProviderErrorKind classify_provider_error(const std::string& error);

// Seconds from a "[retry-after: N]" suffix that providers append to an
// error when the server sent a Retry-After header (HTTP-level failures
// only — SSE-embedded error events carry no headers). 0 when absent.
double parse_retry_after_seconds(const std::string& error);

// Exponential backoff for transient retries, milliseconds by 0-based
// attempt: 500 / 2000 / 8000, capped at 10000. The engine takes
// max(backoff, retry_after) — also capped — before re-sending.
int retry_backoff_ms(int attempt);

} // namespace haicode
