// ChatView markdown rendering: whatever path an assistant reply takes into
// the view — streamed in arbitrary chunks, interleaved with reasoning/tool
// entries, joined mid-stream from a batch replay, re-laid out for a new
// width — the view must show exactly (text AND per-byte font/color) what a
// fresh batch render of the same transcript shows.

#include "ChatView.h"

#include <Application.h>
#include <Font.h>
#include <TextView.h>
#include <Window.h>

#include <cstdio>
#include <random>
#include <string>
#include <vector>

static int failures = 0;

#define CHECK(cond, what) do { \
    if (!(cond)) { \
        std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #cond, \
                    std::string(what).c_str()); \
        ++failures; \
    } \
} while (0)

namespace {

struct ByteStyle {
    BFont     font;
    rgb_color color;
};

struct Snapshot {
    std::string            text;
    std::vector<ByteStyle> styles;
};

Snapshot
Snap(ChatView& view)
{
    BTextView* tv = view.TextView();
    Snapshot s;
    s.text.assign(tv->Text(), tv->TextLength());
    int32 len = tv->TextLength();
    if (len == 0) return s;
    int32 count = 0;
    text_run_array* runs = tv->RunArray(0, len, &count);
    s.styles.resize(len);
    for (int32 r = 0; runs && r < runs->count; ++r) {
        int32 end = r + 1 < runs->count ? runs->runs[r + 1].offset : len;
        for (int32 b = runs->runs[r].offset; b < end && b < len; ++b)
            s.styles[b] = {runs->runs[r].font, runs->runs[r].color};
    }
    BTextView::FreeRunArray(runs);
    return s;
}

bool
SameStyle(const ByteStyle& a, const ByteStyle& b)
{
    return a.font == b.font && a.color.red == b.color.red
        && a.color.green == b.color.green && a.color.blue == b.color.blue;
}

bool
Same(const Snapshot& a, const Snapshot& b, std::string* why)
{
    if (a.text != b.text) {
        *why = "text differs:\n--- live ---\n" + a.text + "\n--- fresh ---\n" + b.text;
        return false;
    }
    for (size_t i = 0; i < a.styles.size(); ++i) {
        if (!SameStyle(a.styles[i], b.styles[i])) {
            *why = "style differs at byte " + std::to_string(i) + " in:\n" + a.text;
            return false;
        }
    }
    return true;
}

// One transcript step, replayable live or in a batch.
struct Step {
    enum Kind { User, Delta, Reasoning, Tool, Result, End } kind;
    std::string text;
};

void
Apply(ChatView& view, const Step& s)
{
    switch (s.kind) {
    case Step::User:      view.AppendUserText(s.text); break;
    case Step::Delta:     view.AppendTextDelta(s.text); break;
    case Step::Reasoning: view.AppendReasoningDelta(s.text); break;
    case Step::Tool:      view.AppendToolCalled("read", s.text); break;
    case Step::Result:    view.AppendToolResult(s.text, true); break;
    case Step::End:       view.EndStreaming(); break;
    }
}

Snapshot
Fresh(ChatView& view, const std::vector<Step>& steps, int cols)
{
    view.Clear();
    view.SetMarkdownLayout(cols, false);
    view.BeginBatch();
    for (const auto& s : steps) Apply(view, s);
    view.EndBatch();
    return Snap(view);
}

const char* kDocs[] = {
    "# Plan\n\nSome **bold** text and `code`.\n\n- item one\n  - nested *it*\n1. first\n\n"
    "| a | b |\n|---|:-:|\n| 1 | 2 |\n| longer cell text here | x |\n\nAfter the table.\n\n"
    "```cpp\nint f(int x) { return x * 2; }\n```\n\n---\n> quote\nEnd **done**",
    "Text with a | pipe\n| h1 | h2 |\n| -- | -- |\n| c | d |\nnext para\n\n\n",
    "Short answer with [a link](https://example.org) and ~~strike~~.",
};

}  // namespace

