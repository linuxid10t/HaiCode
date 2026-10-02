#include "SnapshotMenu.h"
#include "ModelMenuField.h"

#include <Application.h>
#include <MenuItem.h>
#include <ListView.h>
#include <StringItem.h>
#include <Window.h>
#include <OS.h>
#include <MessageQueue.h>
#include <cstdio>
#include <cstdlib>
#include <thread>

static void check_at(bool value, int line)
{
    if (!value) {
        std::fprintf(stderr, "snapshot menu check failed at line %d\n", line);
        std::exit(1);
    }
}

#define check(value) check_at(value, __LINE__)

static SnapshotMenu::Entry model(const char* id)
{
    BMessage message('test');
    message.AddString("model_id", id);
    return {short_model_label(id), message};
}

int main()
{
    BApplication app("application/x-vnd.HaiCode-SnapshotMenuTest");
    SnapshotMenu menu("models");
    BMessenger target(&app);
    menu.Publish({SnapshotMenu::Placeholder("(fetch failed: HTTP 503)")}, 0, "local", target);
    menu.AddDynamicItem(BMenu::B_INITIAL_ADD);
    auto* error = menu.ItemAt(0);
    check(error && !error->IsEnabled() && !error->Message());
    menu.Publish({SnapshotMenu::Placeholder("(loading…)")}, 0, "local", target);
    check(menu.ItemAt(0) == error);
    menu.Publish({model("/a/Qwen.gguf"), model("/b/Qwen.gguf")}, 1, "local", target);
    check(menu.ItemAt(0) == error);
    check(std::string(error->Label()) == "(fetch failed: HTTP 503)");
    menu.AddDynamicItem(BMenu::B_PROCESSING);
    menu.AddDynamicItem(BMenu::B_ABORT);
    check(menu.ItemAt(0) == error);
    menu.AddDynamicItem(BMenu::B_INITIAL_ADD);
    check(menu.CountItems() == 2);
    check(std::string(menu.ItemAt(0)->Label()) == menu.ItemAt(1)->Label());
    check(model_item_id(menu.ItemAt(0)) == "/a/Qwen.gguf");
    check(model_item_id(menu.ItemAt(1)) == "/b/Qwen.gguf");
    check(menu.ItemAt(1)->IsMarked());
    check(menu.ItemAt(0)->Target() == &app);
    std::string id;
    BMessage selection(*menu.ItemAt(1)->Message());
    check(menu.Accept(selection, "model_id", id) && id == "/b/Qwen.gguf");
    BMessage wrong(selection);
    wrong.ReplaceString("snapshot_provider", "other");
    check(!menu.Accept(wrong, "model_id", id));
    auto* rendered = menu.ItemAt(0);
    std::thread publisher([&]() {
        for (int i = 0; i < 1000; ++i)
            menu.Publish({model("new")}, 0, "local", target);
    });
    publisher.join();
    check(menu.ItemAt(0) == rendered);
    check(!menu.Accept(selection, "model_id", id));
    menu.AddDynamicItem(BMenu::B_INITIAL_ADD);
    check(model_item_id(menu.ItemAt(0)) == "new");
    check(menu.Select("model_id", "new"));
    check(!menu.Select("model_id", "missing"));
    menu.Publish({SnapshotMenu::Placeholder("(off)")}, 0, "", target);
    menu.AddDynamicItem(BMenu::B_INITIAL_ADD);
    check(!menu.ItemAt(0)->Message());
    check(!menu.Select("model_id", ""));

    BMessage none('none');
    none.AddString("provider_id", "");
    menu.Publish({{"(none)", none}}, 0, "", target);
    menu.AddDynamicItem(BMenu::B_INITIAL_ADD);
    check(menu.Accept(*menu.ItemAt(0)->Message(), "provider_id", id) && id.empty());
    check(menu.Select("provider_id", ""));
    SnapshotMenu live("live models");
    live.Publish({SnapshotMenu::Placeholder("(loading…)")}, 0, "local", target);
    live.OpenModelPopup(BRect(100, 100, 400, 125));
    auto find_menu = [&]() -> BWindow* {
        int32 cookie = 0;
        thread_info info;
        while (get_next_thread_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
            auto* window = dynamic_cast<BWindow*>(BLooper::LooperForThread(info.thread));
            if (window && window->Lock()) {
                if (window->FindView("Models"))
                    return window;
                window->Unlock();
            }
        }
        return nullptr;
    };
    bool loading = false;
    for (int i = 0; i < 200 && !loading; ++i) {
        snooze(10000);
        if (auto* window = find_menu()) {
            auto* native = dynamic_cast<BPopUpMenu*>(window->FindView("Models"));
            loading = native && native->CountItems() == 1
                && !native->ItemAt(0)->IsEnabled();
            window->Unlock();
        }
    }
    check(loading);
    live.Publish({model("/a/loaded.gguf"), model("/b/other.gguf")}, 0, "local", target);
    bool updated = false;
    for (int i = 0; i < 200 && !updated; ++i) {
        snooze(10000);
        if (auto* window = find_menu()) {
            auto* native = dynamic_cast<BPopUpMenu*>(window->FindView("Models"));
            updated = native && native->CountItems() == 2
                && native->ItemAt(0)->IsEnabled();
            if (updated) {
                check(model_item_id(native->ItemAt(1)) == "/b/other.gguf");
                check(live.Accept(*native->ItemAt(1)->Message(), "model_id", id));
                check(id == "/b/other.gguf");
            }
            window->Unlock();
        }
    }
    check(updated);
    live.OpenModelPopup(BRect(100, 100, 400, 125));
    std::puts("snapshot and native popup refresh tests passed");
    return 0;
}
