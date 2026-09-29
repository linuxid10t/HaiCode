#include "PermissionWindow.h"
#include "Messages.h"

#include <Application.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <Message.h>
#include <Messenger.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextView.h>
#include <View.h>

#include <string>

namespace {

constexpr int32 kDecisionDeny = 0;
constexpr int32 kDecisionAllowOnce = 1;
constexpr int32 kDecisionAllowSession = 2;

// Window-internal button codes (multi-char literals, little-endian order).
const uint32 kBtnDeny = 'pDny';
const uint32 kBtnAllowOnce = 'pAlo';
const uint32 kBtnAllowSession = 'pAsn';

// Plain-language operation summary derived from tool identity and input —
// never model-provided prose.
std::string summarize(const haicode::PermissionRequest& req) {
    const std::string& t = req.tool_name;
    if (t == "bash")  return "Run a shell command";
    if (t == "write") return "Write a file";
    if (t == "edit")  return "Edit a file";
    if (t == "git")   return "Run a Git command";
    if (t == "process") return "Manage a running process";
    if (t == "web_search")  return "Search the web";
    if (t == "web_extract") return "Fetch a web page";
    if (t == "screenshot")  return "Capture a screenshot";
    if (t == "external_terminal") return "Open a Terminal window";
    if (t == "read")  return "Read a file";
    return "Use the '" + t + "' tool";
}

bool within(const std::string& path, const std::string& base) {
    if (base.empty() || path.empty()) return false;
    if (path == base) return true;
    return path.size() > base.size() && path[base.size()] == '/'
        && path.compare(0, base.size(), base) == 0;
}

} // namespace

