/* test_store.c - session persistence and the prov_msgsrc iterator. */

#include "../store.h"
#include "../plat.h"
#include "../provider.h"
#include "../json.h"
#include "../buf.h"
#include "tap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_root[512];

static void
scratch(void)
{
    sprintf(g_root, "/tmp/haios2_store_%ld", (long)plat_time());
    plat_mkdir_p(g_root);
}

static void
scratch_clean(void)
{
    buf cmd;
    buf out;
    buf_init(&cmd);
    buf_init(&out);
    buf_puts(&cmd, "rm -rf ");
    buf_puts(&cmd, g_root);
    plat_run(buf_cstr(&cmd), NULL, 10, &out, 1024);
    buf_free(&cmd);
    buf_free(&out);
}

static void
test_create_and_meta(void)
{
    store      s;
    store_meta m;
    char       id[STORE_ID_LEN];

    OK(store_open(&s, g_root) == 0, "store opens");
    OK(store_create(&s, "/proj", "qwen3", id) == 0, "session created");
    OK(strlen(id) == 8, "session id is 8 chars (8.3-safe)");

    OK(store_read_meta(&s, id, &m) == 0, "meta reads back");
    EQSTR(m.cwd, "/proj", "cwd persisted");
    EQSTR(m.model, "qwen3", "model persisted");
    OK(m.created > 0, "created timestamp set");

    strcpy(m.title, "Fix CONFIG.SYS");
    m.tokens_in = 1234;
    OK(store_write_meta(&s, &m) == 0, "meta updates");
    OK(store_read_meta(&s, id, &m) == 0, "meta re-reads");
    EQSTR(m.title, "Fix CONFIG.SYS", "title persisted");
    EQLONG(m.tokens_in, 1234, "counters persisted");
}

/* Ids must sort newest-first by plain filename order, which is what makes
 * store_list cheap. */
static void
test_ids_sort_newest_first(void)
{
    store      s;
    store_meta list[8];
    char       a[STORE_ID_LEN];
    char       b[STORE_ID_LEN];
    int        n;

    store_open(&s, g_root);
    store_create(&s, "/p1", "m", a);
    store_create(&s, "/p2", "m", b);

    OK(strcmp(a, b) != 0, "consecutive sessions get distinct ids");

    n = store_list(&s, list, 8);
    OK(n >= 2, "list returns the sessions");
    {
        int i;
        int sorted = 1;
        for (i = 1; i < n; i++) {
            if (strcmp(list[i - 1].id, list[i].id) > 0)
                sorted = 0;
        }
        OK(sorted, "list is sorted by id");
    }
}

static void
test_append_and_iterate(void)
{
    store         s;
    char          id[STORE_ID_LEN];
    store_iter    it;
    prov_msg      m;
    prov_toolcall calls[1];

    store_open(&s, g_root);
    store_create(&s, "/proj", "m", id);

    calls[0].id   = "c1";
    calls[0].name = "read";
    calls[0].args = "{\"path\":\"C:\\\\OS2\\\\CONFIG.SYS\"}";

    OK(store_append(&s, id, PROV_ROLE_USER, "show me", 7, NULL, 0, NULL) == 0,
       "append user");
    OK(store_append(&s, id, PROV_ROLE_ASSISTANT, NULL, 0, calls, 1, NULL) == 0,
       "append assistant with tool call");
    OK(store_append(&s, id, PROV_ROLE_TOOL, "REM line", 8, NULL, 0, "c1") == 0,
       "append tool result");

    OK(store_iter_open(&it, &s, id) == 0, "iterator opens");

    OK(store_iter_next(&it, &m) == 1, "first record");
    EQLONG(m.role, PROV_ROLE_USER, "role user");
    EQSTR(m.content, "show me", "user content");

    OK(store_iter_next(&it, &m) == 1, "second record");
    EQLONG(m.role, PROV_ROLE_ASSISTANT, "role assistant");
    OK(m.content == NULL, "tool-only turn has no content");
    EQLONG((long)m.ncalls, 1, "one call");
    EQSTR(m.calls[0].name, "read", "call name");
    EQSTR(m.calls[0].args, "{\"path\":\"C:\\\\OS2\\\\CONFIG.SYS\"}",
          "call args survive the round trip");

    OK(store_iter_next(&it, &m) == 1, "third record");
    EQLONG(m.role, PROV_ROLE_TOOL, "role tool");
    EQSTR(m.tool_call_id, "c1", "tool_call_id linked");

    OK(store_iter_next(&it, &m) == 0, "exhausted");

    /* Rewind is what makes the two-pass Content-Length scheme work. */
    store_iter_rewind(&it);
    OK(store_iter_next(&it, &m) == 1, "rewound");
    EQSTR(m.content, "show me", "rewind returns to the first record");

    store_iter_close(&it);
}

/* The iterator is a prov_msgsrc, so it must drive prov_write_request
 * identically on both passes. */
