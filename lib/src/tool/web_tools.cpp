#include <haicode/tool.h>
#include <haicode/util.h>
#include <haicode/config.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace haicode {

// 100 KB output cap, matching the other tools.
static const size_t MAX_OUTPUT = 100 * 1024;

// Browser-like UA — many sites gate non-browser UAs.
static const char* kBrowserUA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0 Safari/537.36";

// ---------------------------------------------------------------------------
// Small string helpers
// ---------------------------------------------------------------------------

static std::string url_encode(const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[(c >> 4) & 0xF];
            out += kHex[c & 0xF];
        }
    }
    return out;
}

static std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i+1])
            && std::isxdigit((unsigned char)s[i+2])) {
            auto hex = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return 0;
            };
            int v = hex(s[i+1]) * 16 + hex(s[i+2]);
            out += static_cast<char>(v);
            i += 2;
        } else if (c == '+') {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

static char tolower_c(char c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static std::string to_lower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += tolower_c(c);
    return out;
}

// Case-insensitive find: returns position in haystack where needle starts, or npos.
static size_t find_ci(const std::string& haystack, size_t start, const std::string& needle) {
    if (needle.empty()) return std::string::npos;
    if (haystack.size() < needle.size()) return std::string::npos;
    size_t last = haystack.size() - needle.size();
    for (size_t i = start; i <= last; ++i) {
        bool ok = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (tolower_c(haystack[i + j]) != tolower_c(needle[j])) { ok = false; break; }
        }
        if (ok) return i;
    }
    return std::string::npos;
}

