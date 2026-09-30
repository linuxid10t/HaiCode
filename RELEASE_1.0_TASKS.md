# HaiCode 1.0 Release Tasks

Instructions for the agent carrying out the 1.0 release work. Tasks are listed
in the order they should be completed: later tasks build on earlier ones
(e.g. clean shutdown depends on cancellable processes and HTTP; project trust
depends on layered config loading).

## Ground rules

- Read `CLAUDE.md` first and follow its build, test, and style conventions.
- HaiCode builds only on Haiku (C++20, GCC 13). Build with
  `cmake -B build -S . && make -C build -j4` and run the relevant `test_*`
  binaries after every task. Do not launch a second GUI instance.
- One task per commit. Keep each change minimal and scoped to its task.
- Every behavior fix gets a regression test in `lib/tests/` where it is
  testable without the GUI. Add new test binaries to `lib/CMakeLists.txt`
  (and to CTest once Task 1 lands) and to the test table in `CLAUDE.md`.
- File and line references below are as of commit `7f634b3` and are
  approximate. Locate code by symbol, not line number.
- When a task changes documented behavior, update `CLAUDE.md`, `README.md`,
  and `agents.md` in the same commit.

---

## Phase 0 — Make the test suite a gate

### Task 1. Register tests with CTest and harden the build

- Add `enable_testing()` to the root `CMakeLists.txt` and an `add_test()` for
  every `test_*` executable in `lib/CMakeLists.txt` (20 today).
- Tests that use `assert()` (6 files, e.g. `test_process.cpp`,
  `test_git_find.cpp`, `test_openai_translate.cpp`) must keep checking in
  Release builds: replace `assert` with a `CHECK` macro that always runs, or
  `#undef NDEBUG` before `<cassert>` in test sources.
- Add `CONFIGURE_DEPENDS` to the `file(GLOB_RECURSE ...)` calls in
  `lib/CMakeLists.txt` and `gui/CMakeLists.txt`, then remove the "re-run cmake
  after adding a file" caveats from the docs.
- Set a default `CMAKE_BUILD_TYPE` (e.g. `RelWithDebInfo`) when none is given,
  and enable `-Wall -Wextra` for project targets.
- Make missing `sqlite3`, `curl`, and `crypto` libraries a configure-time
  `FATAL_ERROR` instead of a warning. Verify the OpenSSL package-name hint
  (`openssl32_devel` looks wrong; likely `openssl3_devel`).

**Done when:** `ctest --test-dir build` runs all tests and passes in both Debug
and Release.

---

## Phase 1 — Persistence foundation

### Task 2. Check every SQLite call and surface failures

- In `lib/src/db/db.cpp`, every `sqlite3_prepare_v2` / `sqlite3_step` /
  `sqlite3_exec` result is ignored (e.g. `SessionStore::create`,
  `append_message`, `update_*`, `insert_checkpoint`). Introduce an RAII
  statement wrapper that finalizes automatically and reports errors.
- Write methods must report failure (return status or throw a typed
  exception). The engine must turn a failed persist into a `StepFailed` event;
  the GUI must show it. Never report success until SQLite confirms it.
- Set `PRAGMA busy_timeout` (e.g. 5000 ms) when opening the database.
- Add `PRAGMA user_version` schema versioning and move the ad-hoc
  `tok_last_input` column check into a numbered migration.
- Remove the unused `permission` table from the schema (via a migration) or
  document why it stays.

### Task 3. Make message sequencing and transactions safe under concurrency

- `append_message` computes `next_seq()` and then inserts as two separate
  steps. Concurrent writers to one session (GUI thread + runner thread) collide
  on `UNIQUE(session_id, seq)` and the row is silently lost. Make the
  read-and-insert atomic (single `INSERT ... SELECT COALESCE(MAX(seq),0)+1`
  or a per-store mutex around the pair).
- `replace_todos` runs `BEGIN` … `COMMIT` on the one connection shared by all
  sessions (`db.cpp` ~734/769). A second thread's `BEGIN` fails and its writes
  get absorbed into the first transaction. Serialize whole transactions with a
  store-level mutex (or a dedicated DB worker thread), check `COMMIT`, and
  `ROLLBACK` on any failure.
