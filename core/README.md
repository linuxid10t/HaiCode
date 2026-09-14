# core/ — the portable layer

Everything the OS/2 client does except draw pixels. One file (`plat_os2.c`)
knows about OS/2; the other twelve are strict C89 and build unchanged with
VisualAge C++, Borland C++ or Open Watcom.

This is the half worth developing on a modern machine. The bugs here are all in
chunk-boundary handling, text encoding and path arithmetic, and they are far
cheaper to find with a sanitizer and a scriptable server than with a 1995
debugger inside a Warp 3 VM.

| File | Lines | What it does |
|------|-------|--------------|
| `buf.c` | 110 | growable byte buffer, sticky OOM flag |
| `json.c` | 1090 | arena + DOM parser + sink-based writer |
| `sse.c` | 190 | Server-Sent Events framing |
| `http.c` | 520 | HTTP/1.1, chunked transfer both directions |
| `provider.c` | 490 | OpenAI-compatible requests and stream decoding |
| `path.c` | 300 | normalisation, containment, globbing |
| `perm.c` | 140 | permission gate |
| `store.c` | 560 | sessions as directories of JSONL |
| `tool.c` | 170 | registry and gated dispatch |
| `tools.c` | 770 | read, write, edit, ls, grep, cmd |
| `config.c` | 210 | two-file JSON config |
| `loop.c` | 385 | the agentic loop |
| `plat_posix.c` | 330 | host platform layer (dev only) |
| `plat_os2.c` | 375 | **OS/2 platform layer — never compiled** |

## Build

```sh
make           # objects
make check     # 454 assertions, no network
make live      # end-to-end over a real socket against a fake llama.cpp
make portable  # proves the shipping sources are strict C89
```

Clean under GCC 13, Clang, and ASan + UBSan with leak detection, at
`-std=c89 -pedantic -Wall -Wextra -Werror`.

`plat_posix.c` and the tests are built with `-std=gnu89 -D_POSIX_C_SOURCE`,
because `-std=c89` hides POSIX headers behind `__STRICT_ANSI__`. The C89
*language* rules still apply; only the header gating is lifted, and `make
portable` checks the shipping sources independently of the tests.

## Decisions worth not undoing

**The JSON writer is sink-based.** It never materialises a document. A
256K-token conversation is ~1.4 MB of request body, and assembling that in
memory on a 16 MB machine — alongside the transcript it was built from — is how
you run that machine out of RAM. `http_req_body_write` has exactly the
`json_sink` signature, so the emitter writes onto the socket directly. To get a
`Content-Length` without buffering, emit twice: once into `json_count_sink` to
measure, once to send. Tests assert the two passes agree byte for byte.

**Messages stream off disk.** `prov_msgsrc` is an iterator with `rewind` and
`next`, and `store_iter` implements it, so building a request holds one message
at a time. The rewind hook exists because the two-pass scheme emits twice;
`prov_write_request` calls it itself so the contract is hard to get wrong.

**All parser state lives in the parser struct, never in locals.** A single SSE
event routinely straddles several network reads once tool-call arguments get
large. Keeping the partial line in a local silently truncates the event, which
surfaces much later as unparseable tool input rather than as a network error —
the Haiku build shipped that bug. `sse_parser` and `http_resp` both carry their
line accumulators and CR/LF state across calls, and the tests pin it down at
every split point, one byte at a time, and at every read size from 1 to 48.

**`path_within()` is a security boundary, not a helper.** Read-only tools skip
the permission prompt when their target is inside the working directory, and
that exemption is only sound because containment is checked *after* `..` is
resolved and *on component boundaries*. A prefix compare would admit both
`proj/../../etc/passwd` and `/project2/x`. Case folding is explicit and
compile-time selected, because OS/2 filesystems are case-insensitive and POSIX
ones are not — getting that backwards makes the check too permissive on one of
them. `path_normalize()` deliberately preserves case, since its output is used
to actually open files.

**`sprintf` only ever formats numbers.** Every longer string is assembled with
`buf_puts`, which cannot overrun. C89 has no `snprintf`, and GCC caught a real
overflow in an earlier draft of `tools.c` that had mixed the two.

## Notable behaviours

- **The system prompt is injected, not stored.** Editing config changes it for
  existing sessions, and history stays a pure record of the conversation.
- **A half-received tool call is discarded, never dispatched.** Running the
  wrong thing from truncated arguments is worse than losing a turn.
  `prov_calls_valid()` is what decides.
- **Denials and unparseable arguments go back to the model as tool results**,
  so it can adapt instead of waiting on a result that never arrives.
- **The assistant turn is persisted with its `tool_calls` intact**, so the next
  request replays them alongside the matching tool result. Dropping them
  produces orphaned tool results and makes the model redo work — the failure
  `CLAUDE.md` documents for the Haiku build. `test_tool_cycle_replays_correctly`
  exists for this one case.
- **An ambiguous `edit` is an error.** Silently editing the first of several
  identical matches is how an agent corrupts a file.
- **`ls` and `grep` walk directories themselves** rather than shelling out.
  A stock OS/2 install has no `grep(1)` or `find(1)`, and depending on ported
  GNU utilities would drag a package stack onto the target for something a
  directory walk does in 200 lines.
- **A crash-truncated JSONL line is skipped, not fatal.** Losing one message is
  recoverable; losing the conversation is not.
- **No ask callback means deny.** Unprompted defaults must fail closed.

## Notes for the target compilers

- **509-char string literals.** C89 only requires that much and old compilers
  enforce it. Shipping sources stay inside the limit; the test fixtures do not
  (suppressed via `-Wno-overlength-strings`). If a target compiler rejects
  them, build those strings at runtime.
- **`plat_os2.c` has never been compiled.** It is written against the Control
  Program API and uses only `Dos*` calls plus ANSI C, which all three
  compilers provide, but expect to fix header names, `PSZ` casts and at least
  one `DosFindFirst` argument. Its header comment lists the traps that cost
  time: `INCL_*` before `<os2.h>`, `HDIR_CREATE`, trailing separators breaking
  `DosQueryPathInfo`, and `DosMove` refusing to overwrite.
- **`plat_run` on OS/2 redirects to a temp file** rather than using `popen`,
  which the three compilers disagree about. Its `timeout` argument is accepted
  and ignored there — do not rely on it to bound anything.
- **`http_resp` carries its own 2 KB read buffer** rather than putting one on
  the stack, because OS/2 threads get small stacks.
- No `long long`, no `//`, no declarations after statements, no `snprintf`.

## What is not here

`sock.c` (OS/2 sockets: `sock_init()` first, handles are not file descriptors,
`soclose()` not `close()`, `sock_errno()` not `errno`, `select()` on sockets
only) and the Presentation Manager UI. `loop_net` is the seam the first plugs
into; the tests fill it with a scripted in-process server, so every decision in
`loop.c` is already exercised.
