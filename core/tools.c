/* tools.c - the six built-in tools. C89.
 *
 * read / ls / grep are read-only and run unprompted inside the working
 * directory. write / edit / cmd always face the permission gate.
 *
 * Deliberately NOT shelling out for ls and grep. A stock OS/2 install has no
 * grep(1) or find(1), and depending on ported GNU utilities would drag a whole
 * package stack onto the target for something a directory walk does in 200
 * lines.
 */

#include "tool.h"
#include "plat.h"
#include "path.h"
#include "buf.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BINARY_SNIFF 8192

static void
fail(tool_result *r, const char *msg, const char *detail)
{
    r->ok = 0;
    buf_clear(&r->out);
    buf_puts(&r->out, msg);
    if (detail != NULL) {
        buf_puts(&r->out, ": ");
        buf_puts(&r->out, detail);
    }
    buf_cstr(&r->out);
}

static const char *
arg_str(json_value *in, const char *key, const char *dflt)
{
    return json_as_str(json_get(in, key), dflt);
}

/* Resource helper shared by every path-taking tool. */
static void
res_path(const tool *t, json_value *in, tool_ctx *c, buf *out)
{
    const char *p = arg_str(in, "path", NULL);
    (void)t;
    buf_clear(out);
    if (p == NULL) {
        if (c != NULL && c->workdir != NULL)
            buf_puts(out, c->workdir);
        return;
    }
    if (tool_resolve(c, p, out, NULL) != 0) {
        buf_clear(out);
        buf_puts(out, p);   /* unresolvable: hand the gate the raw string */
    }
}

/* Reads a whole file, refusing binaries and enforcing the output cap. */
static int
slurp(const char *path, buf *out, long cap, const char **err)
{
    FILE  *f;
    char   chunk[1024];
    size_t n;
    long   total = 0;
    int    checked = 0;

    f = fopen(path, "rb");
    if (f == NULL) {
        *err = "cannot open";
        return -1;
    }
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (!checked) {
            size_t i;
            for (i = 0; i < n; i++) {
                if (chunk[i] == '\0') {
                    fclose(f);
                    *err = "file is binary";
                    return -1;
                }
            }
            if (total + (long)n >= BINARY_SNIFF)
                checked = 1;
        }
        if (cap > 0 && total + (long)n > cap) {
            buf_append(out, chunk, (size_t)(cap - total));
            buf_puts(out, "\n[truncated]");
            fclose(f);
            return 0;
        }
        buf_append(out, chunk, n);
        total += (long)n;
    }
    fclose(f);
    if (out->oom) {
        *err = "out of memory";
        return -1;
    }
    return 0;
}

static int
atomic_write(const char *path, const char *data, size_t len, const char **err)
{
    buf   tmp;
    FILE *f;
    int   rc = -1;

    buf_init(&tmp);
    buf_puts(&tmp, path);
    buf_puts(&tmp, ".tmpw");
    if (buf_cstr(&tmp) == NULL) { *err = "out of memory"; goto done; }

    f = fopen(tmp.data, "wb");
    if (f == NULL) { *err = "cannot create temporary file"; goto done; }
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        plat_remove(tmp.data);
        *err = "write failed";
        goto done;
    }
    if (fclose(f) != 0) {
        plat_remove(tmp.data);
        *err = "close failed";
        goto done;
    }
    /* rename() will not replace an existing file on OS/2. */
    plat_remove(path);
    if (plat_rename(tmp.data, path) != 0) {
        plat_remove(tmp.data);
        *err = "rename failed";
        goto done;
    }
    rc = 0;

done:
    buf_free(&tmp);
    return rc;
}

/* Creates the parent directory chain of a file path. */
static void
make_parent(const char *path)
{
    buf    p;
    size_t i;

    buf_init(&p);
    buf_puts(&p, path);
    if (buf_cstr(&p) == NULL) { buf_free(&p); return; }
    for (i = p.len; i > 0; i--) {
        if (p.data[i - 1] == '/') {
            p.data[i - 1] = '\0';
            plat_mkdir_p(p.data);
            break;
        }
    }
    buf_free(&p);
}

/* ================================================================= read == */

