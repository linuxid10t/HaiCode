#pragma once

#include <Window.h>
#include <Messenger.h>

#include <string>
#include <vector>

#include <haicode/model_db.h>

class BButton;
class BCheckBox;
class BListView;
class BStringView;
class BTextControl;

// Settings → Model Database…: a searchable view of every model the app knows
// (built-in context windows, output caps, vision flags, and prices) merged
// with the user's own entries, which it adds, edits, and reverts. Entries
// are written straight to the GLOBAL config file ("models", "max_output",
// "vision", "pricing" maps) through save_model_db_entry; each write posts
// MSG_MODEL_DB_CHANGED to be_app, which reloads the config and pushes the
// new overrides into the running engine without interrupting sessions.
// One instance per application (HaiCodeApp keeps the pointer; cleared via
// MSG_MODEL_DB_CLOSED).
class ModelDatabaseWindow : public BWindow {
public:
    ModelDatabaseWindow(const std::string& config_path, BMessenger app);

    void MessageReceived(BMessage* msg) override;
    bool QuitRequested() override;

private:
    void _Reload();          // config file → user_ + rows_, then _Rebuild()
    void _Rebuild();         // rows_ + filter → list
    void _UpdateButtons();
    const haicode::ModelDbRow* _SelectedRow() const;
    void _OpenEditor(const haicode::ModelDbRow* row);  // nullptr = add
    void _ApplyEntry(BMessage* msg);
    void _RevertSelected();
    bool _Save(const std::string& key, const haicode::ModelDbEntry& entry);

    std::string config_path_;
    BMessenger app_;
    haicode::ModelOverrides user_;
    std::vector<haicode::ModelDbRow> rows_;
    std::vector<size_t> visible_;   // list index → rows_ index

    BTextControl* filter_ = nullptr;
    BCheckBox* mine_only_ = nullptr;
    BListView* list_ = nullptr;
    BButton* edit_btn_ = nullptr;
    BButton* revert_btn_ = nullptr;
    BStringView* status_ = nullptr;
};
