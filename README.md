# HaiCode

A native coding-agent app for **Haiku R1** — a native GUI (BeAPI) frontend backed by a C++20 agentic loop. Talks to Anthropic and OpenAI-compatible providers, runs tools with per-action permissions, and persists every session to SQLite.

> Status: early preview. Built and tested on Haiku R1-beta5 / development tip.

## Features

- **Agentic loop** — up to 20 tool-use steps per turn, with atomic interruption between steps.
- **Complete tool exchanges** — denied, interrupted, or plan-stopped batches persist an explicit failed result for each skipped call. Provider context groups each batch's results together and repairs missing results in older histories without changing stored messages.
- **`/retry` command** — typing `/retry` in the input deletes the last turn's assistant output and re-runs it on the stored prompt (attachments and skill invocations included); refused while a turn is streaming.
- **Native GUI** — `haicode-gui`, a Haiku BeAPI frontend.
- **Nineteen built-in tools** — `bash`, `read`, `write`, `edit`, `glob`, `grep`, `ls`, `find`, `symbols`, `diff`, `git`, `process`, `external_terminal`, `todo_write`, `propose_plan`, `discard_plan`, `write_agents_md`, plus `web_search` and `web_extract` — each with safe argument handling and a 100 KB output cap. Every tool that shells out runs through one cancellable subprocess runner: interrupts kill the whole child process group, output past the cap is drained (never corrupting the exit status), bash timeouts are clamped to 600 s, and a backgrounded command (`server &`) returns promptly instead of hanging the session. The `symbols` tool does heuristic C/C++ symbol search (definitions + classified references), skipping comments and string literals for less noise than `grep`.
- **Multi-provider** — any number of Anthropic and OpenAI-compatible endpoints (proxies, Ollama, LM Studio, …) in `config.json`, with message-format translation between them.
- **Permissions** — fnmatch rules per session plus literal session grants, with a structured approval flow (Deny / Allow Once / Allow this target for this session), a Permissions center for inspecting pending requests, revoking grants, editing global/project policy, and tracing why any operation was allowed or denied. Approval waits are cancellable: interrupting a session or closing the window denies it, and no Allow action is ever the default.
- **SQLite session history** — every user prompt, assistant message, tool call, and tool result is stored and reloadable. WAL mode + cascading deletes.
- **Project + global config** — global config in `B_USER_SETTINGS_DIRECTORY/haicode/config.json`, project config in `<project>/.haicode/config.json`.

## Prerequisites

### Required Haiku packages

Install these via `pkgman`:

| Package | What it provides | Used by |
|---------|------------------|---------|
| `haiku_devel` | BeAPI headers (`os/`, incl. `Tracker`), `libbe`, `libroot`, `libtracker` | `lib`, `gui` |
| `nlohmann_json` | `nlohmann/json.hpp` (header-only) | `lib`, `gui` (config, provider payloads, events) |
| `sqlite_devel` | `sqlite3.h` + `libsqlite3` | `lib` (session persistence) |
| `curl_devel` | `curl/curl.h` + `libcurl` | `lib` (LLM HTTP) |
| `openssl3_devel` | `openssl/sha.h` + `libcrypto` | `lib` (ChatGPT OAuth PKCE flow) |
| `cmake` | build configuration | all |
| `make` | build runner | all |

The compiler itself (`gcc`/`g++`, gcc13) ships with the base Haiku install, so
no separate package is needed for it. `haiku_devel` is the single source of the
BeAPI headers and the `be`, `root`, and `tracker` libraries — there is no
separate `tracker_devel`.

Install everything in one line:

```bash
pkgman install haiku_devel nlohmann_json sqlite_devel curl_devel openssl3_devel cmake make
```

### Optional

| Package | Why |
|---------|-----|
| `git` | the built-in `git` tool wraps it; only needed if you want in-app git operations |

### API credentials

You'll also need API credentials. The default providers read from the
environment, but you can also configure any number of Anthropic and
OpenAI-compatible endpoints (proxies, Ollama, LM Studio, …) in `config.json`:

```bash
export ANTHROPIC_API_KEY=sk-...     # for the default Anthropic provider
export OPENAI_API_KEY=sk-...        # for the default OpenAI provider
```

## Build

```bash
# Configure (only needed once)
cmake -B build -S .

# Build everything
make -C build -j4

# Run the test suite
ctest --test-dir build --output-on-failure

# Or build targets individually
make -C build haicode-gui
make -C build test_db
```