PermissionWindow::PermissionWindow(const haicode::PermissionRequest& req,
                                   const std::string& session_label,
                                   const std::string& build_command,
                                   BMessenger notify_target)
    : BWindow(BRect(150, 150, 640, 560), "Permission Request",
              B_TITLED_WINDOW,
              B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
    , notify_target_(notify_target)
    , request_id_(req.id)
    , session_id_(req.session_id)
{
    BStringView* header = new BStringView("header", "Permission required");
    BFont bold(be_bold_font);
    bold.SetSize(14.0f);
    header->SetFont(&bold);

    std::string who;
    if (!session_label.empty())
        who += "Session:     " + session_label + "\n";
    if (!req.working_dir.empty())
        who += "Directory:   " + req.working_dir + "\n";
    who += "Tool:        " + req.tool_name + "\n";
    who += "Category:    " + req.action + "\n";
    BStringView* who_view = new BStringView("who", who.c_str());
    who_view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

    BStringView* summary = new BStringView("summary", summarize(req).c_str());
    BFont summary_font(be_bold_font);
    summary->SetFont(&summary_font);
    summary->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

    // Full target, selectable and scrollable.
    BTextView* target = new BTextView("target");
    target->SetText(req.resource.c_str());
    target->MakeEditable(false);
    target->MakeSelectable(true);
    target->SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    target->SetLowColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    target->SetExplicitMinSize(BSize(420, 44));
    target->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 96));
    BScrollView* target_scroll = new BScrollView("target_scroll", target,
                                                 0, false, true, B_FANCY_BORDER);

    // Tool-specific details from the structured input.
    std::string details;
    const auto& in = req.input;
    auto field = [&](const char* key, const char* label) {
        if (in.contains(key) && in[key].is_string())
            details += std::string(label)
                     + in[key].get<std::string>() + "\n";
    };
    if (req.tool_name == "bash") {
        field("command", "Command:\n");
        field("cwd", "Working directory: ");
    } else if (req.tool_name == "write") {
        if (in.contains("content") && in["content"].is_string()) {
            std::string c = in["content"].get<std::string>();
            const size_t cap = 600;
            if (c.size() > cap) {
                size_t end = c.rfind('\n', cap);
                if (end == std::string::npos) end = cap;
                c = c.substr(0, end) + "\n... ("
                  + std::to_string(in["content"].get<std::string>().size() - end)
                  + " more bytes truncated)";
            }
            details += "Content preview:\n" + c + "\n";
        }
    } else if (req.tool_name == "edit") {
        field("old_string", "Replace:\n");
        field("new_string", "With:\n");
    } else if (req.tool_name == "git") {
        field("subcommand", "Subcommand: ");
        if (in.contains("args") && in["args"].is_array()) {
            details += "Arguments:";
            for (const auto& a : in["args"])
                if (a.is_string()) details += " " + a.get<std::string>();
            details += "\n";
        }
    } else if (req.tool_name == "process") {
        field("action", "Operation: ");
        if (in.contains("pid") && in["pid"].is_number_integer())
            details += "PID: " + std::to_string(in["pid"].get<int>()) + "\n";
        if (in.contains("port") && in["port"].is_number_integer())
            details += "Port: " + std::to_string(in["port"].get<int>()) + "\n";
    }

    BScrollView* detail_scroll = nullptr;
    if (!details.empty()) {
        auto* detail_view = new BTextView("details");
        detail_view->SetText(details.c_str());
        detail_view->MakeEditable(false);
        detail_view->MakeSelectable(true);
        detail_view->SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
        detail_view->SetLowColor(ui_color(B_PANEL_BACKGROUND_COLOR));
        detail_view->SetExplicitMinSize(BSize(420, 60));
        detail_view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 180));
        detail_scroll = new BScrollView("detail_scroll", detail_view,
                                        0, false, true, B_FANCY_BORDER);
    }

    // Warnings: outside-project target, post-write build hook.
    std::string warnings;
    if (!req.working_dir.empty() && !req.resource.empty()
            && req.resource[0] == '/'
            && req.resource.compare(0, 4, "http") != 0
            && !within(req.resource, req.working_dir))
        warnings += "! Target is outside this session's project directory.\n";
    if (!build_command.empty()
            && (req.tool_name == "write" || req.tool_name == "edit"))
        warnings += "! After this write, the configured build command will "
                    "run automatically:\n    " + build_command + "\n";
    BStringView* warn_view = nullptr;
    if (!warnings.empty()) {
        warn_view = new BStringView("warnings", warnings.c_str());
        warn_view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    }

    // Selectable original input, clearly truncated.
    std::string input_dump;
    try { input_dump = req.input.dump(2); } catch (...) {}
    const size_t cap = 1500;
    if (input_dump.size() > cap)
        input_dump = input_dump.substr(0, cap)
                   + "\n... (input truncated at 1500 bytes)";
    auto* input_view = new BTextView("input");
    input_view->SetText(input_dump.c_str());
    input_view->MakeEditable(false);
    input_view->MakeSelectable(true);
    input_view->SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    input_view->SetLowColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    input_view->SetExplicitMinSize(BSize(420, 60));
    input_view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 160));
    BScrollView* input_scroll = new BScrollView("input_scroll", input_view,
                                                0, false, true, B_FANCY_BORDER);

    auto* deny = new BButton("deny", "Deny", new BMessage(kBtnDeny));
    auto* allow_once = new BButton("allow_once", "Allow Once",
                                   new BMessage(kBtnAllowOnce));
    auto* allow_session = new BButton("allow_session",
        "Allow This Target for This Session",
        new BMessage(kBtnAllowSession));

    // An exact session grant on the working directory itself would cover
    // every operation of this category in the project — too coarse to
    // explain as "this target".
    if (!req.working_dir.empty() && req.resource == req.working_dir) {
        allow_session->SetEnabled(false);
        allow_session->SetToolTip("This tool's target is the whole working "
                                  "directory; a session grant would be broader "
                                  "than it looks");
    }

    // Deny is the default: Enter denies, Escape denies, close denies. No
    // Allow action is ever the default button.
    deny->MakeDefault(true);

    auto layout = BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(header)
        .Add(who_view)
        .Add(summary)
        .Add(target_scroll);
    if (detail_scroll) layout.Add(detail_scroll);
    if (warn_view)     layout.Add(warn_view);
    layout.Add(input_scroll)
        .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
            .Add(deny)
            .Add(allow_once)
            .Add(allow_session)
        .End()
    .End();

    CenterOnScreen();
}

void
PermissionWindow::_SendDecision(int32 decision)
{
    if (replied_) return;
    replied_ = true;

    BMessage reply(MSG_PERMISSION_DECISION);
    reply.AddString("request_id", request_id_.c_str());
    reply.AddInt32("decision", decision);
    be_app->PostMessage(&reply);

    // Let MainWindow surface the next queued request for this session.
    if (notify_target_.IsValid()) {
        BMessage closed(MSG_PERMISSION_WINDOW_CLOSED);
        closed.AddString("session_id", session_id_.c_str());
        notify_target_.SendMessage(&closed);
    }
}

void
PermissionWindow::MessageReceived(BMessage* msg)
{
    switch (msg->what) {
        case kBtnDeny:
            _SendDecision(kDecisionDeny);
            Quit();
            break;
        case kBtnAllowOnce:
            _SendDecision(kDecisionAllowOnce);
            Quit();
            break;
        case kBtnAllowSession:
            _SendDecision(kDecisionAllowSession);
            Quit();
            break;
        default:
            BWindow::MessageReceived(msg);
            break;
    }
}

bool
PermissionWindow::QuitRequested()
{
    // Close or Escape denies.
    _SendDecision(kDecisionDeny);
    return true;
}
