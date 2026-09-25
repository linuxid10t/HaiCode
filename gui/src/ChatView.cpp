#include "ChatView.h"

#include <Clipboard.h>
#include <Cursor.h>
#include <Font.h>
#include <Message.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <ScrollView.h>
#include <TextView.h>
#include <Window.h>

#include <string>

// Color palette
static const rgb_color kColorUser           = {  30, 100, 220, 255 };
static const rgb_color kColorAssistant      = {  20, 140,  60, 255 };
static const rgb_color kColorToolHeader     = { 180, 140,   0, 255 };
static const rgb_color kColorToolBody       = { 120, 120, 120, 255 };
static const rgb_color kColorToolOk         = {  30, 160,  50, 255 };
static const rgb_color kColorToolErr        = { 200,  30,  30, 255 };
static const rgb_color kColorSystem         = { 200, 120,   0, 255 };
static const rgb_color kColorThinkingHeader = { 110,  90, 180, 255 };
static const rgb_color kColorThinkingBody   = { 140, 140, 140, 255 };
static const rgb_color kColorCopyControl    = {  45,  90, 160, 255 };
static const rgb_color kColorCopyFeedback   = {  30, 160,  50, 255 };
static const uint32 kMsgResetCopyFeedback  = 'RCfb';

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
        int32 buttons = 0;
        if (Window() && Window()->CurrentMessage())
            Window()->CurrentMessage()->FindInt32("buttons", &buttons);
        if (buttons & B_PRIMARY_MOUSE_BUTTON) {
            int32 offset = OffsetAt(where);
            int copy_idx = owner_->FindCopyAt(offset);
            if (copy_idx >= 0) {
                owner_->CopyEntry(copy_idx);
                return;
            }
            int idx = owner_->FindBlockAt(offset);
            if (idx >= 0) {
                owner_->ToggleBlock(idx);
                return;
            }
        }
    }
    BTextView::MouseDown(where);
}

void
ClickableTextView::MouseMoved(BPoint where, uint32 transit, const BMessage* dragMessage)
{
    BTextView::MouseMoved(where, transit, dragMessage);
    if (transit == B_EXITED_VIEW) return;

    static const BCursor pointer(B_CURSOR_ID_SYSTEM_DEFAULT);
    static const BCursor text_cursor(B_CURSOR_ID_I_BEAM);
    bool over_copy = owner_ && owner_->FindCopyAt(OffsetAt(where)) >= 0;
    SetViewCursor(over_copy ? &pointer : &text_cursor);
}

