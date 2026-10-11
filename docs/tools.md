# Built-in tools

Developer reference for the 21 tools the agent can call. Built-in tools live in
`lib/src/tool/tools.cpp` and are registered by `register_builtin_tools()`; the
web tools live in `lib/src/tool/web_tools.cpp` and join through
`register_web_tools()`. Permission behavior shared by all tools (rule layers,
built-in exemptions, mode allowlists) is in [permissions.md](permissions.md).

## Shared behavior

All tools share a `MAX_OUTPUT = 100 KB` cap and a `sq()` helper for safe single-quoted shell arguments.

**Shared subprocess runner** (`util::run_subprocess`, `lib/src/util/subprocess.cpp`): every tool that shells out (bash, grep, diff, git, find, process, screenshot) and the engine build hook run through one cancellable runner. Per command it forks a child in its **own process group**, gives it `/dev/null` stdin, merges stdout+stderr into one pipe, `chdir`s to the working dir, applies `extra_env`, and **closes every fd >= 3** so parent SQLite/socket handles never leak. The parent polls (100 ms): `*interrupt` kills the whole group (`interrupted`), the deadline does the same (`timed_out`), output is appended up to a 100 KB cap and **discarded past it** (`truncated` — the child is never SIGPIPEd, so a truncated success keeps its real exit code). Once the direct child exits, a bounded ~500 ms grace drain captures a backgrounded descendant's (`server &`) last output before returning — backgrounded commands return promptly and stay alive by design.

## BashTool (`bash`)
- Runs `{ command; }` via `util::run_subprocess` with `ctx.working_dir` as cwd (the runner chdirs — no `cd` wrapper)
- Timeout = model-supplied seconds, default 30, clamped to [1, 600]; `timed_out` → failed result "Command timed out after Ns"; `interrupted` → "Command interrupted"; no more exit-code-124 convention
- Exit code 0 → success; nonzero → failure with "Exit code: N" and output preserved
- Over-cap output appends `\n[output truncated]` without flipping success
- **Rule semantics (Task 20)**: a bash Allow pattern carrying glob metacharacters (`*?[`) must cover EVERY command of the invocation — the resource is split quote-aware at `;`, `&&`/`||`, `|`, newlines, and whitespace-delimited `&`, and each segment must fnmatch the pattern (`bash_pattern_authorizes()` in `lib/src/permission/permission.cpp`), so `make*` allows `make -j4` but not `make; rm -rf ~`; a segment containing command substitution (`$(…)`, backticks) never matches any pattern except the universal `*` (its output is unknowable at match time). Deny patterns keep whole-string fnmatch (over-matching a Deny is the safe direction); literal resources stay exact-equality; the session bypass `{"*","*",Allow}` matches every segment and still covers compounds. Redirect operators (`2>&1`, `&>`) and quoted separators are not command separators

## ReadTool (`read`)
- Resolves relative paths against `ctx.working_dir` (trailing slashes stripped before join)
- Rejects empty path; detects binary files (null byte scan of first 8 KB) and refuses them
- `offset` is 1-based (offset=1 starts at line 1, the default)
- Enforces 100 KB cap mid-read
- Error messages include `strerror(errno)`
- **`f.clear()` is called before `seekg(0)` after the binary scan** — `read()` past EOF sets `eofbit`; without the clear, `getline()` fails immediately and returns empty output for any file smaller than 8 KB

