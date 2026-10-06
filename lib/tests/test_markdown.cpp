// Markdown renderer (haicode/markdown.h): block + inline subset, GFM tables
// sized to the available columns, and the streaming contract — at every
// prefix of a document, the frozen units accumulated so far plus the open
// tail equal a from-scratch render, byte for byte and run for run.

#include <haicode/markdown.h>
#include "test_check.h"

#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace haicode::md;

namespace {

Options opts(int cols = 80, bool ascii = false)
{
    Options o;
    o.max_cols = cols;
    o.ascii_borders = ascii;
    return o;
}

// Flags of the first byte of `needle` in the rendered text.
uint16_t flags_of(const Styled& s, const std::string& needle)
{
    size_t p = s.text.find(needle);
    TEST_REQUIRE(p != std::string::npos, "needle not rendered: " + needle + " in: " + s.text);
    return s.style_at(p).flags;
}

std::vector<std::string> split(const std::string& s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string line;
    while (std::getline(ss, line)) out.push_back(line);
    return out;
}

std::string repeat_check(const std::string& s, int n)
{
    std::string out;
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

void test_inline()
{
    Styled s = render("Use **bold**, *it*, `code`, ~~gone~~ and ***both***.", opts());
    TEST_REQUIRE(s.text == "Use bold, it, code, gone and both.", "inline markers stripped: " + s.text);
    TEST_REQUIRE(flags_of(s, "bold") == kBold, "bold flag");
    TEST_REQUIRE(flags_of(s, "it,") == kItalic, "italic flag");
    TEST_REQUIRE(flags_of(s, "code") == kCode, "code flag");
    TEST_REQUIRE(flags_of(s, "gone") == kStrike, "strike flag");
    TEST_REQUIRE(flags_of(s, "both") == (kBold | kItalic), "bold+italic flag");
    TEST_REQUIRE(flags_of(s, "Use") == 0, "plain flag");

    // Nesting and code spans protecting their content.
    s = render("**a *b* c** and `**not bold**`", opts());
    TEST_REQUIRE(s.text == "a b c and **not bold**", "nested/code: " + s.text);
    TEST_REQUIRE(flags_of(s, "b c") == (kBold | kItalic), "nested italic inside bold");
    TEST_REQUIRE(flags_of(s, "**not") == kCode, "code span content literal");

    // snake_case and lone markers stay literal; escapes drop the backslash.
    s = render("snake_case_name, 2 * 3 * 4, \\*literal\\*, a ~ b", opts());
    TEST_REQUIRE(s.text == "snake_case_name, 2 * 3 * 4, *literal*, a ~ b", "literals: " + s.text);
    TEST_REQUIRE(s.runs.size() == 1 && s.runs[0].flags == 0, "no styles on literals");

    // Unclosed markers stay literal (as when a stream is mid-span).
    s = render("**unclosed and `open", opts());
    TEST_REQUIRE(s.text == "**unclosed and `open", "unclosed literal: " + s.text);

    // Links: label styled, URL kept dim (not clickable); same-as-label URL and
    // anchors omitted. Images and autolinks.
    s = render("See [the docs](https://x.org/d \"t\") or [https://y.org](https://y.org) "
               "or [sec](#a). ![diagram](d.png) <https://z.org>", opts());
    TEST_REQUIRE(s.text == "See the docs (https://x.org/d) or https://y.org or sec. "
                           "[image: diagram] https://z.org", "links: " + s.text);
    TEST_REQUIRE(flags_of(s, "the docs") == kLink, "link flag");
    TEST_REQUIRE(flags_of(s, " (https://x.org/d)") == kDim, "url dim");
    TEST_REQUIRE(flags_of(s, "[image") == kDim, "image dim");
    TEST_REQUIRE(flags_of(s, "https://z.org") == kLink, "autolink");

    // Bold link text keeps both flags.
    s = render("**[x](u)**", opts());
    TEST_REQUIRE(flags_of(s, "x") == (kBold | kLink), "bold link");
}

void test_blocks()
{
    Styled s = render("# Title ##\n## Sub\n#### Small\n#nospace", opts());
    auto lines = split(s.text);
    TEST_REQUIRE(lines.size() == 4, "heading line count");
    TEST_REQUIRE(lines[0] == "Title" && lines[1] == "Sub" && lines[2] == "Small"
                 && lines[3] == "#nospace", "heading text: " + s.text);
    TEST_REQUIRE(s.style_at(0).heading == 1 && s.style_at(0).flags == kBold, "h1 style");
    TEST_REQUIRE(s.style_at(s.text.find("Sub")).heading == 2, "h2 style");
    TEST_REQUIRE(s.style_at(s.text.find("Small")).heading == 4, "h4 style");
    TEST_REQUIRE(s.style_at(s.text.find("#nospace")).heading == 0, "not a heading");

    // Lists: bullets per level, ordered markers kept, task boxes.
    s = render("- one\n  - two\n    - three\n1. first\n2) second\n- [ ] todo\n- [x] done\n* star\n+ plus", opts());
    lines = split(s.text);
    TEST_REQUIRE(lines.size() == 9, "list line count: " + s.text);
    TEST_REQUIRE(lines[0] == "\xe2\x80\xa2 one", "level 0 bullet: " + lines[0]);
    TEST_REQUIRE(lines[1] == "    \xe2\x97\xa6 two", "level 1 bullet: " + lines[1]);
    TEST_REQUIRE(lines[2] == "        \xe2\x96\xaa three", "level 2 bullet: " + lines[2]);
    TEST_REQUIRE(lines[3] == "1. first" && lines[4] == "2) second", "ordered");
    TEST_REQUIRE(lines[5] == "\xe2\x80\xa2 [ ] todo", "task open: " + lines[5]);
    TEST_REQUIRE(lines[6] == "\xe2\x80\xa2 [\xe2\x9c\x93] done", "task done: " + lines[6]);
    TEST_REQUIRE(lines[7] == "\xe2\x80\xa2 star" && lines[8] == "\xe2\x80\xa2 plus", "other bullets");

    // Thematic break vs list: "* * *" is a rule; "**x**" is not a list.
    s = render("* * *\n**x** y", opts(10));
    lines = split(s.text);
    TEST_REQUIRE(lines[0] == repeat_check("\xe2\x94\x80", 10), "rule spans max_cols");
    TEST_REQUIRE(flags_of(s, "\xe2\x94\x80") == (kMono | kDim), "rule style");
    TEST_REQUIRE(lines[1] == "x y", "bold paragraph");
    TEST_REQUIRE(split(render("---", opts(200)).text)[0].size() == 80 * 3, "rule capped at 80");
    TEST_REQUIRE(render("---", opts(10, true)).text == "----------", "ascii rule");

    // Quotes.
    s = render("> quoted *it*\n>> deeper", opts());
    lines = split(s.text);
    TEST_REQUIRE(lines[0] == "\xe2\x94\x82 quoted it", "quote: " + lines[0]);
    TEST_REQUIRE(lines[1] == "\xe2\x94\x82 \xe2\x94\x82 deeper", "nested quote: " + lines[1]);
    TEST_REQUIRE(flags_of(s, "quoted") == kQuote, "quote flag");
    TEST_REQUIRE(flags_of(s, "it") == (kQuote | kItalic), "italic in quote");

    // Blank lines: leading/trailing dropped, runs collapsed to one.
    s = render("\n\n a\n\n\n\nb  \n\n", opts());
    TEST_REQUIRE(s.text == "a\n\nb", "blank handling: [" + s.text + "]");
    // Indentation of 2+ columns is kept (list continuations).
    TEST_REQUIRE(render("- item\n  more", opts()).text == "\xe2\x80\xa2 item\n  more", "continuation");
}

void test_code_fences()
{
    Styled s = render("Before\n```cpp\nint *a = **b; // # not heading\n\n  - not list\n```\nAfter", opts());
    auto lines = split(s.text);
    TEST_REQUIRE(lines.size() == 6, "fence line count: " + s.text);
    TEST_REQUIRE(lines[1] == "cpp", "language label");
    TEST_REQUIRE(flags_of(s, "cpp") == (kCodeBlock | kDim), "label style");
    TEST_REQUIRE(lines[2] == "int *a = **b; // # not heading", "code verbatim");
    TEST_REQUIRE(lines[3].empty(), "blank code line kept");
    TEST_REQUIRE(lines[4] == "  - not list", "code indentation kept");
    TEST_REQUIRE(flags_of(s, "int") == kCodeBlock, "code block flag");
    TEST_REQUIRE(lines[5] == "After" && flags_of(s, "After") == 0, "fence closed");

    // Indented fence (inside a list) strips the fence indent from code lines;
    // a longer closing fence closes; tildes work; no label without info.
    s = render("  ```\n  x\n    y\n  ````\n~~~\nz\n~~~", opts());
    TEST_REQUIRE(s.text == "x\n  y\nz", "indented/tilde fences: " + s.text);

    // Unterminated fence: the rest is code.
    s = render("```\n# still code", opts());
    TEST_REQUIRE(s.text == "# still code" && flags_of(s, "#") == kCodeBlock, "unterminated");
}

void test_tables()
{
    const std::string md =
        "| Name | Qty | Note |\n"
        "|:-----|----:|:----:|\n"
        "| **apple** | 3 | `ok` |\n"
        "| pear | 12 | a \\| b |\n";
    Styled s = render(md, opts());
    auto lines = split(s.text);
    TEST_REQUIRE(lines.size() == 6, "table line count: " + s.text);
    TEST_REQUIRE(lines[0] == "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                 "\xe2\x94\xac\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                 "\xe2\x94\xac\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                 "\xe2\x94\x90", "top border: " + lines[0]);
    const std::string V = "\xe2\x94\x82";
    TEST_REQUIRE(lines[1] == V + " Name  " + V + " Qty " + V + " Note  " + V, "header row: " + lines[1]);
    TEST_REQUIRE(lines[3] == V + " apple " + V + "   3 " + V + "  ok   " + V, "aligned row: " + lines[3]);
    TEST_REQUIRE(lines[4] == V + " pear  " + V + "  12 " + V + " a | b " + V, "escaped pipe: " + lines[4]);
    for (const auto& l : lines)
        TEST_REQUIRE(display_width(l) == display_width(lines[0]), "rows equal width: " + l);
    TEST_REQUIRE(flags_of(s, "Name") == (kMono | kBold), "header style");
    TEST_REQUIRE(flags_of(s, "apple") == (kMono | kBold), "bold cell");
    TEST_REQUIRE(flags_of(s, "ok") == (kMono | kCode), "code cell");
    TEST_REQUIRE(flags_of(s, "pear") == kMono, "plain cell");
    TEST_REQUIRE(flags_of(s, V) == (kMono | kDim), "border style");
    TEST_REQUIRE(width_dependent(md), "table is width dependent");

    // ASCII borders.
    s = render("a|b\n-|-\n1|2", opts(80, true));
    TEST_REQUIRE(s.text == "+---+---+\n| a | b |\n+---+---+\n| 1 | 2 |\n+---+---+", "ascii table: " + s.text);

    // Short rows padded, long rows truncated; header-only table.
    s = render("|a|b|\n|-|-|\n|1|\n|1|2|3|", opts(80, true));
    TEST_REQUIRE(split(s.text)[3] == "| 1 |   |" && split(s.text)[4] == "| 1 | 2 |", "pad/truncate: " + s.text);
    TEST_REQUIRE(render("|a|\n|-|", opts(80, true)).text == "+---+\n| a |\n+---+",
                 "header only: " + render("|a|\n|-|", opts(80, true)).text);

    // Narrow: wide columns wrap inside their cells; every line fits exactly.
    const std::string wide =
        "| id | description |\n|---|---|\n"
        "| 1 | the quick brown fox jumps over the lazy dog again and again |\n"
        "| 2 | short |\n";
    s = render(wide, opts(30, true));
    lines = split(s.text);
    for (const auto& l : lines)
        TEST_REQUIRE(display_width(l) == 30, "wrapped table fills max_cols: [" + l + "]");
    TEST_REQUIRE(s.text.find("| 1  | the quick brown fox   |") != std::string::npos,
                 "first wrapped line: " + s.text);
    TEST_REQUIRE(lines.size() > 6, "wrapped rows add lines");
    // Multi-line rows get separators between body rows.
    int seps = 0;
    for (const auto& l : lines) if (l.rfind("+", 0) == 0) ++seps;
    TEST_REQUIRE(seps == 4, "top/head/between/bottom separators: " + s.text);

    // <br> breaks inside a cell.
    s = render("|a|\n|-|\n|x<br>yy<br/>z|", opts(80, true));
    TEST_REQUIRE(s.text == "+----+\n| a  |\n+----+\n| x  |\n| yy |\n| z  |\n+----+", "br: " + s.text);

    // Too narrow for a grid: record layout.
    s = render("| k | v |\n|---|---|\n| one | **1** |\n| two | 2 |", opts(8));
    TEST_REQUIRE(s.text == "k: one\nv: 1\n\nk: two\nv: 2", "record layout: " + s.text);
    TEST_REQUIRE(flags_of(s, "k:") == kBold && flags_of(s, "1") == kBold, "record styles");

    // Not a table: count mismatch, missing delimiter, pipes in code.
    TEST_REQUIRE(render("a | b | c\n--|--", opts()).text == "a | b | c\n--|--", "count mismatch");
    TEST_REQUIRE(render("a | b\nc | d", opts()).text == "a | b\nc | d", "no delimiter");
    TEST_REQUIRE(!width_dependent("```\n| a | b |\n|---|---|\n---\n```"), "fenced table/rule ignored");
    TEST_REQUIRE(!width_dependent("plain | text"), "plain not width dependent");
    TEST_REQUIRE(width_dependent("x\n***\ny"), "rule width dependent");

    // Wide characters count two cells.
    TEST_REQUIRE(display_width("a\xe4\xb8\xad") == 3, "CJK width");
    TEST_REQUIRE(display_width("e\xcc\x81") == 1, "combining width");
    s = render("|\xe4\xb8\xad|x|\n|-|-|", opts(80, true));
    TEST_REQUIRE(split(s.text)[1] == "| \xe4\xb8\xad | x |", "wide cell: " + s.text);
}

// Text covered by each link span, paired with its target.
std::vector<std::pair<std::string, std::string>> links_of(const Styled& s)
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& l : s.links) {
        TEST_REQUIRE(l.start < l.end && l.end <= s.text.size(), "link span in range");
        out.push_back({s.text.substr(l.start, l.end - l.start), l.target});
    }
    return out;
}

void test_links()
{
    using P = std::pair<std::string, std::string>;
    Styled s = render("See [the docs](https://x.org/d \"t\"), <https://z.org>, "
                      "https://bare.org/a_(b). and ![shot](/tmp/s.png) or [sec](#a)", opts());
    auto links = links_of(s);
    TEST_REQUIRE(links.size() == 5, "five links: " + std::to_string(links.size()));
    TEST_REQUIRE(links[0] == P("the docs (https://x.org/d)", "https://x.org/d"), "md link covers label + url");
    TEST_REQUIRE(links[1] == P("https://z.org", "https://z.org"), "autolink");
    TEST_REQUIRE(links[2] == P("https://bare.org/a_(b)", "https://bare.org/a_(b)"),
                 "bare url keeps balanced paren, drops trailing dot: " + links[2].first);
    TEST_REQUIRE(links[3] == P("[image: shot]", "/tmp/s.png"), "image links its source");
    TEST_REQUIRE(links[4] == P("sec", "#a"), "anchor link recorded (resolves to nothing)");
    TEST_REQUIRE(flags_of(s, "https://bare") == kLink, "bare url styled as link");
    for (const auto& l : s.links) TEST_REQUIRE(!l.implicit, "explicit links");

    // Unbalanced paren / trailing punctuation; no bare link mid-word.
    s = render("(see https://a.org/x) and xhttps://no.org", opts());
    TEST_REQUIRE(links_of(s).size() == 1 && links_of(s)[0].second == "https://a.org/x",
                 "unbalanced paren trimmed");

    // Code spans that look like paths are implicit candidates; others aren't.
    s = render("Edit `lib/src/x.cpp`, `README.md:12`, `~/n.txt`, `/tmp/a.png`, "
               "`std::vector`, `x = 1`, `1.5`, `foo()`, `main`", opts());
    links = links_of(s);
    TEST_REQUIRE(links.size() == 4, "path-like code spans only: " + std::to_string(links.size()));
    TEST_REQUIRE(links[0] == P("lib/src/x.cpp", "lib/src/x.cpp") && links[1].second == "README.md:12"
                 && links[2].second == "~/n.txt" && links[3].second == "/tmp/a.png", "code path targets");
    for (const auto& l : s.links) TEST_REQUIRE(l.implicit, "code spans are implicit");
    TEST_REQUIRE(flags_of(s, "lib/src") == kCode, "code style kept on implicit links");

    // Nothing nests: a code span or URL inside a link label adds no span.
    s = render("[`a/b.c` and https://x.org](https://y.org) **[bold](b.md)**", opts());
    links = links_of(s);
    TEST_REQUIRE(links.size() == 2 && links[0].second == "https://y.org"
                 && links[1] == P("bold (b.md)", "b.md"), "no nested spans");

    // Links inside table cells keep their spans through padding and wrapping.
    s = render("| file | note |\n|---|---|\n| [x](a.md) | see `lib/b.cpp` now |", opts(80, true));
    links = links_of(s);
    TEST_REQUIRE(links.size() == 2 && links[0] == P("x (a.md)", "a.md")
                 && links[1] == P("lib/b.cpp", "lib/b.cpp") && links[1].first.size() == 9,
                 "table cell links: " + s.text);
    s = render("|a|\n|-|\n|[one two three four](https://w.org)|", opts(14, true));
    links = links_of(s);
    TEST_REQUIRE(links.size() >= 2, "wrapped link split per line: " + s.text);
    for (const auto& l : links) TEST_REQUIRE(l.second == "https://w.org", "wrapped link target");
    // Record layout keeps links.
    s = render("| k | v |\n|---|---|\n| [x](https://r.org) | 1 |", opts(8));
    TEST_REQUIRE(links_of(s).size() == 1 && links_of(s)[0].second == "https://r.org", "record layout link");

    // append/slice carry links with rebased offsets.
    Styled a = render("x [l](https://a.org)", opts());
    Styled b = a;
    b.append(a);
    TEST_REQUIRE(b.links.size() == 2 && b.links[1].start == a.links[0].start + a.text.size(),
                 "append rebases links");
    Styled sl = a.slice(4);
    TEST_REQUIRE(sl.links.size() == 1 && sl.links[0].start == 0
                 && sl.links[0].end == a.links[0].end - 4, "slice clips links");
}

void test_resolve_link()
{
    auto r = [](const char* d) { return resolve_link(d, "/proj/", "/home/u"); };
    TEST_REQUIRE(r("https://x.org/a").kind == LinkKind::Web && r("https://x.org/a").target == "https://x.org/a", "https");
    TEST_REQUIRE(r("HTTP://x.org").kind == LinkKind::Web, "scheme case-insensitive");
    TEST_REQUIRE(r("www.x.org").kind == LinkKind::Web && r("www.x.org").target == "https://www.x.org", "www");
    TEST_REQUIRE(r("mailto:a@b.c").kind == LinkKind::Mail, "mailto");
    for (const char* bad : {"javascript:alert(1)", "data:text/html,x", "x-vnd.foo:bar", "ftp://x",
                            "#anchor", "", "https://", "a b.txt", "file://relative/x", "%00.txt"})
        TEST_REQUIRE(r(bad).kind == LinkKind::None, std::string("refused: ") + bad);

    auto f = r("lib/x.cpp");
    TEST_REQUIRE(f.kind == LinkKind::File && f.target == "/proj/lib/x.cpp" && f.line == 0, "relative: " + f.target);
    TEST_REQUIRE(r("./a.md").target == "/proj/a.md", "dot-slash");
    TEST_REQUIRE(r("../up.md").target == "/proj/../up.md", "parent kept for the OS to resolve");
    TEST_REQUIRE(r("/abs/p.png").target == "/abs/p.png", "absolute");
    TEST_REQUIRE(r("~/n.txt").target == "/home/u/n.txt", "home");
    TEST_REQUIRE(r("file:///tmp/a%20b.png").target == "/tmp/a b.png", "file url decoded");
    TEST_REQUIRE(r("file://localhost/tmp/x").target == "/tmp/x", "file://localhost");
    TEST_REQUIRE(r("my%20file.md").target == "/proj/my file.md", "relative decoded");
    f = r("README.md:12");
    TEST_REQUIRE(f.kind == LinkKind::File && f.target == "/proj/README.md" && f.line == 12, "name:line");
    f = r("src/a.cpp:40:7");
    TEST_REQUIRE(f.target == "/proj/src/a.cpp" && f.line == 40, "path:line:col");
    f = r("src/a.cpp#L9");
    TEST_REQUIRE(f.target == "/proj/src/a.cpp" && f.line == 9, "#L line");
    TEST_REQUIRE(r("src/a.cpp#section").line == 0 && r("src/a.cpp#section").target == "/proj/src/a.cpp", "other fragment dropped");
    TEST_REQUIRE(resolve_link("a.md", "", "/h").kind == LinkKind::None, "relative without base");
    TEST_REQUIRE(resolve_link("~/a", "/p", "").kind == LinkKind::None, "home without HOME");
    TEST_REQUIRE(resolve_link("a.md", "/", "").target == "/a.md", "root base");
}

void test_common_prefix()
{
    Styled a, b;
    a.add("abc"); a.add("def", kBold);
    b.add("abc"); b.add("de", kBold); b.add("x");
    TEST_REQUIRE(common_prefix(a, b) == 5, "text divergence");
    Styled c;
    c.add("abcd"); c.add("ef", kBold);
    TEST_REQUIRE(common_prefix(a, c) == 3, "style divergence");
    // "é" (c3 a9) vs "è" (c3 a8): never split the sequence.
    Styled d, e;
    d.add("x\xc3\xa9");
    e.add("x\xc3\xa8");
    TEST_REQUIRE(common_prefix(d, e) == 1, "utf-8 backoff");
    TEST_REQUIRE(common_prefix(a, a) == a.text.size(), "identical");

    Styled sl = a.slice(2);
    TEST_REQUIRE(sl.text == "cdef" && sl.runs.size() == 2 && sl.runs[1].offset == 1
                 && sl.runs[1].flags == kBold, "slice rebases runs");
}

// Stream `doc` in the given chunk sizes the way the GUI does: keep the frozen
// prefix, re-render only the tail from the saved state.
void stream_check(const std::string& doc, const std::vector<size_t>& chunks,
                  const Options& o, const std::string& label)
{
    std::string text;
    Styled frozen_acc;
    size_t frozen_end = 0;
    State state;
    size_t pos = 0, ci = 0;
    while (pos < doc.size()) {
        size_t n = std::min(chunks[ci++ % chunks.size()], doc.size() - pos);
        text += doc.substr(pos, n);
        pos += n;
        Incremental inc = render_from(text, frozen_end, state, o);
        TEST_REQUIRE(inc.frozen_end >= frozen_end, label + ": frozen_end moved back");
        TEST_REQUIRE(inc.frozen_end == 0 || text[inc.frozen_end - 1] == '\n',
                     label + ": frozen_end not at a line start");
        frozen_acc.append(inc.frozen);
        frozen_end = inc.frozen_end;
        state = inc.state;
        Styled shown = frozen_acc;
        shown.append(inc.tail);
        Styled fresh = render(text, o);
        if (!(shown == fresh)) {
            std::cerr << label << " at " << text.size() << " bytes\n--- shown ---\n"
                      << shown.text << "\n--- fresh ---\n" << fresh.text << "\n";
        }
        TEST_REQUIRE(shown == fresh, label + ": streamed rendering diverged");
    }
}

void test_streaming()
{
    const std::vector<std::string> docs = {
        "# Plan\n\nSome **bold** text and `code`.\n\n- item one\n  - nested *it*\n1. first\n\n"
        "| a | b |\n|---|:-:|\n| 1 | 2 |\n| longer cell | x |\n\nAfter the table.\n\n"
        "```python\ndef f(x):\n    return x * 2\n```\n\n---\n> quote\nEnd **done**",
        "Text with a | pipe\nand more | pipes\n| h1 | h2 |\n| -- | -- |\n| c | d |\nnext para\n",
        "```\nunterminated\n| a | b |\n|---|---|\n",
        "\n\n\nleading blanks\n\n\n\ntrailing\n\n\n",
        "| x |\n|---|\n| 1 |\n\n| y | z |\n|---|---|\n| 2 | 3 |",
        "- [ ] a\n- [x] b\n* * *\n___\nsnake_case and **bold _it_**\n\xe4\xb8\xad\xe6\x96\x87 text \xc3\xa9\n",
        "Links: [docs](https://x.org/a) and https://bare.org/p(1). See `lib/a.cpp:3`, ![s](/tmp/s.png)\n"
        "| f | u |\n|---|---|\n| [x](a.md) | https://t.org long words to wrap here |\n<https://end.org>",
    };
    std::mt19937 rng(1234);
    for (size_t d = 0; d < docs.size(); ++d) {
        std::string label = "doc" + std::to_string(d);
        stream_check(docs[d], {1}, opts(), label + "/bytewise");
        stream_check(docs[d], {1}, opts(24, true), label + "/narrow");
        stream_check(docs[d], {3, 7, 1, 15}, opts(), label + "/mixed");
        for (int trial = 0; trial < 20; ++trial) {
            std::vector<size_t> chunks;
            for (int k = 0; k < 8; ++k) chunks.push_back(1 + rng() % 12);
            stream_check(docs[d], chunks, opts(40), label + "/random");
        }
    }
}

}  // namespace

int main()
{
    test_inline();
    test_blocks();
    test_code_fences();
    test_tables();
    test_links();
    test_resolve_link();
    test_common_prefix();
    test_streaming();
    std::cout << "test_markdown: all tests passed\n";
    return 0;
}
