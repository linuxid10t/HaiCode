# CLAUDE.md

This file provides guidance to AI coding assistants (Claude Code, HaiCode itself, or similar tools) when working with code in this repository.

## Build

```bash
# Configure (only needed once, or when adding new source files)
cd /boot/home/haicode
cmake -B build -S .

# Build everything
make -C build -j4

# Build individual targets
make -C build haicode-tui
make -C build haicode-gui
make -C build test_db
```

**Adding new `.cpp` files:** CMake uses `GLOB_RECURSE` to collect sources at configure time. After adding a new file, re-run `cmake -B build -S .` before `make`.

## Run

```bash
./build/tui/haicode-tui [/path/to/project]   # TUI (ncurses)
./build/gui/haicode-gui [/path/to/project]   # GUI (BeAPI)
./build/lib/test_db                           # Database smoke test
```

## Test

The only automated test is `test_db`, a Phase 1 smoke test that creates a session, appends events, and reloads them. It uses `/tmp/test_haicode_fresh.db` and deletes it before each run.

## Architecture

The project has three layers:

### `lib/` — libhaicode (core, no GUI dependency)

Pure C++20 + POSIX. Key types live in `lib/include/haicode/`:

| Header | What it defines |
|--------|----------------|
| `engine.h` | `SessionEngine` (agentic loop), `ContextBuilder` |
| `provider.h` | `Provider` ABC, `LLMRequest`, `StreamCallbacks`, `ProviderRegistry` |
| `tool.h` | `Tool` ABC, `ToolContext`, `ToolResult`, `ToolRegistry`, `PermissionGate` |
| `events.h` | Event structs, `SessionEventBus` |
| `db.h` | `Database` (SQLite RAII), `SessionStore`, `SessionInfo`, `SessionMessage` |
| `config.h` | `AppConfig`, `ConfigLoader` (merges global + project JSON) |
| `util.h` | `HttpClient` (libcurl SSE + GET), `make_id()`, `now_ms()` |

**Agentic loop** (`lib/src/engine/engine.cpp`): `SessionEngine` spawns one `std::thread` per session. The thread runs up to 20 steps: `ContextBuilder::build()` → `Provider::stream()` → collect text/tool-calls via `StreamCallbacks` → execute tools via `ToolRegistry` → repeat if `FinishReason::ToolUse`. An `std::atomic<bool>` per session allows interruption between steps. If any tool returns `result.denied == true`, the loop publishes `StepFailed` and breaks immediately. `session_running_` (a `std::map<string,bool>` under mutex) tracks whether a session thread is active — do not use `std::thread::joinable()` for this, as it returns true even after the thread finishes.

**Providers** (`lib/src/provider/`): `AnthropicProvider` and `OpenAIProvider` each implement `stream()` (SSE) and `list_models()` (HTTP GET). OpenAI's message format differs from Anthropic's; `translate_messages()` in `openai.cpp` converts between them, including converting Anthropic content arrays with `tool_use` blocks into OpenAI `tool_calls`. Register providers via `ProviderRegistry::register_provider()`.

**Permission gate** (`lib/src/permission/permission.cpp`): `PermissionGate::check()` tests action+resource against fnmatch rules (session rules take priority over config rules). On "Ask", it blocks the engine thread on `std::future<PermissionEffect>` until the UI resolves a `std::promise`. `ToolRegistry::execute()` sets `result.denied = true` when the gate returns `Deny`.

**Config** (`lib/src/config/config.cpp`): Global config at `B_USER_SETTINGS_DIRECTORY/haicode/config.json`; project config at `<project_dir>/.haicode/config.json`. Project values overlay globals. The global config also stores `last_directory` (the most recently used project directory).

**Tools** (`lib/src/tool/tools.cpp` + `lib/src/tool/web_tools.cpp`): Eleven built-in tools — see Tool Details below.

### `tui/` — ncurses frontend

Pure POSIX + Haiku kernel (no `BApplication`). `TuiApp::run()` multiplexes `STDIN_FILENO` and a wake pipe via `select()`. Engine threads post `EngineEvent` objects into a mutex-protected queue and write one byte to the pipe to wake the main loop. Permission requests travel through this same queue carrying a raw `PendingPermission*` (heap-allocated, with a `std::promise*`); the main loop renders the overlay and resolves the promise.

