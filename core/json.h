/* json.h - JSON reader and writer for haios2.
 *
 * C89. Depends only on buf.h and the C standard library.
 *
 * Two independent halves:
 *
 *   WRITER (json_w_*) is sink-based. It never materializes the document; every
 *   byte goes straight to a caller-supplied callback. This is what lets a
 *   ~1.4 MB chat request be emitted straight onto a socket on a machine with
 *   16 MB of RAM. Pass json_count_sink to measure a document without sending
 *   it, which is how you obtain a Content-Length for a body you intend to
 *   stream (emit twice: once to count, once to send).
 *
 *   READER (json_parse / json_* accessors) builds a small DOM in an arena.
 *   Intended for the individual SSE event payloads, which are a few hundred
 *   bytes each. Free the whole document with one json_arena_free().
 *
 * Strings are handled as UTF-8. The writer replaces malformed sequences with
 * U+FFFD rather than failing, so a tool that emits a stray 0x80 byte cannot
 * abort a request. The reader decodes \uXXXX escapes, including surrogate
 * pairs, into UTF-8.
 */
#ifndef HAIOS2_JSON_H
#define HAIOS2_JSON_H

#include <stddef.h>

#define JSON_MAX_DEPTH 64

/* ---------------------------------------------------------------- writer -- */

/* Return 0 to continue, non-zero to abort the emit. */
typedef int (*json_sink)(void *ctx, const char *data, size_t len);

typedef struct json_writer {
    json_sink      sink;
    void          *ctx;
    int            err;      /* sticky: first error code, or 0 */
    int            depth;    /* index of the current frame; -1 = none open */
    unsigned char  is_obj[JSON_MAX_DEPTH];
    unsigned char  need_comma[JSON_MAX_DEPTH];
    unsigned char  awaiting[JSON_MAX_DEPTH];  /* key written, value pending */
} json_writer;

void json_w_init(json_writer *w, json_sink sink, void *ctx);
int  json_w_obj_open(json_writer *w);
int  json_w_obj_close(json_writer *w);
int  json_w_arr_open(json_writer *w);
int  json_w_arr_close(json_writer *w);
int  json_w_key(json_writer *w, const char *k);
int  json_w_keyn(json_writer *w, const char *k, size_t n);
int  json_w_str(json_writer *w, const char *s);
int  json_w_strn(json_writer *w, const char *s, size_t n);
int  json_w_long(json_writer *w, long v);
int  json_w_double(json_writer *w, double v);
int  json_w_bool(json_writer *w, int v);
int  json_w_null(json_writer *w);
/* Emit already-encoded JSON text as one value (no escaping, no validation). */
int  json_w_raw(json_writer *w, const char *s, size_t n);
/* 0 when the document is well-formed and fully closed. */
int  json_w_finish(json_writer *w);

/* A sink that discards bytes and counts them. Pass a `long *` as ctx. */
int  json_count_sink(void *ctx, const char *data, size_t len);
/* A sink that appends to a `buf *`. Useful in tests and for small documents. */
int  json_buf_sink(void *ctx, const char *data, size_t len);

/* ---------------------------------------------------------------- reader -- */

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUM,
    JSON_STR,
    JSON_ARR,
    JSON_OBJ
} json_type;

typedef struct json_arena  json_arena;
typedef struct json_value  json_value;
typedef struct json_member json_member;

struct json_member {
    char       *key;
    size_t      klen;
    json_value *val;
};

struct json_value {
    json_type type;
    union {
        int    boolean;
        double num;
        struct { char *p; size_t n; }        str;
        struct { json_value **v; size_t n; } arr;
        struct { json_member *m; size_t n; } obj;
    } u;
};

json_arena *json_arena_new(void);
void        json_arena_free(json_arena *a);
void       *json_arena_alloc(json_arena *a, size_t n);

/* Parses `text` (need not be NUL-terminated). Returns NULL on malformed input.
 * The returned tree is owned by `a`. */
json_value *json_parse(json_arena *a, const char *text, size_t len);

/* Accessors. All are NULL-safe and type-safe: a mismatch returns the default. */
json_type   json_type_of(const json_value *v);
const char *json_as_str(const json_value *v, const char *dflt);
double      json_as_num(const json_value *v, double dflt);
long        json_as_long(const json_value *v, long dflt);
int         json_as_bool(const json_value *v, int dflt);
size_t      json_len(const json_value *v);              /* arr or obj */
json_value *json_at(const json_value *v, size_t i);     /* arr */
json_value *json_get(const json_value *v, const char *key);   /* obj */
/* Dotted lookup: json_path(root, "delta.content") */
json_value *json_path(const json_value *v, const char *path);

#endif
