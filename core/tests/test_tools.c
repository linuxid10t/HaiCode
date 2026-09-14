/* test_tools.c - permission gate, tool dispatch and the six built-ins. */

#include "../tool.h"
#include "../perm.h"
#include "../plat.h"
#include "../path.h"
#include "../json.h"
#include "../buf.h"
#include "tap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_dir[512];

static void
scratch(void)
{
    buf p;
    sprintf(g_dir, "/tmp/haios2_tools_%ld", (long)plat_time());
    plat_mkdir_p(g_dir);
    buf_init(&p);
    buf_puts(&p, g_dir);
    buf_puts(&p, "/sub");
    plat_mkdir_p(buf_cstr(&p));
    buf_free(&p);
}

static void
scratch_clean(void)
{
    buf cmd, out;
    buf_init(&cmd); buf_init(&out);
    buf_puts(&cmd, "rm -rf ");
    buf_puts(&cmd, g_dir);
    plat_run(buf_cstr(&cmd), NULL, 10, &out, 1024);
    buf_free(&cmd); buf_free(&out);
}

static void
put(const char *rel, const char *content)
{
    buf   p;
    FILE *f;
    buf_init(&p);
    buf_puts(&p, g_dir);
    buf_putc(&p, '/');
    buf_puts(&p, rel);
    f = fopen(buf_cstr(&p), "wb");
    if (f != NULL) { fputs(content, f); fclose(f); }
    buf_free(&p);
}

static int
slurp_rel(const char *rel, buf *out)
{
    buf   p;
    FILE *f;
    char  c[512];
    size_t n;
    buf_init(&p);
    buf_puts(&p, g_dir);
    buf_putc(&p, '/');
    buf_puts(&p, rel);
    f = fopen(buf_cstr(&p), "rb");
    buf_free(&p);
    if (f == NULL) return -1;
    while ((n = fread(c, 1, sizeof(c), f)) > 0) buf_append(out, c, n);
    fclose(f);
    buf_cstr(out);
    return 0;
}

/* ------------------------------------------------------------- harness --- */

typedef struct {
    int         asked;
    perm_effect answer;
    char        last_action[64];
    char        last_resource[512];
} asker;

static perm_effect
ask_cb(void *ctx, const char *action, const char *resource, const char *input)
{
    asker *a = (asker *)ctx;
    (void)input;
    a->asked++;
    strncpy(a->last_action, action, sizeof(a->last_action) - 1);
    a->last_action[sizeof(a->last_action) - 1] = '\0';
    strncpy(a->last_resource, resource, sizeof(a->last_resource) - 1);
    a->last_resource[sizeof(a->last_resource) - 1] = '\0';
    return a->answer;
}

static void
run_tool(tool_registry *reg, perm_gate *g, const char *name,
         const char *args_json, tool_result *r)
{
    json_arena *a = json_arena_new();
    json_value *v = json_parse(a, args_json, strlen(args_json));
    tool_ctx    c;

    c.workdir   = g_dir;
    c.gate      = g;
    c.call_id   = "c1";
    c.raw_input = args_json;

    tool_result_init(r);
    tool_execute(reg, name, v, &c, r);
    json_arena_free(a);
}

/* ---------------------------------------------------------------- perm --- */

static void
test_perm_rules(void)
{
    perm_gate g;

    perm_init(&g);
    EQLONG(perm_lookup(&g, "write", "/x"), PERM_ASK, "unmatched asks");

    perm_add(&g, "read", "*", PERM_ALLOW);
    EQLONG(perm_lookup(&g, "read", "/anything"), PERM_ALLOW, "wildcard allow");
    EQLONG(perm_lookup(&g, "write", "/anything"), PERM_ASK, "other action asks");

    /* Last match wins, so a later rule narrows an earlier one. */
    perm_add(&g, "read", "*/secrets/*", PERM_DENY);
    EQLONG(perm_lookup(&g, "read", "/a/secrets/b"), PERM_DENY,
           "later rule narrows the earlier wildcard");
    EQLONG(perm_lookup(&g, "read", "/a/public/b"), PERM_ALLOW,
           "unrelated path still allowed");

    /* Session rules outrank config rules entirely. */
    perm_add_session(&g, "read", "*/secrets/*", PERM_ALLOW);
    EQLONG(perm_lookup(&g, "read", "/a/secrets/b"), PERM_ALLOW,
           "session rule outranks config rule");
    perm_clear_session(&g);
    EQLONG(perm_lookup(&g, "read", "/a/secrets/b"), PERM_DENY,
           "clearing session rules restores config");

    EQLONG(perm_check(&g, "write", "/x", NULL), PERM_DENY,
           "no ask callback means deny, not allow");
}

