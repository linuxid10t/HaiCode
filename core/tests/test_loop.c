/* test_loop.c - the agentic loop, driven by a scripted in-process server. */

#include "../loop.h"
#include "../store.h"
#include "../tool.h"
#include "../perm.h"
#include "../plat.h"
#include "../config.h"
#include "../json.h"
#include "../buf.h"
#include "tap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_root[512];
static char g_work[512];

/* ------------------------------------------------- scripted fake server -- */

#define MAX_TURNS 8

typedef struct {
    const char *turns[MAX_TURNS];  /* SSE body per step */
    int         status[MAX_TURNS]; /* HTTP status per step */
    int         nturns;
    int         step;              /* which turn is next */

    buf         sent[MAX_TURNS];   /* request bodies we received */
    int         nsent;

    buf         wire;              /* response being served */
    size_t      pos;
    int         connects;
    int         fail_connect;
} server;

static void
server_init(server *s)
{
    int i;
    s->nturns = 0;
    s->step   = 0;
    s->nsent  = 0;
    s->connects = 0;
    s->fail_connect = 0;
    s->pos = 0;
    buf_init(&s->wire);
    for (i = 0; i < MAX_TURNS; i++) {
        buf_init(&s->sent[i]);
        s->status[i] = 200;
    }
}

static void
server_free(server *s)
{
    int i;
    buf_free(&s->wire);
    for (i = 0; i < MAX_TURNS; i++)
        buf_free(&s->sent[i]);
}

static void
server_reply(server *s, const char *sse_body, int status)
{
    if (s->nturns >= MAX_TURNS) return;
    s->turns[s->nturns]  = sse_body;
    s->status[s->nturns] = status;
    s->nturns++;
}

static int
srv_send(void *ctx, const char *p, size_t n)
{
    server *s = (server *)ctx;
    if (s->nsent > 0 && s->nsent <= MAX_TURNS)
        buf_append(&s->sent[s->nsent - 1], p, n);
    return 0;
}

static int
srv_recv(void *ctx, char *p, size_t n)
{
    server *s = (server *)ctx;
    size_t  avail = s->wire.len - s->pos;
    size_t  take;

    if (avail == 0) return 0;
    /* Small reads, so every boundary case gets exercised on the way through. */
    take = (n < avail) ? n : avail;
    if (take > 13) take = 13;
    memcpy(p, s->wire.data + s->pos, take);
    s->pos += take;
    return (int)take;
}

static int
srv_open(void *ctx, http_transport *t)
{
    server *s = (server *)ctx;
    const char *body;
    char        hdr[256];
    int         status;

    s->connects++;
    if (s->fail_connect)
        return -1;

    if (s->nsent < MAX_TURNS)
        s->nsent++;

    body   = (s->step < s->nturns) ? s->turns[s->step] : "data: [DONE]\n\n";
    status = (s->step < s->nturns) ? s->status[s->step] : 200;
    s->step++;

    buf_clear(&s->wire);
    sprintf(hdr, "HTTP/1.1 %d X\r\nContent-Type: text/event-stream\r\n"
                 "Content-Length: %lu\r\n\r\n",
            status, (unsigned long)strlen(body));
    buf_puts(&s->wire, hdr);
    buf_puts(&s->wire, body);
    s->pos = 0;

    t->ctx   = s;
    t->xsend = srv_send;
    t->xrecv = srv_recv;
    return 0;
}

static void srv_close(void *ctx) { (void)ctx; }

/* Returns the JSON body of request `i` (0-based), parsed. */
static json_value *
sent_body(server *s, int i, json_arena *a)
{
    const char *raw = buf_cstr(&s->sent[i]);
    const char *sep;

    if (raw == NULL) return NULL;
    sep = strstr(raw, "\r\n\r\n");
    if (sep == NULL) return NULL;
    return json_parse(a, sep + 4, strlen(sep + 4));
}

/* ------------------------------------------------------------ UI probe --- */

typedef struct {
    buf text;
    int steps;
    int tools_started;
    int tools_done;
    int errors;
    buf last_error;
    buf last_tool;
} probe;

