/* store.c - session persistence as plain files. C89. */

#include "store.h"
#include "plat.h"
#include "json.h"
#include "buf.h"
#include "path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
store_path(const store *s, buf *out, const char *id, const char *leaf)
{
    buf_clear(out);
    buf_puts(out, s->root);
    buf_puts(out, "/sessions");
    if (id != NULL) {
        buf_putc(out, '/');
        buf_puts(out, id);
    }
    if (leaf != NULL) {
        buf_putc(out, '/');
        buf_puts(out, leaf);
    }
    buf_cstr(out);
}

int
store_open(store *s, const char *root)
{
    buf p;
    int rc;

    if (root == NULL || *root == '\0')
        return -1;
    strncpy(s->root, root, sizeof(s->root) - 1);
    s->root[sizeof(s->root) - 1] = '\0';

    buf_init(&p);
    store_path(s, &p, NULL, NULL);
    rc = plat_mkdir_p(p.data);
    buf_free(&p);
    return rc;
}

/*
 * Descending-time id so a plain alphabetical directory listing is
 * newest-first, and eight characters so the directory name is 8.3-safe.
 * The low nibble is a per-second sequence, giving 16 sessions a second before
 * the loop below has to look for a free slot.
 */
static void
store_make_id(char *out, long t, int seq)
{
    unsigned long v = (unsigned long)(0x0FFFFFFFL - (t & 0x0FFFFFFFL));
    sprintf(out, "%07lx%x", v, (unsigned)(seq & 0xF));
}

int
store_create(store *s, const char *cwd, const char *model, char *id_out)
{
    buf        p;
    long       t = plat_time();
    int        seq;
    store_meta m;
    int        rc = -1;

    buf_init(&p);
    for (seq = 0; seq < 16; seq++) {
        plat_info info;
        store_make_id(id_out, t, seq);
        store_path(s, &p, id_out, NULL);
        if (plat_stat(p.data, &info) == 0 && info.exists)
            continue;
        if (plat_mkdir_p(p.data) != 0)
            goto done;
        rc = 0;
        break;
    }
    if (rc != 0)
        goto done;

    memset(&m, 0, sizeof(m));
    strncpy(m.id, id_out, STORE_ID_LEN - 1);
    if (cwd != NULL)
        strncpy(m.cwd, cwd, sizeof(m.cwd) - 1);
    if (model != NULL)
        strncpy(m.model, model, sizeof(m.model) - 1);
    m.created = t;
    rc = store_write_meta(s, &m);

done:
    buf_free(&p);
    return rc;
}

int
store_write_meta(store *s, const store_meta *m)
{
    buf         p;
    buf         tmp;
    buf         doc;
    json_writer w;
    FILE       *f;
    int         rc = -1;

    buf_init(&p);
    buf_init(&tmp);
    buf_init(&doc);

    json_w_init(&w, json_buf_sink, &doc);
    json_w_obj_open(&w);
      json_w_key(&w, "id");         json_w_str(&w, m->id);
      json_w_key(&w, "title");      json_w_str(&w, m->title);
      json_w_key(&w, "model");      json_w_str(&w, m->model);
      json_w_key(&w, "cwd");        json_w_str(&w, m->cwd);
      json_w_key(&w, "created");    json_w_long(&w, m->created);
      json_w_key(&w, "nmsgs");      json_w_long(&w, m->nmsgs);
      json_w_key(&w, "tokens_in");  json_w_long(&w, m->tokens_in);
      json_w_key(&w, "tokens_out"); json_w_long(&w, m->tokens_out);
    json_w_obj_close(&w);
    if (json_w_finish(&w) != 0)
        goto done;

    /* Write-then-rename: a crash never leaves a half-written meta.json that
     * would make the session unreadable. */
    store_path(s, &tmp, m->id, "meta.tmp");
    f = fopen(tmp.data, "wb");
    if (f == NULL)
        goto done;
    if (fwrite(doc.data, 1, doc.len, f) != doc.len) {
        fclose(f);
        plat_remove(tmp.data);
        goto done;
    }
    if (fclose(f) != 0) {
        plat_remove(tmp.data);
        goto done;
    }

    store_path(s, &p, m->id, "meta.json");
    plat_remove(p.data);            /* rename() will not clobber on OS/2 */
    rc = plat_rename(tmp.data, p.data);

done:
    buf_free(&p);
    buf_free(&tmp);
    buf_free(&doc);
    return rc;
}

static void
meta_str(json_value *v, const char *key, char *out, size_t n)
{
    const char *s = json_as_str(json_get(v, key), "");
    strncpy(out, s, n - 1);
    out[n - 1] = '\0';
}

