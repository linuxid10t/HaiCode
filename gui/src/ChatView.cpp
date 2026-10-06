#include "ChatView.h"
#include "MarkdownView.h"

#include <Clipboard.h>
#include <Cursor.h>
#include <Font.h>
#include <Message.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <TextView.h>
#include <Window.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
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
static const uint32 kMsgRelayout           = 'MDrl';
static const bigtime_t kRelayoutDelay      = 150000;  // resize debounce

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
            // Links open on release (MouseUp), so text in them stays
            // drag-selectable.
            pressed_link_offset_ = MarkdownPointOverText(this, where)
                && owner_->FindLinkAt(offset) >= 0 ? offset : -1;
        }
    }
    BTextView::MouseDown(where);
}

void
ClickableTextView::MouseUp(BPoint where)
{
    BTextView::MouseUp(where);
    int32 pressed = pressed_link_offset_;
    pressed_link_offset_ = -1;
    if (pressed < 0 || !owner_) return;
    // A click, not a drag-select: no selection and still on the same link.
    int32 start, end;
    GetSelection(&start, &end);
    int link = owner_->FindLinkAt(pressed);
    if (start == end && link >= 0 && MarkdownPointOverText(this, where)
        && owner_->FindLinkAt(OffsetAt(where)) == link)
        owner_->OpenLink(link);
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
    static const BCursor hand(B_CURSOR_ID_FOLLOW_LINK);
    if (transit == B_EXITED_VIEW) {
        if (owner_) owner_->LinkUsable(-1);
        SetViewCursor(&pointer);
        return;
    }

    bool over_text = MarkdownPointOverText(this, where);
    bool over_copy = over_text && owner_ && owner_->FindCopyAt(OffsetAt(where)) >= 0;
    bool over_indicator = over_text && owner_ && owner_->IsIndicatorAt(where);
    int link = over_text && owner_ ? owner_->FindLinkAt(OffsetAt(where)) : -1;
    bool over_link = owner_ && owner_->LinkUsable(link);
    if (over_link)
        SetViewCursor(&hand);
    else
        SetViewCursor(over_text && !over_copy && !over_indicator ? &text_cursor : &pointer);
}

void
ClickableTextView::FrameResized(float width, float height)
{
    BTextView::FrameResized(width, height);
    if (owner_)
        owner_->ViewResized();
}

