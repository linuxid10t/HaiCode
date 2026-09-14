/* provider.h - OpenAI-compatible /v1/chat/completions request and stream.
 *
 * C89. Depends on buf.h, json.h, sse.h.
 *
 * One implementation covers llama.cpp's server, Ollama and LM Studio: all
 * three expose the same /v1/chat/completions shape over plain HTTP.
 *
 * REQUESTS are written through a json_writer, so they stream. Messages are
 * pulled from a prov_msgsrc iterator rather than an array, which means the
 * store can read them off disk one at a time and a 256K-token conversation
 * never exists in memory. Because the two-pass Content-Length scheme emits the
 * document twice, the iterator must be restartable -- hence the rewind hook,
 * which prov_write_request calls before it emits anything.
 *
 * RESPONSES are decoded by prov_on_sse, which has exactly the sse_cb
 * signature and so plugs straight into an sse_parser:
 *
 *     prov_stream st;
 *     sse_parser  p;
 *     http_resp   r;
 *
 *     prov_stream_init(&st, &callbacks);
 *     sse_init(&p, prov_on_sse, &st);
 *     http_resp_init(&r, feed_sse, &p);
 *     http_pump(&r, transport);
 *     sse_finish(&p);
 *
 * Text arrives through callbacks as it streams, so the UI can paint it live.
 * Tool calls accumulate internally -- a half-received argument list is not
 * useful to anyone -- and are read out once the stream ends.
 */
#ifndef HAIOS2_PROVIDER_H
#define HAIOS2_PROVIDER_H

#include <stddef.h>
#include "buf.h"
#include "json.h"

#define PROV_MAX_TOOLCALLS 8

/* ------------------------------------------------------------- request --- */

enum {
    PROV_ROLE_SYSTEM = 0,
    PROV_ROLE_USER,
    PROV_ROLE_ASSISTANT,
    PROV_ROLE_TOOL
};

/* One tool call on an assistant turn. `args` is JSON *text* -- the wire format
 * carries it as a string containing a JSON object, not as an object. */
typedef struct prov_toolcall {
    const char *id;
    const char *name;
    const char *args;
} prov_toolcall;

typedef struct prov_msg {
    int                  role;
    const char          *content;      /* NULL for a pure tool-call turn */
    size_t               content_len;
    const prov_toolcall *calls;        /* assistant turns only */
    size_t               ncalls;
    const char          *tool_call_id; /* PROV_ROLE_TOOL only */
} prov_msg;

/*
 * Message source. next() fills *out and returns 1, or returns 0 when the
 * conversation is exhausted. Anything it points into must stay valid until the
 * following next() call.
 */
typedef struct prov_msgsrc {
    void *ctx;
    void (*rewind)(void *ctx);
    int  (*next)(void *ctx, prov_msg *out);
} prov_msgsrc;

/* `schema` is a pre-encoded JSON object, normally a string literal on the
 * tool itself. Building a schema DSL in C89 would cost more than it saves. */
typedef struct prov_tool {
    const char *name;
    const char *description;
    const char *schema;
} prov_tool;

typedef struct prov_req {
    const char      *model;
    int              stream;
    long             max_tokens;       /* <= 0 omits the field */
    double           temperature;
    int              have_temperature;
    double           top_p;
    int              have_top_p;
    const prov_tool *tools;
    size_t           ntools;
    prov_msgsrc      msgs;
} prov_req;

void prov_req_init(prov_req *r, const char *model);

/* Emits the whole request body. Rewinds the message source first. Returns 0,
 * or the writer's error. */
int  prov_write_request(json_writer *w, const prov_req *r);

/* Emits the body into a counting sink and returns the byte length, or -1 on
 * error. The value is exactly what prov_write_request will later send. */
long prov_request_length(const prov_req *r);

/* ------------------------------------------------------------- response -- */

enum {
    PROV_FINISH_NONE = 0,
    PROV_FINISH_STOP,
    PROV_FINISH_LENGTH,
    PROV_FINISH_TOOLS,
    PROV_FINISH_FILTER,
    PROV_FINISH_OTHER
};

typedef struct prov_call_acc {
    buf id;
    buf name;
    buf args;
    int used;
} prov_call_acc;

typedef struct prov_callbacks {
    void *ctx;
    /* Text the model is emitting, as it arrives. */
    void (*on_text)(void *ctx, const char *p, size_t n);
    /* Chain-of-thought from servers that expose reasoning_content. */
    void (*on_reasoning)(void *ctx, const char *p, size_t n);
    /* An error object carried inside the stream. */
    void (*on_error)(void *ctx, const char *msg);
} prov_callbacks;

typedef struct prov_stream {
    prov_callbacks cb;

    buf            text;        /* full assistant text, also delivered live */
    buf            reasoning;
    buf            error;

    prov_call_acc  calls[PROV_MAX_TOOLCALLS];
    int            ncalls;      /* highest slot touched, plus one */
    int            last_slot;   /* for servers that omit tool_calls[].index */

    int            finish;
    long           prompt_tokens;
    long           completion_tokens;
    int            done;        /* saw the [DONE] sentinel */
    int            bad_events;  /* events that would not parse */
    int            oom;
} prov_stream;

void prov_stream_init(prov_stream *s, const prov_callbacks *cb);
void prov_stream_free(prov_stream *s);

/* Has the sse_cb signature; pass a prov_stream * as ctx. */
void prov_on_sse(void *ctx, const char *event, const char *data, size_t dlen);

/* Read-out once the stream has ended. */
/* Non-const: reading a field NUL-terminates its buffer in place. */
int         prov_ncalls(const prov_stream *s);
const char *prov_call_id(prov_stream *s, int i);
const char *prov_call_name(prov_stream *s, int i);
const char *prov_call_args(prov_stream *s, int i);
/* 1 when every accumulated call has a name and args that parse as JSON. A
 * false here means the turn should be discarded rather than shown to the
 * model as a half-formed tool_use. */
int         prov_calls_valid(prov_stream *s);

#endif
