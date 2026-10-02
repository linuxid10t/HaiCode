#include "ModelDatabaseWindow.h"
#include "Messages.h"

#include <Alert.h>
#include <Button.h>
#include <CheckBox.h>
#include <ControlLook.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <ScrollView.h>
#include <StringItem.h>
#include <String.h>
#include <StringView.h>
#include <TextControl.h>

#include <haicode/config.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

const uint32 kFilterChanged = 'mdFl';
const uint32 kMineToggled   = 'mdMn';
const uint32 kSelChanged    = 'mdSl';
const uint32 kBtnAdd        = 'mdAd';
const uint32 kBtnEdit       = 'mdEd';
const uint32 kBtnRevert     = 'mdRv';
const uint32 kBtnSave       = 'mdSv';  // edit dialog OK
const uint32 kVisionPicked  = 'mdVp';  // edit dialog vision popup

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 1050000 → "1,050,000"
std::string fmt_int(int n) {
    std::string digits = std::to_string(n < 0 ? -n : n);
    std::string out;
    int count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (count > 0 && count % 3 == 0) out.insert(out.begin(), ',');
        out.insert(out.begin(), *it);
        ++count;
    }
    return n < 0 ? "-" + out : out;
}

// 0.0750 → "0.075", 10.0 → "10"
std::string fmt_price(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

// Token counts: "200000", "200,000", "200k", "1.05m". Empty = unset.
// Returns false (with `error`) on anything else.
bool parse_tokens(const std::string& raw, std::optional<int>& out,
                  std::string& error, const char* what) {
    out.reset();
    std::string s;
    for (char c : trim(raw))
        if (c != ',' && c != '_' && c != ' ') s += c;
    if (s.empty()) return true;
    double mult = 1.0;
    char last = static_cast<char>(std::tolower(static_cast<unsigned char>(s.back())));
    if (last == 'k') { mult = 1000.0; s.pop_back(); }
    else if (last == 'm') { mult = 1000000.0; s.pop_back(); }
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (s.empty() || !end || *end != '\0' || !std::isfinite(v)) {
        error = std::string(what) + " must be a number of tokens "
                "(e.g. 200000, 200k, or 1.05m).";
        return false;
    }
    v = std::round(v * mult);
    if (v < 1.0 || v > static_cast<double>(INT_MAX)) {
        error = std::string(what) + " must be a positive number of tokens.";
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

// USD per 1M tokens: "3", "0.30", "$15". Empty = unset.
bool parse_price(const std::string& raw, std::optional<double>& out,
                 std::string& error, const char* what) {
    out.reset();
    std::string s = trim(raw);
    if (!s.empty() && s.front() == '$') s.erase(0, 1);
    if (s.empty()) return true;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(v) || v < 0.0) {
        error = std::string(what) + " must be a non-negative price in USD "
                "per 1M tokens.";
        return false;
    }
    out = v;
    return true;
}

std::string describe_entry(const haicode::ModelDbEntry& e, bool tiers) {
    std::string s;
    auto add = [&](const std::string& part) {
        if (!s.empty()) s += "  ·  ";
        s += part;
    };
    if (e.context) add("context " + fmt_int(*e.context));
    if (e.max_output) add("max output " + fmt_int(*e.max_output));
    if (e.vision) add(std::string("vision ") + (*e.vision ? "yes" : "no"));
    if (e.pricing) {
        add("$" + fmt_price(e.pricing->input) + " in / $"
            + fmt_price(e.pricing->output) + " out / $"
            + fmt_price(e.pricing->cache_read) + " cache read / $"
            + fmt_price(e.pricing->cache_write) + " cache write"
            + (tiers ? " (base tier)" : ""));
    }
    return s.empty() ? "none" : s;
}

const int kKeyWidth = 30;

std::string format_line(const std::string& key, const std::string& ctx,
                        const std::string& out, const std::string& vis,
                        const std::string& in_p, const std::string& out_p,
                        const std::string& cr_p, const std::string& cw_p,
                        const std::string& source) {
    std::string k = key;
    if (k.size() > static_cast<size_t>(kKeyWidth))
        k = k.substr(0, kKeyWidth - 1) + "~";
    char buf[256];
    std::snprintf(buf, sizeof buf, "%-*s %11s %9s %6s %8s %8s %8s %8s  %s",
                  kKeyWidth, k.c_str(), ctx.c_str(), out.c_str(), vis.c_str(),
                  in_p.c_str(), out_p.c_str(), cr_p.c_str(), cw_p.c_str(),
                  source.c_str());
    return buf;
}

std::string format_row(const haicode::ModelDbRow& r) {
    const haicode::ModelDbEntry& e = r.effective;
    // "*" marks a value coming from the user's entries; "+" marks a built-in
    // price that is only the base of a long-context/input-size tier ladder.
    auto mark = [](bool user) { return std::string(user ? "*" : ""); };
    std::string ctx = e.context ? fmt_int(*e.context) + mark(r.user_context) : "-";
    std::string out = e.max_output ? fmt_int(*e.max_output) + mark(r.user_max_output) : "-";
    std::string vis = e.vision ? std::string(*e.vision ? "yes" : "no")
                                 + mark(r.user_vision) : "-";
    std::string in_p = "-", out_p = "-", cr_p = "-", cw_p = "-";
    if (e.pricing) {
        const std::string m = r.user_pricing ? "*" : (r.price_tiers ? "+" : "");
        in_p  = fmt_price(e.pricing->input) + m;
        out_p = fmt_price(e.pricing->output) + m;
        cr_p  = fmt_price(e.pricing->cache_read) + mark(r.user_pricing);
        cw_p  = fmt_price(e.pricing->cache_write) + mark(r.user_pricing);
    }
    const char* source = r.has_user() ? (r.builtin_key ? "edited" : "custom")
                                      : "built-in";
    return format_line(r.key, ctx, out, vis, in_p, out_p, cr_p, cw_p, source);
}

// ---- Add/Edit dialog --------------------------------------------------------

class ModelDbEditWindow : public BWindow {
public:
    ModelDbEditWindow(BMessenger target, const std::string& key, bool adding,
                      const haicode::ModelDbEntry& user,
                      const haicode::ModelDbEntry& builtin, bool tiers);

    void MessageReceived(BMessage* msg) override;

private:
    bool _Collect(BMessage& out);

    BMessenger target_;
    bool adding_;
    std::string vision_value_;  // "", "yes", "no"
    BTextControl* key_ = nullptr;
    BTextControl* context_ = nullptr;
    BTextControl* max_out_ = nullptr;
    BTextControl* in_ = nullptr;
    BTextControl* out_ = nullptr;
    BTextControl* cache_read_ = nullptr;
    BTextControl* cache_write_ = nullptr;
};

ModelDbEditWindow::ModelDbEditWindow(BMessenger target, const std::string& key,
                                     bool adding,
                                     const haicode::ModelDbEntry& user,
                                     const haicode::ModelDbEntry& builtin,
                                     bool tiers)
    : BWindow(BRect(220, 180, 700, 520),
              adding ? "Add Model Entry" : "Edit Model Entry",
              B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
    , target_(target)
    , adding_(adding)
{
    auto num = [](const std::optional<int>& v) {
        return v ? std::to_string(*v) : std::string();
    };
    auto price = [&](double haicode::ModelPricing::*field) {
        return user.pricing ? fmt_price((*user.pricing).*field) : std::string();
    };

    key_ = new BTextControl("key", "Model ID or prefix:", key.c_str(), nullptr);
    key_->SetEnabled(adding);
    context_ = new BTextControl("context", "Context window:",
                                num(user.context).c_str(), nullptr);
    max_out_ = new BTextControl("max_out", "Max output:",
                                num(user.max_output).c_str(), nullptr);

    if (user.vision) vision_value_ = *user.vision ? "yes" : "no";
    auto* vision_menu = new BPopUpMenu("vision");
    vision_menu->SetRadioMode(true);
    vision_menu->SetLabelFromMarked(true);
    std::string default_label = "Built-in";
    if (!adding) {
        default_label += builtin.vision
            ? (*builtin.vision ? " (yes)" : " (no)") : " (unknown: no)";
    }
    const struct { std::string label; const char* value; } kVision[] = {
        {default_label, ""}, {"Yes", "yes"}, {"No", "no"},
    };
    for (auto& v : kVision) {
        auto* m = new BMessage(kVisionPicked);
        m->AddString("value", v.value);
        auto* item = new BMenuItem(v.label.c_str(), m);
        vision_menu->AddItem(item);
        if (vision_value_ == v.value) item->SetMarked(true);
    }
    auto* vision_field = new BMenuField("vision_field", "Vision:", vision_menu);

    in_ = new BTextControl("in", "Input $/1M:", price(&haicode::ModelPricing::input).c_str(), nullptr);
    out_ = new BTextControl("out", "Output $/1M:", price(&haicode::ModelPricing::output).c_str(), nullptr);
    cache_read_ = new BTextControl("cache_read", "Cache read $/1M:",
                                   price(&haicode::ModelPricing::cache_read).c_str(), nullptr);
    cache_write_ = new BTextControl("cache_write", "Cache write $/1M:",
                                    price(&haicode::ModelPricing::cache_write).c_str(), nullptr);

    BFont small(*be_plain_font);
    small.SetSize(10.0f);
    std::string builtin_text = adding
        ? std::string("New entry.")
        : "Built-in for this id: " + describe_entry(builtin, tiers);
    auto* builtin_view = new BStringView("builtin", builtin_text.c_str());
    builtin_view->SetFont(&small);
    builtin_view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    auto* help = new BStringView("help",
        "Leave a field blank to use the built-in value. Keys match as\n"
        "case-insensitive prefixes of the model id: the longest matching key\n"
        "wins, and yours wins a tie with a built-in one. Token counts accept\n"
        "200000, 200k or 1.05m. A price override needs input and output\n"
        "(cache prices default to 0) and replaces any built-in tier ladder.");
    help->SetFont(&small);

    auto* save = new BButton("save", "OK", new BMessage(kBtnSave));
    save->MakeDefault(true);
    auto* cancel = new BButton("cancel", "Cancel", new BMessage(B_QUIT_REQUESTED));

    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .AddGrid(B_USE_SMALL_SPACING, B_USE_SMALL_SPACING)
            .AddTextControl(key_, 0, 0)
            .AddTextControl(context_, 0, 1)
            .AddTextControl(max_out_, 0, 2)
            .AddMenuField(vision_field, 0, 3)
            .AddTextControl(in_, 0, 4)
            .AddTextControl(out_, 0, 5)
            .AddTextControl(cache_read_, 0, 6)
            .AddTextControl(cache_write_, 0, 7)
        .End()
        .Add(builtin_view)
        .Add(help)
        .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
            .AddGlue()
            .Add(cancel)
            .Add(save)
        .End()
    .End();

    (adding ? key_ : context_)->MakeFocus(true);
    CenterOnScreen();
}

bool
ModelDbEditWindow::_Collect(BMessage& out)
{
    std::string error;
    const std::string key = trim(key_->Text());
    if (key.empty()) error = "Enter a model id or prefix.";

    std::optional<int> context, max_out;
    std::optional<double> in, outp, cr, cw;
    if (error.empty()) parse_tokens(context_->Text(), context, error, "Context window");
    if (error.empty()) parse_tokens(max_out_->Text(), max_out, error, "Max output");
    if (error.empty()) parse_price(in_->Text(), in, error, "Input price");
    if (error.empty()) parse_price(out_->Text(), outp, error, "Output price");
    if (error.empty()) parse_price(cache_read_->Text(), cr, error, "Cache read price");
    if (error.empty()) parse_price(cache_write_->Text(), cw, error, "Cache write price");
    const bool any_price = in || outp || cr || cw;
    if (error.empty() && any_price && (!in || !outp))
        error = "A price override needs both an input and an output price.";
    if (error.empty() && adding_ && !context && !max_out
            && vision_value_.empty() && !any_price)
        error = "Enter at least one value to override.";

    if (!error.empty()) {
        BAlert* alert = new BAlert("Model entry", error.c_str(), "OK",
                                   nullptr, nullptr, B_WIDTH_AS_USUAL,
                                   B_WARNING_ALERT);
        alert->Go();
        return false;
    }

    out.AddString("key", key.c_str());
    out.AddBool("adding", adding_);
    if (context) out.AddInt32("context", *context);
    if (max_out) out.AddInt32("max_output", *max_out);
    if (!vision_value_.empty()) out.AddBool("vision", vision_value_ == "yes");
    if (any_price) {
        out.AddDouble("input", *in);
        out.AddDouble("output", *outp);
        out.AddDouble("cache_read", cr.value_or(0.0));
        out.AddDouble("cache_write", cw.value_or(0.0));
    }
    return true;
}

void
ModelDbEditWindow::MessageReceived(BMessage* msg)
{
    switch (msg->what) {
        case kVisionPicked: {
            const char* v = nullptr;
            if (msg->FindString("value", &v) == B_OK && v)
                vision_value_ = v;
            break;
        }
        case kBtnSave: {
            BMessage done(MSG_MODEL_DB_ENTRY);
            if (!_Collect(done)) break;
            if (target_.IsValid()) target_.SendMessage(&done);
            PostMessage(B_QUIT_REQUESTED);
            break;
        }
        default:
            BWindow::MessageReceived(msg);
            break;
    }
}

} // namespace

// ---- Model Database window --------------------------------------------------

ModelDatabaseWindow::ModelDatabaseWindow(const std::string& config_path,
                                         BMessenger app)
    : BWindow(BRect(120, 100, 1080, 640), "Model Database",
              B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS)
    , config_path_(config_path)
    , app_(app)
{
    filter_ = new BTextControl("filter", "Search:", "", nullptr);
    filter_->SetModificationMessage(new BMessage(kFilterChanged));
    mine_only_ = new BCheckBox("mine_only", "Only my entries",
                               new BMessage(kMineToggled));

    // Fixed-width font so the columns line up; set before any item is
    // added (items cache their height from the owner's font).
    BFont fixed(be_fixed_font);
    auto* header = new BStringView("header",
        format_line("MODEL / PREFIX", "CONTEXT", "MAX OUT", "VISION",
                    "IN $", "OUT $", "CACHE R", "CACHE W", "SOURCE").c_str());
    header->SetFont(&fixed);
    header->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

    list_ = new BListView("models", B_SINGLE_SELECTION_LIST);
    list_->SetFont(&fixed);
    list_->SetSelectionMessage(new BMessage(kSelChanged));
    list_->SetInvocationMessage(new BMessage(kBtnEdit));  // double-click edits
    auto* scroll = new BScrollView("models_scroll", list_, 0, false, true,
                                   B_FANCY_BORDER);
    scroll->SetExplicitMinSize(BSize(B_SIZE_UNSET, 260));

    BFont small(*be_plain_font);
    small.SetSize(10.0f);
    auto* legend = new BStringView("legend",
        "* = your entry   + = base price of a built-in tier ladder   "
        "Prices: USD per 1M tokens.\n"
        "Keys match as prefixes of the model id; the longest matching key "
        "wins, and your entry wins a tie with a built-in one.\n"
        "Changes are saved to your global config and apply to the next "
        "request — running sessions are not interrupted.");
    legend->SetFont(&small);
    legend->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

    status_ = new BStringView("status", "");
    status_->SetFont(&small);
    status_->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

    auto* add = new BButton("add", "Add" B_UTF8_ELLIPSIS, new BMessage(kBtnAdd));
    edit_btn_ = new BButton("edit", "Edit" B_UTF8_ELLIPSIS, new BMessage(kBtnEdit));
    revert_btn_ = new BButton("revert", "Revert to Built-in",
                              new BMessage(kBtnRevert));
    auto* close = new BButton("close", "Close", new BMessage(B_QUIT_REQUESTED));

    // BStringItem draws its text one label-spacing in from the item frame,
    // inside the scroll view's 2 px fancy border: indent the header to match.
    const float header_inset = be_control_look->DefaultLabelSpacing() + 2.0f;

    BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
        .SetInsets(B_USE_WINDOW_INSETS)
        .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
            .Add(filter_)
            .Add(mine_only_)
        .End()
        .AddGroup(B_HORIZONTAL, 0)
            .SetInsets(header_inset, 0, 0, 0)
            .Add(header)
        .End()
        .Add(scroll)
        .Add(legend)
        .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
            .Add(add)
            .Add(edit_btn_)
            .Add(revert_btn_)
            .Add(status_)
            .AddGlue()
            .Add(close)
        .End()
    .End();

    _Reload();
    filter_->MakeFocus(true);
    CenterOnScreen();
}

void
ModelDatabaseWindow::_Reload()
{
    // Only the GLOBAL file's entries are shown and edited; a project config
    // can still layer its own entries on top at runtime.
    haicode::ConfigLayer layer = haicode::load_layer(config_path_);
    user_ = haicode::model_overrides_from(layer.values);
    rows_ = haicode::build_model_database(user_);
    _Rebuild();
}

void
ModelDatabaseWindow::_Rebuild()
{
    std::string selected_key;
    if (const haicode::ModelDbRow* r = _SelectedRow())
        selected_key = r->key;

    for (int32 i = list_->CountItems() - 1; i >= 0; --i)
        delete list_->RemoveItem(i);
    visible_.clear();

    const std::string needle = lower(trim(filter_->Text()));
    const bool mine_only = mine_only_->Value() == B_CONTROL_ON;
    int32 reselect = -1;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const haicode::ModelDbRow& r = rows_[i];
        if (mine_only && !r.has_user()) continue;
        if (!needle.empty() && lower(r.key).find(needle) == std::string::npos)
            continue;
        if (r.key == selected_key) reselect = list_->CountItems();
        list_->AddItem(new BStringItem(format_row(r).c_str()));
        visible_.push_back(i);
    }
    if (reselect >= 0) {
        list_->Select(reselect);
        list_->ScrollToSelection();
    }
    _UpdateButtons();
}

const haicode::ModelDbRow*
ModelDatabaseWindow::_SelectedRow() const
{
    int32 sel = list_ ? list_->CurrentSelection() : -1;
    if (sel < 0 || static_cast<size_t>(sel) >= visible_.size()) return nullptr;
    return &rows_[visible_[sel]];
}

void
ModelDatabaseWindow::_UpdateButtons()
{
    const haicode::ModelDbRow* r = _SelectedRow();
    edit_btn_->SetEnabled(r != nullptr);
    revert_btn_->SetEnabled(r != nullptr && r->has_user());
}

void
ModelDatabaseWindow::_OpenEditor(const haicode::ModelDbRow* row)
{
    haicode::ModelDbEntry none;
    auto* editor = row
        ? new ModelDbEditWindow(BMessenger(this), row->key, false, row->user,
                                row->builtin,
                                haicode::has_price_tiers(row->key))
        : new ModelDbEditWindow(BMessenger(this), "", true, none, none, false);
    editor->Show();
}

bool
ModelDatabaseWindow::_Save(const std::string& key,
                           const haicode::ModelDbEntry& entry)
{
    std::string err;
    if (!haicode::save_model_db_entry(config_path_, key, entry, err)) {
        BString text;
        text << "Could not save the model database entry:\n\n" << err.c_str();
        BAlert* alert = new BAlert("Model Database", text.String(), "OK",
                                   nullptr, nullptr, B_WIDTH_AS_USUAL,
                                   B_STOP_ALERT);
        alert->Go();
        return false;
    }
    app_.SendMessage(MSG_MODEL_DB_CHANGED);
    _Reload();
    return true;
}

void
ModelDatabaseWindow::_ApplyEntry(BMessage* msg)
{
    const char* key_c = nullptr;
    if (msg->FindString("key", &key_c) != B_OK || !key_c || !*key_c) return;
    const std::string key = key_c;

    haicode::ModelDbEntry entry;
    int32 i = 0;
    bool b = false;
    double in = 0, out = 0, cr = 0, cw = 0;
    if (msg->FindInt32("context", &i) == B_OK) entry.context = i;
    if (msg->FindInt32("max_output", &i) == B_OK) entry.max_output = i;
    if (msg->FindBool("vision", &b) == B_OK) entry.vision = b;
    if (msg->FindDouble("input", &in) == B_OK
            && msg->FindDouble("output", &out) == B_OK) {
        msg->FindDouble("cache_read", &cr);
        msg->FindDouble("cache_write", &cw);
        entry.pricing = haicode::ModelPricing{in, out, cr, cw};
    }

    // "Add" for a key that already has an entry would silently drop the
    // fields the (blank) add dialog didn't repeat — confirm first.
    bool adding = false;
    msg->FindBool("adding", &adding);
    if (adding && !haicode::user_entry(user_, key).empty()) {
        BString text;
        text << "You already have an entry for \"" << key.c_str()
             << "\". Replace it with the values you just entered?";
        BAlert* alert = new BAlert("Model Database", text.String(),
                                   "Cancel", "Replace", nullptr,
                                   B_WIDTH_AS_USUAL, B_WARNING_ALERT);
        alert->SetShortcut(0, B_ESCAPE);
        if (alert->Go() != 1) return;
    }

    if (_Save(key, entry)) {
        BString text;
        text << (entry.empty() ? "Reverted " : "Saved ") << key.c_str() << ".";
        status_->SetText(text.String());
    }
}

void
ModelDatabaseWindow::_RevertSelected()
{
    const haicode::ModelDbRow* r = _SelectedRow();
    if (!r || !r->has_user()) return;
    const std::string key = r->key;
    BString text;
    text << "Remove your entry for \"" << key.c_str() << "\"?\n\n";
    if (!r->builtin.empty())
        text << "The built-in values will apply again.";
    else
        text << "No built-in entry covers this model: it will be unknown "
                "again (no context meter or auto-compaction, no cost "
                "tracking, treated as text-only).";
    BAlert* alert = new BAlert("Model Database", text.String(),
                               "Cancel", "Revert", nullptr,
                               B_WIDTH_AS_USUAL, B_WARNING_ALERT);
    alert->SetShortcut(0, B_ESCAPE);
    if (alert->Go() != 1) return;
    if (_Save(key, haicode::ModelDbEntry{})) {
        BString done;
        done << "Reverted " << key.c_str() << ".";
        status_->SetText(done.String());
    }
}

void
ModelDatabaseWindow::MessageReceived(BMessage* msg)
{
    switch (msg->what) {
        case kFilterChanged:
        case kMineToggled:
            _Rebuild();
            break;
        case kSelChanged:
            _UpdateButtons();
            break;
        case kBtnAdd:
            _OpenEditor(nullptr);
            break;
        case kBtnEdit:
            if (const haicode::ModelDbRow* r = _SelectedRow())
                _OpenEditor(r);
            break;
        case kBtnRevert:
            _RevertSelected();
            break;
        case MSG_MODEL_DB_ENTRY:
            _ApplyEntry(msg);
            break;
        case MSG_MODEL_DB_ACTIVATE:
            // Re-read in case the file changed (Settings save, hand edit).
            _Reload();
            Activate();
            break;
        default:
            BWindow::MessageReceived(msg);
            break;
    }
}

bool
ModelDatabaseWindow::QuitRequested()
{
    app_.SendMessage(MSG_MODEL_DB_CLOSED);
    return true;
}
