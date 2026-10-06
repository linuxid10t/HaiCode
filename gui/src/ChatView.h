#pragma once

#include <TextView.h>
#include <ScrollView.h>
#include <MessageRunner.h>

#include <haicode/markdown.h>

#include <deque>
#include <memory>
#include <string>
#include <vector>

class ChatView;
struct RenderBuf;

// BTextView subclass that routes clicks on tool headers to ChatView::ToggleBlock.
class ClickableTextView : public BTextView {
public:
    ClickableTextView(BRect frame, const char* name, BRect textRect,
                      uint32 flags, uint32 resizingMode);
    void SetOwner(ChatView* owner) { owner_ = owner; }
    void MouseDown(BPoint where) override;
    void MouseUp(BPoint where) override;
    void MouseMoved(BPoint where, uint32 transit, const BMessage* dragMessage) override;
    void FrameResized(float width, float height) override;
    void MakeFocus(bool focus = true) override;
    void Select(int32 startOffset, int32 endOffset) override;
    void MessageReceived(BMessage* message) override;
private:
    ChatView* owner_ = nullptr;
    int32     pressed_link_offset_ = -1;   // offset of a primary press on a link
};

struct ChatEntry {
    enum Kind { UserText, AssistantText, ToolCalled, ToolResult, System, Reasoning, CompactionSummary };
    Kind        kind;
    std::string text;       // content, input_json for ToolCalled
    std::string name;       // tool name for ToolCalled, header for CompactionSummary
    bool        success   = true;
    bool        collapsed = false;  // meaningful for ToolCalled and Reasoning
    std::string call_id;            // ToolResult: the call it answers
};

// How [Thinking] blocks display by default; the user's manual
// click-to-toggle on a header still works in all modes.
enum class ThinkingDisplay {
    AlwaysCollapsed,       // "off"
    AlwaysExpanded,        // "on"
    ExpandedWhileStreaming // "on_while_thinking"
};

struct ToolHeaderRange {
    int32 start, end;
    int   model_idx;
};

struct CopyControlRange {
    int32 start, end;
    int32 feedback_start;
    int   model_idx;
};

// A clickable link in the view (from haicode::md::LinkSpan).
struct LinkRange {
    int32       start, end;
    int         model_idx;
    std::string target;
    bool        implicit;
};

class ChatView {
public:
    explicit ChatView(const char* name);

    // Called from MainWindow::MessageReceived (BLooper thread only).
    // attachment_names: image files attached to this prompt, rendered as a
    // "[image: …]" line after the text (may be empty).
    void AppendUserText(const std::string& text,
                        const std::vector<std::string>& attachment_names = {});
    void AppendTextDelta(const std::string& delta);
    void AppendReasoningDelta(const std::string& delta);
    void EndReasoningStreaming();
    void EndStreaming();
    // Default thinking-block display; applied by MainWindow from config
    // (initially and after each settings save).
    void SetThinkingDisplay(ThinkingDisplay d) { thinking_display_ = d; }
    void AppendToolCalled(const std::string& tool_name, const std::string& input_json);
    void AppendToolResult(const std::string& output, bool success,
                          const std::string& call_id = "");
    // Rewrite the newest result line for `call_id` in place (ask_user swaps
    // its placeholder for the picked answer; the stored row is rewritten the
    // same way, so a reload shows one line too). False when no line matches.
    bool UpdateToolResult(const std::string& call_id, const std::string& output,
                          bool success);
    void AppendSystem(const std::string& text);
    // Collapsible [context compacted] transcript entry: header line plus the
    // checkpoint summary body (collapsed by default, click to expand).
    void AppendCompactionSummary(const std::string& header,
                                 const std::string& summary);
    void Clear();

    // History replay: defer per-message rebuilds and auto-scroll until
    // EndBatch re-renders everything in a single pass.
    void BeginBatch();
    void EndBatch();

    BScrollView* ScrollContainer() const { return scroll_; }
    BTextView*   TextView() const { return text_view_; }