void
ClickableTextView::MessageReceived(BMessage* message)
{
    if (message->what == kMsgRelayout && owner_) {
        owner_->RelayoutWidthDependent();
        return;
    }
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
        uint16    flags;    // haicode::md::StyleFlag bits
        uint8     heading;
        rgb_color color;
    };
    std::string                   text;
    std::vector<Run>              runs;
    std::vector<ToolHeaderRange>  headers;  // offsets relative to text
    std::vector<CopyControlRange> copies;   // offsets relative to text
    std::vector<LinkRange>        links;    // offsets relative to text

    int32 Length() const { return (int32)text.size(); }

    void Add(const std::string& s, rgb_color color, bool bold = false)
    {
        AddStyled(s, color, bold ? haicode::md::kBold : 0, 0);
    }

    void AddStyled(std::string_view s, rgb_color color, uint16 flags, uint8 heading)
    {
        if (s.empty()) return;
        const Run* last = runs.empty() ? nullptr : &runs.back();
        if (!last || last->flags != flags || last->heading != heading
            || last->color.red != color.red || last->color.green != color.green
            || last->color.blue != color.blue || last->color.alpha != color.alpha)
            runs.push_back({Length(), flags, heading, color});
        text.append(s);
    }

    void AddMarkdown(const haicode::md::Styled& md, const MarkdownPalette& palette,
                     int model_idx)
    {
        for (const auto& l : md.links)
            links.push_back({Length() + (int32)l.start, Length() + (int32)l.end,
                             model_idx, l.target, l.implicit});
        for (size_t i = 0; i < md.runs.size(); ++i) {
            size_t start = md.runs[i].offset;
            size_t end = i + 1 < md.runs.size() ? md.runs[i + 1].offset
                                                : md.text.size();
            AddStyled(std::string_view(md.text).substr(start, end - start),
                      MarkdownColor(md.runs[i].flags, palette),
                      md.runs[i].flags, md.runs[i].heading);
        }
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

TextRunArrayPtr
MakeRunArray(const RenderBuf& buf)
{
    if (buf.runs.empty()) return nullptr;
    TextRunArrayPtr arr(BTextView::AllocRunArray((int32)buf.runs.size()));
    if (!arr) return nullptr;
    for (size_t i = 0; i < buf.runs.size(); ++i) {
        arr->runs[i].offset = buf.runs[i].offset;
        arr->runs[i].font   = MarkdownFont(buf.runs[i].flags, buf.runs[i].heading);
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

    md_opts_.max_cols = MarkdownColumns(text_view_);
    md_opts_.ascii_borders = MarkdownAsciiBorders();
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

ChatView::ScrollAnchor
ChatView::_AnchorScroll() const
{
    ScrollAnchor a;
    if (BScrollBar* vsb = scroll_->ScrollBar(B_VERTICAL)) {
        float lo, hi;
        vsb->GetRange(&lo, &hi);
        a.value = vsb->Value();
        a.at_bottom = a.value >= hi - 1;
    }
    return a;
}

void
ChatView::_FollowScroll(const ScrollAnchor& anchor)
{
    if (anchor.at_bottom) {
        ScrollToBottom();
        return;
    }
    // Scrolled up: keep the reading position (an append at the end never
    // moves it by itself; this guards any in-place edit that did).
    BScrollBar* vsb = scroll_->ScrollBar(B_VERTICAL);
    if (vsb && vsb->Value() != anchor.value)
        vsb->SetValue(anchor.value);
}

// The canonical rendering of one entry. Live streaming paths append the same
// bytes piecewise, so a later _ReplaceEntry lands on identical layout.
//
// Line breaks LEAD rather than trail: every entry after the first opens with
// the "\n" that ends the previous one, and no entry ends with one. So the
// transcript never ends in "\n" — a trailing newline is an empty last line
// that shows as a blank line under the newest entry when scrolled to the
// bottom — and streamed text appends at the very end with nothing to step
// around.
void
ChatView::_RenderEntry(int i, RenderBuf& out) const
{
    const auto& e = model_[i];
    auto separator = [&](rgb_color color) {
        if (i > 0) out.Add("\n", color);
    };
    switch (e.kind) {
    case ChatEntry::UserText:
        separator(kColorUser);
        out.Add("\nYou:", kColorUser, true);
        out.AddCopyControl(i);
        out.Add("\n" + e.text, kColorUser);
        if (!e.name.empty())
            out.Add("\n[image: " + e.name + "]", kColorUser);
        break;

    case ChatEntry::AssistantText:
        // The body is rendered markdown; the streaming path shows the same
        // bytes (render_from's frozen units + open tail == render).
        separator(kColorAssistant);
        out.Add("\nAssistant:", kColorAssistant, true);
        out.AddCopyControl(i);
        out.Add("\n", kColorAssistant);
        out.AddMarkdown(haicode::md::render(e.text, md_opts_),
                        MarkdownPaletteFor(kColorAssistant), i);
        break;

    case ChatEntry::ToolCalled: {
        separator(kColorToolHeader);
        std::string indicator = e.collapsed ? " \xe2\x96\xb6" : " \xe2\x96\xbc";
        out.AddHeader("\n[Tool: " + e.name + "]" + indicator, kColorToolHeader, i);
        if (!e.collapsed && !e.text.empty() && e.text != "{}")
            out.Add("\n" + e.text, kColorToolBody);
        break;
    }

    case ChatEntry::ToolResult: {
        std::string summary = e.text;
        auto nl = summary.find('\n');
        if (nl != std::string::npos) summary = summary.substr(0, nl) + " \xe2\x80\xa6";
        rgb_color color = e.success ? kColorToolOk : kColorToolErr;
        separator(color);
        out.Add((e.success ? "[OK] " : "[ERR] ") + summary, color);
        break;
    }

    case ChatEntry::Reasoning: {
        separator(kColorThinkingHeader);
        std::string indicator = e.collapsed ? " \xe2\x96\xb6" : " \xe2\x96\xbc";
        out.AddHeader("\n[Thinking]" + indicator, kColorThinkingHeader, i);
        out.AddCopyControl(i);
        if (!e.collapsed && !e.text.empty()) {
            out.Add("\n", kColorThinkingHeader);
            out.Add(e.text, kColorThinkingBody);
        }
        break;
    }

    case ChatEntry::CompactionSummary: {
        separator(kColorThinkingHeader);
        std::string indicator = e.collapsed ? " \xe2\x96\xb6" : " \xe2\x96\xbc";
        out.AddHeader("\n" + e.name + indicator, kColorThinkingHeader, i);
        if (!e.collapsed && !e.text.empty()) {
            out.Add("\n", kColorThinkingBody);
            out.AddMarkdown(haicode::md::render(e.text, md_opts_),
                            MarkdownPaletteFor(kColorThinkingBody), i);
        }
        break;
    }

    case ChatEntry::System:
        separator(kColorSystem);
        out.Add("\n[System] " + e.text, kColorSystem);
        break;
    }
}

// Insert a rendered buffer at `offset` and register its click ranges.
// Callers shift any ranges/entry starts that sit after `offset`.
void
ChatView::_InsertRendered(int32 offset, const RenderBuf& buf)
{
    if (buf.text.empty()) return;
    TextRunArrayPtr runs = MakeRunArray(buf);
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
    for (auto l : buf.links) {
        l.start += offset;
        l.end   += offset;
        link_ranges_.push_back(std::move(l));
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
    std::vector<LinkRange> links;
    links.reserve(link_ranges_.size());
    for (auto& l : link_ranges_) {
        if (l.model_idx == i) continue;
        if (l.start >= end) { l.start += delta; l.end += delta; }
        links.push_back(std::move(l));
    }
    link_ranges_.swap(links);
    for (size_t j = i + 1; j < entry_starts_.size(); ++j)
        entry_starts_[j] += delta;

    // Insert the new rendering before removing the old one: the content
    // never transiently shrinks, so the scroll range isn't clamped (and the
    // view scrolled) mid-update.
    int32 old_len = end - start;
    _InsertRendered(start, buf);
    if (old_len > 0)
        _DeleteText(start + buf.Length(), start + buf.Length() + old_len);

    if (i == md_stream_.idx)
        _ResyncStream();
}

// BTextView::Delete() collapses the selection onto the caret and scrolls the
// caret into view. A SetText rebuild (every session load) leaves the caret at
// offset 0, and appends at the end never move it, so in a session that was
// switched to, each in-place re-render — the streamed markdown tail, a
// collapse, the copy feedback — jumped the view to the top and back: two
// full-view scrolls and repaints per update, and a streamed reply could stay
// undrawn until something else scrolled the view. Park an unselected caret on
// a line in the middle of the view (the top line may be only partly visible,
// which still scrolls), where a delete below it leaves it in view, and put
// the scroll position back in case the delete still moved it.
void
ChatView::_DeleteText(int32 start, int32 end)
{
    int32 sel_start, sel_end;
    text_view_->GetSelection(&sel_start, &sel_end);
    if (sel_start == sel_end) {
        BRect bounds = text_view_->Bounds();
        int32 park = text_view_->OffsetAt(
            BPoint(bounds.left, bounds.top + bounds.Height() / 2));
        if (park != sel_start)
            text_view_->Select(park, park);
    }
    BScrollBar* vsb = scroll_->ScrollBar(B_VERTICAL);
    float value = vsb ? vsb->Value() : 0;
    text_view_->Delete(start, end);
    if (vsb && vsb->Value() != value)
        vsb->SetValue(value);
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
    link_ranges_.clear();

    RenderBuf buf;
    for (int i = 0; i < (int)model_.size(); i++) {
        entry_starts_[i] = buf.Length();
        _RenderEntry(i, buf);
    }
    TextRunArrayPtr runs = MakeRunArray(buf);
    text_view_->SetText(buf.text.data(), buf.Length(), runs.get());
    header_ranges_ = std::move(buf.headers);
    copy_ranges_   = std::move(buf.copies);
    link_ranges_   = std::move(buf.links);
    _ResyncStream();
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
    _AppendEntry(_PushEntry({ChatEntry::UserText, text, names, true, false, {}}));
    ScrollToBottom();
}

void
ChatView::AppendTextDelta(const std::string& delta)
{
    ScrollAnchor anchor = _AnchorScroll();
    EndReasoningStreaming();
    if (!streaming_ || model_.empty()
            || model_.back().kind != ChatEntry::AssistantText) {
        // Header first (canonical rendering of the still-empty entry), then
        // each delta lands at the very end of the text.
        _AppendEntry(_PushEntry({ChatEntry::AssistantText, "", "", true, false, {}}));
        streaming_ = true;
        md_stream_ = MdStream{};
        md_stream_.idx = (int)model_.size() - 1;
    }
    model_.back().text += delta;
    _StreamMarkdown();
    _FollowScroll(anchor);
}

void
ChatView::_StreamMarkdown()
{
    if (defer_rebuild_) return;
    int last = (int)model_.size() - 1;
    if (md_stream_.idx != last || model_[last].kind != ChatEntry::AssistantText)
        return;

    // Units that can no longer change are frozen; only the open tail (the
    // incomplete last line, a table still receiving rows, ...) re-renders.
    // The previous tail is the last tail.text.size() bytes of the view.
    haicode::md::Incremental inc = haicode::md::render_from(
        model_[last].text, md_stream_.frozen_src, md_stream_.state, md_opts_);
    haicode::md::Styled next = std::move(inc.frozen);
    next.append(inc.tail);

    // Replace only what differs from what is already shown — usually the
    // delta is a pure append and nothing is deleted.
    size_t keep = haicode::md::common_prefix(md_stream_.tail, next);
    int32 tail_start = text_view_->TextLength() - (int32)md_stream_.tail.text.size();
    int32 at = tail_start + (int32)keep;
    int32 old_rest = (int32)(md_stream_.tail.text.size() - keep);
    RenderBuf buf;
    buf.AddMarkdown(next.slice(keep), MarkdownPaletteFor(kColorAssistant), last);
    buf.links.clear();  // re-registered below for the whole replaced region
    // Insert before deleting so the content never transiently shrinks.
    _InsertRendered(at, buf);
    if (old_rest > 0)
        _DeleteText(at + buf.Length(), at + buf.Length() + old_rest);

    // Links of the old tail go; every link of `next` (which starts where the
    // old tail started) is registered — a kept prefix may hold a link whose
    // target changed, and nothing after the tail exists to shift.
    link_ranges_.erase(std::remove_if(link_ranges_.begin(), link_ranges_.end(),
        [&](const LinkRange& l) { return l.model_idx == last && l.start >= tail_start; }),
        link_ranges_.end());
    for (const auto& l : next.links)
        link_ranges_.push_back({tail_start + (int32)l.start, tail_start + (int32)l.end,
                                last, l.target, l.implicit});

    md_stream_.frozen_src = inc.frozen_end;
    md_stream_.state = inc.state;
    md_stream_.tail = std::move(inc.tail);
}

void
ChatView::_ResyncStream()
{
    md_stream_ = MdStream{};
    if (!streaming_ || model_.empty()
            || model_.back().kind != ChatEntry::AssistantText)
        return;
    // The view ends with the canonical rendering of the last entry, whose
    // body is frozen + tail of a from-scratch render_from.
    haicode::md::Incremental inc = haicode::md::render_from(
        model_.back().text, 0, haicode::md::State{}, md_opts_);
    md_stream_.idx = (int)model_.size() - 1;
    md_stream_.frozen_src = inc.frozen_end;
    md_stream_.state = inc.state;
    md_stream_.tail = std::move(inc.tail);
}

void
ChatView::EndStreaming()
{
    EndReasoningStreaming();
    streaming_ = false;
    // The view already shows the canonical rendering; nothing to redo.
    md_stream_ = MdStream{};
}

void
ChatView::ViewResized()
{
    if (MarkdownColumns(text_view_) == md_opts_.max_cols) {
        relayout_timer_.reset();
        return;
    }
    // Debounce: each resize event restarts the timer, so a live window drag
    // re-renders tables once it settles rather than on every step.
    BMessage msg(kMsgRelayout);
    relayout_timer_ = std::make_unique<BMessageRunner>(
        BMessenger(text_view_), msg, kRelayoutDelay, 1);
    if (relayout_timer_->InitCheck() != B_OK) {
        relayout_timer_.reset();
        RelayoutWidthDependent();
    }
}

void
ChatView::RelayoutWidthDependent()
{
    relayout_timer_.reset();
    SetMarkdownLayout(MarkdownColumns(text_view_), MarkdownAsciiBorders());
}

void
ChatView::SetMarkdownLayout(int cols, bool ascii_borders)
{
    if (cols == md_opts_.max_cols && ascii_borders == md_opts_.ascii_borders)
        return;
    md_opts_.max_cols = cols;
    md_opts_.ascii_borders = ascii_borders;
    if (defer_rebuild_) return;  // EndBatch renders with the new layout

    ScrollAnchor anchor = _AnchorScroll();
    for (int i = 0; i < (int)model_.size(); ++i) {
        const ChatEntry& e = model_[i];
        bool markdown = e.kind == ChatEntry::AssistantText
            || (e.kind == ChatEntry::CompactionSummary && !e.collapsed);
        if (markdown && haicode::md::width_dependent(e.text))
            _ReplaceEntry(i);
    }
    _FollowScroll(anchor);
}

void
ChatView::AppendReasoningDelta(const std::string& delta)
{
    ScrollAnchor anchor = _AnchorScroll();
    if (!reasoning_streaming_) {
        bool collapsed = thinking_display_ == ThinkingDisplay::AlwaysCollapsed;
        _AppendEntry(_PushEntry({ChatEntry::Reasoning, "", "", true, collapsed, {}}));
        reasoning_streaming_ = true;
    }
    ChatEntry& e = model_.back();
    RenderBuf buf;
    if (!e.collapsed && !delta.empty()) {
        // The body's line break comes with its first text, as in _RenderEntry.
        if (e.text.empty())
            buf.Add("\n", kColorThinkingHeader);
        buf.Add(delta, kColorThinkingBody);
    }
    e.text += delta;
    _AppendRendered(buf);
    _FollowScroll(anchor);
}

void
ChatView::EndReasoningStreaming()
{
    if (!reasoning_streaming_) return;
    reasoning_streaming_ = false;
    if (model_.empty() || model_.back().kind != ChatEntry::Reasoning) return;
    // ExpandedWhileStreaming: collapse now and re-render just this entry.
    // AlwaysExpanded and AlwaysCollapsed leave the view as-is.
    if (thinking_display_ == ThinkingDisplay::ExpandedWhileStreaming) {
        model_.back().collapsed = true;
        _ReplaceEntry((int)model_.size() - 1);
    }
}

void
ChatView::AppendToolCalled(const std::string& tool_name, const std::string& input_json)
{
    ScrollAnchor anchor = _AnchorScroll();
    EndReasoningStreaming();
    streaming_ = false;
    // Shown expanded while the tool runs; collapsed when its result arrives.
    int idx = _PushEntry({ChatEntry::ToolCalled, input_json, tool_name, true, false, {}});
    pending_tools_.push_back(idx);
    _AppendEntry(idx);
    _FollowScroll(anchor);
}

void
ChatView::AppendToolResult(const std::string& output, bool success,
                           const std::string& call_id)
{
    ScrollAnchor anchor = _AnchorScroll();
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
    _AppendEntry(_PushEntry({ChatEntry::ToolResult, output, "", success, false,
                             call_id}));
    _FollowScroll(anchor);
}

bool
ChatView::UpdateToolResult(const std::string& call_id, const std::string& output,
                           bool success)
{
    if (call_id.empty()) return false;
    // Newest first: some OpenAI-compatible servers reuse call ids across
    // turns, and the store rewrites the newest matching row as well.
    for (int i = (int)model_.size() - 1; i >= 0; --i) {
        ChatEntry& e = model_[i];
        if (e.kind != ChatEntry::ToolResult || e.call_id != call_id) continue;
        e.text = output;
        e.success = success;
        _ReplaceEntry(i);
        return true;
    }
    return false;
}

void
ChatView::AppendSystem(const std::string& text)
{
    ScrollAnchor anchor = _AnchorScroll();
    EndReasoningStreaming();
    streaming_ = false;
    _AppendEntry(_PushEntry({ChatEntry::System, text, "", true, false, {}}));
    _FollowScroll(anchor);
}

void
ChatView::AppendCompactionSummary(const std::string& header,
                                  const std::string& summary)
{
    ScrollAnchor anchor = _AnchorScroll();
    EndReasoningStreaming();
    streaming_ = false;
    _AppendEntry(_PushEntry({ChatEntry::CompactionSummary, summary, header, true, true, {}}));
    _FollowScroll(anchor);
}

void
ChatView::Clear()
{
    ClearCopyFeedback();
    streaming_            = false;
    reasoning_streaming_  = false;
    pending_tools_.clear();
    md_stream_ = MdStream{};
    model_.clear();
    entry_starts_.clear();
    header_ranges_.clear();
    copy_ranges_.clear();
    link_ranges_.clear();
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

void
ChatView::SetBaseDirectory(const std::string& dir)
{
    base_dir_ = dir;
    LinkUsable(-1);
}

int
ChatView::FindLinkAt(int32 offset) const
{
    for (size_t i = 0; i < link_ranges_.size(); ++i) {
        if (offset >= link_ranges_[i].start && offset < link_ranges_[i].end)
            return (int)i;
    }
    return -1;
}

bool
ChatView::LinkUsable(int link_idx)
{
    if (link_idx < 0 || link_idx >= (int)link_ranges_.size()) {
        hover_target_.clear();
        return false;
    }
    const std::string& target = link_ranges_[link_idx].target;
    if (hover_target_.empty() || hover_target_ != target) {
        hover_target_ = target;
        hover_usable_ = MarkdownLinkUsable(target, base_dir_);
    }
    return hover_usable_;
}

void
ChatView::OpenLink(int link_idx)
{
    if (link_idx < 0 || link_idx >= (int)link_ranges_.size()) return;
    OpenMarkdownLink(link_ranges_[link_idx].target, base_dir_);
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

        // Swap the 3-byte slot like _ReplaceEntry: insert the new bytes WITH
        // their style run, then delete the old ones. Never SetFontAndColor
        // here: with a font mode it calls InvalidateLayout(), so a relayout
        // re-wraps the whole transcript AFTER this returns — after the scroll
        // position below was put back (a copy click moved the view).
        RenderBuf buf;
        if (visible)
            buf.Add("\xe2\x9c\x93", kColorCopyFeedback, true);
        else
            buf.Add("   ", kColorCopyFeedback);  // as AddCopyControl renders it
        TextRunArrayPtr runs = MakeRunArray(buf);
        text_view_->Insert(start, buf.text.data(), buf.Length(), runs.get());
        _DeleteText(start + buf.Length(), start + buf.Length() + 3);

        // Put back a real selection (Insert collapses it). A bare caret
        // stays where _DeleteText parked it, in view: restoring it would put
        // it back where appends left it — the end of the transcript — for
        // the next caret scroll to jump to.
        if (selected_start != selected_end)
            text_view_->Select(selected_start, selected_end);
        if (vsb && vsb->Value() != scroll_value)
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
        // A click right of a header line lands on its line break, which the
        // range doesn't include (the "\n" belongs to what follows).
        if (offset >= h.start && (offset < h.end
                || (offset == h.end && offset < text_view_->TextLength()
                    && text_view_->ByteAt(offset) == '\n')))
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
