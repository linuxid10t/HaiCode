/* test_config.c - defaults, file overlay and permission-rule ordering. */

#include "../config.h"
#include "../perm.h"
#include "../plat.h"
#include "../buf.h"
#include "tap.h"

#include <stdio.h>
#include <string.h>

static char g_dir[512];
static char g_work[512];

static void
write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f != NULL) { fputs(text, f); fclose(f); }
}

static void
test_defaults(void)
{
    config c;

    config_defaults(&c);
    EQSTR(c.host, "127.0.0.1", "default host");
    EQLONG(c.port, 8080, "default port is llama.cpp's");
    EQSTR(c.endpoint, "/v1/chat/completions", "default endpoint");
    EQLONG(c.max_steps, 24, "default step budget");
    OK(c.have_temperature == 0, "temperature unset by default");
    OK(c.system_prompt[0] != '\0', "a system prompt is present");
}

static void
test_overlay(void)
{
    config    c;
    perm_gate g;
    buf       p;

    buf_init(&p);
    buf_puts(&p, g_dir);
    buf_puts(&p, "/config.json");
    write_file(buf_cstr(&p),
               "{\"server\":{\"host\":\"10.0.0.5\",\"port\":11434},"
               "\"model\":\"qwen3\",\"max_tokens\":4096,"
               "\"temperature\":0.2,"
               "\"permissions\":[{\"action\":\"read\",\"resource\":\"*\","
               "\"effect\":\"allow\"}]}");

    buf_clear(&p);
    buf_puts(&p, g_work);
    buf_puts(&p, "/.haios2.json");
    write_file(buf_cstr(&p),
               "{\"model\":\"glm\",\"max_steps\":40,"
               "\"permissions\":[{\"action\":\"read\","
               "\"resource\":\"*/.ssh/*\",\"effect\":\"deny\"}]}");
    buf_free(&p);

    config_defaults(&c);
    perm_init(&g);
    EQLONG(config_load(&c, &g, g_dir, g_work), 2, "both files applied");

    EQSTR(c.host, "10.0.0.5", "global host applied");
    EQLONG(c.port, 11434, "global port applied");
    EQSTR(c.model, "glm", "project file overrides the global model");
    EQLONG(c.max_tokens, 4096, "global value kept where the project is silent");
    EQLONG(c.max_steps, 40, "project value applied");
    OK(c.have_temperature == 1, "temperature now set");
    OK(c.temperature == 0.2, "with the configured value");
    EQSTR(c.endpoint, "/v1/chat/completions",
          "untouched field keeps its default");

    /* Rules append, project last, and the last match wins -- so the project
     * file can carve an exception out of a global allow. */
    EQLONG(g.nrules, 2, "rules from both files appended");
    EQLONG(perm_lookup(&g, "read", "/home/u/notes.txt"), PERM_ALLOW,
           "global allow still applies");
    EQLONG(perm_lookup(&g, "read", "/home/u/.ssh/id_rsa"), PERM_DENY,
           "project deny overrides it for the paths it names");
}

static void
test_missing_and_malformed(void)
{
    config    c;
    perm_gate g;
    buf       p;

    config_defaults(&c);
    perm_init(&g);
    EQLONG(config_load_file(&c, &g, "/nonexistent/nope.json"), 0,
           "a missing file is not an error");
    EQSTR(c.model, "local", "and changes nothing");

    buf_init(&p);
    buf_puts(&p, g_dir);
    buf_puts(&p, "/bad.json");
    write_file(buf_cstr(&p), "{ this is not json");
    EQLONG(config_load_file(&c, &g, buf_cstr(&p)), -1,
           "a malformed file IS an error");

    buf_clear(&p);
    buf_puts(&p, g_dir);
    buf_puts(&p, "/empty.json");
    write_file(buf_cstr(&p), "");
    EQLONG(config_load_file(&c, &g, buf_cstr(&p)), 0,
           "an empty file is tolerated");
    buf_free(&p);
}

static void
test_bad_values_ignored(void)
{
    config    c;
    perm_gate g;
    buf       p;

    buf_init(&p);
    buf_puts(&p, g_dir);
    buf_puts(&p, "/types.json");
    /* Wrong types must be ignored rather than coerced: a port of "eighty"
     * silently becoming 0 would be worse than keeping the default. */
    write_file(buf_cstr(&p),
               "{\"server\":{\"port\":\"eighty\",\"host\":42},"
               "\"max_steps\":\"lots\",\"model\":null}");

    config_defaults(&c);
    perm_init(&g);
    config_load_file(&c, &g, buf_cstr(&p));

    EQLONG(c.port, 8080, "non-numeric port ignored");
    EQSTR(c.host, "127.0.0.1", "non-string host ignored");
    EQLONG(c.max_steps, 24, "non-numeric max_steps ignored");
    EQSTR(c.model, "local", "null model ignored");
    buf_free(&p);

    buf_init(&p);
    buf_puts(&p, g_dir);
    buf_puts(&p, "/port.json");
    write_file(buf_cstr(&p), "{\"server\":{\"port\":99999}}");
    config_defaults(&c);
    config_load_file(&c, &g, buf_cstr(&p));
    EQLONG(c.port, 8080, "out-of-range port ignored");
    buf_free(&p);
}

int
main(void)
{
    buf cmd, out;

    sprintf(g_dir,  "/tmp/haios2_cfg_%ld", (long)plat_time());
    sprintf(g_work, "%.400s/work", g_dir);
    plat_mkdir_p(g_work);

    test_defaults();
    test_overlay();
    test_missing_and_malformed();
    test_bad_values_ignored();

    buf_init(&cmd); buf_init(&out);
    buf_puts(&cmd, "rm -rf ");
    buf_puts(&cmd, g_dir);
    plat_run(buf_cstr(&cmd), NULL, 10, &out, 1024);
    buf_free(&cmd); buf_free(&out);

    TAP_REPORT("test_config");
}
