#pragma once

#include <Message.h>
#include <Messenger.h>
#include <PopUpMenu.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

class BMenuField;

class SnapshotMenu : public BPopUpMenu {
public:
    struct Entry {
        std::string label;
        BMessage message;
        bool enabled = true;
        bool selectable = true;
    };

    ~SnapshotMenu() override;
    explicit SnapshotMenu(const char* name);
    void Publish(std::vector<Entry> entries, int32 selected,
                 const std::string& provider, BMessenger target);
    bool Select(const char* key, const std::string& id);
    bool Accept(const BMessage& message, const char* key, std::string& id) const;
    bool HasRevision(const BMessage& message) const;
    void UpdateLabel(BMenuField* field);
    void OpenModelPopup(BRect frame);
    bool AddDynamicItem(add_state state) override;

    static Entry Placeholder(const std::string& label);

private:
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    int32 selected_ = -1;
    std::string provider_;
    BMessenger target_;
    int64 revision_ = 0;
    int64 rendered_revision_ = -1;
    struct PopupState;
    std::shared_ptr<PopupState> popup_;
    BMessage _PopupSnapshot() const;
};
