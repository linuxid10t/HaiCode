/* perm.h - permission gate. C89.
 *
 * A rule is (action-glob, resource-glob) -> effect. Two lists are consulted:
 * session rules, set by the user answering a prompt with "always", then config
 * rules. Within a list the LAST match wins, so a config file can narrow a
 * broad earlier rule by appending to it.
 *
 * Unmatched means PERM_ASK, and asking is what makes this safe by default:
 * a tool nobody has written a rule for stops and waits for a human.
 */
#ifndef HAIOS2_PERM_H
#define HAIOS2_PERM_H

#include <stddef.h>

#define PERM_MAX_RULES 64

typedef enum {
    PERM_ASK = 0,
    PERM_ALLOW,
    PERM_DENY
} perm_effect;

typedef struct perm_rule {
    char        action[32];
    char        resource[256];
    perm_effect effect;
} perm_rule;

/* Returns the user's decision. `input` is the raw tool arguments, for display.
 * Blocking here blocks the engine; the UI layer is responsible for pumping
 * its own event loop while this call is outstanding. */
typedef perm_effect (*perm_ask_fn)(void *ctx, const char *action,
                                   const char *resource, const char *input);

typedef struct perm_gate {
    perm_rule    rules[PERM_MAX_RULES];
    int          nrules;
    perm_rule    session[PERM_MAX_RULES];
    int          nsession;
    perm_ask_fn  ask;
    void        *ask_ctx;
    /* When set, every check is allowed without prompting. Off by default and
     * only ever set from an explicit user action. */
    int          yolo;
} perm_gate;

void perm_init(perm_gate *g);
int  perm_add(perm_gate *g, const char *action, const char *resource,
              perm_effect e);
int  perm_add_session(perm_gate *g, const char *action, const char *resource,
                      perm_effect e);
void perm_clear_session(perm_gate *g);
void perm_set_ask(perm_gate *g, perm_ask_fn fn, void *ctx);

/* Consults the rules only; never prompts. */
perm_effect perm_lookup(const perm_gate *g, const char *action,
                        const char *resource);

/* Full decision: rules, then the ask callback if the rules are silent.
 * Returns PERM_ALLOW or PERM_DENY; never PERM_ASK. With no ask callback
 * installed, an unmatched check is DENIED rather than allowed. */
perm_effect perm_check(perm_gate *g, const char *action, const char *resource,
                       const char *input);

#endif
