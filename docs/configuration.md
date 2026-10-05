# Configuration

Two config files, merged at startup:

- **Global:** `B_USER_SETTINGS_DIRECTORY/haicode/config.json`
  (`~/config/settings/haicode/config.json`) — provider keys, default model,
  default mode, model database, `last_directory`, `trusted_projects`.
- **Project:** `<project_dir>/.haicode/config.json` — overlays the global file
  when that project is open.

Parsing and merging live in `lib/src/config/config.cpp`; the trust boundary is
documented in [`lib/include/haicode/config.h`](../lib/include/haicode/config.h).

## How merges work

Merging is **presence-based**: a project config key overrides the global value
only when the project file actually sets it. An absent project key never resets
a global setting (a missing `default_mode` no longer wipes a global `"build"`),
an explicit value equal to the default still counts (booleans can be turned back
*on*), and provider entries merge field by field — a project `base_url` no longer
drops the global `api_key`.

Collections (`permissions`, `instructions`, `skills`) append across layers;
per-key maps (`models`, `vision`, `max_output`, `pricing`, `web_search.api_keys`)
overlay per key.

## Saves are atomic and non-destructive

All config saves go through `update_config_file()`: the app reads the existing
file, **refuses to overwrite one that doesn't parse** (so a hand-edit typo can't
cost you your API keys — the error is shown instead), preserves every key it
doesn't own, and writes through a temp file + rename. Config files and the
ChatGPT OAuth token store are written owner-only (`0600`), and existing secret
files are tightened at startup if another editor left them group-readable.

Settings saves are **scope-aware**: only global-scope keys are written to the
global file (see `global_scope_keys()`), and the build command is project-scoped
— saved to the open project's own `.haicode/config.json`, so one project's build
hook never runs in every project. `permissions` is owned by the Permissions
center's policy editor, which replaces only that array and detects concurrent
semantic edits via a content fingerprint.

## Project trust

A repository's `.haicode/config.json` is treated as untrusted input. Two rules
protect you when you open a repository for the first time:

- **Providers never come from a project.** Credentials and endpoints are
  configured only in your global config — a checked-in config can never re-point
  a provider at another server (which would capture your API key or OAuth token),
  no matter how the project is configured.
- **Authority-granting keys require your consent.** A project's `permissions`
  rules, `build_command`, `agents`, and `web_search.api_keys` are ignored until
  you trust that project. When such keys are present, HaiCode shows exactly what
  they would enable (permission rules including any allow-all rule, the build
  command, agent overrides, which search engines get keys) and asks; **Don't
  Trust** is the default. Trust is recorded per resolved project path in the
  global config (`"trusted_projects"`), and any later change to those keys
  invalidates the record and asks again — cosmetic reformatting does not.

Everything else in a project config (model, default mode, vision overrides,
skills, …) merges as before; untrusted projects lose only the keys that grant
authority.

## Providers

The `"providers"` object maps arbitrary ids to provider configs. Each entry has
a `type` (`"anthropic"`, `"openai"`, `"chatgpt"`, or a flavored OpenAI-compatible
server: `"ollama"`, `"vllm"`, `"openrouter"`, `"lmstudio"`, `"llamacpp"`), an
optional `api_key`, and an optional `base_url`. When `type` is omitted it is
inferred from the id: `"anthropic"` defaults to the Anthropic type, anything else
to OpenAI-compatible. Flavored types get their flavor's default `base_url`
(e.g. `lmstudio` → `http://localhost:1234/v1`).

The `"chatgpt"` type (**ChatGPT (Experimental)** in Settings) signs in with your
ChatGPT account instead of an API key. It uses the Codex CLI backend — unofficial,
may break or violate the ChatGPT terms of service. Until you sign in (Settings →
provider editor), the entry stays in the toolbar dropdown greyed-out with
"(sign in via Settings)". Credentials live in the token store at
`~/config/settings/haicode/openai-auth.json`, never in the provider entry.

`base_url` is the **complete API root** — scheme, host, path prefix, and version
segment. The app appends only the resource path (`/messages`,
`/chat/completions`, `/models`), so the version must be part of `base_url`.
Defaults are `https://api.anthropic.com/v1` and `https://api.openai.com/v1` when
omitted.

```json
{
  "providers": {
    "anthropic": { "type": "anthropic", "api_key": "sk-..." },
    "anthropic-proxy": {
      "type": "anthropic",
      "api_key": "sk-...",
      "base_url": "https://my-proxy.example.com/v1"
    },
    "ollama": { "type": "openai", "base_url": "http://localhost:11434/v1" }
  }
}
```

Env-var fallback applies only to the providers whose ids are literally
`"anthropic"` and `"openai"`: `ANTHROPIC_API_KEY` and `OPENAI_API_KEY`
respectively. An OpenAI-compatible entry with no key but a `base_url` (e.g. a
local Ollama instance) is registered keyless.