    // Assistant replies and compaction summaries render as markdown; tables
    // and rules are laid out to the view's fixed-font column count. A width
    // change re-renders the affected entries (debounced through
    // ViewResized -> RelayoutWidthDependent).
    void ViewResized();
    void RelayoutWidthDependent();
    // Apply a column count / border style and re-render the entries that
    // depend on it (no-op when unchanged). Public for tests.
    void SetMarkdownLayout(int cols, bool ascii_borders);

    // Links in rendered markdown open on click; relative paths resolve
    // against the session's project directory.
    void SetBaseDirectory(const std::string& dir);
    int  FindLinkAt(int32 offset) const;
    // Hand cursor? Existence is cached while the pointer stays on one link;
    // -1 (pointer off every link) clears the cache.
    bool LinkUsable(int link_idx);
    void OpenLink(int link_idx);
    const std::vector<LinkRange>& Links() const { return link_ranges_; }

    // Called by ClickableTextView::MouseDown
    int  FindBlockAt(int32 offset) const;
    bool IsIndicatorAt(BPoint where) const;
    int  FindCopyAt(int32 offset) const;
    void CopyEntry(int model_idx);
    void ResetCopyFeedback(int32 generation);
    void ToggleBlock(int model_idx);

private:
    // Rendering is incremental: each model entry owns the byte range
    // [entry_starts_[i], entry_starts_[i + 1]) of the text view. Appends
    // insert at the end, and a collapse/expand re-renders only the one
    // entry in place. A full SetText-based rebuild happens only at EndBatch;
    // rebuilding the whole view per tool result left it blank (app_server
    // erases invalidated regions to the view color at once, while Draw waits
    // for the looper to finish the rebuild) — the white flash on tool calls.
    int  _PushEntry(ChatEntry entry);
    void _RenderEntry(int model_idx, RenderBuf& out) const;
    void _InsertRendered(int32 offset, const RenderBuf& buf);
    void _AppendRendered(const RenderBuf& buf);
    void _AppendEntry(int model_idx);
    void _ReplaceEntry(int model_idx);
    void SetCopyFeedback(int model_idx, bool visible);
    void ClearCopyFeedback();
    void ScrollToBottom();
    void _Rebuild();
    // Streamed assistant text: re-render only the open tail of the reply
    // (haicode::md::render_from) and touch only the bytes that changed.
    void _StreamMarkdown();
    // Re-derive the streaming state from the canonical rendering of the
    // last entry (after a full rebuild or an in-place re-render of it).
    void _ResyncStream();

    ClickableTextView* text_view_       = nullptr;
    BScrollView*       scroll_          = nullptr;
    bool               streaming_       = false;
    bool               reasoning_streaming_ = false;
    bool               defer_rebuild_   = false;
    ThinkingDisplay    thinking_display_ = ThinkingDisplay::AlwaysCollapsed;

    std::vector<ChatEntry>        model_;
    std::vector<int32>            entry_starts_;  // parallel to model_
    std::vector<ToolHeaderRange>  header_ranges_;
    std::vector<CopyControlRange> copy_ranges_;
    std::vector<LinkRange>        link_ranges_;
    std::string                   base_dir_;
    // Hover cache: MouseMoved fires constantly, so a file link is stat()ed
    // once per entry into it rather than per move.
    std::string                   hover_target_;
    bool                          hover_usable_ = false;
    int                          feedback_idx_ = -1;
    int32                        feedback_generation_ = 0;
    std::unique_ptr<BMessageRunner> feedback_timer_;
    // Tool calls still waiting for their result, oldest first; each result
    // collapses the oldest (parallel calls in one batch resolve in order).
    std::deque<int>              pending_tools_;

    haicode::md::Options            md_opts_;
    std::unique_ptr<BMessageRunner> relayout_timer_;
    // Streaming assistant entry: source bytes before frozen_src are rendered
    // for good; `tail` is what the view currently shows for the rest (the
    // last tail.text.size() bytes of the view).
    struct MdStream {
        int                 idx = -1;
        size_t              frozen_src = 0;
        haicode::md::State  state;
        haicode::md::Styled tail;
    } md_stream_;
};
