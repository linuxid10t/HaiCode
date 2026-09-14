/* sse.h - Server-Sent Events stream parser.
 *
 * C89. Depends on buf.h.
 *
 * THE POINT OF THIS FILE: all parse state lives in sse_parser and survives
 * across sse_feed() calls. A single SSE event routinely straddles two or more
 * network reads once tool-call arguments get large, and holding the partial
 * line in a local variable silently truncates the event -- which shows up much
 * later as unparseable tool input rather than as a network error. Keep the
 * state here; never reconstruct it per read.
 *
 * Handles CRLF, LF and bare-CR line endings, including a CRLF pair split
 * across a chunk boundary.
 *
 * Usage:
 *     sse_parser p;
 *     sse_init(&p, on_event, ctx);
 *     while ((n = read(...)) > 0)
 *         sse_feed(&p, buffer, n);
 *     sse_finish(&p);          
 *     sse_free(&p);
 */
#ifndef HAIOS2_SSE_H
#define HAIOS2_SSE_H

#include <stddef.h>
#include "buf.h"

/* `event` is the event: field ("" when absent). `data` is NUL-terminated and
 * `dlen` bytes long, with data: lines joined by LF and no trailing newline. */
typedef void (*sse_cb)(void *ctx, const char *event,
                       const char *data, size_t dlen);

typedef struct sse_parser {
    buf     line;        /* partial line carried between feeds */
    buf     event;       /* current event: value  */
    buf     data;        /* accumulated data: values, LF-joined */
    int     have_data;   /* a data: field was seen for this event */
    int     last_was_cr; /* previous byte was CR (may be CRLF split) */
    sse_cb  cb;
    void   *ctx;
    int     oom;
} sse_parser;

void sse_init(sse_parser *p, sse_cb cb, void *ctx);
void sse_free(sse_parser *p);

/* Clears all accumulated state. Call before reusing a parser for a new
 * response; a half-finished event from a previous request must not leak into
 * the next one. */
void sse_reset(sse_parser *p);

/* Feeds `n` bytes. Returns 0, or -1 if an allocation failed (sticky). */
int  sse_feed(sse_parser *p, const char *data, size_t n);

/* Flushes a final event that was not terminated by a blank line, which is what
 * a server that simply closes the connection leaves behind. */
void sse_finish(sse_parser *p);

#endif