- Test: two threads appending to the same session and two sessions replacing
  todos concurrently, looped; no lost rows, no duplicate seqs.

### Task 4. Enforce a single running instance

- Add an application resource file (`.rdef`) with signature
  `application/x-vnd.haicode` and `B_SINGLE_LAUNCH`, wired into the GUI build
  (full resource work is Task 28; the launch flag is needed now).
- Forward a second launch's project-directory argument to the running instance
  (`ArgvReceived`/`RefsReceived`) instead of starting a new one.
- Remove the "single instance only" warning from the README once enforced.

---

## Phase 2 — Configuration correctness and trust

### Task 5. Make config writes atomic and non-destructive

- `HaiCodeApp.cpp` rewrites `config.json` with a truncating `std::ofstream` in
  several handlers (`MSG_PERSIST_PM`, `MSG_DIR_CHANGED`,
  `MSG_PROVIDERS_UPDATED`, `MSG_SETTINGS_SAVED`). A parse failure is swallowed,
  so saving after a hand-edit typo replaces the whole file with a few keys
  (API keys and providers lost).
- Centralize all config updates in one library function: read, refuse to write
  if the existing file does not parse (show the error to the user), apply the
  change, write through `util::atomic_write_file`.
- Report write failures to the user.

### Task 6. Store secrets with owner-only permissions

- `util::atomic_write_file` creates new files as 0644 (`util.cpp` ~186). Add a
  mode parameter; write `openai-auth.json` (OAuth tokens, `codex_auth_save`)
  and `config.json` (API keys) as 0600.
- On startup, tighten existing files in the settings `haicode` directory to
  0600 if they are group/world readable.
- `save_permission_document` uses a fixed `.perm-tmp` name without fsync;
  switch it to `atomic_write_file` too.

### Task 7. Fix config merge semantics

`ConfigLoader::merge` uses struct default values as "not specified":

- `default_mode` defaults to `"plan"`, so an absent project config always
  overwrites a global `"build"` — the setting is lost on every relaunch.
- `web_search_max_results` defaults to `5`, so the global value is always
  overwritten.
- `providers` entries are replaced wholesale, dropping the global `api_key`.
- `auto_compact` / autoname booleans can only be turned off by an overlay,
  never back on; numeric overrides equal to the default are ignored.

Fix by parsing into a layer with `std::optional` fields (or tracking key
presence) and applying defaults only after merging. Per-key merge for
provider entries. Extend `test_config_permission` for every case above.
Remove the unused `auto_compact_reserve`, `MCPServerConfig`/`mcp`, and
`ProviderConfig::env` fields, or implement them.

### Task 8. Stop project settings leaking into the global config

- `config_` in `HaiCodeApp` is the merged global+project config, and the
  Settings save writes all of it into the global file: `build_command`,
  `providers`, `skills`, `models`, `vision`. Opening project A and saving
  Settings makes A's build hook run in every project.
- Keep the global and project layers separate in memory. The Settings window
  edits only the global layer; project-only fields (`build_command`) are edited
  as project settings and written to `<project>/.haicode/config.json`.

### Task 9. Add a trust boundary for project configuration

A repository's `.haicode/config.json` is currently fully trusted. A hostile
repo can:

- set `providers.anthropic.base_url` / `providers.openai.base_url` /
  `providers.chatgpt.base_url` to its own server; the `ANTHROPIC_API_KEY` /
  `OPENAI_API_KEY` env fallback or the ChatGPT OAuth token is then sent there
  (`make_provider_registry` in `HaiCodeApp.cpp`);
- add `permissions` rules that allow everything without prompting;
- set `build_command` to run arbitrary shell commands after the first
  `write`/`edit`.

Required:

- Never merge `providers` from project config. Credentials and endpoints come
  from the global layer only.
- Per-project trust record (stored in the global settings, keyed by resolved
  project path). Until the user trusts a project, ignore its `permissions`,
  `build_command`, `agents`, and `web_search.api_keys`.
