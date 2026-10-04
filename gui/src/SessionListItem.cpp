#include "SessionListItem.h"

#include <Font.h>
#include <View.h>

#include <algorithm>
#include <ctime>

SessionListItem::SessionListItem(const std::string& title,
                                 const std::string& directory,
                                 const std::string& session_id,
                                 int64_t time_updated)
    : BStringItem(title.c_str()), directory_(directory.empty()
          ? "No project directory" : directory), session_id_(session_id)
{
    SetModifiedTime(time_updated);
}

bool
SessionListItem::SetRunning(bool running)
{
    if (running == running_) return false;
    running_ = running;
    return true;
}

void
SessionListItem::SetSpinnerFrame(int frame)
{
    spinner_frame_ = frame;
}

bool
SessionListItem::SetModifiedTime(int64_t time_updated)
{
    if (time_updated == modified_time_) return false;
    modified_time_ = time_updated;
    if (time_updated <= 0) {
        // 0 means "never updated"; showing 1970 would be misleading.
        bool changed = modified_label_ != "Modified: unknown";
        modified_label_ = "Modified: unknown";
        return changed;
    }
    time_t seconds = time_updated / 1000;
    tm local{};
    char text[64];
    std::string label = "Modified: unknown";
    if (localtime_r(&seconds, &local)
            && std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local))
        label = std::string("Modified: ") + text;
    bool changed = label != modified_label_;
    modified_label_ = std::move(label);
    return changed;
}

const char*
SessionListItem::SpinnerGlyph(int frame, const BFont& font)
{
    // GetHasGlyphs reads UTF-8 and counts characters, so each frame goes in
    // as-is with a count of one (a UTF-32 buffer here was read as UTF-8 and
    // probed the wrong characters: U+2722 became '"', U+2736 '6', ...).
    // Fallback fonts count as coverage because DrawString renders through
    // them — the Dingbats frames usually come from a fallback font, not the
    // UI font itself. Frames nothing covers are skipped so rows never render
    // replacement boxes.
    frame %= kSpinnerFrameCount;
    if (frame < 0) frame += kSpinnerFrameCount;
    for (int step = 0; step < kSpinnerFrameCount; ++step) {
        int i = (frame + step) % kSpinnerFrameCount;
        bool has = false;
        font.GetHasGlyphs(kSpinnerFrames[i], 1, &has, true);
        if (has) return kSpinnerFrames[i];
    }
    return "*";
}

void
SessionListItem::Update(BView* owner, const BFont* font)
{
    BStringItem::Update(owner, font);
    font_height metrics;
    font->GetHeight(&metrics);
    float line = std::ceil(metrics.ascent + metrics.descent + metrics.leading);
    SetHeight(line * 3 + 8);
    SetWidth(std::max({Width(), font->StringWidth(directory_.c_str()) + 12,
        font->StringWidth(modified_label_.c_str()) + 12}));
}

void
SessionListItem::DrawItem(BView* owner, BRect frame, bool complete)
{
    (void)complete;
    owner->PushState();
    rgb_color background = ui_color(IsSelected()
        ? B_LIST_SELECTED_BACKGROUND_COLOR : B_LIST_BACKGROUND_COLOR);
    rgb_color foreground = ui_color(IsSelected()
        ? B_LIST_SELECTED_ITEM_TEXT_COLOR : B_LIST_ITEM_TEXT_COLOR);
    owner->SetLowColor(background);
    owner->SetHighColor(background);
    owner->FillRect(frame);
    owner->SetHighColor(foreground);
    BFont font;
    owner->GetFont(&font);
    font_height metrics;
    font.GetHeight(&metrics);
    if (running_) {
        const char* glyph = SpinnerGlyph(spinner_frame_, font);
        float glyph_width = font.StringWidth(glyph);
        float x = frame.left + 6 + (kSpinnerGutter - 6 - glyph_width) / 2;
        owner->DrawString(glyph, BPoint(x, frame.top + 4 + metrics.ascent));
    }
    // The gutter is always reserved so titles stay put when activity
    // starts or stops; only the glyph comes and goes.
    frame.left += kSpinnerGutter;
    float line = std::ceil(metrics.ascent + metrics.descent + metrics.leading);
    float width = std::max(0.0f, frame.Width() - 12 - kSpinnerGutter);
    BString title(Text());
    font.TruncateString(&title, B_TRUNCATE_END, width);
    owner->DrawString(title.String(), BPoint(frame.left + 6, frame.top + 4 + metrics.ascent));
    font.SetSize(std::max(8.0f, font.Size() - 1));
    owner->SetFont(&font);
    BString directory(directory_.c_str());
    font.TruncateString(&directory, B_TRUNCATE_MIDDLE, width);
    owner->DrawString(directory.String(),
        BPoint(frame.left + 6, frame.top + 4 + metrics.ascent + line));
    BString modified(modified_label_.c_str());
    font.TruncateString(&modified, B_TRUNCATE_END, width);
    owner->DrawString(modified.String(),
        BPoint(frame.left + 6, frame.top + 4 + metrics.ascent + line * 2));
    owner->PopState();
}
