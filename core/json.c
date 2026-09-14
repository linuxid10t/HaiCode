/* json.c - JSON reader and writer. C89. */

#include "json.h"
#include "buf.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <float.h>

/* ================================================================= arena == */

typedef union {
    double  d;
    long    l;
    void   *p;
} arena_align;

#define ARENA_ALIGN   (sizeof(arena_align))
#define ARENA_BLOCK   4096

typedef struct arena_block {
    struct arena_block *next;
    size_t              cap;
    size_t              used;
} arena_block;

struct json_arena {
    arena_block *head;
};

json_arena *
json_arena_new(void)
{
    json_arena *a = (json_arena *)malloc(sizeof(json_arena));
    if (a == NULL)
        return NULL;
    a->head = NULL;
    return a;
}

void
json_arena_free(json_arena *a)
{
    arena_block *b;
    arena_block *next;

    if (a == NULL)
        return;
    b = a->head;
    while (b != NULL) {
        next = b->next;
        free(b);
        b = next;
    }
    free(a);
}

void *
json_arena_alloc(json_arena *a, size_t n)
{
    arena_block *b;
    size_t       cap;
    char        *p;

    /* Round up to the alignment unit. */
    n = (n + ARENA_ALIGN - 1) / ARENA_ALIGN * ARENA_ALIGN;
    if (n == 0)
        n = ARENA_ALIGN;

    b = a->head;
    if (b == NULL || b->cap - b->used < n) {
        cap = (n > ARENA_BLOCK) ? n : ARENA_BLOCK;
        b = (arena_block *)malloc(sizeof(arena_block) + cap);
        if (b == NULL)
            return NULL;
        b->next = a->head;
        b->cap  = cap;
        b->used = 0;
        a->head = b;
    }

    p = (char *)(b + 1) + b->used;
    b->used += n;
    return p;
}

/* ================================================================ writer == */

/* Error codes are negative so a sink's own non-zero return can pass through. */
#define JW_E_DEPTH  (-1)
#define JW_E_STATE  (-2)

void
json_w_init(json_writer *w, json_sink sink, void *ctx)
{
    w->sink  = sink;
    w->ctx   = ctx;
    w->err   = 0;
    w->depth = -1;
}

static int
w_emit(json_writer *w, const char *p, size_t n)
{
    int rc;

    if (w->err != 0)
        return w->err;
    if (n == 0)
        return 0;
    rc = w->sink(w->ctx, p, n);
    if (rc != 0)
        w->err = rc;
    return w->err;
}

static int
w_emit_s(json_writer *w, const char *s)
{
    return w_emit(w, s, strlen(s));
}

/* Emit the separator required before the next value or key at this depth. */
static int
w_pre(json_writer *w, int is_key)
{
    int d;

    if (w->err != 0)
        return w->err;

    d = w->depth;
    if (d < 0)
        return 0;   /* top-level value, nothing precedes it */

    if (w->is_obj[d] && !is_key && w->awaiting[d]) {
        /* Value that follows a key: the ':' was already emitted. */
        return 0;
    }
    if (w->is_obj[d] && !is_key && !w->awaiting[d]) {
        /* A bare value inside an object is a programming error. */
        w->err = JW_E_STATE;
        return w->err;
    }
    if (w->need_comma[d])
        return w_emit(w, ",", 1);
    return 0;
}

/* Record that a complete value was just written at the current depth. */
static void
w_post(json_writer *w)
{
    int d = w->depth;
    if (d < 0)
        return;
    w->need_comma[d] = 1;
    w->awaiting[d]   = 0;
}

static int
w_push(json_writer *w, int is_obj)
{
    if (w->err != 0)
        return w->err;
    if (w->depth + 1 >= JSON_MAX_DEPTH) {
        w->err = JW_E_DEPTH;
        return w->err;
    }
    w->depth++;
    w->is_obj[w->depth]     = (unsigned char)(is_obj ? 1 : 0);
    w->need_comma[w->depth] = 0;
    w->awaiting[w->depth]   = 0;
    return 0;
}