The build type defaults to `RelWithDebInfo` when none is specified.

### Hybrid (x86_gcc2) systems

On a hybrid Haiku image (gcc2 primary + GCC4+ secondary), HaiCode must be
built with the **secondary-arch** toolchain — it requires C++20, which gcc2
(GCC 2.95.3) cannot provide. The configure step detects this and aborts with a
clear message if you invoke it under the wrong compiler:

```
CMake Error: This compiler does not support C++20. ...
```

Activate the secondary arch first:

```bash
setarch x86         # 32-bit (subdir: /boot/system/lib/x86)
# or, for x86_64: ensure /boot/system/bin/x86_64 is on PATH
cmake -B build -S .
make -C build -j4
```

CMake reads the compiler's target macros to derive the arch and prepends the
matching `/lib/<arch>` subdirectory to the library search path, so the
correct-ABI `libsqlite3.so`, `libcurl.so`, etc. are linked automatically.
On a pure single-arch system (no `<arch>` subdirs) the flat paths are used —
no special action is needed.

## Run

```bash
./build/gui/haicode-gui [/path/to/project]   # GUI (BeAPI)
./build/lib/test_db                           # Database smoke test
```

If no project directory is given, the GUI opens the last-used project from global config.

**Single instance.** The app signature (`application/x-vnd.haicode`) carries `B_SINGLE_LAUNCH`: launching HaiCode again while it is running does not start a second process — the existing instance is activated and, if the new launch named a project directory (command line, Tracker "Open With", or drag-onto-icon), that directory is forwarded to the running window and becomes the active project.

**Disruptive changes ask first.** Saving settings, changing providers, or switching the project directory replaces the engine, which stops every running session. When any session is running, HaiCode asks first: it lists them all (including background sessions) and offers *Cancel* (the default — nothing changes) or *Interrupt and Apply*. An accepted change stops all runs deliberately.

## Permissions

Authorization is layered, evaluated per session in this order (first decisive layer wins):

1. **Exact temporary grants** — literal (category, target) pairs approved via "Allow this target for this session". They match the displayed text exactly, never glob-expand, and last until revoked or HaiCode exits.
2. **Session pattern grants** — legacy pattern grants created by older "Allow Always" flows.
3. **Session toggles** — the Permissions center's per-session switches: *Automatically allow writes*, *Allow reads everywhere*, and *Bypass permission prompts* (confirmed, not a sandbox; mode and offline restrictions still apply). Persisted per session, restored on reopen. Toggles are mode-scoped: write presets arm only in Build mode (Plan is non-destructive, Chat has no local access), and the reads-everywhere toggle arms in Build and Plan — outside-project reads gate and prompt in both. Switching modes re-derives the armed rules, so a dormant flag never waits to fire.
4. **Configured rules** — `permissions` arrays in the global (`~/config/settings/haicode/config.json`) and project (`<project>/.haicode/config.json`) config files, editable in the Permissions center's Policies tab. Within one source the last matching rule wins; an `Ask` rule falls through to prompting.

Above all of these sits the session's **mode**, which no grant can override: it is checked before any rule layer, so Plan can never write and Chat can never touch the local system regardless of toggles, grants, or configured rules.