static void p_text(void *c, const char *p, size_t n)
{ buf_append(&((probe *)c)->text, p, n); }
static void p_step(void *c, int s, int m) { (void)s; (void)m; ((probe *)c)->steps++; }
static void p_tstart(void *c, const char *n, const char *a)
{ (void)a; buf_clear(&((probe *)c)->last_tool);
  buf_puts(&((probe *)c)->last_tool, n); ((probe *)c)->tools_started++; }
static void p_tdone(void *c, const char *n, int ok, int den, const char *o)
{ (void)n; (void)ok; (void)den; (void)o; ((probe *)c)->tools_done++; }
static void p_err(void *c, const char *m)
{ probe *p = (probe *)c; p->errors++;
  buf_clear(&p->last_error); buf_puts(&p->last_error, m); }

static void
probe_init(probe *p, loop_ui *ui)
{
    buf_init(&p->text);
    buf_init(&p->last_error);
    buf_init(&p->last_tool);
    p->steps = p->tools_started = p->tools_done = p->errors = 0;
    ui->ctx           = p;
    ui->on_text       = p_text;
    ui->on_reasoning  = NULL;
    ui->on_step       = p_step;
    ui->on_tool_start = p_tstart;
    ui->on_tool_done  = p_tdone;
    ui->on_usage      = NULL;
    ui->on_error      = p_err;
}

static void
probe_free(probe *p)
{
    buf_free(&p->text);
    buf_free(&p->last_error);
    buf_free(&p->last_tool);
}

/* ------------------------------------------------------------- fixture --- */

typedef struct {
    store         st;
    char          id[STORE_ID_LEN];
    tool_registry reg;
    perm_gate     gate;
    config        cfg;
    server        srv;
    probe         pr;
    loop_ctx      lc;
} fixture;

static perm_effect allow_all(void *c, const char *a, const char *r, const char *i)
{ (void)c; (void)a; (void)r; (void)i; return PERM_ALLOW; }

static void
fx_init(fixture *f)
{
    store_open(&f->st, g_root);
    store_create(&f->st, g_work, "test-model", f->id);

    tool_registry_init(&f->reg);
    tool_register_builtins(&f->reg);

    perm_init(&f->gate);
    perm_set_ask(&f->gate, allow_all, NULL);

    config_defaults(&f->cfg);
    strcpy(f->cfg.model, "test-model");
    strcpy(f->cfg.system_prompt, "SYSTEM PROMPT");
    f->cfg.max_steps = 5;

    server_init(&f->srv);
    probe_init(&f->pr, &f->lc.ui);

    f->lc.st         = &f->st;
    f->lc.session_id = f->id;
    f->lc.tools      = &f->reg;
    f->lc.gate       = &f->gate;
    f->lc.cfg        = &f->cfg;
    f->lc.workdir    = g_work;
    f->lc.net.ctx    = &f->srv;
    f->lc.net.open   = srv_open;
    f->lc.net.close  = srv_close;
    f->lc.interrupt  = NULL;
}

static void
fx_free(fixture *f)
{
    server_free(&f->srv);
    probe_free(&f->pr);
}

static const char *
sse_text(const char *s)
{
    static char b[1024];
    sprintf(b, "data: {\"choices\":[{\"delta\":{\"content\":\"%s\"},"
               "\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n", s);
    return b;
}

/* ---------------------------------------------------------------- tests -- */

static void
test_plain_answer(void)
{
    fixture f;
    int     steps = 0;
    int     rc;

    fx_init(&f);
    server_reply(&f.srv, sse_text("Hello there"), 200);

    rc = loop_run(&f.lc, "hi", &steps);

    EQLONG(rc, LOOP_OK, "plain answer completes");
    EQLONG(steps, 1, "one step");
    EQSTR(buf_cstr(&f.pr.text), "Hello there", "text streamed to the UI");
    EQLONG(f.pr.tools_started, 0, "no tools run");

    /* The user turn and the answer are both on disk. */
    {
        store_iter it;
        prov_msg   m;
        int        n = 0;
        store_iter_open(&it, &f.st, f.id);
        while (store_iter_next(&it, &m) == 1) n++;
        EQLONG(n, 2, "user turn and assistant turn persisted");
        store_iter_close(&it);
    }
    fx_free(&f);
}

