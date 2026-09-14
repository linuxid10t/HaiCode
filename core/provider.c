/* provider.c - OpenAI-compatible chat completions. C89. */

#include "provider.h"
#include "json.h"
#include "buf.h"

#include <string.h>

/* ------------------------------------------------------------- request --- */

void
prov_req_init(prov_req *r, const char *model)
{
    r->model            = model;
    r->stream           = 1;
    r->max_tokens       = 0;
    r->temperature      = 0.0;
    r->have_temperature = 0;
    r->top_p            = 0.0;
    r->have_top_p       = 0;
    r->tools            = NULL;
    r->ntools           = 0;
    r->msgs.ctx         = NULL;
    r->msgs.rewind      = NULL;
    r->msgs.next        = NULL;
}

static const char *
prov_role_name(int role)
{
    switch (role) {
    case PROV_ROLE_SYSTEM:    return "system";
    case PROV_ROLE_USER:      return "user";
    case PROV_ROLE_ASSISTANT: return "assistant";
    case PROV_ROLE_TOOL:      return "tool";
    default:                  return "user";
    }
}

static void
prov_write_msg(json_writer *w, const prov_msg *m)
{
    size_t i;

    json_w_obj_open(w);

    json_w_key(w, "role");
    json_w_str(w, prov_role_name(m->role));

    if (m->role == PROV_ROLE_TOOL && m->tool_call_id != NULL) {
        json_w_key(w, "tool_call_id");
        json_w_str(w, m->tool_call_id);
    }

    /* An assistant turn that is nothing but tool calls still needs a content
     * key; servers differ on whether they tolerate its absence, and null is
     * accepted everywhere. */
    json_w_key(w, "content");
    if (m->content != NULL)
        json_w_strn(w, m->content, m->content_len);
    else
        json_w_null(w);

    if (m->role == PROV_ROLE_ASSISTANT && m->ncalls > 0) {
        json_w_key(w, "tool_calls");
        json_w_arr_open(w);
        for (i = 0; i < m->ncalls; i++) {
            json_w_obj_open(w);
              json_w_key(w, "id");
              json_w_str(w, m->calls[i].id);
              json_w_key(w, "type");
              json_w_str(w, "function");
              json_w_key(w, "function");
              json_w_obj_open(w);
                json_w_key(w, "name");
                json_w_str(w, m->calls[i].name);
                /* arguments is a string carrying JSON text, not an object. */
                json_w_key(w, "arguments");
                json_w_str(w, (m->calls[i].args != NULL) ? m->calls[i].args
                                                         : "{}");
              json_w_obj_close(w);
            json_w_obj_close(w);
        }
        json_w_arr_close(w);
    }

    json_w_obj_close(w);
}

int
prov_write_request(json_writer *w, const prov_req *r)
{
    prov_msg m;
    size_t   i;

    if (r->msgs.rewind != NULL)
        r->msgs.rewind(r->msgs.ctx);

    json_w_obj_open(w);

    json_w_key(w, "model");
    json_w_str(w, (r->model != NULL) ? r->model : "");

    json_w_key(w, "stream");
    json_w_bool(w, r->stream);

    if (r->max_tokens > 0) {
        json_w_key(w, "max_tokens");
        json_w_long(w, r->max_tokens);
    }
    if (r->have_temperature) {
        json_w_key(w, "temperature");
        json_w_double(w, r->temperature);
    }
    if (r->have_top_p) {
        json_w_key(w, "top_p");
        json_w_double(w, r->top_p);
    }

    json_w_key(w, "messages");
    json_w_arr_open(w);
    if (r->msgs.next != NULL) {
        memset(&m, 0, sizeof(m));
        while (r->msgs.next(r->msgs.ctx, &m)) {
            prov_write_msg(w, &m);
            memset(&m, 0, sizeof(m));
        }
    }
    json_w_arr_close(w);

    if (r->ntools > 0 && r->tools != NULL) {
        json_w_key(w, "tools");
        json_w_arr_open(w);
        for (i = 0; i < r->ntools; i++) {
            json_w_obj_open(w);
              json_w_key(w, "type");
              json_w_str(w, "function");
              json_w_key(w, "function");
              json_w_obj_open(w);
                json_w_key(w, "name");
                json_w_str(w, r->tools[i].name);
                json_w_key(w, "description");
                json_w_str(w, r->tools[i].description);
                json_w_key(w, "parameters");
                if (r->tools[i].schema != NULL)
                    json_w_raw(w, r->tools[i].schema,
                               strlen(r->tools[i].schema));
                else
                    json_w_raw(w, "{\"type\":\"object\"}", 17);
              json_w_obj_close(w);
            json_w_obj_close(w);
        }
        json_w_arr_close(w);
    }

    json_w_obj_close(w);
    return json_w_finish(w);
}

