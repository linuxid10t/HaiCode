#pragma once
// Markdown -> styled text for the GUI transcript and plan review.
//
// Pure C++ (no BeAPI): the renderer turns markdown source into display text
// plus style runs carrying SEMANTIC flags (bold, inline code, table grid,
// ...). The frontend maps flags to fonts and colors. Covered subset: ATX
// headings, fenced code, thematic breaks, GFM tables, block quotes,
// bullet/ordered/task lists, and inline code / bold / italic / strikethrough
// / links / images / autolinks / backslash escapes. Source line breaks are
// kept as line breaks (chat style, not paragraph reflow); runs of blank
// lines collapse to one, and leading/trailing blank lines are dropped.
//
// Rendering is a left fold over "units" (one line, or one whole table) with
// an explicit State, so render(text) == render_from(text, 0, State{}) and a
// streaming caller can freeze the units that can no longer change and only
// re-render the open tail: frozen + tail always equals render() of the same
// text, byte for byte and run for run.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace haicode::md {

enum StyleFlag : uint16_t {
    kBold      = 1u << 0,
    kItalic    = 1u << 1,
    kStrike    = 1u << 2,
    kCode      = 1u << 3,   // inline code span
    kCodeBlock = 1u << 4,   // fenced code block line
    kLink      = 1u << 5,   // link text
    kDim       = 1u << 6,   // decoration: table borders, rules, URLs, fence labels
    kMono      = 1u << 7,   // fixed-width grid (tables, rules)
    kQuote     = 1u << 8,   // block quote content
};

struct Run {
    size_t   offset;    // byte offset into Styled::text where this run starts
    uint16_t flags;
    uint8_t  heading;   // 0 = none, 1..6 = heading level
};

// Display text plus contiguous style runs (each run lasts until the next
// run's offset). Adjacent identical styles are always merged.
struct Styled {
    std::string      text;
    std::vector<Run> runs;

    void add(std::string_view s, uint16_t flags = 0, uint8_t heading = 0);
    void append(const Styled& other);
    // Style of the byte at `pos` (pos < text.size()).
    Run style_at(size_t pos) const;
    // Copy of [from, end) with runs rebased to 0.
    Styled slice(size_t from) const;
    bool operator==(const Styled& o) const;
};

struct Options {
    int  max_cols      = 80;     // fixed-width columns available for tables/rules
    bool ascii_borders = false;  // use +-| instead of box-drawing characters
};

// Fold state carried between units.
struct State {
    bool any_output    = false;  // something has been emitted (next line leads with "\n")
    bool pending_blank = false;  // blank source line(s) seen since the last output line
    bool in_fence      = false;
    char fence_char    = 0;
    int  fence_len     = 0;
    int  fence_indent  = 0;
    bool operator==(const State& o) const = default;
};

Styled render(std::string_view text, const Options& opts);

struct Incremental {
    Styled frozen;           // rendering of the complete units in [from, frozen_end)
    size_t frozen_end = 0;   // source offset (a line start) where the open tail begins
    State  state;            // fold state at frozen_end
    Styled tail;             // rendering of [frozen_end, end) from `state`
};

// Render text[from..] (from must be a line start) starting from `st`. Units
// whose rendering can no longer change when more text is appended go to
// `frozen`; the rest (the incomplete last line, a table still receiving rows,
// a line that may yet turn out to be a table header) goes to `tail`.
Incremental render_from(std::string_view text, size_t from, const State& st,
                        const Options& opts);

// True when the rendering depends on Options::max_cols (contains a table or
// a thematic break outside code fences), i.e. it must be re-rendered when the
// view width changes.
bool width_dependent(std::string_view text);

// Length of the longest common prefix of a and b over which both the text
// and the style of every byte agree, backed off to a UTF-8 character
// boundary.
size_t common_prefix(const Styled& a, const Styled& b);

// Terminal-style display width of UTF-8 text in fixed-width cells (East
// Asian wide characters count 2, combining marks 0).
size_t display_width(std::string_view utf8);

}  // namespace haicode::md