- Trust prompt when a project with such keys is opened, listing exactly what
  it would enable. Re-prompt if those keys change.
- Tests: an untrusted project config cannot change provider URLs, add Allow
  rules, or set a build hook.

---

## Phase 3 — Cancellable execution

### Task 10. One cancellable subprocess runner for all tools

- `BashTool` (`tools.cpp` ~262) uses `popen` + `timeout`: it ignores
  `ctx.interrupt`, has no cap on the model-supplied timeout, and a backgrounded
  child (`server &`) keeps the pipe open so the call never returns.
- `git`, `grep`, `find`, `process`, and `screenshot` use `popen` with no
  timeout or interrupt at all; `git push/pull/fetch` can hang forever on a
  credential prompt.
- When `read_pipe` stops at 100 KB, `pclose` kills the child with SIGPIPE and
  the tool reports exit code -1 — a truncated success is shown as a failure.
- Generalize `detail::run_build_hook` (own process group, poll loop, kill
  group on interrupt/timeout, output cap) into a shared runner. Keep draining
  (and discarding) output past the cap; report truncation separately from
  exit status. Close inherited file descriptors in the child (or use
  `O_CLOEXEC`) and give the child `/dev/null` as stdin.
- Cap the bash timeout (e.g. 600 s). Set `GIT_TERMINAL_PROMPT=0` for git.
- Move all tools onto the runner and pass `ctx.interrupt` through.
- Tests: interrupt kills a `sleep 60`; `sleep 60 &` returns promptly;
  >100 KB output reports success plus a truncation marker.

### Task 11. Make HTTP cancellable and bounded

In `lib/src/util/util.cpp`:

- Cancellation only takes effect when data arrives; a stalled connection
  ignores it until the 300 s total timeout. Add `CURLOPT_XFERINFOFUNCTION`
  checking the cancel flag.
- Replace the fixed 300 s `CURLOPT_TIMEOUT` on streams with
  `CURLOPT_CONNECTTIMEOUT` plus `CURLOPT_LOW_SPEED_LIMIT`/`LOW_SPEED_TIME`, so
  long-but-live generations (slow local models) are not cut off.
- Cap response bodies in `get()`/`post_json()` and the SSE line buffer.
- `CURLOPT_FOLLOWLOCATION` is on for credential-bearing requests; curl strips
  only `Authorization`/cookies on a cross-host redirect, so `x-api-key` is
  forwarded. Disable redirects for provider API calls (or restrict to
  same-host) and set `CURLOPT_PROTOCOLS`/`REDIR_PROTOCOLS` to HTTP(S).
- Set `CURLOPT_NOSIGNAL`. Call `curl_global_init` once at startup, not in
  every `HttpClient` constructor.
- SSE parser: join multiple `data:` lines with `\n` per the SSE spec.
- Tests (extend `test_http_client`): cancel during connect and during a silent
  stream returns within ~1 s; oversized body is truncated safely.

### Task 12. Orderly shutdown instead of `std::exit`

- `HaiCodeApp::QuitRequested` calls `std::exit(0)`, running static
  destructors while engine threads are live.
- With Tasks 10–11 in place: ask for confirmation if any session is running;
  then cancel pending asks and approvals, interrupt all sessions, call
  `SessionEngine::shutdown()` with a bounded wait, close the database, and
  quit. Fall back to `_exit` only after the timeout.

---

## Phase 4 — Turn integrity in the agentic loop

### Task 13. Never leave a tool call without a result

- In `SessionEngine::agentic_loop` the tool loop breaks early on a denied call
  (`if (result.denied) { any_denied = true; break; }`), on a successful
  `propose_plan`, and on interrupt. Remaining calls from the same assistant
  turn get no `tool_result` row. Both Anthropic and OpenAI reject every later
  request containing an unanswered `tool_use`, so the session is permanently
  broken.
- Persist a "not run: <reason>" `tool_result` for every skipped call.
- Add a repair pass in context assembly (`assemble_messages` or
  `load_context_messages`) that synthesizes a result for any unanswered
  `tool_use`, so existing broken sessions recover.