static void
test_system_prompt_injected_not_stored(void)
{
    fixture     f;
    json_arena *a;
    json_value *body;
    json_value *msgs;

    fx_init(&f);
    server_reply(&f.srv, sse_text("ok"), 200);
    loop_run(&f.lc, "hi", NULL);

    a    = json_arena_new();
    body = sent_body(&f.srv, 0, a);
    OK(body != NULL, "request body is valid JSON");
    msgs = json_get(body, "messages");
    EQSTR(json_as_str(json_path(json_at(msgs, 0), "role"), NULL), "system",
          "system prompt is first in the request");
    EQSTR(json_as_str(json_path(json_at(msgs, 0), "content"), NULL),
          "SYSTEM PROMPT", "with the configured text");
    EQSTR(json_as_str(json_path(json_at(msgs, 1), "role"), NULL), "user",
          "user turn follows");
    json_arena_free(a);

    /* It must not be on disk: changing the config should change the prompt
     * for existing sessions. */
    {
        store_iter it;
        prov_msg   m;
        int        sys = 0;
        store_iter_open(&it, &f.st, f.id);
        while (store_iter_next(&it, &m) == 1)
            if (m.role == PROV_ROLE_SYSTEM) sys++;
        EQLONG(sys, 0, "system prompt is not written to the transcript");
        store_iter_close(&it);
    }
    fx_free(&f);
}

static void
test_tools_advertised(void)
{
    fixture     f;
    json_arena *a;
    json_value *body;

    fx_init(&f);
    server_reply(&f.srv, sse_text("ok"), 200);
    loop_run(&f.lc, "hi", NULL);

    a    = json_arena_new();
    body = sent_body(&f.srv, 0, a);
    EQLONG((long)json_len(json_get(body, "tools")), 6,
           "all six tools advertised");
    EQSTR(json_as_str(json_path(json_at(json_get(body, "tools"), 0),
                                "function.name"), NULL), "read",
          "tool name present");
    OK(json_type_of(json_path(json_at(json_get(body, "tools"), 0),
                              "function.parameters")) == JSON_OBJ,
       "schema embedded as an object, not a string");
    json_arena_free(a);
    fx_free(&f);
}

/*
 * The important one. After a tool runs, the NEXT request must replay the
 * assistant turn with its tool_calls AND the matching tool result. Dropping
 * the calls leaves an orphaned tool result and makes the model redo work it
 * already did -- the exact failure CLAUDE.md documents for the Haiku build.
 */
static void
test_tool_cycle_replays_correctly(void)
{
    fixture     f;
    int         steps = 0;
    int         rc;
    json_arena *a;
    json_value *body;
    json_value *msgs;
    buf         p;
    FILE       *fp;

    fx_init(&f);

    buf_init(&p);
    buf_puts(&p, g_work);
    buf_puts(&p, "/target.txt");
    fp = fopen(buf_cstr(&p), "wb");
    fputs("FILE CONTENTS\n", fp);
    fclose(fp);
    buf_free(&p);

    server_reply(&f.srv,
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_1\",\"function\":{\"name\":\"read\","
        "\"arguments\":\"{\\\"path\\\":\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"function\":{\"arguments\":\"\\\"target.txt\\\"}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n", 200);
    server_reply(&f.srv, sse_text("The file says FILE CONTENTS"), 200);

    rc = loop_run(&f.lc, "read target.txt", &steps);

    EQLONG(rc, LOOP_OK, "tool cycle completes");
    EQLONG(steps, 2, "two steps");
    EQLONG(f.pr.tools_started, 1, "one tool started");
    EQLONG(f.pr.tools_done, 1, "one tool finished");
    EQSTR(buf_cstr(&f.pr.last_tool), "read", "the read tool ran");
    EQLONG(f.srv.connects, 2, "one connection per step");

    a    = json_arena_new();
    body = sent_body(&f.srv, 1, a);
    OK(body != NULL, "second request is valid JSON");
    msgs = json_get(body, "messages");

    EQLONG((long)json_len(msgs), 4,
           "system + user + assistant + tool result");
    EQSTR(json_as_str(json_path(json_at(msgs, 2), "role"), NULL), "assistant",
          "assistant turn replayed");
    EQSTR(json_as_str(json_path(json_at(json_get(json_at(msgs, 2),
                      "tool_calls"), 0), "function.name"), NULL), "read",
          "with its tool_calls intact");
    EQSTR(json_as_str(json_path(json_at(json_get(json_at(msgs, 2),
                      "tool_calls"), 0), "id"), NULL), "call_1",
          "and the original call id");
    EQSTR(json_as_str(json_path(json_at(msgs, 3), "role"), NULL), "tool",
          "tool result replayed");
    EQSTR(json_as_str(json_path(json_at(msgs, 3), "tool_call_id"), NULL),
          "call_1", "tool result is linked to the call that produced it");
    OK(strstr(json_as_str(json_path(json_at(msgs, 3), "content"), ""),
              "FILE CONTENTS") != NULL,
       "and carries what the tool actually returned");

    json_arena_free(a);
    fx_free(&f);
}

