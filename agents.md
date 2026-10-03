# Project

HaiCode — a native coding-agent app for **Haiku R1**. C++20 core library
(`lib/`) with a `haicode-gui` (Haiku BeAPI) frontend. Talks to Anthropic
and OpenAI-compatible LLM providers, runs
tools with per-action permissions, and persists every session to SQLite.

# Build & run

```bash
cmake -B build -S .            # only needed once
make -C build -j4              # builds lib + gui + tests
ctest --test-dir build         # runs all 36 test binaries

./build/gui/haicode-gui [project_dir]
```

Build hook for this project: `make -C build -j4 2>&1`. Header changes can
require a multi-minute rebuild; use a sufficiently long tool timeout.

# Plan mode

When proposing a plan, always include a `## Tasks` section with a markdown
checklist as the **last** section. Each item is one atomic step in imperative
form. The harness parses this section on approval and seeds the todo panel
automatically — no need to call `todo_write` at the start of the build turn.

```markdown
## Tasks
- [ ] Add BuildHookResult event to events.h
- [ ] Publish event in engine.cpp
- [ ] Handle event in GUI relay
```

Rules:
- Use `- [ ] verb + object` phrasing (imperative, e.g. "Add", "Fix", "Update").
- One item per file or logical unit — not one item per line changed.
- Do not pre-check items (`- [x]`); the harness sets all items to `pending`.
- Sections after `## Tasks` terminate the list, so keep it last.

# Conventions

- **Tool exchanges stay complete.** Persist failed `not run: <reason>` results
  for every skipped call after denial, plan proposal, or interruption. Context
  assembly groups one batch's responses in one user message, marks failures
  with `is_error`, and repairs legacy missing/late results without rewriting
  stored history; orphan and duplicate responses never go on the wire.

- **Prompts are FIFO per session.** Capture attachments/skills at submission,
  queue while foreground work runs, persist and publish `Prompted` only when
  each distinct turn starts. Drain after complete tool exchanges (including
  manual compaction); clear running state atomically with the queue check.
  Shutdown rejects new work and discards queued prompts. `PromptQueued` is
  feedback only; `StepEnded` accounts usage, `TurnEnded` marks foreground
  completion. Do not reset counters/todos when merely queueing.
- **Title refinement is tracked maintenance.** Never hold foreground busy
  state behind title requests. Use scoped cancellation, skip interrupted
  turns, atomically reject stale baseline writes, and join outside engine/UI
  locks on shutdown or retirement; no detached workers.

- **Disruptive changes warn and ask.** Settings saves, provider updates, and
  directory switches go through `HaiCodeApp::_ConfirmDisruptiveChange` before
  mutating anything; directory selection is proposed (`MSG_DIR_PROPOSED`),
  not applied, until accepted. Cancel leaves app, window, DB, and config
  files unchanged. Accepted replacements interrupt every running session —
  background ones included.

- **Session deletion is confirmed and retired.** `SessionEngine::delete_session`
  marks the session retiring (spawns refused, queue discarded), interrupts
  asks/approvals/scoped streams, joins foreground and title workers outside
  engine locks, deletes rows only afterwards, and clears that session's gate
  layers (`erase_session_state`). DB failure rolls retirement back and keeps
  the session usable. The GUI runs retirement on tracked workers and ignores
  late events for deleted sessions.