static int
w_pop(json_writer *w, int is_obj)
{
    if (w->err != 0)
        return w->err;
    if (w->depth < 0 || w->is_obj[w->depth] != (unsigned char)(is_obj ? 1 : 0)) {
        w->err = JW_E_STATE;
        return w->err;
    }
    w->depth--;
    w_post(w);
    return 0;
}

int
json_w_obj_open(json_writer *w)
{
    if (w_pre(w, 0) != 0) return w->err;
    if (w_emit(w, "{", 1) != 0) return w->err;
    return w_push(w, 1);
}

int
json_w_obj_close(json_writer *w)
{
    if (w->err != 0) return w->err;
    if (w->depth < 0 || !w->is_obj[w->depth]) {
        w->err = JW_E_STATE;
        return w->err;
    }
    if (w->awaiting[w->depth]) {   /* key with no value */
        w->err = JW_E_STATE;
        return w->err;
    }
    if (w_emit(w, "}", 1) != 0) return w->err;
    return w_pop(w, 1);
}

int
json_w_arr_open(json_writer *w)
{
    if (w_pre(w, 0) != 0) return w->err;
    if (w_emit(w, "[", 1) != 0) return w->err;
    return w_push(w, 0);
}

int
json_w_arr_close(json_writer *w)
{
    if (w->err != 0) return w->err;
    if (w->depth < 0 || w->is_obj[w->depth]) {
        w->err = JW_E_STATE;
        return w->err;
    }
    if (w_emit(w, "]", 1) != 0) return w->err;
    return w_pop(w, 0);
}

/* --- UTF-8 aware string escaping ---------------------------------------- */

static const char hexdig[] = "0123456789abcdef";

static int
w_escape_u(json_writer *w, unsigned int cp)
{
    char out[6];

    out[0] = '\\';
    out[1] = 'u';
    out[2] = hexdig[(cp >> 12) & 0xF];
    out[3] = hexdig[(cp >>  8) & 0xF];
    out[4] = hexdig[(cp >>  4) & 0xF];
    out[5] = hexdig[cp & 0xF];
    return w_emit(w, out, 6);
}

/*
 * Length of the valid UTF-8 sequence starting at s[0], or 0 if the bytes there
 * are not a well-formed sequence. Rejects overlong encodings, surrogates
 * (U+D800..U+DFFF) and anything above U+10FFFF, matching RFC 3629.
 */
static size_t
utf8_seq_len(const unsigned char *s, size_t avail)
{
    unsigned char c0;
    unsigned char c1;

    if (avail == 0)
        return 0;
    c0 = s[0];

    if (c0 < 0x80)
        return 1;
    if (c0 < 0xC2)              /* continuation byte, or overlong 2-byte lead */
        return 0;

    if (c0 <= 0xDF) {
        if (avail < 2) return 0;
        if ((s[1] & 0xC0) != 0x80) return 0;
        return 2;
    }

    if (c0 <= 0xEF) {
        if (avail < 3) return 0;
        c1 = s[1];
        if ((c1 & 0xC0) != 0x80) return 0;
        if ((s[2] & 0xC0) != 0x80) return 0;
        if (c0 == 0xE0 && c1 < 0xA0) return 0;   /* overlong */
        if (c0 == 0xED && c1 > 0x9F) return 0;   /* surrogate half */
        return 3;
    }

    if (c0 <= 0xF4) {
        if (avail < 4) return 0;
        c1 = s[1];
        if ((c1 & 0xC0) != 0x80) return 0;
        if ((s[2] & 0xC0) != 0x80) return 0;
        if ((s[3] & 0xC0) != 0x80) return 0;
        if (c0 == 0xF0 && c1 < 0x90) return 0;   /* overlong */
        if (c0 == 0xF4 && c1 > 0x8F) return 0;   /* > U+10FFFF */
        return 4;
    }

    return 0;
}

/*
 * Writes s[0..n) as a quoted JSON string.
 *
 * Malformed UTF-8 is replaced with U+FFFD rather than rejected. A tool that
 * emits a stray high byte must not be able to abort an in-flight request --
 * this is the same failure the Haiku build hit with a strict encoder.
 */
