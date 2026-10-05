# Permissions

Every tool call an agent makes passes a permission gate. This document describes
what the gate does, how rules are matched, and how to configure policy.

## Evaluation order

Authorization is layered and evaluated per session. The first decisive layer
wins:

1. **Exact temporary grants** — literal (category, target) pairs approved via
   "Allow this target for this session". They match the displayed text exactly,
   never glob-expand, and last until revoked or HaiCode exits.
2. **Session pattern grants** — legacy pattern grants created by older
   "Allow Always" flows.
3. **Session toggles** — the Permissions center's per-session switches:
   *Automatically allow writes*, *Allow reads everywhere*, and *Bypass permission
   prompts* (confirmed, not a sandbox; mode and offline restrictions still
   apply). Persisted per session, restored on reopen. Toggles are mode-scoped:
   write presets arm only in Build mode (Plan is non-destructive, Chat has no
   local access), and the reads-everywhere toggle arms in Build and Plan —
   outside-project reads gate and prompt in both. Switching modes re-derives the
   armed rules, so a dormant flag never waits to fire.
4. **Configured rules** — `permissions` arrays in the global
   (`~/config/settings/haicode/config.json`) and project
   (`<project>/.haicode/config.json`) config files, editable in the Permissions
   center's Policies tab. Within one source the last matching rule wins; an
   `Ask` rule falls through to prompting. Project-file rules only count once the
   project is trusted (see [Project trust](configuration.md#project-trust)).

## Mode sits above every layer

Above all of these sits the session's **mode**, which no grant can override: it
is checked before any rule layer, so Plan can never write (its `git` runs only
read-only invocations, for reviewing history and diffs) and Chat can never touch
the local system regardless of toggles, grants, or configured rules.

| Mode | Writes / `bash` | Reads | Local `git` |
|------|-----------------|-------|-------------|
| **Build** | allowed (gated by rules) | gated by rules | full |
| **Plan** | blocked — `[mode restriction]` | gated by rules | read-only invocations only |
| **Chat** | blocked | blocked | not offered |

## Built-in exemptions are fallbacks, not overrides

Read-only tools inside the project directory (and Haiku's system header and
documentation roots, which stay absolute) are allowed without prompting **only
when no rule layer matched**. A Deny or Ask rule written in the Policies tab on
`web_search`/`web_extract`, `screenshot`, `process`, read-only `git`, or
in-project reads takes effect — an `Ask` rule routes the call through the normal
approval prompt.

What the fallback layer covers (`ToolRegistry::evaluate`,
`lib/src/permission/permission.cpp`): `read`, `ls`, `grep`, `diff`, `find`,
`symbols` inside the session's working directory; `glob` when its literal prefix
before the first wildcard resolves inside the project; read-only `git`
invocations; `web_search`/`web_extract`; `propose_plan`, `todo_write`,
`ask_user`; `screenshot`; and `process list`/`check_port`.

Two details worth knowing when writing rules:

- Containment is symlink-aware (`path_resolves_within`: realpath on both sides),
  so an in-project symlink pointing outside the tree stays gated. `read` opens
  with `O_NOFOLLOW` and re-checks the resolved target on `ELOOP`.
- `glob` and `grep` report the **`read` permission action**, not their tool
  name — a rule scoping them must use `{"action": "read", ...}`.
- The always-readable system roots
  (`/boot/system/develop/headers`, `/boot/system/non-packaged/develop/headers`,
  `/boot/system/documentation`) are hardcoded and no rule can deny them.

## Bash pattern rules are segment-aware

An Allow rule whose target contains glob characters must cover *every* `;`,
`&&`, `||`, `|`, and newline-separated command in the invocation — `make*`
allows `make -j4` but not `make; rm -rf ~` (that one prompts). A command
containing command substitution (`$(…)`, backticks) never matches a pattern and
always prompts. Deny patterns match the whole command string, and literal
(glob-free) targets stay exact-match. The bypass-prompts toggle still covers
everything.

Matching logic lives in `bash_pattern_authorizes()`
(`lib/src/permission/permission.cpp`).

## Git rule granularity

One granularity limitation remains on `git`: an Allow rule for a subcommand
(e.g. `branch`) also covers its mutating forms (`branch -D`). Read-only
invocations are classified by `git_invocation_is_readonly(subcommand, args,
working_dir)` — subcommand *and* arguments, fail-closed — which gates unsafe
long options (`--output`, `--ext-diff`, `--textconv`, `--contents`, …) including
their abbreviations (`blame --content=` expands to `--contents`), unsafe short
clusters (`grep -O`, `blame -S`), and `diff` paths resolving outside the project.
`range-diff` is never read-only. `branch`/`tag` count as read-only only in
listing forms; `stash` only via `list`/`show`.

Read-only invocations additionally run with repo-local hooks, fsmonitor,
`diff.external`, and textconv drivers disabled, so a hostile repository cannot
get a script executed by `git diff` through the bypass path.

## The approval dialog

The approval window explains the operation in plain language (tool, category,
full selectable target, tool-specific previews, outside-project and build-hook
warnings). **Deny is always the default**: Enter, Escape, and closing the window
all deny. Interrupting a session denies its pending approvals, so a wait can
never hang the engine.

## The Permissions center

Settings → Permissions… shows pending requests, every temporary grant with
revoke, the rule editors, an operation inspector that reports the real decision
path without executing, and a per-session activity log of authorization outcomes
since launch.

For quick changes, the prompt-row Permissions dropdown offers the common presets
— Standard, Auto-write, YOLO — and the allow-reads-everywhere toggle without
opening the center. Its item set is rebuilt per mode, so it only ever offers what
the current mode can act on:

- **Build** — the write presets plus the read toggle (grayed while YOLO is
  selected, since allow-all covers reads).
- **Plan** — the read toggle only (status: Standard, or All reads when the toggle
  is on).
- **Chat** — the dropdown is hidden entirely; nothing in Chat can touch the local
  system, so there is nothing to configure.

The closed field is a fixed compact width sized to the largest status word; the
open menu sizes itself to its items.

## See also

- Configuring `permissions` arrays: [Configuration](configuration.md)
- Gate implementation and its test coverage: [`CLAUDE.md`](../CLAUDE.md)
  ("Key constraints", `test_config_permission`)