static void
test_perm_ask(void)
{
    perm_gate g;
    asker     a;

    memset(&a, 0, sizeof(a));
    a.answer = PERM_ALLOW;
    perm_init(&g);
    perm_set_ask(&g, ask_cb, &a);

    EQLONG(perm_check(&g, "cmd", "del *.*", NULL), PERM_ALLOW, "ask allows");
    EQLONG(a.asked, 1, "callback fired");
    EQSTR(a.last_action, "cmd", "action passed to the prompt");
    EQSTR(a.last_resource, "del *.*", "resource passed to the prompt");

    a.answer = PERM_DENY;
    EQLONG(perm_check(&g, "cmd", "x", NULL), PERM_DENY, "ask denies");

    a.answer = PERM_ASK;   /* an indecisive UI must not mean yes */
    EQLONG(perm_check(&g, "cmd", "x", NULL), PERM_DENY, "ASK answer is denied");

    g.yolo = 1;
    a.asked = 0;
    EQLONG(perm_check(&g, "cmd", "rm -rf /", NULL), PERM_ALLOW, "yolo allows");
    EQLONG(a.asked, 0, "yolo does not prompt");
}

/* --------------------------------------------------------------- tools --- */

static void
test_read(void)
{
    tool_registry reg;
    perm_gate     g;
    tool_result   r;

    tool_registry_init(&reg);
    tool_register_builtins(&reg);
    perm_init(&g);

    put("a.txt", "line1\nline2\nline3\n");

    run_tool(&reg, &g, "read", "{\"path\":\"a.txt\"}", &r);
    OK(r.ok, "read succeeds without prompting inside the working directory");
    EQSTR(buf_cstr(&r.out), "line1\nline2\nline3\n", "contents");
    tool_result_free(&r);

    run_tool(&reg, &g, "read", "{\"path\":\"a.txt\",\"offset\":2}", &r);
    EQSTR(buf_cstr(&r.out), "line2\nline3\n", "offset is 1-based");
    tool_result_free(&r);

    run_tool(&reg, &g, "read", "{\"path\":\"a.txt\",\"offset\":1,\"limit\":2}", &r);
    EQSTR(buf_cstr(&r.out), "line1\nline2\n", "limit");
    tool_result_free(&r);

    run_tool(&reg, &g, "read", "{\"path\":\"nope.txt\"}", &r);
    OK(!r.ok, "missing file fails");
    OK(!r.denied, "and is an error, not a denial");
    tool_result_free(&r);

    run_tool(&reg, &g, "read", "{}", &r);
    OK(!r.ok, "missing path fails");
    tool_result_free(&r);
}

static void
test_read_binary_refused(void)
{
    tool_registry reg;
    perm_gate     g;
    tool_result   r;
    buf           p;
    FILE         *f;

    tool_registry_init(&reg);
    tool_register_builtins(&reg);
    perm_init(&g);

    buf_init(&p);
    buf_puts(&p, g_dir);
    buf_puts(&p, "/bin.dat");
    f = fopen(buf_cstr(&p), "wb");
    fwrite("ab\0cd", 1, 5, f);
    fclose(f);
    buf_free(&p);

    run_tool(&reg, &g, "read", "{\"path\":\"bin.dat\"}", &r);
    OK(!r.ok, "binary file refused");
    OK(strstr(buf_cstr(&r.out), "binary") != NULL, "and says why");
    tool_result_free(&r);
}

/*
 * The read-only exemption must be bounded by the working directory, and
 * bounded after ".." is resolved. A prefix compare would let this through.
 */
static void
test_readonly_escape_is_gated(void)
{
    tool_registry reg;
    perm_gate     g;
    asker         a;
    tool_result   r;

    tool_registry_init(&reg);
    tool_register_builtins(&reg);
    memset(&a, 0, sizeof(a));
    a.answer = PERM_DENY;
    perm_init(&g);
    perm_set_ask(&g, ask_cb, &a);

    run_tool(&reg, &g, "read", "{\"path\":\"../../etc/passwd\"}", &r);
    OK(a.asked == 1, "a read escaping the working directory is prompted");
    OK(r.denied, "and can be refused");
    tool_result_free(&r);

    a.asked = 0;
    run_tool(&reg, &g, "read", "{\"path\":\"sub/../a.txt\"}", &r);
    OK(a.asked == 0, "a path that only looks like an escape is not prompted");
    OK(r.ok, "and still works");
    tool_result_free(&r);
}

