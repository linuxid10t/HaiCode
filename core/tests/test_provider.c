/* test_provider.c - OpenAI-compatible request building and stream decoding. */

#include "../provider.h"
#include "../json.h"
#include "../sse.h"
#include "../buf.h"
#include "tap.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------ array message source --- */

typedef struct {
    const prov_msg *msgs;
    size_t          n;
    size_t          pos;
    int             rewinds;
} arraysrc;

static void
arr_rewind(void *ctx)
{
    arraysrc *a = (arraysrc *)ctx;
    a->pos = 0;
    a->rewinds++;
}

static int
arr_next(void *ctx, prov_msg *out)
{
    arraysrc *a = (arraysrc *)ctx;
    if (a->pos >= a->n)
        return 0;
    *out = a->msgs[a->pos++];
    return 1;
}

static prov_msgsrc
arr_src(arraysrc *a, const prov_msg *msgs, size_t n)
{
    prov_msgsrc s;
    a->msgs    = msgs;
    a->n       = n;
    a->pos     = 0;
    a->rewinds = 0;
    s.ctx      = a;
    s.rewind   = arr_rewind;
    s.next     = arr_next;
    return s;
}

static void
render(const prov_req *r, buf *out)
{
    json_writer w;
    buf_init(out);
    json_w_init(&w, json_buf_sink, out);
    prov_write_request(&w, r);
}

/* ------------------------------------------------------------- request --- */

static void
test_request_minimal(void)
{
    static const prov_msg msgs[] = {
        { PROV_ROLE_SYSTEM, "be terse", 8, NULL, 0, NULL },
        { PROV_ROLE_USER,   "hi",       2, NULL, 0, NULL }
    };
    arraysrc a;
    prov_req r;
    buf      out;

    prov_req_init(&r, "qwen3");
    r.msgs = arr_src(&a, msgs, 2);
    render(&r, &out);

    EQSTR(buf_cstr(&out),
          "{\"model\":\"qwen3\",\"stream\":true,\"messages\":["
          "{\"role\":\"system\",\"content\":\"be terse\"},"
          "{\"role\":\"user\",\"content\":\"hi\"}]}",
          "minimal request");
    EQLONG(a.rewinds, 1, "message source is rewound before emitting");
    buf_free(&out);
}

static void
test_request_optionals(void)
{
    static const prov_msg msgs[] = {
        { PROV_ROLE_USER, "x", 1, NULL, 0, NULL }
    };
    arraysrc a;
    prov_req r;
    buf      out;

    /* Unset optionals must be absent, not sent as zero -- temperature 0 and
     * "no temperature" are different requests. */
    prov_req_init(&r, "m");
    r.msgs = arr_src(&a, msgs, 1);
    render(&r, &out);
    OK(strstr(buf_cstr(&out), "temperature") == NULL, "temperature omitted");
    OK(strstr(buf_cstr(&out), "max_tokens") == NULL, "max_tokens omitted");
    OK(strstr(buf_cstr(&out), "top_p") == NULL, "top_p omitted");
    buf_free(&out);

    prov_req_init(&r, "m");
    r.msgs             = arr_src(&a, msgs, 1);
    r.max_tokens       = 512;
    r.temperature      = 0.0;
    r.have_temperature = 1;
    r.top_p            = 0.95;
    r.have_top_p       = 1;
    r.stream           = 0;
    render(&r, &out);
    OK(strstr(buf_cstr(&out), "\"stream\":false") != NULL, "stream false");
    OK(strstr(buf_cstr(&out), "\"max_tokens\":512") != NULL, "max_tokens sent");
    OK(strstr(buf_cstr(&out), "\"temperature\":0") != NULL,
       "explicit temperature 0 is sent");
    OK(strstr(buf_cstr(&out), "\"top_p\":0.95") != NULL, "top_p sent");
    buf_free(&out);
}

