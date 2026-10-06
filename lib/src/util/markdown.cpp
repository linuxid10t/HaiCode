#include "haicode/markdown.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace haicode::md {

// ---------------------------------------------------------------------------
// Styled
// ---------------------------------------------------------------------------

void
Styled::add(std::string_view s, uint16_t flags, uint8_t heading)
{
    if (s.empty()) return;
    if (runs.empty() || runs.back().flags != flags || runs.back().heading != heading)
        runs.push_back({text.size(), flags, heading});
    text.append(s);
}

void
Styled::append(const Styled& other)
{
    const size_t base = text.size();
    for (const LinkSpan& l : other.links)
        links.push_back({l.start + base, l.end + base, l.target, l.implicit});
    for (size_t i = 0; i < other.runs.size(); ++i) {
        size_t start = other.runs[i].offset;
        size_t end = i + 1 < other.runs.size() ? other.runs[i + 1].offset
                                               : other.text.size();
        add(std::string_view(other.text).substr(start, end - start),
            other.runs[i].flags, other.runs[i].heading);
    }
}

Run
Styled::style_at(size_t pos) const
{
    auto it = std::upper_bound(runs.begin(), runs.end(), pos,
        [](size_t p, const Run& r) { return p < r.offset; });
    if (it == runs.begin()) return {pos, 0, 0};
    return *(it - 1);
}

Styled
Styled::slice(size_t from) const
{
    Styled out;
    for (size_t i = 0; i < runs.size(); ++i) {
        size_t start = runs[i].offset;
        size_t end = i + 1 < runs.size() ? runs[i + 1].offset : text.size();
        if (end <= from) continue;
        start = std::max(start, from);
        out.add(std::string_view(text).substr(start, end - start),
                runs[i].flags, runs[i].heading);
    }
    for (const LinkSpan& l : links) {
        if (l.end <= from) continue;
        out.links.push_back({std::max(l.start, from) - from, l.end - from,
                             l.target, l.implicit});
    }
    return out;
}

bool
Styled::operator==(const Styled& o) const
{
    if (text != o.text || runs.size() != o.runs.size() || links != o.links)
        return false;
    for (size_t i = 0; i < runs.size(); ++i) {
        if (runs[i].offset != o.runs[i].offset || runs[i].flags != o.runs[i].flags
            || runs[i].heading != o.runs[i].heading)
            return false;
    }
    return true;
}

size_t
common_prefix(const Styled& a, const Styled& b)
{
    size_t n = std::min(a.text.size(), b.text.size());
    size_t i = 0;
    size_t ra = 0, rb = 0;  // current run index in a / b
    while (i < n && a.text[i] == b.text[i]) {
        while (ra + 1 < a.runs.size() && a.runs[ra + 1].offset <= i) ++ra;
        while (rb + 1 < b.runs.size() && b.runs[rb + 1].offset <= i) ++rb;
        const Run& x = a.runs[ra];
        const Run& y = b.runs[rb];
        if (x.flags != y.flags || x.heading != y.heading) break;
        ++i;
    }
    // Never split a UTF-8 sequence: back off to the start of the character.
    auto continuation = [](char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; };
    while (i > 0 && ((i < a.text.size() && continuation(a.text[i]))
                     || (i < b.text.size() && continuation(b.text[i]))))
        --i;
    return i;
}

// ---------------------------------------------------------------------------
// UTF-8 / width helpers
// ---------------------------------------------------------------------------

namespace {

// Decode one code point at s[i]; `len` receives its byte length. Invalid
// sequences decode as the single byte (width 1) so nothing is ever dropped.
uint32_t
decode_utf8(std::string_view s, size_t i, size_t& len)
{
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t need = 0;
    uint32_t cp = 0;
    if (c < 0x80) { len = 1; return c; }
    else if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; }
    else { len = 1; return c; }
    if (i + need >= s.size()) { len = 1; return c; }
    for (size_t k = 1; k <= need; ++k) {
        unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) { len = 1; return c; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    len = need + 1;
    return cp;
}

int
cp_width(uint32_t cp)
{
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x200B && cp <= 0x200F)
        || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x20E3)
        return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF)
        || (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF)
        || (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60)
        || (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F)
        || (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD))
        return 2;
    return 1;
}

}  // namespace

size_t
display_width(std::string_view utf8)
{
    size_t w = 0;
    for (size_t i = 0; i < utf8.size();) {
        size_t len = 1;
        w += cp_width(decode_utf8(utf8, i, len));
        i += len;
    }
    return w;
}

// ---------------------------------------------------------------------------
// Line-level helpers
// ---------------------------------------------------------------------------

namespace {

struct Line {
    std::string_view s;     // content without the line break
    size_t           end;   // source offset just past the line break (or text end)
    bool             nl;    // the line break has arrived
};

std::vector<Line>
split_lines(std::string_view text, size_t from)
{
    std::vector<Line> lines;
    size_t pos = std::min(from, text.size());
    for (;;) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) {
            lines.push_back({text.substr(pos), text.size(), false});
            break;
        }
        std::string_view s = text.substr(pos, nl - pos);
        if (!s.empty() && s.back() == '\r') s.remove_suffix(1);
        lines.push_back({s, nl + 1, true});
        pos = nl + 1;
    }
    return lines;
}

bool is_space(char c) { return c == ' ' || c == '\t'; }
bool is_ws(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }
bool is_word(char c)
{
    unsigned char u = static_cast<unsigned char>(c);
    return u >= 0x80 || std::isalnum(u);
}
bool is_punct(char c) { return std::ispunct(static_cast<unsigned char>(c)) != 0; }