/* A tool call whose arguments never finished arriving must not be dispatched. */
static void
test_incomplete_toolcall_discarded(void)
{
    fixture f;
    int     steps = 0;

    fx_init(&f);
    server_reply(&f.srv,
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"c\",\"function\":{\"name\":\"cmd\","
        "\"arguments\":\"{\\\"command\\\":\\\"del \"}}]}}]}\n\n"
        "data: [DONE]\n\n", 200);
    server_reply(&f.srv, sse_text("sorry, retrying"), 200);

    loop_run(&f.lc, "go", &steps);

    EQLONG(f.pr.tools_started, 0,
           "a half-received tool call is never dispatched");
    OK(f.pr.errors >= 1, "and the UI is told");
    OK(strstr(buf_cstr(&f.pr.last_error), "incomplete") != NULL,
       "with a clear reason");
    fx_free(&f);
}

static void
test_denied_tool_is_reported_to_model(void)
{
    fixture     f;
    json_arena *a;
    json_value *body;
    const char *content;

    fx_init(&f);
    perm_init(&f.gate);              /* no ask callback: everything denied */
    server_reply(&f.srv,
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"c\",\"function\":{\"name\":\"cmd\","
        "\"arguments\":\"{\\\"command\\\":\\\"echo hi\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n", 200);
    server_reply(&f.srv, sse_text("understood"), 200);

    loop_run(&f.lc, "go", NULL);

    a    = json_arena_new();
    body = sent_body(&f.srv, 1, a);
    content = json_as_str(json_path(json_at(json_get(body, "messages"), 3),
                                    "content"), "");
    OK(strstr(content, "denied") != NULL,
       "a denial is fed back as a tool result so the model can adapt");
    json_arena_free(a);
    fx_free(&f);
}

static void
test_unparseable_args_reported(void)
{
    fixture     f;
    json_arena *a;
    json_value *body;
    const char *content;

    fx_init(&f);
    /* Valid JSON string, but its contents are not an object. */
    server_reply(&f.srv,
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"c\",\"function\":{\"name\":\"read\","
        "\"arguments\":\"\"}}]}}]}\n\n"
        "data: [DONE]\n\n", 200);
    server_reply(&f.srv, sse_text("ok"), 200);

    loop_run(&f.lc, "go", NULL);

    a    = json_arena_new();
    body = sent_body(&f.srv, 1, a);
    content = json_as_str(json_path(json_at(json_get(body, "messages"), 3),
                                    "content"), "");
    OK(content[0] != '\0',
       "unparseable arguments produce a tool result, not silence");
    json_arena_free(a);
    fx_free(&f);
}