### `gui/` — BeAPI (Haiku native) frontend

- `HaiCodeApp` (BApplication) owns all haicode objects as `unique_ptr`. On startup, reads `last_directory` from settings JSON if no command-line arg was given.
- `MainWindow` (BWindow) handles all `MSG_*` messages in the BeAPI looper thread. Holds `engine_` as a pointer (`haicode::SessionEngine*`) not a reference, updated via `SetEngine()` when settings change.
- `GuiEventRelay` subscribes to `SessionEventBus` from engine threads and forwards events as `BMessage` via `BMessenger::SendMessage()` (thread-safe).
- Permission requests: engine thread allocates a heap `std::promise`, packs the raw pointer into a `BMessage` via `AddPointer`, posts to `MainWindow`. `PermissionWindow` sends `MSG_PERMISSION_REP` with action, resource, and effect. `MainWindow` fulfills the promise and `delete`s it. If effect=1 (Allow Always), posts `MSG_ADD_PERMISSION` to `be_app` which calls `perm_gate_->add_allow()`.
- Model list is fetched asynchronously: `MainWindow` posts `MSG_FETCH_MODELS` to `be_app`; `HaiCodeApp` spawns a detached thread capturing a `shared_ptr<Provider>`, calls `list_models()`, posts `MSG_MODELS_LOADED` back to `MainWindow`. Fetched automatically on startup for the first configured provider.
- Session list uses `suppress_next_select_` flag to prevent `BListView::Select()` from triggering a recursive `MSG_SELECT_SESSION` → `_SelectSession()` → `Clear()` cascade. Always call `_SwitchToSession()` (not `_SelectSession()`) when programmatically switching sessions.
- Selecting a session restores its `project_dir_`, directory button label, provider dropdown, and model dropdown from the DB.
- `QuitRequested()` calls `std::exit(0)` — engine threads may be blocked in libcurl and cannot be joined cleanly.

## Key constraints

- **Never block the BeAPI looper thread** — all network/engine calls go through `be_app` or a detached thread posting back via `BMessenger`.
- **Never block the TUI main loop** — engine events arrive via the wake pipe; the loop must remain responsive to `getch`.
- **`B_SIZE_UNLIMITED` is only valid as a maximum size**, not a minimum. Use `B_SIZE_UNSET` for unconstrained minimum dimensions.
- **BeAPI `BScrollView` constructor**: use the 6-arg layout-aware form (no `resizingMode` parameter) when building layout-managed views. The 7-arg old-style form breaks layout and grays out child views.
- **SQLite schema**: `session`, `session_message` (types: `user_prompted`, `assistant_text`, `tool_called`, `tool_result`), `permission`. WAL mode + FK constraints (`ON DELETE CASCADE`) enabled.
- **Tool context messages**: assistant messages that include tool calls must be stored and reassembled with Anthropic `tool_use` content blocks (not plain text). Missing these causes orphaned `tool_result` blocks in the next request, making the model repeat work it already completed.
- **`ToolResult` struct field order**: `{bool success, string output, string error, bool denied}`. The `denied` field is last — do not insert fields before it or existing brace-initializers break.
- Tool output is truncated to 100 KB (`MAX_OUTPUT` in `tools.cpp`).
- Session IDs are descending-timestamp sortable (newest first in `store_.list()`). Display human-readable dates by parsing `INT64_MAX - desc_hex` from the ID.

## Tool Details (`lib/src/tool/tools.cpp`, `lib/src/tool/web_tools.cpp`)

All tools share a `MAX_OUTPUT = 100 KB` cap and a `sq()` helper for safe single-quoted shell arguments. Web tools (`web_search`, `web_extract`) live in `web_tools.cpp` and are pulled into `register_builtin_tools()` via `register_web_tools()`.

### BashTool (`bash`)
- Wraps user command in `timeout N sh -c 'cd <working_dir> && { command; }'  2>&1`
- Working dir is single-quoted via `sq()` — safe against spaces, `$`, backticks, quotes
- Timeout enforced via the `timeout` binary (default 30s); exit code 124 = timed out
- Exit code extracted with `WEXITSTATUS()` from `pclose()` status word
- Output read via `read_pipe()` which stops early at 100 KB

