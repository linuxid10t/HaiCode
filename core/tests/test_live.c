/* test_live.c - drives core/ against a real HTTP server over a real socket.
 *
 * The fake transport in test_http.c controls read sizes exactly; this one does
 * not, which is the point -- it exercises whatever segmentation the kernel and
 * the server actually produce.
 *
 * The POSIX socket code here is test scaffolding. The OS/2 implementation is a
 * separate sock.c and differs in ways that matter: sock_init() must be called
 * first, handles are not file descriptors, and close() must be soclose().
 *
 *     usage: test_live <host> <port>
 */

#include "../http.h"
#include "../sse.h"
#include "../json.h"
#include "../provider.h"
#include "../buf.h"
#include "tap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

typedef struct { int fd; } psock;

static int
psock_send(void *ctx, const char *p, size_t n)
{
    psock *s = (psock *)ctx;
    size_t off = 0;

    while (off < n) {
        ssize_t w = send(s->fd, p + off, n - off, 0);
        if (w <= 0)
            return -1;
        off += (size_t)w;
    }
    return 0;
}

static int
psock_recv(void *ctx, char *p, size_t n)
{
    psock  *s = (psock *)ctx;
    ssize_t r = recv(s->fd, p, n, 0);
    if (r < 0)
        return -1;
    return (int)r;
}

static int
psock_connect(psock *s, const char *host, const char *port)
{
    struct addrinfo  hints;
    struct addrinfo *res;
    struct addrinfo *ai;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0)
        return -1;

    s->fd = -1;
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            s->fd = fd;
            break;
        }
        close(fd);
    }
    freeaddrinfo(res);
    return (s->fd >= 0) ? 0 : -1;
}

/* ---------------------------------------------------------------------- */

typedef struct {
    buf live;      /* text as delivered by the streaming callback */
    int errors;
} live_sink;

static void
on_body(void *ctx, const char *p, size_t n)
{
    sse_feed((sse_parser *)ctx, p, n);
}

static void
live_text(void *ctx, const char *p, size_t n)
{
    buf_append(&((live_sink *)ctx)->live, p, n);
}

static void
live_error(void *ctx, const char *msg)
{
    ((live_sink *)ctx)->errors++;
    fprintf(stderr, "  provider error: %s\n", msg);
}

/* Emits the request body. Called twice: once to measure, once to send. */
static void
emit_body(json_writer *w, prov_req *r)
{
    prov_write_request(w, r);
}

/* ------------------------------------------------ message source --------- */

typedef struct {
    const prov_msg *msgs;
    size_t          n;
    size_t          pos;
} src;

static void src_rewind(void *c) { ((src *)c)->pos = 0; }

static int
src_next(void *c, prov_msg *out)
{
    src *a = (src *)c;
    if (a->pos >= a->n)
        return 0;
    *out = a->msgs[a->pos++];
    return 1;
}

