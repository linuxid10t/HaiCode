#include "SnapshotMenu.h"

#include <MenuField.h>
#include <MenuItem.h>
#include <Screen.h>
#include <atomic>
#include <thread>

struct SnapshotMenu::PopupState {
    std::mutex mutex;
    BMessage snapshot;
    BMessenger menu;
    void Cancel()
    {
        BMessenger target;
        {
            std::lock_guard<std::mutex> lock(mutex);
            target = menu;
        }
        BMessage escape(B_KEY_DOWN);
        escape.AddString("bytes", "\x1b");
        if (target.IsValid())
            target.SendMessage(&escape);
    }
    std::atomic<int64> revision{0};
    std::atomic<bool> closed{false};
    std::atomic<bool> active{true};
    std::thread worker;
};

SnapshotMenu::SnapshotMenu(const char* name)
    : BPopUpMenu(name)
{
    SetRadioMode(true);
    SetLabelFromMarked(false);
}

SnapshotMenu::~SnapshotMenu()
{
    if (popup_) {
        popup_->closed = true;
        popup_->Cancel();
        if (popup_->worker.joinable())
            popup_->worker.join();
    }
}

SnapshotMenu::Entry
SnapshotMenu::Placeholder(const std::string& label)
{
    return {label, BMessage(), false, false};
}

void
SnapshotMenu::Publish(std::vector<Entry> entries, int32 selected,
                      const std::string& provider, BMessenger target)
{
    std::shared_ptr<PopupState> popup;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_ = std::move(entries);
        selected_ = selected;
        provider_ = provider;
        target_ = target;
        ++revision_;
        popup = popup_;
    }
    if (popup && popup->active) {
        BMessage update = _PopupSnapshot();
        {
            std::lock_guard<std::mutex> lock(popup->mutex);
            popup->snapshot = update;
            ++popup->revision;
        }
        popup->Cancel();
    }
}

BMessage
SnapshotMenu::_PopupSnapshot() const
{
    BMessage update;
    std::lock_guard<std::mutex> lock(mutex_);
    update.AddInt32("selected", selected_);
    update.AddMessenger("target", target_);
    for (const auto& value : entries_) {
        BMessage entry;
        entry.AddString("label", value.label.c_str());
        entry.AddBool("enabled", value.enabled && value.selectable);
        if (value.selectable) {
            BMessage selection(value.message);
            selection.AddInt64("snapshot_revision", revision_);
            selection.AddString("snapshot_provider", provider_.c_str());
            entry.AddMessage("selection", &selection);
        }
        update.AddMessage("entry", &entry);
    }
    return update;
}

void
SnapshotMenu::OpenModelPopup(BRect frame)
{
    if (popup_ && popup_->active) {
        popup_->closed = true;
        popup_->Cancel();
        return;
    }
    if (popup_ && popup_->worker.joinable())
        popup_->worker.join();
    auto state = std::make_shared<PopupState>();
    state->snapshot = _PopupSnapshot();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        popup_ = state;
    }
    state->worker = std::thread([state, frame]() {
        struct Tracking {
            PopupState* state;
            int64 revision;
        };
        while (!state->closed) {
            BMessage snapshot;
            int64 revision;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                snapshot = state->snapshot;
                revision = state->revision;
            }
            BPopUpMenu menu("Models", true, false);
            BMessage entry;
            int32 selected = -1;
            snapshot.FindInt32("selected", &selected);
            for (int32 i = 0; snapshot.FindMessage("entry", i, &entry) == B_OK; ++i) {
                const char* label = "";
                bool enabled = false;
                entry.FindString("label", &label);
                entry.FindBool("enabled", &enabled);
                BMessage selection;
                BMessage* message = nullptr;
                if (entry.FindMessage("selection", &selection) == B_OK)
                    message = new BMessage(selection);
                auto* item = new BMenuItem(label, message);
                item->SetEnabled(enabled);
                menu.AddItem(item);
                if (i == selected)
                    item->SetMarked(true);
            }
            Tracking tracking{state.get(), revision};
            menu.SetTrackingHook([](BMenu* menu, void* data) {
                auto* tracking = static_cast<Tracking*>(data);
                {
                    std::lock_guard<std::mutex> lock(tracking->state->mutex);
                    tracking->state->menu = BMessenger(menu);
                }
                return tracking->state->closed
                    || tracking->state->revision != tracking->revision;
            }, &tracking);
            BPoint where(frame.left, frame.bottom + 1);
            BRect screen = BScreen().Frame();
            // Size the menu so its height is known before clamping: Go()
            // places the top-left at `where`, so a list that would run off
            // the screen bottom flips above the field instead.
            menu.ResizeToPreferred();
            float height = menu.Bounds().Height();
            float width = menu.Bounds().Width();
            if (where.y + height > screen.bottom)
                where.y = std::max(screen.top, frame.top - height - 1);
            where.x = std::max(screen.left,
                std::min(where.x, screen.right - width));
            BMenuItem* item = menu.Go(where, false, true);
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->menu = BMessenger();
            }
            if (state->closed)
                break;
            if (state->revision != revision)
                continue;
            if (item && item->IsEnabled() && item->Message()) {
                BMessage selection(*item->Message());
                BMessenger target;
                snapshot.FindMessenger("target", &target);
                target.SendMessage(&selection);
            }
            break;
        }
        state->active = false;
    });
}