static void
test_request_tools(void)
{
    static const prov_msg msgs[] = {
        { PROV_ROLE_USER, "read it", 7, NULL, 0, NULL }
    };
    static const prov_tool tools[] = {
        { "read", "Read a file",
          "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
          "\"required\":[\"path\"]}" }
    };
    arraysrc a;
    prov_req r;
    buf      out;

    prov_req_init(&r, "m");
    r.msgs   = arr_src(&a, msgs, 1);
    r.tools  = tools;
    r.ntools = 1;
    render(&r, &out);

    OK(strstr(buf_cstr(&out),
              "\"tools\":[{\"type\":\"function\",\"function\":"
              "{\"name\":\"read\",\"description\":\"Read a file\","
              "\"parameters\":{\"type\":\"object\",\"properties\":"
              "{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}}]")
       != NULL, "tool schema embedded verbatim");
    buf_free(&out);
}

static void
test_request_toolcall_roundtrip(void)
{
    static const prov_toolcall calls[] = {
        { "call_1", "read", "{\"path\":\"C:\\\\OS2\\\\CONFIG.SYS\"}" }
    };
    static const prov_msg msgs[] = {
        { PROV_ROLE_USER,      "show config", 11, NULL,  0, NULL },
        { PROV_ROLE_ASSISTANT, NULL,           0, calls, 1, NULL },
        { PROV_ROLE_TOOL,      "REM ...",      7, NULL,  0, "call_1" }
    };
    arraysrc    a;
    prov_req    r;
    buf         out;
    json_arena *ar;
    json_value *v;
    json_value *m;
    const char *args;

    prov_req_init(&r, "m");
    r.msgs = arr_src(&a, msgs, 3);
    render(&r, &out);

    ar = json_arena_new();
    v  = json_parse(ar, out.data, out.len);
    OK(v != NULL, "request with tool calls is valid JSON");

    m = json_at(json_get(v, "messages"), 1);
    EQSTR(json_as_str(json_get(m, "role"), NULL), "assistant", "assistant turn");
    EQLONG(json_type_of(json_get(m, "content")), JSON_NULL,
           "tool-only turn carries content: null");
    EQSTR(json_as_str(json_path(json_at(json_get(m, "tool_calls"), 0), "id"),
                      NULL), "call_1", "call id");
    EQSTR(json_as_str(json_path(json_at(json_get(m, "tool_calls"), 0),
                                "function.name"), NULL), "read", "call name");

    /* arguments is a *string* carrying JSON, so it must survive double
     * encoding and still parse once extracted. */
    args = json_as_str(json_path(json_at(json_get(m, "tool_calls"), 0),
                                 "function.arguments"), NULL);
    EQSTR(args, "{\"path\":\"C:\\\\OS2\\\\CONFIG.SYS\"}",
          "arguments round-trip as a JSON string");
    {
        json_arena *a2 = json_arena_new();
        json_value *av = json_parse(a2, args, strlen(args));
        OK(av != NULL, "extracted arguments re-parse");
        EQSTR(json_as_str(json_get(av, "path"), NULL), "C:\\OS2\\CONFIG.SYS",
              "backslashes intact after double encoding");
        json_arena_free(a2);
    }

    m = json_at(json_get(v, "messages"), 2);
    EQSTR(json_as_str(json_get(m, "role"), NULL), "tool", "tool turn");
    EQSTR(json_as_str(json_get(m, "tool_call_id"), NULL), "call_1",
          "tool_call_id linked");

    json_arena_free(ar);
    buf_free(&out);
}

/* The counting pass and the sending pass must agree, or the server sees a
 * truncated body. This is the whole basis of the Content-Length scheme. */
static void
test_request_length_matches(void)
{
    static const prov_toolcall calls[] = {
        { "c1", "grep", "{\"q\":\"caf\\u00e9\"}" }
    };
    static const prov_msg msgs[] = {
        { PROV_ROLE_SYSTEM,    "sys \303\251",  6, NULL,  0, NULL },
        { PROV_ROLE_USER,      "hi \342\226\266", 6, NULL, 0, NULL },
        { PROV_ROLE_ASSISTANT, "thinking",      8, calls, 1, NULL },
        { PROV_ROLE_TOOL,      "res\001ult",    8, NULL,  0, "c1" }
    };
    static const prov_tool tools[] = {
        { "grep", "Search", "{\"type\":\"object\"}" }
    };
    arraysrc a;
    prov_req r;
    buf      out;
    long     len;

    prov_req_init(&r, "m");
    r.msgs   = arr_src(&a, msgs, 4);
    r.tools  = tools;
    r.ntools = 1;

    len = prov_request_length(&r);
    render(&r, &out);

    EQLONG(len, (long)out.len, "counted length equals emitted length");
    EQLONG(a.rewinds, 2, "each pass rewinds the source");
    buf_free(&out);
}

