/* test_http.c - HTTP/1.1 framing, plus the full HTTP -> SSE -> JSON stack. */

#include "../http.h"
#include "../sse.h"
#include "../json.h"
#include "../buf.h"
#include "tap.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------- fake transport -------- */

typedef struct {
    const char *in;      /* bytes the "server" will return */
    size_t      inlen;
    size_t      inpos;
    size_t      max_read; /* cap per recv, to simulate small reads */
    buf         sent;     /* bytes the client wrote */
} fake;

static int
fake_send(void *ctx, const char *p, size_t n)
{
    return buf_append(&((fake *)ctx)->sent, p, n);
}

static int
fake_recv(void *ctx, char *p, size_t n)
{
    fake  *f = (fake *)ctx;
    size_t avail = f->inlen - f->inpos;
    size_t take;

    if (avail == 0)
        return 0;
    take = (n < avail) ? n : avail;
    if (f->max_read > 0 && take > f->max_read)
        take = f->max_read;
    memcpy(p, f->in + f->inpos, take);
    f->inpos += take;
    return (int)take;
}

static void
fake_init(fake *f, const char *in, size_t inlen, size_t max_read)
{
    f->in       = in;
    f->inlen    = inlen;
    f->inpos    = 0;
    f->max_read = max_read;
    buf_init(&f->sent);
}

static http_transport
fake_transport(fake *f)
{
    http_transport t;
    t.ctx   = f;
    t.xsend = fake_send;
    t.xrecv = fake_recv;
    return t;
}

/* --------------------------------------------------------- body sink ----- */

static void
body_to_buf(void *ctx, const char *p, size_t n)
{
    buf_append((buf *)ctx, p, n);
}

/* ---------------------------------------------------- wire builders ------ */

/*
 * Builds a chunked response from a list of body pieces. Hand-written hex chunk
 * lengths in a fixture are a bug waiting to happen -- and a wrong one tests the
 * parser's error path rather than the path you meant to test.
 */
static void
chunkify(buf *out, const char *hdrs, const char **pieces, int n)
{
    char tmp[32];
    int  i;

    buf_puts(out, hdrs);
    for (i = 0; i < n; i++) {
        sprintf(tmp, "%lx\r\n", (unsigned long)strlen(pieces[i]));
        buf_puts(out, tmp);
        buf_puts(out, pieces[i]);
        buf_puts(out, "\r\n");
    }
    buf_puts(out, "0\r\n\r\n");
}

/* ------------------------------------------------------------ request ---- */

static void
test_request_content_length(void)
{
    fake      f;
    http_req  r;
    const char *body = "{\"a\":1}";

    fake_init(&f, "", 0, 0);
    http_req_begin(&r, fake_transport(&f), "POST", "/v1/chat/completions",
                   "10.0.0.2:8080");
    http_req_header(&r, "Content-Type", "application/json");
    http_req_body_begin(&r, (long)strlen(body));
    http_req_body_write(&r, body, strlen(body));
    http_req_body_end(&r);

    EQSTR(buf_cstr(&f.sent),
          "POST /v1/chat/completions HTTP/1.1\r\n"
          "Host: 10.0.0.2:8080\r\n"
          "Content-Type: application/json\r\n"
          "Content-Length: 7\r\n"
          "\r\n"
          "{\"a\":1}",
          "request with Content-Length");
    OK(r.err == 0, "no request error");

    http_req_free(&r);
    buf_free(&f.sent);
}

static void
test_request_chunked(void)
{
    fake     f;
    http_req r;

    fake_init(&f, "", 0, 0);
    http_req_begin(&r, fake_transport(&f), "POST", "/x", "h");
    http_req_body_begin(&r, -1);
    http_req_body_write(&r, "abc", 3);
    http_req_body_write(&r, "de", 2);
    http_req_body_end(&r);

    EQSTR(buf_cstr(&f.sent),
          "POST /x HTTP/1.1\r\n"
          "Host: h\r\n"
          "Transfer-Encoding: chunked\r\n"
          "\r\n"
          "3\r\nabc\r\n"
          "2\r\nde\r\n"
          "0\r\n\r\n",
          "chunked request body");

    http_req_free(&r);
    buf_free(&f.sent);
}

