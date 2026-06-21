#include "AskUserWindow.h"
#include "Messages.h"

#include <Application.h>
#include <Button.h>
#include <ControlLook.h>
#include <GroupLayout.h>
#include <LayoutBuilder.h>
#include <OS.h>
#include <RadioButton.h>
#include <ScrollView.h>
#include <TextControl.h>
#include <TextView.h>
#include <View.h>
#include <Font.h>

#include <cstdio>

static const uint32 kMsgRadioSelected = 'rdsL';
static const uint32 kMsgSubmit = 'sbtm';
static const uint32 kMsgCancel = 'cncl';

AskUserWindow::AskUserWindow(const BString& question,
                             const std::vector<BString>& options,
                             const BString& session_id,
                             const BString& call_id,
                             BMessenger reply_target)
    : BWindow(BRect(150, 150, 700, 500),
              "Question",
              B_TITLED_WINDOW,
              B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
    , fQuestion(question)
    , fSessionID(session_id)
    , fCallID(call_id)
    , fReplyTarget(reply_target)
    , fOtherRadio(nullptr)
    , fOtherText(nullptr)
    , fOptionCount(options.size())
    , fAnswered(false)
{
    // Question text as a read-only word-wrapped BTextView (BStringView has no
    // word wrap, so long questions would overflow). Matches PlanReviewWindow's
    // styling: panel background, selectable but not editable.
    BTextView* question_view = new BTextView("question");
    question_view->SetText(question);
    question_view->MakeEditable(false);
    question_view->MakeSelectable(true);
    question_view->SetWordWrap(true);
    question_view->SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    question_view->SetLowColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    question_view->SetExplicitMinSize(BSize(400, 40));
    question_view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 200));

    // Create radio buttons for each option
    BView* radioContainer = new BView("radioContainer", 0);
    BGroupLayout* radioLayout = new BGroupLayout(B_VERTICAL, B_USE_SMALL_SPACING);
    radioContainer->SetLayout(radioLayout);

    for (size_t i = 0; i < options.size(); i++) {
        BMessage* msg = new BMessage(kMsgRadioSelected);
        msg->AddInt32("option_index", i);
        BRadioButton* radio = new BRadioButton(options[i], msg);
        radioContainer->AddChild(radio);
        fOptionRadios.push_back(radio);
        // First option selected by default
        if (i == 0)
            radio->SetValue(B_CONTROL_ON);
    }

    // "Other:" radio button + text control on same line
    BMessage* otherMsg = new BMessage(kMsgRadioSelected);
    otherMsg->AddInt32("option_index", options.size());
    fOtherRadio = new BRadioButton("Other:", otherMsg);

    fOtherText = new BTextControl("other_text", "", "", nullptr);
    fOtherText->SetExplicitMinSize(BSize(200, B_SIZE_UNSET));

    BGroupLayout* otherLayout = new BGroupLayout(B_HORIZONTAL, B_USE_SMALL_SPACING);
    otherLayout->AddView(fOtherRadio);
    otherLayout->AddView(fOtherText, 1.0f);

    BButton* submitBtn = new BButton("submit", "Submit", new BMessage(kMsgSubmit));
    submitBtn->MakeDefault(true);
    BButton* cancelBtn = new BButton("cancel", "Cancel", new BMessage(kMsgCancel));

    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(question_view)
        .Add(radioContainer)
        .Add(otherLayout)
        .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
            .Add(submitBtn)
            .Add(cancelBtn)
            .AddGlue()
        .End()
    .End();

    CenterOnScreen();

    // Disable "Other" text field initially
    fOtherText->SetEnabled(false);
}

AskUserWindow::~AskUserWindow()
{
}

void
AskUserWindow::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kMsgRadioSelected: {
            int32 index = -1;
            message->FindInt32("option_index", &index);

            if (index >= 0 && index < fOptionCount) {
                // A preset option was selected - disable Other text field
                fOtherText->SetEnabled(false);
                fOtherText->SetText("");
            } else if (index == fOptionCount) {
                // "Other" was selected - enable text field and focus it
                fOtherText->SetEnabled(true);
                fOtherText->MakeFocus(true);
            }
            break;
        }

        case kMsgSubmit: {
            if (fAnswered)
                break;
            fAnswered = true;

            BString answer = "";
            bool cancelled = false;

            // Check which radio button is selected
            int32 selected = -1;
            for (size_t i = 0; i < fOptionRadios.size(); i++) {
                if (fOptionRadios[i]->Value() == B_CONTROL_ON) {
                    answer = fOptionRadios[i]->Label();
                    selected = i;
                    break;
                }
            }

            // Check if "Other" is selected
            if (selected == -1 && fOtherRadio->Value() == B_CONTROL_ON) {
                answer = fOtherText->Text();
                if (answer.IsEmpty()) {
                    // Treat empty "Other" as cancellation
                    cancelled = true;
                }
            } else if (selected == -1) {
                // Nothing selected - treat as cancellation
                cancelled = true;
            }

            BMessage reply(MSG_ASK_USER_REPLY);
            reply.AddString("session_id", fSessionID);
            reply.AddString("call_id", fCallID);
            if (!cancelled) {
                reply.AddString("answer", answer);
                reply.AddBool("custom", selected == (int32)fOptionCount);
            } else {
                reply.AddBool("cancelled", true);
            }
            fReplyTarget.SendMessage(&reply);

            Quit();
            break;
        }

        case kMsgCancel: {
            if (!fAnswered) {
                fAnswered = true;
                BMessage reply(MSG_ASK_USER_REPLY);
                reply.AddString("session_id", fSessionID);
                reply.AddString("call_id", fCallID);
                reply.AddBool("cancelled", true);
                fReplyTarget.SendMessage(&reply);
            }
            Quit();
            break;
        }

        default:
            BWindow::MessageReceived(message);
            break;
    }
}

bool
AskUserWindow::QuitRequested()
{
    if (!fAnswered) {
        fAnswered = true;
        BMessage reply(MSG_ASK_USER_REPLY);
        reply.AddString("session_id", fSessionID);
        reply.AddString("call_id", fCallID);
        reply.AddBool("cancelled", true);
        fReplyTarget.SendMessage(&reply);
    }
    return true;
}