static void
test_write_and_edit(void)
{
    tool_registry reg;
    perm_gate     g;
    asker         a;
    tool_result   r;
    buf           got;

    tool_registry_init(&reg);
    tool_register_builtins(&reg);
    memset(&a, 0, sizeof(a));
    a.answer = PERM_ALLOW;
    perm_init(&g);
    perm_set_ask(&g, ask_cb, &a);

    a.asked = 0;
    run_tool(&reg, &g, "write",
             "{\"path\":\"out/new.txt\",\"content\":\"hello\\nworld\\n\"}", &r);
    OK(r.ok, "write succeeds");
    EQLONG(a.asked, 1, "write is always gated, even inside the project");
    tool_result_free(&r);

    buf_init(&got);
    OK(slurp_rel("out/new.txt", &got) == 0, "parent directory was created");
    EQSTR(buf_cstr(&got), "hello\nworld\n", "content written verbatim");
    buf_free(&got);

    run_tool(&reg, &g, "edit",
             "{\"path\":\"out/new.txt\",\"old\":\"world\",\"new\":\"OS/2\"}", &r);
    OK(r.ok, "edit succeeds");
    tool_result_free(&r);

    buf_init(&got);
    slurp_rel("out/new.txt", &got);
    EQSTR(buf_cstr(&got), "hello\nOS/2\n", "edit applied");
    buf_free(&got);

    /* Ambiguity must be an error: silently editing the first of several
     * identical matches is how an agent corrupts a file. */
    put("dup.txt", "x\nx\nx\n");
    run_tool(&reg, &g, "edit",
             "{\"path\":\"dup.txt\",\"old\":\"x\",\"new\":\"y\"}", &r);
    OK(!r.ok, "ambiguous edit refused");
    OK(strstr(buf_cstr(&r.out), "ambiguous") != NULL, "and says so");
    tool_result_free(&r);

    buf_init(&got);
    slurp_rel("dup.txt", &got);
    EQSTR(buf_cstr(&got), "x\nx\nx\n", "refused edit left the file untouched");
    buf_free(&got);

    run_tool(&reg, &g, "edit",
             "{\"path\":\"dup.txt\",\"old\":\"x\",\"new\":\"y\","
             "\"replace_all\":true}", &r);
    OK(r.ok, "replace_all succeeds");
    tool_result_free(&r);

    buf_init(&got);
    slurp_rel("dup.txt", &got);
    EQSTR(buf_cstr(&got), "y\ny\ny\n", "all occurrences replaced");
    buf_free(&got);

    run_tool(&reg, &g, "edit",
             "{\"path\":\"dup.txt\",\"old\":\"zzz\",\"new\":\"q\"}", &r);
    OK(!r.ok, "edit with no match fails");
    tool_result_free(&r);

    run_tool(&reg, &g, "edit",
             "{\"path\":\"dup.txt\",\"old\":\"\",\"new\":\"q\"}", &r);
    OK(!r.ok, "empty 'old' refused");
    tool_result_free(&r);
}

static void
test_write_denied_leaves_no_file(void)
{
    tool_registry reg;
    perm_gate     g;
    asker         a;
    tool_result   r;
    buf           got;

    tool_registry_init(&reg);
    tool_register_builtins(&reg);
    memset(&a, 0, sizeof(a));
    a.answer = PERM_DENY;
    perm_init(&g);
    perm_set_ask(&g, ask_cb, &a);

    run_tool(&reg, &g, "write",
             "{\"path\":\"denied.txt\",\"content\":\"nope\"}", &r);
    OK(r.denied, "write denied");
    OK(!r.ok, "and reported as not ok");
    tool_result_free(&r);

    buf_init(&got);
    OK(slurp_rel("denied.txt", &got) != 0, "denied write created no file");
    buf_free(&got);
}

static void
test_ls(void)
{
    tool_registry reg;
    perm_gate     g;
    tool_result   r;

    tool_registry_init(&reg);
    tool_register_builtins(&reg);
    perm_init(&g);

    run_tool(&reg, &g, "ls", "{}", &r);
    OK(r.ok, "ls defaults to the working directory");
    OK(strstr(buf_cstr(&r.out), "a.txt") != NULL, "lists a file");
    OK(strstr(buf_cstr(&r.out), "sub/") != NULL, "marks directories with /");
    tool_result_free(&r);

    run_tool(&reg, &g, "ls", "{\"path\":\"sub\"}", &r);
    OK(r.ok, "ls of an empty subdirectory succeeds");
    tool_result_free(&r);

    run_tool(&reg, &g, "ls", "{\"path\":\"nosuchdir\"}", &r);
    OK(!r.ok, "ls of a missing directory fails");
    tool_result_free(&r);
}

