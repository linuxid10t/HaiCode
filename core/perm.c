/* perm.c - permission gate. C89. */

#include "perm.h"
#include "path.h"

#include <string.h>

void
perm_init(perm_gate *g)
{
    g->nrules   = 0;
    g->nsession = 0;
    g->ask      = NULL;
    g->ask_ctx  = NULL;
    g->yolo     = 0;
}

static int
perm_push(perm_rule *list, int *n, const char *action, const char *resource,
          perm_effect e)
{
    if (*n >= PERM_MAX_RULES)
        return -1;
    strncpy(list[*n].action, (action != NULL) ? action : "*",
            sizeof(list[0].action) - 1);
    list[*n].action[sizeof(list[0].action) - 1] = '\0';
    strncpy(list[*n].resource, (resource != NULL) ? resource : "*",
            sizeof(list[0].resource) - 1);
    list[*n].resource[sizeof(list[0].resource) - 1] = '\0';
    list[*n].effect = e;
    (*n)++;
    return 0;
}

int
perm_add(perm_gate *g, const char *action, const char *resource, perm_effect e)
{
    return perm_push(g->rules, &g->nrules, action, resource, e);
}

int
perm_add_session(perm_gate *g, const char *action, const char *resource,
                 perm_effect e)
{
    return perm_push(g->session, &g->nsession, action, resource, e);
}

void
perm_clear_session(perm_gate *g)
{
    g->nsession = 0;
}

void
perm_set_ask(perm_gate *g, perm_ask_fn fn, void *ctx)
{
    g->ask     = fn;
    g->ask_ctx = ctx;
}

/* Last match wins, so a later rule can narrow an earlier one. */
static int
perm_scan(const perm_rule *list, int n, const char *action,
          const char *resource, perm_effect *out)
{
    int i;
    int found = 0;

    for (i = 0; i < n; i++) {
        if (!path_glob(list[i].action, action, 1))
            continue;
        if (!path_glob(list[i].resource,
                       (resource != NULL) ? resource : "", PATH_FOLD_CASE))
            continue;
        *out  = list[i].effect;
        found = 1;
    }
    return found;
}

perm_effect
perm_lookup(const perm_gate *g, const char *action, const char *resource)
{
    perm_effect e = PERM_ASK;

    /* Session rules win outright: they are the user's most recent word. */
    if (perm_scan(g->session, g->nsession, action, resource, &e))
        return e;
    if (perm_scan(g->rules, g->nrules, action, resource, &e))
        return e;
    return PERM_ASK;
}

perm_effect
perm_check(perm_gate *g, const char *action, const char *resource,
           const char *input)
{
    perm_effect e;

    if (g->yolo)
        return PERM_ALLOW;

    e = perm_lookup(g, action, resource);
    if (e != PERM_ASK)
        return e;

    if (g->ask == NULL)
        return PERM_DENY;   /* nobody to ask means no */

    e = g->ask(g->ask_ctx, action, (resource != NULL) ? resource : "",
               (input != NULL) ? input : "");
    return (e == PERM_ALLOW) ? PERM_ALLOW : PERM_DENY;
}