- Emit all `tool_result` blocks for one assistant turn in a single user
  message (currently one user message per result).
- Tests: deny the first of two parallel calls, then send another prompt — the
  next request is well-formed.

### Task 14. Handle prompts submitted while a turn is running

- `MainWindow::_SubmitPrompt` does not check whether the session is running.
  A prompt submitted during tool execution or a permission prompt is stored
  between the `tool_use` row and its `tool_result`; the API rejects the next
  request.
- `submit_prompt` stores the prompt but spawns a runner only when
  `session_running_` is false. The flag stays true (a) after `interrupt()`
  while the loop winds down — the GUI already shows idle — and (b) during
  `refine_title_llm` after the final step. A prompt sent in either window is
  never processed.
- Fix: queue prompts that arrive while running and append them only at a safe
  boundary (after all tool results of the current step); when the runner
  exits, re-check for unprocessed prompts and continue. Clear the running flag
  before title refinement, or run refinement after the flag is cleared.
- Skip `refine_title_llm` when the run was interrupted, and pass the run's
  stream token so it is cancellable.
- Tests: submit during a parked tool, submit right after interrupt, submit
  right after the final step — each prompt gets exactly one response.

### Task 15. Smaller engine correctness fixes

- Both request rebuilds after compaction (`engine.cpp` ~1550 and ~1697) call
  `builder.build(...)` without `model_accepts_images`, so text-only models
  receive raw images. Pass `primary_supports_vision` at both sites.
- `agentic_loop` reads `interrupt_flags_[session_id]` without holding `mu_`
  (~1213) while other threads insert into the map. Read it under the lock.
- `backfill_attachment_descriptions`: pass the run token to `describe_image`
  and stop retrying a failed description on every step.
- Mirror the GUI's 4 MB image cap in `submit_prompt` (text attachments are
  already clamped engine-side).
- `create_session` falls back to `"anthropic"` even if it is not registered;
  fall back to the first registered provider.
- Old screenshot/image attachments from past turns are resent in full every
  request; replace them with a placeholder after N turns (as with old tool
  output truncation).
- `assemble_messages` silently drops rows that throw during parsing; log them.

### Task 16. Don't silently interrupt running sessions on settings changes

- Saving Settings, changing providers, or changing directory calls
  `_RecreateEngine`, which shuts down the engine and interrupts every running
  session. Warn and ask first, or defer the recreate until runs finish.

### Task 17. Safe session deletion

- `MSG_DELETE_SESSION` deletes immediately with no confirmation and while the
  session may be running.
- Confirm with a `BAlert`. If running: interrupt, wait for the runner, then
  delete. Clear the session's gate rules, grants, pending approvals, and
  engine maps (add an engine API for this).

---

## Phase 5 — Permissions hardening

### Task 18. Let configured Deny rules override built-in exemptions

- `ToolRegistry::evaluate` returns Allow for `web_search`, `web_extract`,
  `screenshot`, `process list/check_port`, read-only `git`, and in-project
  reads before consulting any rule. A Deny written in the Policies tab is
  accepted and silently has no effect.
- Check explicit configured/session Deny rules first; built-in exemptions
  become the fallback. Keep mode restrictions above everything.
- The operation inspector must show the new decision path. Update tests in
  `test_config_permission`.

### Task 19. Gate web access and screenshots

- `web_extract` never prompts: a prompt-injected model can exfiltrate data in
  a URL or reach `localhost`/private services.
  - Prompt on the first fetch to each new host per session (grant = host).
  - Block loopback, link-local, and private address ranges unless explicitly
    allowed (resolve the host, check the IP, pin it for the request).
  - Cap `max_chars` and the download size.
- `screenshot` captures the whole screen with no prompt; make it Ask by
  default.
- Document the new defaults in the permission docs.

### Task 20. Tighten rule matching and read-only git

- `fnmatch` rules match the whole bash command string, so `make*` also allows
  `make; rm -rf ~`. Split commands on `;`, `&&`, `||`, `|`, newlines, and
  command substitution; require every segment to match, or refuse pattern
  Allow rules for bash and warn in `PermissionRuleEditWindow`.