static int
t_read_run(const tool *t, json_value *in, tool_ctx *c, tool_result *r)
{
    buf         full;
    buf         body;
    const char *err = NULL;
    const char *rel = arg_str(in, "path", NULL);
    long        offset = json_as_long(json_get(in, "offset"), 1);
    long        limit  = json_as_long(json_get(in, "limit"), 0);
    int         rc = -1;

    (void)t;
    if (rel == NULL) { fail(r, "read: 'path' is required", NULL); return -1; }

    buf_init(&full);
    buf_init(&body);

    if (tool_resolve(c, rel, &full, NULL) != 0) {
        fail(r, "read: bad path", rel);
        goto done;
    }
    if (slurp(full.data, &body, TOOL_MAX_OUTPUT, &err) != 0) {
        fail(r, "read", err);
        goto done;
    }

    buf_clear(&r->out);
    if (offset <= 1 && limit <= 0) {
        buf_append(&r->out, body.data, body.len);
    } else {
        /* offset is 1-based, matching how the model talks about line numbers. */
        const char *p   = body.data;
        const char *end = body.data + body.len;
        long        line = 1;
        long        emitted = 0;

        if (offset < 1) offset = 1;
        while (p < end && line < offset) {
            if (*p == '\n') line++;
            p++;
        }
        while (p < end) {
            const char *nl = p;
            while (nl < end && *nl != '\n') nl++;
            buf_append(&r->out, p, (size_t)(nl - p));
            buf_putc(&r->out, '\n');
            p = (nl < end) ? nl + 1 : end;
            if (limit > 0 && ++emitted >= limit)
                break;
        }
    }
    buf_cstr(&r->out);
    r->ok = 1;
    rc = 0;

done:
    buf_free(&full);
    buf_free(&body);
    return rc;
}

static const tool t_read = {
    "read", "Read a text file. Returns its contents.",
    "{\"type\":\"object\",\"properties\":{"
      "\"path\":{\"type\":\"string\",\"description\":\"File to read\"},"
      "\"offset\":{\"type\":\"integer\",\"description\":\"First line, 1-based\"},"
      "\"limit\":{\"type\":\"integer\",\"description\":\"Maximum lines\"}},"
    "\"required\":[\"path\"]}",
    "read", 1, res_path, t_read_run
};

/* ================================================================ write == */

static int
t_write_run(const tool *t, json_value *in, tool_ctx *c, tool_result *r)
{
    buf         full;
    const char *err = NULL;
    const char *rel = arg_str(in, "path", NULL);
    json_value *cv  = json_get(in, "content");
    const char *content;
    int         rc = -1;

    (void)t;
    if (rel == NULL) { fail(r, "write: 'path' is required", NULL); return -1; }
    if (json_type_of(cv) != JSON_STR) {
        fail(r, "write: 'content' must be a string", NULL);
        return -1;
    }
    content = json_as_str(cv, "");

    buf_init(&full);
    if (tool_resolve(c, rel, &full, NULL) != 0) {
        fail(r, "write: bad path", rel);
        goto done;
    }
    make_parent(full.data);
    if (atomic_write(full.data, content, strlen(content), &err) != 0) {
        fail(r, "write", err);
        goto done;
    }

    buf_clear(&r->out);
    buf_puts(&r->out, "wrote ");
    buf_puts(&r->out, full.data);
    buf_cstr(&r->out);
    r->ok = 1;
    rc = 0;

done:
    buf_free(&full);
    return rc;
}

static const tool t_write = {
    "write", "Create or overwrite a file with the given contents.",
    "{\"type\":\"object\",\"properties\":{"
      "\"path\":{\"type\":\"string\"},"
      "\"content\":{\"type\":\"string\"}},"
    "\"required\":[\"path\",\"content\"]}",
    "write", 0, res_path, t_write_run
};

/* ================================================================= edit == */

