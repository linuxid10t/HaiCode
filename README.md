# HaiCode

A native coding-agent app for **Haiku R1** — a native GUI (BeAPI) frontend backed
by a C++20 agentic loop. Talks to Anthropic and OpenAI-compatible providers, runs
tools with per-action permissions, and persists every session to SQLite.

> Status: early preview. Built and tested on Haiku R1~beta6 / development tip.

## Features

- **Agentic loop** — a renewable 50-step tool budget per turn (configurable per
  agent, with a hard ceiling), and atomic interruption between steps.
- **Twenty-one built-in tools** — `bash`, `read`, `write`, `edit`, `glob`, `grep`,
  `ls`, `find`, `symbols`, `diff`, `git`, `process`, `external_terminal`,
  `todo_write`, `ask_user`, `propose_plan`, `discard_plan`, `write_agents_md`,
  `screenshot`, `web_search`, `web_extract` — each with safe argument handling and
  a 100 KB output cap, all shelling out through one cancellable subprocess runner.
- **Complete tool exchanges** — denied, interrupted, or plan-stopped batches
  persist an explicit failed result for each skipped call, so provider context
  never drifts from the stored transcript. See
  [Sessions](docs/sessions.md#tool-exchange-integrity).
- **Prompts while running** — submit another prompt mid-turn; it queues per
  session and runs FIFO when the current tool exchange completes. `/retry` re-runs
  the last turn. See [Sessions](docs/sessions.md#prompts-while-running).
- **Native GUI** — `haicode-gui`, a Haiku BeAPI frontend with a streaming markdown
  transcript, session sidebar, plan review, permissions center, and model database.
- **Multi-provider** — any number of Anthropic and OpenAI-compatible endpoints
  (proxies, Ollama, vLLM, OpenRouter, LM Studio, llama.cpp) plus a ChatGPT/Codex
  sign-in, with message-format translation between them.
- **Permissions** — layered rules per session with a structured approval flow, a
  Permissions center, and an inspector that explains why any operation was allowed
  or denied. Details in [Permissions](docs/permissions.md).
- **SQLite session history** — every prompt, assistant message, tool call, and tool
  result is stored and reloadable, with checkpoint-based auto-compaction so long
  sessions keep running. Details in [Sessions](docs/sessions.md).
- **Project + global config** — global config in
  `B_USER_SETTINGS_DIRECTORY/haicode/config.json`, project config in
  `<project>/.haicode/config.json`, with a trust boundary for checked-in configs.
  Details in [Configuration](docs/configuration.md).

## Prerequisites

### Required Haiku packages

| Package | What it provides | Used by |
|---------|------------------|---------|
| `haiku_devel` | BeAPI headers (`os/`, incl. `Tracker`), `libbe`, `libroot`, `libtracker` | `lib`, `gui` |
| `nlohmann_json` | `nlohmann/json.hpp` (header-only) | `lib`, `gui` (config, provider payloads, events) |
| `sqlite_devel` | `sqlite3.h` + `libsqlite3` | `lib` (session persistence) |
| `curl_devel` | `curl/curl.h` + `libcurl` | `lib` (LLM HTTP) |
| `openssl3_devel` | `openssl/sha.h` + `libcrypto` | `lib` (ChatGPT OAuth PKCE flow) |
| `cmake` | build configuration | all |
| `make` | build runner | all |

The compiler itself (`gcc`/`g++`, gcc13) ships with the base Haiku install, so no
separate package is needed for it. `haiku_devel` is the single source of the BeAPI
headers and the `be`, `root`, and `tracker` libraries — there is no separate
`tracker_devel`.

Install everything in one line:

```bash
pkgman install haiku_devel nlohmann_json sqlite_devel curl_devel openssl3_devel cmake make
```

### Optional

| Package | Why |
|---------|-----|
| `git` | the built-in `git` tool wraps it; only needed if you want in-app git operations |

### API credentials

You'll also need API credentials. The default providers read from the environment,
but you can configure any number of Anthropic and OpenAI-compatible endpoints in
`config.json`:

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

On a hybrid Haiku image (gcc2 primary + GCC4+ secondary), HaiCode must be built
with the **secondary-arch** toolchain — it requires C++20, which gcc2 (GCC 2.95.3)
cannot provide. The configure step detects this and aborts with a clear message if
you invoke it under the wrong compiler:

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
correct-ABI `libsqlite3.so`, `libcurl.so`, etc. are linked automatically. On a pure
single-arch system (no `<arch>` subdirs) the flat paths are used — no special action
is needed.

## Run

```bash
./build/gui/haicode-gui [/path/to/project]   # GUI (BeAPI)
./build/lib/test_db                          # Database smoke test
```

If no project directory is given, the GUI opens the last-used project from global
config.

**Single instance.** The app signature (`application/x-vnd.haicode`) carries
`B_SINGLE_LAUNCH`: launching HaiCode again while it is running does not start a
second process — the existing instance is activated and, if the new launch named a
project directory (command line, Tracker "Open With", or drag-onto-icon), that
directory is forwarded to the running window and becomes the active project.

**Disruptive changes ask first.** Saving settings, changing providers, or switching
the project directory replaces the engine, which stops every running session. When
any session is running, HaiCode asks first: it lists them all (including background
sessions) and offers *Cancel* (the default — nothing changes) or *Interrupt and
Apply*. An accepted change stops all runs deliberately.

## Modes

Every session runs in one of three modes, and mode outranks every permission grant:

| Mode | What it can do |
|------|----------------|
| **Build** | Full tool access, gated by permission rules — the default working mode. |
| **Plan** | Read, search, review (read-only `git`), and `propose_plan`. Cannot write or run `bash`. |
| **Chat** | Conversation and web research only — zero local access. |

New sessions start in `default_mode` (`"plan"` unless you change it in
[configuration](docs/configuration.md#other-keys)).

## Documentation

| Document | Covers |
|---|---|
| [docs/permissions.md](docs/permissions.md) | Rule layers and precedence, built-in exemptions, bash/git pattern semantics, the approval dialog and Permissions center |
| [docs/configuration.md](docs/configuration.md) | Config files and merge rules, project trust, providers, model database, skills, web tools, build hook, key reference |
| [docs/sessions.md](docs/sessions.md) | Auto-compaction, prompt queueing, `/retry`, image handling, deletion, cleanup, storage housekeeping, autonaming |
| [docs/architecture.md](docs/architecture.md) | `lib/` and `gui/` layout, key headers, SQLite schema, where the deep detail lives |
| [`CLAUDE.md`](CLAUDE.md) | Agent-facing guide to this repository: build/test commands, the test suite, per-tool behavior, implementation constraints |
| [`MODEL_NUMBERS.md`](MODEL_NUMBERS.md) | Sourced model context windows, output caps, and pricing |

## Architecture

Two layers: `lib/` (`libhaicode` — engine, providers, tools, persistence) and
`gui/` (`haicode-gui` — BeAPI frontend; engine threads post events to the window
via `BMessenger`). Key headers live in `lib/include/haicode/`. The full layout is in
[docs/architecture.md](docs/architecture.md).

## License

MIT — see [LICENSE](./LICENSE).
