/* loop.c - the agentic loop. C89. */

#include "loop.h"
#include "json.h"
#include "path.h"
#include "sse.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------- system-prompt source -- */

/*
 * Wraps the store iterator so the system prompt is yielded first without ever
 * being written to disk. prov_write_request rewinds through this, so both the
 * counting pass and the sending pass see the same sequence.
 */
typedef struct sys_src {
    const char *system;
    int         emitted;
    store_iter *inner;
} sys_src;

static void
sys_rewind(void *ctx)
{
    sys_src *s = (sys_src *)ctx;
    s->emitted = 0;
    store_iter_rewind(s->inner);
}

static int
sys_next(void *ctx, prov_msg *out)
{
    sys_src *s = (sys_src *)ctx;

    if (!s->emitted) {
        s->emitted = 1;
        if (s->system != NULL && *s->system != '\0') {
            memset(out, 0, sizeof(*out));
            out->role        = PROV_ROLE_SYSTEM;
            out->content     = s->system;
            out->content_len = strlen(s->system);
            return 1;
        }
    }
    return store_iter_next(s->inner, out);
}

/* ------------------------------------------------------- UI trampolines -- */

static void
ui_text(void *ctx, const char *p, size_t n)
{
    loop_ctx *lc = (loop_ctx *)ctx;
    if (lc->ui.on_text != NULL)
        lc->ui.on_text(lc->ui.ctx, p, n);
}

static void
ui_reasoning(void *ctx, const char *p, size_t n)
{
    loop_ctx *lc = (loop_ctx *)ctx;
    if (lc->ui.on_reasoning != NULL)
        lc->ui.on_reasoning(lc->ui.ctx, p, n);
}

static void
ui_error(void *ctx, const char *msg)
{
    loop_ctx *lc = (loop_ctx *)ctx;
    if (lc->ui.on_error != NULL)
        lc->ui.on_error(lc->ui.ctx, msg);
}

static void
loop_fail(loop_ctx *lc, const char *msg)
{
    if (lc->ui.on_error != NULL)
        lc->ui.on_error(lc->ui.ctx, msg);
}

/* ------------------------------------------------------------ one round -- */

static void
feed_sse(void *ctx, const char *p, size_t n)
{
    sse_feed((sse_parser *)ctx, p, n);
}

/*
 * Sends one request and decodes the reply into `out`, which the caller has
 * already initialised. Does not touch the store and does not run tools.
 */
