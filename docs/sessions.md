# Sessions

How a session behaves over time: context growth, queueing, images, deletion, and
the storage housekeeping that runs around it.

## Sessions and modes

A session owns one conversation, its todo list, its permission grants, and its
own foreground prompt queue. New sessions start in `default_mode`
(`"plan"` unless overridden) and can switch modes at any time. Mode is the
highest authority in the permission gate — see
[Permissions](permissions.md#mode-sits-above-every-layer).

New sessions start with an empty title and are named automatically; see
[Session autonaming](#session-autonaming).

## Auto-compaction (checkpoint-based)

Long sessions grow toward the model's context window. When the input-token usage
reported by the previous step reaches a configurable fraction of the window,
HaiCode summarizes the older portion of the conversation into a checkpoint and
assembles future requests from the checkpoint block plus the retained tail, so
the loop keeps going instead of dying on a context-overflow rejection. The
current user turn is always kept intact.

Compaction **never deletes messages**: the full conversation stays in the
`session_message` table, and a `compaction_checkpoint` row records the
`through_seq` boundary. Context assembly slices `seq > through_seq` and prepends
the rendered checkpoint block (`SessionEngine::load_context_messages`,
`lib/src/compaction/compaction.cpp`). A manual **Compact** action uses the same
path.

Summaries are generated **inline** whenever possible — the conversation's own
next request plus a summarization instruction at the tail — so the provider's
prompt cache covers the whole history; a failed or invalid inline attempt gets one
corrective retry, then falls back to the standalone summarizer.

```json
{
  "auto_compact": true,
  "auto_compact_threshold": 0.80,
  "compaction_buffer": 8192,
  "compaction_recent_context": 10240,
  "compaction_summary_max_tokens": 4096
}
```

| Key | Default | Meaning |
|-----|---------|---------|
| `auto_compact` | `true` | Master switch. |
| `auto_compact_threshold` | `0.80` | Fraction (0.0–1.0) of the window at which compaction triggers. |
| `compaction_buffer` | `8192` | Safety margin held back from the window when computing the trigger. |
| `compaction_recent_context` | `10240` | Token budget for the retained recent tail (half of it is kept verbatim after a checkpoint, since the summary covers the rest). |
| `compaction_summary_max_tokens` | `4096` | Output cap for the summarizer, and the validation budget its reply must fit. |

Compaction is **disabled when the model's context window is unknown**
(`window == 0`), since the threshold cannot be sized safely. Set the window in
**Settings → Model Database…** (or via the top-level `"models"` object, e.g.
`"models": {"my-local-model": 131072}`) to enable compaction for models HaiCode
doesn't recognize.

After each compaction, a collapsible `[context compacted]` transcript entry shows
the checkpoint summary in the chat scrollback (click to toggle in the GUI). It is
rendered from the checkpoint table, never stored as a message, so the model's
context is unaffected. The context-size indicator in the status area also drops
immediately to a post-compaction estimate (checkpoint block + retained tail)
instead of waiting for the next model response to report fresh usage.

## Prompts while running

You can submit another prompt while a session is streaming, executing a tool,
waiting for approval, or compacting. It queues for that session and runs as its
own turn in FIFO order once the current tool exchange is complete. Attachment
payloads and slash-skill content are captured when you submit, not when the
queued turn eventually starts. The status strip shows the queued count without
resetting the active turn's counters or todos.

Interrupt stops the current turn; queued prompts then start with fresh
interruption state. Closing the engine discards queued prompts rather than
restarting work. Each session has an independent foreground queue.

## `/retry`

Typing `/retry` (exact match) in the input deletes the last turn's assistant
output and re-runs the agentic loop on the stored prompt — attachments and skill
invocations included, since they ride the stored prompt row. It is refused while
a turn is streaming, and never stored as a prompt or matched against skills. Any
compaction checkpoint created inside the retried turn is dropped, because its
boundary would otherwise hide the re-run's rows.

## Images

Image attachments are limited to **4 MiB** each, including programmatically
supplied payloads. Unavailable or oversized images are recorded explicitly
instead of silently disappearing. Raw user images and screenshots are sent only
for the current and immediately preceding user turn; older images become text
placeholders or available descriptions. Persisted image bytes remain unchanged,
and expired images are never backfilled.

Text-only models receive descriptions from your configured vision fallback
(`vision_fallback`, see [Configuration](configuration.md#other-keys)), or
placeholders when no description is available — even after compaction. Failed
description attempts are remembered across steps and reopening; interrupt cancels
fallback requests too, and cancelled attempts can be retried.

## Tool-exchange integrity

Every assistant tool batch is stored with a result for every call — including an
explicit failed result for calls skipped after a denial, a plan proposal, or an
interruption. Context assembly groups each batch's responses into one user message
and marks failures, and repairs missing or orphaned results in older histories
without changing stored messages.

## Deleting a session

Deleting a session asks for confirmation first (a running session is interrupted
as part of deletion). The engine retires the session — refusing new prompts,
discarding queued ones, cancelling open approvals and questions — waits for its
work to fully stop, and only then removes the conversation, todos, and history. If
the database delete fails, the session stays usable and the error is shown. Other
sessions, including their permission grants, are untouched.

**File → Cleanup** offers three bulk deletion choices:

| Item | What it removes |
|---|---|
| Delete Empty Sessions… | Sessions with no submitted prompts, regardless of title |
| Delete Inactive Sessions → 7 / 30 / 90 days… | Sessions whose last activity is older than the selected interval |
| Delete All Sessions in This Project… | Every session of the currently selected project directory, including conversations with history |

An untitled session can still contain a conversation, so title alone is not a
cleanup criterion. The sidebar shows each session's directory below its title and
the last-modified time on a third line, refreshed every few seconds while sessions
run. While a session is actively working, an animated spinner shows to the left of
its title — including background sessions. Hover over a row to see the full path.
Cleanup confirmations also show directories.

Each deletion names what it targets, counts how many sessions are running and will
be interrupted, and confirms with *Cancel* as the default — Enter or Escape
cancels. Bulk deletion runs through the same retirement path as a single delete, so
unmatched sessions keep running with their grants intact. Selection is uncapped,
unlike the 50-row sidebar, so sessions outside that list are still included in
cleanup.

## Storage housekeeping

Storage maintenance is automatic, not a menu action. It runs silently at startup
and is scheduled after single or bulk deletion. While the app is running, a
tracked worker checks every 30 seconds until foreground sessions and title jobs are
idle. Failed maintenance is logged and retried later. Database operations share one
connection mutex so maintenance cannot overlap a session transaction.

Housekeeping removes orphaned message/todo/checkpoint rows, stale `pending` or
`failed` compaction attempts, superseded checkpoint context payloads, and scratch
files older than a day (`haicode_shot_*`, `haicode_diff_*`). Summaries, conversation
history, and the complete compaction chain survive. Database compaction runs only
when at least 1 MiB of free pages can be reclaimed; WAL checkpoints before and
after `VACUUM` return that space to the filesystem without deleting conversations.

## Session autonaming

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
   one if the session's focus has shifted. Best-effort: on any error the existing
   title is kept. Refinement is separate, tracked maintenance work: it never keeps
   the session busy or delays your next prompt. Interrupted turns skip it;
   interrupt and shutdown cancel it. A stale result cannot overwrite a newer
   session title.

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

## See also

- [Configuration](configuration.md) — the keys above in context
- [Permissions](permissions.md) — grants that die with a session