In the GUI, **Settings → Preferences** opens a list-based editor where you can
add, edit, and remove providers; changes persist to the global config file. The
provider dropdown in the toolbar is rebuilt dynamically from the config.

## Model database

**Settings → Model Database…** shows everything HaiCode knows about each model —
context window, maximum output, vision support, and prices (USD per 1M tokens) —
and lets you add your own entries or override the built-in ones. Search to find a
model, select it, and click **Edit…** (or double-click). **Add…** creates an
entry for a model HaiCode doesn't know, and **Revert to Built-in** removes your
entry. Leave a field blank to keep the built-in value. Changes apply to the next
request without interrupting running sessions.

Entries are keyed by a model id **or prefix** (`my-local` covers
`my-local-7b`). The longest matching key wins, and your entry wins a tie with a
built-in one. That way a short prefix like `gpt` never shadows the built-in
`gpt-5.5` row. Vision is fail-closed: a model absent from both the built-in table
and your entries is treated as *not* vision-capable. Entries are stored in the
global config file:

```json
{
  "models":     {"my-local": 32768},
  "max_output": {"my-local": 4096},
  "vision":     {"my-local": false},
  "pricing":    {"my-proxy-model": {"input": 1.0, "output": 2.0,
                                    "cache_read": 0.1, "cache_write": 0}}
}
```

A price you set is flat: it replaces any built-in long-context pricing tiers for
that model. Pricing keys may also be scoped `"<provider_id>:<model-prefix>"`.
The built-in tables come from [`MODEL_NUMBERS.md`](../MODEL_NUMBERS.md).

## Skills

A skill is a markdown file with optional `name:` / `description:` frontmatter,
discovered in two places:

- `~/config/settings/haicode/skills/` (global)
- `<project_dir>/.haicode/skills/` (project; a same-named project file shadows
  the global one)

Skills can be packaged as a directory with a `SKILL.md` and supporting files.
Enabled skills are listed in the GUI per session, injected into the system prompt,
and can also be invoked ad hoc by starting a prompt with `/<skill-id> args` —
the framed skill body plus your arguments go on the wire, the verbatim command
stays in the transcript. `"skills"` is the array of skill **filenames** enabled by
default for newly created sessions:

```json
{ "skills": ["git-commit.md", "review-pr.md"] }
```

## Web tools

```json
{
  "web_search": {
    "engine": "ddg_lite",
    "max_results": 5,
    "api_keys": {"exa": "…", "zai": "…"}
  }
}
```

Engines: `ddg_lite` (default), `ddg_html`, `exa`, `zai`. Exa and Z.ai are
API-key services; keys resolve from `api_keys` with an `$EXA_API_KEY` /
`$ZAI_API_KEY` fallback. `web_search.api_keys` is a gated key in project configs
(see Project trust). The tools themselves are exempt from prompting only when no
rule matched — see [Permissions](permissions.md).

## Build hook

Set `build_command` in `<project_dir>/.haicode/config.json` to run a build after
every successful `write` or `edit` tool call:

```json
{ "build_command": "make -C build -j4 2>&1" }
```

If the command exits non-zero, its output is appended to the tool result with a
`[build_hook]` prefix and the result is marked failed, so the model sees compile
errors immediately. After every run the app shows a brief system line — `build ✓`
or `build ✗ (exit N)`. The hook blocks the engine thread; keep it under ~30
seconds.

## Other keys

| Key | Scope | Default | Meaning |
|-----|-------|---------|---------|
| `provider` | global | — | Preferred provider id for new sessions. |
| `model` | global | — | Default model id. |
| `default_mode` | both | `"plan"` | Initial mode for new sessions: `plan`, `build`, `chat`. |
| `agent` / `agents` | project (gated) | — | Named agents: `model`, `system_prompt`, `max_steps`, `permissions`, `color`. |
| `instructions` | both (append) | — | Extra system-prompt text appended per layer. |
| `agents.md` | file, not JSON | — | `<project>/agents.md` (or `claude.md` fallback, case-insensitive) is read verbatim into the system prompt. |
| `thinking_display` | global | `"off"` | `off`, `on`, or `on_while_thinking` for reasoning blocks in the chat view. |
| `vision_fallback` | global | — | `{"provider": "…", "model": "…"}` — a vision-capable model used to describe images for text-only primaries. |
| `trusted_projects` | global (app-owned) | — | Fingerprint map written by the trust prompt. |
| `last_directory` | global (app-owned) | — | Project opened when the GUI starts with no argument. |

Session-level inference overrides (`max_tokens`, `temperature`, `top_p`,
`max_steps`, `reasoning_effort`) are set per session by the GUI/engine, not in
config files.

## See also

- [Permissions](permissions.md) — writing `permissions` arrays
- [Sessions](sessions.md) — compaction and autonaming keys
- Architecture of the config loader: [`CLAUDE.md`](../CLAUDE.md)
