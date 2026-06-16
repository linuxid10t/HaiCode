#include "PermissionWindow.h"
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

#include <string>
#include <future>

#include <haicode/tool.h>

static const uint32 MSG_ALLOW_ONCE   = 'pAlo';
static const uint32 MSG_ALLOW_ALWAYS = 'pAlA';
static const uint32 MSG_DENY         = 'pDny';

PermissionWindow::PermissionWindow(const std::string& action,
                                   const std::string& resource,
                                   const std::string& detail,
                                   BMessenger reply_target,
                                   void* promise_ptr)
    : BWindow(BRect(200, 200, 650, 450),
              "Permission Request",
              B_TITLED_WINDOW,
              B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
    , reply_target_(reply_target)
    , promise_ptr_(promise_ptr)
    , action_(action)
    , resource_(resource)
{
    // Build a description string
    std::string desc = "Action:    " + action + "\n"
                     + "Resource:  " + resource + "\n";
    if (!detail.empty()) {
        desc += "\n" + detail;
    }

    BStringView* header = new BStringView("header", "Permission Required");
    BFont bold_font(be_bold_font);
    bold_font.SetSize(14.0f);
    header->SetFont(&bold_font);

    BTextView* desc_view = new BTextView("desc");
    desc_view->SetText(desc.c_str());
    desc_view->MakeEditable(false);
    desc_view->MakeSelectable(true);
    desc_view->SetWordWrap(true);
    desc_view->SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    desc_view->SetLowColor(ui_color(B_PANEL_BACKGROUND_COLOR));
    desc_view->SetExplicitMinSize(BSize(380, 120));
    desc_view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 300));

    BScrollView* desc_scroll = new BScrollView("desc_scroll", desc_view,
                                               0, false, true, B_NO_BORDER);
    desc_scroll->SetExplicitMinSize(BSize(380, 120));
    desc_scroll->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 300));

    BButton* allow_once_btn   = new BButton("allow_once",   "Allow Once",   new BMessage(MSG_ALLOW_ONCE));
    BButton* allow_always_btn = new BButton("allow_always", "Allow Always", new BMessage(MSG_ALLOW_ALWAYS));
    BButton* deny_btn         = new BButton("deny",         "Deny",         new BMessage(MSG_DENY));

    allow_once_btn->MakeDefault(true);

    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(header)
        .Add(desc_scroll)
        .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
            .Add(allow_once_btn)
            .Add(allow_always_btn)
            .AddGlue()
            .Add(deny_btn)
        .End()
    .End();

    CenterOnScreen();
}

void
PermissionWindow::_SendReply(int32 effect)
{
    if (promise_fulfilled_)
        return;
    promise_fulfilled_ = true;

    BMessage reply(MSG_PERMISSION_REP);
    reply.AddPointer("promise_ptr", promise_ptr_);
    reply.AddInt32("effect",        effect);
    reply.AddString("action",       action_.c_str());
    reply.AddString("resource",     resource_.c_str());
    reply_target_.SendMessage(&reply);
}

void
PermissionWindow::MessageReceived(BMessage* msg)
{
    switch (msg->what) {
        case MSG_ALLOW_ONCE:
            _SendReply(0);  // Allow once
            Quit();
            break;
        case MSG_ALLOW_ALWAYS:
            _SendReply(1);  // Allow always
            Quit();
            break;
        case MSG_DENY:
            _SendReply(2);  // Deny
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
    // If the window is closed without a button press, treat as Deny
    _SendReply(2);
    return true;
}