static void
test_iterator_drives_request(void)
{
    store       s;
    char        id[STORE_ID_LEN];
    store_iter  it;
    prov_req    r;
    buf         out;
    json_writer w;
    long        len;

    store_open(&s, g_root);
    store_create(&s, "/proj", "m", id);
    store_append(&s, id, PROV_ROLE_USER, "hi \303\251", 5, NULL, 0, NULL);
    store_append(&s, id, PROV_ROLE_ASSISTANT, "hello", 5, NULL, 0, NULL);

    store_iter_open(&it, &s, id);
    prov_req_init(&r, "m");
    r.msgs = store_iter_src(&it);

    len = prov_request_length(&r);

    buf_init(&out);
    json_w_init(&w, json_buf_sink, &out);
    prov_write_request(&w, &r);

    EQLONG(len, (long)out.len, "counting pass matches emitting pass");
    OK(strstr(buf_cstr(&out), "\"content\":\"hi \303\251\"") != NULL,
       "user message emitted from disk");
    OK(strstr(buf_cstr(&out), "\"content\":\"hello\"") != NULL,
       "assistant message emitted from disk");

    buf_free(&out);
    store_iter_close(&it);
}

/*
 * A crash mid-append leaves a final line with no newline. That record must be
 * skipped rather than failing the whole session -- losing one message is
 * recoverable, losing the conversation is not.
 */
static void
test_truncated_tail(void)
{
    store       s;
    char        id[STORE_ID_LEN];
    store_iter  it;
    prov_msg    m;
    buf         p;
    FILE       *f;
    int         n = 0;

    store_open(&s, g_root);
    store_create(&s, "/proj", "m", id);
    store_append(&s, id, PROV_ROLE_USER, "good", 4, NULL, 0, NULL);

    buf_init(&p);
    buf_puts(&p, g_root);
    buf_puts(&p, "/sessions/");
    buf_puts(&p, id);
    buf_puts(&p, "/msgs.jsonl");
    f = fopen(buf_cstr(&p), "ab");
    OK(f != NULL, "can append raw");
    fputs("{\"role\":\"user\",\"cont", f);     /* truncated, no newline */
    fclose(f);
    buf_free(&p);

    store_iter_open(&it, &s, id);
    while (store_iter_next(&it, &m) == 1)
        n++;
    EQLONG(n, 1, "the intact record is returned");
    OK(it.skipped == 1, "the truncated tail is counted as skipped");
    store_iter_close(&it);
}

static void
test_garbage_line_skipped(void)
{
    store      s;
    char       id[STORE_ID_LEN];
    store_iter it;
    prov_msg   m;
    buf        p;
    FILE      *f;
    int        n = 0;

    store_open(&s, g_root);
    store_create(&s, "/proj", "m", id);

    buf_init(&p);
    buf_puts(&p, g_root);
    buf_puts(&p, "/sessions/");
    buf_puts(&p, id);
    buf_puts(&p, "/msgs.jsonl");
    f = fopen(buf_cstr(&p), "wb");
    fputs("{\"role\":\"user\",\"content\":\"a\"}\n", f);
    fputs("this is not json\n", f);
    fputs("\n", f);
    fputs("{\"role\":\"user\",\"content\":\"b\"}\n", f);
    fclose(f);
    buf_free(&p);

    store_iter_open(&it, &s, id);
    while (store_iter_next(&it, &m) == 1)
        n++;
    EQLONG(n, 2, "good records survive a corrupt line between them");
    OK(it.skipped == 1, "the corrupt line is counted");
    store_iter_close(&it);
}

static void
test_missing_session(void)
{
    store      s;
    store_meta m;
    store_iter it;
    prov_msg   msg;

    store_open(&s, g_root);
    OK(store_read_meta(&s, "deadbeef", &m) != 0, "missing meta fails cleanly");
    OK(store_iter_open(&it, &s, "deadbeef") == 0,
       "iterating a missing session is not an error");
    OK(store_iter_next(&it, &msg) == 0, "and yields nothing");
    store_iter_close(&it);
}

static void
test_delete(void)
{
    store      s;
    store_meta m;
    char       id[STORE_ID_LEN];

    store_open(&s, g_root);
    store_create(&s, "/proj", "m", id);
    store_append(&s, id, PROV_ROLE_USER, "x", 1, NULL, 0, NULL);
    OK(store_read_meta(&s, id, &m) == 0, "exists before delete");
    store_delete(&s, id);
    OK(store_read_meta(&s, id, &m) != 0, "gone after delete");
}

int
main(void)
{
    scratch();
    test_create_and_meta();
    test_ids_sort_newest_first();
    test_append_and_iterate();
    test_iterator_drives_request();
    test_truncated_tail();
    test_garbage_line_skipped();
    test_missing_session();
    test_delete();
    scratch_clean();
    TAP_REPORT("test_store");
}
