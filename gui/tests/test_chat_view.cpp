// ChatView markdown rendering: whatever path an assistant reply takes into
// the view — streamed in arbitrary chunks, interleaved with reasoning/tool
// entries, joined mid-stream from a batch replay, re-laid out for a new
// width — the view must show exactly (text AND per-byte font/color) what a
// fresh batch render of the same transcript shows.

#include "ChatView.h"
#include "MarkdownView.h"

#include <Application.h>
#include <Font.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <TextView.h>
#include <Window.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <tuple>
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
    // (start, end, target), sorted — registration order differs between
    // the streaming and batch paths.
    std::vector<std::tuple<int32, int32, std::string>> links;
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
    for (const auto& l : view.Links())
        s.links.emplace_back(l.start, l.end, l.target);
    std::sort(s.links.begin(), s.links.end());
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
    if (a.links != b.links) {
        *why = "links differ (" + std::to_string(a.links.size()) + " live vs "
            + std::to_string(b.links.size()) + " fresh) in:\n" + a.text;
        return false;
    }
    for (const auto& [start, end, target] : a.links) {
        if (start < 0 || end > (int32)a.text.size() || start >= end) {
            *why = "link out of range: " + target;
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
    "Wrote `lib/a.cpp:12` and [the plan](.haicode/plans/p.md); docs at https://x.org/a(b).\n"
    "| file | url |\n|---|---|\n| `src/x.cpp` | [w](https://w.org) long cell text to wrap |\n"
    "![shot](/tmp/s.png) <https://end.org>",
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
                {Step::Tool, "{\"path\":\"x\"}"},
                // A reply with links lands before the tool result, whose
                // collapse re-renders the (earlier) tool entry in place:
                // the later entry's links must shift with it.
                {Step::Delta, "Mid [l](https://l.org) and `a/b.md`"}, {Step::End, ""},
                {Step::Result, "ok"},
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

    // Link ranges point at the right bytes of the view, and FindLinkAt
    // hits them (and only them).
    {
        live.Clear();
        live.SetMarkdownLayout(70, false);
        live.AppendUserText("q");
        std::string md = "Intro line.\nSee [a link](https://example.org) and `lib/a.cpp`.";
        for (size_t pos = 0; pos < md.size(); pos += 4)
            live.AppendTextDelta(md.substr(pos, 4));
        live.EndStreaming();
        std::string text(live.TextView()->Text(), live.TextView()->TextLength());
        CHECK(live.Links().size() == 2, "two links");
        for (const auto& l : live.Links()) {
            std::string covered = text.substr(l.start, l.end - l.start);
            if (l.target == "https://example.org")
                CHECK(covered == "a link (https://example.org)", covered);
            else
                CHECK(l.target == "lib/a.cpp" && covered == "lib/a.cpp", covered);
            CHECK(live.FindLinkAt(l.start) >= 0 && live.FindLinkAt(l.end - 1) >= 0
                  && live.FindLinkAt(l.end) < 0, "FindLinkAt bounds");
        }
        CHECK(live.FindLinkAt(0) < 0, "no link on the header");
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

    // [Copy] feedback: the click must not move the view, the "✓" swaps in
    // place, and clearing it (copying another entry) restores exactly the
    // bytes and styles a fresh render shows.
    {
        std::vector<Step> steps;
        for (int i = 0; i < 40; ++i) {
            steps.push_back({Step::User, "question " + std::to_string(i)});
            steps.push_back({Step::Delta, "Answer **" + std::to_string(i)
                                          + "** with a few words of text."});
            steps.push_back({Step::End, ""});
        }
        Snapshot want = Fresh(fresh, steps, 60);
        live.Clear();
        live.SetMarkdownLayout(60, false);
        live.BeginBatch();
        for (const auto& s : steps) Apply(live, s);
        live.EndBatch();

        // Feedback slots: the 3 bytes after "[Copy] ".
        std::vector<int32> slots;
        for (size_t p = want.text.find("[Copy] "); p != std::string::npos;
             p = want.text.find("[Copy] ", p + 1))
            slots.push_back((int32)p + 7);
        CHECK(slots.size() >= 4, "copy controls rendered");

        BScrollBar* vsb = live.ScrollContainer()->ScrollBar(B_VERTICAL);
        float lo = 0, hi = 0;
        if (vsb) vsb->GetRange(&lo, &hi);
        CHECK(vsb && hi > 0, "transcript must be taller than the view");

        // `live` with `slot` patched to fresh's bytes must equal fresh.
        auto same_except = [&](int32 slot, std::string* why) {
            Snapshot s = Snap(live);
            if (s.text.size() != want.text.size()) {
                *why = "length changed";
                return false;
            }
            for (int32 b = slot; b < slot + 3; ++b) {
                s.text[b] = want.text[b];
                s.styles[b] = want.styles[b];
            }
            return Same(s, want, why);
        };

        for (float at : {0.0f, hi / 2}) {
            if (!vsb || slots.size() < 4) break;
            vsb->SetValue(at);
            float before = vsb->Value();
            int a = live.FindCopyAt(slots[0] - 2);  // inside "[Copy]"
            int b = live.FindCopyAt(slots[1] - 2);
            CHECK(a >= 0 && b >= 0 && a != b, "copy controls hit");
            if (a < 0 || b < 0) break;

            live.CopyEntry(a);
            CHECK(vsb->Value() == before, "copy must not scroll the view");
            Snapshot s = Snap(live);
            CHECK(s.text.compare(slots[0], 3, "\xe2\x9c\x93") == 0,
                  "feedback shown in the slot");
            std::string why;
            CHECK(same_except(slots[0], &why), why);

            live.CopyEntry(b);  // clears a's feedback, shows b's
            CHECK(vsb->Value() == before, "second copy must not scroll");
            CHECK(same_except(slots[1], &why), why);
            CHECK(Snap(live).text.compare(slots[1], 3, "\xe2\x9c\x93") == 0,
                  "feedback moved to the second entry");
        }
    }

    // What a click would do (planned only — nothing is launched here).
    {
        char tmpl[] = "/tmp/haicode_linktest_XXXXXX";
        std::string dir = mkdtemp(tmpl);
        auto touch = [&](const std::string& name, mode_t mode) {
            std::string path = dir + "/" + name;
            if (FILE* f = std::fopen(path.c_str(), "w")) std::fclose(f);
            chmod(path.c_str(), mode);
            return path;
        };
        std::string doc = touch("notes.md", 0644);
        std::string script = touch("run.sh", 0755);
        std::string pkg = touch("thing.hpkg", 0644);
        mkdir((dir + "/sub").c_str(), 0755);

        auto plan = [&](const std::string& t) { return PlanMarkdownLink(t, dir); };
        MarkdownLinkAction a = plan("notes.md:7");
        CHECK(a.kind == MarkdownLinkAction::OpenFile && a.target == doc && a.line == 7, "doc at line");
        a = plan("file://" + doc);
        CHECK(a.kind == MarkdownLinkAction::OpenFile && a.target == doc, "file url");
        a = plan("run.sh");
        CHECK(a.kind == MarkdownLinkAction::OpenFolder && a.target == dir && a.select == script,
              "executable only reveals, selected");
        a = plan("thing.hpkg");
        CHECK(a.kind == MarkdownLinkAction::OpenFolder && a.target == dir && a.select == pkg,
              "package only reveals, selected");
        a = plan("sub");
        CHECK(a.kind == MarkdownLinkAction::OpenFolder && a.target == dir + "/sub" && a.select.empty(),
              "directory opens, nothing selected");
        CHECK(plan("missing.md").kind == MarkdownLinkAction::None, "missing file unusable");
        CHECK(!MarkdownLinkUsable("missing.md", dir) && MarkdownLinkUsable("notes.md", dir), "usable");
        a = plan("https://x.org/a");
        CHECK(a.kind == MarkdownLinkAction::OpenUrl && a.url_mime == "application/x-vnd.Be.URL.https",
              a.url_mime);
        CHECK(plan("mailto:a@b.c").url_mime == "application/x-vnd.Be.URL.mailto", "mailto mime");
        CHECK(plan("javascript:alert(1)").kind == MarkdownLinkAction::None, "script url refused");
        // be:line: 1-based for Pe/Koder/others, 0-based for StyledEdit.
        CHECK(EditorLineFor("application/x-vnd.Haiku-StyledEdit", 12) == 11, "StyledEdit line");
        CHECK(EditorLineFor("application/x-vnd.haiku-stylededit", 1) == 0, "StyledEdit first line");
        CHECK(EditorLineFor("application/x-vnd.KapiX-Koder", 12) == 12, "Koder line");
        CHECK(EditorLineFor("application/x-vnd.beunited.pe", 12) == 12, "Pe line");
        CHECK(EditorLineFor("", 12) == 12, "unknown editor line");
        CHECK(plan("x-vnd.app:open").kind == MarkdownLinkAction::None, "app scheme refused");

        for (const char* name : {"notes.md", "run.sh", "thing.hpkg"})
            std::remove((dir + "/" + name).c_str());
        rmdir((dir + "/sub").c_str());
        rmdir(dir.c_str());
    }

    window->Quit();
    if (failures == 0)
        std::printf("test_chat_view: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
