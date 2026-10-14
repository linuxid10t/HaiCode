#include "Glyphs.h"

#include <Application.h>
#include <Font.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace haicode_gui;

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

static size_t
longest_sequence(const std::string& s)
{
    size_t longest = 0, i = 0;
    while (i < s.size()) {
        size_t len = 1;
        if ((s[i] & 0xf8) == 0xf0) len = 4;
        else if ((s[i] & 0xf0) == 0xe0) len = 3;
        else if ((s[i] & 0xe0) == 0xc0) len = 2;
        longest = std::max(longest, len);
        i += len;
    }
    return longest;
}

static bool
covered(const BFont& font, const char* glyph)
{
    return FontCoversGlyph(font, glyph);
}

int
main()
{
    BApplication app("application/x-vnd.haicode-test-statusglyphs");

    BFont probe(*be_plain_font);
    const bool emoji = EmojiFont() != nullptr;
    std::printf("emoji font: %s\n", emoji ? "loaded" : "absent");

    // CoveredGlyph never returns an astral candidate: even when
    // GetHasGlyphs (with fallbacks) claims coverage the render fallback
    // chain draws a box, so astral entries are skipped outright.
    const std::vector<const char*> allAstral = {
        "\xf0\x9f\x92\xa1", "\xf0\x9f\x92\xac", "\xf0\x9f\x94\xa7",
    };
    CHECK(CoveredGlyph(probe, allAstral, "*") == "*");
    CHECK(CoveredGlyph(probe, {}, "x") == "x");
    const std::vector<const char*> ordered = {"A", "B"};
    CHECK(CoveredGlyph(probe, ordered, "z") == "A");
    if (!covered(probe, "\xe2\x9a\x99")) {  // ⚙ uncovered on this system
        const std::vector<const char*> withFallback = {"\xe2\x9a\x99", "B"};
        CHECK(CoveredGlyph(probe, withFallback, "z") == "B");
    }

    StatusGlyphs g = ResolveStatusGlyphs(probe);
    // Astral (emoji) results are legal ONLY when the directly-loaded
    // NotoEmoji exists — StatusStripView draws them in that font.
    CHECK(longest_sequence(g.thinking) == (emoji ? 4 : 3));
    CHECK(longest_sequence(g.streaming) == (emoji ? 4 : 3));
    CHECK(longest_sequence(g.tool) == (emoji ? 4 : 3));
    CHECK(g.building == g.tool);
    // idle and compacting never use emoji: their BMP symbols render
    // through the shipped fallbacks everywhere.
    CHECK(longest_sequence(g.idle) <= 3);
    CHECK(longest_sequence(g.compacting) <= 3);

    // Every BMP glyph is covered (font + fallbacks) or exactly its ASCII
    // last resort — nothing can draw a missing-glyph box.
    struct State { const char* name; const std::string* glyph;
                   const char* lastResort; };
    const State states[] = {
        {"idle", &g.idle, "*"},
        {"thinking", &g.thinking, "*"},
        {"streaming", &g.streaming, ">"},
        {"tool", &g.tool, "*"},
        {"building", &g.building, "*"},
        {"compacting", &g.compacting, "*"},
    };
    for (const State& st : states) {
        CHECK(!st.glyph->empty());
        if (longest_sequence(*st.glyph) <= 3) {
            if (covered(probe, st.glyph->c_str()))
                std::printf("%s: covered %s\n", st.name, st.glyph->c_str());
            else
                CHECK(*st.glyph == st.lastResort);
        } else {
            std::printf("%s: emoji %s (direct font)\n", st.name,
                        st.glyph->c_str());
        }
    }

    CHECK(covered(probe, "A"));

    if (failures == 0) std::printf("status_glyphs: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