int
store_read_meta(store *s, const char *id, store_meta *out)
{
    buf         p;
    buf         data;
    FILE       *f;
    char        chunk[512];
    size_t      n;
    json_arena *a;
    json_value *v;
    int         rc = -1;

    memset(out, 0, sizeof(*out));
    buf_init(&p);
    buf_init(&data);

    store_path(s, &p, id, "meta.json");
    f = fopen(p.data, "rb");
    if (f == NULL)
        goto done;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        buf_append(&data, chunk, n);
    fclose(f);
    if (data.oom)
        goto done;

    a = json_arena_new();
    if (a == NULL)
        goto done;
    v = json_parse(a, data.data, data.len);
    if (v != NULL) {
        meta_str(v, "id",    out->id,    sizeof(out->id));
        meta_str(v, "title", out->title, sizeof(out->title));
        meta_str(v, "model", out->model, sizeof(out->model));
        meta_str(v, "cwd",   out->cwd,   sizeof(out->cwd));
        out->created    = json_as_long(json_get(v, "created"), 0);
        out->nmsgs      = json_as_long(json_get(v, "nmsgs"), 0);
        out->tokens_in  = json_as_long(json_get(v, "tokens_in"), 0);
        out->tokens_out = json_as_long(json_get(v, "tokens_out"), 0);
        /* Trust the directory name over the file's own claim. */
        strncpy(out->id, id, sizeof(out->id) - 1);
        rc = 0;
    }
    json_arena_free(a);

done:
    buf_free(&p);
    buf_free(&data);
    return rc;
}