long
prov_request_length(const prov_req *r)
{
    json_writer w;
    long        len = 0;

    json_w_init(&w, json_count_sink, &len);
    if (prov_write_request(&w, r) != 0)
        return -1;
    return len;
}

/* ------------------------------------------------------------- response -- */

void
prov_stream_init(prov_stream *s, const prov_callbacks *cb)
{
    int i;

    if (cb != NULL) {
        s->cb = *cb;
    } else {
        s->cb.ctx          = NULL;
        s->cb.on_text      = NULL;
        s->cb.on_reasoning = NULL;
        s->cb.on_error     = NULL;
    }

    buf_init(&s->text);
    buf_init(&s->reasoning);
    buf_init(&s->error);

    for (i = 0; i < PROV_MAX_TOOLCALLS; i++) {
        buf_init(&s->calls[i].id);
        buf_init(&s->calls[i].name);
        buf_init(&s->calls[i].args);
        s->calls[i].used = 0;
    }

    s->ncalls            = 0;
    s->last_slot         = -1;
    s->finish            = PROV_FINISH_NONE;
    s->prompt_tokens     = 0;
    s->completion_tokens = 0;
    s->done              = 0;
    s->bad_events        = 0;
    s->oom               = 0;
}

void
prov_stream_free(prov_stream *s)
{
    int i;

    buf_free(&s->text);
    buf_free(&s->reasoning);
    buf_free(&s->error);
    for (i = 0; i < PROV_MAX_TOOLCALLS; i++) {
        buf_free(&s->calls[i].id);
        buf_free(&s->calls[i].name);
        buf_free(&s->calls[i].args);
    }
}

static int
prov_finish_code(const char *s)
{
    if (s == NULL)                     return PROV_FINISH_NONE;
    if (strcmp(s, "stop") == 0)        return PROV_FINISH_STOP;
    if (strcmp(s, "length") == 0)      return PROV_FINISH_LENGTH;
    if (strcmp(s, "tool_calls") == 0)  return PROV_FINISH_TOOLS;
    if (strcmp(s, "function_call") == 0) return PROV_FINISH_TOOLS;
    if (strcmp(s, "content_filter") == 0) return PROV_FINISH_FILTER;
    return PROV_FINISH_OTHER;
}

static void
prov_note_error(prov_stream *s, const char *msg)
{
    buf_clear(&s->error);
    if (buf_puts(&s->error, msg) != 0)
        s->oom = 1;
    if (s->cb.on_error != NULL) {
        char *e = buf_cstr(&s->error);
        s->cb.on_error(s->cb.ctx, (e != NULL) ? e : "");
    }
}

/*
 * Merge one tool_calls[] delta into its accumulator slot.
 *
 * The wire form sends id and name once, then dribbles `arguments` out as
 * fragments that only become valid JSON once concatenated. `index` selects the
 * slot; some servers omit it, in which case we continue whichever slot was
 * last touched -- appending a fragment to the wrong call produces a tool
 * invocation that looks plausible and is wrong, which is worse than an error.
 */
static void
prov_merge_call(prov_stream *s, json_value *tc)
{
    json_value *idx;
    json_value *fn;
    const char *str;
    int         slot;

    idx = json_get(tc, "index");
    if (json_type_of(idx) == JSON_NUM) {
        slot = (int)json_as_long(idx, 0);
    } else if (s->last_slot >= 0) {
        slot = s->last_slot;
    } else {
        slot = 0;
    }

    if (slot < 0 || slot >= PROV_MAX_TOOLCALLS)
        return;

    s->last_slot         = slot;
    s->calls[slot].used  = 1;
    if (slot + 1 > s->ncalls)
        s->ncalls = slot + 1;

    str = json_as_str(json_get(tc, "id"), NULL);
    if (str != NULL && *str != '\0') {
        buf_clear(&s->calls[slot].id);
        if (buf_puts(&s->calls[slot].id, str) != 0)
            s->oom = 1;
    }

    fn = json_get(tc, "function");
    if (fn == NULL)
        return;

    str = json_as_str(json_get(fn, "name"), NULL);
    if (str != NULL && *str != '\0') {
        /* The name arrives once; a later empty string must not erase it. */
        buf_clear(&s->calls[slot].name);
        if (buf_puts(&s->calls[slot].name, str) != 0)
            s->oom = 1;
    }

    str = json_as_str(json_get(fn, "arguments"), NULL);
    if (str != NULL) {
        if (buf_puts(&s->calls[slot].args, str) != 0)
            s->oom = 1;
    }
}

