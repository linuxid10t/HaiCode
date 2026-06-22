# HaiCode

A native coding-agent app for **Haiku R1** — TUI (ncurses) and GUI (BeAPI) frontends backed by a C++20 agentic loop. Talks to Anthropic and OpenAI-compatible providers, runs tools with per-action permissions, and persists every session to SQLite.

> Status: early preview. Built and tested on Haiku R1-beta5 / development tip.

## Features

- **Agentic loop** — up to 20 tool-use steps per turn, with atomic interruption between steps.
- **Two frontends** — `haicode-tui` (ncurses, pure POSIX) and `haicode-gui` (Haiku native BeAPI).
- **Nineteen built-in tools** — `bash`, `read`, `write`, `edit`, `glob`, `grep`, `ls`, `find`, `symbols`, `diff`, `git`, `process`, `external_terminal`, `todo_write`, `propose_plan`, `discard_plan`, `write_agents_md`, plus `web_search` and `web_extract` — each with safe argument handling and a 100 KB output cap. The `symbols` tool does heuristic C/C++ symbol search (definitions + classified references), skipping comments and string literals for less noise than `grep`.
- **Multi-provider** — any number of Anthropic and OpenAI-compatible endpoints (proxies, Ollama, LM Studio, …) in `config.json`, with message-format translation between them.
- **Permissions** — fnmatch rules per session, with an interactive Ask → Allow / Deny / Allow-Always flow.
- **SQLite session history** — every user prompt, assistant message, tool call, and tool result is stored and reloadable. WAL mode + cascading deletes.
- **Project + global config** — global config in `B_USER_SETTINGS_DIRECTORY/haicode/config.json`, project config in `<project>/.haicode/config.json`.

## Prerequisites

### Required Haiku packages

Install these via `pkgman`:

| Package | What it provides | Used by |
|---------|------------------|---------|
| `haiku_devel` | BeAPI headers (`os/`, incl. `Tracker`), `libbe`, `libroot`, `libtracker` | `lib`, `tui`, `gui` |
| `nlohmann_json` | `nlohmann/json.hpp` (header-only) | `lib`, `gui` (config, provider payloads, events) |
| `sqlite_devel` | `sqlite3.h` + `libsqlite3` | `lib` (session persistence) |
| `curl_devel` | `curl/curl.h` + `libcurl` | `lib` (LLM HTTP) |
| `ncurses_devel` | `ncurses.h` + `libncursesw` | `tui` |
| `cmake` | build configuration | all |
| `make` | build runner | all |

The compiler itself (`gcc`/`g++`, gcc13) ships with the base Haiku install, so
no separate package is needed for it. `haiku_devel` is the single source of the
BeAPI headers and the `be`, `root`, and `tracker` libraries — there is no
separate `tracker_devel`.

Install everything in one line:

```bash
pkgman install haiku_devel nlohmann_json sqlite_devel curl_devel ncurses_devel cmake make
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
# Configure (only needed once, or when adding new source files)
cmake -B build -S .

# Build everything
make -C build -j4

# Or build targets individually
make -C build haicode-tui
make -C build haicode-gui
make -C build test_db
```

CMake uses `GLOB_RECURSE` to collect sources at configure time. After adding a new `.cpp` file, re-run `cmake -B build -S .` before `make`.

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
./build/tui/haicode-tui [/path/to/project]   # TUI (ncurses)
./build/gui/haicode-gui [/path/to/project]   # GUI (BeAPI)
./build/lib/test_db                           # Database smoke test
```

If no project directory is given, the GUI opens the last-used project from global config.

## Architecture

Three layers:

| Layer | What it is |
|-------|------------|
| `lib/` | `libhaicode` — core engine, providers, tools, persistence. Pure C++20 + POSIX. No GUI dependency. |
| `tui/` | ncurses frontend. `select()`-based main loop multiplexes stdin with a wake pipe fed by engine threads. |
| `gui/` | Haiku native BeAPI frontend. `BApplication` owns engine; events ride `BMessage`s from engine threads via `BMessenger`. |

Key types live in `lib/include/haicode/` — `engine.h`, `provider.h`, `tool.h`, `events.h`, `db.h`, `config.h`, `util.h`. See [`CLAUDE.md`](./CLAUDE.md) for a deeper walkthrough of the agentic loop, permission gate, message-format translation, and per-tool behavior.

## Configuration

- **Global:** `B_USER_SETTINGS_DIRECTORY/haicode/config.json` — provider keys, default model, `last_directory`.
- **Project:** `<project_dir>/.haicode/config.json` — overlays globals when that project is open.

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
  "auto_compact_threshold": 0.80,
  "auto_compact_reserve": 8192
}
```

| Key | Default | Meaning |
|-----|---------|---------|
| `auto_compact` | `true` | Master switch. |
| `auto_compact_threshold` | `0.80` | Fraction (0.0–1.0) of the window at which compaction triggers. |
| `auto_compact_reserve` | `8192` | Tokens subtracted from the window before computing the threshold, leaving room for the summary + the model's reply. |

Compaction is **disabled when the model's context window is unknown** (`window == 0`),
since the threshold cannot be sized safely. Set the window explicitly via the
top-level `"models"` object (e.g. `"models": {"my-local-model": 131072}`) to
enable compaction for models HaiCode doesn't recognize.

### Session autonaming

New sessions are created with an empty title and given a descriptive name
automatically, in two stages:

1. **Immediately** on the first prompt — a short title is derived from the first
   line of the user's message (whitespace collapsed, truncated to ~60 chars at a
   word boundary). This costs nothing and appears in the sidebar before the model
   even responds.
2. **After the first turn completes** — a one-shot LLM call refines the title
   into a concise (≤6-word) description. It fires at most once per session
   (gated on a single `user_prompted` message) and is best-effort: on any error
   the heuristic title is kept.

```json
{
  "autoname_sessions": true,
  "autoname_llm_refine": true
}
```

| Key | Default | Meaning |
|-----|---------|---------|
| `autoname_sessions` | `true` | Master switch. When `false`, both stages are skipped and titles stay empty. |
| `autoname_llm_refine` | `true` | Enables the LLM refinement step only. Set `false` to keep just the heuristic title. |

## License

MIT — see [LICENSE](./LICENSE).