static void
test_max_steps(void)
{
    fixture f;
    int     steps = 0;
    int     rc;
    int     i;

    fx_init(&f);
    f.cfg.max_steps = 3;
    /* The model keeps asking for tools and never stops. */
    for (i = 0; i < MAX_TURNS; i++)
        server_reply(&f.srv,
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
            "\"id\":\"c\",\"function\":{\"name\":\"ls\","
            "\"arguments\":\"{}\"}}]}}]}\n\n"
            "data: [DONE]\n\n", 200);

    rc = loop_run(&f.lc, "go", &steps);
    EQLONG(rc, LOOP_ERR_STEPS, "running out of steps is reported");
    EQLONG(steps, 3, "and stops at the limit");
    fx_free(&f);
}

/* Finishing on the final allowed step is success, not exhaustion. */
static void
test_finish_on_last_step(void)
{
    fixture f;
    int     steps = 0;
    int     rc;

    fx_init(&f);
    f.cfg.max_steps = 1;
    server_reply(&f.srv, sse_text("done in one"), 200);

    rc = loop_run(&f.lc, "go", &steps);
    EQLONG(rc, LOOP_OK, "finishing on the last step is success");
    EQLONG(steps, 1, "one step used");
    fx_free(&f);
}

static void
test_interrupt(void)
{
    fixture f;
    int     flag = 1;
    int     steps = 0;
    int     rc;

    fx_init(&f);
    f.lc.interrupt = &flag;
    server_reply(&f.srv, sse_text("never sent"), 200);

    rc = loop_run(&f.lc, "go", &steps);
    EQLONG(rc, LOOP_ERR_INTERRUPT, "interrupt stops the loop");
    EQLONG(steps, 0, "before any request is made");
    EQLONG(f.srv.connects, 0, "and without connecting");
    fx_free(&f);
}

static void
test_http_error(void)
{
    fixture f;
    int     rc;

    fx_init(&f);
    server_reply(&f.srv, "nope", 500);
    rc = loop_run(&f.lc, "go", NULL);
    EQLONG(rc, LOOP_ERR_HTTP, "non-200 reported as an HTTP error");
    OK(strstr(buf_cstr(&f.pr.last_error), "500") != NULL,
       "with the status in the message");
    fx_free(&f);
}

static void
test_connect_failure(void)
{
    fixture f;
    int     rc;

    fx_init(&f);
    f.srv.fail_connect = 1;
    rc = loop_run(&f.lc, "go", NULL);
    EQLONG(rc, LOOP_ERR_NET, "connect failure reported");
    OK(f.pr.errors >= 1, "and surfaced to the UI");
    fx_free(&f);
}

static void
test_provider_error(void)
{
    fixture f;
    int     rc;

    fx_init(&f);
    server_reply(&f.srv,
        "data: {\"error\":{\"message\":\"model not loaded\"}}\n\n"
        "data: [DONE]\n\n", 200);
    rc = loop_run(&f.lc, "go", NULL);
    EQLONG(rc, LOOP_ERR_PROVIDER, "in-stream error reported");
    OK(strstr(buf_cstr(&f.pr.last_error), "model not loaded") != NULL,
       "with the server's own message");
    fx_free(&f);
}

int
main(void)
{
    sprintf(g_root, "/tmp/haios2_loop_%ld", (long)plat_time());
    sprintf(g_work, "%.400s/work", g_root);
    plat_mkdir_p(g_work);

    test_plain_answer();
    test_system_prompt_injected_not_stored();
    test_tools_advertised();
    test_tool_cycle_replays_correctly();
    test_incomplete_toolcall_discarded();
    test_denied_tool_is_reported_to_model();
    test_unparseable_args_reported();
    test_max_steps();
    test_finish_on_last_step();
    test_interrupt();
    test_http_error();
    test_connect_failure();
    test_provider_error();

    {
        buf cmd, out;
        buf_init(&cmd); buf_init(&out);
        buf_puts(&cmd, "rm -rf ");
        buf_puts(&cmd, g_root);
        plat_run(buf_cstr(&cmd), NULL, 10, &out, 1024);
        buf_free(&cmd); buf_free(&out);
    }
    TAP_REPORT("test_loop");
}