- Read-only git invocations run without a prompt but honor repo-local config
  that can execute programs (`core.fsmonitor`, `diff.external`, textconv
  filters). Invoke read-only git with `-c core.fsmonitor=
  -c core.hooksPath=/dev/null --no-ext-diff --no-textconv`.
- Show the git rule-granularity limitation (an Allow for `branch` also covers
  `branch -D`) in the rule editor, or match on subcommand+first argument.

---

## Phase 6 — File-system correctness on Haiku

### Task 21. Preserve attributes and symlinks on write

- `util::atomic_write_file` renames a new `mkstemp` file over the target,
  dropping BFS attributes (`BEOS:TYPE`, Tracker metadata), and replaces a
  symlink with a regular file.
- Resolve symlinks before writing (write to the target). Copy all attributes
  from the old file to the temp file (`fs_open_attr_dir`/`fs_read_attr`/
  `fs_write_attr`) before the rename. Tests for both.

### Task 22. Keep `diff` non-destructive

- `DiffTool` creates its scratch file beside the target
  (`<path>.tmp_diff_XXXXXX`). It is allowed in Plan mode and fails for
  read-only locations such as the system headers. Create the scratch file in
  the system temp directory instead.

---

## Phase 7 — Provider and model correctness

Before changing request parameters, verify them against each provider's
current API documentation. Parameters differ per model; drive them from a
per-model capability table rather than hardcoding one behavior.

### Task 23. Anthropic provider

In `lib/src/provider/anthropic.cpp`:

- `reasoning_effort == "off"` is sent as `output_config.effort: "off"`, which
  is not a valid effort value (valid: `low`, `medium`, `high`, `xhigh`,
  `max`) — every request fails. Map "Off" to the lowest supported effort, or
  to a thinking-disabled setting only on models that accept one.
- No `thinking` parameter is sent. Send `thinking: {type: "adaptive"}` where
  supported and `display: "summarized"` so the reasoning panel shows text
  (current models default to omitted thinking text).
- Capture thinking blocks (including signatures) and replay them unchanged in
  assistant turns during tool-use loops.
- Update the effort/"Off" choices in the Inference UI to match.

### Task 24. OpenAI-compatible provider

In `lib/src/provider/openai.cpp`:

- `chat_template_kwargs` and `reasoning_effort: "off"` are sent to real
  OpenAI, which rejects unknown parameters. Send `chat_template_kwargs` only
  to flavored local servers (vLLM, llama.cpp, LM Studio, Ollama); map "off" to
  a valid value per provider.
- Reasoning models require `max_completion_tokens` instead of `max_tokens`
  and reject `temperature`/`top_p`.
- Parse cached-token usage (`prompt_tokens_details.cached_tokens`).

### Task 25. Output limits, retries, and error handling

