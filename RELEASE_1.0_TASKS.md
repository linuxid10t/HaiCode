# HaiCode 1.0 — Release Tasks and Status

One file, two purposes: the ordered task list that drove the 1.0 work, annotated
with per-task status and the code that proves it. Later tasks build on earlier
ones (e.g. clean shutdown depends on cancellable processes and HTTP; project
trust depends on layered config loading).

Verification audited against the working tree at `bc183cb` (2026-10-05). Every
status claim below was checked in code and tests, not read off commit messages.

## Status at a glance

Gate state: `make -C build -j$(nproc)` is clean under `-Wall -Wextra`, and
`ctest --test-dir build` reports **40/40 passing** (37 `lib/tests` + 3
`gui/tests`). Debug and Release trees both configured and green.

| Phase | Task | Status |
|---|---|---|
| 0 — Test gate | 1 CTest + build hardening | Done |
| 1 — Persistence | 2 SQLite error checking | Done |
| | 3 Sequencing + transactions | Done |
| | 4 Single instance | Done |
| 2 — Config & trust | 5 Atomic config writes | Done |
| | 6 Owner-only secrets | Done |
| | 7 Merge semantics | Done |
| | 8 No project→global leak | Done |
| | 9 Project trust boundary | Done |
| 3 — Cancellation | 10 Subprocess runner | Done |
| | 11 Cancellable HTTP | Done |
| | 12 Orderly shutdown | Done |
| 4 — Turn integrity | 13 Complete tool exchanges | Done |
| | 14 Prompts while running | Done |
| | 15 Engine correctness fixes | Done |
| | 16 Disruptive-change confirmation | Done |
| | 17 Safe session deletion | Done |
| 5 — Permissions | 18 Rules beat built-ins | Done |
| | 19 Web bounds, visible screenshots | Done (revised scope) |
| | 20 Rule matching, read-only git | Done |
| 6 — File system | 21 Attributes + symlinks | Done |
| | 22 Non-destructive diff | Done |
| 7 — Providers | 23 Anthropic | Done |
| | 24 OpenAI-compatible | Done |
| | 25 Output limits, retries, errors | Done (one optional item skipped) |
| | 26 Model metadata | Done |
| | 27 ChatGPT (Codex) provider | Done |
| 8 — Smaller fixes | All six items | Done |
| 9 — Packaging | 28 Identity + packaging | **Not started** |
| | 29 GUI completeness | **Partly done** (cleanup + popup placement done) |
| 10 — Documentation | Docs match code | **Done** (all stale statements closed) |

### Remaining work, in dependency order

1. **Version single-source + `.rdef` completion** — `project(VERSION …)` →
   generated header; `app_shortname`, `app_version`, vector icon; About window;
   `--version`. (Task 28)
2. **Session list beyond 50** — paging or search, replacing both `list(50)` call
   sites. (Task 29)
3. **Session sorting** — by day and by project: store query + sidebar UI.
   (Task 29)
4. **Install/packaging** — `install()` rules, HaikuPorts recipe / `.hpkg`,
   runtime vs development deps, clean-system install + first-launch check.
   (Task 28)
5. **First-run onboarding** — provider setup, connection test, model selection.
   (Task 29)
6. **Session export** (Markdown/JSON) + documented `sessions.db` backup/restore.
   (Task 29)
7. **Categorized, secret-free error presentation** in the GUI. (Task 29)
8. **Edit + Help menus, `B_TRANSLATE` catalogs, persisted window frame.**
   (Task 29)
9. **Performance pass** on a 500+ message session with a long streamed reply.
   (Task 29)
10. **Docs sweep** — the six stale statements listed under Phase 10.

One deliberate deviation to confirm: Task 25's optional "consider automatic
continuation" after a `max_tokens` stop shipped as a **notice** instead.

## Ground rules

- Read `CLAUDE.md` first and follow its build, test, and style conventions.
- HaiCode builds only on Haiku (C++20, GCC 13). Build with
  `cmake -B build -S . && make -C build -j$(nproc)` and run the relevant `test_*`
  binaries after every task. Do not launch a second GUI instance.
- One task per commit. Keep each change minimal and scoped to its task.
- Every behavior fix gets a regression test in `lib/tests/` where it is
  testable without the GUI. Add new test binaries to `lib/CMakeLists.txt`
  (and to CTest once Task 1 lands) and to the test table in `CLAUDE.md`.
- File and line references in the original specs are as of commit `7f634b3` and
  are approximate. Line references under **Verified** are as of `bc183cb`.
  Locate code by symbol, not line number.
- When a task changes documented behavior, update `CLAUDE.md`, `README.md`,
  and `agents.md` in the same commit.

---

## Phase 0 — Make the test suite a gate

### Task 1. Register tests with CTest and harden the build — **Done** (`52c56b6`)

Spec:

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
and Release. — met.

Verified:

- `enable_testing()` at `CMakeLists.txt:4`; 36 `add_test` entries in
  `lib/CMakeLists.txt:161-174`, 3 GUI tests at `gui/CMakeLists.txt:44,49,57`.
- `TEST_REQUIRE` macro in `lib/tests/test_check.h`; zero `assert(` left in
  `lib/tests/*.cpp`.
- `CONFIGURE_DEPENDS` at `lib/CMakeLists.txt:4` and `gui/CMakeLists.txt:3`; no
  "re-run cmake" caveat remains in any doc.
- Default `RelWithDebInfo` at `CMakeLists.txt:8-10`; `-Wall -Wextra` at `:12`.
- `FATAL_ERROR` guards at `CMakeLists.txt:89-97`, with `openssl3_devel`.
- `build-debug` (Debug) and `build-release` (Release) both exist; the Release
  run is 40/40 green.

Extras found along the way: a C++20 toolchain pre-flight check for hybrid
x86_gcc2 systems (`CMakeLists.txt:17-23`) and secondary-arch library path
detection (`CMakeLists.txt:32-70`).

---

## Phase 1 — Persistence foundation

### Task 2. Check every SQLite call and surface failures — **Done** (`7142af3`)

Spec:

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

Verified:

- `DbStmt` RAII wrapper at every prepare/step site in `db.cpp`; `expect_row()`
  and `pragma()` helpers.
- `PRAGMA busy_timeout` 5000 ms on open (`db.cpp:81`), `set_busy_timeout()`
  (`db.cpp:132`), WAL + `foreign_keys=ON` (`db.cpp:86-90`).
