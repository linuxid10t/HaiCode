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

On Haiku:

```bash
pkgman install cmake git sqlite_devel curl_devel ncurses_devel
```

The Haiku BeAPI headers come with the standard `gcc`/`g++` devel install.

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
      "base_url": "https://my-proxy.example.com"
    },
    "ollama": {
      "type": "openai",
      "base_url": "http://localhost:11434"
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

## License

MIT — see [LICENSE](./LICENSE).