static int
t_edit_run(const tool *t, json_value *in, tool_ctx *c, tool_result *r)
{
    buf         full;
    buf         body;
    buf         outp;
    const char *err = NULL;
    const char *rel = arg_str(in, "path", NULL);
    const char *old = arg_str(in, "old", NULL);
    const char *nw  = arg_str(in, "new", "");
    int         all = json_as_bool(json_get(in, "replace_all"), 0);
    size_t      oldlen;
    const char *p;
    long        hits = 0;
    int         rc = -1;

    (void)t;
    if (rel == NULL) { fail(r, "edit: 'path' is required", NULL); return -1; }
    if (old == NULL || *old == '\0') {
        fail(r, "edit: 'old' must be a non-empty string", NULL);
        return -1;
    }
    oldlen = strlen(old);

    buf_init(&full);
    buf_init(&body);
    buf_init(&outp);

    if (tool_resolve(c, rel, &full, NULL) != 0) {
        fail(r, "edit: bad path", rel);
        goto done;
    }
    if (slurp(full.data, &body, TOOL_MAX_OUTPUT, &err) != 0) {
        fail(r, "edit", err);
        goto done;
    }
    if (buf_cstr(&body) == NULL) { fail(r, "edit", "out of memory"); goto done; }

    for (p = body.data; (p = strstr(p, old)) != NULL; p += oldlen)
        hits++;

    if (hits == 0) {
        fail(r, "edit: 'old' string not found in", full.data);
        goto done;
    }
    /* Ambiguity is an error, not a coin flip: silently editing the first of
     * several identical matches is how an agent corrupts a file. */
    if (hits > 1 && !all) {
        char n[32];
        /* sprintf here only ever formats a number; anything longer is built
         * with buf_puts, which cannot overrun. */
        sprintf(n, "%ld", hits);
        buf_clear(&r->out);
        buf_puts(&r->out, "edit: ambiguous, ");
        buf_puts(&r->out, n);
        buf_puts(&r->out, " occurrences of 'old'. Pass replace_all, or "
                          "include surrounding lines to make it unique.");
        buf_cstr(&r->out);
        r->ok = 0;
        goto done;
    }

    p = body.data;
    for (;;) {
        const char *hit = strstr(p, old);
        if (hit == NULL) {
            buf_puts(&outp, p);
            break;
        }
        buf_append(&outp, p, (size_t)(hit - p));
        buf_puts(&outp, nw);
        p = hit + oldlen;
        if (!all) {
            buf_puts(&outp, p);
            break;
        }
    }
    if (outp.oom) { fail(r, "edit", "out of memory"); goto done; }

    if (atomic_write(full.data, outp.data, outp.len, &err) != 0) {
        fail(r, "edit", err);
        goto done;
    }

    buf_clear(&r->out);
    {
        char n[32];
        sprintf(n, "%ld", hits);
        buf_puts(&r->out, "replaced ");
        buf_puts(&r->out, n);
        buf_puts(&r->out, (hits == 1) ? " occurrence in " : " occurrences in ");
    }
    buf_puts(&r->out, full.data);
    buf_cstr(&r->out);
    r->ok = 1;
    rc = 0;

done:
    buf_free(&full);
    buf_free(&body);
    buf_free(&outp);
    return rc;
}

static const tool t_edit = {
    "edit", "Replace an exact string in a file. Fails if the string is "
            "ambiguous unless replace_all is set.",
    "{\"type\":\"object\",\"properties\":{"
      "\"path\":{\"type\":\"string\"},"
      "\"old\":{\"type\":\"string\",\"description\":\"Exact text to replace\"},"
      "\"new\":{\"type\":\"string\",\"description\":\"Replacement text\"},"
      "\"replace_all\":{\"type\":\"boolean\"}},"
    "\"required\":[\"path\",\"old\",\"new\"]}",
    "write", 0, res_path, t_edit_run
};

/* =================================================================== ls == */

static int
name_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int
t_ls_run(const tool *t, json_value *in, tool_ctx *c, tool_result *r)
{
    buf         full;
    plat_dir   *d;
    const char *rel = arg_str(in, "path", NULL);
    char      **names = NULL;
    int         n = 0;
    int         cap = 0;
    int         i;
    int         rc = -1;

    (void)t;
    buf_init(&full);

    if (rel != NULL) {
        if (tool_resolve(c, rel, &full, NULL) != 0) {
            fail(r, "ls: bad path", rel);
            goto done;
        }
    } else {
        buf_puts(&full, (c != NULL && c->workdir != NULL) ? c->workdir : ".");
        buf_cstr(&full);
    }

    d = plat_opendir(full.data);
    if (d == NULL) {
        fail(r, "ls: cannot open directory", full.data);
        goto done;
    }

    for (;;) {
        int         is_dir = 0;
        const char *nm = plat_readdir(d, &is_dir);
        char       *copy;
        size_t      len;

        if (nm == NULL)
            break;
        if (n >= cap) {
            char **grown;
            cap = (cap == 0) ? 32 : cap * 2;
            grown = (char **)realloc(names, (size_t)cap * sizeof(char *));
            if (grown == NULL) break;
            names = grown;
        }
        len  = strlen(nm);
        copy = (char *)malloc(len + 2);
        if (copy == NULL) break;
        memcpy(copy, nm, len);
        if (is_dir) { copy[len] = '/'; copy[len + 1] = '\0'; }
        else        { copy[len] = '\0'; }
        names[n++] = copy;
    }
    plat_closedir(d);

    if (n > 1)
        qsort(names, (size_t)n, sizeof(char *), name_cmp);

    buf_clear(&r->out);
    for (i = 0; i < n; i++) {
        if (r->out.len < TOOL_MAX_OUTPUT) {
            buf_puts(&r->out, names[i]);
            buf_putc(&r->out, '\n');
        }
        free(names[i]);
    }
    free(names);
    if (n == 0)
        buf_puts(&r->out, "(empty)\n");
    buf_cstr(&r->out);
    r->ok = 1;
    rc = 0;

done:
    buf_free(&full);
    return rc;
}