## WriteTool (`write`)
- Resolves relative paths against `ctx.working_dir`
- Creates missing parent directories by walking path components with `mkdir()`
- Rejects a missing or non-string `content` **before touching the file** (a missing key must not default to `""` — that would empty the target); an explicitly empty string stays legal
- Atomic write via `util::atomic_write_file()`: `mkstemp` on `<resolved-path>.tmp_write_XXXXXX` (O_CREAT|O_EXCL — can never clobber a pre-existing sibling), `fchmod` (to the target's previous mode when it existed — an 0755 script keeps its execute bits; 0644 for new files; an EXPLICIT mode argument always wins over both, so secret files (`config.json`, `openai-auth.json`) are written 0600 even over pre-existing loose bits), write loop + `fsync`, then `rename()`; temp unlinked on any failure. Haiku-specific: BFS attributes (`BEOS:TYPE` MIME, Tracker metadata) are copied from the old file onto the replacement before the fsync (a bare rename drops them — best-effort, individual failures skipped); a symlink `path` is resolved with `realpath()` and the write lands in the target, keeping the link; an unresolvable (dangling/loop) symlink is refused with an error and nothing is written. `util::ensure_owner_only(path)` tightens an existing file to 0600 when it carries group/world bits (startup hardening for secret files older releases created 0644); it is a no-op on already-tight and missing files

## GlobTool (`glob`)
- Absolute patterns (starting with `/`) bypass base directory prepend
- Permission action is `"read"` (not `"glob"`) so the "Allow reads outside trusted roots" session toggle covers it outside the project tree; a config rule scoping it must use `{"action":"read", ...}`
- `GLOB_MARK` removed — was adding unwanted trailing slashes to directory matches
- `globfree()` called unconditionally after `glob()` (always safe per POSIX)
- Returns error on `rc != 0 && rc != GLOB_NOMATCH`; GLOB_NOMATCH returns "(no matches)"
- Note: `**` recursive matching is not supported by POSIX `glob()`

## GrepTool (`grep`)
- Runs `/bin/grep -r -E -I --color=never` plus the mode flags; pattern, path, include, and exclude globs are all passed through `sq()` — no shell injection possible
- `-I` skips binary files, so object files and executables under `build/` never print "binary file matches" lines; `.git` is always excluded (`--exclude-dir`), and `exclude_dirs` (array of directory-name globs, e.g. `["build"]`) adds more
- `output_mode`: `"content"` (default, matching lines), `"files"` (`-l`, paths only), `"count"` (`-c -H`, `path:N` per file with zero-count files dropped — `-H` keeps the path on a single-file search); any other value is refused. Use `files`/`count` to size a search before reading matches
- Content mode only: `only_matching` (`-o`), `line_numbers` (default true), and `max_line_length` (default 1000 bytes; `0` = no limit) — longer output lines are cut at a UTF-8 boundary with a ` [... +N bytes]` note, so one-row-per-line markdown tables or minified files can't spend the whole output budget on a few matches
- Permission action is `"read"` (not `"grep"`) so the "Allow reads outside trusted roots" session toggle covers it outside the project tree; a config rule scoping it must use `{"action":"read", ...}`
- Relative search path resolved against `ctx.working_dir`
- Exit codes: 0 = matches found, 1 = no matches (success), 2 = grep error (returns failure)
- Runs via the shared runner (60 s timeout, interruptible); output capped at 100 KB with a `[output truncated]` marker
- Schema key is `"line_numbers"` (boolean, default true), not `"-n"`

## EditTool (`edit`)
- Reads the whole file into memory, refuses binary files (null byte scan)
- Counts `old_string` occurrences; requires `replace_all=true` if it appears more than once
- Empty `old_string` is rejected with a hint to use both fields for deletion
- Rejects a missing or non-string `new_string` before touching the file (a missing key must not default to `""` — that would delete `old_string`); an explicitly empty string stays legal (deletion by design)
- Replacement is in-memory (single-pass for `replace_all`, single `find` otherwise)
- Writes via `util::atomic_write_file()` — same mkstemp/mode-preserving path as WriteTool
- Required permission: `write`

## LsTool (`ls`)
- Resolves relative `path` against `ctx.working_dir`; defaults to the project dir if omitted
- Uses `opendir()`/`readdir()`; skips `.` and `..`
- Adds suffixes via `stat()`: `/` for directories, `@` for symlinks
- Output sorted with `std::sort` and capped at 100 KB
- Required permission: `read`

## ExternalTerminalTool (`external_terminal`)
- Double-forks: first child calls `setsid()` and detaches stdio, grandchild `execl`s `/boot/system/apps/Terminal`
- Invokes Terminal as `Terminal -w <working_dir> /bin/sh -c '<command>'` — the window closes when the command exits
- Returns immediately; output is **not** captured (designed for interactive TUIs like vim, REPLs)
- Distinct permission action (`external_terminal`) so prompts make clear which tool is asking
- Required permission: `external_terminal`

## ProposePlanTool (`propose_plan`)
- Available in both Plan and Build mode — no engine-level filter removes it
- Always allowed — `ToolRegistry::execute()` bypasses the permission gate (same as web tools)
- Writes the plan markdown to `<project_dir>/.haicode/plans/plan_YYYYMMDD_HHMMSS_<4-hex-rand>.md`
- Parent directory created with `mkdir()` walking (same pattern as WriteTool)
- Returns the on-disk path so the engine can surface it via `PlanProposed` and end the turn

## DiscardPlanTool (`discard_plan`)
- Gate-gated `write` action (resource `<working_dir>/.haicode/plans/`) — unlike `propose_plan`/`todo_write`/`ask_user` it has no built-in exemption (the interaction-tool exemption in `ToolRegistry::evaluate`, `lib/src/permission/permission.cpp`, covers only those three), so an Ask/Deny rule can gate it
- Finds the newest plan file whose first line carries `<!-- haicode-status: active -->` (lexicographic name order = chronological), rewrites that header to `<!-- haicode-status: <reason> -->`, and writes back through `atomic_write_file`
- No plans directory → failure; no active plan → failure with an explanatory message, because the engine auto-retires a plan (status `implemented`) once all its todos complete — so calling this after finishing implementation is a no-op, not an error
- Retired plans are no longer sent as the active plan (a status update says "No plan is active any more")
- Available in Plan and Build mode (the `plan_allowed` set in `tool_allowed_in_mode`, `lib/src/permission/permission.cpp`)

## WriteAgentsMdTool (`write_agents_md`)
- Gate-gated `write` action; `resource()` mirrors `execute()` so the gate sees the file actually written — an existing case variant (`AGENTS.md`) is the real destination, never shadowed by a lowercase sibling
- Resolves the target with `find_project_instructions_file` (case-insensitive lookup) and updates that variant in place; creates `agents.md` only when no variant exists
- Empty `content` is rejected; the write goes through `atomic_write_file`; output is `{path, bytes_written, note}`
- Build mode only — not in the Plan allowlist
- Takes effect from the next session: the loader appends `agents.md` (or `claude.md`) verbatim to the system prompt

## TodoWriteTool (`todo_write`)
- Always allowed — `ToolRegistry::execute()` bypasses the permission gate (same as `propose_plan` and web tools)
- Whole-list replace: the model sends the full todo list every call, never a delta; an empty array clears everything
- Each item requires `content` (imperative form), `activeForm` (present-continuous for the spinner), and `status` (`pending`, `in_progress`, or `completed`)
- The engine detects this tool by name, writes the parsed todos to the `session_todo` table, and publishes a `TodoUpdated` event; the tool itself only validates and echoes the input
- Available in both Plan and Build mode

## AskUserTool (`ask_user`)
- Always allowed — `ToolRegistry::execute()` bypasses the permission gate (same as `propose_plan`, `todo_write`, and web tools)
- Asks the user a disambiguation question with 2–5 preset options plus a free-form "Other" entry; the model passes `question` (string) and `options` (string array of 2–5 items)
- The tool itself only validates and echoes the request as a JSON placeholder (`status: "asking"`); the real answer arrives later — the engine detects `ask_user` by name, publishes `AskUserRequested`, ends the turn, and blocks the agentic loop on `asking_cv_` until `reply_to_ask()` is called
- Once the user replies (selects a preset or types custom text, or cancels), `SessionStore::update_tool_result_by_call_id(session_id, call_id, …)` overwrites the placeholder `tool_result` row's `output` with `{"answer": "..."}` — scoped to the session and the newest matching row, and pending asks are keyed by (session, call id), because OpenAI-compatible servers' call ids are passed through and some reuse them and the engine republishes `ToolSuccess` so the UI swaps the placeholder bubble for the picked answer; then the loop resumes with the real answer visible to the model as the tool result
- Available in both Plan and Build mode — the engine adds it to the Plan-mode allowlist so it survives the read-only tool filter

## DiffTool (`diff`)
- Writes proposed content to a securely-created scratch file in the **system temp directory** (`find_directory(B_SYSTEM_TEMP_DIRECTORY)` with a `/tmp` fallback, `mkstemp` on `haicode_diff_XXXXXX`) — never beside the target, so a preview against a read-only location (e.g. the packagefs-mounted system headers in Plan mode) succeeds; runs `diff -u <original> <tmp>` via the shared runner (30 s, interruptible), then `unlink`s it — a pre-existing sibling like `foo.tmp_diff` is never read or deleted
- Exit code 1 (differences found) is treated as success; exit code 2 is a diff error (returns failure)
- Returns `"(no differences)"` when the content is identical
- Always allowed within the project directory and the Haiku system header roots; paths outside go through the gate (`read` permission)

## GitTool (`git`)
- Runs `git -C <working_dir> <subcommand> [args...]` via the shared runner (300 s timeout, `ctx.interrupt` honored, `GIT_TERMINAL_PROMPT=0` in the child env so a credential prompt fails fast instead of hanging)
- **No terminal**: stdin is `/dev/null`, so interactive modes can never be answered. `interactive_refusal()` refuses them before git runs, naming the patch-file route: `add -p`/`--patch`/`-i`/`--interactive`/`-e`/`--edit` (and `add`/`reset` short clusters containing those letters, e.g. `-Ap` — neither has a short option that takes a value), `commit -p`/`--patch`/`--interactive` (`commit -i` is `--include` and passes), `checkout`/`reset -p`/`--patch`, `stash -p`/`--patch` except after `list`/`show` (which print patches), and `rebase -i`/`--interactive`/`--edit-todo`. Exact spellings only (an abbreviation like `--patc` simply reads an empty stdin); scanning stops at `--`. `GIT_EDITOR=false` and `GIT_SEQUENCE_EDITOR=false` make anything that would open an editor (commit/tag/amend without a message, `merge --edit`) fail at once; a failure mentioning the editor gets a "pass the message with -m, or use --no-edit" hint
- **Per-hunk staging** is `apply --cached <patch>`: write the wanted hunks to a patch file and apply them to the index. `apply` is never read-only (always gated, refused in Plan mode), and `--unsafe-paths` is refused down to its shortest unambiguous abbreviation `--uns` so a patch can't write outside the working tree
- **Read-only hardening (Task 20)**: when `git_invocation_is_readonly()` classifies the invocation, the command becomes `git -c core.fsmonitor= -c core.hooksPath=/dev/null -c gpg.program=false -c gpg.ssh.program=false -c gpg.x509.program=false -c log.showSignature=false -C … <sub> [--no-ext-diff --no-textconv]` with `GIT_OPTIONAL_LOCKS=0` in the env — the bypass path (and everything Plan mode may run) must not honor repo-local config that can execute programs (a repo setting `diff.external` to a script gets real diff text, never a driver run; a repo setting `gpg.program` + `log.showSignature` would otherwise run it on a plain `git log`) and `git status` must not rewrite the index. `--no-ext-diff --no-textconv` are appended for `diff`/`log`/`show` and after the verb of `stash list`/`show` (`stash list -p` runs textconv); `blame` gets `--no-textconv` (blame runs textconv by default). The classifier refuses the re-enabling `--ext-diff`/`--textconv`, so model args appended after the flags can't override them. Residual limitation: in-tree `.gitattributes` filter drivers (`filter.<name>.clean`) configured in the repo's own `.git/config` can still run on `status`/`diff` — the driver name is repo-chosen, so no fixed `-c` blanks it
- Subcommand allowlist (validated before execution): `status`, `diff`, `log`, `show`, `branch`, `blame`, `stash`, `add`, `commit`, `checkout`, `reset`, `remote`, `merge`, `rebase`, `pull`, `push`, `fetch`, `tag`, `shortlog`, `describe`, `rev-parse`, `ls-files`, `merge-base`, `rev-list`, `ls-tree`, `cat-file`, `grep`, `range-diff`, `for-each-ref`, `apply`
- The tool description carries review guidance (bound `log` with `-n`; base via `merge-base`, `log --oneline <base>..HEAD`, `diff --stat <base>...HEAD`, then per-file `diff <base>...HEAD -- <path>`, because output over 100 KB is truncated)
- **Plan mode**: `git` is in the Plan allowlist but `ToolRegistry::evaluate` refuses any invocation the classifier does not accept with a `[mode restriction]` failed result (`denied=false`), checked right after the mode/offline checks and BEFORE every rule layer — no Allow rule, session bypass, or grant can make Plan mode commit, branch, or run a driver
- All args passed through `sq()` — no shell injection possible
- Non-zero exit with non-empty output returns `success=false` with the output in `error`
- Read-only invocations are allowed by default (built-in exemption when no rule matched), classified by `git_invocation_is_readonly()` (see the Permission gate notes: listing-only forms for `branch`/`stash`/`tag`, unsafe-option and outside-path guards, fail-closed); everything else goes through the gate (`git` permission, resource = subcommand name — granularity limitation: an Allow rule on a subcommand covers its mutating forms too)

## FindTool (`find`)
- Wraps `find(1)` with optional `-maxdepth`, `-type`, `-name`, `-mtime`, `-size` flags; runs via the shared runner (60 s, interruptible)
- All user-supplied values passed through `sq()` — safe against injection
- `type` is validated to one of `f`, `d`, `l` before being passed to the shell
- Relative `path` resolved against `ctx.working_dir`; defaults to `ctx.working_dir` if omitted
- Always allowed within the project directory and the Haiku system header roots; paths outside go through the gate (`read` permission)
- Fills the gap left by GlobTool, which does not support `**` recursive matching

## SymbolsTool (`symbols`)
- Heuristic C/C++ symbol search: finds definitions and classifies references, faster than grep for tracing fields and functions across files
- Three queries: `definition` (where a name is defined), `references` (every occurrence, classified), `callers` (usage sites only — `call` + `member_access`)
- **Tokenizer (`strip_non_code`)**: a state machine blanks out line/block comments, string literals, and char literals before matching, eliminating grep's false positives on identifiers mentioned in comments or strings; comment/string state persists across lines
- **Scope tracking**: a brace-depth counter plus a stack of enclosing class/struct names lets each hit be classified (`definition` / `call` / `member_access` / `declaration` / `mention`) and annotated with `(in ClassName::method)` for usage sites
- No `clangd` / `libclang` / index — pure `<regex>` over CODE-only substrings; unresolvable queries (templates, macros) degrade to candidate sets. C/C++ only at launch
- Always allowed within the project directory and the Haiku system header roots (read-only, `resource()` returns a resolved path like `find`); paths outside go through the gate (`read` permission)
- Cap: 200 hits with a `[truncated]` marker; output sorted by file then line, with a trailing `N hits in M files` summary

## ProcessTool (`process`)
- Three actions selected via the `action` field: `list`, `kill`, `check_port`
- **list**: runs `ps` via the shared runner (30 s, interruptible) and optionally filters output lines by a `filter` substring (header always stripped from the no-match path); returns `"(no matching processes)"` if filter matches nothing
- **kill**: calls `kill(2)` directly (not a shell command) with the given `pid` and `signal` (TERM, KILL, HUP, INT, USR1, USR2, STOP, CONT; default TERM)
- **check_port**: runs `netstat -n` via the shared runner (30 s, interruptible) and filters for lines containing `:<port>`; returns `"Nothing listening on port N"` if none found
- `list` and `check_port` are allowed by default (built-in exemption when no rule matched); `kill` goes through the gate (`process` permission, resource `pid:<N>`)

## WebSearchTool (`web_search`) — `web_tools.cpp`
- Backends: `ddg_lite` (default, no API key), `ddg_html` (HTML scraping), `exa` and `zai` (JSON APIs via `HttpClient::post_json`). Configurable via `AppConfig::web_search_engine`
- Exa/Z.ai need an API key: resolved from `AppConfig::web_search_api_keys[engine]` (config `web_search.api_keys.exa`/`.zai`), falling back to `$EXA_API_KEY` / `$ZAI_API_KEY`. Missing key → tool error before any network call
- Exa: `POST api.exa.ai/search` with `x-api-key` header; first `highlights` entry becomes the snippet. Z.ai: `POST api.z.ai/api/paas/v4/web_search` with `Bearer` auth; `content` truncated to 400 chars for the snippet
- Returns ranked results: title, URL, snippet — read snippets before calling `web_extract`
- `max_results` defaults to 5, capped at 10 (all engines)
- HTML parsing is in-process (no external deps); DDG may surface CAPTCHA errors
- Required permission: `web_search`

## WebExtractTool (`web_extract`) — `web_tools.cpp`
- Fetches one URL via `HttpClient` (libcurl) with a browser User-Agent
- Returns cleaned main-body text — nav, ads, scripts stripped (HTML stripper is ~400 lines, untested)
- `max_chars` defaults to 8000 and is clamped to `[1, 100 KB]` (the shared output budget — the model-supplied bound is untrusted); rejects non-http(s) URLs
- The output's `truncated` flag is `text_truncated || download_truncated`: a body over `HttpClient`'s 10 MB download cap aborts at the cap with the real HTTP code and surfaces `truncated: true` on an otherwise successful result
- Allowed by default (built-in exemption); can be gated with an Ask/Deny rule in the Policies tab (effective since the Task 18 rule-precedence change)
- Required permission: `web_extract`

## ScreenshotTool (`screenshot`) — `tools.cpp`
- Runs the system CLI `screenshot --silent --format=png [--window] [--delay=N] '<path>'` (`sq()`-quoted path) via the shared runner (15 s, interruptible); the CLI writes a PNG into the temp directory (`find_directory(B_SYSTEM_TEMP_DIRECTORY)` → `/tmp`)
- **Exit status is not trusted** — the CLI returns nonzero codes even on success; the tool validates the output file instead (exists, ≥24 bytes, PNG signature)
- Parses WxH from the PNG IHDR header (bytes 16–23, big-endian; no image library); enforces the same 4 MB cap as GUI image attachments (over-cap → error suggesting `window=true`)
- Returns a JSON object with a reserved `attachments` array (`{media_type, path, data_b64}`, same shape as user attachments); the engine strips it at the `tool_result` append site and persists it on the row, and `assemble_messages` emits it as Anthropic image blocks inside the tool_result content (OpenAI's `translate_messages` splits the blocks into a follow-up `image_url` user message)
- **Vision-gated**: hidden from the tool list unless `model_supports_vision()` says the active model accepts images (config `"vision"` entries + `kVisionModels` prefix table as one prefix database → fail-closed false). Re-evaluated per step, so a mid-loop model switch flips availability on the next step
- Allowed by default (read-only, shared screen state — a built-in exemption, so an Ask/Deny rule can gate it); available in Plan and Build modes, absent from Chat
- **Visible in the transcript**: tool bubbles collapse, so a successful capture also appends a non-collapsible system line ("Screenshot captured — image sent to the provider.") both live (`MainWindow::_HandleToolResult`) and on session replay (`_RebuildChat` matches the `tool_result` row's `call_id` back to the `tool_calls` entry from its `assistant_text` row)
- Config: `"vision": {"<model_id or prefix>": true|false}` joins the prefix table (longest key wins, user wins ties; also editable in Settings → Model Database…); editable in Settings → General as the "Vision:" dropdown (Auto/Yes/No, scoped to the selected default model — Auto removes the override, Yes/No write the `"vision"` key in config.json)
- **Vision fallback**: `"vision_fallback": {"provider": "...", "model": "..."}` names a vision-capable (provider, model) pair (editable in Settings → General dropdowns). When the session's primary model is text-only and the fallback is configured: at `agentic_loop` start, a backfill pass (`SessionEngine::backfill_attachment_descriptions`) one-shot describes every persisted attachment (user_prompted and tool_result rows) lacking a `"description"` key via `SessionEngine::describe_image` (same synchronous provider-stream pattern as `refine_title_llm`), persists descriptions through `SessionStore::update_message_data` (describe-once; clamped to 4 KiB; survives restart), and `ContextBuilder::assemble_messages(..., model_accepts_images=false)` emits `[image: path — described by vision fallback: ...]` text blocks instead of image blocks. The screenshot-tool vision gate is relaxed (`!vision && !vision_fallback_ready()`), so the tool stays available with a fallback; switch to a vision model sends the original image bytes as-is. Fallback readiness gate: model non-empty and its provider registered — the fallback is an explicit user choice, trusted without a `model_supports_vision` prefix-table check (local vision servers aren't in the table). Fallback describer, title-refinement, and compaction-summarizer usage IS added to session cost accounting (Task 26): each maintenance call captures `TokenUsage` from its `on_finish` and calls `store_.update_cost` through the same `compute_step_cost` resolution as foreground steps
