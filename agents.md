# Project

HaiCode — a native coding-agent app for **Haiku R1**. C++20 core library
(`lib/`) with a `haicode-gui` (Haiku BeAPI) frontend. Talks to Anthropic
and OpenAI-compatible LLM providers, runs
tools with per-action permissions, and persists every session to SQLite.

# Build & run

```bash
cmake -B build -S .            # re-run after adding a new .cpp source
make -C build -j4              # builds lib + gui + tests

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
- [ ] Handle event in GUI relay
```

Rules:
- Use `- [ ] verb + object` phrasing (imperative, e.g. "Add", "Fix", "Update").
- One item per file or logical unit — not one item per line changed.
- Do not pre-check items (`- [x]`); the harness sets all items to `pending`.
- Sections after `## Tasks` terminate the list, so keep it last.

# Conventions

- **Provider registration is generic.** `AppConfig::providers` is a
  `map<id, ProviderConfig>`; each `ProviderConfig` has a `type` of
  `"anthropic"`, `"openai"`, `"chatgpt"`, or one of the flavored
  OpenAI-compatible servers (`"ollama"`, `"vllm"`, `"openrouter"`,
  `"lmstudio"`, `"llamacpp"`; inferred from the id when empty —
  `"anthropic"` → anthropic, `"chatgpt"` → chatgpt, else openai). Both
  frontends iterate this map to register providers. Env-var fallback
  (`ANTHROPIC_API_KEY`, `OPENAI_API_KEY`) applies only to ids literally
  `"anthropic"`/`"openai"`. Flavored types use
  `make_openai_compat_provider()` with a flavor-specific default `base_url`
  (e.g. `lmstudio` → `http://localhost:1234/v1`).
- **The `chatgpt` provider type is OAuth-based, not key-based.** It uses
  `make_codex_provider()` (`lib/src/provider/codex.cpp`), registered only
  when `codex_auth_signed_in()`; credentials live in the codex_auth token
  store at `$(B_USER_SETTINGS_DIRECTORY)/haicode/openai-auth.json`, never
  in `ProviderConfig` (key/base_url stay empty). The OAuth access token is
  audience-locked to the ChatGPT Codex backend, so the provider speaks the
  **Responses API** (`{base}/codex/responses`, default base
  `https://chatgpt.com/backend-api`) — NOT chat/completions — with
  `Authorization`, `chatgpt-account-id`, `originator: codex_cli_rs`, and
  `OpenAI-Beta: responses=experimental` headers. Auth flow lives in
  `lib/src/auth/codex_auth.cpp` (PKCE + loopback:1455 callback server +
  refresh-on-skew + one forced 401-retry). Usage is subscription-billed,
  so there are no built-in pricing entries; the cost column stays empty
  unless the user adds a `pricing` override (lookup miss is handled).
- **Provider menu items carry their id.** Each `BMenuItem` in MainWindow's
  provider dropdown attaches `provider_id` to its `BMessage`; the
  `MSG_FETCH_MODELS` handler reads it from the message, never from the label.
  `SelectProvider()` matches by exact id — do not coerce to two values.
- **Pricing is keyed `"<provider_id>:<model-prefix>"`.** Custom-id Anthropic
  providers (e.g. `"my-proxy"`) do not match the built-in
  `"anthropic:claude-..."` entries; the `pricing` config override is the escape
  hatch until pricing is keyed on type instead of id.
- **Compaction never deletes rows.** Context compaction is checkpoint-based:
  `compaction_checkpoint` rows record `through_seq` boundaries; the full
  conversation stays in `session_message`. Context assembly goes through
  `SessionEngine::load_context_messages` → `apply_checkpoint` (slice
  `seq > through_seq`, prepend the rendered checkpoint block). Pure logic
  lives in `lib/src/compaction/compaction.cpp`.
- Style: minimal comments (only for non-obvious why), 4-space indent, Haiku
  BeAPI naming (`BWindow`, `BMessage`, `BMessenger`). See `CLAUDE.md` for the
  deeper engine/pricing/permission walkthrough.
