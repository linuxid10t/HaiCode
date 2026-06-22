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
make -C build test_diff
make -C build test_git_find
make -C build test_process
make -C build test_file_tools
make -C build test_config_permission
```

**Adding new `.cpp` files:** CMake uses `GLOB_RECURSE` to collect sources at configure time. After adding a new file, re-run `cmake -B build -S .` before `make`.

**Hybrid (x86_gcc2) systems:** HaiCode requires a C++20 compiler, so on a hybrid image it must be built under the secondary-arch toolchain (GCC 13), not the default gcc2. The root `CMakeLists.txt` enforces this: a `cxx_std_20` feature check aborts configure with an actionable message if the compiler can't do C++20, and a secondary-arch detection block reads the compiler's target macros (`__x86_64__` → `x86_64`, `__i386__` → `x86`) to prepend `/lib/<arch>` to the `find_library` HINTS (`HAIKU_DEVELOP_LIB_DIRS` / `HAIKU_LIB_DIRS`). On a pure single-arch system the `<arch>` subdirs don't exist and `find_library` falls through to the flat path. When adding a new `find_library`, use `HINTS ${HAIKU_DEVELOP_LIB_DIRS} ${HAIKU_LIB_DIRS}` rather than hardcoding the flat paths. Headers are single-location on hybrid builds, so `include_directories` is unaffected.

## Run

```bash
./build/tui/haicode-tui [/path/to/project]   # TUI (ncurses)
./build/gui/haicode-gui [/path/to/project]   # GUI (BeAPI)
./build/lib/test_db                           # Database smoke test
./build/lib/test_diff                         # DiffTool unit tests
./build/lib/test_git_find                     # GitTool + FindTool unit tests
./build/lib/test_process                      # ProcessTool unit tests
./build/lib/test_file_tools                   # ReadTool / WriteTool / EditTool unit tests
./build/lib/test_config_permission            # ConfigLoader + PermissionGate unit tests
./build/lib/test_compaction                   # auto-compaction threshold + DB round-trip
./build/lib/test_model_context                # context-window parsing for vLLM/OpenRouter/LM Studio/Ollama/llama.cpp
```

## Test

| Binary | What it tests |
|--------|--------------|
| `test_db` | Phase 1 smoke: session create/list, message append/reload, get-by-ID |
| `test_diff` | DiffTool: identical content, additions, deletions, modifications, error cases |
| `test_git_find` | GitTool: subcommand allowlist, status/log/branch/ls-files. FindTool: name/type/maxdepth filters, relative paths |
| `test_process` | ProcessTool: list/filter, kill (via forked subprocess), check_port, error cases |
| `test_file_tools` | ReadTool: offset/limit, binary rejection, relative paths, empty file. WriteTool: atomic write, parent dir creation, binary content, empty content. EditTool: replace_all, duplicate detection, delete, binary rejection, whitespace matching |
| `test_config_permission` | ConfigLoader::load_file: all fields, permissions (allow/deny/ask/default resource), build_command, web_search, instructions, providers, model contexts, missing/invalid files. ConfigLoader::merge: scalar overlay, empty overlay preservation, appended collections. PermissionGate: allow/deny/ask rules, wildcards, fnmatch patterns, last-rule-wins, session priority, add_allow, ask callback. ToolRegistry gate integration |
| `test_model_context` | Context-window parsing for flavored OpenAI-compatible servers: vLLM `max_model_len`, OpenRouter `context_length`, LM Studio native `/api/v1/models` (`max_context_length` + loaded `config.context_length` runtime window), Ollama `/api/show` `llama.context_length`, llama.cpp `/props` `n_ctx`. Malformed/missing inputs, stringified numbers, first-loaded fallback |
| `test_compaction` | Auto-compaction: threshold decision arithmetic (trigger at `effective*threshold`, disabled when `window==0`), `get_context_window` unknown vs override, `SessionStore::compact_messages` DB round-trip (head replaced by one `compaction_summary` at seq 0, tail seqs untouched, no-op when `keep_from_seq<=1`) |

All tests use `/tmp` for scratch files and clean up after themselves.

To add a test for a new tool, create `lib/tests/test_<name>.cpp` and add two lines to `lib/CMakeLists.txt`:
```cmake
add_executable(test_<name> tests/test_<name>.cpp)
target_link_libraries(test_<name> haicode)
```
Then re-run `cmake -B build -S .` and add the binary to the table above. All transitive deps (sqlite3, curl, be, root) are inherited from the `haicode` target — no manual `-l` flags needed.

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
| `pricing.h` | `ModelPricing`, `TokenUsage`, `lookup_pricing`, `compute_cost` |
| `model_info.h` | `get_context_window()` (config override → prefix table → provider discovery), `model_context_parse.h` (flavored-server context parsers: `parse_model_context_inline`, `parse_lmstudio_native_models`, `parse_ollama_show`, `parse_llamacpp_props`, `ServerFlavor`) |
| `types.h` | Shared types and constants |

**Agentic loop** (`lib/src/engine/engine.cpp`): `SessionEngine` spawns one `std::thread` per session. The thread runs up to 50 steps (default; configurable via `max_steps` in agent config): `ContextBuilder::build()` → `Provider::stream()` → collect text/tool-calls via `StreamCallbacks` → execute tools via `ToolRegistry` → repeat if `FinishReason::ToolUse`. An `std::atomic<bool>` per session allows interruption between steps. If any tool returns `result.denied == true`, the loop publishes `StepFailed` and breaks immediately. `session_running_` (a `std::map<string,bool>` under mutex) tracks whether a session thread is active — do not use `std::thread::joinable()` for this, as it returns true even after the thread finishes. The dynamic system block (separate from the stable cached body) escalates in tone as the step budget depletes: `render_dynamic_prompt()` appends a "getting tight" line at 5–14 steps left and a "CRITICAL" line at 1–4 steps left, so the model reprioritizes before exhaustion.

**Auto-compaction** (`SessionEngine::compact_history`): at the top of each loop step, if `config_.auto_compact` is on and the previous step's total input tokens (`usage.input + cache_read + cache_write`) reach `threshold = (window - reserve) * auto_compact_threshold`, the conversation head is summarized via a non-streaming `provider->stream()` call and persisted through `SessionStore::compact_messages` (delete head rows `seq < tail_start_seq`, insert the summary at seq 0 so it precedes the intact tail). Compaction is disabled when the context window is unknown (`get_context_window == 0`) — set `"models": {"<model_id>": <tokens>}` in config to enable it for unrecognized models. The two `CompactionStarted`/`CompactionEnded` events drive a "compacting context…" indicator in both frontends.

The tail boundary is picked by `choose_tail_start_seq()` (declared in `engine.h`): in the normal multi-turn case it returns the seq of the most recent `user_prompted`. In the **single-turn fallback** — a long agentic loop with one prompt and many tool calls — it returns the seq of the K-th most recent `assistant_text`-with-tool_calls (K=4), keeping the last four round-trips intact and summarizing everything before. Returns `-1` (no compaction) when there are fewer than K round-trips and no prior user turn to anchor the tail.

The summary row is persisted with `role=assistant` and emitted by `assemble_messages` as an assistant turn with a `"Here is a summary of the prior conversation:"` lead-in. This guarantees legal user/assistant alternation when the tail starts with `user_prompted` (which emits as `user`). If the single-turn fallback puts the summary directly before an `assistant_text` turn, `assemble_messages` inserts a stub `user` message (`"Continue from where the summary leaves off."`) between them to preserve alternation — Anthropic rejects consecutive same-role messages.

`select_summary_prompt()` picks the summarization instruction: the first-pass prompt for fresh compactions, the **resegment prompt** when the head already begins with a prior `compaction_summary`. The resegment prompt instructs the model to preserve the prior summary verbatim under `## Segment 1` and add `## Segment 2` for the new content, preventing fidelity loss across repeated compactions.

