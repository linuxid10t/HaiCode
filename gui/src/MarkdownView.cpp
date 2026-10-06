#include "MarkdownView.h"

#include <AppFileInfo.h>
#include <Cursor.h>
#include <Entry.h>
#include <File.h>
#include <Message.h>
#include <Messenger.h>
#include <Node.h>
#include <Roster.h>
#include <ScrollBar.h>
#include <Window.h>

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <thread>

using namespace haicode::md;

MarkdownPalette
MarkdownPaletteFor(rgb_color base)
{
    MarkdownPalette p;
    p.base       = base;
    p.code       = { 160,  50, 100, 255 };
    p.code_block = {  70,  70,  95, 255 };
    p.link       = {  30,  90, 200, 255 };
    p.dim        = { 150, 150, 150, 255 };
    // Darker than `dim`: the grid must read clearly yet stay behind the text.
    p.rule       = {  90,  90,  90, 255 };
    p.quote      = { 105, 105, 105, 255 };
    return p;
}

BFont
MarkdownFont(uint16 flags, uint8 heading)
{
    if (heading == 0 && flags == 0) return *be_plain_font;
    if (heading == 0 && flags == kBold) return *be_bold_font;

    bool mono = (flags & (kCode | kCodeBlock | kMono)) != 0;
    BFont font(mono ? *be_fixed_font : *be_plain_font);
    uint16 face = 0;
    if ((flags & kBold) || heading > 0) face |= B_BOLD_FACE;
    if (flags & (kItalic | kQuote)) face |= B_ITALIC_FACE;
    if (flags & kLink) face |= B_UNDERSCORE_FACE;
    if (flags & kStrike) face |= B_STRIKEOUT_FACE;
    if (face == 0) face = B_REGULAR_FACE;
    font.SetFace(face);
    if (heading > 0 && heading <= 3) {
        static const float kScale[] = { 1.5f, 1.3f, 1.15f };
        font.SetSize(font.Size() * kScale[heading - 1]);
    }
    return font;
}

rgb_color
MarkdownColor(uint16 flags, const MarkdownPalette& palette)
{
    if (flags & kRule) return palette.rule;
    if (flags & kDim) return palette.dim;
    if (flags & kCode) return palette.code;
    if (flags & kCodeBlock) return palette.code_block;
    if (flags & kLink) return palette.link;
    if (flags & kQuote) return palette.quote;
    return palette.base;
}

int
MarkdownColumns(const BTextView* view)
{
    BFont fixed(*be_fixed_font);
    float cell = fixed.StringWidth("0");
    if (cell <= 0) return 80;
    // One column of slack for rounding, so a full-width grid line never
    // wraps onto a second line.
    // The bounds cap a text rect that has not caught up with a shrink yet.
    float width = view->TextRect().Width();
    float bounds = view->Bounds().Width() - 8;
    if (bounds > 0) width = std::min(width, bounds);
    int cols = (int)(width / cell) - 1;
    return std::max(cols, 12);
}

bool
MarkdownAsciiBorders()
{
    // GetHasGlyphs reads UTF-8 and counts characters.
    static const char kBox[] = "\xe2\x94\x80\xe2\x94\x82\xe2\x94\x8c\xe2\x94\xbc";  // ─│┌┼
    bool has[4] = { false, false, false, false };
    BFont fixed(*be_fixed_font);
    fixed.GetHasGlyphs(kBox, 4, has, false);
    return !(has[0] && has[1] && has[2] && has[3]);
}

TextRunArrayPtr
MarkdownRunArray(const Styled& styled, const MarkdownPalette& palette)
{
    if (styled.runs.empty()) return nullptr;
    TextRunArrayPtr arr(BTextView::AllocRunArray((int32)styled.runs.size()));
    if (!arr) return nullptr;
    for (size_t i = 0; i < styled.runs.size(); ++i) {
        const Run& run = styled.runs[i];
        arr->runs[i].offset = (int32)run.offset;
        arr->runs[i].font   = MarkdownFont(run.flags, run.heading);
        arr->runs[i].color  = MarkdownColor(run.flags, palette);
    }
    return arr;
}

bool
MarkdownPointOverText(const BTextView* view, BPoint where)
{
    if (view->TextLength() == 0 || !view->Bounds().Contains(where)) return false;
    int32 line = view->LineAt(where);
    if (line < 0 || line >= view->CountLines()) return false;
    float height;
    BPoint origin = view->PointAt(view->OffsetAt(line), &height);
    float width = view->LineWidth(line);
    return width > 0 && where.x >= origin.x && where.x < origin.x + width
        && where.y >= origin.y && where.y < origin.y + height;
}

