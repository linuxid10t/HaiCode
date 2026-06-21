#pragma once
#include <haicode/haicode.h>
#include <ncurses.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <functional>
#include <map>
#include <future>

namespace tui {

// ---- Chat line types ----

enum class LineType {
    UserText,
    AssistantText,
    ToolHeader,
    ToolBody,
    ToolResult,
    System,
    Separator,
    ThinkingHeader,
    ThinkingText
};

struct ChatLine {
    LineType type;
    std::string text;
    bool streaming = false;   // currently receiving text
};

// ---- Permission request ----

struct PendingPermission {
    std::string call_id;
    std::string action;
    std::string resource;
    std::string detail;        // JSON or command text to show
    std::promise<haicode::PermissionEffect>* promise = nullptr;
};

// ---- Event queued from engine thread ----

enum class EngineEventKind {
    TextDelta,
    ReasoningDelta,
    StepStarted,
    StepEnded,
    StepFailed,
    ToolCalled,
    ToolResult,
    SessionsChanged,
    PermissionReq,
    PlanProposed,
    TodoUpdated,
    BuildHookResult,
    Compaction,
    Interrupted
};

struct EngineEvent {
    EngineEventKind kind;
    std::string session_id;
    std::string str1;   // text delta, tool name, error, etc.
    std::string str2;   // tool output or call_id (or path for PlanProposed)
    std::string str3;   // plan markdown for PlanProposed
    bool bool1 = false; // success flag for tool results
    int int1   = 0;     // input tokens for StepEnded
    int int2   = 0;     // output tokens for StepEnded
    double dbl1 = 0.0;  // step cost (USD) for StepEnded
    std::vector<haicode::Todo> todos;  // for TodoUpdated
    PendingPermission* perm = nullptr;
};

// ---- Color pair constants ----

static constexpr int CP_USER        = 1;  // cyan
static constexpr int CP_ASSISTANT   = 2;  // green
static constexpr int CP_TOOL_HEADER = 3;  // yellow bold
static constexpr int CP_TOOL_BODY   = 4;  // white
static constexpr int CP_TOOL_OK     = 5;  // green
static constexpr int CP_TOOL_ERR    = 6;  // red
static constexpr int CP_STATUS      = 7;  // reverse
static constexpr int CP_SESSION_ACT = 8;  // cyan bold
static constexpr int CP_PERM_SEL    = 9;  // magenta reverse

// ---- Main TUI application ----

class TuiApp {
public:
    TuiApp(haicode::SessionEngine& engine,
           haicode::SessionStore& store,
           haicode::SessionEventBus& bus,
           haicode::ConfigLoader& config_loader,
           const haicode::AppConfig& config,
           const std::string& project_dir);
    ~TuiApp();

    void run();

    // Public shim so main.cpp can post events from the permission callback
    // (called from engine background threads before run() returns)
    void push_engine_event(EngineEvent ev);

private:
    // Setup
    void init_ncurses();
    void teardown_ncurses();
    void layout();
    void subscribe_events();

    // Event loop
    void process_engine_events();
    void handle_key(int key);

    // Session management
    void new_session();
    void select_session(int idx);
    void load_history(const std::string& session_id);

    // Rendering
    void render_all();
    void render_session_list();
    void render_chat();
    void render_input();
    void render_statusbar();
    void render_permission_overlay();
    void render_plan_overlay();
    void render_confirm_build_overlay();
    void render_todos_overlay();
    void render_thinking_indicator();

    // Mode toggle
    void toggle_mode();
    void refresh_mode();

    // Chat helpers
    void append_line(const ChatLine& line);
    void append_text_delta(const std::string& delta);
    void append_reasoning_delta(const std::string& delta);
    void end_streaming();
    void end_reasoning_streaming();
    void append_tool_called(const std::string& tool_name, const std::string& input_json);
    void append_tool_result(const std::string& call_id, const std::string& output, bool success);

    // Word wrap
    std::vector<std::string> wrap(const std::string& text, int width) const;

    // --- Windows ---
    WINDOW* win_sessions_ = nullptr;
    WINDOW* win_chat_     = nullptr;
    WINDOW* win_input_    = nullptr;
    WINDOW* win_status_   = nullptr;

    int rows_ = 0, cols_ = 0;
    static constexpr int kSessionPaneWidth = 22;
    static constexpr int kInputHeight      = 3;
    static constexpr int kStatusHeight     = 1;

    // --- Engine integration ---
    haicode::SessionEngine&   engine_;
    haicode::SessionStore&    store_;
    haicode::SessionEventBus& bus_;
    haicode::AppConfig        config_;
    std::string               project_dir_;

    // pipe for cross-thread wakeup
    int wake_pipe_[2] = {-1, -1};

    // event queue (written from engine threads, read on main thread)
    std::mutex            ev_mutex_;
    std::deque<EngineEvent> ev_queue_;

    // --- Session state ---
    std::vector<haicode::SessionInfo> sessions_;
    int         session_sel_ = 0;           // highlighted index in session list
    std::string active_session_id_;

    // --- Chat state ---
    std::vector<ChatLine> chat_lines_;
    int  chat_scroll_ = 0;                  // lines from bottom (0 = pinned to bottom)
    bool streaming_   = false;
    bool reasoning_streaming_ = false;

    // --- Input state ---
    std::string input_buf_;
    int input_cursor_ = 0;
    int input_scroll_ = 0;                  // horizontal scroll offset

    // --- Status ---
    bool engine_running_ = false;
    bool thinking_       = false;  // true between submit and first text delta
    bool compacting_     = false;  // true while summarizing the context head
    int  total_tokens_   = 0;
    int  last_prompt_input_  = 0;
    int  last_prompt_output_ = 0;
    int  session_input_total_   = 0;
    int  session_output_total_  = 0;
    int  current_context_tokens_ = 0;
    int  max_context_            = 0;
    double session_cost_         = 0.0;

    // --- Tool block collapse state ---
    bool tools_expanded_ = false;   // x key toggles; collapsed by default
    bool thinking_expanded_ = false;  // mirrors tools_expanded_; collapsed by default

    // --- TodoWrite state ---
    std::vector<haicode::Todo> current_todos_;
    bool   todos_visible_ = false;
    int    todos_scroll_  = 0;

    // --- Permission overlay ---
    bool              perm_visible_ = false;
    PendingPermission perm_pending_;
    int               perm_sel_ = 0;        // 0=once, 1=always, 2=deny

    // --- Plan-review overlay ---
    bool         plan_visible_ = false;
    std::string  plan_text_;
    std::string  plan_path_;
    std::string  plan_session_id_;
    int          plan_scroll_ = 0;
    bool         confirm_build_visible_ = false;

    // Focus: 0=sessions pane, 1=chat+input pane
    int focus_ = 1;
};

} // namespace tui
