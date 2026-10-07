#pragma once

#include <StringItem.h>

#include <cmath>
#include <cstdint>
#include <string>

struct font_height;
class BView;
class BFont;
struct rgb_color;

// Horizontal gutter left of the title, reserved for the activity glyph.
constexpr float kSpinnerGutter = 22.0f;

// Activity glyph cycle, matching Claude Code: the six-frame base array
// played forward, then reversed (12 frames total). The doubled frames at
// the two turnarounds are intentional.
//   · ✢ * ✶ ✻ ✽ ✽ ✻ ✶ * ✢ ·
constexpr const char* kSpinnerFrames[12] = {
    "\xC2\xB7",             // ·  U+00B7 middle dot
    "\xE2\x9C\xA2",         // ✢  U+2722 four teardrop-spoked asterisk
    "*",                    // *  U+002A ASCII asterisk
    "\xE2\x9C\xB6",         // ✶  U+2736 six-pointed black star
    "\xE2\x9C\xBB",         // ✻  U+273B teardrop-spoked asterisk
    "\xE2\x9C\xBD",         // ✽  U+273D heavy teardrop-spoked asterisk
    "\xE2\x9C\xBD",         // ✽  (reversed pass begins)
    "\xE2\x9C\xBB",         // ✻
    "\xE2\x9C\xB6",         // ✶
    "*",                    // *
    "\xE2\x9C\xA2",         // ✢
    "\xC2\xB7",             // ·
};
constexpr int kSpinnerFrameCount = 12;

// Sidebar row: session title on the first line, project directory on the
// second (or a chat-only note for Chat-mode sessions, which have no local
// access and so no meaningful directory), and the last-modified time on the
// third. SetModifiedTime() returns whether the label changed so refreshers
// can skip invalidating untouched rows.
class SessionListItem : public BStringItem {
public:
    SessionListItem(const std::string& title, const std::string& directory,
                    const std::string& session_id, int64_t time_updated,
                    bool chat_only = false);

    const std::string& Directory() const { return directory_; }
    // The second line as drawn: the directory, or the chat-only note.
    const std::string& DetailLabel() const;
    bool IsChatOnly() const { return chat_only_; }
    const std::string& SessionId() const { return session_id_; }
    const std::string& ModifiedLabel() const { return modified_label_; }
    bool IsRunning() const { return running_; }

    // SetRunning returns whether the state changed (the glyph gutter needs a
    // repaint); SetModifiedTime whether the label changed.
    bool SetRunning(bool running);
    void SetSpinnerFrame(int frame);
    bool SetModifiedTime(int64_t time_updated);
    // Returns whether the state changed (the row needs a repaint).
    bool SetChatOnly(bool chat_only);

    // The glyph shown for the current spinner frame, honoring font coverage.
    static const char* SpinnerGlyph(int frame, const BFont& font);

    void Update(BView* owner, const BFont* font) override;
    void DrawItem(BView* owner, BRect frame, bool complete) override;

private:
    std::string directory_;
    std::string session_id_;
    std::string modified_label_;
    int64_t modified_time_ = -1;
    bool running_ = false;
    bool chat_only_ = false;
    int spinner_frame_ = 0;
};