static std::string decode_html_entities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    auto parse_entity = [&](size_t& i) -> std::string {
        // i points to char after '&'. Returns decoded text and advances i
        // past the closing ';'. If the entity isn't recognized, returns "&"
        // and leaves i at the next char (so the rest is treated literally).
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) return "&";
        std::string ent = s.substr(i, semi - i);

        if (ent == "amp")         { i = semi + 1; return "&"; }
        if (ent == "lt")          { i = semi + 1; return "<"; }
        if (ent == "gt")          { i = semi + 1; return ">"; }
        if (ent == "quot")        { i = semi + 1; return "\""; }
        if (ent == "apos")        { i = semi + 1; return "'"; }
        if (ent == "#39")         { i = semi + 1; return "'"; }
        if (ent == "nbsp")        { i = semi + 1; return " "; }

        // Numeric: &#NNN; or &#xHH;
        if (ent.size() > 1 && ent[0] == '#') {
            try {
                int code = 0;
                if (ent.size() > 2 && (ent[1] == 'x' || ent[1] == 'X')) {
                    code = std::stoi(ent.substr(2), nullptr, 16);
                } else {
                    code = std::stoi(ent.substr(1), nullptr, 10);
                }
                i = semi + 1;
                // Render basic ASCII / Latin-1 range directly.
                if (code >= 0 && code <= 0x7F) {
                    return std::string(1, static_cast<char>(code));
                }
                // For higher code points, render UTF-8.
                std::string utf;
                if (code <= 0x7FF) {
                    utf += static_cast<char>(0xC0 | (code >> 6));
                    utf += static_cast<char>(0x80 | (code & 0x3F));
                } else if (code <= 0xFFFF) {
                    utf += static_cast<char>(0xE0 | (code >> 12));
                    utf += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    utf += static_cast<char>(0x80 | (code & 0x3F));
                } else {
                    utf += static_cast<char>(0xF0 | (code >> 18));
                    utf += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                    utf += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    utf += static_cast<char>(0x80 | (code & 0x3F));
                }
                return utf;
            } catch (...) {
                return "&";
            }
        }
        // Unknown entity — leave the literal '&' in place.
        return "&";
    };

    for (size_t i = 0; i < s.size(); ) {
        char c = s[i];
        if (c == '&') {
            size_t save = i;
            ++i;  // skip '&'
            std::string decoded = parse_entity(i);
            if (decoded == "&" && i == save + 1) {
                // Not an entity after all; emit '&' and continue from next char.
                out += '&';
            } else {
                out += decoded;
            }
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

// Case-insensitive: remove "<tag ...> ... </tag>" blocks. `tag` is matched
// as a prefix on the start tag, so passing "script" matches "<script>",
// "<script type=...>". Non-greedy: stops at the first matching close tag.
static std::string remove_tag_blocks(const std::string& html, const std::string& tag) {
    std::string lower = to_lower(html);
    std::string open_mark = "<" + tag;
    std::string close_mark = "</" + tag;

    std::string result;
    result.reserve(html.size());
    size_t cursor = 0;
    while (cursor < html.size()) {
        size_t start = find_ci(lower, cursor, open_mark);
        if (start == std::string::npos) {
            result.append(html, cursor, std::string::npos);
            break;
        }
        // Copy everything before the block.
        result.append(html, cursor, start - cursor);
        // Find the closing tag.
        size_t close = find_ci(lower, start, close_mark);
        if (close == std::string::npos) {
            // No closer — drop everything from start to end and stop.
            break;
        }
        // Skip past the close tag's trailing '>'.
        size_t gt = lower.find('>', close);
        cursor = (gt == std::string::npos) ? html.size() : gt + 1;
    }
    return result;
}

// Extract the main article content if a <main>/<article> element is present.
// Returns the inner HTML of the first such container; falls back to the full
// input if none is found.
static std::string extract_main_if_present(const std::string& html) {
    std::string lower = to_lower(html);

    size_t start = std::string::npos;
    // <main or <article (followed by space, >, or attribute).
    for (const std::string& tag : {"<main", "<article"}) {
        size_t p = find_ci(lower, 0, tag);
        // Make sure it's a real tag boundary, not "<mainly" etc.
        if (p != std::string::npos) {
            char after = lower[p + tag.size()];
            if (after == ' ' || after == '>' || after == '\t' || after == '\n') {
                start = p;
                break;
            }
        }
    }
    // role="main"
    if (start == std::string::npos) {
        size_t p = find_ci(lower, 0, "role=\"main\"");
        if (p != std::string::npos) {
            // Backtrack to the opening tag.
            size_t lt = lower.rfind('<', p);
            if (lt != std::string::npos) start = lt;
        }
    }
    if (start == std::string::npos) return html;

    // Find the matching close: first </main> or </article> after start.
    size_t close_main = find_ci(lower, start, "</main>");
    size_t close_article = find_ci(lower, start, "</article>");
    size_t close = std::string::npos;
    if (close_main != std::string::npos && close_article != std::string::npos)
        close = std::min(close_main, close_article);
    else if (close_main != std::string::npos)
        close = close_main;
    else if (close_article != std::string::npos)
        close = close_article;
    if (close == std::string::npos) return html.substr(start);

    return html.substr(start, close - start);
}

// Replace tags that signal block boundaries with newlines so paragraphs survive.
static std::string insert_newlines_for_blocks(const std::string& html) {
    // Replace any of <br>, </p>, </div>, </li>, </h1>..</h6>, </tr> with '\n'.
    // We do a single pass that copies everything and emits '\n' whenever a
    // known closing tag is seen (case-insensitive).
    std::string lower = to_lower(html);
    static const std::vector<std::string> kBlockClosers = {
        "<br", "</p>", "</div>", "</li>", "</h1>", "</h2>", "</h3>",
        "</h4>", "</h5>", "</h6>", "</tr>"
    };

    std::string result;
    result.reserve(html.size());
    size_t i = 0;
    while (i < html.size()) {
        char c = html[i];
        if (c == '<') {
            // Try to match each known closing tag at this position.
            bool matched = false;
            for (auto& tag : kBlockClosers) {
                if (lower.compare(i, tag.size(), tag) == 0) {
                    // For "<br" we may have "<br>", "<br/>", "<br />"; consume
                    // up to and including the next '>'.
                    if (tag == "<br") {
                        size_t gt = lower.find('>', i);
                        i = (gt == std::string::npos) ? html.size() : gt + 1;
                    } else {
                        i += tag.size();
                    }
                    result += '\n';
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
        }
        result += c;
        ++i;
    }
    return result;
}

// Drop all remaining <...> tags.
static std::string strip_remaining_tags(const std::string& html) {
    std::string out;
    out.reserve(html.size() / 2);
    bool in_tag = false;
    for (char c : html) {
        if (!in_tag && c == '<') {
            in_tag = true;
            continue;
        }
        if (in_tag) {
            if (c == '>') in_tag = false;
            continue;
        }
        out += c;
    }
    return out;
}

static std::string collapse_whitespace(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool prev_was_space = false;
    int blank_run = 0;
    auto push = [&](char c) {
        out += c;
        prev_was_space = (c == ' ' || c == '\t');
        if (c == '\n') {
            ++blank_run;
        } else if (!std::isspace((unsigned char)c)) {
            blank_run = 0;
        }
    };

    for (char c : s) {
        if (c == '\r') continue;
        if (c == ' ' || c == '\t') {
            if (!prev_was_space) push(' ');
        } else if (c == '\n') {
            // Allow at most one blank line in the output.
            if (blank_run < 2) push('\n');
        } else {
            push(c);
        }
    }
    return out;
}

// The full HTML → text pipeline. Order matters: remove blocks first, then
// narrow to main content, then convert structural tags to newlines, then
// drop remaining tags, then decode entities, then collapse whitespace.
static std::string strip_html(const std::string& html_in) {
    if (html_in.empty()) return "";

    std::string s = html_in;
    // <!DOCTYPE ...> and HTML comments
    {
        std::string lower = to_lower(s);
        // Strip comments
        size_t pos = 0;
        std::string acc;
        while (true) {
            size_t start = lower.find("<!--", pos);
            if (start == std::string::npos) {
                acc.append(s, pos, std::string::npos);
                break;
            }
            acc.append(s, pos, start - pos);
            size_t end = lower.find("-->", start);
            pos = (end == std::string::npos) ? s.size() : end + 3;
        }
        s = acc;
    }

    // Remove blocks we never want
    s = remove_tag_blocks(s, "script");
    s = remove_tag_blocks(s, "style");
    s = remove_tag_blocks(s, "nav");
    s = remove_tag_blocks(s, "header");
    s = remove_tag_blocks(s, "footer");
    s = remove_tag_blocks(s, "aside");
    s = remove_tag_blocks(s, "form");
    s = remove_tag_blocks(s, "noscript");

    // Narrow to main content if possible
    s = extract_main_if_present(s);

    // Block-boundary tags → newline
    s = insert_newlines_for_blocks(s);

    // Drop remaining tags
    s = strip_remaining_tags(s);

    // Decode entities
    s = decode_html_entities(s);

    // Collapse whitespace runs
    s = collapse_whitespace(s);

    return s;
}

// ---------------------------------------------------------------------------
// Parsing helpers for DuckDuckGo result pages
// ---------------------------------------------------------------------------

// Pull the value of a URL query parameter out of an href string. Returns ""
// if the parameter isn't present. Handles the case where the param value
// continues to the end of the query (no trailing &).
static std::string extract_query_param(const std::string& url, const std::string& param) {
    std::string needle = param + "=";
    size_t pos = find_ci(url, 0, needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    size_t end = url.find_first_of("&\"#", pos);
    if (end == std::string::npos) end = url.size();
    return url_decode(url.substr(pos, end - pos));
}

// Extract the first `max_results` results from a DDG results page.
// anchor_class: "result-link" (lite) or "result__a" (html).
// snippet_class: "result-snippet" (lite) or "result__snippet" (html).
struct SearchResult { std::string title; std::string url; std::string snippet; };

static std::vector<SearchResult> parse_ddg_results(const std::string& html,
                                                    const std::string& anchor_class,
                                                    const std::string& snippet_class,
                                                    int max_results) {
    std::vector<SearchResult> out;
    if (max_results <= 0) return out;

    std::string lower = to_lower(html);
    std::string anchor_needle = "class=\"" + anchor_class + "\"";
    std::string snippet_needle = "class=\"" + snippet_class + "\"";

    size_t cursor = 0;
    while ((int)out.size() < max_results) {
        size_t anchor = find_ci(lower, cursor, anchor_needle);
        if (anchor == std::string::npos) break;

        // Backtrack to find the opening '<' of this anchor so we can grab href.
        size_t lt = lower.rfind('<', anchor);
        if (lt == std::string::npos) { cursor = anchor + anchor_needle.size(); continue; }

        // Find the closing '>' of the anchor start tag.
        size_t tag_end = lower.find('>', anchor);
        if (tag_end == std::string::npos) break;

        // Within the start tag, look for href="..."
        std::string start_tag = html.substr(lt, tag_end - lt + 1);
        std::string href;
        {
            size_t hp = find_ci(start_tag, 0, "href=\"");
            if (hp != std::string::npos) {
                hp += 6;  // strlen("href=\"")
                size_t quote = start_tag.find('"', hp);
                if (quote != std::string::npos)
                    href = start_tag.substr(hp, quote - hp);
            }
        }

        // Extract uddg= param if present; otherwise use href directly.
        std::string url = extract_query_param(href, "uddg");
        if (url.empty()) url = href;

        // Title: between '>' and the next '<' after the anchor start tag.
        size_t title_start = tag_end + 1;
        size_t title_end = lower.find('<', title_start);
        std::string title_html;
        if (title_end != std::string::npos) {
            title_html = html.substr(title_start, title_end - title_start);
        }
        std::string title = decode_html_entities(title_html);
        // Trim whitespace
        auto trim = [](std::string& t) {
            size_t a = t.find_first_not_of(" \t\r\n");
            size_t b = t.find_last_not_of(" \t\r\n");
            if (a == std::string::npos) { t.clear(); return; }
            t = t.substr(a, b - a + 1);
        };
        trim(title);

        // Snippet: find the next occurrence of snippet_needle after this anchor.
        std::string snippet;
        size_t snip = find_ci(lower, title_end == std::string::npos ? tag_end : title_end, snippet_needle);
        if (snip != std::string::npos) {
            size_t snip_tag_end = lower.find('>', snip);
            if (snip_tag_end != std::string::npos) {
                size_t snip_start = snip_tag_end + 1;
                size_t snip_close = lower.find('<', snip_start);
                if (snip_close != std::string::npos) {
                    std::string snip_html = html.substr(snip_start, snip_close - snip_start);
                    // The snippet may contain inline tags (<b>, <em>) — strip them.
                    snippet = strip_remaining_tags(snip_html);
                    snippet = decode_html_entities(snippet);
                    trim(snippet);
                }
            }
        }

        if (!url.empty() || !title.empty()) {
            out.push_back({ std::move(title), std::move(url), std::move(snippet) });
        }

        cursor = (title_end == std::string::npos) ? lower.size() : title_end + 1;
    }
    return out;
}

// Parse Mojeek's results page. Each result is an `<li class="rN">` containing
// `<a class="title" href="URL">TITLE</a>` and a `<p class="s">SNIPPET</p>`.
// URLs are direct (no redirect wrapper), so we take href verbatim.
static std::vector<SearchResult> parse_mojeek_results(const std::string& html,
                                                      int max_results) {
    std::vector<SearchResult> out;
    if (max_results <= 0) return out;

    std::string lower = to_lower(html);
    const std::string title_needle   = "class=\"title\"";
    const std::string snippet_needle = "class=\"s\"";

    auto trim = [](std::string& t) {
        size_t a = t.find_first_not_of(" \t\r\n");
        size_t b = t.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) { t.clear(); return; }
        t = t.substr(a, b - a + 1);
    };

    size_t cursor = 0;
    while ((int)out.size() < max_results) {
        size_t anchor = find_ci(lower, cursor, title_needle);
        if (anchor == std::string::npos) break;

        // Backtrack to opening '<' so we can read the start tag (for href).
        size_t lt = lower.rfind('<', anchor);
        if (lt == std::string::npos) { cursor = anchor + title_needle.size(); continue; }

        size_t tag_end = lower.find('>', anchor);
        if (tag_end == std::string::npos) break;

        std::string start_tag = html.substr(lt, tag_end - lt + 1);
        std::string href;
        {
            size_t hp = find_ci(start_tag, 0, "href=\"");
            if (hp != std::string::npos) {
                hp += 6;
                size_t quote = start_tag.find('"', hp);
                if (quote != std::string::npos)
                    href = start_tag.substr(hp, quote - hp);
            }
        }

        // Title text sits between the opening tag's '>' and the next '<'.
        size_t title_start = tag_end + 1;
        size_t title_end = lower.find('<', title_start);
        std::string title;
        if (title_end != std::string::npos) {
            title = decode_html_entities(html.substr(title_start, title_end - title_start));
            trim(title);
        }

        // Snippet: first <p class="s"> after the title. Read to </p> (not just
        // the next '<', since Mojeek bolds query terms with <strong>).
        std::string snippet;
        size_t search_from = (title_end == std::string::npos) ? tag_end : title_end;
        size_t snip = find_ci(lower, search_from, snippet_needle);
        if (snip != std::string::npos) {
            size_t snip_tag_end = lower.find('>', snip);
            if (snip_tag_end != std::string::npos) {
                size_t snip_start = snip_tag_end + 1;
                size_t snip_close = find_ci(lower, snip_start, "</p>");
                if (snip_close == std::string::npos)
                    snip_close = lower.find('<', snip_start);
                if (snip_close != std::string::npos) {
                    snippet = strip_remaining_tags(html.substr(snip_start, snip_close - snip_start));
                    snippet = decode_html_entities(snippet);
                    trim(snippet);
                }
            }
        }

        if (!href.empty() || !title.empty()) {
            out.push_back({ std::move(title), std::move(href), std::move(snippet) });
        }

        cursor = (title_end == std::string::npos) ? lower.size() : title_end + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// WebSearchTool
// ---------------------------------------------------------------------------

class WebSearchTool : public Tool {
public:
    std::string name() const override { return "web_search"; }
    std::string description() const override {
        return "Search the web (Mojeek, no API key) and return ranked results. "
               "Use this FIRST when researching — it's cheap. Read the snippets "
               "before calling web_extract on any URL.";
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"query",       {{"type", "string"},  {"description", "Search query"}}},
                {"max_results", {{"type", "integer"}, {"description", "Max results (default 5, capped 10)"}}}
            }},
            {"required", nlohmann::json::array({"query"})}
        };
    }
    std::string required_permission() const override { return "web_search"; }
    std::string resource(const nlohmann::json& in, const ToolContext&) const override {
        return in.value("query", "");
    }

    ToolResult execute(const nlohmann::json& input, const ToolContext& ctx) override {
        std::string query = input.value("query", "");
        if (query.empty())
            return {false, "", "web_search: missing or empty 'query'."};

        int max_results = 5;
        if (input.contains("max_results") && input["max_results"].is_number_integer()) {
            int n = input["max_results"].get<int>();
            if (n > 0) max_results = n;
        }
        if (max_results > 10) max_results = 10;

        // Engine: read from config if available; default mojeek.
        std::string engine = "mojeek";
        if (ctx.config && !ctx.config->web_search_engine.empty())
            engine = ctx.config->web_search_engine;
        if (engine != "mojeek" && engine != "ddg_lite" && engine != "ddg_html")
            engine = "mojeek";

        std::string url;
        std::string anchor_class, snippet_class;
        if (engine == "ddg_html") {
            url = "https://html.duckduckgo.com/html/?q=" + url_encode(query);
            anchor_class  = "result__a";
            snippet_class = "result__snippet";
        } else if (engine == "ddg_lite") {
            url = "https://lite.duckduckgo.com/lite/?q=" + url_encode(query) + "&kl=us-en";
            anchor_class  = "result-link";
            snippet_class = "result-snippet";
        }  // mojeek: URL built below.

        std::map<std::string, std::string> headers = {
            {"User-Agent", kBrowserUA},
            {"Accept",     "text/html,application/xhtml+xml"},
        };

        std::string body;
        try {
            if (engine == "mojeek") {
                std::string murl = "https://www.mojeek.com/search?q=" + url_encode(query);
                body = http_.get(murl, headers, 20L);
            } else {
                body = http_.get(url, headers, 20L);
            }
        } catch (const std::exception& e) {
            return {false, "", std::string("web_search fetch failed: ") + e.what()};
        }
        if (body.empty())
            return {true, "(no results — empty response from search engine)", ""};

        // DuckDuckGo now serves an "anomaly" CAPTCHA page (HTTP 202) to many
        // clients; parsing it yields zero results and a confusing "(no results)"
        // message. Detect the markers and tell the user how to recover.
        if (engine == "ddg_lite" || engine == "ddg_html") {
            std::string lower = to_lower(body);
            if (lower.find("anomaly-modal") != std::string::npos ||
                lower.find("bots use duckduckgo") != std::string::npos) {
                return {false, "",
                    "web_search: DuckDuckGo returned a CAPTCHA challenge page. "
                    "Switch engine to 'mojeek' in config (web_search.engine) and retry."};
            }
        }

        std::vector<SearchResult> results;
        if (engine == "mojeek") {
            results = parse_mojeek_results(body, max_results);
        } else {
            results = parse_ddg_results(body, anchor_class, snippet_class, max_results);
        }

        if (results.empty())
            return {true, "(no results)", ""};

        nlohmann::json arr = nlohmann::json::array();
        for (auto& r : results) {
            arr.push_back({
                {"title",   r.title},
                {"url",     r.url},
                {"snippet", r.snippet}
            });
        }

        std::string out = arr.dump(2);
        if (out.size() > MAX_OUTPUT) {
            out.resize(MAX_OUTPUT);
            out += "\n[output truncated]";
        }
        return {true, out, ""};
    }

private:
    HttpClient http_;
};

// ---------------------------------------------------------------------------
// WebExtractTool
// ---------------------------------------------------------------------------

class WebExtractTool : public Tool {
public:
    std::string name() const override { return "web_extract"; }
    std::string description() const override {
        return "Fetch one URL and return its cleaned main-body text (article "
               "text, not nav/ads/scripts). Use selectively on URLs that "
               "web_search suggested were relevant.";
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"url",       {{"type", "string"},  {"description", "http(s) URL"}}},
                {"max_chars", {{"type", "integer"}, {"description", "Truncate to this many chars (default 8000)"}}}
            }},
            {"required", nlohmann::json::array({"url"})}
        };
    }
    std::string required_permission() const override { return "web_extract"; }
    std::string resource(const nlohmann::json& in, const ToolContext&) const override {
        return in.value("url", "");
    }

    ToolResult execute(const nlohmann::json& input, const ToolContext& /*ctx*/) override {
        std::string url = input.value("url", "");
        if (url.empty())
            return {false, "", "web_extract: missing or empty 'url'."};
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
            return {false, "", "web_extract: url must start with http:// or https://"};

        int max_chars = input.value("max_chars", 8000);
        if (max_chars <= 0) max_chars = 8000;

        std::map<std::string, std::string> headers = {
            {"User-Agent", kBrowserUA},
            {"Accept",     "text/html,application/xhtml+xml"},
        };

        std::string body;
        try {
            body = http_.get(url, headers, 15L);
        } catch (const std::exception& e) {
            nlohmann::json err = {{"url", url}, {"error", std::string("fetch failed: ") + e.what()}};
            return {false, err.dump(2), "fetch failed"};
        }
        if (body.empty()) {
            nlohmann::json err = {{"url", url},
                                  {"error", "no extractable content (empty response)"}};
            return {true, err.dump(2), ""};
        }

        std::string text = strip_html(body);
        if (text.empty()) {
            nlohmann::json err = {{"url", url},
                                  {"error", "no extractable content (paywall, JS-rendered page, or non-article URL)"}};
            return {true, err.dump(2), ""};
        }

        bool truncated = false;
        if ((int)text.size() > max_chars) {
            text.resize(max_chars);
            truncated = true;
        }

        nlohmann::json out = {
            {"url",        url},
            {"text",       text},
            {"truncated",  truncated}
        };
        std::string dumped = out.dump(2);
        if (dumped.size() > MAX_OUTPUT) {
            dumped.resize(MAX_OUTPUT);
            dumped += "\n[output truncated]";
        }
        return {true, dumped, ""};
    }

private:
    HttpClient http_;
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void register_web_tools(ToolRegistry& registry) {
    registry.register_tool(std::make_shared<WebSearchTool>());
    registry.register_tool(std::make_shared<WebExtractTool>());
}

} // namespace haicode