- Numbered migrations keyed on `user_version` (`db.cpp:121-190`): v1
  `tok_last_input`, **v2 drops the `permission` table** (`db.cpp:160-162`), v3
  `project_used` backfill.
- `DbError` thrown from writes; `run_turn_barried` maps it to `StepFailed`
  (`engine.h:327`, `engine.cpp:1006,1070,1219,…`); GUI renders it
  (`MainWindow.cpp:2323`).
- Tests: `test_db_upgrade`, `test_db`.

### Task 3. Make message sequencing and transactions safe under concurrency — **Done** (`355d46f`)

Spec:

- `append_message` computes `next_seq()` and then inserts as two separate
  steps. Concurrent writers to one session (GUI thread + runner thread) collide
  on `UNIQUE(session_id, seq)` and the row is silently lost. Make the
  read-and-insert atomic (single `INSERT ... SELECT COALESCE(MAX(seq),0)+1`
  or a per-store mutex around the pair).
- `replace_todos` runs `BEGIN` … `COMMIT` on the one connection shared by all
  sessions (`db.cpp` ~734/769). A second thread's `BEGIN` fails and its writes get
  absorbed into the first transaction. Serialize whole transactions with a
  store-level mutex (or a dedicated DB worker thread), check `COMMIT`, and
  `ROLLBACK` on any failure.
- Test: two threads appending to the same session and two sessions replacing
  todos concurrently, looped; no lost rows, no duplicate seqs.

Verified:

- Single-statement `INSERT … SELECT COALESCE(MAX(seq),0)+1` (`db.cpp:685-688`).
- `conn_mu_` is the connection's own mutex and is held across whole
  transactions with checked `COMMIT`/`ROLLBACK` (`db.cpp:298,901-904`).
- `test_db_concurrency`: 2 threads × 50 appends looped 20×, 50× concurrent todo
  replaces across two sessions, and a 100×100 cross-shape race.

### Task 4. Enforce a single running instance — **Done** (`d0ab151`)

Spec:

- Add an application resource file (`.rdef`) with signature
  `application/x-vnd.haicode` and `B_SINGLE_LAUNCH`, wired into the GUI build
  (full resource work is Task 28; the launch flag is needed now).
- Forward a second launch's project-directory argument to the running instance
  (`ArgvReceived`/`RefsReceived`) instead of starting a new one.
- Remove the "single instance only" warning from the README once enforced.

Verified:

- `gui/haicode.rdef` (signature + `B_SINGLE_LAUNCH`), compiled with `rc` in
  `gui/CMakeLists.txt:19-22`.
- `HaiCodeApp::ArgvReceived` / `RefsReceived` (`HaiCodeApp.cpp:305,329`) forward
  to `MSG_DIR_PROPOSED`.
- README states it as enforced behavior; the warning is gone.

---

## Phase 2 — Configuration correctness and trust

### Task 5. Make config writes atomic and non-destructive — **Done** (`21d0454`)

Spec:

- `HaiCodeApp.cpp` rewrites `config.json` with a truncating `std::ofstream` in
  several handlers (`MSG_PERSIST_PM`, `MSG_DIR_CHANGED`,
  `MSG_PROVIDERS_UPDATED`, `MSG_SETTINGS_SAVED`). A parse failure is swallowed,
  so saving after a hand-edit typo replaces the whole file with a few keys
  (API keys and providers lost).
- Centralize all config updates in one library function: read, refuse to write
  if the existing file does not parse (show the error to the user), apply the
  change, write through `util::atomic_write_file`.
- Report write failures to the user.

Verified:

- `update_config_file(path, mutate, err)` (`config.cpp:692+`): refuses an
  unparseable document, preserves unrelated/unknown keys, writes atomically.
- GUI wrapper `save_config_at()` (`HaiCodeApp.cpp:54`) surfaces failures in a
  `BAlert`; no truncating config `ofstream` remains.
- Test: `test_config_write`.

### Task 6. Store secrets with owner-only permissions — **Done** (`bf63af9`)

Spec:

- `util::atomic_write_file` creates new files as 0644 (`util.cpp` ~186). Add a
  mode parameter; write `openai-auth.json` (OAuth tokens, `codex_auth_save`) and
  `config.json` (API keys) as 0600.
- On startup, tighten existing files in the settings `haicode` directory to 0600
  if they are group/world readable.
- `save_permission_document` uses a fixed `.perm-tmp` name without fsync; switch
  it to `atomic_write_file` too.

Verified:

- `atomic_write_file(path, content, mode)` (`util.cpp:210-251`); an explicit
  mode beats preserved bits so secrets always tighten.
- 0600 for `openai-auth.json` (`codex_auth.cpp:440`) and config (`config.cpp:774`).
- Startup tightening (`HaiCodeApp.cpp:174-184`) via `util::ensure_owner_only`
  (`util.cpp:300-306`).
- `save_permission_document` goes through `atomic_write_file`
  (`config.cpp:716-722`) with fingerprint conflict detection.
- Tests: `test_file_tools` (mode matrix), `test_config_permission` (policy
  documents).

### Task 7. Fix config merge semantics — **Done** (`f970109`)

Spec:

`ConfigLoader::merge` uses struct default values as "not specified":

- `default_mode` defaults to `"plan"`, so an absent project config always
  overwrites a global `"build"` — the setting is lost on every relaunch.
- `web_search_max_results` defaults to `5`, so the global value is always
  overwritten.
- `providers` entries are replaced wholesale, dropping the global `api_key`.
- `auto_compact` / autoname booleans can only be turned off by an overlay,
  never back on; numeric overrides equal to the default are ignored.

Fix by parsing into a layer with `std::optional` fields (or tracking key
presence) and applying defaults only after merging. Per-key merge for provider
entries. Extend `test_config_permission` for every case above. Remove the unused
`auto_compact_reserve`, `MCPServerConfig`/`mcp`, and `ProviderConfig::env`
fields, or implement them.

Verified:

- `ConfigLayer` = values + a `present` key set; `merge()` applies an overlay only
  for keys actually present (`config.h:133-148`). Explicit values equal to struct
  defaults still override, so booleans can be re-enabled.
- Provider entries merge per subkey (`config.h:135-136`).
- `auto_compact_reserve`, `MCPServerConfig`/`mcp`, `ProviderConfig::env` removed
  (documented in `CLAUDE.md:209`).
