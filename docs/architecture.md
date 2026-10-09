# Architecture

## Two layers

| Layer | What it is |
|-------|------------|
| `lib/` | `libhaicode` (static) — core engine, providers, tools, permissions, persistence. C++20 + POSIX plus the Haiku Storage/Support kits (`BPath`, `find_directory`, `BUrl`): it links `be`, `root`, `network`, sqlite3, curl, and libcrypto (`lib/CMakeLists.txt`). No GUI dependency — no Application Kit, no views. |
| `gui/` | `haicode-gui` — Haiku native BeAPI frontend. `BApplication` owns the engine; engine threads post events back to the window through `BMessenger` (`gui/src/GuiEventRelay.cpp`). |

Both frontends consume the same `libhaicode` API, so the engine can be driven
headlessly from a test binary.

## `lib/` layout

| Directory | Contents |
|---|---|
| `src/engine/` | The agentic loop: turn accounting, step budget, status updates, compaction triggering, plan seeding. |
| `src/provider/` | Anthropic, OpenAI-compatible (and the `ollama`/`vllm`/`openrouter`/`lmstudio`/`llamacpp` flavors), and the ChatGPT/Codex Responses-API provider, plus request-body building and error classification. |
| `src/auth/` | ChatGPT OAuth (PKCE + loopback callback on port 1455, refresh-on-skew, token store). |
| `src/tool/` | The 21 built-in tools and the `ToolRegistry` gate integration. |
| `src/permission/` | `PermissionGate` rule layers, bash pattern segmentation, git read-only classification, approval broker. |
| `src/db/`, `src/session/` | SQLite storage (`Database`, `SessionStore`), migrations, bulk-query primitives. |
| `src/compaction/` | Pure checkpoint logic: token budget, history splitting, serialization, summary validation. |
| `src/config/` | Layered config loading, presence-based merge, project trust boundary, non-destructive saves. |
| `src/pricing/`, `src/util/` | Model metadata tables and pricing, plus shared utilities (atomic writes, subprocess runner, SSE parsing, markdown). |

Public headers are in `lib/include/haicode/` — `engine.h`, `provider.h`,
`tool.h`, `events.h`, `db.h`, `config.h`, `types.h`, `util.h`, `compaction.h`,
`model_db.h`, `pricing.h`, `permission_requests.h`, `skills.h`, `subprocess.h`,
`markdown.h`.

## `gui/` layout

`HaiCodeApp` (single-instance app signature, disruptive-change confirmation,
engine ownership) · `MainWindow` (sidebar, prompt row, mode/provider controls,
plan decision handling) · `ChatView` + `MarkdownView` (streaming transcript,
markdown rendering, link resolution) · `PermissionWindow` /
`PermissionsCenterWindow` / `PermissionRuleEditWindow` · `SettingsWindow` /
`ModelDatabaseWindow` · `PlanReviewWindow` · `AskUserWindow` · `GuiEventRelay`
(engine events → `BMessage`s).

## Persistence

SQLite in WAL mode with foreign keys and `ON DELETE CASCADE`; all sessions share
one connection, serialized by a store-level mutex. Tables: `session`,
`session_message` (types `user_prompted`, `assistant_text`, `tool_called`,
`tool_result`), `session_todo`, `compaction_checkpoint`. Schema versioning is
`PRAGMA user_version`, currently 3; migrations are numbered `if (version < N)`
steps in `Database::migrate()` and old ones are never edited. Session ids are
descending-timestamp sortable, so `list()` returns newest first.

## Where the deep detail lives

- [`CLAUDE.md`](../CLAUDE.md) — for agents working on this repository: build and
  test commands, the full test-suite table, per-tool behavior, the permission
  gate's implementation constraints, provider request-body rules, and the plan
  mode flow.
- [`agents.md`](../agents.md) — the project's own agent-facing instructions
  (build hook, plan mode, conventions), which HaiCode injects into its system
  prompt when this repository is open.
- [`MODEL_NUMBERS.md`](../MODEL_NUMBERS.md) — the sourced reference behind the
  model context-window, output-cap, and pricing tables.
- [`RELEASE_1.0_TASKS.md`](../RELEASE_1.0_TASKS.md) — the remaining release
  checklist.