static int
loop_round(loop_ctx *lc, prov_stream *out)
{
    http_transport t;
    http_req       req;
    http_resp      resp;
    sse_parser     sse;
    store_iter     it;
    sys_src        src;
    prov_req       pr;
    prov_tool      tools[TOOL_MAX];
    json_writer    w;
    char           hostport[160];
    char           portstr[16];
    long           len;
    int            rc = LOOP_OK;
    int            opened = 0;

    if (store_iter_open(&it, lc->st, lc->session_id) != 0)
        return LOOP_ERR_STORE;

    src.system  = (lc->cfg != NULL) ? lc->cfg->system_prompt : NULL;
    src.emitted = 0;
    src.inner   = &it;

    prov_req_init(&pr, lc->cfg->model);
    pr.msgs.ctx    = &src;
    pr.msgs.rewind = sys_rewind;
    pr.msgs.next   = sys_next;
    pr.max_tokens  = lc->cfg->max_tokens;
    if (lc->cfg->have_temperature) {
        pr.temperature      = lc->cfg->temperature;
        pr.have_temperature = 1;
    }
    if (lc->cfg->have_top_p) {
        pr.top_p      = lc->cfg->top_p;
        pr.have_top_p = 1;
    }
    if (lc->tools != NULL) {
        pr.ntools = tool_defs(lc->tools, tools, TOOL_MAX);
        pr.tools  = tools;
    }

    len = prov_request_length(&pr);
    if (len < 0) {
        rc = LOOP_ERR_STORE;
        goto done;
    }

    if (lc->net.open == NULL || lc->net.open(lc->net.ctx, &t) != 0) {
        loop_fail(lc, "cannot reach the model server");
        rc = LOOP_ERR_NET;
        goto done;
    }
    opened = 1;

    sprintf(portstr, "%d", lc->cfg->port);
    sprintf(hostport, "%.120s:%.12s", lc->cfg->host, portstr);

    http_req_begin(&req, t, "POST", lc->cfg->endpoint, hostport);
    http_req_header(&req, "Content-Type", "application/json");
    http_req_header(&req, "Accept", "text/event-stream");
    /* One connection per step, so say so: the server releases the socket
     * promptly, and a response with no Content-Length becomes legitimately
     * EOF-delimited instead of hanging until the read timeout. */
    http_req_header(&req, "Connection", "close");
    http_req_body_begin(&req, len);
    json_w_init(&w, http_req_body_write, &req);
    prov_write_request(&w, &pr);
    http_req_body_end(&req);

    if (req.err != 0 || json_w_finish(&w) != 0) {
        loop_fail(lc, "failed while sending the request");
        http_req_free(&req);
        rc = LOOP_ERR_NET;
        goto done;
    }

    sse_init(&sse, prov_on_sse, out);
    http_resp_init(&resp, feed_sse, &sse);

    if (http_pump(&resp, t) != 0) {
        loop_fail(lc, "the connection failed mid-response");
        rc = LOOP_ERR_NET;
    } else if (resp.status != 200) {
        char msg[160];
        sprintf(msg, "server returned HTTP %d", resp.status);
        loop_fail(lc, msg);
        rc = LOOP_ERR_HTTP;
    }
    sse_finish(&sse);

    if (rc == LOOP_OK && out->error.len > 0)
        rc = LOOP_ERR_PROVIDER;

    http_resp_free(&resp);
    sse_free(&sse);
    http_req_free(&req);

done:
    if (opened && lc->net.close != NULL)
        lc->net.close(lc->net.ctx);
    store_iter_close(&it);
    return rc;
}

int
loop_step_once(loop_ctx *lc, prov_stream *out)
{
    return loop_round(lc, out);
}

/* --------------------------------------------------------------- driver -- */

static int
loop_interrupted(const loop_ctx *lc)
{
    return (lc->interrupt != NULL && *lc->interrupt != 0);
}

/* Persists the assistant turn exactly as received, tool calls included, so the
 * next request replays it verbatim. Dropping the calls here is what produces
 * orphaned tool results on the following turn. */
static int
persist_assistant(loop_ctx *lc, prov_stream *st)
{
    prov_toolcall calls[PROV_MAX_TOOLCALLS];
    int           n = prov_ncalls(st);
    int           i;
    const char   *text;

    for (i = 0; i < n && i < PROV_MAX_TOOLCALLS; i++) {
        calls[i].id   = prov_call_id(st, i);
        calls[i].name = prov_call_name(st, i);
        calls[i].args = prov_call_args(st, i);
        if (calls[i].id == NULL)   calls[i].id = "";
        if (calls[i].name == NULL) calls[i].name = "";
        if (calls[i].args == NULL) calls[i].args = "{}";
    }

    text = (st->text.len > 0) ? buf_cstr(&st->text) : NULL;
    return store_append(lc->st, lc->session_id, PROV_ROLE_ASSISTANT,
                        text, (text != NULL) ? st->text.len : 0,
                        (n > 0) ? calls : NULL, (size_t)n, NULL);
}

static void
run_one_tool(loop_ctx *lc, prov_stream *st, int i)
{
    json_arena *a;
    json_value *args;
    tool_ctx    tc;
    tool_result res;
    const char *name = prov_call_name(st, i);
    const char *raw  = prov_call_args(st, i);
    const char *id   = prov_call_id(st, i);

    if (name == NULL) name = "";
    if (raw  == NULL) raw  = "{}";
    if (id   == NULL) id   = "";

    if (lc->ui.on_tool_start != NULL)
        lc->ui.on_tool_start(lc->ui.ctx, name, raw);

    tool_result_init(&res);

    a    = json_arena_new();
    args = (a != NULL) ? json_parse(a, raw, strlen(raw)) : NULL;

    if (args == NULL) {
        /* Unparseable arguments are reported back to the model as a failed
         * result rather than dropped, so it can correct itself instead of
         * waiting on a tool result that never comes. */
        res.ok = 0;
        buf_puts(&res.out, "could not parse tool arguments as JSON");
        buf_cstr(&res.out);
    } else {
        tc.workdir   = lc->workdir;
        tc.gate      = lc->gate;
        tc.call_id   = id;
        tc.raw_input = raw;
        tool_execute(lc->tools, name, args, &tc, &res);
    }

    if (lc->ui.on_tool_done != NULL)
        lc->ui.on_tool_done(lc->ui.ctx, name, res.ok, res.denied,
                            buf_cstr(&res.out));

    store_append(lc->st, lc->session_id, PROV_ROLE_TOOL,
                 buf_cstr(&res.out), res.out.len, NULL, 0, id);

    if (a != NULL)
        json_arena_free(a);
    tool_result_free(&res);
}

