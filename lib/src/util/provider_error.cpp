#include <haicode/provider_error.h>
#include <algorithm>
#include <cctype>

namespace haicode {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

} // namespace

ProviderErrorKind classify_provider_error(const std::string& error) {
    const std::string lo = lower(error);

    // Overflow markers are provider-specific and deliberately narrow: bare
    // "context"/"exceeds" substrings match unrelated errors ("exceeds your
    // quota") and would trigger pointless compaction.
    static const char* kOverflow[] = {
        "context_length_exceeded",       // OpenAI / many compat servers
        "request_too_large",             // generic 413 body
        "prompt is too long",            // Anthropic
        "input length and `max_tokens` exceed",  // Anthropic (backticked)
        "input length and max_tokens exceed",    // same, unbackticked
        "maximum context length",        // OpenAI/Azure phrasing
    };
    for (const char* m : kOverflow)
        if (lo.find(m) != std::string::npos) return ProviderErrorKind::Overflow;

    static const char* kTransient[] = {
        "overloaded", "timeout", "timed out", "connection",
        "internal server", "service unavailable", "bad gateway",
        "gateway timeout", "temporarily unavailable",
        "429", "529",                      // rate limits (529: Anthropic)
        "rate limit", "too many requests",
    };
    for (const char* m : kTransient)
        if (lo.find(m) != std::string::npos) return ProviderErrorKind::Transient;

    return ProviderErrorKind::Fatal;
}

double parse_retry_after_seconds(const std::string& error) {
    static const std::string kMarker = "[retry-after: ";
    size_t p = error.rfind(kMarker);
    if (p == std::string::npos) return 0.0;
    size_t end = error.find(']', p);
    if (end == std::string::npos) return 0.0;
    try {
        return std::stod(error.substr(p + kMarker.size(), end - p - kMarker.size()));
    } catch (...) {
        return 0.0;
    }
}

int retry_backoff_ms(int attempt) {
    // 0.5s / 2s / 8s for attempts 0/1/2, capped at 10s.
    if (attempt <= 0) return 500;
    if (attempt == 1) return 2000;
    if (attempt == 2) return 8000;
    return 10000;
}

} // namespace haicode