/*
 * The two-pass pattern the header documents: count with a discarding sink,
 * then emit the identical bytes onto the wire. If these ever disagree the
 * server sees a truncated or over-long body.
 */
static void
test_request_two_pass(void)
{
    fake        f;
    http_req    r;
    json_writer w;
    long        len = 0;
    char        got[64];
    int         i;

    json_w_init(&w, json_count_sink, &len);
    json_w_obj_open(&w);
    json_w_key(&w, "msg");
    json_w_str(&w, "caf\303\251 \001");
    json_w_obj_close(&w);
    json_w_finish(&w);

    fake_init(&f, "", 0, 0);
    http_req_begin(&r, fake_transport(&f), "POST", "/x", "h");
    http_req_body_begin(&r, len);
    json_w_init(&w, http_req_body_write, &r);
    json_w_obj_open(&w);
    json_w_key(&w, "msg");
    json_w_str(&w, "caf\303\251 \001");
    json_w_obj_close(&w);
    json_w_finish(&w);
    http_req_body_end(&r);

    OK(json_w_finish(&w) == 0, "streamed emit succeeded");

    /* Pull the declared Content-Length back out and compare to the real body. */
    {
        const char *s   = buf_cstr(&f.sent);
        const char *cl  = strstr(s, "Content-Length: ");
        const char *hdr = strstr(s, "\r\n\r\n");
        long        declared = 0;

        OK(cl != NULL && hdr != NULL, "headers well-formed");
        if (cl != NULL) {
            cl += strlen("Content-Length: ");
            for (i = 0; cl[i] >= '0' && cl[i] <= '9'; i++)
                declared = declared * 10 + (cl[i] - '0');
        }
        if (hdr != NULL) {
            size_t actual = strlen(hdr + 4);
            EQLONG(declared, (long)actual,
                   "declared Content-Length equals bytes sent");
            strncpy(got, hdr + 4, sizeof(got) - 1);
            got[sizeof(got) - 1] = '\0';
            EQSTR(got, "{\"msg\":\"caf\303\251 \\u0001\"}", "body bytes");
        }
    }

    http_req_free(&r);
    buf_free(&f.sent);
}

/* ----------------------------------------------------------- response ---- */

