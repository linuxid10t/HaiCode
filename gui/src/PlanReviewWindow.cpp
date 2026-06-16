#include "PlanReviewWindow.h"
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

    BTextView* plan_view = new BTextView("plan");
    plan_view->SetText(plan_markdown.c_str());
    plan_view->MakeEditable(false);
    plan_view->MakeSelectable(true);
    plan_view->SetWordWrap(true);
    plan_view->SetStylable(true);
    plan_view->SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    plan_view->SetLowColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    plan_view->SetExplicitMinSize(BSize(600, 400));
    plan_view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));

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