static void
prov_handle_choice(prov_stream *s, json_value *choice)
{
    json_value *delta;
    json_value *calls;
    json_value *fr;
    const char *str;
    size_t      i;

    fr = json_get(choice, "finish_reason");
    if (json_type_of(fr) == JSON_STR)
        s->finish = prov_finish_code(json_as_str(fr, NULL));

    delta = json_get(choice, "delta");
    if (delta == NULL)
        delta = json_get(choice, "message");   /* non-streamed responses */
    if (delta == NULL)
        return;

    str = json_as_str(json_get(delta, "content"), NULL);
    if (str != NULL && *str != '\0') {
        size_t n = strlen(str);
        if (buf_append(&s->text, str, n) != 0)
            s->oom = 1;
        if (s->cb.on_text != NULL)
            s->cb.on_text(s->cb.ctx, str, n);
    }

    /* Emitted by DeepSeek-style servers and by llama.cpp with some templates. */
    str = json_as_str(json_get(delta, "reasoning_content"), NULL);
    if (str == NULL)
        str = json_as_str(json_get(delta, "reasoning"), NULL);
    if (str != NULL && *str != '\0') {
        size_t n = strlen(str);
        if (buf_append(&s->reasoning, str, n) != 0)
            s->oom = 1;
        if (s->cb.on_reasoning != NULL)
            s->cb.on_reasoning(s->cb.ctx, str, n);
    }

    calls = json_get(delta, "tool_calls");
    if (json_type_of(calls) == JSON_ARR) {
        for (i = 0; i < json_len(calls); i++)
            prov_merge_call(s, json_at(calls, i));
    }
}

void
prov_on_sse(void *ctx, const char *event, const char *data, size_t dlen)
{
    prov_stream *s = (prov_stream *)ctx;
    json_arena  *a;
    json_value  *root;
    json_value  *choices;
    json_value  *usage;
    json_value  *err;
    size_t       i;

    (void)event;   /* the OpenAI shape carries everything in data: */

    if (dlen == 6 && memcmp(data, "[DONE]", 6) == 0) {
        s->done = 1;
        return;
    }
    if (dlen == 0)
        return;

    a = json_arena_new();
    if (a == NULL) {
        s->oom = 1;
        return;
    }

    root = json_parse(a, data, dlen);
    if (root == NULL) {
        /* Count it and carry on: one malformed keep-alive or truncated frame
         * must not discard a turn that is otherwise arriving fine. */
        s->bad_events++;
        json_arena_free(a);
        return;
    }

    err = json_get(root, "error");
    if (err != NULL) {
        const char *msg = json_as_str(json_get(err, "message"), NULL);
        if (msg == NULL)
            msg = json_as_str(err, "unknown provider error");
        prov_note_error(s, msg);
        json_arena_free(a);
        return;
    }

    choices = json_get(root, "choices");
    if (json_type_of(choices) == JSON_ARR) {
        for (i = 0; i < json_len(choices); i++)
            prov_handle_choice(s, json_at(choices, i));
    }

    /* Usage rides the final chunk when stream_options.include_usage is on, and
     * is simply absent otherwise. */
    usage = json_get(root, "usage");
    if (usage != NULL) {
        long v;
        v = json_as_long(json_get(usage, "prompt_tokens"), -1);
        if (v >= 0) s->prompt_tokens = v;
        v = json_as_long(json_get(usage, "completion_tokens"), -1);
        if (v >= 0) s->completion_tokens = v;
    }

    json_arena_free(a);
}

/* ------------------------------------------------------------- read-out -- */

int
prov_ncalls(const prov_stream *s)
{
    return s->ncalls;
}

static const char *
prov_call_field(prov_stream *s, int i, buf *b)
{
    if (i < 0 || i >= s->ncalls || !s->calls[i].used)
        return NULL;
    return buf_cstr(b);
}

const char *
prov_call_id(prov_stream *s, int i)
{
    if (i < 0 || i >= s->ncalls) return NULL;
    return prov_call_field(s, i, &s->calls[i].id);
}

const char *
prov_call_name(prov_stream *s, int i)
{
    if (i < 0 || i >= s->ncalls) return NULL;
    return prov_call_field(s, i, &s->calls[i].name);
}

const char *
prov_call_args(prov_stream *s, int i)
{
    if (i < 0 || i >= s->ncalls) return NULL;
    return prov_call_field(s, i, &s->calls[i].args);
}

int
prov_calls_valid(prov_stream *s)
{
    json_arena *a;
    int         i;
    int         ok = 1;

    if (s->ncalls == 0)
        return 1;

    a = json_arena_new();
    if (a == NULL)
        return 0;

    for (i = 0; i < s->ncalls && ok; i++) {
        const char *name = prov_call_name(s, i);
        const char *args = prov_call_args(s, i);

        if (!s->calls[i].used || name == NULL || *name == '\0') {
            ok = 0;
            break;
        }
        /* Empty arguments are legitimate for a no-parameter tool; anything
         * else must parse, or the fragments did not all arrive. */
        if (args != NULL && *args != '\0') {
            if (json_parse(a, args, strlen(args)) == NULL)
                ok = 0;
        }
    }

    json_arena_free(a);
    return ok;
}
