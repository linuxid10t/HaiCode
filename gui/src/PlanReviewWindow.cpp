#include "PlanReviewWindow.h"
#include "MarkdownView.h"
#include "Messages.h"

#include <Window.h>
#include <View.h>
#include <Button.h>
#include <TextView.h>
#include <ScrollView.h>
#include <StringView.h>
#include <LayoutBuilder.h>
#include <Message.h>
#include <Messenger.h>
#include <Font.h>

#include <string>

PlanReviewWindow::PlanReviewWindow(const std::string& plan_markdown,
                                   const std::string& plan_path,
                                   const std::string& session_id,
                                   BMessenger reply_target)
    : BWindow(BRect(150, 150, 950, 750),
              "Plan Review",
              B_TITLED_WINDOW,
              B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
    , reply_target_(reply_target)
    , session_id_(session_id)
{
    BStringView* header = new BStringView("header", "Proposed Plan");
    BFont bold_font(be_bold_font);
    bold_font.SetSize(16.0f);
    header->SetFont(&bold_font);

    BStringView* path_label = nullptr;
    if (!plan_path.empty()) {
        path_label = new BStringView("path_label", plan_path.c_str());
        BFont small_font(*be_plain_font);
        small_font.SetSize(10.0f);
        small_font.SetFace(B_ITALIC_FACE);
        path_label->SetFont(&small_font);
        path_label->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    }

    MarkdownTextView* plan_view = new MarkdownTextView("plan",
        MarkdownPaletteFor(ui_color(B_PANEL_TEXT_COLOR)));
    plan_view->SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    plan_view->SetLowColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    plan_view->SetExplicitMinSize(BSize(600, 400));
    plan_view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    // Relative links in the plan resolve against the project directory
    // (plans live in <project>/.haicode/plans/).
    std::string base_dir = plan_path;
    size_t plans = base_dir.rfind("/.haicode/plans/");
    if (plans != std::string::npos)
        base_dir.resize(plans);
    else if (size_t slash = base_dir.rfind('/'); slash != std::string::npos)
        base_dir.resize(slash);
    else
        base_dir.clear();
    plan_view->SetBaseDirectory(base_dir);
    plan_view->SetMarkdown(plan_markdown);

    BScrollView* plan_scroll = new BScrollView("plan_scroll", plan_view,
                                               0, true, true, B_FANCY_BORDER);
    plan_scroll->SetExplicitMinSize(BSize(600, 400));
    plan_scroll->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));

    BMessage* approve_msg = new BMessage(MSG_PLAN_DECISION);
    approve_msg->AddBool("approved", true);
    BButton* approve_btn = new BButton("approve", "Approve \xe2\x9c\x93", approve_msg);
    approve_btn->MakeDefault(true);

    BMessage* discard_msg = new BMessage(MSG_PLAN_DECISION);
    discard_msg->AddBool("approved", false);
    BButton* discard_btn = new BButton("discard", "Discard", discard_msg);

    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(header)
        .Add(path_label)
        .Add(plan_scroll)
        .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
            .Add(approve_btn)
            .Add(discard_btn)
            .AddGlue()
        .End()
    .End();

    CenterOnScreen();
}

void
PlanReviewWindow::_SendDecision(bool approved)
{
    if (decided_) return;
    decided_ = true;

    BMessage reply(MSG_PLAN_DECISION);
    reply.AddBool("approved", approved);
    reply.AddString("session_id", session_id_.c_str());
    reply_target_.SendMessage(&reply);
}

void
PlanReviewWindow::MessageReceived(BMessage* msg)
{
    switch (msg->what) {
        case MSG_PLAN_DECISION: {
            bool approved = false;
            msg->FindBool("approved", &approved);
            _SendDecision(approved);
            Quit();
            break;
        }
        default:
            BWindow::MessageReceived(msg);
            break;
    }
}

bool
PlanReviewWindow::QuitRequested()
{
    // Closing without a decision = Discard
    _SendDecision(false);
    return true;
}
