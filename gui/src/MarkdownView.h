#pragma once
// BeAPI side of the markdown renderer (haicode/markdown.h): maps the
// semantic style flags to fonts and colors, and a read-only BTextView that
// displays a markdown document (used by the plan review window; ChatView
// renders its entries through the same mapping).

#include <Font.h>
#include <GraphicsDefs.h>
#include <TextView.h>

#include <haicode/markdown.h>

#include <memory>
#include <string>
#include <vector>

struct MarkdownPalette {
    rgb_color base;        // ordinary text
    rgb_color code;        // inline code spans
    rgb_color code_block;  // fenced code lines
    rgb_color link;
    rgb_color dim;         // table borders, rules, URLs, fence labels
    rgb_color quote;
};

// Fixed accent colors around the entry's own text color.
MarkdownPalette MarkdownPaletteFor(rgb_color base);

// flags 0 is exactly be_plain_font and kBold exactly be_bold_font, so text
// without markdown renders as it always has.
BFont MarkdownFont(uint16 flags, uint8 heading);
rgb_color MarkdownColor(uint16 flags, const MarkdownPalette& palette);

// be_fixed_font columns that fit the view's text rect (tables and rules are
// laid out to this width, so they never word-wrap).
int MarkdownColumns(const BTextView* view);

// True when be_fixed_font itself lacks the box-drawing glyphs: fallback
// glyphs would not share the grid's advance width, so tables use +-| then.
bool MarkdownAsciiBorders();

struct TextRunArrayDeleter {
    void operator()(text_run_array* a) const { BTextView::FreeRunArray(a); }
};
using TextRunArrayPtr = std::unique_ptr<text_run_array, TextRunArrayDeleter>;

TextRunArrayPtr MarkdownRunArray(const haicode::md::Styled& styled,
                                 const MarkdownPalette& palette);

// True when `where` is over an actual glyph of `view` (not the empty space
// right of a line's end, which OffsetAt still maps to that line's last
// character).
bool MarkdownPointOverText(const BTextView* view, BPoint where);

// Links. Relative paths resolve against `base_dir` (the session's project
// directory). Usable = resolve_link() gives a web/mail URL, or a file or
// directory that currently exists — only usable links get the hand cursor
// and open on click.
bool MarkdownLinkUsable(const std::string& target, const std::string& base_dir);

// What a click on a link would do — the side-effect-free half of
// OpenMarkdownLink (tests check it without launching anything).
struct MarkdownLinkAction {
    enum Kind { None, OpenUrl, OpenFile, OpenFolder } kind = None;
    std::string target;     // URL, or the path to open
    std::string url_mime;   // OpenUrl: application/x-vnd.Be.URL.<scheme>
    int         line = 0;   // OpenFile: 1-based line, 0 = none
};
MarkdownLinkAction PlanMarkdownLink(const std::string& target,
                                    const std::string& base_dir);

// The "be:line" value to send an editor for 1-based `line`. The convention
// (TextSearch, Pe, Koder) is 1-based, but StyledEdit passes it straight to
// BTextView::GoToLine(), which counts from 0 — so it gets line - 1.
int32 EditorLineFor(const std::string& app_signature, int line);

// Opens a usable link without blocking the caller (the launch runs on a
// detached thread): web URLs and mailto in their registered handler,
// directories in Tracker, files in their preferred application (at the
// linked line when there is one). An executable, or a package, is never
// launched from a click — its folder opens instead. False when the link is
// not usable.
bool OpenMarkdownLink(const std::string& target, const std::string& base_dir);

// Read-only text view showing rendered markdown; re-renders when a width
// change alters the column count of a document with tables or rules.
class MarkdownTextView : public BTextView {
public:
    MarkdownTextView(const char* name, const MarkdownPalette& palette);

    void SetMarkdown(const std::string& markdown);
    // Base for relative link paths.
    void SetBaseDirectory(const std::string& dir) { base_dir_ = dir; }

    void FrameResized(float width, float height) override;
    void MouseDown(BPoint where) override;
    void MouseUp(BPoint where) override;
    void MouseMoved(BPoint where, uint32 transit, const BMessage* dragMessage) override;

private:
    void _Render();
    int  _LinkAt(BPoint where) const;

    std::string     markdown_;
    MarkdownPalette palette_;
    int             cols_ = 0;
    std::string     base_dir_;
    std::vector<haicode::md::LinkSpan> links_;
    int             pressed_link_ = -1;   // link under the last primary press
};
