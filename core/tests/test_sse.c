/* test_sse.c - SSE parser, with emphasis on state surviving chunk splits. */

#include "../sse.h"
#include "../buf.h"
#include "tap.h"

#include <stdlib.h>
#include <string.h>

#define MAX_EV 32

typedef struct {
    char  event[MAX_EV][64];
    char  data[MAX_EV][512];
    size_t dlen[MAX_EV];
    int   n;
} collector;

static void
collect(void *ctx, const char *event, const char *data, size_t dlen)
{
    collector *c = (collector *)ctx;
    size_t     keep;

    if (c->n >= MAX_EV)
        return;
    keep = dlen;

    strncpy(c->event[c->n], event, sizeof(c->event[0]) - 1);
    c->event[c->n][sizeof(c->event[0]) - 1] = '\0';
    if (keep > sizeof(c->data[0]) - 1)
        keep = sizeof(c->data[0]) - 1;
    memcpy(c->data[c->n], data, keep);
    c->data[c->n][keep] = '\0';
    c->dlen[c->n] = dlen;          /* true length, before the test truncates */
    c->n++;
}

static void
test_basic(void)
{
    collector  c;
    sse_parser p;
    const char *s =
        "event: message\n"
        "data: hello\n"
        "\n"
        "data: one\n"
        "data: two\n"
        "\n"
        ": keep-alive comment\n"
        "\n"
        "data: [DONE]\n"
        "\n";

    memset(&c, 0, sizeof(c));
    sse_init(&p, collect, &c);
    sse_feed(&p, s, strlen(s));

    EQLONG(c.n, 3, "three events dispatched");
    EQSTR(c.event[0], "message", "event name");
    EQSTR(c.data[0], "hello", "single data line");
    EQSTR(c.event[1], "", "event name resets between events");
    EQSTR(c.data[1], "one\ntwo", "multi-line data joined with LF");
    EQSTR(c.data[2], "[DONE]", "terminator event");

    sse_free(&p);
}

/*
 * The regression this whole file exists for. Feeding one byte per call is the
 * pathological case of a network read splitting an event; every field, every
 * line ending and every event boundary must still reassemble.
 */
static void
test_byte_at_a_time(void)
{
    collector  c;
    sse_parser p;
    const char *s =
        "event: content_block_delta\r\n"
        "data: {\"type\":\"text_delta\",\"text\":\"a long-ish chunk \"}\r\n"
        "\r\n"
        "event: message_stop\r\n"
        "data: {}\r\n"
        "\r\n";
    size_t i;

    memset(&c, 0, sizeof(c));
    sse_init(&p, collect, &c);
    for (i = 0; i < strlen(s); i++)
        sse_feed(&p, s + i, 1);

    EQLONG(c.n, 2, "byte-at-a-time yields both events");
    EQSTR(c.event[0], "content_block_delta", "event survives byte splitting");
    EQSTR(c.data[0], "{\"type\":\"text_delta\",\"text\":\"a long-ish chunk \"}",
          "data payload intact");
    EQSTR(c.event[1], "message_stop", "second event");

    sse_free(&p);
}

/* Every possible split point of a two-event stream must produce the same
 * result as feeding it whole. */
static void
test_all_split_points(void)
{
    const char *s =
        "event: a\ndata: first\n\nevent: b\ndata: second\n\n";
    size_t len = strlen(s);
    size_t cut;
    int    bad = 0;

    for (cut = 0; cut <= len; cut++) {
        collector  c;
        sse_parser p;

        memset(&c, 0, sizeof(c));
        sse_init(&p, collect, &c);
        sse_feed(&p, s, cut);
        sse_feed(&p, s + cut, len - cut);

        if (c.n != 2 ||
            strcmp(c.event[0], "a") != 0 ||
            strcmp(c.data[0], "first") != 0 ||
            strcmp(c.event[1], "b") != 0 ||
            strcmp(c.data[1], "second") != 0) {
            bad++;
        }
        sse_free(&p);
    }
    EQLONG(bad, 0, "all split points agree");
}

/* A CRLF straddling two feeds must not be seen as two line endings. */
static void
test_crlf_split(void)
{
    collector  c;
    sse_parser p;

    memset(&c, 0, sizeof(c));
    sse_init(&p, collect, &c);
    sse_feed(&p, "data: x\r", 8);
    sse_feed(&p, "\n\r\n", 3);

    EQLONG(c.n, 1, "CRLF split across feeds is one line ending");
    EQSTR(c.data[0], "x", "payload correct");

    sse_free(&p);
}