**Hysteresis**: `should_compact_with_hysteresis()` gates the trigger. After a successful compaction, a per-session `last_compaction_step_` (under `mu_`) is recorded; the next compaction is blocked for at least 2 steps even if tokens stay above threshold. This prevents runaway re-compaction in the degenerate case where the post-compaction tail is itself above threshold. `submit_prompt` resets the counter to `-1` so every fresh user turn re-arms the trigger.

`compact_now()` (used by the GUI's **Compact** button) reuses the same locking discipline as `submit_prompt`: under `mu_` it marks `session_running_[id]=true`, joins any finished runner thread, and stores its worker in `runner_threads_`. The worker clears the flag on exit. This closes a race where a concurrent `submit_prompt` could start the agentic loop on top of an in-progress manual compaction, leading to concurrent SQLite writes on the same session.

**Session autonaming** (`config_.autoname_sessions`, default on): new sessions get a title without user action, in two stages. (1) In `submit_prompt`, after appending the first `user_prompted` message, if the title is still empty `derive_heuristic_title()` produces a short title (first non-empty line, whitespace collapsed, ~60-char word-boundary truncation) and calls `SessionStore::update_title`. (2) At the end of `agentic_loop`, when the user-turn count (`user_prompted` messages) satisfies `count % 5 == 1` — i.e. turns 1, 6, 11, 16, … — and `autoname_llm_refine` is on, `refine_title_llm()` makes a one-shot `provider->stream()` call (same model, `max_tokens` ~48, no tools). On the first call it generates a fresh ≤6-word title; on later calls it reconsiders — shown the current title and the full prompt history, the model either echoes the title verbatim (no write, no event) or returns a revised one. Both stages publish a `SessionRenamed` event, which the GUI relay forwards as `MSG_SESSION_RENAMED` (not gated on active session, so the sidebar refreshes regardless) and the TUI handles by reloading `sessions_` from the store. The master toggle disables both; the refine flag disables only the LLM call.

**Providers** (`lib/src/provider/`): `AnthropicProvider` and `OpenAIProvider` each implement `stream()` (SSE) and `list_models()` (HTTP GET, reports HTTP status via an `error` out-param). `base_url` is the **complete API root** (scheme + host + path prefix + version segment); the code appends only the resource path (`/messages`, `/chat/completions`, `/models`), so the version must be part of `base_url`. Defaults: `https://api.anthropic.com/v1`, `https://api.openai.com/v1`. OpenAI's message format differs from Anthropic's; `translate_messages()` in `openai.cpp` converts between them, including converting Anthropic content arrays with `tool_use` blocks into OpenAI `tool_calls`. Register providers via `ProviderRegistry::register_provider()`.

The app supports **any number** of providers. Each `ProviderConfig` in `AppConfig::providers` (a `map<id, ProviderConfig>`) has a `type` of `"anthropic"`, `"openai"`, or one of the flavored OpenAI-compatible servers: `"ollama"`, `"vllm"`, `"openrouter"`, `"lmstudio"`, `"llamacpp"`; when empty it is inferred from the id (`"anthropic"` → anthropic, anything else → openai). Both frontends iterate the map to register providers. The factory functions `make_anthropic_provider()` / `make_openai_provider()` take an optional `id` so each instance reports a distinct id to the registry. Flavored providers are constructed via `make_openai_compat_provider(api_key, base_url, id, flavor)`, which applies a flavor-specific default `base_url` when one isn't supplied (e.g. `lmstudio` → `http://localhost:1234/v1`, `ollama` → `http://localhost:11434/v1`). Env-var fallback (`ANTHROPIC_API_KEY`, `OPENAI_API_KEY`) applies only to providers whose ids are literally `"anthropic"` / `"openai"`; an OpenAI-compatible entry with no key but a `base_url` (e.g. local Ollama) registers keyless.

**Context-window discovery** (`get_context_window()` in `lib/src/util/model_info.cpp`, declared in `model_info.h`): resolution order is (1) exact-match config override (`"models": {"<model_id>": <tokens>}`), (2) longest-prefix match against a hardcoded table of common Anthropic/OpenAI/Llama models, (3) `0` (unknown). When a `Provider*` is passed (tier 4), an unknown result falls through to `provider->get_model_context(model_id)` — the discovery path used by flavored local servers so the context meter and auto-compaction work without manual config. Each flavor discovers context differently (`ServerFlavor` enum in `lib/include/haicode/model_context_parse.h`):
- **vLLM / OpenRouter**: context carried inline in the OpenAI-compatible `/v1/models` `data[]` entries (`max_model_len` / `context_length` respectively); parsed during `list_models()` into a cache.
- **LM Studio**: the OpenAI-compatible `/v1/models` carries no context. `fetch_lmstudio_context()` calls the native `GET /api/v1/models` (the deprecated `/api/v0` is not used), whose `{"models":[{key, max_context_length, loaded_instances:[{config.context_length}]}]}` shape is parsed by `parse_lmstudio_native_models()`. The OpenAI `/v1/models` `id` matches the native `key` exactly. A matched model with a non-empty `loaded_instances` returns its runtime window (`config.context_length`); otherwise the model's native `max_context_length`. If no key matches, the first model with a loaded instance is used (LM Studio typically serves one active model behind the OpenAI endpoint). Lazily fetched on first `get_model_context()` call and cached.
- **Ollama**: `POST {native_root}/api/show` → `model_info["llama.context_length"]`; lazily fetched. Note this is the GGUF-declared native max, not the runtime `num_ctx` (default 4096) — may overstate the effective window.
- **llama.cpp**: `GET {server_root}/props` → `default_generation_settings.n_ctx`; lazily fetched.

All four pure parse functions (`parse_model_context_inline`, `parse_lmstudio_native_models`, `parse_ollama_show`, `parse_llamacpp_props`) are unit-tested in `lib/tests/test_model_context.cpp`.

In the GUI, the provider dropdown is built dynamically via `MainWindow::RebuildProviderMenu()`. Each menu item carries its real `provider_id` in its `BMessage`; the `MSG_FETCH_MODELS` handler reads it from the message (never from the label). `SelectProvider()` matches by exact id and stores it verbatim — it must not coerce to two values, or restored sessions using a custom id would be silently remapped. `SettingsWindow` presents an add/edit/remove list editor; on save it serializes the full providers map and `HaiCodeApp` re-runs the registration loop, recreates the engine, and calls `RebuildProviderMenu()`.

**Pricing caveat:** `lookup_pricing()` keys on `"provider_id:model_id"`. A custom-id Anthropic provider (e.g. `"my-proxy"`) does not match the built-in `"anthropic:claude-..."` entries, so cost tracking returns null for it. The per-model `pricing` config override (keyed `"<provider_id>:<model>"`) remains the escape hatch.

**Pricing** (`lib/src/pricing/`): Handles model costs, token usage calculations, and pricing lookups for various providers.

**Session** (`lib/src/session/`): Manages session-specific events and state.

**Permission gate** (`lib/src/permission/permission.cpp`): `PermissionGate::check()` tests action+resource against fnmatch rules (session rules take priority over config rules). On "Ask", it blocks the engine thread on `std::future<PermissionEffect>` until the UI resolves a `std::promise`. `ToolRegistry::execute()` sets `result.denied = true` when the gate returns `Deny`.

**Config** (`lib/src/config/config.cpp`): Global config at `B_USER_SETTINGS_DIRECTORY/haicode/config.json`; project config at `<project_dir>/.haicode/config.json`. Project values overlay globals. The global config also stores `last_directory` (the most recently used project directory). Notable project-only fields: `build_command` (shell command run after every successful `write`/`edit` — see Build hook below). `ConfigLoader::merge()` is public so it can be called directly in tests and tooling. `libhaicode` links `-lbe` because `config.cpp` uses `BPath` and `find_directory`; executables that link `libhaicode` get this transitively.

**Tools** (`lib/src/tool/tools.cpp` + `lib/src/tool/web_tools.cpp`): Twenty built-in tools — see Tool Details below.

### `tui/` — ncurses frontend

Pure POSIX + Haiku kernel (no `BApplication`). `TuiApp::run()` multiplexes `STDIN_FILENO` and a wake pipe via `select()`. Engine threads post `EngineEvent` objects into a mutex-protected queue and write one byte to the pipe to wake the main loop. Permission requests travel through this same queue carrying a raw `PendingPermission*` (heap-allocated, with a `std::promise*`); the main loop renders the overlay and resolves the promise.

**Key bindings** (global — intercepted before input buffer):
- `Ctrl+N` — new session
- `Ctrl+P` — toggle Build/Plan mode
- `Ctrl+C` / `Ctrl+X` — interrupt running engine
- `t` / `T` — toggle todo list overlay
- `x` / `X` — toggle tool body expand/collapse (default: collapsed; header shows `▶`/`▼`)
- `Tab` — switch focus between session list and chat/input pane

Tool call bodies (`╔ … ╚` block) are hidden by default. `x` expands all blocks globally; `x` again collapses them. The tool result line (`✓`/`✗`) is always visible regardless of expand state. `ToolHeader` lines store just the tool name; `render_chat()` formats the full `╔ <name> ▶/▼` display text dynamically.

**Ask-user overlay**: when an `AskUserRequested` event arrives, `process_engine_events()` sets `ask_visible_` and `render_ask_overlay()` draws a centered box with the question and the preset options as radio rows, plus an "Other:" row for free-form input. `↑`/`↓` (or `k`/`j`) move the selection through options and the Other row; `Enter` submits the highlighted preset or switches to the Other text field for typing; `Esc`/`q` cancels. Submitting calls `engine_->reply_to_ask()` with the picked answer (or a cancel sentinel), which unblocks the waiting engine thread.

### `gui/` — BeAPI (Haiku native) frontend

- `HaiCodeApp` (BApplication) owns all haicode objects as `unique_ptr`. On startup, reads `last_directory` from settings JSON if no command-line arg was given.
- `MainWindow` (BWindow) handles all `MSG_*` messages in the BeAPI looper thread. Holds `engine_` as a pointer (`haicode::SessionEngine*`) not a reference, updated via `SetEngine()` when settings change.
- `GuiEventRelay` subscribes to `SessionEventBus` from engine threads and forwards events as `BMessage` via `BMessenger::SendMessage()` (thread-safe).
- Permission requests: engine thread allocates a heap `std::promise`, packs the raw pointer into a `BMessage` via `AddPointer`, posts to `MainWindow`. `PermissionWindow` sends `MSG_PERMISSION_REP` with action, resource, and effect. `MainWindow` fulfills the promise and `delete`s it. If effect=1 (Allow Always), posts `MSG_ADD_PERMISSION` to `be_app` which calls `perm_gate_->add_allow()`.
- `ask_user` questions: `GuiEventRelay` forwards `AskUserRequested` as `MSG_ASK_USER_REQ` (packing options as repeated `"option"` string fields). `_HandleAskUserReq()` instantiates `AskUserWindow` (a modal-style `BWindow` with radio buttons for each preset option plus an "Other:" radio wired to a `BTextControl`, modeled on `PlanReviewWindow`). The window posts `MSG_ASK_USER_REPLY` with the selected answer (or `cancelled=true` on dismiss), and `_HandleAskUserReply()` calls `engine_->reply_to_ask()` — this unblocks the engine thread waiting on `asking_cv_`.
- Model list is fetched asynchronously: `MainWindow` posts `MSG_FETCH_MODELS` to `be_app`; `HaiCodeApp` spawns a detached thread capturing a `shared_ptr<Provider>`, calls `list_models()`, posts `MSG_MODELS_LOADED` back to `MainWindow`. Fetched automatically on startup for the first configured provider.
- Session list uses `suppress_next_select_` flag to prevent `BListView::Select()` from triggering a recursive `MSG_SELECT_SESSION` → `_SelectSession()` → `Clear()` cascade. Always call `_SwitchToSession()` (not `_SelectSession()`) when programmatically switching sessions.
- Selecting a session restores its `project_dir_`, directory button label, provider dropdown, and model dropdown from the DB. Token totals, cumulative cost, and `current_context_tokens_` are repopulated from the persisted `SessionInfo` via `_RestoreSessionTotals()` — so the status strip's `last turn:` / `session:` / `context:` indicators reflect the session immediately on switch and after relaunch, rather than staying blank until the next completed turn. The live `StepEnded` handler still overwrites `current_context_tokens_` when a provider reports `usage.input > 0`.
- The status strip's context indicator is never blank when `max_context_` is known: before any usage arrives (first turn, or providers that omit the usage chunk) it renders `context: — / M`; once a turn reports input tokens it switches to `context: N / M (P%)`.
- `QuitRequested()` calls `std::exit(0)` — engine threads may be blocked in libcurl and cannot be joined cleanly.
- **`ChatView` message model**: `ChatView` maintains a `std::vector<ChatEntry>` (kinds: `UserText`, `AssistantText`, `ToolCalled`, `ToolResult`, `System`). Tool call bodies are shown expanded (`▼`) while the tool runs, then `AppendToolResult()` marks the entry `collapsed = true` and calls `_Rebuild()` to redraw the `BTextView` from scratch. Clicking a `[Tool: name] ▶/▼` header (detected via `ClickableTextView::MouseDown` → `OffsetAt()` → `FindBlockAt()`) calls `ToggleBlock()` which flips `collapsed` and rebuilds. `inhibit_scroll_` suppresses `ScrollToBottom()` during rebuild so intermediate appends don't thrash the scrollbar.

## Project Metadata

- `agents.md`: Contains system prompt instructions for agents.

## Key constraints

- **Never block the BeAPI looper thread** — all network/engine calls go through `be_app` or a detached thread posting back via `BMessenger`.
- **Never block the TUI main loop** — engine events arrive via the wake pipe; the loop must remain responsive to `getch`.
- **`B_SIZE_UNLIMITED` is only valid as a maximum size**, not a minimum. Use `B_SIZE_UNSET` for unconstrained minimum dimensions.
- **BeAPI `BScrollView` constructor**: use the 6-arg layout-aware form (no `resizingMode` parameter) when building layout-managed views. The 7-arg old-style form breaks layout and grays out child views.
- **SQLite schema**: `session`, `session_message` (types: `user_prompted`, `assistant_text`, `tool_called`, `tool_result`), `permission`. WAL mode + FK constraints (`ON DELETE CASCADE`) enabled.
- **Tool context messages**: assistant messages that include tool calls must be stored and reassembled with Anthropic `tool_use` content blocks (not plain text). Missing these causes orphaned `tool_result` blocks in the next request, making the model repeat work it already completed.
- **Non-destructive tools are always allowed within the project directory** (`ToolRegistry::execute()` in `lib/src/permission/permission.cpp`): `read`, `ls`, `grep`, `diff`, `find`, `symbols` bypass the gate when their resolved path is inside `ctx.working_dir`; `glob` bypasses for relative patterns or absolute patterns rooted inside the project; read-only `git` subcommands (`status`, `diff`, `log`, `show`, `branch`, `blame`, `ls-files`, `shortlog`, `describe`, `rev-parse`) always bypass; `process list` and `process check_port` always bypass; `web_search`, `web_extract`, `propose_plan`, `todo_write`, `ask_user` always bypass regardless of directory. **When adding a new read-only tool that should always bypass the gate inside the project directory, add it to the `if (name == "read" || ...)` chain in `lib/src/permission/permission.cpp` (search `name == "read"` to find the block) and update this list.**
- **SSE parser state must persist across `write_cb` invocations** (`lib/src/util/util.cpp`). `event_type` and `event_data` live on `HttpClient::State`, not as locals — libcurl invokes the write callback once per network chunk and a single SSE event (event:/data:/blank line) frequently straddles those chunks for large tool inputs (e.g. `propose_plan` markdown). Locals silently dropped half-parsed events, truncating tool input JSON and causing `parse_failed` drops in the engine. Clear both fields at the start of every new `post_sse()`.
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
- **`f.clear()` is called before `seekg(0)` after the binary scan** — `read()` past EOF sets `eofbit`; without the clear, `getline()` fails immediately and returns empty output for any file smaller than 8 KB

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
- Available in both Plan and Build mode — no engine-level filter removes it
- Always allowed — `ToolRegistry::execute()` bypasses the permission gate (same as web tools)
- Writes the plan markdown to `<project_dir>/.haicode/plans/plan_YYYYMMDD_HHMMSS_<4-hex-rand>.md`
- Parent directory created with `mkdir()` walking (same pattern as WriteTool)
- Returns the on-disk path so the engine can surface it via `PlanProposed` and end the turn

### TodoWriteTool (`todo_write`)
- Always allowed — `ToolRegistry::execute()` bypasses the permission gate (same as `propose_plan` and web tools)
- Whole-list replace: the model sends the full todo list every call, never a delta; an empty array clears everything
- Each item requires `content` (imperative form), `activeForm` (present-continuous for the spinner), and `status` (`pending`, `in_progress`, or `completed`)
- The engine detects this tool by name, writes the parsed todos to the `session_todo` table, and publishes a `TodoUpdated` event; the tool itself only validates and echoes the input
- Available in both Plan and Build mode

### AskUserTool (`ask_user`)
- Always allowed — `ToolRegistry::execute()` bypasses the permission gate (same as `propose_plan`, `todo_write`, and web tools)
- Asks the user a disambiguation question with 2–5 preset options plus a free-form "Other" entry; the model passes `question` (string) and `options` (string array of 2–5 items)
- The tool itself only validates and echoes the request as a JSON placeholder (`status: "asking"`); the real answer arrives later — the engine detects `ask_user` by name, publishes `AskUserRequested`, ends the turn, and blocks the agentic loop on `asking_cv_` until `reply_to_ask()` is called
- Once the user replies (selects a preset or types custom text, or cancels), `SessionStore::update_tool_result_by_call_id()` overwrites the placeholder `tool_result` row with `{"answer": "..."}` and the engine republishes `ToolSuccess` so the UI swaps the placeholder bubble for the picked answer; then the loop resumes with the real answer visible to the model as the tool result
- Available in both Plan and Build mode — the engine adds it to the Plan-mode allowlist so it survives the read-only tool filter

### DiffTool (`diff`)
- Writes proposed content to `<path>.tmp_diff`, runs `diff -u <original> <tmp>`, then `unlink`s the temp file
- Exit code 1 (differences found) is treated as success; exit code 2 is a diff error (returns failure)
- Returns `"(no differences)"` when the content is identical
- Always allowed within the project directory; paths outside go through the gate (`read` permission)

### GitTool (`git`)
- Runs `git -C <working_dir> <subcommand> [args...]` via `popen`
- Subcommand allowlist (validated before execution): `status`, `diff`, `log`, `show`, `branch`, `blame`, `stash`, `add`, `commit`, `checkout`, `reset`, `remote`, `merge`, `rebase`, `pull`, `push`, `fetch`, `tag`, `shortlog`, `describe`, `rev-parse`, `ls-files`
- All args passed through `sq()` — no shell injection possible
- Non-zero exit with non-empty output returns `success=false` with the output in `error`
- Read-only subcommands (`status`, `diff`, `log`, `show`, `branch`, `blame`, `ls-files`, `shortlog`, `describe`, `rev-parse`) are always allowed; mutating subcommands go through the gate (`git` permission, resource = subcommand name)

### FindTool (`find`)
- Wraps `find(1)` with optional `-maxdepth`, `-type`, `-name`, `-mtime`, `-size` flags
- All user-supplied values passed through `sq()` — safe against injection
- `type` is validated to one of `f`, `d`, `l` before being passed to the shell
- Relative `path` resolved against `ctx.working_dir`; defaults to `ctx.working_dir` if omitted
- Always allowed within the project directory; paths outside go through the gate (`read` permission)
- Fills the gap left by GlobTool, which does not support `**` recursive matching

### SymbolsTool (`symbols`)
- Heuristic C/C++ symbol search: finds definitions and classifies references, faster than grep for tracing fields and functions across files
- Three queries: `definition` (where a name is defined), `references` (every occurrence, classified), `callers` (usage sites only — `call` + `member_access`)
- **Tokenizer (`strip_non_code`)**: a state machine blanks out line/block comments, string literals, and char literals before matching, eliminating grep's false positives on identifiers mentioned in comments or strings; comment/string state persists across lines
- **Scope tracking**: a brace-depth counter plus a stack of enclosing class/struct names lets each hit be classified (`definition` / `call` / `member_access` / `declaration` / `mention`) and annotated with `(in ClassName::method)` for usage sites
- No `clangd` / `libclang` / index — pure `<regex>` over CODE-only substrings; unresolvable queries (templates, macros) degrade to candidate sets. C/C++ only at launch
- Always allowed within the project directory (read-only, `resource()` returns a resolved path like `find`); paths outside go through the gate (`read` permission)
- Cap: 200 hits with a `[truncated]` marker; output sorted by file then line, with a trailing `N hits in M files` summary

### ProcessTool (`process`)
- Three actions selected via the `action` field: `list`, `kill`, `check_port`
- **list**: runs `ps` and optionally filters output lines by a `filter` substring (header always stripped from the no-match path); returns `"(no matching processes)"` if filter matches nothing
- **kill**: calls `kill(2)` directly (not a shell command) with the given `pid` and `signal` (TERM, KILL, HUP, INT, USR1, USR2, STOP, CONT; default TERM)
- **check_port**: runs `netstat -n` and filters for lines containing `:<port>`; returns `"Nothing listening on port N"` if none found
- `list` and `check_port` are always allowed; `kill` goes through the gate (`process` permission, resource `pid:<N>`)

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

## Build hook

Set `build_command` in `<project_dir>/.haicode/config.json` to automatically run a build after every successful `write` or `edit` tool call:

```json
{ "build_command": "make -C build -j4 2>&1" }
```

If the command exits non-zero, its output is appended to the tool result with a `[build_hook]` prefix and `result.success` is set to `false`, so the model sees compile errors immediately and can fix them in the same turn rather than discovering them steps later.

After every run (success or failure) the engine publishes a `BuildHookResult` event (`lib/include/haicode/events.h`). Both frontends display a brief system line — `build ✓` or `build ✗ (exit N)` — so the user can see at a glance whether the hook ran and passed, even when it succeeded silently.

The hook runs synchronously in the engine thread. Avoid commands that take more than ~30 seconds, as the engine is blocked for the duration. The agent detects the build system and sets this field automatically when starting work on a new project (CMake → `make -C build 2>&1`, plain Makefile → `make 2>&1`, npm → `npm run build 2>&1`, Cargo → `cargo build 2>&1`).

## Plan mode

When a session is in Plan mode, the engine:
- Filters `bash`, `write`, `edit`, `external_terminal` out of the tool list — the model literally cannot attempt them
- Keeps `ask_user` in the tool list (alongside `read`, `glob`, `grep`, `ls`, `find`, `diff`, `todo_write`, `web_search`, `web_extract`, `propose_plan`, `discard_plan`) so the model can resolve genuine ambiguities with the user before proposing
- Injects `kPlanModeInstructions` (`lib/include/haicode/default_prompt.h`) into the system prompt
- Ends the turn automatically after a successful `propose_plan` call and publishes `PlanProposed` so the UI can show the review window

Plan mode instructions tell the model to call `ask_user` with 2–5 concrete options if the request is ambiguous before researching or proposing. Questions that can be answered by reading the codebase should not be asked, and no more than one `ask_user` call should precede a proposal.

`_HandlePlanDecision()` in `gui/src/MainWindow.cpp`: on approval, reads the plan file stored in `pending_plan_path_` (set when `PlanProposed` fired), calls `haicode::parse_plan_tasks()` to extract the `## Tasks` checklist, and calls `engine_->seed_todos(sid, todos)` to write the list to the DB and publish `TodoUpdated` — so the todo panel is populated immediately before the engine resumes. Then calls `engine_->set_mode(sid, Build)`, `engine_->inject_message(sid, kPlanApprovedMessage)`, and `engine_->continue_session(sid)` unconditionally. The `inject_message` call appends a `user_prompted` message (the shared `kPlanApprovedMessage` constant from `lib/include/haicode/default_prompt.h`) so the model, on resume, knows the plan was approved and it is now in Build mode — without it, `continue_session` re-enters the loop with no new input and the model has to infer the state change from the tool list shifting. The UI refresh is gated on `sid == active_session_id_` but the session always resumes. The TUI plan-review overlay (`tui/src/TuiApp.cpp`, `'a'`/Enter case) follows the identical sequence: `seed_todos` → `set_mode(Build)` → `inject_message(kPlanApprovedMessage)` → `continue_session`. On denial, the session stays in Plan mode; a "Plan discarded" system message is appended to the active chat but no mode change or `continue_session` is called.

**Plan task format**: `propose_plan` should include a `## Tasks` section with a markdown checklist for seeding to work:

```markdown
## Tasks
- [ ] Add BuildHookResult event to events.h
- [ ] Publish event in engine.cpp
- [ ] Handle event in GUI relay
```

`parse_plan_tasks()` (`lib/src/engine/engine.cpp`) reads lines from the `## Tasks` section until the next `##` heading or EOF. It accepts `- [ ]`, `- [x]`, `- [X]`, and plain `- ` prefixes. `activeForm` is derived by converting the leading verb to gerund (e.g. "Add" → "Adding") via a lookup table; unknown verbs fall back to the content string unchanged.

`engine_->seed_todos()` (`lib/include/haicode/engine.h`) calls `store_.replace_todos()` then publishes `TodoUpdated` — it is also callable from the TUI plan approval path and anywhere else a todo list needs to be set programmatically.
