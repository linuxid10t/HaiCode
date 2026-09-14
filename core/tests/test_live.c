/* test_live.c - the whole client, end to end, over a real socket.
 *
 *     usage: test_live <host> <port>
 *
 * Everything except Presentation Manager runs here: config, the store, the
 * agentic loop, request emission through a real TCP socket, chunked HTTP,
 * SSE framing, provider decoding, the permission gate, and a real tool
 * touching a real file on disk. The only pieces the target swaps are
 * plat_os2.c and the six calls inside sock.c's platform block.
 *
 * The server on the other end is not passive: it asserts that the second
 * request replays the assistant turn WITH its tool_calls and the matching
 * tool result, and answers HTTP 400 with the reason if not. Checking that on
 * the wire is stronger than checking it in the client's own tests.
 */

#include "../loop.h"
#include "../sock.h"
#include "../store.h"
#include "../tool.h"
#include "../perm.h"
#include "../config.h"
#include "../plat.h"
#include "../json.h"
#include "../buf.h"
#include "tap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_root[512];
static char g_work[512];

/* ------------------------------------------------------------ UI probe --- */

typedef struct {
    buf text;
    int steps;
    int tools;
    int errors;
    buf last_error;
    buf last_tool;
    buf last_tool_out;
    long tin;
    long tout;
} probe;

static void p_text(void *c, const char *p, size_t n)
{ buf_append(&((probe *)c)->text, p, n); }
static void p_step(void *c, int s, int m) { (void)s; (void)m; ((probe *)c)->steps++; }
static void p_tstart(void *c, const char *n, const char *a)
{ probe *p = (probe *)c; (void)a; buf_clear(&p->last_tool);
  buf_puts(&p->last_tool, n); p->tools++; }
static void p_tdone(void *c, const char *n, int ok, int den, const char *o)
{ probe *p = (probe *)c; (void)n; (void)ok; (void)den;
  buf_clear(&p->last_tool_out); buf_puts(&p->last_tool_out, o); }
static void p_usage(void *c, long i, long o)
{ probe *p = (probe *)c; p->tin = i; p->tout = o; }
static void p_err(void *c, const char *m)
{ probe *p = (probe *)c; p->errors++;
  buf_clear(&p->last_error); buf_puts(&p->last_error, m); }

/* ------------------------------------------------------------------------ */

static perm_effect
allow_all(void *c, const char *a, const char *r, const char *i)
{ (void)c; (void)a; (void)r; (void)i; return PERM_ALLOW; }

