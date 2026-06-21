#pragma once

#include <Window.h>
#include <Messenger.h>
#include <String.h>
#include <vector>

class BRadioButton;
class BTextControl;

class AskUserWindow : public BWindow {
public:
    AskUserWindow(const BString& question,
                  const std::vector<BString>& options,
                  const BString& session_id,
                  const BString& call_id,
                  BMessenger reply_target);
    virtual ~AskUserWindow();
    
    virtual void MessageReceived(BMessage* msg);
    virtual bool QuitRequested();

private:
    BString           fQuestion;
    BString           fSessionID;
    BString           fCallID;
    BMessenger        fReplyTarget;
    
    std::vector<BRadioButton*> fOptionRadios;
    BRadioButton*     fOtherRadio;
    BTextControl*     fOtherText;
    
    int32             fOptionCount;
    bool              fAnswered;
};