### ReadTool (`read`)
- Resolves relative paths against `ctx.working_dir` (trailing slashes stripped before join)
- Rejects empty path; detects binary files (null byte scan of first 8 KB) and refuses them
- `offset` is 1-based (offset=1 starts at line 1, the default)
- Enforces 100 KB cap mid-read
- Error messages include `strerror(errno)`

### WriteTool (`write`)
- Resolves relative paths against `ctx.working_dir`
- Creates missing parent directories by walking path components with `mkdir()`
- Atomic write: writes to `<path>.tmp_write` then `rename()` to final destination
- Uses `ios::binary` + `f.write()` to preserve exact byte content
- Checks both write errors and close/flush errors; removes temp file on any failure

### GlobTool (`glob`)
- Absolute patterns (starting with `/`) bypass base directory prepend
- `GLOB_MARK` removed — was adding unwanted trailing slashes to directory matches
- `globfree()` called unconditionally after `glob()` (always safe per POSIX)
- Returns error on `rc != 0 && rc != GLOB_NOMATCH`; GLOB_NOMATCH returns "(no matches)"
- Note: `**` recursive matching is not supported by POSIX `glob()`

### GrepTool (`grep`)
- Pattern, path, and include are all passed through `sq()` — no shell injection possible
- Uses `/bin/grep` absolute path
- Relative search path resolved against `ctx.working_dir`
- Exit codes: 0 = matches found, 1 = no matches (success), 2 = grep error (returns failure)
- Output capped at 100 KB in-process (no `head` pipe)
- Schema key is `"line_numbers"` (boolean, default true), not `"-n"`

### EditTool (`edit`)
- Reads the whole file into memory, refuses binary files (null byte scan)
- Counts `old_string` occurrences; requires `replace_all=true` if it appears more than once
- Empty `old_string` is rejected with a hint to use both fields for deletion
- Replacement is in-memory (single-pass for `replace_all`, single `find` otherwise)
- Writes via shared `atomic_write()` helper (`.tmp_write` → `rename`) — same as WriteTool
- Required permission: `write`

### LsTool (`ls`)
- Resolves relative `path` against `ctx.working_dir`; defaults to the project dir if omitted
- Uses `opendir()`/`readdir()`; skips `.` and `..`
- Adds suffixes via `stat()`: `/` for directories, `@` for symlinks
- Output sorted with `std::sort` and capped at 100 KB
- Required permission: `read`

### ExternalTerminalTool (`external_terminal`)
- Double-forks: first child calls `setsid()` and detaches stdio, grandchild `execl`s `/boot/system/apps/Terminal`
- Invokes Terminal as `Terminal -w <working_dir> /bin/sh -c '<command>'` — the window closes when the command exits
- Returns immediately; output is **not** captured (designed for interactive TUIs like vim, REPLs)
- Distinct permission action (`external_terminal`) so prompts make clear which tool is asking
- Required permission: `external_terminal`

### ProposePlanTool (`propose_plan`)
- Only available in Plan mode — engine filters it out of `tool_defs` in Build mode
- Writes the plan markdown to `<project_dir>/.haicode/plans/plan_YYYYMMDD_HHMMSS_<4-hex-rand>.md`
- Parent directory created with `mkdir()` walking (same pattern as WriteTool)
- Returns the on-disk path so the engine can surface it via `PlanProposed` and end the turn
- Required permission: `propose_plan`

### WebSearchTool (`web_search`) — `web_tools.cpp`
- Backends: `mojeek` (default, no API key), `ddg_lite`, `ddg_html`. Configurable via `AppConfig::web_search_engine`
- Returns ranked results: title, URL, snippet — read snippets before calling `web_extract`
- `max_results` defaults to 5, capped at 10
- HTML parsing is in-process (no external deps); DDG may surface CAPTCHA errors
- Required permission: `web_search`

### WebExtractTool (`web_extract`) — `web_tools.cpp`
- Fetches one URL via `HttpClient` (libcurl) with a browser User-Agent
- Returns cleaned main-body text — nav, ads, scripts stripped (HTML stripper is ~400 lines, untested)
- `max_chars` defaults to 8000; rejects non-http(s) URLs
- Required permission: `web_extract`