static void
test_request_empty_conversation(void)
{
    arraysrc a;
    prov_req r;
    buf      out;

    prov_req_init(&r, "m");
    r.msgs = arr_src(&a, NULL, 0);
    render(&r, &out);
    OK(strstr(buf_cstr(&out), "\"messages\":[]") != NULL,
       "empty conversation emits an empty array, not a broken document");
    buf_free(&out);
}

/* ------------------------------------------------------------- response -- */

typedef struct {
    buf live_text;
    buf live_reason;
    buf err;
    int errors;
} sink;

static void on_text(void *c, const char *p, size_t n)
{ buf_append(&((sink *)c)->live_text, p, n); }
static void on_reason(void *c, const char *p, size_t n)
{ buf_append(&((sink *)c)->live_reason, p, n); }
static void on_err(void *c, const char *m)
{ buf_clear(&((sink *)c)->err); buf_puts(&((sink *)c)->err, m);
  ((sink *)c)->errors++; }

static void
sink_init(sink *s, prov_callbacks *cb)
{
    buf_init(&s->live_text);
    buf_init(&s->live_reason);
    buf_init(&s->err);
    s->errors        = 0;
    cb->ctx          = s;
    cb->on_text      = on_text;
    cb->on_reasoning = on_reason;
    cb->on_error     = on_err;
}

static void
sink_free(sink *s)
{
    buf_free(&s->live_text);
    buf_free(&s->live_reason);
    buf_free(&s->err);
}

static void
feed(prov_stream *st, const char *json_text)
{
    prov_on_sse(st, "", json_text, strlen(json_text));
}

static void
test_stream_text(void)
{
    prov_stream    st;
    prov_callbacks cb;
    sink           s;

    sink_init(&s, &cb);
    prov_stream_init(&st, &cb);

    feed(&st, "{\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}");
    feed(&st, "{\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}");
    feed(&st, "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}");
    prov_on_sse(&st, "", "[DONE]", 6);

    EQSTR(buf_cstr(&st.text), "Hello", "text accumulated");
    EQSTR(buf_cstr(&s.live_text), "Hello", "text also delivered live");
    EQLONG(st.finish, PROV_FINISH_STOP, "finish_reason stop");
    OK(st.done == 1, "saw [DONE]");
    EQLONG(prov_ncalls(&st), 0, "no tool calls");

    prov_stream_free(&st);
    sink_free(&s);
}

static void
test_stream_reasoning(void)
{
    prov_stream    st;
    prov_callbacks cb;
    sink           s;

    sink_init(&s, &cb);
    prov_stream_init(&st, &cb);

    feed(&st, "{\"choices\":[{\"delta\":{\"reasoning_content\":\"hmm \"}}]}");
    feed(&st, "{\"choices\":[{\"delta\":{\"reasoning\":\"ok\"}}]}");
    feed(&st, "{\"choices\":[{\"delta\":{\"content\":\"answer\"}}]}");

    EQSTR(buf_cstr(&st.reasoning), "hmm ok",
          "both reasoning spellings accumulate");
    EQSTR(buf_cstr(&st.text), "answer", "reasoning kept out of the answer");
    EQSTR(buf_cstr(&s.live_reason), "hmm ok", "reasoning delivered live");

    prov_stream_free(&st);
    sink_free(&s);
}

/* The core case: name arrives once, arguments dribble in as fragments that are
 * not valid JSON until concatenated. */