- **Bulk cleanup reuses that retirement path.** File → Cleanup resolves a
  `SessionFilter` (`SessionStore::sessions_matching`, uncapped on purpose —
  the sidebar's `list(50)` is not a deletion limit), confirms with Cancel
  default + `B_ESCAPE`, then posts stable session ids (never list indices) to
  a tracked lifecycle worker that loops `delete_session` and finally runs
  housekeeping: `Database::prune_orphans`, `SessionStore::prune_stale_checkpoints`
  (non-`complete` rows only), `SessionStore::clear_stale_checkpoint_contexts`
  (every complete row except each session's highest-`through_seq` one —
  summaries and `previous_checkpoint_id` always survive), and
  `Database::reclaim_space` (WAL checkpoint then `VACUUM`; never inside a
  `DbTxn`, never while a statement is mid-step, and deferred while any session
  streams). `util::sweep_scratch_files` ages out `haicode_shot_*` /
  `haicode_diff_*` in the system temp directory with an mtime floor, since a
  just-taken screenshot or a live diff scratch is still in flight. The GUI
  never calls `SessionStore::delete_session` directly. Prune
  `HaiCodeApp::session_flags_` whenever a session id dies.

- **Image retention is wire-only.** Keep raw user/screenshot images for the
  current and immediately preceding user turn; replace older images with
  placeholders/descriptions without changing DB bytes. Do not backfill
  expired images. Fallback requests share the foreground scoped token and
  register their provider for cancellation. Persist failed description
  attempts once; cancellation stays retryable. Enforce the 4 MiB decoded
  user-image cap at engine ingestion, with bounded image reads and explicit
  unavailable markers. Every compaction rebuild retains vision and inference
  settings. Malformed-row diagnostics must never include raw payloads.

- **Built-in permission exemptions are fallbacks.** Mode/offline checks sit
  above everything; the always-readable system roots are absolute; every
  other built-in allow (in-project reads, read-only git, web tools,
  screenshot, process list/check_port) applies only when no rule layer
  matched, so a configured or session Deny/Ask rule on those tools takes
  effect. The gate reports a matched Ask rule with its layer source instead
  of the anonymous `"prompt"` default — rely on that distinction, not on
  effect alone.

- **Bash Allow patterns are segment-aware; read-only git ignores repo
  config.** A bash Allow rule with glob metacharacters must cover every
  `;`/`&&`/`||`/`|`/newline/`&`-separated segment
  (`bash_pattern_authorizes` in `lib/src/permission/permission.cpp`);
  segments with command substitution never match a pattern. Deny patterns
  and non-bash actions keep whole-string fnmatch. GitTool runs read-only
  invocations with `-c core.fsmonitor= -c core.hooksPath=/dev/null` plus
  `--no-ext-diff --no-textconv` (diff/log/show only), so repo-local config
  can't execute programs through the bypass path.

- **Provider registration is generic.** `AppConfig::providers` is a
  `map<id, ProviderConfig>`; each `ProviderConfig` has a `type` of
  `"anthropic"`, `"openai"`, `"chatgpt"`, or one of the flavored
  OpenAI-compatible servers (`"ollama"`, `"vllm"`, `"openrouter"`,
  `"lmstudio"`, `"llamacpp"`; inferred from the id when empty —
  `"anthropic"` → anthropic, `"chatgpt"` → chatgpt, else openai). Both
  frontends iterate this map to register providers. Env-var fallback
  (`ANTHROPIC_API_KEY`, `OPENAI_API_KEY`) applies only to ids literally
  `"anthropic"`/`"openai"`. Flavored types use
  `make_openai_compat_provider()` with a flavor-specific default `base_url`
  (e.g. `lmstudio` → `http://localhost:1234/v1`).
- **The `chatgpt` provider type is OAuth-based, not key-based.** It uses
  `make_codex_provider()` (`lib/src/provider/codex.cpp`), registered only
  when `codex_auth_signed_in()`; credentials live in the codex_auth token
  store at `$(B_USER_SETTINGS_DIRECTORY)/haicode/openai-auth.json`, never
  in `ProviderConfig` (key/base_url stay empty). The OAuth access token is
  audience-locked to the ChatGPT Codex backend, so the provider speaks the
  **Responses API** (`{base}/codex/responses`, default base
  `https://chatgpt.com/backend-api`) — NOT chat/completions — with
  `Authorization`, `chatgpt-account-id`, `originator: codex_cli_rs`, and
  `OpenAI-Beta: responses=experimental` headers. Auth flow lives in
  `lib/src/auth/codex_auth.cpp` (PKCE + loopback:1455 callback server +
  refresh-on-skew + one forced 401-retry). Usage is subscription-billed,
  so there are no built-in pricing entries; the cost column stays empty
  unless the user adds a `pricing` override (lookup miss is handled).
  **Experimental** (Task 27): Settings labels the type "ChatGPT
  (Experimental)" with a terms-of-use note (unofficial Codex CLI backend;
  may break or violate ToS), and an unsigned-in chatgpt entry stays in
  MainWindow's provider dropdown disabled with "(sign in via Settings)"
  instead of vanishing — `SelectProvider` never marks a disabled item.
- **Provider menu items carry their id.** Each `BMenuItem` in MainWindow's
  provider dropdown attaches `provider_id` to its `BMessage`; the
  `MSG_FETCH_MODELS` handler reads it from the message, never from the label.
  `SelectProvider()` matches by exact id — do not coerce to two values.
- **Pricing is keyed `"<provider_id>:<model-prefix>"`.** Custom-id Anthropic
  providers (e.g. `"my-proxy"`) do not match the built-in
  `"anthropic:claude-..."` entries; the `pricing` config override is the escape
  hatch until pricing is keyed on type instead of id.
- **Compaction never deletes rows.** Context compaction is checkpoint-based:
  `compaction_checkpoint` rows record `through_seq` boundaries; the full
  conversation stays in `session_message`. Context assembly goes through
  `SessionEngine::load_context_messages` → `apply_checkpoint` (slice
  `seq > through_seq`, prepend the rendered checkpoint block). Pure logic
  lives in `lib/src/compaction/compaction.cpp`.
- Style: minimal comments (only for non-obvious why), 4-space indent, Haiku
  BeAPI naming (`BWindow`, `BMessage`, `BMessenger`). See `CLAUDE.md` for the
  deeper engine/pricing/permission walkthrough.
