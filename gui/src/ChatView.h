#pragma once

#include <TextView.h>
#include <ScrollView.h>
#include <MessageRunner.h>

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
    void MouseMoved(BPoint where, uint32 transit, const BMessage* dragMessage) override;
    void MakeFocus(bool focus = true) override;
    void Select(int32 startOffset, int32 endOffset) override;
    void MessageReceived(BMessage* message) override;
private:
    ChatView* owner_ = nullptr;
};

struct ChatEntry {
    enum Kind { UserText, AssistantText, ToolCalled, ToolResult, System, Reasoning, CompactionSummary };
    Kind        kind;
    std::string text;       // content, input_json for ToolCalled
    std::string name;       // tool name for ToolCalled, header for CompactionSummary
    bool        success   = true;
    bool        collapsed = false;  // meaningful for ToolCalled and Reasoning
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
    void AppendToolResult(const std::string& output, bool success);
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
    int                          feedback_idx_ = -1;
    int32                        feedback_generation_ = 0;
    std::unique_ptr<BMessageRunner> feedback_timer_;
    // Tool calls still waiting for their result, oldest first; each result
    // collapses the oldest (parallel calls in one batch resolve in order).
    std::deque<int>              pending_tools_;
};