Built-in exemptions are **fallbacks, not overrides**: read-only tools inside the project directory (and Haiku's system header/doc roots, which stay absolute) are allowed without prompting only when no rule layer matched. A Deny or Ask rule written in the Policies tab on `web_search`/`web_extract`, `screenshot`, `process`, read-only `git`, or in-project reads takes effect — an `Ask` rule routes the call through the normal approval prompt.

The approval window explains the operation in plain language (tool, category, full selectable target, tool-specific previews, outside-project and build-hook warnings). Deny is always the default: Enter, Escape, and closing the window all deny. Interrupting a session denies its pending approvals, so a wait can never hang the engine. The Permissions center (Settings → Permissions…) shows pending requests, every temporary grant with revoke, the rule editors, an operation inspector that reports the real decision path without executing, and a per-session activity log of authorization outcomes since launch. For quick changes, the prompt-row Permissions dropdown offers the common presets — Standard, Auto-write, YOLO — and the allow-reads-everywhere toggle without opening the center. Its item set is rebuilt per mode, so it only ever offers what the current mode can act on: Build shows the write presets plus the read toggle (grayed while YOLO is selected, since allow-all covers reads), Plan shows the read toggle (status: Standard, or All reads when the toggle is on), and in Chat the dropdown is hidden entirely — nothing in Chat can touch the local system, so there is nothing to configure. The closed field is a fixed compact width sized to the largest status word; the open menu sizes itself to its items.

## Architecture

Two layers:

| Layer | What it is |
|-------|------------|
| `lib/` | `libhaicode` — core engine, providers, tools, persistence. Pure C++20 + POSIX. No GUI dependency. |
| `gui/` | Haiku native BeAPI frontend. `BApplication` owns engine; events ride `BMessage`s from engine threads via `BMessenger`. |

Key types live in `lib/include/haicode/` — `engine.h`, `provider.h`, `tool.h`, `events.h`, `db.h`, `config.h`, `util.h`. See [`CLAUDE.md`](./CLAUDE.md) for a deeper walkthrough of the agentic loop, permission gate, message-format translation, and per-tool behavior.

## Configuration

- **Global:** `B_USER_SETTINGS_DIRECTORY/haicode/config.json` — provider keys, default model, `last_directory`.
- **Project:** `<project_dir>/.haicode/config.json` — overlays globals when that project is open.

All config saves are atomic and non-destructive: the app reads the existing
file, refuses to overwrite one that doesn't parse (so a hand-edit typo can't
cost you your API keys — the error is shown instead), preserves every key it
doesn't own, and writes through a temp file + rename. Config files and the
ChatGPT OAuth token store are written owner-only (`0600`), and existing
secret files are tightened at startup if another editor left them
group-readable.

Merging is presence-based: a project config key overrides the global value
only when the project file actually sets it. An absent project key never
resets a global setting (a missing `default_mode` no longer wipes a global
`"build"`), an explicit value equal to the default still counts (booleans
can be turned back *on*), and provider entries merge field by field — a
project `base_url` no longer drops the global `api_key`.

Settings saves are scope-aware: only global-scope keys are written to the
global config, and the build command is project-scoped — saved to the open
project's own `.haicode/config.json`, so one project's build hook never runs
in every project.

### Project trust

A repository's `.haicode/config.json` is treated as untrusted input. Two
rules protect you when you open a repository for the first time:

- **Providers never come from a project.** Credentials and endpoints are
  configured only in your global config — a checked-in config can never
  re-point a provider at another server (which would capture your API key
  or OAuth token), no matter how the project is configured.
- **Authority-granting keys require your consent.** A project's
  `permissions` rules, `build_command`, `agents`, and `web_search.api_keys`
  are ignored until you trust that project. When such keys are present,
  HaiCode shows exactly what they would enable (permission rules including
  any allow-all rule, the build command, agent overrides, which search
  engines get keys) and asks; **Don't Trust** is the default. Trust is
  recorded per resolved project path in the global config
  (`"trusted_projects"`), and any later change to those keys invalidates
  the record and asks again — cosmetic reformatting does not.

Everything else in a project config (model, default mode, vision overrides,
skills, …) merges as before; untrusted projects lose only the keys that
grant authority.

### Providers

The `"providers"` object maps arbitrary ids to provider configs. Each entry has
a `type` (`"anthropic"` or `"openai"`), an optional `api_key`, and an optional
`base_url`. When `type` is omitted it is inferred from the id: `"anthropic"`
defaults to the Anthropic type, anything else to OpenAI-compatible.

`base_url` is the **complete API root** — scheme, host, path prefix, and
version segment. The app appends only the resource path (`/messages`,
`/chat/completions`, `/models`), so the version must be part of `base_url`.
Defaults are `https://api.anthropic.com/v1` and `https://api.openai.com/v1`
when omitted.

```json
{
  "providers": {
    "anthropic": {
      "type": "anthropic",
      "api_key": "sk-..."
    },
    "anthropic-proxy": {
      "type": "anthropic",
      "api_key": "sk-...",
      "base_url": "https://my-proxy.example.com/v1"
    },
    "ollama": {
      "type": "openai",
      "base_url": "http://localhost:11434/v1"
    }
  }
}
```

Env-var fallback applies only to the providers whose ids are literally
`"anthropic"` and `"openai"`: `ANTHROPIC_API_KEY` and `OPENAI_API_KEY`
respectively. An OpenAI-compatible entry with no key but a `base_url` (e.g. a
local Ollama instance) is registered keyless.