static void
test_stream_toolcall_fragments(void)
{
    prov_stream    st;
    prov_callbacks cb;
    sink           s;

    sink_init(&s, &cb);
    prov_stream_init(&st, &cb);

    feed(&st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
              "\"id\":\"call_x\",\"type\":\"function\","
              "\"function\":{\"name\":\"read\",\"arguments\":\"\"}}]}}]}");
    feed(&st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
              "\"function\":{\"arguments\":\"{\\\"pa\"}}]}}]}");
    feed(&st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
              "\"function\":{\"arguments\":\"th\\\":\\\"a.txt\\\"}\"}}]}}]}");
    feed(&st, "{\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}");

    EQLONG(prov_ncalls(&st), 1, "one tool call");
    EQSTR(prov_call_id(&st, 0), "call_x", "id");
    EQSTR(prov_call_name(&st, 0), "read", "name survives later empty deltas");
    EQSTR(prov_call_args(&st, 0), "{\"path\":\"a.txt\"}",
          "argument fragments concatenated");
    EQLONG(st.finish, PROV_FINISH_TOOLS, "finish_reason tool_calls");
    OK(prov_calls_valid(&st), "assembled call validates");

    prov_stream_free(&st);
    sink_free(&s);
}

/* Some servers omit tool_calls[].index on continuation deltas. Appending to
 * the wrong slot yields a plausible-looking wrong tool invocation, which is
 * worse than an error, so this case gets its own test. */
static void
test_stream_toolcall_no_index(void)
{
    prov_stream    st;
    prov_callbacks cb;
    sink           s;

    sink_init(&s, &cb);
    prov_stream_init(&st, &cb);

    feed(&st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
              "\"id\":\"c\",\"function\":{\"name\":\"ls\",\"arguments\":\"{\"}}"
              "]}}]}");
    feed(&st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
              "\"function\":{\"arguments\":\"\\\"p\\\":\\\".\\\"\"}}]}}]}");
    feed(&st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
              "\"function\":{\"arguments\":\"}\"}}]}}]}");

    EQLONG(prov_ncalls(&st), 1, "index-less deltas stay in one slot");
    EQSTR(prov_call_args(&st, 0), "{\"p\":\".\"}", "fragments joined");
    OK(prov_calls_valid(&st), "validates");

    prov_stream_free(&st);
    sink_free(&s);
}

static void
test_stream_parallel_calls(void)
{
    prov_stream    st;
    prov_callbacks cb;
    sink           s;

    sink_init(&s, &cb);
    prov_stream_init(&st, &cb);

    feed(&st, "{\"choices\":[{\"delta\":{\"tool_calls\":["
              "{\"index\":0,\"id\":\"a\",\"function\":{\"name\":\"read\","
              "\"arguments\":\"{\\\"p\\\":1}\"}},"
              "{\"index\":1,\"id\":\"b\",\"function\":{\"name\":\"ls\","
              "\"arguments\":\"{}\"}}]}}]}");
    /* Interleaved continuations, out of order. */
    feed(&st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":1,"
              "\"function\":{\"arguments\":\"\"}}]}}]}");

    EQLONG(prov_ncalls(&st), 2, "two parallel calls");
    EQSTR(prov_call_name(&st, 0), "read", "slot 0 name");
    EQSTR(prov_call_args(&st, 0), "{\"p\":1}", "slot 0 args");
    EQSTR(prov_call_name(&st, 1), "ls", "slot 1 name");
    EQSTR(prov_call_args(&st, 1), "{}", "slot 1 args");
    OK(prov_calls_valid(&st), "both validate");

    prov_stream_free(&st);
    sink_free(&s);
}

/* A truncated stream must be detectable, not silently dispatched. */
static void
test_stream_truncated_args(void)
{
    prov_stream    st;
    prov_callbacks cb;
    sink           s;

    sink_init(&s, &cb);
    prov_stream_init(&st, &cb);

    feed(&st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
              "\"id\":\"c\",\"function\":{\"name\":\"read\","
              "\"arguments\":\"{\\\"path\\\":\\\"un\"}}]}}]}");

    OK(!prov_calls_valid(&st), "half-received arguments are rejected");

    prov_stream_free(&st);
    sink_free(&s);
}

static void
test_stream_missing_name(void)
{
    prov_stream    st;
    prov_callbacks cb;
    sink           s;

    sink_init(&s, &cb);
    prov_stream_init(&st, &cb);

    feed(&st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
              "\"function\":{\"arguments\":\"{}\"}}]}}]}");

    OK(!prov_calls_valid(&st), "a call with no name is rejected");

    prov_stream_free(&st);
    sink_free(&s);
}