static int
w_string(json_writer *w, const char *s, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    size_t run;
    size_t seq;

    if (w_emit(w, "\"", 1) != 0) return w->err;

    while (i < n) {
        unsigned char c = p[i];

        if (c == '"' || c == '\\') {
            char esc[2];
            esc[0] = '\\';
            esc[1] = (char)c;
            if (w_emit(w, esc, 2) != 0) return w->err;
            i++;
            continue;
        }
        if (c < 0x20) {
            switch (c) {
            case '\n': if (w_emit(w, "\\n", 2) != 0) return w->err; break;
            case '\r': if (w_emit(w, "\\r", 2) != 0) return w->err; break;
            case '\t': if (w_emit(w, "\\t", 2) != 0) return w->err; break;
            case '\b': if (w_emit(w, "\\b", 2) != 0) return w->err; break;
            case '\f': if (w_emit(w, "\\f", 2) != 0) return w->err; break;
            default:   if (w_escape_u(w, c) != 0) return w->err; break;
            }
            i++;
            continue;
        }
        if (c < 0x80) {
            /* Emit the longest run of plain ASCII in one call. */
            run = i;
            while (run < n) {
                unsigned char d = p[run];
                if (d < 0x20 || d >= 0x80 || d == '"' || d == '\\')
                    break;
                run++;
            }
            if (w_emit(w, (const char *)(p + i), run - i) != 0) return w->err;
            i = run;
            continue;
        }

        seq = utf8_seq_len(p + i, n - i);
        if (seq == 0) {
            /* U+FFFD REPLACEMENT CHARACTER, then resynchronise one byte on. */
            if (w_emit(w, "\357\277\275", 3) != 0) return w->err;
            i++;
        } else {
            if (w_emit(w, (const char *)(p + i), seq) != 0) return w->err;
            i += seq;
        }
    }

    return w_emit(w, "\"", 1);
}

int
json_w_keyn(json_writer *w, const char *k, size_t n)
{
    if (w->err != 0) return w->err;
    if (w->depth < 0 || !w->is_obj[w->depth] || w->awaiting[w->depth]) {
        w->err = JW_E_STATE;
        return w->err;
    }
    if (w_pre(w, 1) != 0) return w->err;
    if (w_string(w, k, n) != 0) return w->err;
    if (w_emit(w, ":", 1) != 0) return w->err;
    w->awaiting[w->depth]   = 1;
    w->need_comma[w->depth] = 1;
    return 0;
}

int
json_w_key(json_writer *w, const char *k)
{
    return json_w_keyn(w, k, strlen(k));
}

int
json_w_strn(json_writer *w, const char *s, size_t n)
{
    if (w_pre(w, 0) != 0) return w->err;
    if (w_string(w, s, n) != 0) return w->err;
    w_post(w);
    return 0;
}

int
json_w_str(json_writer *w, const char *s)
{
    if (s == NULL)
        return json_w_null(w);
    return json_w_strn(w, s, strlen(s));
}

int
json_w_long(json_writer *w, long v)
{
    char tmp[32];

    if (w_pre(w, 0) != 0) return w->err;
    sprintf(tmp, "%ld", v);
    if (w_emit_s(w, tmp) != 0) return w->err;
    w_post(w);
    return 0;
}

/*
 * Shortest representation that still reads back as the same double.
 *
 * %.17g always round-trips but renders 0.95 as 0.94999999999999996, which is
 * both ugly in a request and ~15 wasted bytes per number. C89 has no
 * shortest-form conversion, so try increasing precision and stop at the first
 * one that survives a strtod round trip.
 */
static void
w_fmt_double(char *out, double v)
{
    int prec;

    for (prec = 15; prec < 17; prec++) {
        sprintf(out, "%.*g", prec, v);
        if (strtod(out, NULL) == v)
            return;
    }
    sprintf(out, "%.17g", v);
}

