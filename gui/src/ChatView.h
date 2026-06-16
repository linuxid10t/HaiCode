#pragma once

#include <TextView.h>
#include <ScrollView.h>

#include <string>

class ChatView {
public:
    explicit ChatView(const char* name);

    // Called from MainWindow::MessageReceived (BLooper thread only)
    void AppendUserText(const std::string& text);
    void AppendTextDelta(const std::string& delta);
    void EndStreaming();
    void AppendToolCalled(const std::string& tool_name, const std::string& input_json);
    void AppendToolResult(const std::string& output, bool success);
    void AppendSystem(const std::string& text);
    void Clear();

    BScrollView* ScrollContainer() const { return scroll_; }

private:
    void AppendStyled(const std::string& text, rgb_color color, bool bold = false);
    void ScrollToBottom();

    BTextView*   text_view_ = nullptr;
    BScrollView* scroll_    = nullptr;
    bool         streaming_ = false;
};
