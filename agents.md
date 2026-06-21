# Project

HaiCode — a native coding-agent app for **Haiku R1**. C++20 core library
(`lib/`) with two frontends: `haicode-tui` (ncurses, POSIX) and `haicode-gui`
(Haiku BeAPI). Talks to Anthropic and OpenAI-compatible LLM providers, runs
tools with per-action permissions, and persists every session to SQLite.

# Build & run

```bash
cmake -B build -S .            # re-run after adding a new .cpp source
make -C build -j4              # builds lib + tui + gui + tests

./build/tui/haicode-tui [project_dir]
./build/gui/haicode-gui [project_dir]
```

CMake uses `GLOB_RECURSE`; new source files require a re-configure.

Build hook for this project: `make -C build 2>&1`.

# Plan mode

When proposing a plan, always include a `## Tasks` section with a markdown
checklist as the **last** section. Each item is one atomic step in imperative
form. The harness parses this section on approval and seeds the todo panel
automatically — no need to call `todo_write` at the start of the build turn.

```markdown
## Tasks
- [ ] Add BuildHookResult event to events.h
- [ ] Publish event in engine.cpp
- [ ] Handle event in GUI relay and TUI
```

Rules:
- Use `- [ ] verb + object` phrasing (imperative, e.g. "Add", "Fix", "Update").
- One item per file or logical unit — not one item per line changed.
- Do not pre-check items (`- [x]`); the harness sets all items to `pending`.
- Sections after `## Tasks` terminate the list, so keep it last.

# Conventions

- **Provider registration is generic.** `AppConfig::providers` is a
  `map<id, ProviderConfig>`; each `ProviderConfig` has a `type` of
  `"anthropic"`, `"openai"`, or one of the flavored OpenAI-compatible servers
  (`"ollama"`, `"vllm"`, `"openrouter"`, `"lmstudio"`, `"llamacpp"`; inferred
  from the id when empty — `"anthropic"` → anthropic, else openai). Both
  frontends iterate this map to register providers. Env-var fallback
  (`ANTHROPIC_API_KEY`, `OPENAI_API_KEY`) applies only to ids literally
  `"anthropic"`/`"openai"`. Flavored types use
  `make_openai_compat_provider()` with a flavor-specific default `base_url`
  (e.g. `lmstudio` → `http://localhost:1234/v1`).
- **Provider menu items carry their id.** Each `BMenuItem` in MainWindow's
  provider dropdown attaches `provider_id` to its `BMessage`; the
  `MSG_FETCH_MODELS` handler reads it from the message, never from the label.
  `SelectProvider()` matches by exact id — do not coerce to two values.
- **Pricing is keyed `"<provider_id>:<model-prefix>"`.** Custom-id Anthropic
  providers (e.g. `"my-proxy"`) do not match the built-in
  `"anthropic:claude-..."` entries; the `pricing` config override is the escape
  hatch until pricing is keyed on type instead of id.
- Style: minimal comments (only for non-obvious why), 4-space indent, Haiku
  BeAPI naming (`BWindow`, `BMessage`, `BMessenger`). See `CLAUDE.md` for the
  deeper engine/pricing/permission walkthrough.