int
json_w_double(json_writer *w, double v)
{
    char tmp[64];

    if (w_pre(w, 0) != 0) return w->err;
    /* JSON has no NaN or Infinity; C89 has no isfinite(). */
    if (v != v || v > DBL_MAX || v < -DBL_MAX) {
        if (w_emit(w, "null", 4) != 0) return w->err;
    } else {
        w_fmt_double(tmp, v);
        if (w_emit_s(w, tmp) != 0) return w->err;
    }
    w_post(w);
    return 0;
}

int
json_w_bool(json_writer *w, int v)
{
    if (w_pre(w, 0) != 0) return w->err;
    if (v) {
        if (w_emit(w, "true", 4) != 0) return w->err;
    } else {
        if (w_emit(w, "false", 5) != 0) return w->err;
    }
    w_post(w);
    return 0;
}

int
json_w_null(json_writer *w)
{
    if (w_pre(w, 0) != 0) return w->err;
    if (w_emit(w, "null", 4) != 0) return w->err;
    w_post(w);
    return 0;
}

int
json_w_raw(json_writer *w, const char *s, size_t n)
{
    if (w_pre(w, 0) != 0) return w->err;
    if (w_emit(w, s, n) != 0) return w->err;
    w_post(w);
    return 0;
}

int
json_w_finish(json_writer *w)
{
    if (w->err != 0)
        return w->err;
    if (w->depth != -1)
        w->err = JW_E_STATE;
    return w->err;
}

int
json_count_sink(void *ctx, const char *data, size_t len)
{
    (void)data;
    *(long *)ctx += (long)len;
    return 0;
}

int
json_buf_sink(void *ctx, const char *data, size_t len)
{
    return buf_append((buf *)ctx, data, len);
}

/* ================================================================ reader == */

typedef struct {
    const char *p;
    const char *end;
    json_arena *a;
    int         depth;
    int         bad;
} jp;

/* Temporary singly-linked nodes, arena-allocated, flattened once the count is
 * known. Avoids a realloc loop and keeps every allocation inside the arena. */
typedef struct jp_item {
    struct jp_item *next;
    json_value     *val;
    char           *key;
    size_t          klen;
} jp_item;

static json_value *jp_value(jp *s);

static void *
jp_alloc(jp *s, size_t n)
{
    void *p = json_arena_alloc(s->a, n);
    if (p == NULL)
        s->bad = 1;
    return p;
}

static void
jp_ws(jp *s)
{
    while (s->p < s->end) {
        char c = *s->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            s->p++;
        else
            break;
    }
}

static int
jp_lit(jp *s, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(s->end - s->p) < n)
        return 0;
    if (memcmp(s->p, lit, n) != 0)
        return 0;
    s->p += n;
    return 1;
}

/* Appends the UTF-8 encoding of cp to out, returning the byte count. */
static size_t
utf8_encode(unsigned long cp, char *out)
{
    if (cp < 0x80UL) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800UL) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000UL) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int
jp_hex4(jp *s, unsigned int *out)
{
    unsigned int v = 0;
    int i;

    if ((size_t)(s->end - s->p) < 4)
        return 0;
    for (i = 0; i < 4; i++) {
        char c = s->p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')       v |= (unsigned int)(c - '0');
        else if (c >= 'a' && c <= 'f')  v |= (unsigned int)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')  v |= (unsigned int)(c - 'A' + 10);
        else return 0;
    }
    s->p += 4;
    *out = v;
    return 1;
}

