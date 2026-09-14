/* config.c - configuration loading. C89. */

#include "config.h"
#include "json.h"
#include "buf.h"
#include "perm.h"

#include <stdio.h>
#include <string.h>

void
config_defaults(config *c)
{
    memset(c, 0, sizeof(*c));
    strcpy(c->host, "127.0.0.1");
    c->port = 8080;                       /* llama.cpp's default */
    strcpy(c->endpoint, "/v1/chat/completions");
    strcpy(c->model, "local");
    c->max_tokens = 2048;
    c->max_steps  = 24;
    c->timeout_s  = 30;
    strcpy(c->system_prompt,
           "You are a coding assistant running natively on OS/2. "
           "Use the provided tools to inspect and modify files. "
           "Paths may use either / or \\ and may carry a drive letter. "
           "Prefer small, verifiable edits, and read a file before editing it.");
}

static void
cfg_str(json_value *v, const char *key, char *out, size_t n)
{
    json_value *f = json_get(v, key);
    if (json_type_of(f) != JSON_STR)
        return;
    strncpy(out, json_as_str(f, ""), n - 1);
    out[n - 1] = '\0';
}

static void
cfg_long(json_value *v, const char *key, long *out)
{
    json_value *f = json_get(v, key);
    if (json_type_of(f) == JSON_NUM)
        *out = json_as_long(f, *out);
}

static void
cfg_dbl(json_value *v, const char *key, double *out, int *have)
{
    json_value *f = json_get(v, key);
    if (json_type_of(f) == JSON_NUM) {
        *out  = json_as_num(f, *out);
        *have = 1;
    }
}

static perm_effect
cfg_effect(const char *s)
{
    if (s == NULL)                  return PERM_ASK;
    if (strcmp(s, "allow") == 0)    return PERM_ALLOW;
    if (strcmp(s, "deny") == 0)     return PERM_DENY;
    return PERM_ASK;
}

int
config_load_file(config *c, perm_gate *g, const char *path)
{
    FILE       *f;
    buf         data;
    char        chunk[512];
    size_t      n;
    json_arena *a;
    json_value *v;
    json_value *srv;
    json_value *perms;
    long        port;
    int         rc = 0;

    f = fopen(path, "rb");
    if (f == NULL)
        return 0;            /* absent is fine */

    buf_init(&data);
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        buf_append(&data, chunk, n);
    fclose(f);

    if (data.oom) { buf_free(&data); return -1; }
    if (data.len == 0) { buf_free(&data); return 0; }

    a = json_arena_new();
    if (a == NULL) { buf_free(&data); return -1; }

    v = json_parse(a, data.data, data.len);
    if (v == NULL || json_type_of(v) != JSON_OBJ) {
        json_arena_free(a);
        buf_free(&data);
        return -1;
    }

    srv = json_get(v, "server");
    if (srv != NULL) {
        cfg_str(srv, "host", c->host, sizeof(c->host));
        cfg_str(srv, "endpoint", c->endpoint, sizeof(c->endpoint));
        port = c->port;
        cfg_long(srv, "port", &port);
        if (port > 0 && port < 65536)
            c->port = (int)port;
    }

    cfg_str(v, "model", c->model, sizeof(c->model));
    cfg_str(v, "system_prompt", c->system_prompt, sizeof(c->system_prompt));
    cfg_long(v, "max_tokens", &c->max_tokens);
    cfg_long(v, "max_steps", &c->max_steps);
    cfg_long(v, "timeout", &c->timeout_s);
    cfg_dbl(v, "temperature", &c->temperature, &c->have_temperature);
    cfg_dbl(v, "top_p", &c->top_p, &c->have_top_p);

    /* Rules append rather than replace: the last match wins, so a project file
     * can narrow a global rule only if its own rules come afterwards. */
    perms = json_get(v, "permissions");
    if (g != NULL && json_type_of(perms) == JSON_ARR) {
        size_t i;
        for (i = 0; i < json_len(perms); i++) {
            json_value *p = json_at(perms, i);
            const char *action   = json_as_str(json_get(p, "action"), NULL);
            const char *resource = json_as_str(json_get(p, "resource"), "*");
            const char *effect   = json_as_str(json_get(p, "effect"), NULL);
            if (action == NULL || effect == NULL)
                continue;
            perm_add(g, action, resource, cfg_effect(effect));
        }
    }

    json_arena_free(a);
    buf_free(&data);
    return (rc == 0) ? 1 : rc;
}

int
config_load(config *c, perm_gate *g, const char *settings_dir,
            const char *workdir)
{
    buf p;
    int applied = 0;
    int rc;

    buf_init(&p);

    if (settings_dir != NULL && *settings_dir != '\0') {
        buf_clear(&p);
        buf_puts(&p, settings_dir);
        buf_puts(&p, "/config.json");
        if (buf_cstr(&p) != NULL) {
            rc = config_load_file(c, g, p.data);
            if (rc < 0) { buf_free(&p); return -1; }
            applied += rc;
        }
    }

    if (workdir != NULL && *workdir != '\0') {
        buf_clear(&p);
        buf_puts(&p, workdir);
        buf_puts(&p, "/.haios2.json");
        if (buf_cstr(&p) != NULL) {
            rc = config_load_file(c, g, p.data);
            if (rc < 0) { buf_free(&p); return -1; }
            applied += rc;
        }
    }

    buf_free(&p);
    return applied;
}
