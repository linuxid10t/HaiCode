#pragma once

#include <Menu.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Message.h>
#include <Window.h>
#include <MenuBar.h>
#include <MessageFilter.h>
#include "SnapshotMenu.h"

#include <string>

// Shorten a model id for display: show only the last path component, so a
// llama.cpp model id that is a full .gguf path (e.g.
// /boot/home/.../Qwen3.8-27B-UD-Q5_K_XL.gguf) renders as its basename. Ids
// without a slash are returned unchanged. Display only — the full id stays the
// real model value (API requests, session DB, config).
inline std::string short_model_label(const std::string& id) {
    auto pos = id.find_last_of('/');
    return (pos == std::string::npos) ? id : id.substr(pos + 1);
}

// The real model id carried by a model menu item's message. Model items are
// populated with a short display label plus a "model_id" string; placeholder
// items ((loading…), (none available), (off), (fetch failed: …)) carry no
// message and return "", so an empty result doubles as the "not a real
// model" test.
inline std::string model_item_id(const BMenuItem* item) {
    if (!item) return "";
    const char* id = nullptr;
    if (item->Message() && item->Message()->FindString("model_id", &id) == B_OK && id)
        return id;
    return "";
}

// Find the first model menu item whose carried id matches (full-id equality,
// so duplicate short labels can't cause a wrong mark). Returns nullptr when
// absent — callers fall back to the first item or clear the mark.
inline BMenuItem* find_model_item(BMenu* menu, const std::string& id) {
    if (!menu || id.empty()) return nullptr;
    for (int32 i = 0; i < menu->CountItems(); ++i) {
        BMenuItem* item = menu->ItemAt(i);
        if (item && model_item_id(item) == id)
            return item;
    }
    return nullptr;
}

// BMenuField that posts a refresh message to its window when clicked, so a
// previously failed model list can be re-fetched on demand. The refresh
// constant is parameterized so the same class backs MainWindow's model
// dropdown (MSG_MODEL_REFRESH) and SettingsWindow's primary and
// vision-fallback model dropdowns. The window-side handler decides whether
// to actually re-fetch (its failed-load flag), keeping this view stateless.
// Native popup tracking owns each rendered snapshot until Go() returns.
// Replies cancel obsolete tracking and reopen with the latest snapshot.
class ModelMenuField : public BMenuField {
public:
    ModelMenuField(const char* name, const char* label, BMenu* menu,
                   uint32 refreshWhat)
        : BMenuField(name, label, menu)
        , refreshWhat_(refreshWhat)
    {
    }

    void AttachedToWindow() override
    {
        BMenuField::AttachedToWindow();
        class RedirectMouse : public BMessageFilter {
        public:
            explicit RedirectMouse(BHandler* field)
                : BMessageFilter(B_MOUSE_DOWN), field_(field) {}
            filter_result Filter(BMessage*, BHandler** target) override
            {
                *target = field_;
                return B_DISPATCH_MESSAGE;
            }
        private:
            BHandler* field_;
        };
        MenuBar()->AddFilter(new RedirectMouse(this));
    }

    void MouseDown(BPoint) override
    {
        int32 buttons = 0;
        if (Window() && Window()->CurrentMessage()
            && Window()->CurrentMessage()->FindInt32("buttons", &buttons) == B_OK
            && (buttons & B_PRIMARY_MOUSE_BUTTON)) {
            BMessage refresh(refreshWhat_);
            Window()->PostMessage(&refresh);
        }
        _OpenPopup();
    }

    void KeyDown(const char* bytes, int32 count) override
    {
        if (count == 1 && (bytes[0] == B_SPACE || bytes[0] == B_DOWN_ARROW
                || bytes[0] == B_RIGHT_ARROW)) {
            if (Window())
                Window()->PostMessage(refreshWhat_);
            _OpenPopup();
        } else
            BMenuField::KeyDown(bytes, count);
    }

private:
    void _OpenPopup()
    {
        if (!IsEnabled())
            return;
        if (auto* menu = dynamic_cast<SnapshotMenu*>(Menu())) {
            // Anchor under the menu-bar portion only, not the whole field —
            // Bounds() includes the "Model:" label and would offset the popup.
            BRect anchor = MenuBar()
                ? MenuBar()->ConvertToScreen(MenuBar()->Bounds())
                : ConvertToScreen(Bounds());
            menu->OpenModelPopup(anchor);
        }
    }

    uint32 refreshWhat_;
};
