#include "PermissionRuleEditWindow.h"
#include "Messages.h"

#include <Button.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Message.h>
#include <PopUpMenu.h>
#include <StringView.h>
#include <TextControl.h>

namespace {
const uint32 kBtnSave = 'rSve';
}

PermissionRuleEditWindow::PermissionRuleEditWindow(BMessenger target,
                                                   const haicode::PermissionRule& rule,
                                                   int index)
    : BWindow(BRect(200, 200, 560, 360), "Permission Rule",
              B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
    , target_(target)
    , index_(index)
{
    auto* action_field = new BTextControl("action", "Action pattern:",
                                          rule.action.c_str(), nullptr);
    auto* resource_field = new BTextControl("resource", "Resource pattern:",
                                            rule.resource.c_str(), nullptr);

    auto* effect_menu = new BPopUpMenu("effect");
    effect_menu->SetRadioMode(true);
    effect_menu->SetLabelFromMarked(true);
    const struct { const char* label; const char* value; } kEffects[] = {
        {"Allow", "allow"}, {"Deny", "deny"}, {"Ask", "ask"},
    };
    std::string current = "allow";
    if (rule.effect == haicode::PermissionEffect::Deny) current = "deny";
    if (rule.effect == haicode::PermissionEffect::Ask)  current = "ask";
    for (auto& e : kEffects) {
        auto* item = new BMenuItem(e.label, new BMessage('rfx'));
        item->Message()->AddString("effect", e.value);
        effect_menu->AddItem(item);
        if (current == e.value) item->SetMarked(true);
    }
    auto* effect_field = new BMenuField("effect_field", "Effect:", effect_menu);

    auto* syntax = new BStringView("syntax",
        "Patterns use shell globbing (* and ?). Within one source, the LAST\n"
        "matching rule decides; Ask falls through to the next source.");
    BFont small(*be_plain_font);
    small.SetSize(10.0f);
    syntax->SetFont(&small);

    auto* save = new BButton("save", "OK", new BMessage(kBtnSave));
    save->MakeDefault(true);
    auto* cancel = new BButton("cancel", "Cancel", new BMessage(B_QUIT_REQUESTED));

    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .Add(action_field)
        .Add(resource_field)
        .Add(effect_field)
        .Add(syntax)
        .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
            .AddGlue()
            .Add(cancel)
            .Add(save)
        .End()
    .End();

    action_field->MakeFocus(true);
    CenterOnScreen();
}

void
PermissionRuleEditWindow::_Done(bool save)
{
    if (save && target_.IsValid()) {
        // Read fields via FindView (they are gone after Quit).
        BTextControl* action = nullptr;
        BTextControl* resource = nullptr;
        BMenuField* effect = nullptr;
        if (auto* v = FindView("action")) action = dynamic_cast<BTextControl*>(v);
        if (auto* v = FindView("resource")) resource = dynamic_cast<BTextControl*>(v);
        if (auto* v = FindView("effect_field")) effect = dynamic_cast<BMenuField*>(v);

        std::string action_s = action ? action->Text() : "";
        std::string resource_s = resource ? resource->Text() : "";
        std::string effect_s = "ask";
        if (effect && effect->Menu()) {
            if (auto* marked = effect->Menu()->FindMarked()) {
                const char* ev = nullptr;
                if (marked->Message() && marked->Message()->FindString("effect", &ev) == B_OK && ev)
                    effect_s = ev;
            }
        }

        BMessage done(MSG_PERM_RULE_DONE);
        done.AddString("action", action_s.c_str());
        done.AddString("resource", resource_s.c_str());
        done.AddString("effect", effect_s.c_str());
        done.AddInt32("index", index_);
        target_.SendMessage(&done);
    }
    PostMessage(B_QUIT_REQUESTED);
}

void
PermissionRuleEditWindow::MessageReceived(BMessage* msg)
{
    switch (msg->what) {
        case kBtnSave:
            _Done(true);
            break;
        default:
            BWindow::MessageReceived(msg);
            break;
    }
}

bool
PermissionRuleEditWindow::QuitRequested()
{
    // Close/Escape without saving: nothing posted.
    return true;
}
