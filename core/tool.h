/* tool.h - tool registry and dispatch. C89.
 *
 * A tool is a struct of function pointers rather than a class, which keeps the
 * whole thing compilable by a 1993 C compiler and makes the registry a plain
 * array.
 *
 * tool_execute() is the only entry point, and it does three things no caller
 * should have to remember: resolve the gate resource, consult the permission
 * gate, and guarantee a well-formed tool_result even when the tool misbehaves.
 */
#ifndef HAIOS2_TOOL_H
#define HAIOS2_TOOL_H

#include <stddef.h>
#include "buf.h"
#include "json.h"
#include "perm.h"
#include "provider.h"

#define TOOL_MAX          32
/* Output cap. Smaller than a desktop agent would use: this has to fit
 * alongside a request buffer on a machine with 16 MB. */
#define TOOL_MAX_OUTPUT   65536

typedef struct tool_ctx {
    const char *workdir;
    perm_gate  *gate;
    const char *call_id;
    /* The arguments exactly as the model sent them. Shown in the permission
     * prompt so the user sees what was actually asked for rather than a
     * summary the tool chose. May be NULL. */
    const char *raw_input;
} tool_ctx;

typedef struct tool_result {
    int ok;        /* the tool ran and succeeded */
    int denied;    /* stopped by the permission gate, not by an error */
    buf out;       /* output on success, the reason on failure */
} tool_result;

void tool_result_init(tool_result *r);
void tool_result_free(tool_result *r);

typedef struct tool tool;

struct tool {
    const char *name;
    const char *description;
    const char *schema;        /* pre-encoded JSON Schema object */
    const char *permission;    /* gate action; NULL means use `name` */
    /* Read-only tools skip the gate when their target resolves inside the
     * working directory. Anything that writes, deletes or executes must
     * leave this at 0. */
    int         readonly;
    void (*resource)(const tool *t, json_value *in, tool_ctx *c, buf *out);
    int  (*run)(const tool *t, json_value *in, tool_ctx *c, tool_result *r);
};

typedef struct tool_registry {
    const tool *tools[TOOL_MAX];
    int         n;
} tool_registry;

void        tool_registry_init(tool_registry *reg);
int         tool_register(tool_registry *reg, const tool *t);
/* Registers read, write, edit, ls, grep and cmd. */
int         tool_register_builtins(tool_registry *reg);
const tool *tool_find(const tool_registry *reg, const char *name);
/* Fills `out` with definitions for prov_req.tools. Returns the count. */
size_t      tool_defs(const tool_registry *reg, prov_tool *out, size_t max);

/* Gate, then run. Always fills `r`; never propagates a failure as a crash. */
void tool_execute(const tool_registry *reg, const char *name,
                  json_value *input, tool_ctx *ctx, tool_result *r);

/* Exposed for testing: resolves `rel` against ctx->workdir and reports whether
 * the result stays inside it. */
int tool_resolve(const tool_ctx *ctx, const char *rel, buf *out, int *inside);

#endif
