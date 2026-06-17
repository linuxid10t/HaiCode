#include "ChatView.h"

#include <ScrollView.h>
#include <TextView.h>
#include <Font.h>

#include <string>

// Color palette
static const rgb_color kColorUser       = {  30, 100, 220, 255 };
static const rgb_color kColorAssistant  = {  20, 140,  60, 255 };
static const rgb_color kColorToolHeader = { 180, 140,   0, 255 };
static const rgb_color kColorToolBody   = { 120, 120, 120, 255 };
static const rgb_color kColorToolOk     = {  30, 160,  50, 255 };
static const rgb_color kColorToolErr    = { 200,  30,  30, 255 };
static const rgb_color kColorSystem     = { 200, 120,   0, 255 };

// ---------------------------------------------------------------------------
// ClickableTextView
// ---------------------------------------------------------------------------

ClickableTextView::ClickableTextView(BRect frame, const char* name,
                                     BRect textRect, uint32 flags,
                                     uint32 resizingMode)
    : BTextView(frame, name, textRect, flags, resizingMode)
{}

void
ClickableTextView::MouseDown(BPoint where)
{
    if (owner_) {
        int32 offset = OffsetAt(where);
        int idx = owner_->FindBlockAt(offset);
        if (idx >= 0) {
            owner_->ToggleBlock(idx);
            return;
        }
    }
    BTextView::MouseDown(where);
}

// ---------------------------------------------------------------------------
// ChatView
// ---------------------------------------------------------------------------

ChatView::ChatView(const char* /*name*/)
{
    BRect tvRect(0, 0, 400, 300);
    BRect textRect(4, 4, 396, 296);

    text_view_ = new ClickableTextView(tvRect, "chat_text", textRect,
                                       B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_SUPPORTS_LAYOUT,
                                       B_FOLLOW_ALL);
    text_view_->SetOwner(this);
    text_view_->MakeEditable(false);
    text_view_->MakeSelectable(true);
    text_view_->SetWordWrap(true);
    text_view_->SetStylable(true);
    text_view_->SetViewColor(255, 255, 255);
    text_view_->SetLowColor(255, 255, 255);
    text_view_->SetExplicitMinSize(BSize(B_SIZE_UNSET, B_SIZE_UNSET));
    text_view_->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));

    scroll_ = new BScrollView("chat_scroll", text_view_,
                              0, false, true, B_FANCY_BORDER);
    scroll_->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
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

    // Insert at end via the explicit-offset overload to avoid selection drift.
    int32 start = text_view_->TextLength();
    text_view_->Insert(start, text.c_str(), text.size());
    int32 end = text_view_->TextLength();
    text_view_->SetFontAndColor(start, end, &font, B_FONT_ALL, &color);
    ScrollToBottom();
}

void
ChatView::ScrollToBottom()
{
    if (inhibit_scroll_) return;

    int32 len = text_view_->TextLength();
    if (len <= 0) return;

    int32 lines = text_view_->CountLines();
    if (lines > 0 && text_view_->ByteAt(len - 1) == '\n') {
        --lines;
    }
    if (lines > 0) {
        BRect tr = text_view_->TextRect();
        tr.bottom = tr.top + text_view_->TextHeight(0, lines);
        text_view_->SetTextRect(tr);
    }

    if (BScrollBar* vsb = scroll_->ScrollBar(B_VERTICAL)) {
        float lo, hi;
        vsb->GetRange(&lo, &hi);
        vsb->SetValue(hi);
    }
}

