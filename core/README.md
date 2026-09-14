# core/ — portable C89 layer

The compiler-independent half of the OS/2 client. Nothing here knows about
OS/2, sockets, or Presentation Manager, so it builds and runs on a modern host
and compiles unchanged with VisualAge C++, Borland C++ or Open Watcom on the
target.

This is the part worth developing here rather than inside a Warp 3 VM: the
awkward bugs are all in chunk-boundary handling and text encoding, and they are
far easier to find with a sanitizer and a scriptable server than with a 1995
debugger.

| File | Lines | What it does |
|------|-------|--------------|
| `buf.c` | 110 | growable byte buffer, sticky OOM flag |
| `json.c` | 1070 | arena + DOM parser + **sink-based** writer |
| `sse.c` | 190 | Server-Sent Events framing |
| `http.c` | 520 | HTTP/1.1 request/response, chunked transfer |

## Build

```sh
make          # library objects
make check    # unit tests (no network)
make live     # end-to-end against a local fake llama.cpp server
```

`CFLAGS` defaults to `-std=c89 -pedantic -Wall -Wextra -Werror`. Keep it that
way: the target compilers are 1993–1995 vintage and will not forgive C99-isms.
The suite passes clean under GCC 13, Clang, and ASan + UBSan with leak
detection.

## Two design decisions worth not undoing

**The JSON writer is sink-based.** It never materializes a document; bytes go
straight to a callback. A 256K-token conversation is roughly 1.4 MB of request
body, and assembling that in memory on a 16 MB machine — alongside the
transcript it was built from — is how you run that machine out of RAM.
`http_req_body_write` has exactly the `json_sink` signature, so the emitter
writes onto the socket directly.

To get a `Content-Length` without buffering, emit twice: once into
`json_count_sink` to measure, once into the socket. `test_request_two_pass`
asserts the two passes agree byte for byte. Chunked request encoding is
supported as an alternative (`http_req_body_begin(&r, -1)`) but needs a server
that accepts chunked request bodies; the two-pass form works everywhere.

**All parser state lives in the parser struct, never in locals.** A single SSE
event routinely straddles several network reads once tool-call arguments get
large. Keeping the partial line in a local silently truncates the event, which
surfaces much later as unparseable tool input rather than as a network error —
the Haiku build shipped that bug and it was not obvious. `sse_parser` and
`http_resp` both carry their line accumulators and CR/LF state across calls.

The tests pin this down hard: `test_all_split_points` feeds a stream at every
possible split, `test_byte_at_a_time` feeds one byte per call, and
`test_full_stack` drives HTTP → SSE → JSON at every read size from 1 to 40.

## Notes for the target compilers

- **509-char string literals.** C89 only requires that much, and old compilers
  enforce it. Library sources stay inside the limit. The test fixtures do not
  (suppressed via `-Wno-overlength-strings`); if a target compiler rejects
  them, build those strings at runtime instead.
- **`tests/test_live.c` is host-only scaffolding.** It is the one file that
  calls POSIX sockets, and the Makefile relaxes `-std=c89` to `-std=gnu89` for
  it alone. The OS/2 socket layer is a separate `sock.c` and differs in ways
  that bite: `sock_init()` must be called before anything else, handles are not
  file descriptors, `close()` must be `soclose()`, `errno` is not set (use
  `sock_errno()`), and `select()` works only on sockets.
- **`http_resp` carries its own 2 KB read buffer** rather than putting one on
  the stack, because OS/2 threads are created with small stacks.
- No `long long`, no `//` comments, no declarations after statements, no
  `snprintf`. Every `sprintf` writes a bounded numeric conversion into a
  fixed buffer with headroom.

## What is not here yet

`sock.c` (OS/2 sockets), `provider.c` (request building and stream decoding for
the OpenAI-compatible `/v1/chat/completions` shape), `loop.c` (the agentic
loop), `store.c` (JSONL sessions), and the tools. Those come next; the UI layer
is separate and is the only part that cares which compiler you picked.