/* Parses a string literal (s->p is on the opening quote) into arena memory. */
static int
jp_string(jp *s, char **out, size_t *outlen)
{
    buf   acc;
    char  enc[4];
    char *dst;

    if (s->p >= s->end || *s->p != '"')
        return 0;
    s->p++;

    buf_init(&acc);

    for (;;) {
        const char *run;
        unsigned char c;

        if (s->p >= s->end) { buf_free(&acc); return 0; }

        c = (unsigned char)*s->p;
        if (c == '"') {
            s->p++;
            break;
        }
        if (c == '\\') {
            s->p++;
            if (s->p >= s->end) { buf_free(&acc); return 0; }
            switch (*s->p) {
            case '"':  buf_putc(&acc, '"');  s->p++; break;
            case '\\': buf_putc(&acc, '\\'); s->p++; break;
            case '/':  buf_putc(&acc, '/');  s->p++; break;
            case 'b':  buf_putc(&acc, '\b'); s->p++; break;
            case 'f':  buf_putc(&acc, '\f'); s->p++; break;
            case 'n':  buf_putc(&acc, '\n'); s->p++; break;
            case 'r':  buf_putc(&acc, '\r'); s->p++; break;
            case 't':  buf_putc(&acc, '\t'); s->p++; break;
            case 'u': {
                unsigned int  hi;
                unsigned int  lo;
                unsigned long cp;

                s->p++;
                if (!jp_hex4(s, &hi)) { buf_free(&acc); return 0; }
                cp = hi;
                if (hi >= 0xD800 && hi <= 0xDBFF) {
                    /* High surrogate: a low surrogate must follow. */
                    if ((size_t)(s->end - s->p) >= 2 &&
                        s->p[0] == '\\' && s->p[1] == 'u') {
                        s->p += 2;
                        if (!jp_hex4(s, &lo)) { buf_free(&acc); return 0; }
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000UL
                               + (((unsigned long)hi - 0xD800UL) << 10)
                               + ((unsigned long)lo - 0xDC00UL);
                        } else {
                            cp = 0xFFFDUL;
                        }
                    } else {
                        cp = 0xFFFDUL;
                    }
                } else if (hi >= 0xDC00 && hi <= 0xDFFF) {
                    cp = 0xFFFDUL;   /* lone low surrogate */
                }
                buf_append(&acc, enc, utf8_encode(cp, enc));
                break;
            }
            default:
                buf_free(&acc);
                return 0;
            }
            continue;
        }
        if (c < 0x20) {   /* raw control character is not legal in a string */
            buf_free(&acc);
            return 0;
        }

        /* Copy the longest plain run in one go. */
        run = s->p;
        while (s->p < s->end) {
            unsigned char d = (unsigned char)*s->p;
            if (d == '"' || d == '\\' || d < 0x20)
                break;
            s->p++;
        }
        buf_append(&acc, run, (size_t)(s->p - run));
    }

    if (acc.oom) { buf_free(&acc); s->bad = 1; return 0; }

    dst = (char *)jp_alloc(s, acc.len + 1);
    if (dst == NULL) { buf_free(&acc); return 0; }
    if (acc.len > 0)
        memcpy(dst, acc.data, acc.len);
    dst[acc.len] = '\0';

    *out    = dst;
    *outlen = acc.len;
    buf_free(&acc);
    return 1;
}

/* Validates the JSON number grammar and consumes it. */
static int
jp_number(jp *s, double *out)
{
    const char *start = s->p;
    char        tmp[64];
    size_t      n;

    if (s->p < s->end && *s->p == '-')
        s->p++;

    if (s->p >= s->end) return 0;
    if (*s->p == '0') {
        s->p++;
    } else if (*s->p >= '1' && *s->p <= '9') {
        while (s->p < s->end && *s->p >= '0' && *s->p <= '9')
            s->p++;
    } else {
        return 0;
    }

    if (s->p < s->end && *s->p == '.') {
        s->p++;
        if (s->p >= s->end || *s->p < '0' || *s->p > '9')
            return 0;
        while (s->p < s->end && *s->p >= '0' && *s->p <= '9')
            s->p++;
    }

    if (s->p < s->end && (*s->p == 'e' || *s->p == 'E')) {
        s->p++;
        if (s->p < s->end && (*s->p == '+' || *s->p == '-'))
            s->p++;
        if (s->p >= s->end || *s->p < '0' || *s->p > '9')
            return 0;
        while (s->p < s->end && *s->p >= '0' && *s->p <= '9')
            s->p++;
    }

    n = (size_t)(s->p - start);
    if (n == 0 || n >= sizeof(tmp))
        return 0;
    memcpy(tmp, start, n);
    tmp[n] = '\0';
    *out = strtod(tmp, NULL);
    return 1;
}

