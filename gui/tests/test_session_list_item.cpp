#include "SessionListItem.h"

#include <Application.h>
#include <Font.h>

#include <cstdio>
#include <cstring>
#include <cmath>

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

int
main()
{
    BApplication app("application/x-vnd.haicode-test-sessionitem");

    const int64_t minute = 60 * 1000;
    int64_t now = 1791093593000;  // fixed instant; label must be deterministic

    SessionListItem item("Fix build", "/boot/home/work/demo", "s1", now);
    CHECK(item.Text() && std::strcmp(item.Text(), "Fix build") == 0);
    CHECK(item.Directory() == "/boot/home/work/demo");
    CHECK(item.SessionId() == "s1");

    // Deterministic label from the fixed instant.
    CHECK(item.ModifiedLabel().rfind("Modified: ", 0) == 0);
    CHECK(item.ModifiedLabel().size() == std::strlen("Modified: ") + 16);
    CHECK(item.ModifiedLabel() == "Modified: 2026-10-04 05:59"
        || item.ModifiedLabel() == "Modified: unknown");

    // Same timestamp is a no-op; a later one changes the label.
    CHECK(!item.SetModifiedTime(now));
    CHECK(item.SetModifiedTime(now + 5 * minute));
    CHECK(item.ModifiedLabel() != "Modified: unknown");

    // Unset timestamps degrade instead of showing 1970.
    CHECK(item.SetModifiedTime(0));
    CHECK(item.ModifiedLabel() == "Modified: unknown");

    // Three text lines: height must exceed a two-line row.
    item.SetModifiedTime(now);
    const BFont* font = be_plain_font;
    item.Update(nullptr, font);
    font_height metrics;
    font->GetHeight(&metrics);
    float line = metrics.ascent + metrics.descent + metrics.leading;
    CHECK(item.Height() > line * 2 + 8);
    CHECK(item.Height() <= std::ceil(line * 3) + 8 + 1.0f);

    // Empty directory falls back to the shared placeholder.
    SessionListItem empty("Untitled session", "", "s2", now);
    CHECK(empty.Directory() == "No project directory");

    // Spinner state: idempotent setters, frame cycling wraps at 12.
    CHECK(item.SetRunning(true));
    CHECK(!item.SetRunning(true));
    CHECK(item.IsRunning());
    item.SetSpinnerFrame(11);
    item.SetSpinnerFrame(12 % 12);
    // Glyph selection never returns null and never index-errors on any
    // frame value, whatever the font covers.
    BFont probe(*be_plain_font);
    for (int frame = 0; frame < 24; ++frame) {
        const char* glyph = item.SpinnerGlyph(frame, probe);
        CHECK(glyph && *glyph);
    }
    // The exact Claude Code cycle: forward, then reversed. Doubled frames
    // at both turnarounds are part of the sequence.
    static const char* const kExpected[12] = {
        "\xC2\xB7", "\xE2\x9C\xA2", "*", "\xE2\x9C\xB6", "\xE2\x9C\xBB",
        "\xE2\x9C\xBD", "\xE2\x9C\xBD", "\xE2\x9C\xBB", "\xE2\x9C\xB6",
        "*", "\xE2\x9C\xA2", "\xC2\xB7",
    };
    // Coverage as DrawString sees it: UTF-8 in, fallback fonts included. A
    // covered frame shows itself; an uncovered one falls through to a glyph
    // that IS covered (never a replacement box).
    auto covered = [&](const char* glyph) {
        bool has = false;
        probe.GetHasGlyphs(glyph, 1, &has, true);
        return has;
    };
    for (int frame = 0; frame < 24; ++frame) {
        const char* glyph = item.SpinnerGlyph(frame, probe);
        if (covered(kExpected[frame % 12]))
            CHECK(std::strcmp(glyph, kExpected[frame % 12]) == 0);
        else
            CHECK(covered(glyph) || std::strcmp(glyph, "*") == 0);
    }
    CHECK(std::strcmp(item.SpinnerGlyph(-1, probe), item.SpinnerGlyph(11, probe)) == 0);
    CHECK(item.SetRunning(false));
    CHECK(!item.SetRunning(false));
    CHECK(!item.IsRunning());

    if (failures == 0) std::printf("session_list_item: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