static void
test_response_content_length(void)
{
    fake      f;
    http_resp r;
    buf       body;
    char      hv[64];
    static const char wire[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "hello, world!";

    buf_init(&body);
    fake_init(&f, wire, sizeof(wire) - 1, 0);
    http_resp_init(&r, body_to_buf, &body);
    OK(http_pump(&r, fake_transport(&f)) == 0, "pump succeeds");

    EQLONG(r.status, 200, "status 200");
    EQSTR(buf_cstr(&r.reason), "OK", "reason phrase");
    EQSTR(buf_cstr(&body), "hello, world!", "body");
    OK(http_resp_header(&r, "Content-Type", hv, sizeof(hv)) == 1,
       "header lookup is case-insensitive");
    EQSTR(hv, "application/json", "header value");
    OK(http_resp_done(&r), "message complete");

    http_resp_free(&r);
    buf_free(&body);
    buf_free(&f.sent);
}

static void
test_response_chunked(void)
{
    fake      f;
    http_resp r;
    buf       body;
    static const char wire[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n"
        "1\r\n \r\n"
        "6\r\nworld!\r\n"
        "0\r\n\r\n";

    buf_init(&body);
    fake_init(&f, wire, sizeof(wire) - 1, 0);
    http_resp_init(&r, body_to_buf, &body);
    OK(http_pump(&r, fake_transport(&f)) == 0, "chunked pump succeeds");
    EQSTR(buf_cstr(&body), "hello world!", "chunks reassembled");
    OK(http_resp_done(&r), "complete after terminal chunk");

    http_resp_free(&r);
    buf_free(&body);
    buf_free(&f.sent);
}

static void
test_response_chunk_extension(void)
{
    fake      f;
    http_resp r;
    buf       body;
    static const char wire[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "4;name=value\r\nabcd\r\n"
        "0\r\n"
        "X-Trailer: t\r\n"
        "\r\n";

    buf_init(&body);
    fake_init(&f, wire, sizeof(wire) - 1, 0);
    http_resp_init(&r, body_to_buf, &body);
    OK(http_pump(&r, fake_transport(&f)) == 0, "chunk extensions tolerated");
    EQSTR(buf_cstr(&body), "abcd", "body past chunk extension");

    http_resp_free(&r);
    buf_free(&body);
    buf_free(&f.sent);
}

static void
test_response_eof_delimited(void)
{
    fake      f;
    http_resp r;
    buf       body;
    static const char wire[] =
        "HTTP/1.0 200 OK\r\n"
        "\r\n"
        "no length header, ends at close";

    buf_init(&body);
    fake_init(&f, wire, sizeof(wire) - 1, 0);
    http_resp_init(&r, body_to_buf, &body);
    OK(http_pump(&r, fake_transport(&f)) == 0, "EOF-delimited body");
    EQSTR(buf_cstr(&body), "no length header, ends at close", "body to EOF");

    http_resp_free(&r);
    buf_free(&body);
    buf_free(&f.sent);
}

static void
test_response_no_body_statuses(void)
{
    fake      f;
    http_resp r;
    buf       body;
    static const char wire[] = "HTTP/1.1 204 No Content\r\n\r\n";

    buf_init(&body);
    fake_init(&f, wire, sizeof(wire) - 1, 0);
    http_resp_init(&r, body_to_buf, &body);
    OK(http_pump(&r, fake_transport(&f)) == 0, "204 completes");
    EQLONG(body.len, 0, "204 has no body");

    http_resp_free(&r);
    buf_free(&body);
    buf_free(&f.sent);
}

static void
test_response_error_status(void)
{
    fake      f;
    http_resp r;
    buf       body;
    static const char wire[] =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 3\r\n"
        "\r\n"
        "nah";

    buf_init(&body);
    fake_init(&f, wire, sizeof(wire) - 1, 0);
    http_resp_init(&r, body_to_buf, &body);
    http_pump(&r, fake_transport(&f));
    EQLONG(r.status, 404, "non-2xx status is reported, not swallowed");
    EQSTR(buf_cstr(&body), "nah", "error body still delivered");

    http_resp_free(&r);
    buf_free(&body);
    buf_free(&f.sent);
}

/* Every read size from 1 upward must produce identical results. */
static void
test_response_all_read_sizes(void)
{
    static const char *pieces[] = {
        "data: {\"i\":1}\n\ndata: {\"i",
        "\":2}\n\n"
    };
    static const char *want = "data: {\"i\":1}\n\ndata: {\"i\":2}\n\n";
    buf    wire;
    size_t sz;
    int    bad = 0;

    buf_init(&wire);
    chunkify(&wire,
             "HTTP/1.1 200 OK\r\n"
             "Transfer-Encoding: chunked\r\n"
             "Content-Type: text/event-stream\r\n"
             "\r\n",
             pieces, 2);

    for (sz = 1; sz <= 64; sz++) {
        fake      f;
        http_resp r;
        buf       body;

        buf_init(&body);
        fake_init(&f, wire.data, wire.len, sz);
        http_resp_init(&r, body_to_buf, &body);
        if (http_pump(&r, fake_transport(&f)) != 0 ||
            r.status != 200 ||
            strcmp(buf_cstr(&body), want) != 0) {
            bad++;
        }
        http_resp_free(&r);
        buf_free(&body);
        buf_free(&f.sent);
    }
    EQLONG(bad, 0, "read sizes 1..64 all agree");
    buf_free(&wire);
}

/* --------------------------------------------------- full stack ---------- */

typedef struct {
    buf  text;      /* concatenated content deltas */
    int  events;
    int  done;
    int  bad_json;
} stream_state;

/* HTTP body -> SSE parser */
static void
stack_body(void *ctx, const char *p, size_t n)
{
    sse_feed((sse_parser *)ctx, p, n);
}

/* SSE event -> JSON -> OpenAI-shaped delta extraction */
static void
stack_event(void *ctx, const char *event, const char *data, size_t dlen)
{
    stream_state *st = (stream_state *)ctx;
    json_arena   *a;
    json_value   *root;
    json_value   *choices;
    const char   *frag;

    (void)event;
    st->events++;

    if (dlen == 6 && memcmp(data, "[DONE]", 6) == 0) {
        st->done = 1;
        return;
    }

    a = json_arena_new();
    root = json_parse(a, data, dlen);
    if (root == NULL) {
        st->bad_json++;
        json_arena_free(a);
        return;
    }
    choices = json_get(root, "choices");
    frag = json_as_str(json_path(json_at(choices, 0), "delta.content"), NULL);
    if (frag != NULL)
        buf_puts(&st->text, frag);
    json_arena_free(a);
}

/*
 * End to end: a chunked SSE response of the shape llama.cpp, Ollama and
 * LM Studio all emit, driven at every read size from 1 to 40 bytes. The chunk
 * boundaries deliberately fall inside SSE field names, inside JSON strings and
 * between CR and LF.
 */
static void
test_full_stack(void)
{
    static const char *pieces[] = {
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"},\"index\":0}]}\n\n",
        "data: {\"choices\":[{\"delta\":{\"content\":\", caf\303\251\"}}]}\n\n",
        "data: {\"choices\":[{\"delta\":{\"content\":\" \342\226\266\"}}]}\n\n",
        "data: [DONE]\n\n"
    };
    buf    wire;
    size_t sz;
    int    bad = 0;
    int    checked = 0;

    buf_init(&wire);
    chunkify(&wire,
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/event-stream\r\n"
             "Transfer-Encoding: chunked\r\n"
             "\r\n",
             pieces, 4);

    for (sz = 1; sz <= 40; sz++) {
        fake          f;
        http_resp     r;
        sse_parser    p;
        stream_state  st;

        buf_init(&st.text);
        st.events = 0;
        st.done = 0;
        st.bad_json = 0;

        sse_init(&p, stack_event, &st);
        fake_init(&f, wire.data, wire.len, sz);
        http_resp_init(&r, stack_body, &p);

        if (http_pump(&r, fake_transport(&f)) != 0)
            bad++;
        sse_finish(&p);

        if (st.bad_json != 0 ||
            st.done != 1 ||
            st.events != 4 ||
            strcmp(buf_cstr(&st.text), "Hello, caf\303\251 \342\226\266") != 0)
            bad++;
        checked++;

        http_resp_free(&r);
        sse_free(&p);
        buf_free(&st.text);
        buf_free(&f.sent);
    }

    EQLONG(checked, 40, "exercised 40 read sizes");
    EQLONG(bad, 0, "HTTP->SSE->JSON stack agrees at every read size");
    buf_free(&wire);
}

int
main(void)
{
    test_request_content_length();
    test_request_chunked();
    test_request_two_pass();
    test_response_content_length();
    test_response_chunked();
    test_response_chunk_extension();
    test_response_eof_delimited();
    test_response_no_body_statuses();
    test_response_error_status();
    test_response_all_read_sizes();
    test_full_stack();
    TAP_REPORT("test_http");
}