std::string_view
trim(std::string_view s)
{
    while (!s.empty() && is_ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_ws(s.back())) s.remove_suffix(1);
    return s;
}

bool
is_blank(std::string_view s)
{
    return trim(s).empty();
}

// Leading indentation in columns (tab = 4); *idx receives the first
// non-indent byte.
int
indent_of(std::string_view s, size_t* idx)
{
    int cols = 0;
    size_t i = 0;
    for (; i < s.size() && is_space(s[i]); ++i)
        cols += s[i] == '\t' ? 4 - cols % 4 : 1;
    if (idx) *idx = i;
    return cols;
}

std::string
repeat(std::string_view s, int n)
{
    std::string out;
    for (int i = 0; i < n; ++i) out.append(s);
    return out;
}

bool
fence_open(std::string_view line, char& ch, int& len, int& indent,
           std::string_view& info)
{
    size_t i = 0;
    indent = indent_of(line, &i);
    if (i >= line.size()) return false;
    char c = line[i];
    if (c != '`' && c != '~') return false;
    size_t k = i;
    while (k < line.size() && line[k] == c) ++k;
    if (k - i < 3) return false;
    info = trim(line.substr(k));
    if (c == '`' && info.find('`') != std::string_view::npos) return false;
    ch = c;
    len = static_cast<int>(k - i);
    return true;
}

bool
fence_close(std::string_view line, char ch, int len)
{
    size_t i = 0;
    indent_of(line, &i);
    size_t k = i;
    while (k < line.size() && line[k] == ch) ++k;
    if (static_cast<int>(k - i) < len) return false;
    return is_blank(line.substr(k));
}

bool
atx_heading(std::string_view line, int& level, std::string_view& content)
{
    size_t i = 0;
    if (indent_of(line, &i) > 3) return false;
    size_t k = i;
    while (k < line.size() && line[k] == '#') ++k;
    int n = static_cast<int>(k - i);
    if (n < 1 || n > 6) return false;
    if (k < line.size() && !is_space(line[k])) return false;
    std::string_view c = trim(line.substr(k));
    // Optional closing sequence: " ##" at the end (or nothing but #s).
    size_t e = c.size();
    while (e > 0 && c[e - 1] == '#') --e;
    if (e == 0) c = {};
    else if (e < c.size() && is_space(c[e - 1])) c = trim(c.substr(0, e));
    level = n;
    content = c;
    return true;
}

bool
thematic_break(std::string_view line)
{
    size_t i = 0;
    if (indent_of(line, &i) > 3) return false;
    if (i >= line.size()) return false;
    char c = line[i];
    if (c != '-' && c != '*' && c != '_') return false;
    int count = 0;
    for (; i < line.size(); ++i) {
        if (line[i] == c) ++count;
        else if (!is_ws(line[i])) return false;
    }
    return count >= 3;
}

bool
quote_prefix(std::string_view line, int& level, std::string_view& content)
{
    size_t i = 0;
    if (indent_of(line, &i) > 3) return false;
    if (i >= line.size() || line[i] != '>') return false;
    level = 0;
    while (i < line.size() && line[i] == '>') {
        ++level;
        ++i;
        if (i < line.size() && is_space(line[i])) ++i;
        // Nested "> >" form.
        size_t j = i;
        while (j < line.size() && is_space(line[j])) ++j;
        if (j < line.size() && line[j] == '>') i = j;
    }
    content = trim(line.substr(i));
    return true;
}

struct ListItem {
    int              level = 0;
    bool             ordered = false;
    std::string_view marker;
    std::string_view content;
};

bool
list_item(std::string_view line, ListItem& item)
{
    size_t i = 0;
    int cols = indent_of(line, &i);
    if (i >= line.size()) return false;
    size_t m_end;
    char c = line[i];
    if (c == '-' || c == '*' || c == '+') {
        m_end = i + 1;
        item.ordered = false;
    } else {
        size_t k = i;
        while (k < line.size() && k - i < 9 && std::isdigit(static_cast<unsigned char>(line[k]))) ++k;
        if (k == i || k >= line.size() || (line[k] != '.' && line[k] != ')')) return false;
        m_end = k + 1;
        item.ordered = true;
    }
    if (m_end < line.size() && !is_space(line[m_end])) return false;
    item.level = std::min(cols / 2, 6);
    item.marker = line.substr(i, m_end - i);
    item.content = trim(line.substr(m_end));
    return true;
}

// Position of the closing backtick run of exactly `k` backticks at or after
// `from`, or npos.
size_t
find_code_close(std::string_view s, size_t from, size_t k)
{
    size_t p = from;
    while (p < s.size()) {
        if (s[p] == '`') {
            size_t r = p;
            while (r < s.size() && s[r] == '`') ++r;
            if (r - p == k) return p;
            p = r;
        } else {
            ++p;
        }
    }
    return std::string_view::npos;
}

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------

enum class Align { Left, Center, Right };

bool
has_pipe(std::string_view s)
{
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\') { ++i; continue; }
        if (s[i] == '|') return true;
    }
    return false;
}

