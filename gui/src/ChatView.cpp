#include "ChatView.h"

#include <ScrollView.h>
#include <TextView.h>
#include <Font.h>

#include <string>

// Color palette
static const rgb_color kColorUser       = {  30, 100, 220, 255 };  // blue
static const rgb_color kColorAssistant  = {  20, 140,  60, 255 };  // dark green
static const rgb_color kColorToolHeader = { 180, 140,   0, 255 };  // amber / bold
static const rgb_color kColorToolBody   = { 120, 120, 120, 255 };  // gray
static const rgb_color kColorToolOk     = {  30, 160,  50, 255 };  // green
static const rgb_color kColorToolErr    = { 200,  30,  30, 255 };  // red
static const rgb_color kColorSystem     = { 200, 120,   0, 255 };  // orange

ChatView::ChatView(const char* /*name*/)
{
    BRect tvRect(0, 0, 400, 300);
    BRect textRect(4, 4, 396, 296);

    text_view_ = new BTextView(tvRect, "chat_text", textRect,
                               B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_SUPPORTS_LAYOUT,
                               B_FOLLOW_ALL);
    text_view_->MakeEditable(false);
    text_view_->MakeSelectable(true);
    text_view_->SetWordWrap(true);
    text_view_->SetStylable(true);
    text_view_->SetViewColor(255, 255, 255);
    text_view_->SetLowColor(255, 255, 255);

    scroll_ = new BScrollView("chat_scroll", text_view_,
                              B_FOLLOW_ALL, 0,
                              false, true, B_FANCY_BORDER);
}

void
ChatView::AppendStyled(const std::string& text, rgb_color color, bool bold)
{
    BFont font;
    if (bold) {
        font = *be_bold_font;
    } else {
        font = *be_plain_font;
    }

    int32 start = text_view_->TextLength();
    text_view_->Insert(text.c_str());
    int32 end = text_view_->TextLength();
    text_view_->SetFontAndColor(start, end, &font, B_FONT_ALL, &color);
    ScrollToBottom();
}

void
ChatView::ScrollToBottom()
{
    // Let BTextView recompute its own scroll range, then scroll there.
    // Going through BScrollBar::SetValue(GetRange()) reads a stale max
    // right after Insert(), so the last appended lines stay off-screen.
    int32 lines = text_view_->CountLines();
    int32 len   = text_view_->TextLength();
    if (lines > 0 && len > 0 && text_view_->ByteAt(len - 1) == '\n') {
        --lines;  // drop the trailing empty line BTextView counts
    }
    if (lines > 0) {
        BRect tr = text_view_->TextRect();
        tr.bottom = tr.top + text_view_->TextHeight(0, lines);
        text_view_->SetTextRect(tr);
    }
    text_view_->ScrollToOffset(text_view_->TextLength());
}

void
ChatView::AppendUserText(const std::string& text)
{
    streaming_ = false;
    std::string label = "\nYou: ";
    AppendStyled(label, kColorUser, true);
    AppendStyled(text + "\n", kColorUser, false);
}

void
ChatView::AppendTextDelta(const std::string& delta)
{
    if (!streaming_) {
        // Start a new assistant turn
        AppendStyled("\nAssistant: ", kColorAssistant, true);
        streaming_ = true;
    }
    AppendStyled(delta, kColorAssistant, false);
}

void
ChatView::EndStreaming()
{
    if (streaming_) {
        AppendStyled("\n", kColorAssistant, false);
        streaming_ = false;
    }
}

void
ChatView::AppendToolCalled(const std::string& tool_name, const std::string& input_json)
{
    streaming_ = false;
    std::string header = "\n[Tool: " + tool_name + "]\n";
    AppendStyled(header, kColorToolHeader, true);
    if (!input_json.empty() && input_json != "{}") {
        AppendStyled(input_json + "\n", kColorToolBody, false);
    }
}

void
ChatView::AppendToolResult(const std::string& output, bool success)
{
    if (success) {
        std::string msg = "[OK] " + output + "\n";
        AppendStyled(msg, kColorToolOk, false);
    } else {
        std::string msg = "[ERR] " + output + "\n";
        AppendStyled(msg, kColorToolErr, false);
    }
}

void
ChatView::AppendSystem(const std::string& text)
{
    streaming_ = false;
    AppendStyled("\n[System] " + text + "\n", kColorSystem, false);
}

void
ChatView::Clear()
{
    streaming_ = false;
    text_view_->SetText("");
}