- Test: `test_config_permission` "Task 7 regressions".

### Task 8. Stop project settings leaking into the global config — **Done** (`97c5677`)

Spec:

- `config_` in `HaiCodeApp` is the merged global+project config, and the Settings
  save writes all of it into the global file: `build_command`, `providers`,
  `skills`, `models`, `vision`. Opening project A and saving Settings makes A's
  build hook run in every project.
- Keep the global and project layers separate in memory. The Settings window
  edits only the global layer; project-only fields (`build_command`) are edited
  as project settings and written to `<project>/.haicode/config.json`.

Verified:

- `HaiCodeApp::global_layer_` kept separate from merged `config_`
  (`HaiCodeApp.cpp:479-480`, `_SyncMergedConfig`).
- Settings saves patch only `global_scope_keys()` via `sync_global_scope()`;
  `build_command` is written to the project file (`HaiCodeApp.cpp:794-800`).

### Task 9. Add a trust boundary for project configuration — **Done** (`2ba48fe`)

Spec:

A repository's `.haicode/config.json` is currently fully trusted. A hostile repo
can:

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
- Trust prompt when a project with such keys is opened, listing exactly what it
  would enable. Re-prompt if those keys change.
- Tests: an untrusted project config cannot change provider URLs, add Allow
  rules, or set a build hook.

Verified:

- Project `providers` are never merged; gated keys are stripped unless a
  fingerprint-matched record exists in the global `"trusted_projects"` map
  (`config.h:179-236`, `load_project_layer`, `store_trust_record`).
- `HaiCodeApp::_MaybePromptProjectTrust` prompts with fail-closed default;
  gated-content changes re-prompt, cosmetic reformatting does not.
- Test: `test_config_trust`.

---

## Phase 3 — Cancellable execution

### Task 10. One cancellable subprocess runner for all tools — **Done** (`0000f23`)

Spec:

- `BashTool` (`tools.cpp` ~262) uses `popen` + `timeout`: it ignores
  `ctx.interrupt`, has no cap on the model-supplied timeout, and a backgrounded
  child (`server &`) keeps the pipe open so the call never returns.
- `git`, `grep`, `find`, `process`, and `screenshot` use `popen` with no timeout
  or interrupt at all; `git push/pull/fetch` can hang forever on a credential
  prompt.
- When `read_pipe` stops at 100 KB, `pclose` kills the child with SIGPIPE and the
  tool reports exit code -1 — a truncated success is shown as a failure.
- Generalize `detail::run_build_hook` (own process group, poll loop, kill group
  on interrupt/timeout, output cap) into a shared runner. Keep draining (and
  discarding) output past the cap; report truncation separately from exit status.
  Close inherited file descriptors in the child (or use `O_CLOEXEC`) and give the
  child `/dev/null` as stdin.
- Cap the bash timeout (e.g. 600 s). Set `GIT_TERMINAL_PROMPT=0` for git.
- Move all tools onto the runner and pass `ctx.interrupt` through.
- Tests: interrupt kills a `sleep 60`; `sleep 60 &` returns promptly; >100 KB
  output reports success plus a truncation marker.

Verified:

- `util::run_subprocess` (`lib/src/util/subprocess.cpp`, contract documented in
  `lib/include/haicode/subprocess.h`): `setpgid` process group, `/dev/null`
  stdin, fds ≥3 closed after the dup2s, 100 ms poll loop, group kill on
  interrupt/timeout, drains past the cap and sets `truncated` independently of
  `exit_code`, ~500 ms bounded orphan grace so `server &` returns promptly.
- **No `popen` remains in `lib/` or `gui/`.** bash (`tools.cpp:257`), grep
  (`:595`), git (`:1468`), find (`:1542`), process (`:2010,2050`), screenshot
  (`:2137`), and the build hook (`engine.cpp:43`) all pass `ctx.interrupt`.
- Bash timeout clamped to 600 s (`tools.cpp:253-255`); `GIT_TERMINAL_PROMPT=0`
  (`tools.cpp:1445`).
- Test: `test_subprocess`.

### Task 11. Make HTTP cancellable and bounded — **Done** (`7236bf8`, `2ea345f`)

Spec:

In `lib/src/util/util.cpp`:

- Cancellation only takes effect when data arrives; a stalled connection ignores
  it until the 300 s total timeout. Add `CURLOPT_XFERINFOFUNCTION` checking the
  cancel flag.
- Replace the fixed 300 s `CURLOPT_TIMEOUT` on streams with
  `CURLOPT_CONNECTTIMEOUT` plus `CURLOPT_LOW_SPEED_LIMIT`/`LOW_SPEED_TIME`, so
  long-but-live generations (slow local models) are not cut off.
- Cap response bodies in `get()`/`post_json()` and the SSE line buffer.
- `CURLOPT_FOLLOWLOCATION` is on for credential-bearing requests; curl strips
  only `Authorization`/cookies on a cross-host redirect, so `x-api-key` is
  forwarded. Disable redirects for provider API calls (or restrict to same-host)
  and set `CURLOPT_PROTOCOLS`/`REDIR_PROTOCOLS` to HTTP(S).
- Set `CURLOPT_NOSIGNAL`. Call `curl_global_init` once at startup, not in every
  `HttpClient` constructor.
- SSE parser: join multiple `data:` lines with `\n` per the SSE spec.
- Tests (extend `test_http_client`): cancel during connect and during a silent
  stream returns within ~1 s; oversized body is truncated safely.

Verified:

- `CURLOPT_XFERINFOFUNCTION` (`util.cpp:617`); `CONNECTTIMEOUT` +
  `LOW_SPEED_LIMIT/TIME` (`util.cpp:628-632`) with per-flavor stall limits
  (local servers 30 min, generic 60 s).
- 10 MB body cap and 2 MB SSE buffer cap (`util.cpp:375-379,684-695`) with
  `truncated` out-params.
- `FOLLOWLOCATION=0` on provider calls plus a manual **same-host-only** hop loop
  and `REDIR_PROTOCOLS(_STR)` pinned to http/https (`util.cpp:735-762,827`).
- `CURLOPT_NOSIGNAL` set; `curl_global_init` once through `std::call_once`
  (`util.cpp:388`).
- Consecutive `data:` lines joined with `\n` (`util.cpp:493-500`).
- Test: `test_http_client` (Task 11 + stall cases).

