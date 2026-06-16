# HaiCode

A native coding-agent app for **Haiku R1** — TUI (ncurses) and GUI (BeAPI) frontends backed by a C++20 agentic loop. Talks to Anthropic and OpenAI-compatible providers, runs tools with per-action permissions, and persists every session to SQLite.

> Status: early preview. Built and tested on Haiku R1-beta5 / development tip.

## Features

- **Agentic loop** — up to 20 tool-use steps per turn, with atomic interruption between steps.
- **Two frontends** — `haicode-tui` (ncurses, pure POSIX) and `haicode-gui` (Haiku native BeAPI).
- **Eleven built-in tools** — `bash`, `read`, `write`, `edit`, `glob`, `grep`, `ls`, `external_terminal`, `propose_plan`, plus `web_search` and `web_extract` — each with safe argument handling and a 100 KB output cap.
- **Multi-provider** — Anthropic and OpenAI-compatible endpoints, with message-format translation between them.
- **Permissions** — fnmatch rules per session, with an interactive Ask → Allow / Deny / Allow-Always flow.
- **SQLite session history** — every user prompt, assistant message, tool call, and tool result is stored and reloadable. WAL mode + cascading deletes.
- **Project + global config** — global config in `B_USER_SETTINGS_DIRECTORY/haicode/config.json`, project config in `<project>/.haicode/config.json`.

## Prerequisites

On Haiku:

```bash
pkgman install cmake git sqlite_devel curl_devel ncurses_devel
```

The Haiku BeAPI headers come with the standard `gcc`/`g++` devel install.

You'll also need API credentials in your environment:

```bash
export ANTHROPIC_API_KEY=sk-...     # for the Anthropic provider
export OPENAI_API_KEY=sk-...        # for OpenAI-compatible providers
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

## License

MIT — see [LICENSE](./LICENSE).