int
store_list(store *s, store_meta *out, int max)
{
    buf         p;
    plat_dir   *d;
    const char *name;
    int         is_dir;
    int         n = 0;
    int         i;
    int         j;

    buf_init(&p);
    store_path(s, &p, NULL, NULL);
    d = plat_opendir(p.data);
    buf_free(&p);
    if (d == NULL)
        return 0;

    while (n < max && (name = plat_readdir(d, &is_dir)) != NULL) {
        if (!is_dir)
            continue;
        if (store_read_meta(s, name, &out[n]) == 0)
            n++;
    }
    plat_closedir(d);

    /* Ids sort newest-first, but readdir order is arbitrary. Insertion sort:
     * the list is small and this avoids pulling in qsort's comparator. */
    for (i = 1; i < n; i++) {
        store_meta key = out[i];
        j = i - 1;
        while (j >= 0 && strcmp(out[j].id, key.id) > 0) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return n;
}

int
store_delete(store *s, const char *id)
{
    buf       p;
    plat_dir *d;
    const char *name;
    int       rc;

    buf_init(&p);
    store_path(s, &p, id, NULL);
    d = plat_opendir(p.data);
    if (d != NULL) {
        while ((name = plat_readdir(d, NULL)) != NULL) {
            buf f;
            buf_init(&f);
            store_path(s, &f, id, name);
            plat_remove(f.data);
            buf_free(&f);
        }
        plat_closedir(d);
    }
    store_path(s, &p, id, NULL);
    rc = plat_remove(p.data);
    buf_free(&p);
    return rc;
}

/* ---------------------------------------------------------------- append -- */

static const char *
store_role_name(int role)
{
    switch (role) {
    case PROV_ROLE_SYSTEM:    return "system";
    case PROV_ROLE_USER:      return "user";
    case PROV_ROLE_ASSISTANT: return "assistant";
    case PROV_ROLE_TOOL:      return "tool";
    default:                  return "user";
    }
}

static int
store_role_code(const char *s)
{
    if (s == NULL)                      return PROV_ROLE_USER;
    if (strcmp(s, "system") == 0)       return PROV_ROLE_SYSTEM;
    if (strcmp(s, "assistant") == 0)    return PROV_ROLE_ASSISTANT;
    if (strcmp(s, "tool") == 0)         return PROV_ROLE_TOOL;
    return PROV_ROLE_USER;
}

int
store_append(store *s, const char *id, int role,
             const char *content, size_t content_len,
             const prov_toolcall *calls, size_t ncalls,
             const char *tool_call_id)
{
    buf         p;
    buf         doc;
    json_writer w;
    FILE       *f;
    size_t      i;
    int         rc = -1;

    buf_init(&p);
    buf_init(&doc);

    json_w_init(&w, json_buf_sink, &doc);
    json_w_obj_open(&w);
      json_w_key(&w, "role");
      json_w_str(&w, store_role_name(role));
      if (content != NULL) {
          json_w_key(&w, "content");
          json_w_strn(&w, content, content_len);
      }
      if (tool_call_id != NULL && *tool_call_id != '\0') {
          json_w_key(&w, "tool_call_id");
          json_w_str(&w, tool_call_id);
      }
      if (calls != NULL && ncalls > 0) {
          json_w_key(&w, "calls");
          json_w_arr_open(&w);
          for (i = 0; i < ncalls && i < PROV_MAX_TOOLCALLS; i++) {
              json_w_obj_open(&w);
                json_w_key(&w, "id");   json_w_str(&w, calls[i].id);
                json_w_key(&w, "name"); json_w_str(&w, calls[i].name);
                json_w_key(&w, "args"); json_w_str(&w, calls[i].args);
              json_w_obj_close(&w);
          }
          json_w_arr_close(&w);
      }
    json_w_obj_close(&w);
    if (json_w_finish(&w) != 0)
        goto done;

    /* One record per line, so a partial write costs exactly one message. */
    buf_putc(&doc, '\n');
    if (doc.oom)
        goto done;

    store_path(s, &p, id, "msgs.jsonl");
    f = fopen(p.data, "ab");
    if (f == NULL)
        goto done;
    if (fwrite(doc.data, 1, doc.len, f) != doc.len) {
        fclose(f);
        goto done;
    }
    rc = (fclose(f) == 0) ? 0 : -1;

done:
    buf_free(&p);
    buf_free(&doc);
    return rc;
}

/* ------------------------------------------------------------------ read -- */

int
store_iter_open(store_iter *it, store *s, const char *id)
{
    buf p;

    it->fp      = NULL;
    it->arena   = NULL;
    it->ncalls  = 0;
    it->skipped = 0;
    buf_init(&it->line);

    buf_init(&p);
    store_path(s, &p, id, "msgs.jsonl");
    it->fp = (void *)fopen(p.data, "rb");
    buf_free(&p);

    /* A session with no messages yet is legitimate, not an error. */
    return 0;
}

void
store_iter_close(store_iter *it)
{
    if (it->fp != NULL) {
        fclose((FILE *)it->fp);
        it->fp = NULL;
    }
    if (it->arena != NULL) {
        json_arena_free(it->arena);
        it->arena = NULL;
    }
    buf_free(&it->line);
}

void
store_iter_rewind(void *itv)
{
    store_iter *it = (store_iter *)itv;
    if (it->fp != NULL)
        rewind((FILE *)it->fp);
    it->skipped = 0;
}

/* Reads one line, growing as needed. Returns 0 at EOF. */
static int
store_read_line(store_iter *it)
{
    char  chunk[512];
    FILE *f = (FILE *)it->fp;

    buf_clear(&it->line);
    if (f == NULL)
        return 0;

    for (;;) {
        if (fgets(chunk, (int)sizeof(chunk), f) == NULL)
            break;
        buf_puts(&it->line, chunk);
        if (it->line.len > 0 && it->line.data[it->line.len - 1] == '\n') {
            it->line.len--;   /* strip the terminator */
            return 1;
        }
    }
    /* A final line with no newline is a crash-truncated record. */
    return (it->line.len > 0) ? 2 : 0;
}

int
store_iter_next(void *itv, prov_msg *out)
{
    store_iter *it = (store_iter *)itv;
    json_value *v;
    json_value *calls;
    size_t      i;
    int         lr;

    for (;;) {
        if (it->arena != NULL) {
            json_arena_free(it->arena);
            it->arena = NULL;
        }

        lr = store_read_line(it);
        if (lr == 0)
            return 0;
        if (lr == 2) {
            /* Truncated tail: skip it rather than fail the whole session. */
            it->skipped++;
            return 0;
        }
        if (it->line.len == 0)
            continue;
        if (buf_cstr(&it->line) == NULL)
            return 0;

        it->arena = json_arena_new();
        if (it->arena == NULL)
            return 0;

        v = json_parse(it->arena, it->line.data, it->line.len);
        if (v == NULL) {
            it->skipped++;
            continue;
        }

        memset(out, 0, sizeof(*out));
        out->role = store_role_code(json_as_str(json_get(v, "role"), NULL));

        if (json_type_of(json_get(v, "content")) == JSON_STR) {
            json_value *c = json_get(v, "content");
            out->content     = json_as_str(c, "");
            out->content_len = strlen(out->content);
        }
        out->tool_call_id = json_as_str(json_get(v, "tool_call_id"), NULL);

        it->ncalls = 0;
        calls = json_get(v, "calls");
        if (json_type_of(calls) == JSON_ARR) {
            for (i = 0; i < json_len(calls) && i < PROV_MAX_TOOLCALLS; i++) {
                json_value *c = json_at(calls, i);
                it->calls[it->ncalls].id   = json_as_str(json_get(c, "id"), "");
                it->calls[it->ncalls].name = json_as_str(json_get(c, "name"), "");
                it->calls[it->ncalls].args = json_as_str(json_get(c, "args"), "{}");
                it->ncalls++;
            }
        }
        if (it->ncalls > 0) {
            out->calls  = it->calls;
            out->ncalls = (size_t)it->ncalls;
        }
        return 1;
    }
}

prov_msgsrc
store_iter_src(store_iter *it)
{
    prov_msgsrc s;
    s.ctx    = it;
    s.rewind = store_iter_rewind;
    s.next   = store_iter_next;
    return s;
}