### Task 12. Orderly shutdown instead of `std::exit` — **Done** (`8ba368c`)

Spec:

- `HaiCodeApp::QuitRequested` calls `std::exit(0)`, running static destructors
  while engine threads are live.
- With Tasks 10–11 in place: ask for confirmation if any session is running; then
  cancel pending asks and approvals, interrupt all sessions, call
  `SessionEngine::shutdown()` with a bounded wait, close the database, and quit.
  Fall back to `_exit` only after the timeout.

Verified:

- `MainWindow::QuitRequested` confirms when sessions are running
  (`MainWindow.cpp:884-900`), Cancel/Escape default.
- `HaiCodeApp` cancels asks/approvals, interrupts, calls `engine_->shutdown()`,
  closes the DB, with `_exit(0)` only as a >10 s fallback
  (`HaiCodeApp.cpp:342-368`). No `std::exit(0)` in the quit path.

---

## Phase 4 — Turn integrity in the agentic loop

### Task 13. Never leave a tool call without a result — **Done** (`167fe47`)

Spec:

- In `SessionEngine::agentic_loop` the tool loop breaks early on a denied call
  (`if (result.denied) { any_denied = true; break; }`), on a successful
  `propose_plan`, and on interrupt. Remaining calls from the same assistant turn
  get no `tool_result` row. Both Anthropic and OpenAI reject every later request
  containing an unanswered `tool_use`, so the session is permanently broken.
- Persist a "not run: <reason>" `tool_result` for every skipped call.
- Add a repair pass in context assembly (`assemble_messages` or
  `load_context_messages`) that synthesizes a result for any unanswered
  `tool_use`, so existing broken sessions recover.
- Emit all `tool_result` blocks for one assistant turn in a single user message
  (currently one user message per result).
- Tests: deny the first of two parallel calls, then send another prompt — the next
  request is well-formed.

Verified:

- The loop no longer breaks: once `stopped_reason` is set, every remaining call
  persists `{success:false, output:"not run: <reason>"}` (`engine.cpp:2709-2740`).
- Repair pass in `ContextBuilder::assemble_messages` synthesizes missing results,
  keeps the late real result, drops duplicates and orphans, and emits one grouped
  user message with `is_error` (`engine.cpp:798-849`).
- Test: `test_turn_integrity` — Anthropic grouping and OpenAI translated
  call/response matching checked for the whole history.

### Task 14. Handle prompts submitted while a turn is running — **Done** (`b304f2a`)

Spec:

- `MainWindow::_SubmitPrompt` does not check whether the session is running. A
  prompt submitted during tool execution or a permission prompt is stored between
  the `tool_use` row and its `tool_result`; the API rejects the next request.
- `submit_prompt` stores the prompt but spawns a runner only when
  `session_running_` is false. The flag stays true (a) after `interrupt()` while
  the loop winds down — the GUI already shows idle — and (b) during
  `refine_title_llm` after the final step. A prompt sent in either window is
  never processed.
- Fix: queue prompts that arrive while running and append them only at a safe
  boundary (after all tool results of the current step); when the runner exits,
  re-check for unprocessed prompts and continue. Clear the running flag before
  title refinement, or run refinement after the flag is cleared.
- Skip `refine_title_llm` when the run was interrupted, and pass the run's stream
  token so it is cancellable.
- Tests: submit during a parked tool, submit right after interrupt, submit right
  after the final step — each prompt gets exactly one response.

Verified:

- Per-session FIFO `prompt_queue_`; queued prompts are not persisted until their
  turn starts, drain only after a complete tool exchange, and are re-checked at
  runner exit so a racing submission cannot strand (`engine.h:375-390`).
- Running flag flips atomically with the queue check; title refinement runs on
  separate tracked maintenance workers with scoped cancellation and is skipped on
  interrupt (`engine.h:321-460`).
- Test: `test_turn_integrity` (parked tool, post-interrupt, final-step and
  runner-exit races, concurrent sessions, title maintenance independence).

### Task 15. Smaller engine correctness fixes — **Done** (`2b176e0`)

Spec:

- Both request rebuilds after compaction (`engine.cpp` ~1550 and ~1697) call
  `builder.build(...)` without `model_accepts_images`, so text-only models
  receive raw images. Pass `primary_supports_vision` at both sites.
- `agentic_loop` reads `interrupt_flags_[session_id]` without holding `mu_`
  (~1213) while other threads insert into the map. Read it under the lock.
- `backfill_attachment_descriptions`: pass the run token to `describe_image` and
  stop retrying a failed description on every step.
- Mirror the GUI's 4 MB image cap in `submit_prompt` (text attachments are already
  clamped engine-side).
- `create_session` falls back to `"anthropic"` even if it is not registered; fall
  back to the first registered provider.
- Old screenshot/image attachments from past turns are resent in full every
  request; replace them with a placeholder after N turns (as with old tool output
  truncation).
- `assemble_messages` silently drops rows that throw during parsing; log them.

Verified:

- One request-building path, `build_step_request(StepRequestShape…)`, carries
  `accepts_images = primary_supports_vision` plus inference overrides and cache
  key for the loop and every compaction rebuild (`engine.cpp:337-363,2226-2238`).
- `interrupt_flags_` reads are under `mu_` (`engine.cpp:1311,1472,1560,1727,1915,3584`)
  or inside `spawn_runner_locked`, whose contract requires the caller to hold
  `mu_` (`engine.cpp:1772-1784`).
- `backfill_attachment_descriptions` takes the run's `stream_token`, registers the
  fallback provider for cancellation, and records `description_status:"failed"` so
  failures are not retried each step (`engine.cpp:3699-3760`).
- Engine-side 4 MiB image cap with explicit unavailable markers
  (`engine.cpp:469-478`).
- `create_session`: anthropic → openai → first registered provider, with a loud
  failure when none exists (`engine.cpp:979-994`).
- Older images become text placeholders via `recent_image_boundary`
  (`engine.cpp:533,607,774`).
- Malformed rows logged by `log_row_parse_failure` with metadata only, never
  payloads (`engine.cpp:795-797`).
- Test: `test_engine_correctness`.

### Task 16. Don't silently interrupt running sessions on settings changes — **Done** (`6564e94`)

Spec:

- Saving Settings, changing providers, or changing directory calls
  `_RecreateEngine`, which shuts down the engine and interrupts every running
  session. Warn and ask first, or defer the recreate until runs finish.