static const tool t_ls = {
    "ls", "List a directory. Directories are suffixed with /.",
    "{\"type\":\"object\",\"properties\":{"
      "\"path\":{\"type\":\"string\",\"description\":"
      "\"Directory; defaults to the working directory\"}}}",
    "read", 1, res_path, t_ls_run
};

/* ================================================================= grep == */

typedef struct grep_state {
    const char *needle;
    const char *include;
    int         fold;
    long        hits;
    long        maxhits;
    buf        *out;
} grep_state;

static const char *
str_find_fold(const char *hay, const char *needle, int fold)
{
    size_t nl;

    if (!fold)
        return strstr(hay, needle);

    nl = strlen(needle);
    if (nl == 0)
        return hay;
    for (; *hay != '\0'; hay++) {
        size_t i;
        for (i = 0; i < nl; i++) {
            char a = hay[i];
            char b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b || hay[i] == '\0')
                break;
        }
        if (i == nl)
            return hay;
    }
    return NULL;
}

static void
grep_file(grep_state *g, const char *full, const char *shown)
{
    FILE *f;
    char  line[1024];
    long  lineno = 0;

    f = fopen(full, "rb");
    if (f == NULL)
        return;
    while (fgets(line, (int)sizeof(line), f) != NULL) {
        size_t len = strlen(line);
        lineno++;
        if (g->hits >= g->maxhits)
            break;
        if (len > 0 && line[len - 1] == '\n')
            line[--len] = '\0';
        if (memchr(line, '\0', len) != NULL)
            continue;                       /* binary-ish line */
        if (str_find_fold(line, g->needle, g->fold) == NULL)
            continue;
        if (g->out->len < TOOL_MAX_OUTPUT) {
            char n[32];
            sprintf(n, "%ld", lineno);
            buf_puts(g->out, shown);
            buf_putc(g->out, ':');
            buf_puts(g->out, n);
            buf_putc(g->out, ':');
            buf_puts(g->out, line);
            buf_putc(g->out, '\n');
        }
        g->hits++;
    }
    fclose(f);
}

static void
grep_walk(grep_state *g, const char *dir, const char *prefix, int depth)
{
    plat_dir   *d;
    const char *nm;
    int         is_dir;

    if (depth > 16 || g->hits >= g->maxhits)
        return;
    d = plat_opendir(dir);
    if (d == NULL)
        return;

    while ((nm = plat_readdir(d, &is_dir)) != NULL && g->hits < g->maxhits) {
        buf  full;
        buf  shown;
        char namecopy[512];

        strncpy(namecopy, nm, sizeof(namecopy) - 1);
        namecopy[sizeof(namecopy) - 1] = '\0';

        buf_init(&full);
        buf_puts(&full, dir);
        buf_putc(&full, '/');
        buf_puts(&full, namecopy);
        buf_cstr(&full);

        buf_init(&shown);
        if (prefix != NULL && *prefix != '\0') {
            buf_puts(&shown, prefix);
            buf_putc(&shown, '/');
        }
        buf_puts(&shown, namecopy);
        buf_cstr(&shown);

        if (is_dir) {
            if (namecopy[0] != '.')
                grep_walk(g, full.data, shown.data, depth + 1);
        } else if (g->include == NULL ||
                   path_glob(g->include, namecopy, PATH_FOLD_CASE)) {
            grep_file(g, full.data, shown.data);
        }

        buf_free(&full);
        buf_free(&shown);
    }
    plat_closedir(d);
}

