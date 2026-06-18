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

# Conventions

- **Provider registration is generic.** `AppConfig::providers` is a
  `map<id, ProviderConfig>`; each `ProviderConfig` has a `type` of
  `"anthropic"` or `"openai"` (inferred from the id when empty). Both frontends
  iterate this map to register providers. Env-var fallback (`ANTHROPIC_API_KEY`,
  `OPENAI_API_KEY`) applies only to ids literally `"anthropic"`/`"openai"`.
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
