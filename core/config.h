/* config.h - configuration, merged from two JSON files. C89.
 *
 *   <settings>/config.json      global defaults
 *   <workdir>/.haios2.json      per-project overlay
 *
 * The overlay wins field by field, and any field absent from both keeps the
 * built-in default, so a project file only has to state what differs.
 * Permission rules are the exception: they append rather than replace, with
 * project rules last, which matters because the last matching rule wins.
 */
#ifndef HAIOS2_CONFIG_H
#define HAIOS2_CONFIG_H

#include <stddef.h>
#include "perm.h"

typedef struct config {
    char   host[128];
    int    port;
    char   endpoint[128];     /* "/v1/chat/completions" */
    char   model[64];
    long   max_tokens;
    double temperature;
    int    have_temperature;
    double top_p;
    int    have_top_p;
    long   max_steps;
    long   timeout_s;         /* per-command timeout for the cmd tool */
    char   system_prompt[2048];
} config;

void config_defaults(config *c);

/* Overlays one JSON file onto `c`, appending any permission rules it carries
 * into `g` (which may be NULL). A missing file is not an error; a malformed
 * one is. Returns 0, -1 on parse failure. */
int  config_load_file(config *c, perm_gate *g, const char *path);

/* Loads the global file then the project overlay. Returns the number of files
 * successfully applied, or -1 if one was malformed. */
int  config_load(config *c, perm_gate *g, const char *settings_dir,
                 const char *workdir);

#endif
