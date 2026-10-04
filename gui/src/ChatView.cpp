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

#include <memory>
#include <string>
#include <utility>
#include <vector>

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
ClickableTextView::MakeFocus(bool focus)
{
    if (focus) {
        int32 start, end;
        GetSelection(&start, &end);
        if (start == end) return;
    }
    BTextView::MakeFocus(focus);
}

void
ClickableTextView::Select(int32 startOffset, int32 endOffset)
{
    if (startOffset == endOffset && IsFocus())
        BTextView::MakeFocus(false);
    BTextView::Select(startOffset, endOffset);
    if (startOffset != endOffset)
        MakeFocus(true);
}

void
ClickableTextView::MouseMoved(BPoint where, uint32 transit, const BMessage* dragMessage)
{
    BTextView::MouseMoved(where, transit, dragMessage);
    static const BCursor pointer(B_CURSOR_ID_SYSTEM_DEFAULT);
    static const BCursor text_cursor(B_CURSOR_ID_I_BEAM);
    if (transit == B_EXITED_VIEW) {
        SetViewCursor(&pointer);
        return;
    }

    bool over_text = false;
    if (TextLength() > 0 && Bounds().Contains(where)) {
        int32 line = LineAt(where);
        if (line >= 0 && line < CountLines()) {
            float height;
            BPoint origin = PointAt(OffsetAt(line), &height);
            float width = LineWidth(line);
            over_text = width > 0 && where.x >= origin.x
                && where.x < origin.x + width && where.y >= origin.y
                && where.y < origin.y + height;
        }
    }
    bool over_copy = over_text && owner_ && owner_->FindCopyAt(OffsetAt(where)) >= 0;
    bool over_indicator = over_text && owner_ && owner_->IsIndicatorAt(where);
    SetViewCursor(over_text && !over_copy && !over_indicator ? &text_cursor : &pointer);
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
// RenderBuf: text + style runs for one or more entries, committed to the
// text view with a single Insert/SetText (one layout + one redraw instead of
// an Insert and a SetFontAndColor per styled fragment).
// ---------------------------------------------------------------------------

struct RenderBuf {
    struct Run {
        int32     offset;
        bool      bold;
        rgb_color color;
    };
    std::string                   text;
    std::vector<Run>              runs;
    std::vector<ToolHeaderRange>  headers;  // offsets relative to text
    std::vector<CopyControlRange> copies;   // offsets relative to text

    int32 Length() const { return (int32)text.size(); }

    void Add(const std::string& s, rgb_color color, bool bold = false)
    {
        if (s.empty()) return;
        const Run* last = runs.empty() ? nullptr : &runs.back();
        if (!last || last->bold != bold || last->color.red != color.red
            || last->color.green != color.green || last->color.blue != color.blue
            || last->color.alpha != color.alpha)
            runs.push_back({Length(), bold, color});
        text += s;
    }

    void AddHeader(const std::string& s, rgb_color color, int model_idx)
    {
        int32 start = Length();
        Add(s, color, true);
        headers.push_back({start, Length(), model_idx});
    }

    void AddCopyControl(int model_idx)
    {
        Add("  ", kColorCopyControl);
        int32 start = Length();
        Add("[Copy]", kColorCopyControl, true);
        int32 end = Length();
        Add(" ", kColorCopyControl);
        int32 feedback_start = Length();
        Add("   ", kColorCopyFeedback);
        copies.push_back({start, end, feedback_start, model_idx});
    }
};

namespace {

struct RunArrayDeleter {
    void operator()(text_run_array* a) const { BTextView::FreeRunArray(a); }
};
using RunArrayPtr = std::unique_ptr<text_run_array, RunArrayDeleter>;

RunArrayPtr
MakeRunArray(const RenderBuf& buf)
{
    if (buf.runs.empty()) return nullptr;
    RunArrayPtr arr(BTextView::AllocRunArray((int32)buf.runs.size()));
    if (!arr) return nullptr;
    for (size_t i = 0; i < buf.runs.size(); ++i) {
        arr->runs[i].offset = buf.runs[i].offset;
        arr->runs[i].font   = buf.runs[i].bold ? *be_bold_font : *be_plain_font;
        arr->runs[i].color  = buf.runs[i].color;
    }
    return arr;
}

}  // namespace

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
ChatView::ScrollToBottom()
{
    if (defer_rebuild_) return;
    // BTextView keeps the scroll range current on every Insert/Delete, so
    // jumping to its end is all that's needed. (This used to also shrink the
    // text rect via SetTextRect, which re-wraps the whole text and
    // invalidates the entire view on every call.)
    if (BScrollBar* vsb = scroll_->ScrollBar(B_VERTICAL)) {
        float lo, hi;
        vsb->GetRange(&lo, &hi);
        if (vsb->Value() != hi)
            vsb->SetValue(hi);
    }
}

// The canonical rendering of one entry. Live streaming paths append the same
// bytes piecewise, so a later _ReplaceEntry lands on identical layout.
void
ChatView::_RenderEntry(int i, RenderBuf& out) const
{
    const auto& e = model_[i];
    switch (e.kind) {
    case ChatEntry::UserText:
        out.Add("\nYou:", kColorUser, true);
        out.AddCopyControl(i);
        out.Add("\n" + e.text + "\n", kColorUser);
        if (!e.name.empty())
            out.Add("[image: " + e.name + "]\n", kColorUser);
        break;

    case ChatEntry::AssistantText:
        out.Add("\nAssistant:", kColorAssistant, true);
        out.AddCopyControl(i);
        out.Add("\n" + e.text + "\n", kColorAssistant);
        break;

    case ChatEntry::ToolCalled: {
        std::string indicator = e.collapsed ? " \xe2\x96\xb6" : " \xe2\x96\xbc";
        out.AddHeader("\n[Tool: " + e.name + "]" + indicator + "\n",
                      kColorToolHeader, i);
        if (!e.collapsed && !e.text.empty() && e.text != "{}")
            out.Add(e.text + "\n", kColorToolBody);
        break;
    }

    case ChatEntry::ToolResult: {
        std::string summary = e.text;
        auto nl = summary.find('\n');
        if (nl != std::string::npos) summary = summary.substr(0, nl) + " \xe2\x80\xa6";
        if (e.success)
            out.Add("[OK] " + summary + "\n", kColorToolOk);
        else
            out.Add("[ERR] " + summary + "\n", kColorToolErr);
        break;
    }

    case ChatEntry::Reasoning: {
        std::string indicator = e.collapsed ? " \xe2\x96\xb6" : " \xe2\x96\xbc";
        out.AddHeader("\n[Thinking]" + indicator, kColorThinkingHeader, i);
        out.AddCopyControl(i);
        out.Add("\n", kColorThinkingHeader);
        if (!e.collapsed && !e.text.empty())
            out.Add(e.text + "\n", kColorThinkingBody);
        break;
    }

    case ChatEntry::CompactionSummary: {
        std::string indicator = e.collapsed ? " \xe2\x96\xb6" : " \xe2\x96\xbc";
        out.AddHeader("\n" + e.name + indicator + "\n", kColorThinkingHeader, i);
        if (!e.collapsed && !e.text.empty())
            out.Add(e.text + "\n", kColorThinkingBody);
        break;
    }

    case ChatEntry::System:
        out.Add("\n[System] " + e.text + "\n", kColorSystem);
        break;
    }
}

// Insert a rendered buffer at `offset` and register its click ranges.
// Callers shift any ranges/entry starts that sit after `offset`.
void
ChatView::_InsertRendered(int32 offset, const RenderBuf& buf)
{
    if (buf.text.empty()) return;
    RunArrayPtr runs = MakeRunArray(buf);
    text_view_->Insert(offset, buf.text.data(), buf.Length(), runs.get());
    for (auto h : buf.headers) {
        h.start += offset;
        h.end   += offset;
        header_ranges_.push_back(h);
    }
    for (auto c : buf.copies) {
        c.start          += offset;
        c.end            += offset;
        c.feedback_start += offset;
        copy_ranges_.push_back(c);
    }
}

void
ChatView::_AppendRendered(const RenderBuf& buf)
{
    if (defer_rebuild_) return;
    _InsertRendered(text_view_->TextLength(), buf);
}

int
ChatView::_PushEntry(ChatEntry entry)
{
    model_.push_back(std::move(entry));
    entry_starts_.push_back(defer_rebuild_ ? 0 : text_view_->TextLength());
    return (int)model_.size() - 1;
}

void
ChatView::_AppendEntry(int model_idx)
{
    if (defer_rebuild_) return;
    RenderBuf buf;
    _RenderEntry(model_idx, buf);
    _AppendRendered(buf);
}

// Re-render one entry in place (collapse/expand). Only the text from that
// entry down is re-laid out and redrawn; the rest of the view is untouched.
void
ChatView::_ReplaceEntry(int i)
{
    if (defer_rebuild_ || i < 0 || i >= (int)model_.size()) return;

    if (feedback_idx_ == i)
        ClearCopyFeedback();

    int32 start = entry_starts_[i];
    int32 end = i + 1 < (int)model_.size() ? entry_starts_[i + 1]
                                           : text_view_->TextLength();
    RenderBuf buf;
    _RenderEntry(i, buf);
    int32 delta = buf.Length() - (end - start);

    // Drop the entry's old click ranges and shift everything after it.
    std::vector<ToolHeaderRange> headers;
    headers.reserve(header_ranges_.size());
    for (auto h : header_ranges_) {
        if (h.model_idx == i) continue;
        if (h.start >= end) { h.start += delta; h.end += delta; }
        headers.push_back(h);
    }
    header_ranges_.swap(headers);
    std::vector<CopyControlRange> copies;
    copies.reserve(copy_ranges_.size());
    for (auto c : copy_ranges_) {
        if (c.model_idx == i) continue;
        if (c.start >= end) {
            c.start += delta; c.end += delta; c.feedback_start += delta;
        }
        copies.push_back(c);
    }
    copy_ranges_.swap(copies);
    for (size_t j = i + 1; j < entry_starts_.size(); ++j)
        entry_starts_[j] += delta;

    // Insert the new rendering before removing the old one: the content
    // never transiently shrinks, so the scroll range isn't clamped (and the
    // view scrolled) mid-update.
    int32 old_len = end - start;
    _InsertRendered(start, buf);
    if (old_len > 0)
        text_view_->Delete(start + buf.Length(), start + buf.Length() + old_len);
}

// Full re-render from the model in ONE SetText — used only at EndBatch
// (history replay), never per live event.
void
ChatView::_Rebuild()
{
    ClearCopyFeedback();
    text_view_->MakeFocus(false);
    header_ranges_.clear();
    copy_ranges_.clear();

    RenderBuf buf;
    for (int i = 0; i < (int)model_.size(); i++) {
        entry_starts_[i] = buf.Length();
        _RenderEntry(i, buf);
    }
    RunArrayPtr runs = MakeRunArray(buf);
    text_view_->SetText(buf.text.data(), buf.Length(), runs.get());
    header_ranges_ = std::move(buf.headers);
    copy_ranges_   = std::move(buf.copies);
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
    _AppendEntry(_PushEntry({ChatEntry::UserText, text, names, true, false}));
    ScrollToBottom();
}

void
ChatView::AppendTextDelta(const std::string& delta)
{
    EndReasoningStreaming();
    RenderBuf buf;
    if (!streaming_ || model_.empty()
            || model_.back().kind != ChatEntry::AssistantText) {
        int idx = _PushEntry({ChatEntry::AssistantText, "", "", true, false});
        buf.Add("\nAssistant:", kColorAssistant, true);
        buf.AddCopyControl(idx);
        buf.Add("\n", kColorAssistant);
        streaming_ = true;
    }
    model_.back().text += delta;
    buf.Add(delta, kColorAssistant);
    _AppendRendered(buf);
    ScrollToBottom();
}

void
ChatView::EndStreaming()
{
    EndReasoningStreaming();
    if (streaming_) {
        RenderBuf buf;
        buf.Add("\n", kColorAssistant);
        _AppendRendered(buf);
        streaming_ = false;
    }
}

void
ChatView::AppendReasoningDelta(const std::string& delta)
{
    RenderBuf buf;
    if (!reasoning_streaming_) {
        bool collapsed = thinking_display_ == ThinkingDisplay::AlwaysCollapsed;
        int idx = _PushEntry({ChatEntry::Reasoning, "", "", true, collapsed});
        reasoning_streaming_ = true;
        buf.AddHeader(collapsed ? "\n[Thinking] \xe2\x96\xb6" : "\n[Thinking] \xe2\x96\xbc",
                      kColorThinkingHeader, idx);
        buf.AddCopyControl(idx);
        buf.Add("\n", kColorThinkingHeader);
    }
    model_.back().text += delta;
    if (!model_.back().collapsed)
        buf.Add(delta, kColorThinkingBody);
    _AppendRendered(buf);
    ScrollToBottom();
}

void
ChatView::EndReasoningStreaming()
{
    if (!reasoning_streaming_) return;
    reasoning_streaming_ = false;
    if (model_.empty() || model_.back().kind != ChatEntry::Reasoning) return;
    ChatEntry& e = model_.back();
    // ExpandedWhileStreaming: collapse now and re-render just this entry.
    // Otherwise close the expanded body the way _RenderEntry does.
    if (thinking_display_ == ThinkingDisplay::ExpandedWhileStreaming) {
        e.collapsed = true;
        _ReplaceEntry((int)model_.size() - 1);
    } else if (!e.collapsed && !e.text.empty()) {
        RenderBuf buf;
        buf.Add("\n", kColorThinkingBody);
        _AppendRendered(buf);
    }
}

void
ChatView::AppendToolCalled(const std::string& tool_name, const std::string& input_json)
{
    EndReasoningStreaming();
    streaming_ = false;
    // Shown expanded while the tool runs; collapsed when its result arrives.
    int idx = _PushEntry({ChatEntry::ToolCalled, input_json, tool_name, true, false});
    pending_tools_.push_back(idx);
    _AppendEntry(idx);
    ScrollToBottom();
}

void
ChatView::AppendToolResult(const std::string& output, bool success)
{
    EndReasoningStreaming();
    streaming_ = false;
    // Collapse the oldest tool call still waiting for its result — in place,
    // without touching the rest of the transcript.
    if (!pending_tools_.empty()) {
        int idx = pending_tools_.front();
        pending_tools_.pop_front();
        if (idx >= 0 && idx < (int)model_.size() && !model_[idx].collapsed) {
            model_[idx].collapsed = true;
            _ReplaceEntry(idx);
        }
    }
    _AppendEntry(_PushEntry({ChatEntry::ToolResult, output, "", success, false}));
    ScrollToBottom();
}

void
ChatView::AppendSystem(const std::string& text)
{
    EndReasoningStreaming();
    streaming_ = false;
    _AppendEntry(_PushEntry({ChatEntry::System, text, "", true, false}));
    ScrollToBottom();
}

void
ChatView::AppendCompactionSummary(const std::string& header,
                                  const std::string& summary)
{
    EndReasoningStreaming();
    streaming_ = false;
    _AppendEntry(_PushEntry({ChatEntry::CompactionSummary, summary, header, true, true}));
    ScrollToBottom();
}

void
ChatView::Clear()
{
    ClearCopyFeedback();
    streaming_            = false;
    reasoning_streaming_  = false;
    pending_tools_.clear();
    model_.clear();
    entry_starts_.clear();
    header_ranges_.clear();
    copy_ranges_.clear();
    text_view_->MakeFocus(false);
    text_view_->SetText("");
}

void
ChatView::BeginBatch()
{
    // While batching only the model is updated; EndBatch renders it once.
    defer_rebuild_ = true;
}

void
ChatView::EndBatch()
{
    if (!defer_rebuild_) return;
    defer_rebuild_ = false;
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
        BScrollBar* vsb = scroll_->ScrollBar(B_VERTICAL);
        float scroll_value = vsb ? vsb->Value() : 0;
        int32 selected_start, selected_end;
        text_view_->GetSelection(&selected_start, &selected_end);
        text_view_->MakeFocus(false);
        text_view_->Delete(start, start + 3);
        const char* label = visible ? "\xe2\x9c\x93" : "   ";
        text_view_->Insert(start, label, 3);
        BFont font(*be_bold_font);
        rgb_color color = visible ? kColorCopyFeedback : kColorCopyControl;
        text_view_->SetFontAndColor(start, start + 3, &font, B_FONT_ALL, &color);
        text_view_->Select(selected_start, selected_end);
        if (vsb)
            vsb->SetValue(scroll_value);
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
    if (entry.kind != ChatEntry::UserText && entry.kind != ChatEntry::AssistantText
        && entry.kind != ChatEntry::Reasoning)
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

bool
ChatView::IsIndicatorAt(BPoint where) const
{
    for (const auto& h : header_ranges_) {
        int32 end = h.end;
        if (end > h.start && text_view_->ByteAt(end - 1) == '\n')
            --end;
        int32 start = end - 3;
        if (start < h.start || text_view_->ByteAt(start) != 0xe2
            || text_view_->ByteAt(start + 1) != 0x96
            || (text_view_->ByteAt(start + 2) != 0xb6
                && text_view_->ByteAt(start + 2) != 0xbc))
            continue;

        float height;
        BPoint origin = text_view_->PointAt(start, &height);
        BFont font;
        text_view_->GetFontAndColor(start, &font);
        char indicator[] = {char(0xe2), char(0x96), char(text_view_->ByteAt(start + 2)), 0};
        if (where.x >= origin.x && where.x < origin.x + font.StringWidth(indicator)
            && where.y >= origin.y && where.y < origin.y + height)
            return true;
    }
    return false;
}

void
ChatView::ToggleBlock(int model_idx)
{
    if (model_idx < 0 || model_idx >= (int)model_.size()) return;
    if (model_[model_idx].kind != ChatEntry::ToolCalled
        && model_[model_idx].kind != ChatEntry::Reasoning
        && model_[model_idx].kind != ChatEntry::CompactionSummary) return;
    model_[model_idx].collapsed = !model_[model_idx].collapsed;
    // Keep the user's scroll position: they clicked a header in view.
    BScrollBar* vsb = scroll_->ScrollBar(B_VERTICAL);
    float scroll_value = vsb ? vsb->Value() : 0;
    _ReplaceEntry(model_idx);
    if (vsb)
        vsb->SetValue(scroll_value);
}