bool
SnapshotMenu::Select(const char* key, const std::string& id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < entries_.size(); ++i) {
        const char* value = nullptr;
        const auto& entry = entries_[i];
        if (entry.enabled && entry.selectable
                && entry.message.FindString(key, &value) == B_OK && id == value) {
            if (selected_ != static_cast<int32>(i)) {
                selected_ = i;
                ++revision_;
            }
            return true;
        }
    }
    return false;
}

bool
SnapshotMenu::HasRevision(const BMessage& message) const
{
    int64 revision;
    return message.FindInt64("snapshot_revision", &revision) == B_OK;
}

bool
SnapshotMenu::Accept(const BMessage& message, const char* key, std::string& id) const
{
    int64 revision;
    const char* provider = nullptr;
    const char* value = nullptr;
    if (message.FindInt64("snapshot_revision", &revision) != B_OK
            || message.FindString("snapshot_provider", &provider) != B_OK
            || message.FindString(key, &value) != B_OK)
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (revision != revision_ || provider_ != provider)
        return false;
    for (const auto& entry : entries_) {
        const char* candidate = nullptr;
        if (entry.enabled && entry.selectable
                && entry.message.FindString(key, &candidate) == B_OK
                && std::string(candidate) == value) {
            id = value;
            return true;
        }
    }
    return false;
}

void
SnapshotMenu::UpdateLabel(BMenuField* field)
{
    std::string label;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (selected_ >= 0 && selected_ < static_cast<int32>(entries_.size()))
            label = entries_[selected_].label;
    }
    if (field && field->MenuItem())
        field->MenuItem()->SetLabel(label.c_str());
}

bool
SnapshotMenu::AddDynamicItem(add_state state)
{
    if (state != B_INITIAL_ADD)
        return false;
    std::vector<Entry> entries;
    BMessenger target;
    std::string provider;
    int32 selected;
    int64 revision;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        revision = revision_;
        if (revision == rendered_revision_)
            return false;
        entries = entries_;
        selected = selected_;
        provider = provider_;
        target = target_;
    }
    // Only the pre-display lifecycle may replace objects used by menu tracking.
    RemoveItems(0, CountItems(), true);
    for (size_t i = 0; i < entries.size(); ++i) {
        auto& entry = entries[i];
        BMessage* message = nullptr;
        if (entry.selectable) {
            message = new BMessage(entry.message);
            message->AddInt64("snapshot_revision", revision);
            message->AddString("snapshot_provider", provider.c_str());
        }
        auto* item = new BMenuItem(entry.label.c_str(), message);
        item->SetEnabled(entry.enabled);
        item->SetTarget(target);
        AddItem(item);
        if (static_cast<int32>(i) == selected)
            item->SetMarked(true);
    }
    rendered_revision_ = revision;
    return false;
}