static json_value *
jp_array(jp *s)
{
    json_value *v;
    jp_item    *head = NULL;
    jp_item    *tail = NULL;
    size_t      count = 0;
    size_t      i;

    s->p++;   /* '[' */
    jp_ws(s);

    if (s->p < s->end && *s->p == ']') {
        s->p++;
    } else {
        for (;;) {
            jp_item    *it;
            json_value *item;

            item = jp_value(s);
            if (item == NULL)
                return NULL;

            it = (jp_item *)jp_alloc(s, sizeof(jp_item));
            if (it == NULL) return NULL;
            it->next = NULL;
            it->val  = item;
            if (tail == NULL) head = it; else tail->next = it;
            tail = it;
            count++;

            jp_ws(s);
            if (s->p < s->end && *s->p == ',') { s->p++; jp_ws(s); continue; }
            if (s->p < s->end && *s->p == ']') { s->p++; break; }
            return NULL;
        }
    }

    v = (json_value *)jp_alloc(s, sizeof(json_value));
    if (v == NULL) return NULL;
    v->type = JSON_ARR;
    v->u.arr.n = count;
    v->u.arr.v = NULL;
    if (count > 0) {
        v->u.arr.v = (json_value **)jp_alloc(s, count * sizeof(json_value *));
        if (v->u.arr.v == NULL) return NULL;
        for (i = 0; i < count; i++) {
            v->u.arr.v[i] = head->val;
            head = head->next;
        }
    }
    return v;
}

static json_value *
jp_object(jp *s)
{
    json_value *v;
    jp_item    *head = NULL;
    jp_item    *tail = NULL;
    size_t      count = 0;
    size_t      i;

    s->p++;   /* '{' */
    jp_ws(s);

    if (s->p < s->end && *s->p == '}') {
        s->p++;
    } else {
        for (;;) {
            jp_item    *it;
            char       *key;
            size_t      klen;
            json_value *val;

            if (!jp_string(s, &key, &klen))
                return NULL;
            jp_ws(s);
            if (s->p >= s->end || *s->p != ':')
                return NULL;
            s->p++;
            jp_ws(s);

            val = jp_value(s);
            if (val == NULL)
                return NULL;

            it = (jp_item *)jp_alloc(s, sizeof(jp_item));
            if (it == NULL) return NULL;
            it->next = NULL;
            it->val  = val;
            it->key  = key;
            it->klen = klen;
            if (tail == NULL) head = it; else tail->next = it;
            tail = it;
            count++;

            jp_ws(s);
            if (s->p < s->end && *s->p == ',') { s->p++; jp_ws(s); continue; }
            if (s->p < s->end && *s->p == '}') { s->p++; break; }
            return NULL;
        }
    }

    v = (json_value *)jp_alloc(s, sizeof(json_value));
    if (v == NULL) return NULL;
    v->type = JSON_OBJ;
    v->u.obj.n = count;
    v->u.obj.m = NULL;
    if (count > 0) {
        v->u.obj.m = (json_member *)jp_alloc(s, count * sizeof(json_member));
        if (v->u.obj.m == NULL) return NULL;
        for (i = 0; i < count; i++) {
            v->u.obj.m[i].key  = head->key;
            v->u.obj.m[i].klen = head->klen;
            v->u.obj.m[i].val  = head->val;
            head = head->next;
        }
    }
    return v;
}