In the GUI, **Settings → Preferences** opens a list-based editor where you can
add, edit, and remove providers; changes persist to the global config file.
The provider dropdown in the toolbar is rebuilt dynamically from the config.

### Auto-compaction

Long sessions grow toward the model's context window. When the input-token
usage reported by the previous step reaches a configurable fraction of the
window, HaiCode summarizes the older portion of the conversation into a single
`[Conversation summary]` message (generated by the active provider) and
replaces it in the session history, so the loop can continue instead of dying
on a context-overflow rejection. The current user turn is always kept intact.

```json
{
  "auto_compact": true,
  "auto_compact_threshold": 0.80
}
```

| Key | Default | Meaning |
|-----|---------|---------|
| `auto_compact` | `true` | Master switch. |
| `auto_compact_threshold` | `0.80` | Fraction (0.0–1.0) of the window at which compaction triggers. |

Compaction is **disabled when the model's context window is unknown** (`window == 0`),
since the threshold cannot be sized safely. Set the window explicitly via the
top-level `"models"` object (e.g. `"models": {"my-local-model": 131072}`) to
enable compaction for models HaiCode doesn't recognize.

After each compaction, a collapsible `[context compacted]` transcript entry
shows the checkpoint summary in the chat scrollback (click to toggle in the
GUI). It is rendered from the checkpoint table, never stored
as a message, so the model's context is unaffected. The context-size indicator
in the status area also drops immediately to a post-compaction estimate
(checkpoint block + retained tail) instead of waiting for the next model
response to report fresh usage.

### Image handling

Image attachments are limited to **4 MiB** each, including programmatically
supplied payloads. Unavailable or oversized images are recorded explicitly
instead of silently disappearing. Raw user images and screenshots are sent
only for the current and immediately preceding user turn; older images become
text placeholders or available descriptions. Persisted image bytes remain
unchanged.

Text-only models receive descriptions from your configured vision fallback,
or placeholders when no description is available—even after compaction.
Failed description attempts are remembered across steps and reopening.
Interrupt cancels fallback requests too; cancelled attempts can be retried.

### Prompts while running

You can submit another prompt while a session is streaming, executing a tool,
waiting for approval, or compacting. It queues for that session and runs as its
own turn in FIFO order once the current tool exchange is complete. Attachment
payloads and slash-skill content are captured when you submit, not when the
queued turn eventually starts. The status strip shows the queued count without
resetting the active turn's counters or todos.

Interrupt stops the current turn; queued prompts then start with fresh
interruption state. Closing the engine discards queued prompts rather than
restarting work. Each session has an independent foreground queue.

### Deleting sessions

Deleting a session asks for confirmation first (a running session is
interrupted as part of deletion). The engine retires the session — refusing
new prompts, discarding queued ones, cancelling open approvals and questions
— waits for its work to fully stop, and only then removes the conversation,
todos, and history. If the database delete fails, the session stays usable
and the error is shown. Other sessions, including their permission grants,
are untouched.

### Session autonaming

New sessions are created with an empty title and given a descriptive name
automatically, in two stages:

1. **Immediately** on the first prompt — a short title is derived from the first
   line of the user's message (whitespace collapsed, truncated to ~60 chars at a
   word boundary). This costs nothing and appears in the sidebar before the model
   even responds.
2. **Periodically** via a one-shot LLM call — on turn 1 and again every 5 turns
   (6, 11, 16, …), the model reconsiders the title given the full prompt
   history. On the first call it generates a fresh ≤6-word title; on later calls
   it either repeats the current title verbatim (no change) or returns a revised
   one if the session's focus has shifted. Best-effort: on any error the
   existing title is kept. Refinement is separate, tracked maintenance work:
   it never keeps the session busy or delays your next prompt. Interrupted
   turns skip it; interrupt and shutdown cancel it. A stale result cannot
   overwrite a newer session title.

```json
{
  "autoname_sessions": true,
  "autoname_llm_refine": true
}
```

| Key | Default | Meaning |
|-----|---------|---------|
| `autoname_sessions` | `true` | Master switch. When `false`, both stages are skipped and titles stay empty. |
| `autoname_llm_refine` | `true` | Enables the LLM refinement step only. Set `false` to keep just the heuristic title. Refinement fires on turn 1 and every 5 turns thereafter. |

## License

MIT — see [LICENSE](./LICENSE).