// ---------------------------------------------------------------------------
// Links
// ---------------------------------------------------------------------------

namespace {

std::string
HomeDirectory()
{
    const char* home = getenv("HOME");
    return home ? home : "";
}

// Files a click must never run: anything executable (ELF, scripts with the
// x bit) and packages (opening one starts an install flow).
bool
RevealInstead(const std::string& path, const struct stat& st)
{
    if (S_ISREG(st.st_mode) && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
        return true;
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return lower.size() > 5 && lower.compare(lower.size() - 5, 5, ".hpkg") == 0;
}

std::string
ParentDirectory(const std::string& path)
{
    size_t slash = path.rfind('/');
    if (slash == std::string::npos) return ".";
    return slash == 0 ? "/" : path.substr(0, slash);
}

// Open `folder` in Tracker with `select` (a path inside it) selected — the
// same request WebPositive's "Open containing folder" sends. Falls back to
// opening the folder alone when there is nothing to select or Tracker isn't
// reachable.
void
RevealInTracker(const std::string& folder, const std::string& select)
{
    entry_ref dir;
    if (get_ref_for_path(folder.c_str(), &dir) != B_OK) return;
    if (!select.empty()) {
        BEntry entry(select.c_str());
        node_ref node;
        BMessenger tracker("application/x-vnd.Be-TRAK");
        if (entry.GetNodeRef(&node) == B_OK && tracker.IsValid()) {
            BMessage message(B_REFS_RECEIVED);
            message.AddRef("refs", &dir);
            message.AddData("nodeRefToSelect", B_RAW_TYPE, &node, sizeof(node_ref));
            if (tracker.SendMessage(&message) == B_OK) return;
        }
    }
    be_roster->Launch(&dir);
}

void
LaunchAction(MarkdownLinkAction action)
{
    if (action.kind == MarkdownLinkAction::OpenFolder) {
        RevealInTracker(action.target, action.select);
        return;
    }
    entry_ref ref;
    if (get_ref_for_path(action.target.c_str(), &ref) != B_OK) return;
    if (action.line > 0) {
        // Preferred app with a "be:line" refs message (StyledEdit, Pe and
        // Koder honor it); fall through to a plain launch otherwise.
        entry_ref app;
        if (be_roster->FindApp(&ref, &app) == B_OK) {
            char signature[B_MIME_TYPE_LENGTH] = "";
            BFile app_file(&app, B_READ_ONLY);
            BAppFileInfo info(&app_file);
            if (info.InitCheck() != B_OK || info.GetSignature(signature) != B_OK)
                signature[0] = '\0';
            BMessage refs(B_REFS_RECEIVED);
            refs.AddRef("refs", &ref);
            refs.AddInt32("be:line", EditorLineFor(signature, action.line));
            refs.AddInt32("line", action.line);   // Pe's own key, 1-based
            status_t err = be_roster->Launch(&app, &refs);
            if (err == B_OK || err == B_ALREADY_RUNNING) return;
        }
    }
    status_t err = be_roster->Launch(&ref);
    if (err == B_OK || err == B_ALREADY_RUNNING) return;
    // No preferred application: show the file in its folder.
    RevealInTracker(ParentDirectory(action.target), action.target);
}

}  // namespace

int32
EditorLineFor(const std::string& app_signature, int line)
{
    std::string sig = app_signature;
    std::transform(sig.begin(), sig.end(), sig.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (sig == "application/x-vnd.haiku-stylededit")
        return std::max(0, line - 1);
    return line;
}

MarkdownLinkAction
PlanMarkdownLink(const std::string& target, const std::string& base_dir)
{
    MarkdownLinkAction action;
    LinkTarget link = resolve_link(target, base_dir, HomeDirectory());
    if (link.kind == LinkKind::Web || link.kind == LinkKind::Mail) {
        // URL handlers register as application/x-vnd.Be.URL.<scheme>.
        std::string scheme = link.target.substr(0, link.target.find(':'));
        std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        action.kind = MarkdownLinkAction::OpenUrl;
        action.target = link.target;
        action.url_mime = "application/x-vnd.Be.URL." + scheme;
        return action;
    }
    if (link.kind != LinkKind::File) return action;

    struct stat st;
    if (stat(link.target.c_str(), &st) != 0) return action;
    if (S_ISDIR(st.st_mode)) {
        action.kind = MarkdownLinkAction::OpenFolder;
        action.target = link.target;
    } else if (RevealInstead(link.target, st)) {
        action.kind = MarkdownLinkAction::OpenFolder;
        action.target = ParentDirectory(link.target);
        action.select = link.target;
    } else {
        action.kind = MarkdownLinkAction::OpenFile;
        action.target = link.target;
        action.line = link.line;
    }
    return action;
}

bool
MarkdownLinkUsable(const std::string& target, const std::string& base_dir)
{
    return PlanMarkdownLink(target, base_dir).kind != MarkdownLinkAction::None;
}

bool
OpenMarkdownLink(const std::string& target, const std::string& base_dir)
{
    MarkdownLinkAction action = PlanMarkdownLink(target, base_dir);
    switch (action.kind) {
    case MarkdownLinkAction::None:
        return false;
    case MarkdownLinkAction::OpenUrl:
        std::thread([mime = action.url_mime, url = action.target] {
            const char* argv[] = { url.c_str(), nullptr };
            be_roster->Launch(mime.c_str(), 1, argv);
        }).detach();
        return true;
    case MarkdownLinkAction::OpenFile:
    case MarkdownLinkAction::OpenFolder:
        std::thread(LaunchAction, action).detach();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// MarkdownTextView
// ---------------------------------------------------------------------------

MarkdownTextView::MarkdownTextView(const char* name, const MarkdownPalette& palette)
    : BTextView(name, B_WILL_DRAW | B_PULSE_NEEDED | B_FRAME_EVENTS)
    , palette_(palette)
{
    MakeEditable(false);
    MakeSelectable(true);
    SetWordWrap(true);
    SetStylable(true);
}

void
MarkdownTextView::SetMarkdown(const std::string& markdown)
{
    markdown_ = markdown;
    _Render();
}

void
MarkdownTextView::FrameResized(float width, float height)
{
    BTextView::FrameResized(width, height);
    if (MarkdownColumns(this) != cols_ && width_dependent(markdown_))
        _Render();
}

void
MarkdownTextView::_Render()
{
    Options opts;
    opts.max_cols = cols_ = MarkdownColumns(this);
    opts.ascii_borders = MarkdownAsciiBorders();
    Styled styled = render(markdown_, opts);
    TextRunArrayPtr runs = MarkdownRunArray(styled, palette_);
    links_ = styled.links;
    pressed_link_ = -1;

    BScrollBar* vsb = ScrollBar(B_VERTICAL);
    float value = vsb ? vsb->Value() : 0;
    SetText(styled.text.data(), (int32)styled.text.size(), runs.get());
    if (vsb) vsb->SetValue(value);
}

int
MarkdownTextView::_LinkAt(BPoint where) const
{
    if (!MarkdownPointOverText(this, where)) return -1;
    size_t offset = (size_t)OffsetAt(where);
    for (size_t i = 0; i < links_.size(); ++i) {
        if (offset >= links_[i].start && offset < links_[i].end)
            return (int)i;
    }
    return -1;
}

void
MarkdownTextView::MouseDown(BPoint where)
{
    int32 buttons = 0;
    if (Window() && Window()->CurrentMessage())
        Window()->CurrentMessage()->FindInt32("buttons", &buttons);
    pressed_link_ = (buttons & B_PRIMARY_MOUSE_BUTTON) ? _LinkAt(where) : -1;
    BTextView::MouseDown(where);
}

void
MarkdownTextView::MouseUp(BPoint where)
{
    BTextView::MouseUp(where);
    int pressed = pressed_link_;
    pressed_link_ = -1;
    // A click, not a drag-select: no selection and still on the same link.
    int32 start, end;
    GetSelection(&start, &end);
    if (pressed >= 0 && start == end && _LinkAt(where) == pressed)
        OpenMarkdownLink(links_[pressed].target, base_dir_);
}

void
MarkdownTextView::MouseMoved(BPoint where, uint32 transit, const BMessage* dragMessage)
{
    BTextView::MouseMoved(where, transit, dragMessage);
    if (transit == B_EXITED_VIEW) return;
    int link = _LinkAt(where);
    static const BCursor hand(B_CURSOR_ID_FOLLOW_LINK);
    static const BCursor text_cursor(B_CURSOR_ID_I_BEAM);
    bool usable = link >= 0 && MarkdownLinkUsable(links_[link].target, base_dir_);
    SetViewCursor(usable ? &hand : &text_cursor);
}