void
ClickableTextView::MessageReceived(BMessage* message)
{
    if (message->what == kMsgResetCopyFeedback && owner_) {
        int32 generation;
        if (message->FindInt32("generation", &generation) == B_OK)
            owner_->ResetCopyFeedback(generation);
        return;
    }
    BTextView::MessageReceived(message);
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
ChatView::AppendCopyControl(int model_idx)
{
    AppendStyled("  ", kColorCopyControl);
    int32 start = text_view_->TextLength();
    AppendStyled("[Copy]", kColorCopyControl, true);
    int32 end = text_view_->TextLength();
    AppendStyled(" ", kColorCopyControl);
    int32 feedback_start = text_view_->TextLength();
    AppendStyled("   ", kColorCopyFeedback);
    copy_ranges_.push_back({start, end, feedback_start, model_idx});
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
    ClearCopyFeedback();
    inhibit_scroll_ = true;
    text_view_->SetText("");
    header_ranges_.clear();
    copy_ranges_.clear();

    for (int i = 0; i < (int)model_.size(); i++) {
        const auto& e = model_[i];
        switch (e.kind) {
        case ChatEntry::UserText:
            AppendStyled("\nYou: ", kColorUser, true);
            AppendStyled(e.text + "\n", kColorUser, false);
            if (!e.name.empty())
                AppendStyled("[image: " + e.name + "]\n", kColorUser, false);
            break;

        case ChatEntry::AssistantText:
            AppendStyled("\nAssistant:", kColorAssistant, true);
            AppendCopyControl(i);
            AppendStyled("\n" + e.text + "\n", kColorAssistant, false);
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

        case ChatEntry::Reasoning: {
            std::string indicator = e.collapsed ? " \xe2\x96\xb6" : " \xe2\x96\xbc";
            std::string header = "\n[Thinking]" + indicator;
            int32 hstart = text_view_->TextLength();
            AppendStyled(header, kColorThinkingHeader, true);
            int32 hend = text_view_->TextLength();
            header_ranges_.push_back({hstart, hend, i});
            AppendCopyControl(i);
            AppendStyled("\n", kColorThinkingHeader);
            if (!e.collapsed && !e.text.empty()) {
                AppendStyled(e.text + "\n", kColorThinkingBody, false);
            }
            break;
        }

        case ChatEntry::CompactionSummary: {
            std::string indicator = e.collapsed ? " \xe2\x96\xb6" : " \xe2\x96\xbc";
            std::string header = "\n" + e.name + indicator + "\n";
            int32 hstart = text_view_->TextLength();
            AppendStyled(header, kColorThinkingHeader, true);
            int32 hend = text_view_->TextLength();
            header_ranges_.push_back({hstart, hend, i});
            if (!e.collapsed && !e.text.empty()) {
                AppendStyled(e.text + "\n", kColorThinkingBody, false);
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
ChatView::AppendUserText(const std::string& text,
                         const std::vector<std::string>& attachment_names)
{
    EndReasoningStreaming();
    streaming_ = false;
    std::string names;
    for (size_t i = 0; i < attachment_names.size(); ++i) {
        if (i) names += ", ";
        names += attachment_names[i];
    }
    model_.push_back({ChatEntry::UserText, text, names, true, false});
    AppendStyled("\nYou: ", kColorUser, true);
    AppendStyled(text + "\n", kColorUser, false);
    if (!names.empty())
        AppendStyled("[image: " + names + "]\n", kColorUser, false);
}

void
ChatView::AppendTextDelta(const std::string& delta)
{
    EndReasoningStreaming();
    if (!streaming_) {
        model_.push_back({ChatEntry::AssistantText, "", "", true, false});
        AppendStyled("\nAssistant:", kColorAssistant, true);
        AppendCopyControl((int)model_.size() - 1);
        AppendStyled("\n", kColorAssistant);
        streaming_ = true;
    }
    model_.back().text += delta;
    AppendStyled(delta, kColorAssistant, false);
}

void
ChatView::EndStreaming()
{
    EndReasoningStreaming();
    if (streaming_) {
        AppendStyled("\n", kColorAssistant, false);
        streaming_ = false;
    }
}

void
ChatView::AppendReasoningDelta(const std::string& delta)
{
    if (!reasoning_streaming_) {
        model_.push_back({ChatEntry::Reasoning, "", "", true, false});
        reasoning_streaming_ = true;
        int idx = (int)model_.size() - 1;
        bool collapsed = thinking_display_ == ThinkingDisplay::AlwaysCollapsed;
        model_.back().collapsed = collapsed;
        int32 hstart = text_view_->TextLength();
        AppendStyled(collapsed ? "\n[Thinking] \xe2\x96\xb6" : "\n[Thinking] \xe2\x96\xbc",
                     kColorThinkingHeader, true);
        header_ranges_.push_back({hstart, text_view_->TextLength(), idx});
        AppendCopyControl(idx);
        AppendStyled("\n", kColorThinkingHeader);
    }
    model_.back().text += delta;
    if (!model_.back().collapsed)
        AppendStyled(delta, kColorThinkingBody, false);
}

void
ChatView::EndReasoningStreaming()
{
    if (!reasoning_streaming_) return;
    reasoning_streaming_ = false;
    if (model_.empty() || model_.back().kind != ChatEntry::Reasoning) return;
    // ExpandedWhileStreaming: collapse now and re-render. AlwaysExpanded and
    // AlwaysCollapsed leave the view as-is (body already shown / hidden).
    if (thinking_display_ == ThinkingDisplay::ExpandedWhileStreaming) {
        model_.back().collapsed = true;
        if (!defer_rebuild_)
            _Rebuild();
    }
}

void
ChatView::AppendToolCalled(const std::string& tool_name, const std::string& input_json)
{
    EndReasoningStreaming();
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
    EndReasoningStreaming();
    // Collapse the tool call that just finished.
    if (pending_tool_idx_ >= 0 && pending_tool_idx_ < (int)model_.size()) {
        model_[pending_tool_idx_].collapsed = true;
        pending_tool_idx_ = -1;
    }

    model_.push_back({ChatEntry::ToolResult, output, "", success, false});
    if (!defer_rebuild_)
        _Rebuild();
}

void
ChatView::AppendSystem(const std::string& text)
{
    EndReasoningStreaming();
    streaming_ = false;
    model_.push_back({ChatEntry::System, text, "", true, false});
    AppendStyled("\n[System] " + text + "\n", kColorSystem, false);
}

void
ChatView::AppendCompactionSummary(const std::string& header,
                                  const std::string& summary)
{
    EndReasoningStreaming();
    streaming_ = false;
    model_.push_back({ChatEntry::CompactionSummary, summary, header, true, true});
    if (!defer_rebuild_)
        _Rebuild();
}

void
ChatView::Clear()
{
    ClearCopyFeedback();
    streaming_            = false;
    reasoning_streaming_  = false;
    pending_tool_idx_     = -1;
    model_.clear();
    header_ranges_.clear();
    copy_ranges_.clear();
    text_view_->SetText("");
}

void
ChatView::BeginBatch()
{
    defer_rebuild_  = true;
    inhibit_scroll_ = true;
}

void
ChatView::EndBatch()
{
    if (!defer_rebuild_) return;
    defer_rebuild_ = false;
    // _Rebuild() resets inhibit_scroll_ and ends with a single ScrollToBottom.
    _Rebuild();
}

int
ChatView::FindCopyAt(int32 offset) const
{
    for (const auto& range : copy_ranges_) {
        if (offset >= range.start && offset < range.end)
            return range.model_idx;
    }
    return -1;
}

void
ChatView::SetCopyFeedback(int model_idx, bool visible)
{
    for (const auto& range : copy_ranges_) {
        if (range.model_idx != model_idx) continue;
        int32 start = range.feedback_start;
        int32 selected_start, selected_end;
        text_view_->GetSelection(&selected_start, &selected_end);
        text_view_->Delete(start, start + 3);
        const char* label = visible ? "\xe2\x9c\x93" : "   ";
        text_view_->Insert(start, label, 3);
        BFont font(*be_bold_font);
        rgb_color color = visible ? kColorCopyFeedback : kColorCopyControl;
        text_view_->SetFontAndColor(start, start + 3, &font, B_FONT_ALL, &color);
        text_view_->Select(selected_start, selected_end);
        return;
    }
}

void
ChatView::ClearCopyFeedback()
{
    ++feedback_generation_;
    feedback_timer_.reset();
    if (feedback_idx_ >= 0)
        SetCopyFeedback(feedback_idx_, false);
    feedback_idx_ = -1;
}

void
ChatView::ResetCopyFeedback(int32 generation)
{
    if (generation == feedback_generation_)
        ClearCopyFeedback();
}

void
ChatView::CopyEntry(int model_idx)
{
    if (model_idx < 0 || model_idx >= (int)model_.size()) return;
    const ChatEntry& entry = model_[model_idx];
    if (entry.kind != ChatEntry::AssistantText && entry.kind != ChatEntry::Reasoning)
        return;

    if (!be_clipboard->Lock()) return;
    bool copied = be_clipboard->Clear() == B_OK
        && be_clipboard->Data()->AddData("text/plain", B_MIME_TYPE,
                                         entry.text.data(), entry.text.size()) == B_OK
        && be_clipboard->Commit() == B_OK;
    be_clipboard->Unlock();
    if (!copied) return;

    ClearCopyFeedback();
    feedback_idx_ = model_idx;
    SetCopyFeedback(model_idx, true);
    BMessage reset(kMsgResetCopyFeedback);
    reset.AddInt32("generation", feedback_generation_);
    feedback_timer_ = std::make_unique<BMessageRunner>(
        BMessenger(text_view_), reset, 1500000, 1);
    if (feedback_timer_->InitCheck() != B_OK)
        ClearCopyFeedback();
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
    if (model_[model_idx].kind != ChatEntry::ToolCalled
        && model_[model_idx].kind != ChatEntry::Reasoning
        && model_[model_idx].kind != ChatEntry::CompactionSummary) return;
    model_[model_idx].collapsed = !model_[model_idx].collapsed;
    _Rebuild();
}
