/* sse.c - Server-Sent Events stream parser. C89. */

#include "sse.h"

#include <string.h>

void
sse_init(sse_parser *p, sse_cb cb, void *ctx)
{
    buf_init(&p->line);
    buf_init(&p->event);
    buf_init(&p->data);
    p->have_data   = 0;
    p->last_was_cr = 0;
    p->cb          = cb;
    p->ctx         = ctx;
    p->oom         = 0;
}

void
sse_free(sse_parser *p)
{
    buf_free(&p->line);
    buf_free(&p->event);
    buf_free(&p->data);
}

void
sse_reset(sse_parser *p)
{
    buf_clear(&p->line);
    buf_clear(&p->event);
    buf_clear(&p->data);
    p->have_data   = 0;
    p->last_was_cr = 0;
    p->oom         = 0;
}

/* Dispatch the accumulated event, then reset per-event state.
 *
 * Per the EventSource algorithm: an event with no data: field is not
 * dispatched, and the event type resets to empty afterwards. */
static void
sse_dispatch(sse_parser *p)
{
    char *d;

    if (!p->have_data) {
        buf_clear(&p->event);
        buf_clear(&p->data);
        return;
    }

    d = buf_cstr(&p->data);
    if (d == NULL) {
        p->oom = 1;
    } else if (p->cb != NULL) {
        char *e = buf_cstr(&p->event);
        p->cb(p->ctx, (e != NULL) ? e : "", d, p->data.len);
    }

    buf_clear(&p->event);
    buf_clear(&p->data);
    p->have_data = 0;
}

/* Process one complete line (terminator already stripped). */
static void
sse_line(sse_parser *p, const char *s, size_t n)
{
    const char *colon;
    const char *field;
    size_t      flen;
    const char *value;
    size_t      vlen;
    size_t      i;

    if (n == 0) {
        sse_dispatch(p);
        return;
    }
    if (s[0] == ':')
        return;   /* comment (also the usual keep-alive ping) */

    colon = NULL;
    for (i = 0; i < n; i++) {
        if (s[i] == ':') {
            colon = s + i;
            break;
        }
    }

    if (colon == NULL) {
        field = s;
        flen  = n;
        value = s + n;
        vlen  = 0;
    } else {
        field = s;
        flen  = (size_t)(colon - s);
        value = colon + 1;
        vlen  = n - flen - 1;
        /* A single leading space after the colon is part of the framing. */
        if (vlen > 0 && value[0] == ' ') {
            value++;
            vlen--;
        }
    }

    if (flen == 5 && memcmp(field, "event", 5) == 0) {
        buf_clear(&p->event);
        if (buf_append(&p->event, value, vlen) != 0)
            p->oom = 1;
        return;
    }
    if (flen == 4 && memcmp(field, "data", 4) == 0) {
        if (p->have_data) {
            if (buf_putc(&p->data, '\n') != 0)
                p->oom = 1;
        }
        if (buf_append(&p->data, value, vlen) != 0)
            p->oom = 1;
        p->have_data = 1;
        return;
    }
    /* id: and retry: are accepted and ignored -- neither matters for a
     * request-scoped completion stream. Unknown fields are ignored per spec. */
}

int
sse_feed(sse_parser *p, const char *data, size_t n)
{
    size_t i = 0;
    size_t start;

    while (i < n) {
        char c = data[i];

        /* A CR at the end of the previous feed may be half of a CRLF. */
        if (p->last_was_cr) {
            p->last_was_cr = 0;
            if (c == '\n') {
                i++;
                continue;
            }
        }

        if (c == '\n' || c == '\r') {
            char *line;

            line = buf_cstr(&p->line);
            if (line == NULL) {
                p->oom = 1;
                return -1;
            }
            sse_line(p, line, p->line.len);
            buf_clear(&p->line);

            if (c == '\r')
                p->last_was_cr = 1;
            i++;
            continue;
        }

        /* Copy the run up to the next terminator in one append. */
        start = i;
        while (i < n && data[i] != '\n' && data[i] != '\r')
            i++;
        if (buf_append(&p->line, data + start, i - start) != 0) {
            p->oom = 1;
            return -1;
        }
    }

    return p->oom ? -1 : 0;
}

void
sse_finish(sse_parser *p)
{
    if (p->line.len > 0) {
        char *line = buf_cstr(&p->line);
        if (line != NULL)
            sse_line(p, line, p->line.len);
        buf_clear(&p->line);
    }
    sse_dispatch(p);
}