Verified:

- `_ConfirmDisruptiveChange(...)` gates provider updates, settings saves, and
  directory switches (`HaiCodeApp.cpp:461,556`); it lists every running session
  including background ones, defaults to Cancel, and directory changes are
  proposed (`MSG_DIR_PROPOSED` → `MSG_DIR_APPLY`) rather than applied.

### Task 17. Safe session deletion — **Done** (`72d376d`)

Spec:

- `MSG_DELETE_SESSION` deletes immediately with no confirmation and while the
  session may be running.
- Confirm with a `BAlert`. If running: interrupt, wait for the runner, then
  delete. Clear the session's gate rules, grants, pending approvals, and engine
  maps (add an engine API for this).

Verified:

- Confirmation `BAlert`, deletion on tracked workers
  (`MSG_DELETE_SESSION_CONFIRMED`, `HaiCodeApp.cpp:565-573`).
- `SessionEngine::delete_session` (`engine.cpp:1444-1545`): marks retiring
  (spawns refused, queue discarded), cancels asks/approvals/streams, joins the
  runner and title workers **outside** engine locks, deletes rows last, and rolls
  retirement back on DB failure.
- `PermissionGate::erase_session_state` clears only that session's gate layers
  (`permission.cpp:187`, `engine.cpp:1576`).
- Test: `test_session_lifecycle`.

---

## Phase 5 — Permissions hardening

### Task 18. Let configured Deny rules override built-in exemptions — **Done** (`ba0050d`)

Spec:

- `ToolRegistry::evaluate` returns Allow for `web_search`, `web_extract`,
  `screenshot`, `process list/check_port`, read-only `git`, and in-project reads
  before consulting any rule. A Deny written in the Policies tab is accepted and
  silently has no effect.
- Check explicit configured/session Deny rules first; built-in exemptions become
  the fallback. Keep mode restrictions above everything.
- The operation inspector must show the new decision path. Update tests in
  `test_config_permission`.

Verified:

- Built-in exemptions are computed as a fallback and reported with source
  `builtin` only when no rule layer matched (`permission.cpp:705-774`); explicit
  Deny/Ask from any layer wins, mode restrictions stay above everything.
- A matched Ask rule is reported with its layer source instead of the anonymous
  `"prompt"` (`permission.cpp:228-246`).
- Inspector shows decision source and rule index (`PermissionsCenterWindow.cpp:190-247`).
- Test: `test_config_permission` "Task 18" block.

### Task 19. Bound web tools and make screenshots visible — **Done** (`6a0a4b1`, scope revised by `ca5d592`)

Spec (as revised):

`web_search` and `web_extract` stay allowed without prompting, including
`localhost` and private-network URLs (fetching a local dev server is normal agent
work). Do not add per-host prompts or address blocking. Users who want web access
gated can add an Ask or Deny rule in the Policies tab, which Task 18 makes
effective.

- Cap `web_extract`'s `max_chars` (e.g. 100 KB) and its download size (the body
  cap from Task 11).
- `screenshot` stays allowed. Add a visible transcript line each time a screenshot
  is captured so the user always knows one was sent to the provider.
- Document in the permission docs that the web tools and `screenshot` are allowed
  by default and how to gate them with policy rules.

Verified:

- `max_chars` clamped to the 100 KB shared budget (`web_tools.cpp:802-806,838`);
  download bounded by the Task 11 cap with `truncated` surfaced.
- Screenshot captures emit a visible transcript line (`MainWindow.cpp:2155-2157`).
- Documented in `docs/permissions.md:49-58,71,96`.
- Test: `test_web_tools`.

### Task 20. Tighten rule matching and read-only git — **Done** (`fe4989c`, `de031c0`)

Spec:

- `fnmatch` rules match the whole bash command string, so `make*` also allows
  `make; rm -rf ~`. Split commands on `;`, `&&`, `||`, `|`, newlines, and command
  substitution; require every segment to match, or refuse pattern Allow rules for
  bash and warn in `PermissionRuleEditWindow`.
- Read-only git invocations run without a prompt but honor repo-local config that
  can execute programs (`core.fsmonitor`, `diff.external`, textconv filters).
  Invoke read-only git with `-c core.fsmonitor= -c core.hooksPath=/dev/null
  --no-ext-diff --no-textconv`.
- Show the git rule-granularity limitation (an Allow for `branch` also covers
  `branch -D`) in the rule editor, or match on subcommand+first argument.

Verified:

- `bash_pattern_authorizes` splits quote-aware at `;`, `&&`, `||`, `|`, newline,
  and `&`; every segment must match and command substitution never matches a
  pattern (`permission.cpp:522-540+`). Deny patterns stay whole-string.
- Read-only git hardened beyond the spec: `-c core.fsmonitor= -c core.hooksPath=
  /dev/null -c gpg.program=false -c gpg.ssh.program=false -c gpg.x509.program=
  false -c log.showSignature=false`, `--no-ext-diff`/`--no-textconv` (also for
  `blame` and `stash list/show`), `GIT_OPTIONAL_LOCKS=0`, and the classifier
  refuses re-enabling flags so model args cannot undo them (`tools.cpp:1420-1466`).
- Rule editor states the granularity limit (`branch` also covers `branch -D`) —
  `PermissionRuleEditWindow.cpp:51-52`.
- Test: `test_config_permission` Task 20 + hostile-repo e2e (`de031c0`).

---

## Phase 6 — File-system correctness on Haiku

### Task 21. Preserve attributes and symlinks on write — **Done** (`4169ca9`)

Spec:

- `util::atomic_write_file` renames a new `mkstemp` file over the target, dropping
  BFS attributes (`BEOS:TYPE`, Tracker metadata), and replaces a symlink with a
  regular file.
- Resolve symlinks before writing (write to the target). Copy all attributes from
  the old file to the temp file (`fs_open_attr_dir`/`fs_read_attr`/`fs_write_attr`)
  before the rename. Tests for both.

Verified:

- `copy_file_attributes` copies every BFS attribute onto the temp fd before the
  rename, with a 1 MiB sanity cap (`util.cpp:176-207`).
- Symlinks resolved first (`lstat`/`S_ISLNK`, `util.cpp:218-226`); broken links
  refused; mode preservation with explicit-mode precedence.