std::vector<std::string_view>
split_row(std::string_view line)
{
    std::string_view t = trim(line);
    if (!t.empty() && t.front() == '|') t.remove_prefix(1);
    if (!t.empty() && t.back() == '|' && !(t.size() >= 2 && t[t.size() - 2] == '\\'))
        t.remove_suffix(1);
    std::vector<std::string_view> cells;
    size_t start = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '\\') { ++i; continue; }
        if (t[i] == '`') {
            size_t r = i;
            while (r < t.size() && t[r] == '`') ++r;
            size_t close = find_code_close(t, r, r - i);
            if (close != std::string_view::npos) {
                i = close + (r - i) - 1;
            } else {
                i = r - 1;
            }
            continue;
        }
        if (t[i] == '|') {
            cells.push_back(trim(t.substr(start, i - start)));
            start = i + 1;
        }
    }
    cells.push_back(trim(t.substr(start)));
    return cells;
}

bool
delimiter_row(std::string_view line, size_t ncols, std::vector<Align>* aligns)
{
    if (!has_pipe(line)) return false;
    auto cells = split_row(line);
    if (cells.size() != ncols) return false;
    std::vector<Align> out;
    for (auto c : cells) {
        if (c.empty()) return false;
        bool left = c.front() == ':';
        bool right = c.back() == ':';
        std::string_view d = c;
        if (left) d.remove_prefix(1);
        if (right && !d.empty()) d.remove_suffix(1);
        if (d.empty()) return false;
        for (char ch : d)
            if (ch != '-') return false;
        out.push_back(left && right ? Align::Center : right ? Align::Right : Align::Left);
    }
    if (aligns) *aligns = std::move(out);
    return true;
}

// ---------------------------------------------------------------------------
// Inline rendering
// ---------------------------------------------------------------------------

constexpr int kMaxInlineDepth = 16;

// Opening position of an emphasis closer: a run of exactly k `c` characters
// not preceded by whitespace (for '_', not followed by a word character).
size_t
find_closer(std::string_view s, size_t from, char c, size_t k)
{
    size_t p = from;
    while (p < s.size()) {
        char ch = s[p];
        if (ch == '\\') { p += 2; continue; }
        if (ch == '`') {
            size_t r = p;
            while (r < s.size() && s[r] == '`') ++r;
            size_t close = find_code_close(s, r, r - p);
            p = close != std::string_view::npos ? close + (r - p) : r;
            continue;
        }
        if (ch == c) {
            size_t r = p;
            while (r < s.size() && s[r] == c) ++r;
            char prev = p > 0 ? s[p - 1] : ' ';
            char next = r < s.size() ? s[r] : ' ';
            if (r - p == k && !is_ws(prev) && (c != '_' || !is_word(next)))
                return p;
            p = r;
            continue;
        }
        ++p;
    }
    return std::string_view::npos;
}

size_t
close_bracket(std::string_view s, size_t open)
{
    int depth = 0;
    for (size_t p = open; p < s.size(); ++p) {
        char c = s[p];
        if (c == '\\') { ++p; continue; }
        if (c == '`') {
            size_t r = p;
            while (r < s.size() && s[r] == '`') ++r;
            size_t close = find_code_close(s, r, r - p);
            if (close != std::string_view::npos) p = close + (r - p) - 1;
            else p = r - 1;
            continue;
        }
        if (c == '[') ++depth;
        else if (c == ']' && --depth == 0) return p;
    }
    return std::string_view::npos;
}

size_t
close_paren(std::string_view s, size_t open)
{
    int depth = 0;
    for (size_t p = open; p < s.size(); ++p) {
        char c = s[p];
        if (c == '\\') { ++p; continue; }
        if (c == '(') ++depth;
        else if (c == ')' && --depth == 0) return p;
    }
    return std::string_view::npos;
}

bool
starts_with_ci(std::string_view s, std::string_view prefix)
{
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i]))
            != std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}

// An inline code span that reads as a path (`lib/src/x.cpp`, `/tmp/a.png`,
// `~/notes.md`, `config.json:12`) or a URL. Only a candidate: the frontend
// makes it clickable once the target turns out to exist.
bool
looks_like_link(std::string_view c)
{
    if (c.empty() || c.size() > 512) return false;
    if (starts_with_ci(c, "http://") || starts_with_ci(c, "https://")) return true;
    if (c.find("::") != std::string_view::npos) return false;
    for (char ch : c) {
        if (is_ws(ch) || std::strchr("()<>=,;{}\"'*$`|&!?[]\\", ch)) return false;
    }
    if (c.front() == '/' || c.rfind("~/", 0) == 0 || c.rfind("./", 0) == 0
        || c.rfind("../", 0) == 0 || c.find('/') != std::string_view::npos)
        return true;
    // name.ext[:line] — the extension carries a letter (not "1.5").
    std::string_view name = c.substr(0, c.find(':'));
    size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= name.size()) return false;
    std::string_view ext = name.substr(dot + 1);
    if (ext.size() > 10) return false;
    bool letter = false;
    for (char ch : ext) {
        if (!std::isalnum(static_cast<unsigned char>(ch))) return false;
        letter = letter || std::isalpha(static_cast<unsigned char>(ch));
    }
    return letter;
}

void render_inline(std::string_view s, uint16_t flags, uint8_t heading,
                   Styled& out, int depth = 0);

