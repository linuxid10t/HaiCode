/* store.h - session persistence as plain files. C89.
 *
 * No SQLite. The schema is four flat things and the target may not have a
 * usable SQLite build, so sessions are directories:
 *
 *     <settings>/sessions/<id>/meta.json     title, model, cwd, counters
 *     <settings>/sessions/<id>/msgs.jsonl    one JSON object per line
 *
 * Append-only JSONL is what makes the memory budget work: store_iter walks the
 * file one record at a time and implements prov_msgsrc directly, so building a
 * request never holds more than one message. It is also trivially greppable
 * and survives a crash mid-write with at worst one truncated final line, which
 * the reader skips.
 *
 * Session ids are eight hex digits of (0x0FFFFFFF - time) << 4 | sequence:
 * sortable newest-first by plain filename order, and 8.3-safe so the session
 * directory works even on FAT.
 */
#ifndef HAIOS2_STORE_H
#define HAIOS2_STORE_H

#include <stddef.h>
#include "buf.h"
#include "json.h"
#include "provider.h"

#define STORE_ID_LEN   9     /* 8 hex digits plus NUL */
#define STORE_MAX_LIST 128

typedef struct store {
    char root[512];          /* the settings directory */
} store;

typedef struct store_meta {
    char id[STORE_ID_LEN];
    char title[128];
    char model[64];
    char cwd[256];
    long created;
    long nmsgs;
    long tokens_in;
    long tokens_out;
} store_meta;

int  store_open(store *s, const char *root);

/* Creates a session directory and meta.json. Fills `id_out` (STORE_ID_LEN). */
int  store_create(store *s, const char *cwd, const char *model, char *id_out);

int  store_read_meta(store *s, const char *id, store_meta *out);
int  store_write_meta(store *s, const store_meta *m);

/* Newest first. Returns the count written, or -1. */
int  store_list(store *s, store_meta *out, int max);

int  store_delete(store *s, const char *id);

/* ---------------------------------------------------------------- append -- */

/* Appends one record. `calls` applies to assistant turns, `tool_call_id` to
 * tool turns. Flushed before returning, so a crash loses nothing acknowledged. */
int store_append(store *s, const char *id, int role,
                 const char *content, size_t content_len,
                 const prov_toolcall *calls, size_t ncalls,
                 const char *tool_call_id);

/* ------------------------------------------------------------------ read -- */

/*
 * Streams messages back out. Fill a prov_msgsrc with store_iter_rewind and
 * store_iter_next to feed prov_write_request directly.
 *
 * Everything the iterator hands out is owned by it and is invalidated by the
 * next call, which is exactly the contract prov_msgsrc documents.
 */
typedef struct store_iter {
    void         *fp;                 /* FILE * */
    buf           line;
    json_arena   *arena;
    prov_toolcall calls[PROV_MAX_TOOLCALLS];
    int           ncalls;
    long          skipped;            /* truncated or unparseable lines */
} store_iter;

int  store_iter_open(store_iter *it, store *s, const char *id);
void store_iter_close(store_iter *it);
void store_iter_rewind(void *it);           /* prov_msgsrc.rewind */
int  store_iter_next(void *it, prov_msg *out);  /* prov_msgsrc.next */

/* Convenience: a prov_msgsrc wired to an open iterator. */
prov_msgsrc store_iter_src(store_iter *it);

#endif