- Test: `test_file_tools` (`BEOS:TYPE` survives, writes land in the link target,
  0755 preserved).

### Task 22. Keep `diff` non-destructive — **Done** (`70207fb`)

Spec:

- `DiffTool` creates its scratch file beside the target
  (`<path>.tmp_diff_XXXXXX`). It is allowed in Plan mode and fails for read-only
  locations such as the system headers. Create the scratch file in the system temp
  directory instead.

Verified:

- Scratch created in `B_SYSTEM_TEMP_DIRECTORY` (`tools.cpp:1309-1317`), prefixed
  `haicode_diff_` and swept by housekeeping; screenshot temps use the same
  directory (`tools.cpp:2120`).
- Tests: `test_diff`, `test_file_tools` (read-only packagefs target works, no
  leftovers).

---

## Phase 7 — Provider and model correctness

> Before changing request parameters, verify them against each provider's current
> API documentation. Parameters differ per model; drive them from a per-model
> capability table rather than hardcoding one behavior.

### Task 23. Anthropic provider — **Done** (`7b14e64`, refined by `5b26d71`)

Spec (`lib/src/provider/anthropic.cpp`):

- `reasoning_effort == "off"` is sent as `output_config.effort: "off"`, which is
  not a valid effort value (valid: `low`, `medium`, `high`, `xhigh`, `max`) —
  every request fails. Map "Off" to the lowest supported effort, or to a
  thinking-disabled setting only on models that accept one.
- No `thinking` parameter is sent. Send `thinking: {type: "adaptive"}` where
  supported and `display: "summarized"` so the reasoning panel shows text
  (current models default to omitted thinking text).
- Capture thinking blocks (including signatures) and replay them unchanged in
  assistant turns during tool-use loops.
- Update the effort/"Off" choices in the Inference UI to match.

Verified:

- Capability table in `lib/src/provider/anthropic_params.cpp`: "off" is never sent
  verbatim — `thinking:{type:"disabled"}` (or `between_tools` on Sonnet 5.5) with
  effort omitted, or `low` on models that cannot disable thinking; models without
  effort support omit the parameter.
- `thinking:{type:"adaptive", display:"summarized"}` sent on adaptive models.
- Thinking blocks captured with signatures (`anthropic.cpp:124-164`) and replayed
  verbatim ahead of `tool_use` in `assemble_messages`.
- Inference UI offers Off/Low/Medium/High/XHigh/Max (`MainWindow.cpp:3345-3351`).
- Test: `test_provider_params`.

### Task 24. OpenAI-compatible provider — **Done** (`926e5cc`, `1e5d99d`, `5a50599`)

Spec (`lib/src/provider/openai.cpp`):

- `chat_template_kwargs` and `reasoning_effort: "off"` are sent to real OpenAI,
  which rejects unknown parameters. Send `chat_template_kwargs` only to flavored
  local servers (vLLM, llama.cpp, LM Studio, Ollama); map "off" to a valid value
  per provider.
- Reasoning models require `max_completion_tokens` instead of `max_tokens` and
  reject `temperature`/`top_p`.
- Parse cached-token usage (`prompt_tokens_details.cached_tokens`).

Verified:

- `lib/src/provider/openai_params.cpp`: `chat_template_kwargs` only for flavored
  local servers; per-flavor "off" mapping (`none`, `enable_thinking:false`,
  llama.cpp template fixups); reasoning models get `max_completion_tokens` and
  omit `temperature`/`top_p` (`openai.cpp:547-640`).
- `prompt_tokens_details.cached_tokens` → `cache_read`.
- Tests: `test_provider_params`, `test_openai_translate`.

### Task 25. Output limits, retries, and error handling — **Done** (`a329125`)

Spec:

