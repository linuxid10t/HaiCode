/* tool.c - tool registry and dispatch. C89. */

#include "tool.h"
#include "path.h"
#include "perm.h"

#include <string.h>

void
tool_result_init(tool_result *r)
{
    r->ok     = 0;
    r->denied = 0;
    buf_init(&r->out);
}

void
tool_result_free(tool_result *r)
{
    buf_free(&r->out);
}

void
tool_registry_init(tool_registry *reg)
{
    reg->n = 0;
}

int
tool_register(tool_registry *reg, const tool *t)
{
    if (reg->n >= TOOL_MAX || t == NULL || t->name == NULL || t->run == NULL)
        return -1;
    reg->tools[reg->n++] = t;
    return 0;
}

const tool *
tool_find(const tool_registry *reg, const char *name)
{
    int i;

    if (name == NULL)
        return NULL;
    for (i = 0; i < reg->n; i++) {
        if (strcmp(reg->tools[i]->name, name) == 0)
            return reg->tools[i];
    }
    return NULL;
}

size_t
tool_defs(const tool_registry *reg, prov_tool *out, size_t max)
{
    size_t n = 0;
    int    i;

    for (i = 0; i < reg->n && n < max; i++) {
        out[n].name        = reg->tools[i]->name;
        out[n].description = reg->tools[i]->description;
        out[n].schema      = reg->tools[i]->schema;
        n++;
    }
    return n;
}

int
tool_resolve(const tool_ctx *ctx, const char *rel, buf *out, int *inside)
{
    if (inside != NULL)
        *inside = 0;
    if (rel == NULL || *rel == '\0')
        return -1;
    if (path_resolve(out, (ctx != NULL) ? ctx->workdir : NULL, rel) != 0)
        return -1;
    if (inside != NULL && ctx != NULL && ctx->workdir != NULL)
        *inside = path_within(ctx->workdir, out->data);
    return 0;
}

void
tool_execute(const tool_registry *reg, const char *name,
             json_value *input, tool_ctx *ctx, tool_result *r)
{
    const tool *t;
    buf         resource;
    const char *action;
    perm_effect e;

    r->ok     = 0;
    r->denied = 0;
    buf_clear(&r->out);

    t = tool_find(reg, name);
    if (t == NULL) {
        buf_puts(&r->out, "unknown tool: ");
        buf_puts(&r->out, (name != NULL) ? name : "(null)");
        buf_cstr(&r->out);
        return;
    }

    buf_init(&resource);
    if (t->resource != NULL)
        t->resource(t, input, ctx, &resource);
    else if (ctx != NULL && ctx->workdir != NULL)
        buf_puts(&resource, ctx->workdir);
    buf_cstr(&resource);

    /*
     * Read-only tools inside the working directory run unprompted. That
     * exemption is only sound because path_within() resolves ".." first --
     * a prefix compare here would let "proj/../../etc/passwd" through.
     */
    if (t->readonly && ctx != NULL && ctx->workdir != NULL &&
        resource.data != NULL && path_within(ctx->workdir, resource.data)) {
        buf_free(&resource);
        if (t->run(t, input, ctx, r) != 0 && r->out.len == 0) {
            buf_puts(&r->out, "tool failed");
            buf_cstr(&r->out);
        }
        return;
    }

    action = (t->permission != NULL) ? t->permission : t->name;
    if (ctx != NULL && ctx->gate != NULL) {
        e = perm_check(ctx->gate, action,
                       (resource.data != NULL) ? resource.data : "",
                       ctx->raw_input);
        if (e != PERM_ALLOW) {
            r->denied = 1;
            buf_puts(&r->out, "denied by permission gate: ");
            buf_puts(&r->out, action);
            if (resource.data != NULL && *resource.data != '\0') {
                buf_puts(&r->out, " ");
                buf_puts(&r->out, resource.data);
            }
            buf_cstr(&r->out);
            buf_free(&resource);
            return;
        }
    }
    buf_free(&resource);

    if (t->run(t, input, ctx, r) != 0 && r->out.len == 0) {
        buf_puts(&r->out, "tool failed");
        buf_cstr(&r->out);
    }
}
