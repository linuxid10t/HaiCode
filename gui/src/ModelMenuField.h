#pragma once

#include <MenuField.h>
#include <Message.h>
#include <Window.h>

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