- `kDefaultMaxTokens = 8192` is too small once thinking counts against it and
  truncates large `write` inputs. Raise it for streaming requests (e.g. 32k+,
  bounded by the model's output cap).
- A `MaxTokens` stop ends the turn silently. Publish a clear notice (and
  consider automatic continuation).
- Add 429/529/"rate limit"/"too many requests" to the transient-error list
  with exponential backoff that honors `Retry-After`.
- The overflow matcher (`"context"`, `"exceeds"`) is broad enough to trigger
  compaction on unrelated errors; match provider-specific overflow codes.

### Task 26. Model metadata tables

- `lib/src/util/model_info.cpp`: current Anthropic models have 1M-token
  windows but match 200K entries or nothing, and the Anthropic provider has no
  `get_model_context`, so auto-compaction is off by default for current
  models. Implement discovery from the Anthropic Models API
  (`max_input_tokens`), then refresh the table.
- `lib/src/pricing/pricing.cpp`: prices are stale (Anthropic Opus entries,
  `openai:gpt-5`). Refresh them, and fall back to the provider *type* when a
  custom provider id misses (`"<type>:<model>"`), fixing cost tracking for
  proxies.
- Replace the hardcoded default model (`HaiCodeApp.cpp`, `claude-opus-4-5`)
  with the first model the configured provider lists.
- Count summarizer, title, and vision-fallback usage in session cost, or
  label them as excluded.

### Task 27. ChatGPT (Codex OAuth) provider

- The provider reuses the Codex CLI's OAuth `client_id` and sends
  `originator: codex_cli_rs` to a private backend. Mark it "Experimental" in
  the UI and docs with a terms-of-use note, or remove it from 1.0.
- When it is configured but not signed in, it disappears from the provider
  menu silently. Show it disabled with "(sign in via Settings)".

---

## Phase 8 — Smaller fixes

- Remove debug `fprintf` calls from `MainWindow::_HandleTodosUpdated`
  (~2203/2208) and audit other GUI stderr noise.
- Validate `argv[1]` in the `HaiCodeApp` constructor (exists and is a
  directory); show an error otherwise.
- Look up `AGENTS.md`/`agents.md` and `CLAUDE.md`/`claude.md`
  case-insensitively (BFS is case-sensitive).
- Don't create `<project>/.haicode/plans` just because a directory was opened
  (`ReadyToRun`); create it when a plan is written.
- Rename environment variables `HPCODE_DEBUG_PROMPT` and `HPCODE_SKILLS_DIR`
  to `HAICODE_*` (accept the old names for one release).
- Fix the stale comment in `HaiCodeApp.cpp` claiming Settings has no API-key
  field, and the `config.h` comment describing `default_mode` as plan/build
  only.

---

## Phase 9 — Release packaging and GUI polish

### Task 28. Application identity and packaging

- Complete the `.rdef` from Task 4: vector icon, version info
  (`app_version`), app flags.
- Add a single version constant (CMake `project(VERSION ...)` → generated
  header) used by an About window and a `--version` flag.
- Add `install()` rules and a HaikuPorts recipe / `.hpkg` build. Separate
  runtime from development dependencies.
- Verify install and first launch on a clean Haiku system.

### Task 29. GUI completeness

- Session list is capped at 50 (`store_.list(50)` in `MainWindow` and the
  Permissions center); add paging or search.
- First-run onboarding: provider setup, connection test, model selection.
- Session export (Markdown/JSON) and a documented backup/restore of
  `sessions.db`.
- Clear, categorized error messages (config, provider, database, tool);
  never include secrets.
- Localization via `B_TRANSLATE` catalogs; persist window frame; add Edit
  (copy/select all) and Help menus.
- If a plan exists in `.haicode/plans/` about permission popup placement
  (`PermissionMenu::ScreenLocation()` vs `BMenu` frame clamping), finish or
  close it; verify popup placement near screen edges in every mode.
- Performance check: open a 500+ message session and stream a long reply;
  make sure per-delta appends in `ChatView` are not quadratic.

---

## Phase 10 — Documentation

Update `README.md`, `CLAUDE.md`, and `agents.md` to match the code:

- Tool count is 21 (README says nineteen, `CLAUDE.md` says twenty); add
  `screenshot` to the system-prompt tool list in `default_prompt.h`.
- README: the agentic loop uses a renewable 50-step budget (not "up to 20");
  rewrite the auto-compaction section for non-destructive checkpoints and the
  real config keys (`compaction_buffer`, `compaction_recent_context`,
  `compaction_summary_max_tokens`); list all provider types (`anthropic`,
  `openai`, `chatgpt`, `ollama`, `vllm`, `openrouter`, `lmstudio`,
  `llamacpp`); add OpenSSL to the dependency table; replace the "early
  preview / beta5" status line.
- `lib/` is not "pure C++20 + POSIX" — it links `libbe` (Storage/Support kits:
  `BPath`, `find_directory`, `BUrl`). Correct the description.
- `CLAUDE.md` test table: add `test_codex_auth`, `test_openai_translate`,
  `test_text_attachment`, and every test added during this work.
- Document the project trust model, new permission defaults (`web_extract`,
  `screenshot`), and the bash/git rule-matching semantics.
