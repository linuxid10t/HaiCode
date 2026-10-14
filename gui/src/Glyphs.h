#pragma once

#include <Font.h>
#include <String.h>
#include <View.h>

#include <string>
#include <vector>

class BFont;

namespace haicode_gui {

// True when `font` (or one of its fallbacks — the way DrawString renders)
// has a glyph for the single UTF-8 character `utf8`. NOTE: with an emoji
// font installed this overreports astral-plane characters: Haiku's render
// fallback chain does not draw them. Trust it for BMP only.
bool FontCoversGlyph(const BFont& font, const char* utf8);

// NotoEmoji loaded straight from disk via BFont::LoadFont, or nullptr when
// no emoji font is installed. The render fallback chain does not draw
// astral glyphs even with noto_emoji installed (the metrics probe claims
// coverage but a box is drawn), so astral emoji must be drawn with this
// font explicitly. Sized to the strip font by the caller.
const BFont* EmojiFont();

// First ladder entry the font or a fallback covers, else `last_resort`
// (which must be ASCII). Never returns empty. Candidates outside the BMP
// (4-byte UTF-8 lead byte) are skipped: with an emoji font installed the
// metrics probe reports them covered, but the render path still draws
// missing-glyph boxes for astral-plane characters.
std::string CoveredGlyph(const BFont& font,
                         const std::vector<const char*>& ladder,
                         const char* last_resort);

// The status strip's state glyphs, resolved once per window against the
// strip font. When EmojiFont() is available the running states use real
// emoji (drawn with that font); otherwise every field is a covered (or
// ASCII last-resort) BMP glyph.
struct StatusGlyphs {
    std::string idle;
    std::string thinking;
    std::string streaming;
    std::string tool;
    std::string building;
    std::string compacting;
};

StatusGlyphs ResolveStatusGlyphs(const BFont& font);

// One-line status strip: plain-font text with an optional glyph segment
// (the state glyph) drawn in the directly-loaded emoji font when — and
// only when — that segment is astral-plane and the font covers it. BMP
// glyphs render through the strip font in a single DrawString.
class StatusStripView : public BView {
public:
    StatusStripView(const char* name, const BFont& font);

    void SetText(const char* text);
    const char* Text() const { return fText.String(); }
    // Byte range within Text() drawn in the emoji font (kept across
    // SetText so callers may append, clamped to the text length).
    void SetGlyphRange(int32 offset, int32 length);

    void Draw(BRect updateRect) override;
    void GetPreferredSize(float* width, float* height) override;
    BSize MinSize() override;
    BSize MaxSize() override;

private:
    bool UseEmojiFont() const;
    void TextSize(float* width, float* height) const;

    BString fText;
    BFont fFont;
    BFont fEmojiFont;
    bool fEmojiLoaded = false;
    int32 fGlyphOffset = 0;
    int32 fGlyphLength = 0;
};

}  // namespace haicode_gui
