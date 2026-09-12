#pragma once

#include <TextView.h>
#include <ScrollView.h>

#include <string>
#include <vector>

class ChatView;

// BTextView subclass that routes clicks on tool headers to ChatView::ToggleBlock.
class ClickableTextView : public BTextView {
public:
    ClickableTextView(BRect frame, const char* name, BRect textRect,
                      uint32 flags, uint32 resizingMode);
    void SetOwner(ChatView* owner) { owner_ = owner; }
    void MouseDown(BPoint where) override;
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

struct ToolHeaderRange {
    int32 start, end;
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
    void ToggleBlock(int model_idx);

private:
    void AppendStyled(const std::string& text, rgb_color color, bool bold = false);
    void ScrollToBottom();
    void _Rebuild();

    ClickableTextView* text_view_       = nullptr;
    BScrollView*       scroll_          = nullptr;
    bool               streaming_       = false;
    bool               reasoning_streaming_ = false;
    bool               inhibit_scroll_  = false;
    bool               defer_rebuild_   = false;

    std::vector<ChatEntry>       model_;
    std::vector<ToolHeaderRange> header_ranges_;
    int                          pending_tool_idx_ = -1;
};