static void
test_stream_usage_and_error(void)
{
    prov_stream    st;
    prov_callbacks cb;
    sink           s;

    sink_init(&s, &cb);
    prov_stream_init(&st, &cb);
    feed(&st, "{\"choices\":[],\"usage\":{\"prompt_tokens\":1234,"
              "\"completion_tokens\":56}}");
    EQLONG(st.prompt_tokens, 1234, "prompt tokens");
    EQLONG(st.completion_tokens, 56, "completion tokens");
    prov_stream_free(&st);
    sink_free(&s);

    sink_init(&s, &cb);
    prov_stream_init(&st, &cb);
    feed(&st, "{\"error\":{\"message\":\"context length exceeded\","
              "\"type\":\"invalid_request_error\"}}");
    EQSTR(buf_cstr(&st.error), "context length exceeded", "error captured");
    EQLONG(s.errors, 1, "error callback fired");
    prov_stream_free(&st);
    sink_free(&s);
}

/* One bad frame must not discard a turn that is otherwise arriving fine. */
static void
test_stream_malformed_survives(void)
{
    prov_stream    st;
    prov_callbacks cb;
    sink           s;

    sink_init(&s, &cb);
    prov_stream_init(&st, &cb);

    feed(&st, "{\"choices\":[{\"delta\":{\"content\":\"a\"}}]}");
    feed(&st, "{\"choices\":[{\"delta\":{\"conte");     /* truncated */
    feed(&st, "not json at all");
    feed(&st, "{\"choices\":[{\"delta\":{\"content\":\"b\"}}]}");

    EQLONG(st.bad_events, 2, "malformed frames counted");
    EQSTR(buf_cstr(&st.text), "ab", "good frames still accumulate");

    prov_stream_free(&st);
    sink_free(&s);
}

/* Non-streamed responses put the payload under "message" rather than "delta". */
static void
test_stream_non_streaming_shape(void)
{
    prov_stream    st;
    prov_callbacks cb;
    sink           s;

    sink_init(&s, &cb);
    prov_stream_init(&st, &cb);
    feed(&st, "{\"choices\":[{\"message\":{\"role\":\"assistant\","
              "\"content\":\"whole answer\"},\"finish_reason\":\"stop\"}]}");

    EQSTR(buf_cstr(&st.text), "whole answer", "message shape handled");
    EQLONG(st.finish, PROV_FINISH_STOP, "finish reason read");

    prov_stream_free(&st);
    sink_free(&s);
}

/* Drive the decoder through a real SSE parser at every read size, so framing
 * and decoding are exercised together rather than separately. */
static void
test_stream_through_sse(void)
{
    /* Parenthesised so clang does not read the wrapped literals as a missing
     * comma between array elements. */
    static const char *frames[] = {
        ("data: {\"choices\":[{\"delta\":{\"content\":\"Read\"}}]}\n\n"),
        ("data: {\"choices\":[{\"delta\":{\"content\":\"ing \342\226\266\"}}]}\n\n"),
        ("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"id\":\"c1\",\"function\":{\"name\":\"read\","
         "\"arguments\":\"{\\\"path\\\":\"}}]}}]}\n\n"),
        ("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"function\":{\"arguments\":\"\\\"C:\\\\\\\\A.TXT\\\"}\"}}]}}]}\n\n"),
        ("data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"),
        ("data: [DONE]\n\n")
    };
    buf    wire;
    size_t sz;
    int    i;
    int    bad = 0;

    buf_init(&wire);
    for (i = 0; i < 6; i++)
        buf_puts(&wire, frames[i]);

    for (sz = 1; sz <= 48; sz++) {
        prov_stream    st;
        prov_callbacks cb;
        sink           s;
        sse_parser     p;
        size_t         off;

        sink_init(&s, &cb);
        prov_stream_init(&st, &cb);
        sse_init(&p, prov_on_sse, &st);

        for (off = 0; off < wire.len; off += sz)
            sse_feed(&p, wire.data + off,
                     (off + sz <= wire.len) ? sz : wire.len - off);
        sse_finish(&p);

        if (st.done != 1 ||
            st.bad_events != 0 ||
            prov_ncalls(&st) != 1 ||
            !prov_calls_valid(&st) ||
            strcmp(buf_cstr(&st.text), "Reading \342\226\266") != 0 ||
            strcmp(prov_call_args(&st, 0), "{\"path\":\"C:\\\\A.TXT\"}") != 0)
            bad++;

        sse_free(&p);
        prov_stream_free(&st);
        sink_free(&s);
    }

    EQLONG(bad, 0, "SSE + provider decode agree at every read size 1..48");
    buf_free(&wire);
}

