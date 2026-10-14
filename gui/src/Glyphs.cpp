#include "Glyphs.h"

#include <Application.h>
#include <FindDirectory.h>
#include <Path.h>
#include <Window.h>

#include <cmath>

namespace haicode_gui {

bool
FontCoversGlyph(const BFont& font, const char* utf8)
{
    // GetHasGlyphs reads UTF-8 and counts characters, so a one-character
    // string goes in with a count of one (a UTF-32 buffer would be read as
    // UTF-8 and probe the wrong characters). Fallback fonts count as
    // coverage because DrawString renders through them — except where a
    // fallback's differing advance width would break a fixed-font grid
    // (MarkdownAsciiBorders), which is not the case here.
    bool has = false;
    font.GetHasGlyphs(utf8, 1, &has, true);
    return has;
}

const BFont*
EmojiFont()
{
    // Loaded once per process; user fonts auto-unload at app exit. The
    // wrench probe guards against a font that loads but lacks the
    // codepoints (wrong file under the same name).
    static BFont emoji;
    static bool resolved = false;
    static bool ok = false;
    if (!resolved) {
        resolved = true;
        BPath p;
        if (find_directory(B_SYSTEM_DATA_DIRECTORY, &p) == B_OK) {
            p.Append("fonts/ttfonts/NotoEmoji-Regular.ttf");
            if (emoji.LoadFont(p.Path()) == B_OK) {
                bool has = false;
                emoji.GetHasGlyphs("\xf0\x9f\x94\xa7", 1, &has, false);
                ok = has;
            }
        }
    }
    return ok ? &emoji : nullptr;
}

std::string
CoveredGlyph(const BFont& font, const std::vector<const char*>& ladder,
             const char* last_resort)
{
    for (const char* candidate : ladder) {
        // Astral-plane candidates (4-byte UTF-8) are skipped even when
        // covered: GetHasGlyphs(useFallbacks) sees an installed Noto Emoji
        // and reports coverage, but the render fallback chain draws a
        // missing-glyph box for them. BMP only.
        if ((unsigned char)candidate[0] >= 0xf0) continue;
        if (FontCoversGlyph(font, candidate)) return candidate;
    }
    return last_resort;
}

StatusGlyphs
ResolveStatusGlyphs(const BFont& font)
{
    // Running states prefer real emoji when a directly-loaded NotoEmoji
    // can draw them (StatusStripView draws that segment in the emoji
    // font); the BMP ladder is the no-emoji-font degrade path and the
    // source of the plain-font text run. Every BMP entry renders through
    // the shipped Noto fallbacks on a default install.
    const bool emoji = EmojiFont() != nullptr;
    StatusGlyphs g;
    g.idle = CoveredGlyph(font, {"\xe2\x9c\x93"}, "*");  // ✓
    g.thinking = emoji ? "\xf0\x9f\x92\xa1"               // 💡
                       : CoveredGlyph(font, {"\xe2\x9c\xa2"}, "*");   // ✢
    g.streaming = emoji ? "\xf0\x9f\x92\xac"              // 💬
                        : CoveredGlyph(font, {"\xe2\x96\xb6"}, ">");  // ▶
    g.tool = emoji ? "\xf0\x9f\x94\xa7"                   // 🔧
                   : CoveredGlyph(font, {"\xe2\x9a\x99"}, "*");       // ⚙
    g.building = g.tool;
    g.compacting = CoveredGlyph(font, {"\xe2\x9c\x8e",    // ✎
                                       "\xe2\x96\xb6"},
        "*");
    return g;
}

// ---------------------------------------------------------------------------
// StatusStripView

StatusStripView::StatusStripView(const char* name, const BFont& font)
    : BView(name, B_WILL_DRAW | B_SUPPORTS_LAYOUT), fFont(font)
{
    SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    if (const BFont* emoji = EmojiFont()) {
        fEmojiFont = *emoji;
        fEmojiFont.SetSize(font.Size());
        fEmojiLoaded = true;
    }
}

void
StatusStripView::SetText(const char* text)
{
    fText = text;
    // Clamp the glyph range to the new length (appenders rely on it
    // pointing at the state glyph even after a length change).
    if (fGlyphOffset > fText.Length())
        fGlyphOffset = fText.Length();
    if (fGlyphOffset + fGlyphLength > fText.Length())
        fGlyphLength = fText.Length() - fGlyphOffset;
    InvalidateLayout();
    Invalidate();
}

void
StatusStripView::SetGlyphRange(int32 offset, int32 length)
{
    fGlyphOffset = offset;
    fGlyphLength = length;
    Invalidate();
}

bool
StatusStripView::UseEmojiFont() const
{
    // Only the astral-plane glyph segment goes through the emoji font, and
    // only when that font really loaded; BMP glyphs render (and measure)
    // through the strip font in one run.
    if (!fEmojiLoaded || fGlyphLength <= 0) return false;
    const char* bytes = fText.String() + fGlyphOffset;
    return (unsigned char)bytes[0] >= 0xf0;
}

void
StatusStripView::TextSize(float* width, float* height) const
{
    font_height mh;
    fFont.GetHeight(&mh);
    float h = mh.ascent + mh.descent + mh.leading;
    float w = 0;
    if (UseEmojiFont()) {
        w += fFont.StringWidth(fText.String(), fGlyphOffset);
        w += fEmojiFont.StringWidth(fText.String() + fGlyphOffset,
                                    fGlyphLength);
        w += fFont.StringWidth(fText.String() + fGlyphOffset + fGlyphLength);
    } else {
        w = fFont.StringWidth(fText.String());
    }
    *width = w;
    *height = h;
}

void
StatusStripView::Draw(BRect)
{
    SetFont(&fFont);
    SetHighUIColor(B_PANEL_TEXT_COLOR);
    font_height mh;
    fFont.GetHeight(&mh);
    float base = mh.ascent + 1;
    if (!UseEmojiFont()) {
        DrawString(fText.String(), BPoint(0, base));
        return;
    }
    const char* bytes = fText.String();
    float x = 0;
    DrawString(bytes, fGlyphOffset, BPoint(x, base));
    x += fFont.StringWidth(bytes, fGlyphOffset);
    SetFont(&fEmojiFont);
    DrawString(bytes + fGlyphOffset, fGlyphLength, BPoint(x, base));
    x += fEmojiFont.StringWidth(bytes + fGlyphOffset, fGlyphLength);
    SetFont(&fFont);
    DrawString(bytes + fGlyphOffset + fGlyphLength, BPoint(x, base));
}

void
StatusStripView::GetPreferredSize(float* width, float* height)
{
    TextSize(width, height);
    *width += 1;
}

BSize
StatusStripView::MinSize()
{
    float w, h;
    TextSize(&w, &h);
    return BSize(w + 1, h);
}

BSize
StatusStripView::MaxSize()
{
    float w, h;
    TextSize(&w, &h);
    return BSize(B_SIZE_UNLIMITED, h);
}

}  // namespace haicode_gui