void
render_inline(std::string_view s, uint16_t flags, uint8_t heading, Styled& out,
              int depth)
{
    if (depth > kMaxInlineDepth) {
        out.add(s, flags, heading);
        return;
    }
    std::string buf;
    auto flush = [&] {
        out.add(buf, flags, heading);
        buf.clear();
    };
    const uint16_t keep = flags & (kMono | kQuote);  // survives into decorations
    // Inside a link label nothing registers another link (spans never nest).
    const bool in_link = (flags & kLink) != 0;
    auto add_link = [&](size_t start, std::string_view target, bool implicit) {
        if (!in_link && out.text.size() > start)
            out.links.push_back({start, out.text.size(), std::string(target), implicit});
    };
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];

        if (c == '\\' && i + 1 < s.size() && is_punct(s[i + 1])) {
            buf += s[i + 1];
            i += 2;
            continue;
        }

        if (c == '`') {
            size_t r = i;
            while (r < s.size() && s[r] == '`') ++r;
            size_t k = r - i;
            size_t close = find_code_close(s, r, k);
            if (close == std::string_view::npos) {
                buf.append(s.substr(i, k));
                i = r;
                continue;
            }
            std::string_view code = s.substr(r, close - r);
            if (code.size() >= 2 && code.front() == ' ' && code.back() == ' '
                && !trim(code).empty())
                code = code.substr(1, code.size() - 2);
            flush();
            size_t start = out.text.size();
            out.add(code, flags | kCode, heading);
            if (looks_like_link(code)) add_link(start, code, true);
            i = close + k;
            continue;
        }

        if (c == '!' && i + 1 < s.size() && s[i + 1] == '[') {
            size_t cb = close_bracket(s, i + 1);
            if (cb != std::string_view::npos && cb + 1 < s.size() && s[cb + 1] == '(') {
                size_t cp = close_paren(s, cb + 1);
                if (cp != std::string_view::npos) {
                    std::string_view alt = trim(s.substr(i + 2, cb - i - 2));
                    std::string_view src = trim(s.substr(cb + 2, cp - cb - 2));
                    if (!src.empty() && src.front() == '<') {
                        size_t gt = src.find('>');
                        src = gt != std::string_view::npos ? src.substr(1, gt - 1)
                                                           : src.substr(1);
                    } else {
                        size_t sp = 0;
                        while (sp < src.size() && !is_ws(src[sp])) ++sp;
                        src = src.substr(0, sp);
                    }
                    flush();
                    // Styled like any other link: label, then the source dim.
                    std::string label = alt.empty() ? std::string("[image]")
                                                    : "[image: " + std::string(alt) + "]";
                    size_t start = out.text.size();
                    out.add(label, flags | kLink, heading);
                    if (!src.empty())
                        out.add(" (" + std::string(src) + ")", keep | kDim, heading);
                    add_link(start, src, false);
                    i = cp + 1;
                    continue;
                }
            }
        }

        if (c == '[') {
            size_t cb = close_bracket(s, i);
            if (cb != std::string_view::npos && cb > i + 1 && cb + 1 < s.size()
                && s[cb + 1] == '(') {
                size_t cp = close_paren(s, cb + 1);
                if (cp != std::string_view::npos) {
                    std::string_view label = s.substr(i + 1, cb - i - 1);
                    std::string_view dest = trim(s.substr(cb + 2, cp - cb - 2));
                    if (!dest.empty() && dest.front() == '<') {
                        size_t gt = dest.find('>');
                        dest = gt != std::string_view::npos ? dest.substr(1, gt - 1)
                                                            : dest.substr(1);
                    } else {
                        size_t sp = 0;
                        while (sp < dest.size() && !is_ws(dest[sp])) ++sp;
                        dest = dest.substr(0, sp);
                    }
                    flush();
                    size_t start = out.text.size();
                    render_inline(label, flags | kLink, heading, out, depth + 1);
                    if (!dest.empty() && dest != label && dest.front() != '#')
                        out.add(" (" + std::string(dest) + ")", keep | kDim, heading);
                    add_link(start, dest, false);
                    i = cp + 1;
                    continue;
                }
            }
        }

        if (c == '<') {
            size_t gt = s.find('>', i + 1);
            if (gt != std::string_view::npos) {
                std::string_view url = s.substr(i + 1, gt - i - 1);
                bool scheme = url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0
                              || url.rfind("mailto:", 0) == 0;
                bool clean = std::none_of(url.begin(), url.end(),
                                          [](char ch) { return is_ws(ch) || ch == '<'; });
                if (scheme && clean) {
                    flush();
                    size_t start = out.text.size();
                    out.add(url, flags | kLink, heading);
                    add_link(start, url, false);
                    i = gt + 1;
                    continue;
                }
            }
        }

        if (c == '*' || c == '_' || c == '~') {
            size_t r = i;
            while (r < s.size() && s[r] == c) ++r;
            size_t k = r - i;
            char prev = i > 0 ? s[i - 1] : ' ';
            char next = r < s.size() ? s[r] : ' ';
            bool can_open = !is_ws(next) && (c != '_' || !is_word(prev))
                            && (c != '~' || k == 2);
            if (can_open) {
                size_t close = find_closer(s, r, c, k);
                if (close != std::string_view::npos && close > r) {
                    uint16_t add = kBold | kItalic;
                    if (c == '~') add = kStrike;
                    else if (k == 1) add = kItalic;
                    else if (k == 2) add = kBold;
                    flush();
                    render_inline(s.substr(r, close - r),
                                  static_cast<uint16_t>(flags | add), heading, out,
                                  depth + 1);
                    i = close + k;
                    continue;
                }
            }
            buf.append(s.substr(i, k));
            i = r;
            continue;
        }

        // Bare http(s) URL (GFM autolink extension): runs to whitespace,
        // minus trailing punctuation and an unbalanced closing paren.
        if ((c == 'h' || c == 'H') && !in_link && (i == 0 || !is_word(s[i - 1]))
            && (starts_with_ci(s.substr(i), "http://")
                || starts_with_ci(s.substr(i), "https://"))) {
            size_t e = i;
            while (e < s.size() && !is_ws(s[e]) && s[e] != '<' && s[e] != '`') ++e;
            while (e > i) {
                char t = s[e - 1];
                if (std::strchr(".,;:!?'\"*_~", t)) { --e; continue; }
                if (t == ')') {
                    std::string_view u = s.substr(i, e - i);
                    if (std::count(u.begin(), u.end(), ')') > std::count(u.begin(), u.end(), '(')) {
                        --e;
                        continue;
                    }
                }
                break;
            }
            size_t scheme_len = (s[i + 4] == 's' || s[i + 4] == 'S') ? 8 : 7;
            if (e > i + scheme_len) {
                flush();
                size_t start = out.text.size();
                out.add(s.substr(i, e - i), flags | kLink, heading);
                add_link(start, s.substr(i, e - i), false);
                i = e;
                continue;
            }
        }

        buf += c;
        ++i;
    }
    flush();
}

