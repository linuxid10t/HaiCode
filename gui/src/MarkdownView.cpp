#include "MarkdownView.h"

#include <ScrollBar.h>

#include <algorithm>

using namespace haicode::md;

MarkdownPalette
MarkdownPaletteFor(rgb_color base)
{
    MarkdownPalette p;
    p.base       = base;
    p.code       = { 160,  50, 100, 255 };
    p.code_block = {  70,  70,  95, 255 };
    p.link       = {  30,  90, 200, 255 };
    p.dim        = { 150, 150, 150, 255 };
    p.quote      = { 105, 105, 105, 255 };
    return p;
}

BFont
MarkdownFont(uint16 flags, uint8 heading)
{
    if (heading == 0 && flags == 0) return *be_plain_font;
    if (heading == 0 && flags == kBold) return *be_bold_font;

    bool mono = (flags & (kCode | kCodeBlock | kMono)) != 0;
    BFont font(mono ? *be_fixed_font : *be_plain_font);
    uint16 face = 0;
    if ((flags & kBold) || heading > 0) face |= B_BOLD_FACE;
    if (flags & (kItalic | kQuote)) face |= B_ITALIC_FACE;
    if (flags & kLink) face |= B_UNDERSCORE_FACE;
    if (flags & kStrike) face |= B_STRIKEOUT_FACE;
    if (face == 0) face = B_REGULAR_FACE;
    font.SetFace(face);
    if (heading > 0 && heading <= 3) {
        static const float kScale[] = { 1.5f, 1.3f, 1.15f };
        font.SetSize(font.Size() * kScale[heading - 1]);
    }
    return font;
}

rgb_color
MarkdownColor(uint16 flags, const MarkdownPalette& palette)
{
    if (flags & kDim) return palette.dim;
    if (flags & kCode) return palette.code;
    if (flags & kCodeBlock) return palette.code_block;
    if (flags & kLink) return palette.link;
    if (flags & kQuote) return palette.quote;
    return palette.base;
}

int
MarkdownColumns(const BTextView* view)
{
    BFont fixed(*be_fixed_font);
    float cell = fixed.StringWidth("0");
    if (cell <= 0) return 80;
    // One column of slack for rounding, so a full-width grid line never
    // wraps onto a second line.
    // The bounds cap a text rect that has not caught up with a shrink yet.
    float width = view->TextRect().Width();
    float bounds = view->Bounds().Width() - 8;
    if (bounds > 0) width = std::min(width, bounds);
    int cols = (int)(width / cell) - 1;
    return std::max(cols, 12);
}

bool
MarkdownAsciiBorders()
{
    // GetHasGlyphs reads UTF-8 and counts characters.
    static const char kBox[] = "\xe2\x94\x80\xe2\x94\x82\xe2\x94\x8c\xe2\x94\xbc";  // ─│┌┼
    bool has[4] = { false, false, false, false };
    BFont fixed(*be_fixed_font);
    fixed.GetHasGlyphs(kBox, 4, has, false);
    return !(has[0] && has[1] && has[2] && has[3]);
}

TextRunArrayPtr
MarkdownRunArray(const Styled& styled, const MarkdownPalette& palette)
{
    if (styled.runs.empty()) return nullptr;
    TextRunArrayPtr arr(BTextView::AllocRunArray((int32)styled.runs.size()));
    if (!arr) return nullptr;
    for (size_t i = 0; i < styled.runs.size(); ++i) {
        const Run& run = styled.runs[i];
        arr->runs[i].offset = (int32)run.offset;
        arr->runs[i].font   = MarkdownFont(run.flags, run.heading);
        arr->runs[i].color  = MarkdownColor(run.flags, palette);
    }
    return arr;
}

// ---------------------------------------------------------------------------
// MarkdownTextView
// ---------------------------------------------------------------------------

MarkdownTextView::MarkdownTextView(const char* name, const MarkdownPalette& palette)
    : BTextView(name, B_WILL_DRAW | B_PULSE_NEEDED | B_FRAME_EVENTS)
    , palette_(palette)
{
    MakeEditable(false);
    MakeSelectable(true);
    SetWordWrap(true);
    SetStylable(true);
}

void
MarkdownTextView::SetMarkdown(const std::string& markdown)
{
    markdown_ = markdown;
    _Render();
}

void
MarkdownTextView::FrameResized(float width, float height)
{
    BTextView::FrameResized(width, height);
    if (MarkdownColumns(this) != cols_ && width_dependent(markdown_))
        _Render();
}

void
MarkdownTextView::_Render()
{
    Options opts;
    opts.max_cols = cols_ = MarkdownColumns(this);
    opts.ascii_borders = MarkdownAsciiBorders();
    Styled styled = render(markdown_, opts);
    TextRunArrayPtr runs = MarkdownRunArray(styled, palette_);

    BScrollBar* vsb = ScrollBar(B_VERTICAL);
    float value = vsb ? vsb->Value() : 0;
    SetText(styled.text.data(), (int32)styled.text.size(), runs.get());
    if (vsb) vsb->SetValue(value);
}
