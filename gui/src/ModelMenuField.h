#pragma once

#include <Menu.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Message.h>
#include <Window.h>

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
// BMenuField tracks its menu on a separate menu-task thread, so the window
// looper stays free to process the refresh while the menu is open.
class ModelMenuField : public BMenuField {
public:
    ModelMenuField(const char* name, const char* label, BMenu* menu,
                   uint32 refreshWhat)
        : BMenuField(name, label, menu)
        , refreshWhat_(refreshWhat)
    {
    }

    void MouseDown(BPoint where) override
    {
        int32 buttons = 0;
        if (Window() && Window()->CurrentMessage()
            && Window()->CurrentMessage()->FindInt32("buttons", &buttons) == B_OK
            && (buttons & B_PRIMARY_MOUSE_BUTTON)) {
            BMessage refresh(refreshWhat_);
            Window()->PostMessage(&refresh);
        }
        BMenuField::MouseDown(where);
    }

private:
    uint32 refreshWhat_;
};
