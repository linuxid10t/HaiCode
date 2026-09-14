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
    buf text;
    buf tool_args;
    int events;
    int done;
    int bad_json;
} state;

static void
on_body(void *ctx, const char *p, size_t n)
{
    sse_feed((sse_parser *)ctx, p, n);
}

static void
on_event(void *ctx, const char *event, const char *data, size_t dlen)
{
    state      *st = (state *)ctx;
    json_arena *a;
    json_value *root;
    json_value *delta;
    json_value *tcalls;
    const char *frag;

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
        fprintf(stderr, "  unparseable event: %.*s\n", (int)dlen, data);
        json_arena_free(a);
        return;
    }

    delta = json_path(json_at(json_get(root, "choices"), 0), "delta");

    frag = json_as_str(json_get(delta, "content"), NULL);
    if (frag != NULL)
        buf_puts(&st->text, frag);

    /* Tool-call arguments arrive as string fragments that must be concatenated
     * before they parse as JSON -- the case that breaks when SSE state is not
     * carried across reads. */
    tcalls = json_get(delta, "tool_calls");
    if (tcalls != NULL) {
        const char *args = json_as_str(
            json_path(json_at(tcalls, 0), "function.arguments"), NULL);
        if (args != NULL)
            buf_puts(&st->tool_args, args);
    }

    json_arena_free(a);
}

int
main(int argc, char **argv)
{
    psock          s;
    http_transport t;
    http_req       req;
    http_resp      resp;
    sse_parser     p;
    state          st;
    json_writer    w;
    long           len = 0;
    char           hostport[128];
    char           ctype[64];
    int            i;

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

    /* Pass 1: measure. */
    json_w_init(&w, json_count_sink, &len);
    json_w_obj_open(&w);
      json_w_key(&w, "model");     json_w_str(&w, "local");
      json_w_key(&w, "stream");    json_w_bool(&w, 1);
      json_w_key(&w, "messages");
      json_w_arr_open(&w);
        json_w_obj_open(&w);
          json_w_key(&w, "role");    json_w_str(&w, "user");
          json_w_key(&w, "content"); json_w_str(&w, "hi \303\251 \342\226\266");
        json_w_obj_close(&w);
      json_w_arr_close(&w);
    json_w_obj_close(&w);
    OK(json_w_finish(&w) == 0, "counting pass clean");

    /* Pass 2: emit onto the socket. */
    http_req_begin(&req, t, "POST", "/v1/chat/completions", hostport);
    http_req_header(&req, "Content-Type", "application/json");
    http_req_body_begin(&req, len);
    json_w_init(&w, http_req_body_write, &req);
    json_w_obj_open(&w);
      json_w_key(&w, "model");     json_w_str(&w, "local");
      json_w_key(&w, "stream");    json_w_bool(&w, 1);
      json_w_key(&w, "messages");
      json_w_arr_open(&w);
        json_w_obj_open(&w);
          json_w_key(&w, "role");    json_w_str(&w, "user");
          json_w_key(&w, "content"); json_w_str(&w, "hi \303\251 \342\226\266");
        json_w_obj_close(&w);
      json_w_arr_close(&w);
    json_w_obj_close(&w);
    http_req_body_end(&req);
    OK(json_w_finish(&w) == 0, "streamed emit clean");
    OK(req.err == 0, "request sent without error");

    buf_init(&st.text);
    buf_init(&st.tool_args);
    st.events = 0;
    st.done = 0;
    st.bad_json = 0;

    sse_init(&p, on_event, &st);
    http_resp_init(&resp, on_body, &p);

    i = http_pump(&resp, t);
    sse_finish(&p);

    EQLONG(i, 0, "pump completed");
    EQLONG(resp.status, 200, "HTTP 200");
    OK(http_resp_header(&resp, "content-type", ctype, sizeof(ctype)) == 1,
       "content-type present");
    OK(strstr(ctype, "text/event-stream") != NULL, "server streamed SSE");
    EQLONG(st.bad_json, 0, "every event parsed as JSON");
    OK(st.done == 1, "saw [DONE]");
    OK(st.events > 1, "received multiple events");

    printf("  assembled text : %s\n", buf_cstr(&st.text));
    printf("  tool arguments : %s\n", buf_cstr(&st.tool_args));
    printf("  event count    : %d\n", st.events);

    EQSTR(buf_cstr(&st.text),
          "Hello from llama.cpp, caf\303\251 \342\226\266 \360\237\232\200!",
          "streamed text reassembled exactly");
    EQSTR(buf_cstr(&st.tool_args),
          "{\"path\":\"C:\\\\OS2\\\\CONFIG.SYS\",\"limit\":100}",
          "fragmented tool arguments reassembled into valid JSON");

    /* And the reassembled arguments must themselves parse. */
    {
        json_arena *a = json_arena_new();
        json_value *v = json_parse(a, st.tool_args.data, st.tool_args.len);
        OK(v != NULL, "reassembled tool arguments re-parse");
        EQSTR(json_as_str(json_get(v, "path"), NULL), "C:\\OS2\\CONFIG.SYS",
              "escaped backslashes survive the round trip");
        json_arena_free(a);
    }

    http_resp_free(&resp);
    sse_free(&p);
    http_req_free(&req);
    buf_free(&st.text);
    buf_free(&st.tool_args);
    close(s.fd);

    TAP_REPORT("test_live");
}