static json_value *
jp_value(jp *s)
{
    json_value *v;

    if (s->bad)
        return NULL;
    if (s->depth >= JSON_MAX_DEPTH)
        return NULL;

    jp_ws(s);
    if (s->p >= s->end)
        return NULL;

    switch (*s->p) {
    case '{':
        s->depth++;
        v = jp_object(s);
        s->depth--;
        return v;
    case '[':
        s->depth++;
        v = jp_array(s);
        s->depth--;
        return v;
    case '"': {
        char   *str;
        size_t  slen;
        if (!jp_string(s, &str, &slen))
            return NULL;
        v = (json_value *)jp_alloc(s, sizeof(json_value));
        if (v == NULL) return NULL;
        v->type      = JSON_STR;
        v->u.str.p   = str;
        v->u.str.n   = slen;
        return v;
    }
    case 't':
        if (!jp_lit(s, "true")) return NULL;
        v = (json_value *)jp_alloc(s, sizeof(json_value));
        if (v == NULL) return NULL;
        v->type = JSON_BOOL;
        v->u.boolean = 1;
        return v;
    case 'f':
        if (!jp_lit(s, "false")) return NULL;
        v = (json_value *)jp_alloc(s, sizeof(json_value));
        if (v == NULL) return NULL;
        v->type = JSON_BOOL;
        v->u.boolean = 0;
        return v;
    case 'n':
        if (!jp_lit(s, "null")) return NULL;
        v = (json_value *)jp_alloc(s, sizeof(json_value));
        if (v == NULL) return NULL;
        v->type = JSON_NULL;
        return v;
    default: {
        double d;
        if (!jp_number(s, &d)) return NULL;
        v = (json_value *)jp_alloc(s, sizeof(json_value));
        if (v == NULL) return NULL;
        v->type  = JSON_NUM;
        v->u.num = d;
        return v;
    }
    }
}

json_value *
json_parse(json_arena *a, const char *text, size_t len)
{
    jp          s;
    json_value *v;

    if (a == NULL || text == NULL)
        return NULL;

    s.p     = text;
    s.end   = text + len;
    s.a     = a;
    s.depth = 0;
    s.bad   = 0;

    v = jp_value(&s);
    if (v == NULL || s.bad)
        return NULL;

    jp_ws(&s);
    if (s.p != s.end)
        return NULL;   /* trailing garbage */
    return v;
}

/* --- accessors ----------------------------------------------------------- */

json_type
json_type_of(const json_value *v)
{
    return (v == NULL) ? JSON_NULL : v->type;
}

const char *
json_as_str(const json_value *v, const char *dflt)
{
    if (v == NULL || v->type != JSON_STR)
        return dflt;
    return v->u.str.p;
}

double
json_as_num(const json_value *v, double dflt)
{
    if (v == NULL || v->type != JSON_NUM)
        return dflt;
    return v->u.num;
}

long
json_as_long(const json_value *v, long dflt)
{
    if (v == NULL || v->type != JSON_NUM)
        return dflt;
    return (long)v->u.num;
}

int
json_as_bool(const json_value *v, int dflt)
{
    if (v == NULL || v->type != JSON_BOOL)
        return dflt;
    return v->u.boolean;
}

size_t
json_len(const json_value *v)
{
    if (v == NULL)
        return 0;
    if (v->type == JSON_ARR)
        return v->u.arr.n;
    if (v->type == JSON_OBJ)
        return v->u.obj.n;
    return 0;
}

json_value *
json_at(const json_value *v, size_t i)
{
    if (v == NULL || v->type != JSON_ARR || i >= v->u.arr.n)
        return NULL;
    return v->u.arr.v[i];
}

static json_value *
json_getn(const json_value *v, const char *key, size_t klen)
{
    size_t i;

    if (v == NULL || v->type != JSON_OBJ || key == NULL)
        return NULL;
    for (i = 0; i < v->u.obj.n; i++) {
        if (v->u.obj.m[i].klen == klen &&
            memcmp(v->u.obj.m[i].key, key, klen) == 0)
            return v->u.obj.m[i].val;
    }
    return NULL;
}

json_value *
json_get(const json_value *v, const char *key)
{
    if (key == NULL)
        return NULL;
    return json_getn(v, key, strlen(key));
}

json_value *
json_path(const json_value *v, const char *path)
{
    json_value *cur;
    const char *seg;
    const char *dot;

    if (path == NULL || v == NULL)
        return NULL;

    /* The cursor is non-const because the result is handed back to callers;
     * the tree itself is only ever read here. */
    cur = (json_value *)v;
    seg = path;
    while (cur != NULL && *seg != '\0') {
        dot = strchr(seg, '.');
        if (dot == NULL) {
            cur = json_getn(cur, seg, strlen(seg));
            break;
        }
        cur = json_getn(cur, seg, (size_t)(dot - seg));
        seg = dot + 1;
    }
    return cur;
}