int
main(int argc, char **argv)
{
    store         st;
    char          sid[STORE_ID_LEN];
    tool_registry reg;
    perm_gate     gate;
    config        cfg;
    sock_net      net;
    probe         pr;
    loop_ctx      lc;
    buf           p;
    FILE         *f;
    int           steps = 0;
    int           rc;

    if (argc < 3) {
        printf("test_live: skipped (no server given)\n");
        return 0;
    }

    sprintf(g_root, "/tmp/haios2_live_%ld", (long)plat_time());
    sprintf(g_work, "%.400s/work", g_root);
    plat_mkdir_p(g_work);

    /* The file the model is going to ask for. */
    buf_init(&p);
    buf_puts(&p, g_work);
    buf_puts(&p, "/probe.txt");
    f = fopen(buf_cstr(&p), "wb");
    OK(f != NULL, "scratch file created");
    if (f != NULL) { fputs("HELLO FROM OS2\n", f); fclose(f); }
    buf_free(&p);

    OK(store_open(&st, g_root) == 0, "store opens");
    OK(store_create(&st, g_work, "local", sid) == 0, "session created");

    tool_registry_init(&reg);
    tool_register_builtins(&reg);
    perm_init(&gate);
    perm_set_ask(&gate, allow_all, NULL);

    config_defaults(&cfg);
    strncpy(cfg.host, argv[1], sizeof(cfg.host) - 1);
    cfg.port      = atoi(argv[2]);
    cfg.max_steps = 4;
    strcpy(cfg.model, "local");

    OK(sock_startup() == 0, "TCP/IP stack up");
    sock_net_init(&net, cfg.host, cfg.port, 10000);

    memset(&pr, 0, sizeof(pr));
    buf_init(&pr.text);
    buf_init(&pr.last_error);
    buf_init(&pr.last_tool);
    buf_init(&pr.last_tool_out);

    lc.st            = &st;
    lc.session_id    = sid;
    lc.tools         = &reg;
    lc.gate          = &gate;
    lc.cfg           = &cfg;
    lc.workdir       = g_work;
    lc.net           = sock_net_loop(&net);
    lc.interrupt     = NULL;
    lc.ui.ctx           = &pr;
    lc.ui.on_text       = p_text;
    lc.ui.on_reasoning  = NULL;
    lc.ui.on_step       = p_step;
    lc.ui.on_tool_start = p_tstart;
    lc.ui.on_tool_done  = p_tdone;
    lc.ui.on_usage      = p_usage;
    lc.ui.on_error      = p_err;

    rc = loop_run(&lc, "what does probe.txt say?", &steps);

    printf("  steps      : %d\n", steps);
    printf("  text       : %s\n", buf_cstr(&pr.text));
    printf("  tool       : %s\n", buf_cstr(&pr.last_tool));
    printf("  tool output: %s", buf_cstr(&pr.last_tool_out));
    printf("  tokens     : %ld in, %ld out\n", pr.tin, pr.tout);
    if (pr.errors > 0)
        printf("  error      : %s\n", buf_cstr(&pr.last_error));

    /* A 400 here means the server rejected what we sent; its message says
     * exactly which assertion failed. */
    EQLONG(rc, LOOP_OK, "loop completed over a real socket");
    EQLONG(pr.errors, 0, "no errors reported");
    EQLONG(steps, 2, "two steps: tool call, then answer");
    EQLONG(pr.tools, 1, "one tool executed");
    EQSTR(buf_cstr(&pr.last_tool), "read", "the read tool ran");
    OK(strstr(buf_cstr(&pr.last_tool_out), "HELLO FROM OS2") != NULL,
       "the tool really read the file off disk");

    OK(strstr(buf_cstr(&pr.text), "Reading that file now") != NULL,
       "turn 1 text streamed through");
    OK(strstr(buf_cstr(&pr.text), "caf\303\251") != NULL,
       "multi-byte UTF-8 survived the socket");
    OK(strstr(buf_cstr(&pr.text), "\360\237\232\200") != NULL,
       "a 4-byte code point survived too");
    OK(strstr(buf_cstr(&pr.text), "HELLO FROM OS2") != NULL,
       "turn 2 answer streamed through");
    EQLONG(pr.tin, 91, "usage from the final turn");

    /* The transcript on disk must be replayable. */
    {
        store_iter it;
        prov_msg   m;
        int        n = 0;
        int        saw_call = 0;
        int        saw_result = 0;

        store_iter_open(&it, &st, sid);
        while (store_iter_next(&it, &m) == 1) {
            n++;
            if (m.role == PROV_ROLE_ASSISTANT && m.ncalls > 0)
                saw_call = 1;
            if (m.role == PROV_ROLE_TOOL && m.tool_call_id != NULL &&
                strcmp(m.tool_call_id, "call_probe") == 0)
                saw_result = 1;
        }
        EQLONG(n, 4, "four records persisted");
        OK(saw_call, "the assistant turn kept its tool call on disk");
        OK(saw_result, "and the tool result is linked to it");
        OK(it.skipped == 0, "no records were skipped");
        store_iter_close(&it);
    }

    {
        buf cmd, out;
        buf_init(&cmd); buf_init(&out);
        buf_puts(&cmd, "rm -rf ");
        buf_puts(&cmd, g_root);
        plat_run(buf_cstr(&cmd), NULL, 10, &out, 1024);
        buf_free(&cmd); buf_free(&out);
    }

    buf_free(&pr.text);
    buf_free(&pr.last_error);
    buf_free(&pr.last_tool);
    buf_free(&pr.last_tool_out);

    TAP_REPORT("test_live");
}