// ---------------------------------------------------------------------------
// Units
// ---------------------------------------------------------------------------

// Start a new output line: the line break LEADS (ends the previous line), and
// a pending blank source line becomes exactly one empty line.
void
begin_line(Styled& out, State& st)
{
    if (st.any_output) {
        out.add("\n");
        if (st.pending_blank) out.add("\n");
    }
    st.pending_blank = false;
    st.any_output = true;
}

struct Glyph {
    std::string s;
    int         w = 0;
    uint16_t    flags = 0;
    bool        brk = false;   // forced line break (<br> in a cell)
    int         link = -1;     // index into the cell's links, -1 = none
};

struct Cell {
    std::vector<Glyph>    glyphs;
    std::vector<LinkSpan> links;   // targets only; positions come from layout
};

Cell
cell_glyphs(std::string_view raw, uint16_t base)
{
    // Split on <br>, <br/>, <br /> (case-insensitive) — common in LLM tables.
    std::vector<std::string_view> parts;
    size_t start = 0;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '<' || i + 3 >= raw.size()) continue;
        if (std::tolower(static_cast<unsigned char>(raw[i + 1])) != 'b'
            || std::tolower(static_cast<unsigned char>(raw[i + 2])) != 'r')
            continue;
        size_t j = i + 3;
        while (j < raw.size() && raw[j] == ' ') ++j;
        if (j < raw.size() && raw[j] == '/') ++j;
        if (j < raw.size() && raw[j] == '>') {
            parts.push_back(raw.substr(start, i - start));
            start = j + 1;
            i = j;
        }
    }
    parts.push_back(raw.substr(start));

    Cell cell;
    for (size_t p = 0; p < parts.size(); ++p) {
        if (p > 0) cell.glyphs.push_back({"", 0, 0, true});
        Styled st;
        render_inline(trim(parts[p]), base, 0, st);
        const int link_base = static_cast<int>(cell.links.size());
        cell.links.insert(cell.links.end(), st.links.begin(), st.links.end());
        size_t li = 0;
        for (size_t i = 0; i < st.text.size();) {
            size_t len = 1;
            uint32_t cp = decode_utf8(st.text, i, len);
            Glyph g;
            g.flags = st.style_at(i).flags;
            if (cp == '\t') { g.s = " "; g.w = 1; }
            else { g.s = st.text.substr(i, len); g.w = cp_width(cp); }
            while (li < st.links.size() && st.links[li].end <= i) ++li;
            if (li < st.links.size() && st.links[li].start <= i)
                g.link = link_base + static_cast<int>(li);
            cell.glyphs.push_back(std::move(g));
            i += len;
        }
    }
    return cell;
}

int
natural_width(const std::vector<Glyph>& glyphs)
{
    int best = 0, cur = 0;
    for (const auto& g : glyphs) {
        if (g.brk) { best = std::max(best, cur); cur = 0; continue; }
        cur += g.w;
    }
    return std::max(best, cur);
}

using GlyphLine = std::vector<Glyph>;

std::vector<GlyphLine>
wrap_cell(const std::vector<Glyph>& glyphs, int width)
{
    std::vector<GlyphLine> lines;
    GlyphLine cur;
    int cur_w = 0;
    auto newline = [&] {
        lines.push_back(std::move(cur));
        cur.clear();
        cur_w = 0;
    };
    const Glyph* space = nullptr;
    size_t i = 0;
    while (i < glyphs.size()) {
        const Glyph& g = glyphs[i];
        if (g.brk) { newline(); space = nullptr; ++i; continue; }
        if (g.s == " ") { if (cur_w > 0) space = &g; ++i; continue; }
        size_t e = i;
        int ww = 0;
        while (e < glyphs.size() && !glyphs[e].brk && glyphs[e].s != " ") {
            ww += glyphs[e].w;
            ++e;
        }
        if (cur_w > 0 && cur_w + 1 + ww <= width) {
            Glyph sp = space ? *space : Glyph{" ", 1, 0, false};
            cur.push_back(sp);
            cur_w += 1;
        } else if (cur_w > 0) {
            newline();
        }
        for (size_t k = i; k < e; ++k) {
            if (cur_w > 0 && cur_w + glyphs[k].w > width) newline();
            cur.push_back(glyphs[k]);
            cur_w += glyphs[k].w;
        }
        space = nullptr;
        i = e;
    }
    lines.push_back(std::move(cur));
    return lines;
}

int
line_width(const GlyphLine& line)
{
    int w = 0;
    for (const auto& g : line) w += g.w;
    return w;
}

