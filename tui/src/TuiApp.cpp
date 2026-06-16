// TuiApp.cpp — ncurses TUI frontend for haicode
//
// Threading model:
//   Main thread  : ncurses render + key input via select()
//   Engine threads: post EngineEvents via push_engine_event() → write to wake_pipe_
//   Main loop wakes from select(), drains ev_queue_, re-renders.

#include "TuiApp.h"
#include <haicode/events.h>
#include <haicode/model_info.h>

#include <ncurses.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/types.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <cassert>
#include <algorithm>
#include <sstream>
#include <cstdio>
#include <nlohmann/json.hpp>

using namespace haicode;
using json = nlohmann::json;

namespace tui {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string truncate(const std::string& s, int max_len) {
    if (max_len <= 0) return "";
    if ((int)s.size() <= max_len) return s;
    return s.substr(0, max_len - 1) + "…";
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

TuiApp::TuiApp(SessionEngine& engine,
               SessionStore& store,
               SessionEventBus& bus,
               ConfigLoader& /*config_loader*/,
               const AppConfig& config,
               const std::string& project_dir)
    : engine_(engine)
    , store_(store)
    , bus_(bus)
    , config_(config)
    , project_dir_(project_dir)
{
    if (::pipe(wake_pipe_) != 0) {
        throw std::runtime_error(std::string("pipe() failed: ") + strerror(errno));
    }
    // Make write end non-blocking so engine threads never stall
    ::fcntl(wake_pipe_[1], F_SETFL, O_NONBLOCK);
}

TuiApp::~TuiApp() {
    teardown_ncurses();
    if (wake_pipe_[0] >= 0) ::close(wake_pipe_[0]);
    if (wake_pipe_[1] >= 0) ::close(wake_pipe_[1]);
}

// ---------------------------------------------------------------------------
// ncurses setup
// ---------------------------------------------------------------------------

void TuiApp::init_ncurses() {
    ::initscr();
    ::cbreak();
    ::noecho();
    ::keypad(stdscr, TRUE);
    ::set_escdelay(25);
    ::curs_set(1);

    if (::has_colors()) {
        ::start_color();
        ::use_default_colors();
        ::init_pair(CP_USER,        COLOR_CYAN,    -1);
        ::init_pair(CP_ASSISTANT,   COLOR_GREEN,   -1);
        ::init_pair(CP_TOOL_HEADER, COLOR_YELLOW,  -1);
        ::init_pair(CP_TOOL_BODY,   -1,            -1);
        ::init_pair(CP_TOOL_OK,     COLOR_GREEN,   -1);
        ::init_pair(CP_TOOL_ERR,    COLOR_RED,     -1);
        ::init_pair(CP_STATUS,      -1,            -1);
        ::init_pair(CP_SESSION_ACT, COLOR_CYAN,    -1);
        ::init_pair(CP_PERM_SEL,    COLOR_MAGENTA, -1);
    }

    layout();
}

void TuiApp::teardown_ncurses() {
    if (win_sessions_) { ::delwin(win_sessions_); win_sessions_ = nullptr; }
    if (win_chat_)     { ::delwin(win_chat_);     win_chat_     = nullptr; }
    if (win_input_)    { ::delwin(win_input_);    win_input_    = nullptr; }
    if (win_status_)   { ::delwin(win_status_);   win_status_   = nullptr; }
    ::endwin();
}

void TuiApp::layout() {
    getmaxyx(stdscr, rows_, cols_);

    int chat_width   = cols_ - kSessionPaneWidth - 1; // -1 for divider
    int chat_top     = 0;
    int chat_rows    = rows_ - kInputHeight - kStatusHeight;
    int input_top    = chat_rows;
    int status_top   = rows_ - kStatusHeight;

    if (win_sessions_) ::delwin(win_sessions_);
    if (win_chat_)     ::delwin(win_chat_);
    if (win_input_)    ::delwin(win_input_);
    if (win_status_)   ::delwin(win_status_);

    win_sessions_ = ::newwin(chat_rows, kSessionPaneWidth, chat_top, 0);
    win_chat_     = ::newwin(chat_rows, chat_width, chat_top, kSessionPaneWidth + 1);
    win_input_    = ::newwin(kInputHeight, cols_, input_top, 0);
    win_status_   = ::newwin(kStatusHeight, cols_, status_top, 0);

    ::scrollok(win_chat_, FALSE);
    ::keypad(win_input_, TRUE);
    ::keypad(stdscr,     TRUE);
}

// ---------------------------------------------------------------------------
// Event bus subscriptions
// ---------------------------------------------------------------------------

void TuiApp::subscribe_events() {
    bus_.subscribe(events::EventType::TextDelta, [this](const json& j) {
        EngineEvent ev;
        ev.kind       = EngineEventKind::TextDelta;
        ev.session_id = j.value("session_id", "");
        ev.str1       = j.value("delta", "");
        push_engine_event(std::move(ev));
    });

    bus_.subscribe(events::EventType::StepStarted, [this](const json& j) {
        EngineEvent ev;
        ev.kind       = EngineEventKind::StepStarted;
        ev.session_id = j.value("session_id", "");
        push_engine_event(std::move(ev));
    });

    bus_.subscribe(events::EventType::StepEnded, [this](const json& j) {
        EngineEvent ev;
        ev.kind       = EngineEventKind::StepEnded;
        ev.session_id = j.value("session_id", "");
        ev.str1       = j.value("finish_reason", "");
        if (j.contains("usage") && j["usage"].is_object()) {
            ev.int1 = j["usage"].value("input", 0);
            ev.int2 = j["usage"].value("output", 0);
            ev.dbl1 = j["usage"].value("cost_usd", 0.0);
        }
        push_engine_event(std::move(ev));
    });

    bus_.subscribe(events::EventType::PlanProposed, [this](const json& j) {
        EngineEvent ev;
        ev.kind       = EngineEventKind::PlanProposed;
        ev.session_id = j.value("session_id", "");
        ev.str3       = j.value("plan", "");
        ev.str2       = j.value("path", "");
        push_engine_event(std::move(ev));
    });

    bus_.subscribe(events::EventType::StepFailed, [this](const json& j) {
        EngineEvent ev;
        ev.kind       = EngineEventKind::StepFailed;
        ev.session_id = j.value("session_id", "");
        ev.str1       = j.value("error", "unknown error");
        push_engine_event(std::move(ev));
    });

    bus_.subscribe(events::EventType::ToolCalled, [this](const json& j) {
        EngineEvent ev;
        ev.kind       = EngineEventKind::ToolCalled;
        ev.session_id = j.value("session_id", "");
        ev.str1       = j.value("tool_name", "");
        ev.str2       = j.contains("input") ? j["input"].dump() : "{}";
        push_engine_event(std::move(ev));
    });

    bus_.subscribe(events::EventType::ToolSuccess, [this](const json& j) {
        EngineEvent ev;
        ev.kind       = EngineEventKind::ToolResult;
        ev.session_id = j.value("session_id", "");
        ev.str1       = j.value("output", "");
        ev.str2       = j.value("call_id", "");
        ev.bool1      = true;
        push_engine_event(std::move(ev));
    });

    bus_.subscribe(events::EventType::ToolFailed, [this](const json& j) {
        EngineEvent ev;
        ev.kind       = EngineEventKind::ToolResult;
        ev.session_id = j.value("session_id", "");
        ev.str1       = j.value("error", "");
        ev.str2       = j.value("call_id", "");
        ev.bool1      = false;
        push_engine_event(std::move(ev));
    });

    // Note: PermissionRequested events are delivered directly from main.cpp's
    // PermissionGate ask-callback via push_engine_event() (not via the bus),
    // because the callback has a direct pointer to TuiApp and because using
    // push_engine_event directly avoids the round-trip through the bus and
    // avoids pointer-encoding in JSON.
}

// ---------------------------------------------------------------------------
// Cross-thread event posting
// ---------------------------------------------------------------------------

void TuiApp::push_engine_event(EngineEvent ev) {
    {
        std::lock_guard<std::mutex> lk(ev_mutex_);
        ev_queue_.push_back(std::move(ev));
    }
    // Wake the select() in the main loop
    char byte = 1;
    (void)::write(wake_pipe_[1], &byte, 1);
}

// ---------------------------------------------------------------------------
// Drain and handle queued engine events
// ---------------------------------------------------------------------------

void TuiApp::process_engine_events() {
    // Drain the wake pipe
    char buf[64];
    (void)::read(wake_pipe_[0], buf, sizeof(buf));

    std::deque<EngineEvent> local;
    {
        std::lock_guard<std::mutex> lk(ev_mutex_);
        local.swap(ev_queue_);
    }

    for (auto& ev : local) {
        if (!ev.session_id.empty() && ev.session_id != active_session_id_) {
            // Event for a different session — ignore for display but track tokens
            if (ev.kind == EngineEventKind::StepEnded) {
                total_tokens_ += ev.int1 + ev.int2;
            }
            continue;
        }

        switch (ev.kind) {
        case EngineEventKind::TextDelta:
            engine_running_ = true;
            thinking_ = false;
            append_text_delta(ev.str1);
            break;

        case EngineEventKind::StepStarted:
            engine_running_ = true;
            thinking_ = true;
            break;

        case EngineEventKind::StepEnded:
            engine_running_ = false;
            thinking_ = false;
            last_prompt_input_   += ev.int1;
            last_prompt_output_  += ev.int2;
            session_input_total_ += ev.int1;
            session_output_total_+= ev.int2;
            session_cost_        += ev.dbl1;
            total_tokens_        += ev.int1 + ev.int2;
            if (ev.int1 > 0) current_context_tokens_ = ev.int1;
            end_streaming();
            // Refresh session list (title may have changed)
            sessions_ = store_.list();
            break;

        case EngineEventKind::StepFailed:
            engine_running_ = false;
            thinking_ = false;
            end_streaming();
            append_line({ LineType::System,
                          std::string("[Error] ") + ev.str1 });
            break;

        case EngineEventKind::ToolCalled:
            engine_running_ = true;
            thinking_ = false;
            end_streaming();
            append_tool_called(ev.str1, ev.str2);
            break;

        case EngineEventKind::ToolResult:
            append_tool_result(ev.str2, ev.str1, ev.bool1);
            break;

        case EngineEventKind::SessionsChanged:
            sessions_ = store_.list();
            break;

        case EngineEventKind::PermissionReq:
            perm_pending_ = *ev.perm;
            perm_sel_     = 0;
            perm_visible_ = true;
            break;

        case EngineEventKind::PlanProposed:
            plan_text_      = ev.str3;
            plan_path_      = ev.str2;
            plan_session_id_= ev.session_id;
            plan_scroll_    = 0;
            plan_visible_   = true;
            break;
        }
    }

    render_all();
}

// ---------------------------------------------------------------------------
// Session management
// ---------------------------------------------------------------------------

void TuiApp::new_session() {
    std::string model = config_.model.empty() ? "claude-opus-4-5" : config_.model;
    std::string sid = engine_.create_session(project_dir_, config_.agent, model);
    sessions_ = store_.list();
    // Select the new session
    for (int i = 0; i < (int)sessions_.size(); ++i) {
        if (sessions_[i].id == sid) { session_sel_ = i; break; }
    }
    select_session(session_sel_);
}

void TuiApp::select_session(int idx) {
    if (sessions_.empty()) return;
    idx = std::clamp(idx, 0, (int)sessions_.size() - 1);
    session_sel_ = idx;

    active_session_id_ = sessions_[idx].id;
    chat_lines_.clear();
    chat_scroll_ = 0;
    streaming_   = false;
    engine_running_ = false;
    thinking_       = false;
    total_tokens_   = sessions_[idx].tokens.input + sessions_[idx].tokens.output;
    last_prompt_input_  = 0;
    last_prompt_output_ = 0;
    session_input_total_   = sessions_[idx].tokens.input;
    session_output_total_  = sessions_[idx].tokens.output;
    session_cost_          = sessions_[idx].cost;
    current_context_tokens_ = 0;

    load_history(active_session_id_);

    // Compute max context from current model + provider
    std::string model_id    = config_.model;
    std::string provider_id = "anthropic";
    try {
        auto mj = json::parse(sessions_[idx].model_json);
        model_id    = mj.value("id", model_id);
        provider_id = mj.value("provider_id", provider_id);
    } catch (...) {}
    max_context_ = haicode::get_context_window(provider_id, model_id,
                                               config_.model_contexts);
}

void TuiApp::load_history(const std::string& session_id) {
    auto messages = store_.load_messages(session_id);
    for (auto& msg : messages) {
        try {
            auto j = json::parse(msg.data_json);
            if (msg.type == "user") {
                std::string text = j.value("text", "");
                append_line({ LineType::UserText, "User: " + text });
                append_line({ LineType::Separator, "" });
            } else if (msg.type == "assistant") {
                // May have text blocks and tool_use blocks
                if (j.contains("content") && j["content"].is_array()) {
                    for (auto& block : j["content"]) {
                        std::string btype = block.value("type", "");
                        if (btype == "text") {
                            std::string t = block.value("text", "");
                            append_line({ LineType::AssistantText, "Assistant: " + t });
                            append_line({ LineType::Separator, "" });
                        } else if (btype == "tool_use") {
                            std::string tname = block.value("name", "");
                            std::string inp   = block.contains("input")
                                                ? block["input"].dump() : "{}";
                            append_tool_called(tname, inp);
                        }
                    }
                } else if (j.contains("text")) {
                    std::string t = j.value("text", "");
                    append_line({ LineType::AssistantText, "Assistant: " + t });
                    append_line({ LineType::Separator, "" });
                }
            } else if (msg.type == "tool_result") {
                std::string call_id = j.value("call_id", "");
                std::string output  = j.value("output", "");
                bool        success = j.value("success", true);
                append_tool_result(call_id, output, success);
            }
        } catch (...) {
            // Ignore malformed history entries
        }
    }
    chat_scroll_ = 0; // pin to bottom after loading history
}

// ---------------------------------------------------------------------------
// Chat line helpers
// ---------------------------------------------------------------------------

void TuiApp::append_line(const ChatLine& line) {
    chat_lines_.push_back(line);
    // Keep a reasonable cap to avoid unbounded memory
    if (chat_lines_.size() > 8000) {
        chat_lines_.erase(chat_lines_.begin(),
                          chat_lines_.begin() + 1000);
    }
}

void TuiApp::append_text_delta(const std::string& delta) {
    if (!streaming_) {
        // Start a new streaming assistant line
        append_line({ LineType::AssistantText, "Assistant: ", true });
        streaming_ = true;
    }
    // Append delta to the last line
    ChatLine& last = chat_lines_.back();
    last.text += delta;
}

void TuiApp::end_streaming() {
    if (streaming_) {
        if (!chat_lines_.empty()) {
            chat_lines_.back().streaming = false;
        }
        streaming_ = false;
        append_line({ LineType::Separator, "" });
    }
}

void TuiApp::append_tool_called(const std::string& tool_name,
                                 const std::string& input_json) {
    // Header line: ╔ bash ════
    append_line({ LineType::ToolHeader,
                  std::string("╔ ") + tool_name + " ════════════════════════" });

    // Show first ~5 lines of the input as tool body
    std::string body;
    try {
        auto j = json::parse(input_json);
        // If there's a "command" or "code" key, show that directly
        if (j.contains("command")) {
            body = "$ " + j["command"].get<std::string>();
        } else if (j.contains("code")) {
            body = j["code"].get<std::string>();
        } else if (j.contains("path") && j.contains("content")) {
            body = j["path"].get<std::string>();
        } else {
            body = j.dump(2);
        }
    } catch (...) {
        body = input_json;
    }

    // Split body into lines
    std::istringstream ss(body);
    std::string ln;
    int count = 0;
    while (std::getline(ss, ln) && count < 8) {
        append_line({ LineType::ToolBody, "║ " + ln });
        ++count;
    }
    if (count == 8) {
        append_line({ LineType::ToolBody, "║ ..." });
    }
    append_line({ LineType::ToolBody, "╚══════════════════════════════" });
}

void TuiApp::append_tool_result(const std::string& /*call_id*/,
                                 const std::string& output,
                                 bool success) {
    std::string prefix = success ? "✓ " : "✗ ";
    // Show only first line of output for brevity
    std::string first_line = output;
    auto nl = first_line.find('\n');
    if (nl != std::string::npos) first_line = first_line.substr(0, nl) + " …";
    append_line({ success ? LineType::ToolResult : LineType::System,
                  prefix + first_line });
    append_line({ LineType::Separator, "" });
}

// ---------------------------------------------------------------------------
// Word wrap
// ---------------------------------------------------------------------------

std::vector<std::string> TuiApp::wrap(const std::string& text, int width) const {
    std::vector<std::string> result;
    if (width <= 0) { result.push_back(text); return result; }

    std::istringstream stream(text);
    std::string paragraph;
    while (std::getline(stream, paragraph)) {
        if (paragraph.empty()) { result.push_back(""); continue; }
        int pos = 0;
        int len = (int)paragraph.size();
        while (pos < len) {
            int take = std::min(width, len - pos);
            // Try to break at a space if possible
            if (pos + take < len && take == width) {
                int break_at = take;
                for (int k = take - 1; k >= width / 2; --k) {
                    if (paragraph[pos + k] == ' ') { break_at = k + 1; break; }
                }
                result.push_back(paragraph.substr(pos, break_at));
                pos += break_at;
            } else {
                result.push_back(paragraph.substr(pos, take));
                pos += take;
            }
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Key handling
// ---------------------------------------------------------------------------

void TuiApp::handle_key(int key) {
    // ---------- Permission overlay ----------
    if (perm_visible_) {
        switch (key) {
        case KEY_LEFT:
        case 'h':
            perm_sel_ = std::max(0, perm_sel_ - 1);
            break;
        case KEY_RIGHT:
        case 'l':
            perm_sel_ = std::min(2, perm_sel_ + 1);
            break;
        case '\n':
        case KEY_ENTER: {
            haicode::PermissionEffect effect;
            if (perm_sel_ == 0)      effect = haicode::PermissionEffect::Allow;
            else if (perm_sel_ == 1) effect = haicode::PermissionEffect::Allow; // "always" — same Allow; gate handles persistence
            else                      effect = haicode::PermissionEffect::Deny;

            perm_visible_ = false;
            if (perm_pending_.promise) {
                perm_pending_.promise->set_value(effect);
                perm_pending_.promise = nullptr;
            }
            break;
        }
        case 27: // Escape → deny
            perm_visible_ = false;
            if (perm_pending_.promise) {
                perm_pending_.promise->set_value(haicode::PermissionEffect::Deny);
                perm_pending_.promise = nullptr;
            }
            break;
        default:
            break;
        }
        render_all();
        return;
    }

    // ---------- Plan-review overlay ----------
    if (plan_visible_) {
        int visible_rows = std::max(5, rows_ - 10);
        switch (key) {
        case 'a':
        case 'A':
        case '\n':
        case KEY_ENTER:
            if (!plan_session_id_.empty()) {
                engine_.set_mode(plan_session_id_, SessionMode::Build);
            }
            plan_visible_ = false;
            break;
        case 'd':
        case 'D':
        case 27:
        case 'q':
        case 'Q':
            plan_visible_ = false;
            break;
        case KEY_UP:
        case 'k':
            ++plan_scroll_;
            break;
        case KEY_DOWN:
        case 'j':
            plan_scroll_ = std::max(0, plan_scroll_ - 1);
            break;
        case KEY_PPAGE:
            plan_scroll_ += visible_rows;
            break;
        case KEY_NPAGE:
            plan_scroll_ = std::max(0, plan_scroll_ - visible_rows);
            break;
        default:
            break;
        }
        render_all();
        return;
    }

    // ---------- Global bindings ----------
    switch (key) {
    case 3:  // Ctrl+C
    case 24: // Ctrl+X
        if (!active_session_id_.empty()) {
            engine_.interrupt(active_session_id_);
        }
        return;

    case 14: // Ctrl+N
        new_session();
        render_all();
        return;

    case 16: // Ctrl+P — toggle Build/Plan mode
        toggle_mode();
        render_all();
        return;

    case '\t': // Tab — switch focus
        focus_ = (focus_ == 0) ? 1 : 0;
        render_all();
        return;

    case KEY_RESIZE:
        layout();
        render_all();
        return;

    default:
        break;
    }

    // ---------- Sessions pane ----------
    if (focus_ == 0) {
        switch (key) {
        case KEY_UP:
            if (session_sel_ > 0) {
                select_session(session_sel_ - 1);
                render_all();
            }
            break;
        case KEY_DOWN:
            if (session_sel_ < (int)sessions_.size() - 1) {
                select_session(session_sel_ + 1);
                render_all();
            }
            break;
        case '\n':
        case KEY_ENTER:
            select_session(session_sel_);
            focus_ = 1;
            render_all();
            break;
        default:
            break;
        }
        return;
    }

    // ---------- Chat + Input pane ----------
    switch (key) {
    // --- Scroll ---
    case KEY_PPAGE: // Page Up
        chat_scroll_ += (rows_ - kInputHeight - kStatusHeight - 2);
        render_all();
        return;

    case KEY_NPAGE: // Page Down
        chat_scroll_ = std::max(0, chat_scroll_ - (rows_ - kInputHeight - kStatusHeight - 2));
        render_all();
        return;

    case KEY_UP:
        ++chat_scroll_;
        render_all();
        return;

    case KEY_DOWN:
        chat_scroll_ = std::max(0, chat_scroll_ - 1);
        render_all();
        return;

    // --- Submit ---
    case '\n':
    case KEY_ENTER: {
        if (input_buf_.empty()) return;
        if (active_session_id_.empty()) new_session();

        std::string text = input_buf_;
        input_buf_.clear();
        input_cursor_ = 0;
        input_scroll_ = 0;

        // Show user message immediately
        append_line({ LineType::UserText, "User: " + text });
        append_line({ LineType::Separator, "" });
        chat_scroll_ = 0; // pin to bottom
        engine_running_ = true;
        thinking_ = true;
        last_prompt_input_ = 0;
        last_prompt_output_ = 0;
        render_all();

        engine_.submit_prompt(active_session_id_, text);
        return;
    }

    // --- Editing ---
    case KEY_BACKSPACE:
    case 127:
    case 8:
        if (input_cursor_ > 0) {
            input_buf_.erase(input_cursor_ - 1, 1);
            --input_cursor_;
        }
        render_all();
        return;

    case KEY_DC: // Delete
        if (input_cursor_ < (int)input_buf_.size()) {
            input_buf_.erase(input_cursor_, 1);
        }
        render_all();
        return;

    case KEY_LEFT:
        if (input_cursor_ > 0) --input_cursor_;
        render_all();
        return;

    case KEY_RIGHT:
        if (input_cursor_ < (int)input_buf_.size()) ++input_cursor_;
        render_all();
        return;

    case KEY_HOME:
        input_cursor_ = 0;
        render_all();
        return;

    case KEY_END:
        input_cursor_ = (int)input_buf_.size();
        render_all();
        return;

    default:
        // Printable character
        if (key >= 32 && key < 256) {
            input_buf_.insert(input_cursor_, 1, (char)key);
            ++input_cursor_;
            render_all();
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void TuiApp::render_all() {
    ::erase();
    ::refresh();

    // Draw vertical divider
    for (int r = 0; r < rows_ - kInputHeight - kStatusHeight; ++r) {
        mvaddch(r, kSessionPaneWidth, ACS_VLINE);
    }

    render_session_list();
    render_chat();
    render_input();
    render_statusbar();

    if (plan_visible_)  render_plan_overlay();
    if (perm_visible_) render_permission_overlay();

    // Position cursor in input window unless perm overlay is up
    if (!perm_visible_) {
        int input_col = 2 + input_cursor_ - input_scroll_;
        ::wmove(win_input_, 1, std::clamp(input_col, 2, cols_ - 2));
        ::wnoutrefresh(win_input_);
    }

    ::doupdate();
}

void TuiApp::render_session_list() {
    ::werase(win_sessions_);
    int h, w;
    getmaxyx(win_sessions_, h, w);
    (void)h;

    // Title
    ::wattron(win_sessions_, A_BOLD);
    mvwprintw(win_sessions_, 0, 0, "%-*s", w, " Sessions");
    ::wattroff(win_sessions_, A_BOLD);

    // [N] New Session shortcut
    mvwprintw(win_sessions_, 1, 0, "[^N] New Session");

    int row = 2;
    for (int i = 0; i < (int)sessions_.size() && row < (h - 1); ++i, ++row) {
        bool is_active = (sessions_[i].id == active_session_id_);
        bool is_sel    = (i == session_sel_);

        std::string title = sessions_[i].title.empty()
            ? sessions_[i].id.substr(0, 8)
            : sessions_[i].title;
        title = truncate(title, w - 2);

        std::string prefix = (is_sel ? "> " : "  ");
        std::string line   = prefix + title;

        if (is_active) {
            ::wattron(win_sessions_, COLOR_PAIR(CP_SESSION_ACT) | A_BOLD);
        } else if (is_sel) {
            ::wattron(win_sessions_, A_REVERSE);
        }
        mvwprintw(win_sessions_, row, 0, "%-*s", w, line.c_str());
        if (is_active) {
            ::wattroff(win_sessions_, COLOR_PAIR(CP_SESSION_ACT) | A_BOLD);
        } else if (is_sel) {
            ::wattroff(win_sessions_, A_REVERSE);
        }
    }
    ::wnoutrefresh(win_sessions_);
}

void TuiApp::render_chat() {
    ::werase(win_chat_);
    int h, w;
    getmaxyx(win_chat_, h, w);

    // Expand every ChatLine through word-wrap into display lines
    struct DisplayLine {
        LineType type;
        std::string text;
        bool streaming;
    };
    std::vector<DisplayLine> display;
    display.reserve(chat_lines_.size() * 2);

    for (auto& cl : chat_lines_) {
        if (cl.type == LineType::Separator) {
            display.push_back({ cl.type, "", false });
            continue;
        }
        auto wrapped = wrap(cl.text, w - 1);
        for (int k = 0; k < (int)wrapped.size(); ++k) {
            display.push_back({ cl.type, wrapped[k],
                                cl.streaming && k == (int)wrapped.size() - 1 });
        }
    }

    int total = (int)display.size();
    // chat_scroll_ lines from bottom; clamp
    chat_scroll_ = std::min(chat_scroll_, std::max(0, total - h));
    int start = std::max(0, total - h - chat_scroll_);
    int end   = std::min(total, start + h);

    for (int i = start; i < end; ++i) {
        int row = i - start;
        auto& dl = display[i];

        switch (dl.type) {
        case LineType::UserText:
            ::wattron(win_chat_, COLOR_PAIR(CP_USER) | A_BOLD);
            mvwprintw(win_chat_, row, 0, "%s", dl.text.c_str());
            ::wattroff(win_chat_, COLOR_PAIR(CP_USER) | A_BOLD);
            break;

        case LineType::AssistantText:
            ::wattron(win_chat_, COLOR_PAIR(CP_ASSISTANT));
            mvwprintw(win_chat_, row, 0, "%s", dl.text.c_str());
            if (dl.streaming) {
                ::waddch(win_chat_, ACS_BLOCK); // blinking cursor indicator
            }
            ::wattroff(win_chat_, COLOR_PAIR(CP_ASSISTANT));
            break;

        case LineType::ToolHeader:
            ::wattron(win_chat_, COLOR_PAIR(CP_TOOL_HEADER) | A_BOLD);
            mvwprintw(win_chat_, row, 0, "%s", dl.text.c_str());
            ::wattroff(win_chat_, COLOR_PAIR(CP_TOOL_HEADER) | A_BOLD);
            break;

        case LineType::ToolBody:
            ::wattron(win_chat_, COLOR_PAIR(CP_TOOL_BODY));
            mvwprintw(win_chat_, row, 0, "%s", dl.text.c_str());
            ::wattroff(win_chat_, COLOR_PAIR(CP_TOOL_BODY));
            break;

        case LineType::ToolResult:
            ::wattron(win_chat_, COLOR_PAIR(CP_TOOL_OK));
            mvwprintw(win_chat_, row, 0, "%s", dl.text.c_str());
            ::wattroff(win_chat_, COLOR_PAIR(CP_TOOL_OK));
            break;

        case LineType::System:
            ::wattron(win_chat_, COLOR_PAIR(CP_TOOL_ERR));
            mvwprintw(win_chat_, row, 0, "%s", dl.text.c_str());
            ::wattroff(win_chat_, COLOR_PAIR(CP_TOOL_ERR));
            break;

        case LineType::Separator:
            break;
        }
    }
    ::wnoutrefresh(win_chat_);
}

void TuiApp::render_input() {
    ::werase(win_input_);
    int h, w;
    getmaxyx(win_input_, h, w);
    (void)h;

    // Top border
    ::wmove(win_input_, 0, 0);
    ::whline(win_input_, ACS_HLINE, w);

    // Prompt prefix
    mvwaddstr(win_input_, 1, 0, "> ");

    // Adjust horizontal scroll so cursor stays visible
    int inner_w = w - 3; // width available for text after "> "
    if (input_cursor_ < input_scroll_) {
        input_scroll_ = input_cursor_;
    }
    if (input_cursor_ >= input_scroll_ + inner_w) {
        input_scroll_ = input_cursor_ - inner_w + 1;
    }

    std::string visible = input_buf_.size() > (size_t)input_scroll_
        ? input_buf_.substr(input_scroll_, inner_w)
        : "";
    mvwaddstr(win_input_, 1, 2, visible.c_str());

    // Running indicator on the right of the input line
    if (engine_running_) {
        std::string indicator;
        if (thinking_)        indicator = "[* thinking]";
        else if (streaming_)  indicator = "[~ streaming]";
        else                  indicator = "[* running]";
        mvwaddstr(win_input_, 1, w - (int)indicator.size() - 1, indicator.c_str());
    }

    ::wnoutrefresh(win_input_);
}

void TuiApp::render_statusbar() {
    ::werase(win_status_);
    int h, w;
    getmaxyx(win_status_, h, w);
    (void)h;

    ::wattron(win_status_, COLOR_PAIR(CP_STATUS) | A_REVERSE);

    // Mode badge
    std::string badge = "[BUILD]";
    if (!active_session_id_.empty()
        && engine_.get_mode(active_session_id_) == SessionMode::Plan)
        badge = "[PLAN]";

    std::string model  = config_.model.empty() ? "?" : config_.model;
    std::string agent  = config_.agent.empty() ? "default" : config_.agent;

    // Format context as N.Nk / max
    auto fmt = [](int n) {
        char b[16];
        if (n >= 10000) std::snprintf(b, sizeof(b), "%.1fk", n / 1000.0);
        else            std::snprintf(b, sizeof(b), "%d", n);
        return std::string(b);
    };

    std::string ctx_str;
    if (current_context_tokens_ > 0) {
        ctx_str = " ctx: " + fmt(current_context_tokens_);
        if (max_context_ > 0) {
            int pct = std::min(100, current_context_tokens_ * 100 / max_context_);
            char pb[8];
            std::snprintf(pb, sizeof(pb), "%d%%", pct);
            ctx_str += "/" + fmt(max_context_) + " (" + pb + ")";
        }
    }

    char cost_buf[24] = "";
    if (session_cost_ > 0.0)
        std::snprintf(cost_buf, sizeof(cost_buf), "  $%.4f", session_cost_);

    char buf[384];
    std::snprintf(buf, sizeof(buf),
                  " %s model: %-20s%s last: %d/%d  total: %d%s",
                  badge.c_str(),
                  model.c_str(),
                  ctx_str.c_str(),
                  last_prompt_input_, last_prompt_output_,
                  total_tokens_,
                  cost_buf);
    std::string status(buf);
    // Pad to full width
    if ((int)status.size() < w) status.resize(w, ' ');
    mvwaddstr(win_status_, 0, 0, status.substr(0, w).c_str());

    ::wattroff(win_status_, COLOR_PAIR(CP_STATUS) | A_REVERSE);
    ::wnoutrefresh(win_status_);
}

void TuiApp::render_permission_overlay() {
    // Centered box
    int box_w = std::min(cols_ - 4, 64);
    int box_h = 10;
    int box_y = (rows_ - box_h) / 2;
    int box_x = (cols_ - box_w) / 2;

    WINDOW* win = ::newwin(box_h, box_w, box_y, box_x);
    ::box(win, 0, 0);
    ::wattron(win, A_BOLD);
    mvwprintw(win, 0, 2, " Permission Request ");
    ::wattroff(win, A_BOLD);

    int inner_w = box_w - 4;

    // Action + resource
    std::string action_line = "Action:   " + perm_pending_.action;
    std::string res_line    = "Resource: " + perm_pending_.resource;
    mvwprintw(win, 2, 2, "%s", truncate(action_line, inner_w).c_str());
    mvwprintw(win, 3, 2, "%s", truncate(res_line,    inner_w).c_str());

    // Detail (first line only)
    if (!perm_pending_.detail.empty()) {
        std::string detail = perm_pending_.detail;
        auto nl = detail.find('\n');
        if (nl != std::string::npos) detail = detail.substr(0, nl);
        mvwprintw(win, 4, 2, "%s", truncate("Detail:   " + detail, inner_w).c_str());
    }

    // Divider
    ::wmove(win, 6, 1);
    ::whline(win, ACS_HLINE, box_w - 2);

    // Hint line
    mvwprintw(win, 7, 2, "h/l or arrows to choose, Enter confirm, Esc deny");

    // Buttons: [Allow Once]  [Allow Always]  [Deny]
    const char* labels[] = { " Allow Once ", " Allow Always ", " Deny " };
    int col = 2;
    for (int i = 0; i < 3; ++i) {
        bool sel = (i == perm_sel_);
        if (sel) ::wattron(win, COLOR_PAIR(CP_PERM_SEL) | A_REVERSE | A_BOLD);
        mvwprintw(win, 8, col, "%s", labels[i]);
        if (sel) ::wattroff(win, COLOR_PAIR(CP_PERM_SEL) | A_REVERSE | A_BOLD);
        col += (int)strlen(labels[i]) + 2;
    }

    ::wnoutrefresh(win);
    ::delwin(win);
}

// ---------------------------------------------------------------------------
// Mode toggle + Plan overlay
// ---------------------------------------------------------------------------

void TuiApp::toggle_mode() {
    if (active_session_id_.empty()) return;
    auto cur = engine_.get_mode(active_session_id_);
    engine_.set_mode(active_session_id_,
        cur == SessionMode::Plan ? SessionMode::Build : SessionMode::Plan);
}

void TuiApp::refresh_mode() {
    // Nothing to do for the TUI — render_statusbar reads mode live each frame.
}

void TuiApp::render_plan_overlay() {
    int box_w = std::min(cols_ - 4, std::max(60, cols_ - 8));
    int box_h = std::min(rows_ - 4, std::max(15, rows_ - 4));
    int box_y = (rows_ - box_h) / 2;
    int box_x = (cols_ - box_w) / 2;

    WINDOW* win = ::newwin(box_h, box_w, box_y, box_x);
    ::box(win, 0, 0);
    ::wattron(win, A_BOLD);
    mvwprintw(win, 0, 2, " Proposed Plan ");
    ::wattroff(win, A_BOLD);

    if (!plan_path_.empty()) {
        std::string p = "saved: " + plan_path_;
        mvwprintw(win, 0, 18, "%s", truncate(p, box_w - 19).c_str());
    }

    int inner_w = box_w - 4;
    int inner_h = box_h - 4;

    // Word-wrap the plan into display rows
    auto lines = wrap(plan_path_.empty() ? plan_text_ : plan_text_, inner_w);

    int total = (int)lines.size();
    plan_scroll_ = std::min(plan_scroll_, std::max(0, total - inner_h));
    int start = std::max(0, total - inner_h - plan_scroll_);
    int end   = std::min(total, start + inner_h);

    for (int i = start; i < end; ++i) {
        int row = (i - start) + 1;
        if (row >= inner_h + 1) break;
        mvwprintw(win, row, 2, "%s", truncate(lines[i], inner_w).c_str());
    }

    // Footer / hint
    ::wmove(win, box_h - 2, 1);
    ::whline(win, ACS_HLINE, box_w - 2);
    ::wattron(win, A_BOLD);
    mvwprintw(win, box_h - 1, 2, " [a] approve   [d] discard   ↑/↓ scroll ");
    ::wattroff(win, A_BOLD);

    ::wnoutrefresh(win);
    ::delwin(win);
}

// ---------------------------------------------------------------------------
// Main run loop
// ---------------------------------------------------------------------------

void TuiApp::run() {
    init_ncurses();
    subscribe_events();

    // Load existing sessions
    sessions_ = store_.list();

    if (sessions_.empty()) {
        new_session();
    } else {
        select_session(0);
    }

    render_all();

    // Main event loop
    while (true) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO,   &rfds);
        FD_SET(wake_pipe_[0],  &rfds);
        int maxfd = std::max(STDIN_FILENO, wake_pipe_[0]) + 1;

        // 200ms timeout so we can handle SIGWINCH resize
        struct timeval tv { 0, 200000 };
        int ret = ::select(maxfd, &rfds, nullptr, nullptr, &tv);

        if (ret < 0) {
            if (errno == EINTR) {
                // Could be SIGWINCH
                layout();
                render_all();
                continue;
            }
            break;
        }

        if (FD_ISSET(wake_pipe_[0], &rfds)) {
            process_engine_events();
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            // Use wtimeout + wgetch on the input window to read key
            ::wtimeout(win_input_, 0);
            int key = ::wgetch(win_input_);
            if (key != ERR) {
                // Ctrl+Q — quit
                if (key == 17) break;
                handle_key(key);
            }
        }
    }

    teardown_ncurses();
}

} // namespace tui