// Rebuild the entire text view from the stored model.
void
ChatView::_Rebuild()
{
    inhibit_scroll_ = true;
    text_view_->SetText("");
    header_ranges_.clear();

    for (int i = 0; i < (int)model_.size(); i++) {
        const auto& e = model_[i];
        switch (e.kind) {
        case ChatEntry::UserText:
            AppendStyled("\nYou: ", kColorUser, true);
            AppendStyled(e.text + "\n", kColorUser, false);
            break;

        case ChatEntry::AssistantText:
            AppendStyled("\nAssistant: ", kColorAssistant, true);
            AppendStyled(e.text + "\n", kColorAssistant, false);
            break;

        case ChatEntry::ToolCalled: {
            std::string indicator = e.collapsed ? " ▶" : " ▼";
            std::string header = "\n[Tool: " + e.name + "]" + indicator + "\n";
            int32 hstart = text_view_->TextLength();
            AppendStyled(header, kColorToolHeader, true);
            int32 hend = text_view_->TextLength();
            header_ranges_.push_back({hstart, hend, i});
            if (!e.collapsed && !e.text.empty() && e.text != "{}") {
                AppendStyled(e.text + "\n", kColorToolBody, false);
            }
            break;
        }

        case ChatEntry::ToolResult: {
            std::string summary = e.text;
            auto nl = summary.find('\n');
            if (nl != std::string::npos) summary = summary.substr(0, nl) + " \xe2\x80\xa6";
            if (e.success) {
                AppendStyled("[OK] " + summary + "\n", kColorToolOk, false);
            } else {
                AppendStyled("[ERR] " + summary + "\n", kColorToolErr, false);
            }
            break;
        }

        case ChatEntry::System:
            AppendStyled("\n[System] " + e.text + "\n", kColorSystem, false);
            break;
        }
    }

    inhibit_scroll_ = false;
    ScrollToBottom();
}

void
ChatView::AppendUserText(const std::string& text)
{
    streaming_ = false;
    model_.push_back({ChatEntry::UserText, text, "", true, false});
    AppendStyled("\nYou: ", kColorUser, true);
    AppendStyled(text + "\n", kColorUser, false);
}

void
ChatView::AppendTextDelta(const std::string& delta)
{
    if (!streaming_) {
        model_.push_back({ChatEntry::AssistantText, "", "", true, false});
        AppendStyled("\nAssistant: ", kColorAssistant, true);
        streaming_ = true;
    }
    model_.back().text += delta;
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
    pending_tool_idx_ = (int)model_.size();
    model_.push_back({ChatEntry::ToolCalled, input_json, tool_name, true, false});

    // Show expanded while the tool is running; will collapse when result arrives.
    std::string header = "\n[Tool: " + tool_name + "] ▼\n";
    int32 hstart = text_view_->TextLength();
    AppendStyled(header, kColorToolHeader, true);
    int32 hend = text_view_->TextLength();
    header_ranges_.push_back({hstart, hend, pending_tool_idx_});

    if (!input_json.empty() && input_json != "{}") {
        AppendStyled(input_json + "\n", kColorToolBody, false);
    }
}

void
ChatView::AppendToolResult(const std::string& output, bool success)
{
    // Collapse the tool call that just finished.
    if (pending_tool_idx_ >= 0 && pending_tool_idx_ < (int)model_.size()) {
        model_[pending_tool_idx_].collapsed = true;
        pending_tool_idx_ = -1;
    }

    model_.push_back({ChatEntry::ToolResult, output, "", success, false});
    _Rebuild();
}

void
ChatView::AppendSystem(const std::string& text)
{
    streaming_ = false;
    model_.push_back({ChatEntry::System, text, "", true, false});
    AppendStyled("\n[System] " + text + "\n", kColorSystem, false);
}

void
ChatView::Clear()
{
    streaming_        = false;
    pending_tool_idx_ = -1;
    model_.clear();
    header_ranges_.clear();
    text_view_->SetText("");
}

int
ChatView::FindBlockAt(int32 offset) const
{
    for (const auto& h : header_ranges_) {
        if (offset >= h.start && offset < h.end)
            return h.model_idx;
    }
    return -1;
}

void
ChatView::ToggleBlock(int model_idx)
{
    if (model_idx < 0 || model_idx >= (int)model_.size()) return;
    if (model_[model_idx].kind != ChatEntry::ToolCalled) return;
    model_[model_idx].collapsed = !model_[model_idx].collapsed;
    _Rebuild();
}
