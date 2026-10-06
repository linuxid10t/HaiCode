#pragma once
// BeAPI side of the markdown renderer (haicode/markdown.h): maps the
// semantic style flags to fonts and colors, and a read-only BTextView that
// displays a markdown document (used by the plan review window; ChatView
// renders its entries through the same mapping).

#include <Font.h>
#include <GraphicsDefs.h>
#include <TextView.h>

#include <haicode/markdown.h>

#include <memory>
#include <string>

struct MarkdownPalette {
    rgb_color base;        // ordinary text
    rgb_color code;        // inline code spans
    rgb_color code_block;  // fenced code lines
    rgb_color link;
    rgb_color dim;         // table borders, rules, URLs, fence labels
    rgb_color quote;
};

// Fixed accent colors around the entry's own text color.
MarkdownPalette MarkdownPaletteFor(rgb_color base);

// flags 0 is exactly be_plain_font and kBold exactly be_bold_font, so text
// without markdown renders as it always has.
BFont MarkdownFont(uint16 flags, uint8 heading);
rgb_color MarkdownColor(uint16 flags, const MarkdownPalette& palette);

// be_fixed_font columns that fit the view's text rect (tables and rules are
// laid out to this width, so they never word-wrap).
int MarkdownColumns(const BTextView* view);

// True when be_fixed_font itself lacks the box-drawing glyphs: fallback
// glyphs would not share the grid's advance width, so tables use +-| then.
bool MarkdownAsciiBorders();

struct TextRunArrayDeleter {
    void operator()(text_run_array* a) const { BTextView::FreeRunArray(a); }
};
using TextRunArrayPtr = std::unique_ptr<text_run_array, TextRunArrayDeleter>;

TextRunArrayPtr MarkdownRunArray(const haicode::md::Styled& styled,
                                 const MarkdownPalette& palette);

// Read-only text view showing rendered markdown; re-renders when a width
// change alters the column count of a document with tables or rules.
class MarkdownTextView : public BTextView {
public:
    MarkdownTextView(const char* name, const MarkdownPalette& palette);

    void SetMarkdown(const std::string& markdown);
    void FrameResized(float width, float height) override;

private:
    void _Render();

    std::string     markdown_;
    MarkdownPalette palette_;
    int             cols_ = 0;
};