/*
 * Close the loop: decode a tool call off the wire, feed it back into a request
 * as an assistant turn, and confirm the arguments survive. This is the cycle
 * the agentic loop runs every step, and it is where double-encoding bugs hide.
 */
static void
test_decode_then_resend(void)
{
    prov_stream    st;
    prov_callbacks cb;
    sink           s;
    prov_toolcall  call;
    prov_msg       msgs[3];
    arraysrc       a;
    prov_req       r;
    buf            out;
    json_arena    *ar;
    json_value    *v;
    const char    *args;

    sink_init(&s, &cb);
    prov_stream_init(&st, &cb);
    feed(&st, "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
              "\"id\":\"c9\",\"function\":{\"name\":\"write\","
              "\"arguments\":\"{\\\"path\\\":\\\"D:\\\\\\\\X.TXT\\\","
              "\\\"body\\\":\\\"a\\\\\\\"b\\\\nc\\\"}\"}}]}}]}");

    OK(prov_calls_valid(&st), "decoded call is valid");
    EQSTR(prov_call_args(&st, 0),
          "{\"path\":\"D:\\\\X.TXT\",\"body\":\"a\\\"b\\nc\"}",
          "decoded arguments");

    call.id   = prov_call_id(&st, 0);
    call.name = prov_call_name(&st, 0);
    call.args = prov_call_args(&st, 0);

    memset(msgs, 0, sizeof(msgs));
    msgs[0].role = PROV_ROLE_USER;
    msgs[0].content = "go"; msgs[0].content_len = 2;
    msgs[1].role = PROV_ROLE_ASSISTANT;
    msgs[1].calls = &call; msgs[1].ncalls = 1;
    msgs[2].role = PROV_ROLE_TOOL;
    msgs[2].content = "ok"; msgs[2].content_len = 2;
    msgs[2].tool_call_id = call.id;

    prov_req_init(&r, "m");
    r.msgs = arr_src(&a, msgs, 3);
    render(&r, &out);

    ar = json_arena_new();
    v  = json_parse(ar, out.data, out.len);
    OK(v != NULL, "re-sent request is valid JSON");

    args = json_as_str(
        json_path(json_at(json_get(json_at(json_get(v, "messages"), 1),
                                   "tool_calls"), 0),
                  "function.arguments"), NULL);
    EQSTR(args, "{\"path\":\"D:\\\\X.TXT\",\"body\":\"a\\\"b\\nc\"}",
          "arguments identical after decode -> re-encode");

    {
        json_arena *a2 = json_arena_new();
        json_value *av = json_parse(a2, args, strlen(args));
        OK(av != NULL, "and still parse");
        EQSTR(json_as_str(json_get(av, "path"), NULL), "D:\\X.TXT", "path");
        EQSTR(json_as_str(json_get(av, "body"), NULL), "a\"b\nc", "body");
        json_arena_free(a2);
    }

    json_arena_free(ar);
    buf_free(&out);
    prov_stream_free(&st);
    sink_free(&s);
}

int
main(void)
{
    test_request_minimal();
    test_request_optionals();
    test_request_tools();
    test_request_toolcall_roundtrip();
    test_request_length_matches();
    test_request_empty_conversation();
    test_stream_text();
    test_stream_reasoning();
    test_stream_toolcall_fragments();
    test_stream_toolcall_no_index();
    test_stream_parallel_calls();
    test_stream_truncated_args();
    test_stream_missing_name();
    test_stream_usage_and_error();
    test_stream_malformed_survives();
    test_stream_non_streaming_shape();
    test_stream_through_sse();
    test_decode_then_resend();
    TAP_REPORT("test_provider");
}