void
render_table(const std::vector<Line>& lines, size_t header, size_t end,
             const std::vector<Align>& aligns, State& st, const Options& opts,
             Styled& out)
{
    const size_t n = aligns.size();
    std::vector<std::string_view> head_raw = split_row(lines[header].s);
    head_raw.resize(n);
    std::vector<std::vector<std::string_view>> body_raw;
    for (size_t r = header + 2; r < end; ++r) {
        auto cells = split_row(lines[r].s);
        cells.resize(n);
        body_raw.push_back(std::move(cells));
    }

    // Natural column widths.
    std::vector<Cell> head(n);
    std::vector<std::vector<Cell>> body(body_raw.size(), std::vector<Cell>(n));
    std::vector<int> nat(n, 1);
    for (size_t c = 0; c < n; ++c) {
        head[c] = cell_glyphs(head_raw[c], kMono | kBold);
        nat[c] = std::max(nat[c], natural_width(head[c].glyphs));
        for (size_t r = 0; r < body_raw.size(); ++r) {
            body[r][c] = cell_glyphs(body_raw[r][c], kMono);
            nat[c] = std::max(nat[c], natural_width(body[r][c].glyphs));
        }
    }

    const int frame = 3 * static_cast<int>(n) + 1;
    const int avail = opts.max_cols - frame;
    long total = 0;
    for (int w : nat) total += w;

    std::vector<int> width = nat;
    if (total > avail) {
        if (avail < 3 * static_cast<int>(n)) {
            // Too narrow for a grid: one "Header: value" line per cell.
            auto cell_text = [](std::string_view raw) {
                std::string t;
                // <br> becomes " / " in the record form.
                size_t start = 0;
                for (size_t i = 0; i < raw.size(); ++i) {
                    if (raw[i] == '<' && i + 3 < raw.size()
                        && std::tolower(static_cast<unsigned char>(raw[i + 1])) == 'b'
                        && std::tolower(static_cast<unsigned char>(raw[i + 2])) == 'r') {
                        size_t j = i + 3;
                        while (j < raw.size() && raw[j] == ' ') ++j;
                        if (j < raw.size() && raw[j] == '/') ++j;
                        if (j < raw.size() && raw[j] == '>') {
                            t.append(raw.substr(start, i - start));
                            t.append(" / ");
                            start = j + 1;
                            i = j;
                        }
                    }
                }
                t.append(raw.substr(start));
                return t;
            };
            if (body_raw.empty()) {
                begin_line(out, st);
                for (size_t c = 0; c < n; ++c) {
                    if (c) out.add(" | ", kDim);
                    render_inline(cell_text(head_raw[c]), kBold, 0, out);
                }
                return;
            }
            for (size_t r = 0; r < body_raw.size(); ++r) {
                if (r > 0 && st.any_output) st.pending_blank = true;
                for (size_t c = 0; c < n; ++c) {
                    begin_line(out, st);
                    render_inline(cell_text(head_raw[c]), kBold, 0, out);
                    out.add(": ", kBold);
                    render_inline(cell_text(body_raw[r][c]), 0, 0, out);
                }
            }
            return;
        }
        // Water-fill: cap the widest columns at the largest c that fits.
        int maxnat = *std::max_element(nat.begin(), nat.end());
        int lo = 1, hi = maxnat;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            long sum = 0;
            for (int w : nat) sum += std::min(w, mid);
            if (sum <= avail) lo = mid; else hi = mid - 1;
        }
        long used = 0;
        for (size_t c = 0; c < n; ++c) {
            width[c] = std::min(nat[c], lo);
            used += width[c];
        }
        long left = avail - used;
        for (size_t c = 0; c < n && left > 0; ++c) {
            if (nat[c] > width[c]) { ++width[c]; --left; }
        }
    }

    const bool ascii = opts.ascii_borders;
    const std::string h = ascii ? "-" : "\xe2\x94\x80";
    const std::string v = ascii ? "|" : "\xe2\x94\x82";
    auto border = [&](const char* l, const char* m, const char* r) {
        begin_line(out, st);
        std::string s = ascii ? "+" : l;
        for (size_t c = 0; c < n; ++c) {
            s += repeat(h, width[c] + 2);
            s += c + 1 < n ? (ascii ? "+" : m) : (ascii ? "+" : r);
        }
        out.add(s, kMono | kDim);
    };
    auto row = [&](const std::vector<Cell>& cells) {
        std::vector<std::vector<GlyphLine>> wrapped(n);
        size_t height = 1;
        for (size_t c = 0; c < n; ++c) {
            wrapped[c] = wrap_cell(cells[c].glyphs, width[c]);
            height = std::max(height, wrapped[c].size());
        }
        for (size_t y = 0; y < height; ++y) {
            begin_line(out, st);
            out.add(v, kMono | kDim);
            for (size_t c = 0; c < n; ++c) {
                GlyphLine empty;
                const GlyphLine& gl = y < wrapped[c].size() ? wrapped[c][y] : empty;
                int pad = std::max(0, width[c] - line_width(gl));
                int lpad = aligns[c] == Align::Right ? pad
                         : aligns[c] == Align::Center ? pad / 2 : 0;
                out.add(std::string(1 + lpad, ' '), kMono);
                // A link wrapped over several lines becomes one span per line.
                int open = -1;
                size_t open_start = 0;
                auto close_link = [&] {
                    if (open >= 0 && out.text.size() > open_start) {
                        const LinkSpan& l = cells[c].links[open];
                        out.links.push_back({open_start, out.text.size(), l.target, l.implicit});
                    }
                    open = -1;
                };
                for (const auto& g : gl) {
                    if (g.link != open) {
                        close_link();
                        open = g.link;
                        open_start = out.text.size();
                    }
                    out.add(g.s, g.flags);
                }
                close_link();
                out.add(std::string(pad - lpad + 1, ' '), kMono);
                out.add(v, kMono | kDim);
            }
        }
        return height;
    };

    // Body rows get separators only when some row wraps onto several lines.
    bool multi = false;
    for (size_t r = 0; r < body.size() && !multi; ++r)
        for (size_t c = 0; c < n && !multi; ++c)
            multi = wrap_cell(body[r][c].glyphs, width[c]).size() > 1;

    border("\xe2\x94\x8c", "\xe2\x94\xac", "\xe2\x94\x90");    // ┌ ┬ ┐
    row(head);
    if (!body.empty())
        border("\xe2\x94\x9c", "\xe2\x94\xbc", "\xe2\x94\xa4");    // ├ ┼ ┤
    for (size_t r = 0; r < body.size(); ++r) {
        if (r > 0 && multi)
            border("\xe2\x94\x9c", "\xe2\x94\xbc", "\xe2\x94\xa4");
        row(body[r]);
    }
    border("\xe2\x94\x94", "\xe2\x94\xb4", "\xe2\x94\x98");    // └ ┴ ┘
}