static int
t_grep_run(const tool *t, json_value *in, tool_ctx *c, tool_result *r)
{
    buf         full;
    grep_state  g;
    const char *pattern = arg_str(in, "pattern", NULL);
    const char *rel     = arg_str(in, "path", NULL);
    int         rc = -1;

    (void)t;
    if (pattern == NULL || *pattern == '\0') {
        fail(r, "grep: 'pattern' is required", NULL);
        return -1;
    }

    buf_init(&full);
    if (rel != NULL) {
        if (tool_resolve(c, rel, &full, NULL) != 0) {
            fail(r, "grep: bad path", rel);
            goto done;
        }
    } else {
        buf_puts(&full, (c != NULL && c->workdir != NULL) ? c->workdir : ".");
        buf_cstr(&full);
    }

    buf_clear(&r->out);
    g.needle  = pattern;
    g.include = arg_str(in, "include", NULL);
    g.fold    = json_as_bool(json_get(in, "ignore_case"), 0);
    g.hits    = 0;
    g.maxhits = json_as_long(json_get(in, "max_results"), 200);
    if (g.maxhits <= 0 || g.maxhits > 2000)
        g.maxhits = 200;
    g.out     = &r->out;

    grep_walk(&g, full.data, "", 0);

    if (g.hits == 0)
        buf_puts(&r->out, "(no matches)\n");
    else if (g.hits >= g.maxhits)
        buf_puts(&r->out, "[truncated]\n");
    buf_cstr(&r->out);
    r->ok = 1;
    rc = 0;

done:
    buf_free(&full);
    return rc;
}

static const tool t_grep = {
    "grep", "Search files under a directory for a literal substring. "
            "Returns path:line:text.",
    "{\"type\":\"object\",\"properties\":{"
      "\"pattern\":{\"type\":\"string\",\"description\":\"Literal substring\"},"
      "\"path\":{\"type\":\"string\"},"
      "\"include\":{\"type\":\"string\",\"description\":"
      "\"Filename glob, e.g. *.c\"},"
      "\"ignore_case\":{\"type\":\"boolean\"},"
      "\"max_results\":{\"type\":\"integer\"}},"
    "\"required\":[\"pattern\"]}",
    "read", 1, res_path, t_grep_run
};

/* ================================================================== cmd == */

static void
res_cmd(const tool *t, json_value *in, tool_ctx *c, buf *out)
{
    (void)t; (void)c;
    buf_clear(out);
    buf_puts(out, arg_str(in, "command", ""));
    buf_cstr(out);
}

static int
t_cmd_run(const tool *t, json_value *in, tool_ctx *c, tool_result *r)
{
    const char *cmd = arg_str(in, "command", NULL);
    long        timeout = json_as_long(json_get(in, "timeout"), 30);
    int         status;

    (void)t;
    if (cmd == NULL || *cmd == '\0') {
        fail(r, "cmd: 'command' is required", NULL);
        return -1;
    }
    if (timeout <= 0 || timeout > 300)
        timeout = 30;

    buf_clear(&r->out);
    status = plat_run(cmd, (c != NULL) ? c->workdir : NULL, timeout,
                      &r->out, TOOL_MAX_OUTPUT);
    if (status < 0) {
        fail(r, "cmd: could not start", cmd);
        return -1;
    }
    if (status != 0) {
        char n[32];
        sprintf(n, "%d", status);
        buf_puts(&r->out, "\n[exit status ");
        buf_puts(&r->out, n);
        buf_puts(&r->out, "]");
        /* A non-zero exit is reported to the model as a failed result, not as
         * a tool error: the output is still the useful part. */
        r->ok = 0;
    } else {
        r->ok = 1;
    }
    if (r->out.len == 0)
        buf_puts(&r->out, "(no output)");
    buf_cstr(&r->out);
    return 0;
}

static const tool t_cmd = {
    "cmd", "Run a shell command in the working directory and return its "
           "combined output.",
    "{\"type\":\"object\",\"properties\":{"
      "\"command\":{\"type\":\"string\"},"
      "\"timeout\":{\"type\":\"integer\",\"description\":\"Seconds\"}},"
    "\"required\":[\"command\"]}",
    "cmd", 0, res_cmd, t_cmd_run
};

/* ============================================================== registry == */

int
tool_register_builtins(tool_registry *reg)
{
    if (tool_register(reg, &t_read)  != 0) return -1;
    if (tool_register(reg, &t_write) != 0) return -1;
    if (tool_register(reg, &t_edit)  != 0) return -1;
    if (tool_register(reg, &t_ls)    != 0) return -1;
    if (tool_register(reg, &t_grep)  != 0) return -1;
    if (tool_register(reg, &t_cmd)   != 0) return -1;
    return 0;
}