- `kDefaultMaxTokens = 8192` is too small once thinking counts against it and
  truncates large `write` inputs. Raise it for streaming requests (e.g. 32k+,
  bounded by the model's output cap).
- A `MaxTokens` stop ends the turn silently. Publish a clear notice (and consider
  automatic continuation).
- Add 429/529/"rate limit"/"too many requests" to the transient-error list with
  exponential backoff that honors `Retry-After`.
- The overflow matcher (`"context"`, `"exceeds"`) is broad enough to trigger
  compaction on unrelated errors; match provider-specific overflow codes.

Verified:

- `kDefaultMaxTokens = 32768` (`provider.h:25`) with per-model clamping
  `clamp_max_tokens` so small-output models cannot 400 (`test_pricing`).
- Truncation announced in the transcript ("Output truncated at the model's
  max_tokens limit — raise it in …", `engine.cpp:2284`) and reported as
  `finish_reason:"max_tokens"` on `StepEnded` (`engine.cpp:2688`).
- 429/529/rate-limit text classified transient (`provider_error.cpp:38`) with
  0.5/2/8 s backoff honoring seconds-form `Retry-After`
  (`engine.cpp:2476-2486`, `util.cpp:422-445,672-676`).
- Overflow matcher replaced with provider-specific codes and false-positive
  guards (`provider_error.cpp:21-29`).
- **Not implemented:** automatic continuation after a `MaxTokens` stop (the spec
  said "consider"; a notice shipped). Confirm or drop.

### Task 26. Model metadata tables — **Done** (`ba208b7`, `0a37ef2`, `18c6b9a`)

Spec:

- `lib/src/util/model_info.cpp`: current Anthropic models have 1M-token windows
  but match 200K entries or nothing, and the Anthropic provider has no
  `get_model_context`, so auto-compaction is off by default for current models.
  Implement discovery from the Anthropic Models API (`max_input_tokens`), then
  refresh the table.
- `lib/src/pricing/pricing.cpp`: prices are stale (Anthropic Opus entries,
  `openai:gpt-5`). Refresh them, and fall back to the provider *type* when a
  custom provider id misses (`"<type>:<model>"`), fixing cost tracking for proxies.
- Replace the hardcoded default model (`HaiCodeApp.cpp`, `claude-opus-4-5`) with
  the first model the configured provider lists.
- Count summarizer, title, and vision-fallback usage in session cost, or label
  them as excluded.

Verified:

- `model_info.cpp` refreshed with per-version windows (opus-4-6+ = 1M, opus-4-5 =
  200K, gpt-5 = 400K, …) and max-output caps.
- Anthropic implements `get_model_context` (`anthropic.cpp:330`) with Models-API
  discovery of `max_input_tokens` (`anthropic.cpp:318-320`), so auto-compaction is
  enabled for current models.
- Pricing refreshed with tier ladders; lookups keyed on **provider kind**
  (`pricing.cpp:346-426`), so custom-id proxies price correctly and local server
  kinds price as free.
- No hardcoded default model remains in the GUI — `default_model_` comes from the
  fetched provider model list (`MainWindow.cpp:1401-1428`); Settings → Model
  Database (`ModelDatabaseWindow.cpp`) lets users correct metadata.
- Summarizer, title, and vision-fallback usage counted with `sets_context=false`
  (`engine.cpp:3226,3495,3675`).
- Tests: `test_pricing`, `test_model_context`, `test_model_db`.
- Residual: user pricing overrides are still keyed by provider id/kind/model, so
  the "until pricing is keyed on type" note is satisfied for built-ins only.

### Task 27. ChatGPT (Codex OAuth) provider — **Done** (`a5d53e5`)

Spec:

- The provider reuses the Codex CLI's OAuth `client_id` and sends `originator:
  codex_cli_rs` to a private backend. Mark it "Experimental" in the UI and docs
  with a terms-of-use note, or remove it from 1.0.
- When it is configured but not signed in, it disappears from the provider menu
  silently. Show it disabled with "(sign in via Settings)".

Verified:

- Settings labels it "ChatGPT (Experimental)" with a terms-of-use warning
  (`SettingsWindow.cpp:73,123`).
- Unsigned-in entries stay visible but disabled with "(sign in via Settings)"
  (`MainWindow.cpp:911-925`); menu items carry `provider_id`, never a
  label-derived value.

---

## Phase 8 — Smaller fixes — **All Done**

Spec:

- Remove debug `fprintf` calls from `MainWindow::_HandleTodosUpdated`
  (~2203/2208) and audit other GUI stderr noise. — **Done** (`89e1b56`): only 5
  intentional diagnostics remain (`HaiCodeApp.cpp:950,953,999,1001`,
  `MainWindow.cpp:326`).
- Validate `argv[1]` in the `HaiCodeApp` constructor (exists and is a directory);
  show an error otherwise. — **Done** (`7680976`): `BEntry` check +
  `startup_arg_warning_` (`HaiCodeApp.cpp:104-110`), also enforced in
  `ArgvReceived`.
- Look up `AGENTS.md`/`agents.md` and `CLAUDE.md`/`claude.md` case-insensitively
  (BFS is case-sensitive). — **Done** (`8eabe11`): `config.cpp:58-123`,
  `agents.md` wins.
- Don't create `<project>/.haicode/plans` just because a directory was opened
  (`ReadyToRun`); create it when a plan is written. — **Done** (`7680976`):
  created on write only (`engine.cpp:2967`); `test_propose_plan`.
- Rename environment variables `HPCODE_DEBUG_PROMPT` and `HPCODE_SKILLS_DIR` to
  `HAICODE_*` (accept the old names for one release). — **Done** (`20319a5`):
  `engine.cpp:2041-2043`, `skills.cpp:47-51`.
- Fix the stale comment in `HaiCodeApp.cpp` claiming Settings has no API-key
  field, and the `config.h` comment describing `default_mode` as plan/build only.
  — **Done** (`421f222`): `config.h:83-86` corrected; the HaiCodeApp comment no
  longer exists (verified by grep).

---

## Phase 9 — Release packaging and GUI polish

### Task 28. Application identity and packaging — **Not started**

Spec:

- Complete the `.rdef` from Task 4: vector icon, version info (`app_version`),
  app flags.
- Add a single version constant (CMake `project(VERSION ...)` → generated header)
  used by an About window and a `--version` flag.
- Add `install()` rules and a HaikuPorts recipe / `.hpkg` build. Separate runtime
  from development dependencies.
- Verify install and first launch on a clean Haiku system.

Current state: `gui/haicode.rdef` holds only `app_signature` and `app_flags`.
Missing: `app_shortname`, `app_version`/copyright, vector icon, a version constant
(root `CMakeLists.txt` has `project(haicode CXX)` with no `VERSION` and no
generated header), an About window, `--version`, any `install()` rule, a
HaikuPorts recipe or `.hpkg` build, and a clean-system install/first-launch check.

### Task 29. GUI completeness — **Partly done**

Spec:

- Session list is capped at 50 (`store_.list(50)` in `MainWindow` and the
  Permissions center); add paging or search.
- ~~Finish implementing session cleanup.~~ **Done** (`ce49233`): File → Cleanup
  offers Delete Empty / Inactive (7-30-90 days) / All Sessions in This Project,
  with explicit Cancel default and Escape, through
  `SessionEngine::delete_session`. Sidebar rows and confirmation previews show
  project directories. Storage maintenance is automatic at startup and after
  deletion, deferred until foreground and title work are idle, serialized with
  all store operations, and retried on failure. Orphans, stale checkpoint state,
  and aged scratch files are swept; WAL checkpoint + `VACUUM` + checkpoint
  reclaims at least 1 MiB of free pages. See `test_session_cleanup` and
  `test_session_lifecycle`.
- Session sorting. Options will be by day and by project.
- First-run onboarding: provider setup, connection test, model selection.
- Session export (Markdown/JSON) and a documented backup/restore of `sessions.db`.
- Clear, categorized error messages (config, provider, database, tool); never
  include secrets.
- Localization via `B_TRANSLATE` catalogs; persist window frame; add Edit
  (copy/select all) and Help menus.
- If a plan exists in `.haicode/plans/` about permission popup placement
  (`PermissionMenu::ScreenLocation()` vs `BMenu` frame clamping), finish or close
  it; verify popup placement near screen edges in every mode.
- Performance check: open a 500+ message session and stream a long reply; make
  sure per-delta appends in `ChatView` are not quadratic.

Done beyond the struck-through cleanup item:

- **Permission popup placement closed.**
  `.haicode/plans/plan_20260929_235058_2387.md` is marked `abandoned`; its
  successor `plan_20260930_002451_2bfb.md` is `implemented`.
  `PermissionMenu::ScreenLocation()` anchors to the closed field's outer bottom
  border and opens fully above the field only when the laid-out menu cannot fit
  below, using `BScreen` bounds (`MainWindow.cpp:85-116`).
- Related robustness: snapshot-based dropdowns (`SnapshotMenu`) so background
  model fetches never mutate items while a menu is being tracked
  (`test_snapshot_menu`).

Outstanding:

1. **Session list still capped at 50** — `store_.list(50)` at
   `MainWindow.cpp:1462` and `PermissionsCenterWindow.cpp:327`. No paging, no
   sidebar search box. (Bulk cleanup's `sessions_matching` is uncapped; the
   visible list is not.)
2. **Session sorting** — only `ORDER BY time_updated DESC` exists
   (`db.cpp:367,399,431`). No by-day or by-project grouping.
3. **First-run onboarding** — no provider setup flow, connection test, or
   model-selection guidance anywhere in `gui/`.
4. **Export and backup/restore** — no Markdown/JSON session export; no documented
   `sessions.db` backup/restore procedure.
5. **Categorized error messages** — provider errors are classified in
   `lib/src/util/provider_error.cpp` and config/housekeeping failures raise
   `BAlert`s, but the GUI shows generic `Error: <text>` transcript lines
   (`MainWindow.cpp:2337`) with no config/provider/database/tool categories.
6. **Localization / window persistence / Edit + Help menus** — zero `B_TRANSLATE`
   usage and no catalogs; window frame never persisted or restored; menu bar has
   File and Settings only (`MainWindow.cpp:550-590`).
7. **Performance check not run** — no 500+ message session stress or benchmark.
   `ChatView` appends each delta through `_AppendRendered` →
   `BTextView::Insert(TextLength(), …)` (`ChatView.cpp:412-460,591-599`), the
   documented risk area; `test_chat_view` proves identical rendering across
   streamed/batched/re-laid-out transcripts, not asymptotics.

---

## Phase 10 — Documentation — **Done**

Spec: update `README.md`, `CLAUDE.md`, and `agents.md` to match the code.

- Tool count is 21 (README said nineteen, `CLAUDE.md` said twenty); add
  `screenshot` to the system-prompt tool list in `default_prompt.h`.
- README: the agentic loop uses a renewable 50-step budget (not "up to 20");
  rewrite the auto-compaction section for non-destructive checkpoints and the real
  config keys (`compaction_buffer`, `compaction_recent_context`,
  `compaction_summary_max_tokens`); list all provider types (`anthropic`,
  `openai`, `chatgpt`, `ollama`, `vllm`, `openrouter`, `lmstudio`, `llamacpp`);
  add OpenSSL to the dependency table; replace the "early preview / beta5" status
  line.
- `lib/` is not "pure C++20 + POSIX" — it links `libbe` (Storage/Support kits:
  `BPath`, `find_directory`, `BUrl`). Correct the description.
- `CLAUDE.md` test table: add `test_codex_auth`, `test_openai_translate`,
  `test_text_attachment`, and every test added during this work.
- Document the project trust model, how to gate the web tools and `screenshot`
  with policy rules, and the bash/git rule-matching semantics.

Done (largely through the README → `docs/` split in `74d78a8`):

- README states "Twenty-one built-in tools", matching the 21 registered tools.
- Renewable 50-step budget documented (`README.md:12`, `CLAUDE.md:153`).
- Auto-compaction rewritten for non-destructive checkpoints with the real keys
  (`docs/sessions.md:17-64`).
- All provider types documented (`README.md:29-31`, `CLAUDE.md:181`).
- OpenSSL added to the dependency table (`openssl3_devel`).
- `lib/` dependency description corrected **in `docs/architecture.md:7`**.
- Test table extended with the Phase 3–7 tests: `test_subprocess`,
  `test_http_client`, `test_turn_integrity`, `test_engine_correctness`,
  `test_config_trust`, `test_config_write`, `test_db_upgrade`,
  `test_db_concurrency`, `test_project_used`, `test_permission_requests`,
  `test_web_tools`, `test_provider_params`, `test_pricing`, `test_codex_params`,
  `test_model_db`, `test_markdown`, `test_session_cleanup`,
  `test_session_lifecycle`, `test_chat_view`.
- Project trust model, web-tool/screenshot gating, and bash/git rule-matching
  semantics documented (`docs/configuration.md:43-62`, `docs/permissions.md:49-96`).

Remaining inaccuracies — **all six closed in the docs sweep** (job-count +
stale-doc pass):

1. Closed: `CLAUDE.md` now says `lib/` is C++20 + POSIX **plus the Haiku API
   kits**, citing `target_link_libraries(haicode)` in `lib/CMakeLists.txt`.
2. Closed: `CLAUDE.md` says twenty-one tools and names the two registration
   sites (`tools.cpp:2194-2212`, `web_tools.cpp:865-866`).
3. Closed: the test table has 40 rows — `test_text_attachment`,
   `test_codex_auth`, `test_propose_plan`, `snapshot_menu` (gui), and
   `session_list_item` (gui) added. `Tool Details` also gained the two
   subsections it was missing (`DiscardPlanTool`, `WriteAgentsMdTool`) — 21
   subsections for 21 tools.
4. Closed: `agents.md` says 40 (37 `lib` + 3 `gui`).
5. Closed: `default_prompt.h`'s main tool list carries `screenshot` (21 entries).
6. Closed: `README.md` states the actual position — 1.0 features complete and
   green, packaging and the remaining GUI items open.

Also documented in the same pass: every make invocation in the docs and the
agent's build-hook guidance takes its job count from `nproc` instead of a
hardcoded number (`-j$(nproc)`, with `sysinfo -cpu | grep -c 'CPU #'` as the
fallback), and `docs/permissions.md` notes that a `make*` Allow rule cannot
cover a `$(nproc)` substitution.

---

## Beyond the plan

Shipped work the release doc never asked for, recorded because it changes the
surface 1.0 will be judged on: markdown rendering and clickable links in the
transcript and plan review (`96d6c10`, `b869b72`), prompt-cache-stable append-only
history with cache-hit display (`1a2f392`, `1ba1c3d`), inline cache-friendly
compaction summarization (`5c2de6e`), ChatGPT encrypted-reasoning replay and cache
layout (`6ecbdb9`), `project_used` chat-vs-project sessions (`d518c0f`, `5fc8a3b`),
OpenAI strict-template role alternation (`2edf52a`, `17b9dfc`), `/retry`
checkpoint drop (`2631c0f`), llama.cpp effort passthrough (`1e5d99d`, `5a50599`),
sidebar repaint and scroll-follow fixes, and session-scoped `ask_user` replies
(`3ff5e62`).