int
main(int argc, char **argv)
{
    static const prov_msg msgs[] = {
        { PROV_ROLE_SYSTEM, "be terse",              8, NULL, 0, NULL },
        { PROV_ROLE_USER,   "hi \303\251 \342\226\266", 10, NULL, 0, NULL }
    };
    static const prov_tool tools[] = {
        { "read", "Read a file",
          "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}" }
    };

    psock          s;
    http_transport t;
    http_req       req;
    http_resp      resp;
    sse_parser     p;
    prov_stream    st;
    prov_callbacks cb;
    live_sink      ls;
    src            msrc;
    prov_req       r;
    json_writer    w;
    long           len;
    char           hostport[128];
    char           ctype[64];
    int            rc;

    if (argc < 3) {
        printf("test_live: skipped (no server given)\n");
        return 0;
    }
    if (psock_connect(&s, argv[1], argv[2]) != 0) {
        printf("test_live: cannot connect to %s:%s\n", argv[1], argv[2]);
        return 1;
    }
    sprintf(hostport, "%.60s:%.20s", argv[1], argv[2]);

    t.ctx   = &s;
    t.xsend = psock_send;
    t.xrecv = psock_recv;

    msrc.msgs = msgs;
    msrc.n    = 2;
    msrc.pos  = 0;

    prov_req_init(&r, "local");
    r.msgs.ctx    = &msrc;
    r.msgs.rewind = src_rewind;
    r.msgs.next   = src_next;
    r.tools       = tools;
    r.ntools      = 1;
    r.max_tokens  = 256;

    /* Pass 1: measure. Pass 2: emit onto the socket. */
    len = prov_request_length(&r);
    OK(len > 0, "request length measured");

    http_req_begin(&req, t, "POST", "/v1/chat/completions", hostport);
    http_req_header(&req, "Content-Type", "application/json");
    http_req_body_begin(&req, len);
    json_w_init(&w, http_req_body_write, &req);
    emit_body(&w, &r);
    http_req_body_end(&req);
    OK(json_w_finish(&w) == 0, "streamed emit clean");
    OK(req.err == 0, "request sent without error");

    buf_init(&ls.live);
    ls.errors = 0;
    cb.ctx          = &ls;
    cb.on_text      = live_text;
    cb.on_reasoning = NULL;
    cb.on_error     = live_error;

    prov_stream_init(&st, &cb);
    sse_init(&p, prov_on_sse, &st);
    http_resp_init(&resp, on_body, &p);

    rc = http_pump(&resp, t);
    sse_finish(&p);

    EQLONG(rc, 0, "pump completed");
    EQLONG(resp.status, 200, "HTTP 200");
    OK(http_resp_header(&resp, "content-type", ctype, sizeof(ctype)) == 1,
       "content-type present");
    OK(strstr(ctype, "text/event-stream") != NULL, "server streamed SSE");
    EQLONG(st.bad_events, 0, "every event parsed");
    EQLONG(ls.errors, 0, "no provider errors");
    OK(st.done == 1, "saw [DONE]");

    printf("  text     : %s\n", buf_cstr(&st.text));
    printf("  live     : %s\n", buf_cstr(&ls.live));
    printf("  finish   : %d\n", st.finish);
    printf("  calls    : %d\n", prov_ncalls(&st));
    if (prov_ncalls(&st) > 0) {
        printf("  call[0]  : %s %s\n",
               prov_call_name(&st, 0), prov_call_args(&st, 0));
    }
    printf("  tokens   : %ld in, %ld out\n",
           st.prompt_tokens, st.completion_tokens);

    EQSTR(buf_cstr(&st.text),
          "Hello from llama.cpp, caf\303\251 \342\226\266 \360\237\232\200!",
          "streamed text reassembled exactly");
    EQSTR(buf_cstr(&ls.live), buf_cstr(&st.text),
          "live callback saw the same bytes");
    EQLONG(st.finish, PROV_FINISH_TOOLS, "finished for tool calls");
    EQLONG(prov_ncalls(&st), 1, "one tool call decoded");
    EQSTR(prov_call_name(&st, 0), "read", "tool name");
    EQSTR(prov_call_args(&st, 0),
          "{\"path\":\"C:\\\\OS2\\\\CONFIG.SYS\",\"limit\":100}",
          "fragmented arguments reassembled");
    OK(prov_calls_valid(&st), "assembled call validates");
    EQLONG(st.prompt_tokens, 42, "usage reported");

    {
        json_arena *a = json_arena_new();
        json_value *v = json_parse(a, prov_call_args(&st, 0),
                                   strlen(prov_call_args(&st, 0)));
        OK(v != NULL, "arguments re-parse");
        EQSTR(json_as_str(json_get(v, "path"), NULL), "C:\\OS2\\CONFIG.SYS",
              "escaped backslashes survive the round trip");
        json_arena_free(a);
    }

    http_resp_free(&resp);
    sse_free(&p);
    prov_stream_free(&st);
    http_req_free(&req);
    buf_free(&ls.live);
    close(s.fd);

    TAP_REPORT("test_live");
}
