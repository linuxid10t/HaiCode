#pragma once

#include <Window.h>
#include <Messenger.h>

#include <string>

#include <haicode/types.h>

// Compact editor for one permission rule. Pre-filled when editing; posts
// MSG_PERM_RULE_DONE back to the parent with action/resource/effect and the
// index it was opened for (-1 = append).
class PermissionRuleEditWindow : public BWindow {
public:
    PermissionRuleEditWindow(BMessenger target,
                             const haicode::PermissionRule& rule,
                             int index);

    void MessageReceived(BMessage* msg) override;
    bool QuitRequested() override;

private:
    void _Done(bool save);

    BMessenger target_;
    int index_;
};