// Render the unit starting at lines[i]. `consumed` receives the number of
// lines used; `complete` whether appending text can still change the result.
void
render_unit(const std::vector<Line>& lines, size_t i, State& st,
            const Options& opts, Styled& out, size_t& consumed, bool& complete)
{
    const Line& L = lines[i];
    consumed = 1;
    complete = L.nl;

    if (st.in_fence) {
        if (fence_close(L.s, st.fence_char, st.fence_len)) {
            st.in_fence = false;
            return;
        }
        std::string_view code = L.s;
        size_t strip = 0;
        while (strip < code.size() && static_cast<int>(strip) < st.fence_indent
               && code[strip] == ' ')
            ++strip;
        begin_line(out, st);
        out.add(code.substr(strip), kCodeBlock);
        return;
    }

    if (is_blank(L.s)) {
        if (st.any_output) st.pending_blank = true;
        return;
    }

    {
        char ch;
        int len, indent;
        std::string_view info;
        if (fence_open(L.s, ch, len, indent, info)) {
            st.in_fence = true;
            st.fence_char = ch;
            st.fence_len = len;
            st.fence_indent = indent;
            size_t sp = 0;
            while (sp < info.size() && !is_ws(info[sp])) ++sp;
            if (sp > 0) {
                begin_line(out, st);
                out.add(info.substr(0, sp), kCodeBlock | kDim);
            }
            return;
        }
    }

    {
        int level;
        std::string_view content;
        if (atx_heading(L.s, level, content)) {
            begin_line(out, st);
            render_inline(content, kBold, static_cast<uint8_t>(level), out);
            return;
        }
    }

    if (thematic_break(L.s)) {
        begin_line(out, st);
        int w = std::clamp(opts.max_cols, 3, 80);
        out.add(repeat(opts.ascii_borders ? "-" : "\xe2\x94\x80", w), kMono | kDim);
        return;
    }

    // A complete line always has a successor (possibly the empty, still
    // arriving last line), so a pipe line can be checked for a table here.
    if (has_pipe(L.s) && i + 1 < lines.size()) {
        std::vector<Align> aligns;
        if (delimiter_row(lines[i + 1].s, split_row(L.s).size(), &aligns)) {
            size_t j = i + 2;
            while (j < lines.size() && !is_blank(lines[j].s) && has_pipe(lines[j].s))
                ++j;
            consumed = j - i;
            // Final only once the line after the last row has arrived
            // whole (it might otherwise still become another row).
            complete = j < lines.size() && lines[j].nl;
            render_table(lines, i, j, aligns, st, opts, out);
            return;
        }
        // Not a table yet: the delimiter line might still be arriving.
        complete = complete && lines[i + 1].nl;
    }

    {
        int level;
        std::string_view content;
        if (quote_prefix(L.s, level, content)) {
            begin_line(out, st);
            out.add(repeat("\xe2\x94\x82 ", level), kDim);   // "│ "
            render_inline(content, kQuote, 0, out);
            return;
        }
    }

    {
        ListItem item;
        if (list_item(L.s, item)) {
            begin_line(out, st);
            out.add(std::string(4 * item.level, ' '));
            if (item.ordered) {
                out.add(std::string(item.marker) + " ");
            } else {
                static const char* bullets[] = {"\xe2\x80\xa2 ", "\xe2\x97\xa6 ", "\xe2\x96\xaa "};
                out.add(bullets[item.level % 3]);       // • ◦ ▪
            }
            std::string_view content = item.content;
            if (content.size() >= 3 && content[0] == '[' && content[2] == ']'
                && (content.size() == 3 || is_space(content[3]))) {
                char m = content[1];
                if (m == ' ') {
                    out.add("[ ] ", kDim);
                    content = trim(content.substr(3));
                } else if (m == 'x' || m == 'X') {
                    out.add("[\xe2\x9c\x93] ", kDim);   // [✓]
                    content = trim(content.substr(3));
                }
            }
            render_inline(content, 0, 0, out);
            return;
        }
    }

    // Paragraph line: keep meaningful indentation (list continuations).
    size_t idx = 0;
    int cols = indent_of(L.s, &idx);
    std::string_view content = L.s.substr(idx);
    while (!content.empty() && is_ws(content.back())) content.remove_suffix(1);
    if (!content.empty() && content.back() == '\\') content.remove_suffix(1);
    begin_line(out, st);
    if (cols >= 2) out.add(std::string(std::min(cols, 16), ' '));
    render_inline(content, 0, 0, out);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

Incremental
render_from(std::string_view text, size_t from, const State& st, const Options& opts)
{
    Incremental r;
    r.state = st;
    r.frozen_end = std::min(from, text.size());
    std::vector<Line> lines = split_lines(text, from);
    State cur = st;
    bool frozen_open = true;
    size_t i = 0;
    while (i < lines.size()) {
        Styled unit;
        size_t consumed = 1;
        bool complete = false;
        render_unit(lines, i, cur, opts, unit, consumed, complete);
        if (frozen_open && complete) {
            r.frozen.append(unit);
            r.state = cur;
            r.frozen_end = lines[i + consumed - 1].end;
        } else {
            frozen_open = false;
            r.tail.append(unit);
        }
        i += consumed;
    }
    return r;
}

Styled
render(std::string_view text, const Options& opts)
{
    Incremental inc = render_from(text, 0, State{}, opts);
    Styled out = std::move(inc.frozen);
    out.append(inc.tail);
    return out;
}

namespace {

std::string
percent_decode(std::string_view s)
{
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            out += static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Pop a trailing ":<digits>" off `path`; false (path untouched) if none.
bool
pop_line_number(std::string& path, int& value)
{
    size_t colon = path.rfind(':');
    if (colon == std::string::npos || colon + 1 >= path.size() || path.size() - colon > 10)
        return false;
    for (size_t i = colon + 1; i < path.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(path[i]))) return false;
    value = std::stoi(path.substr(colon + 1));
    path.resize(colon);
    return true;
}

}  // namespace

LinkTarget
resolve_link(std::string_view dest, std::string_view base_dir, std::string_view home_dir)
{
    LinkTarget r;
    std::string_view d = trim(dest);
    if (d.empty() || d.front() == '#') return r;
    if (std::any_of(d.begin(), d.end(), [](char c) { return is_ws(c); })) return r;

    if (starts_with_ci(d, "http://") || starts_with_ci(d, "https://")) {
        if (d.size() > (starts_with_ci(d, "https://") ? 8u : 7u)) {
            r.kind = LinkKind::Web;
            r.target = d;
        }
        return r;
    }
    if (starts_with_ci(d, "mailto:")) {
        if (d.size() > 7) {
            r.kind = LinkKind::Mail;
            r.target = d;
        }
        return r;
    }
    if (starts_with_ci(d, "www.")) {
        r.kind = LinkKind::Web;
        r.target = "https://" + std::string(d);
        return r;
    }

    std::string path;
    if (starts_with_ci(d, "file://")) {
        std::string_view rest = d.substr(7);
        if (starts_with_ci(rest, "localhost/")) rest.remove_prefix(9);
        if (rest.empty() || rest.front() != '/') return r;
        path = percent_decode(rest);
    } else {
        // Any other scheme is refused — except "name.ext:12", a path with a
        // line number that merely looks like one.
        size_t colon = d.find(':');
        if (colon != std::string_view::npos && colon > 0
            && std::isalpha(static_cast<unsigned char>(d[0]))) {
            std::string_view scheme = d.substr(0, colon);
            bool is_scheme = std::all_of(scheme.begin(), scheme.end(), [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-'
                    || c == '.';
            });
            std::string_view after = d.substr(colon + 1);
            bool line_suffix = !after.empty()
                && std::all_of(after.begin(), after.end(), [](char c) {
                       return std::isdigit(static_cast<unsigned char>(c)) || c == ':';
                   });
            if (is_scheme && !line_suffix) return r;
        }
        path = percent_decode(d);
    }
    if (path.find('\0') != std::string::npos) return r;

    size_t hash = path.find('#');
    if (hash != std::string::npos) {
        std::string frag = path.substr(hash + 1);
        path.resize(hash);
        if (frag.size() > 1 && frag.size() <= 10 && (frag[0] == 'L' || frag[0] == 'l')
            && std::all_of(frag.begin() + 1, frag.end(),
                           [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            r.line = std::stoi(frag.substr(1));
    }
    int a = 0, b = 0;
    if (pop_line_number(path, a))
        r.line = pop_line_number(path, b) ? b : a;   // path:line:col
    if (path.empty()) return r;

    if (path.rfind("~/", 0) == 0 || path == "~") {
        if (home_dir.empty()) return r;
        path = std::string(home_dir) + path.substr(1);
    } else if (path.front() != '/') {
        if (base_dir.empty()) return r;
        while (path.rfind("./", 0) == 0) path.erase(0, 2);
        std::string base(base_dir);
        while (base.size() > 1 && base.back() == '/') base.pop_back();
        path = base + (base == "/" ? "" : "/") + path;
    }
    r.kind = LinkKind::File;
    r.target = std::move(path);
    return r;
}

bool
width_dependent(std::string_view text)
{
    std::vector<Line> lines = split_lines(text, 0);
    bool in_fence = false;
    char fch = 0;
    int flen = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string_view s = lines[i].s;
        if (in_fence) {
            if (fence_close(s, fch, flen)) in_fence = false;
            continue;
        }
        int indent;
        std::string_view info;
        if (fence_open(s, fch, flen, indent, info)) { in_fence = true; continue; }
        if (thematic_break(s)) return true;
        if (has_pipe(s) && i + 1 < lines.size()
            && delimiter_row(lines[i + 1].s, split_row(s).size(), nullptr))
            return true;
    }
    return false;
}

}  // namespace haicode::md