int
loop_run(loop_ctx *lc, const char *prompt, int *steps_out)
{
    int  step = 0;
    int  rc   = LOOP_OK;
    int  done = 0;
    long maxs;

    if (steps_out != NULL)
        *steps_out = 0;
    if (lc == NULL || lc->cfg == NULL || lc->st == NULL)
        return LOOP_ERR_STORE;

    maxs = lc->cfg->max_steps;
    if (maxs <= 0)
        maxs = 1;

    if (prompt != NULL && *prompt != '\0') {
        if (store_append(lc->st, lc->session_id, PROV_ROLE_USER,
                         prompt, strlen(prompt), NULL, 0, NULL) != 0)
            return LOOP_ERR_STORE;
    }

    while (step < (int)maxs) {
        prov_stream    ps;
        prov_callbacks cb;
        int            ncalls;
        int            i;

        if (loop_interrupted(lc)) {
            rc = LOOP_ERR_INTERRUPT;
            break;
        }

        step++;
        if (lc->ui.on_step != NULL)
            lc->ui.on_step(lc->ui.ctx, step, (int)maxs);

        cb.ctx          = lc;
        cb.on_text      = ui_text;
        cb.on_reasoning = ui_reasoning;
        cb.on_error     = ui_error;
        prov_stream_init(&ps, &cb);

        rc = loop_round(lc, &ps);
        if (rc != LOOP_OK) {
            prov_stream_free(&ps);
            break;
        }

        if (lc->ui.on_usage != NULL &&
            (ps.prompt_tokens > 0 || ps.completion_tokens > 0))
            lc->ui.on_usage(lc->ui.ctx, ps.prompt_tokens, ps.completion_tokens);

        ncalls = prov_ncalls(&ps);

        /* A turn whose tool calls did not fully arrive is worse than no turn:
         * dispatching half-formed arguments runs the wrong thing. Persist the
         * text, drop the calls, and let the model try again. */
        if (ncalls > 0 && !prov_calls_valid(&ps)) {
            loop_fail(lc, "discarded an incomplete tool call from the model");
            if (ps.text.len > 0)
                store_append(lc->st, lc->session_id, PROV_ROLE_ASSISTANT,
                             buf_cstr(&ps.text), ps.text.len, NULL, 0, NULL);
            prov_stream_free(&ps);
            continue;
        }

        if (persist_assistant(lc, &ps) != 0) {
            prov_stream_free(&ps);
            rc = LOOP_ERR_STORE;
            break;
        }

        if (ncalls == 0) {
            prov_stream_free(&ps);
            rc   = LOOP_OK;
            done = 1;
            break;                 /* the model is done talking */
        }

        for (i = 0; i < ncalls; i++) {
            if (loop_interrupted(lc)) {
                rc = LOOP_ERR_INTERRUPT;
                break;
            }
            run_one_tool(lc, &ps, i);
        }
        prov_stream_free(&ps);

        if (rc == LOOP_ERR_INTERRUPT)
            break;
    }

    /* Only a loop that ran out with the model still working is a step
     * exhaustion. Finishing cleanly on the last allowed step is success. */
    if (rc == LOOP_OK && !done && step >= (int)maxs)
        rc = LOOP_ERR_STEPS;

    if (steps_out != NULL)
        *steps_out = step;
    return rc;
}