int
main()
{
    BApplication app("application/x-vnd.haicode-test-chatview");
    BWindow* window = new BWindow(BRect(0, 0, 640, 480), "test",
                                  B_TITLED_WINDOW, 0);
    window->Lock();
    ChatView live("live");
    ChatView fresh("fresh");
    window->AddChild(live.ScrollContainer());
    window->AddChild(fresh.ScrollContainer());

    std::mt19937 rng(42);
    for (const char* doc : kDocs) {
        std::string md = doc;
        for (int trial = 0; trial < 6; ++trial) {
            const int cols = trial % 2 ? 30 : 70;
            std::vector<Step> steps = {{Step::User, "question"}};
            live.Clear();
            live.SetMarkdownLayout(cols, false);
            Apply(live, steps[0]);
            // Stream in random chunks; compare after every chunk.
            for (size_t pos = 0; pos < md.size();) {
                size_t n = std::min<size_t>(1 + rng() % (trial < 2 ? 2 : 17),
                                            md.size() - pos);
                Step s{Step::Delta, md.substr(pos, n)};
                pos += n;
                Apply(live, s);
                steps.push_back(s);
                Snapshot a = Snap(live);
                Snapshot b = Fresh(fresh, steps, cols);
                std::string why;
                CHECK(Same(a, b, &why), why);
                if (failures) break;
            }
            // A width change mid-stream re-renders the table in place, and
            // streaming continues correctly afterwards.
            int cols2 = cols == 30 ? 55 : 24;
            live.SetMarkdownLayout(cols2, false);
            Step more{Step::Delta, "\n| p | q |\n|---|---|\n| 1 | 2 |"};
            Apply(live, more);
            steps.push_back(more);
            std::string why;
            CHECK(Same(Snap(live), Fresh(fresh, steps, cols2), &why), why);

            // Reasoning, tool call and result after the reply, then a second
            // reply; ending the stream changes nothing visible.
            std::vector<Step> tail = {
                {Step::End, ""}, {Step::Reasoning, "thinking **hard**"},
                {Step::Tool, "{\"path\":\"x\"}"}, {Step::Result, "ok"},
                {Step::Delta, "Second | reply\n"}, {Step::Delta, "|-|-|\n|1|2|"},
                {Step::End, ""}};
            for (const auto& s : tail) {
                Apply(live, s);
                steps.push_back(s);
                CHECK(Same(Snap(live), Fresh(fresh, steps, cols2), &why), why);
            }
            if (failures) break;
        }
    }

    // Width change while a table is still streaming (it is the open tail):
    // the in-place re-render must resync the streaming state.
    {
        std::string md = "Intro\n| a | b |\n|---|---|\n| 1 | some long cell text here |\n"
                         "| 2 | more |\nafter";
        std::vector<Step> steps = {{Step::User, "q"}};
        live.Clear();
        live.SetMarkdownLayout(70, false);
        Apply(live, steps[0]);
        int cols = 70;
        for (size_t pos = 0; pos < md.size(); pos += 3) {
            Step s{Step::Delta, md.substr(pos, 3)};
            Apply(live, s);
            steps.push_back(s);
            if (pos % 12 == 0) {
                cols = cols == 70 ? 26 : 70;
                live.SetMarkdownLayout(cols, false);
            }
            std::string why;
            CHECK(Same(Snap(live), Fresh(fresh, steps, cols), &why), why);
            if (failures) break;
        }
    }

    // Joining mid-stream: a batch replay ends inside a reply, live deltas
    // continue after EndBatch (MainWindow's in-flight load path).
    {
        std::string md = kDocs[0];
        size_t split = md.find("| longer");
        std::vector<Step> steps = {{Step::User, "q"}, {Step::Delta, md.substr(0, split)}};
        live.Clear();
        live.SetMarkdownLayout(60, false);
        live.BeginBatch();
        for (const auto& s : steps) Apply(live, s);
        live.EndBatch();
        for (size_t pos = split; pos < md.size(); pos += 5) {
            Step s{Step::Delta, md.substr(pos, 5)};
            Apply(live, s);
            steps.push_back(s);
            std::string why;
            CHECK(Same(Snap(live), Fresh(fresh, steps, 60), &why), why);
            if (failures) break;
        }
    }

    window->Quit();
    if (failures == 0)
        std::printf("test_chat_view: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