static void
test_grep(void)
{
    tool_registry reg;
    perm_gate     g;
    tool_result   r;

    tool_registry_init(&reg);
    tool_register_builtins(&reg);
    perm_init(&g);

    put("g1.c", "int main(void)\n{\n  return NEEDLE;\n}\n");
    put("g2.h", "#define NEEDLE 1\n");
    put("g3.txt", "no match here\n");

    run_tool(&reg, &g, "grep", "{\"pattern\":\"NEEDLE\"}", &r);
    OK(r.ok, "grep runs");
    OK(strstr(buf_cstr(&r.out), "g1.c:3:") != NULL, "reports path:line:");
    OK(strstr(buf_cstr(&r.out), "g2.h:1:") != NULL, "finds the header too");
    OK(strstr(buf_cstr(&r.out), "g3.txt") == NULL, "skips non-matching files");
    tool_result_free(&r);

    run_tool(&reg, &g, "grep",
             "{\"pattern\":\"NEEDLE\",\"include\":\"*.h\"}", &r);
    OK(strstr(buf_cstr(&r.out), "g2.h") != NULL, "include glob keeps .h");
    OK(strstr(buf_cstr(&r.out), "g1.c") == NULL, "include glob drops .c");
    tool_result_free(&r);

    run_tool(&reg, &g, "grep", "{\"pattern\":\"needle\"}", &r);
    OK(strstr(buf_cstr(&r.out), "no matches") != NULL, "case-sensitive by default");
    tool_result_free(&r);

    run_tool(&reg, &g, "grep",
             "{\"pattern\":\"needle\",\"ignore_case\":true}", &r);
    OK(strstr(buf_cstr(&r.out), "g1.c") != NULL, "ignore_case works");
    tool_result_free(&r);

    run_tool(&reg, &g, "grep", "{}", &r);
    OK(!r.ok, "grep without a pattern fails");
    tool_result_free(&r);
}

static void
test_cmd(void)
{
    tool_registry reg;
    perm_gate     g;
    asker         a;
    tool_result   r;

    tool_registry_init(&reg);
    tool_register_builtins(&reg);
    memset(&a, 0, sizeof(a));
    a.answer = PERM_ALLOW;
    perm_init(&g);
    perm_set_ask(&g, ask_cb, &a);

    a.asked = 0;
    run_tool(&reg, &g, "cmd", "{\"command\":\"echo hello\"}", &r);
    OK(r.ok, "cmd runs");
    EQLONG(a.asked, 1, "cmd is always gated");
    EQSTR(a.last_resource, "echo hello",
          "the prompt shows the command, not the directory");
    OK(strstr(buf_cstr(&r.out), "hello") != NULL, "captures stdout");
    tool_result_free(&r);

    run_tool(&reg, &g, "cmd", "{\"command\":\"echo oops 1>&2\"}", &r);
    OK(strstr(buf_cstr(&r.out), "oops") != NULL, "captures stderr too");
    tool_result_free(&r);

    run_tool(&reg, &g, "cmd", "{\"command\":\"exit 3\"}", &r);
    OK(!r.ok, "non-zero exit reported as failure");
    OK(strstr(buf_cstr(&r.out), "exit status 3") != NULL, "status included");
    tool_result_free(&r);

    /* The command runs in the working directory, not wherever the host is. */
    run_tool(&reg, &g, "cmd", "{\"command\":\"ls a.txt\"}", &r);
    OK(r.ok, "command runs in the working directory");
    tool_result_free(&r);
}

static void
test_unknown_tool(void)
{
    tool_registry reg;
    perm_gate     g;
    tool_result   r;

    tool_registry_init(&reg);
    tool_register_builtins(&reg);
    perm_init(&g);

    run_tool(&reg, &g, "teleport", "{}", &r);
    OK(!r.ok, "unknown tool fails");
    OK(!r.denied, "and is not a denial");
    OK(strstr(buf_cstr(&r.out), "unknown tool") != NULL, "says what happened");
    tool_result_free(&r);
}

static void
test_defs(void)
{
    tool_registry reg;
    prov_tool     defs[TOOL_MAX];
    size_t        n;
    size_t        i;
    int           schemas_ok = 1;

    tool_registry_init(&reg);
    tool_register_builtins(&reg);

    n = tool_defs(&reg, defs, TOOL_MAX);
    EQLONG((long)n, 6, "six built-in tools");

    /* Every schema is embedded raw into the request, so a malformed one would
     * corrupt the whole document rather than just that tool. */
    for (i = 0; i < n; i++) {
        json_arena *a = json_arena_new();
        if (json_parse(a, defs[i].schema, strlen(defs[i].schema)) == NULL)
            schemas_ok = 0;
        json_arena_free(a);
    }
    OK(schemas_ok, "every built-in schema is valid JSON");
}

int
main(void)
{
    scratch();
    test_perm_rules();
    test_perm_ask();
    test_read();
    test_read_binary_refused();
    test_readonly_escape_is_gated();
    test_write_and_edit();
    test_write_denied_leaves_no_file();
    test_ls();
    test_grep();
    test_cmd();
    test_unknown_tool();
    test_defs();
    scratch_clean();
    TAP_REPORT("test_tools");
}
