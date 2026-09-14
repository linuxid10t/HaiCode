/* loop.h - the agentic loop. C89.
 *
 * One step is: build a request from the stored conversation, stream the
 * response, persist it, run any tool calls, persist their results. Repeat
 * while the model keeps asking for tools, up to cfg->max_steps.
 *
 * The loop owns no I/O of its own. Transport comes in through loop_net, which
 * the OS/2 build fills with sock.c and the tests fill with a fake, so every
 * decision in here is exercised on the host.
 *
 * The system prompt is injected at request time rather than stored as the
 * first message. Editing a config file then changes the prompt for existing
 * sessions, which is what a user expects, and history stays a pure record of
 * the conversation.
 */
#ifndef HAIOS2_LOOP_H
#define HAIOS2_LOOP_H

#include "buf.h"
#include "config.h"
#include "http.h"
#include "perm.h"
#include "provider.h"
#include "store.h"
#include "tool.h"

typedef struct loop_net {
    void *ctx;
    /* Opens a connection and fills `t`. Returns 0 on success. Called once per
     * step, so a dropped connection costs one step, not the session. */
    int  (*open)(void *ctx, http_transport *t);
    void (*close)(void *ctx);
} loop_net;

enum {
    LOOP_OK = 0,
    LOOP_ERR_NET,        /* could not connect or the transfer failed */
    LOOP_ERR_HTTP,       /* server answered, but not with 200 */
    LOOP_ERR_PROVIDER,   /* the stream carried an error object */
    LOOP_ERR_STORE,      /* could not persist */
    LOOP_ERR_STEPS,      /* ran out of steps with the model still working */
    LOOP_ERR_INTERRUPT
};

typedef struct loop_ui {
    void *ctx;
    void (*on_text)(void *ctx, const char *p, size_t n);
    void (*on_reasoning)(void *ctx, const char *p, size_t n);
    void (*on_step)(void *ctx, int step, int max_steps);
    void (*on_tool_start)(void *ctx, const char *name, const char *args);
    void (*on_tool_done)(void *ctx, const char *name, int ok, int denied,
                         const char *output);
    void (*on_usage)(void *ctx, long tokens_in, long tokens_out);
    void (*on_error)(void *ctx, const char *msg);
} loop_ui;

typedef struct loop_ctx {
    store         *st;
    const char    *session_id;
    tool_registry *tools;
    perm_gate     *gate;
    const config  *cfg;
    const char    *workdir;
    loop_net       net;
    loop_ui        ui;
    /* Polled between steps and between tool calls. Set from the UI thread to
     * stop cleanly at the next boundary. */
    volatile int  *interrupt;
} loop_ctx;

/* Appends a user turn and runs until the model stops asking for tools.
 * `prompt` may be NULL to resume without new input. Returns a LOOP_* code and
 * writes the number of steps taken to `steps_out` when non-NULL. */
int loop_run(loop_ctx *lc, const char *prompt, int *steps_out);

/* Exposed for testing: one request/response round trip with no tool
 * execution and no persistence. */
int loop_step_once(loop_ctx *lc, prov_stream *out);

#endif