static void
test_line_ending_variants(void)
{
    collector  c;
    sse_parser p;
    const char *lf   = "data: a\n\n";
    const char *crlf = "data: b\r\n\r\n";
    const char *cr   = "data: c\r\r";

    memset(&c, 0, sizeof(c));
    sse_init(&p, collect, &c);
    sse_feed(&p, lf, strlen(lf));
    sse_feed(&p, crlf, strlen(crlf));
    sse_feed(&p, cr, strlen(cr));

    EQLONG(c.n, 3, "LF, CRLF and bare CR all terminate lines");
    EQSTR(c.data[0], "a", "LF");
    EQSTR(c.data[1], "b", "CRLF");
    EQSTR(c.data[2], "c", "bare CR");

    sse_free(&p);
}

static void
test_field_forms(void)
{
    collector  c;
    sse_parser p;
    const char *s =
        "data:no-space\n"
        "\n"
        "data:  two-spaces\n"
        "\n"
        "data\n"
        "\n"
        "id: 7\n"
        "retry: 100\n"
        "unknown: ignored\n"
        "data: after-ignored\n"
        "\n";

    memset(&c, 0, sizeof(c));
    sse_init(&p, collect, &c);
    sse_feed(&p, s, strlen(s));

    EQLONG(c.n, 4, "four events");
    EQSTR(c.data[0], "no-space", "colon with no space");
    EQSTR(c.data[1], " two-spaces", "only one leading space is stripped");
    EQSTR(c.data[2], "", "bare field name yields empty data");
    EQSTR(c.data[3], "after-ignored", "id/retry/unknown fields ignored");

    sse_free(&p);
}

/* An event with no data: field is not dispatched. */
static void
test_no_data_no_dispatch(void)
{
    collector  c;
    sse_parser p;
    const char *s = "event: ping\n\nevent: x\ndata: y\n\n";

    memset(&c, 0, sizeof(c));
    sse_init(&p, collect, &c);
    sse_feed(&p, s, strlen(s));

    EQLONG(c.n, 1, "data-less event is not dispatched");
    EQSTR(c.event[0], "x", "and does not leak its event name forward");

    sse_free(&p);
}

/* A server that just closes the connection leaves a final unterminated event. */
static void
test_finish_flushes(void)
{
    collector  c;
    sse_parser p;
    const char *s = "data: trailing";

    memset(&c, 0, sizeof(c));
    sse_init(&p, collect, &c);
    sse_feed(&p, s, strlen(s));
    EQLONG(c.n, 0, "nothing dispatched before finish");
    sse_finish(&p);
    EQLONG(c.n, 1, "finish flushes the pending event");
    EQSTR(c.data[0], "trailing", "pending payload delivered");

    sse_free(&p);
}

static void
test_reset(void)
{
    collector  c;
    sse_parser p;

    memset(&c, 0, sizeof(c));
    sse_init(&p, collect, &c);
    sse_feed(&p, "data: half", 10);
    sse_reset(&p);
    sse_feed(&p, "data: whole\n\n", 13);

    EQLONG(c.n, 1, "one event after reset");
    EQSTR(c.data[0], "whole", "stale partial line discarded by reset");

    sse_free(&p);
}

/* A payload larger than any plausible socket read, split arbitrarily. */
static void
test_large_payload(void)
{
    collector   c;
    sse_parser  p;
    buf         s;
    buf         want;
    size_t      i;
    size_t      off;

    buf_init(&s);
    buf_init(&want);
    buf_puts(&s, "data: ");
    for (i = 0; i < 5000; i++) {
        buf_putc(&s, (char)('a' + (i % 26)));
        buf_putc(&want, (char)('a' + (i % 26)));
    }
    buf_puts(&s, "\n\n");

    memset(&c, 0, sizeof(c));
    sse_init(&p, collect, &c);
    for (off = 0; off < s.len; off += 137)
        sse_feed(&p, s.data + off,
                 (off + 137 <= s.len) ? 137 : s.len - off);

    EQLONG(c.n, 1, "large payload yields one event");
    EQLONG(c.dlen[0], 5000, "all 5000 bytes reassembled across 137-byte feeds");
    OK(memcmp(c.data[0], want.data, 511) == 0, "leading bytes correct");

    sse_free(&p);
    buf_free(&s);
    buf_free(&want);
}

int
main(void)
{
    test_basic();
    test_byte_at_a_time();
    test_all_split_points();
    test_crlf_split();
    test_line_ending_variants();
    test_field_forms();
    test_no_data_no_dispatch();
    test_finish_flushes();
    test_reset();
    test_large_payload();
    TAP_REPORT("test_sse");
}
