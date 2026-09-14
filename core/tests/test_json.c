/* test_json.c - JSON writer and parser. C89. */

#include "../json.h"
#include "../buf.h"
#include "tap.h"

#include <stdlib.h>
#include <string.h>

static void
emit_sample(json_writer *w)
{
    json_w_obj_open(w);
      json_w_key(w, "model");
      json_w_str(w, "qwen3");
      json_w_key(w, "stream");
      json_w_bool(w, 1);
      json_w_key(w, "n");
      json_w_long(w, -42);
      json_w_key(w, "messages");
      json_w_arr_open(w);
        json_w_obj_open(w);
          json_w_key(w, "role");
          json_w_str(w, "user");
          json_w_key(w, "content");
          json_w_str(w, "hi");
        json_w_obj_close(w);
      json_w_arr_close(w);
      json_w_key(w, "nothing");
      json_w_null(w);
    json_w_obj_close(w);
    json_w_finish(w);
}

static void
test_writer_basic(void)
{
    buf         out;
    json_writer w;

    buf_init(&out);
    json_w_init(&w, json_buf_sink, &out);
    emit_sample(&w);

    OK(json_w_finish(&w) == 0, "writer finished clean");
    EQSTR(buf_cstr(&out),
          "{\"model\":\"qwen3\",\"stream\":true,\"n\":-42,"
          "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
          "\"nothing\":null}",
          "document round-trips");
    buf_free(&out);
}

/* The counting sink must agree with the real emit, byte for byte -- that
 * equality is what makes the two-pass Content-Length scheme correct. */
static void
test_writer_count_matches(void)
{
    buf         out;
    json_writer w;
    long        count = 0;

    json_w_init(&w, json_count_sink, &count);
    emit_sample(&w);

    buf_init(&out);
    json_w_init(&w, json_buf_sink, &out);
    emit_sample(&w);

    EQLONG(count, (long)out.len, "counting pass matches emitting pass");
    buf_free(&out);
}

static void
test_writer_escapes(void)
{
    buf         out;
    json_writer w;

    buf_init(&out);
    json_w_init(&w, json_buf_sink, &out);
    json_w_str(&w, "a\"b\\c\nd\te\001f");
    EQSTR(buf_cstr(&out), "\"a\\\"b\\\\c\\nd\\te\\u0001f\"",
          "control and quote escapes");
    buf_free(&out);

    /* Valid multi-byte UTF-8 passes through untouched. */
    buf_init(&out);
    json_w_init(&w, json_buf_sink, &out);
    json_w_str(&w, "caf\303\251 \342\226\266 \360\237\232\200");
    EQSTR(buf_cstr(&out), "\"caf\303\251 \342\226\266 \360\237\232\200\"",
          "valid UTF-8 preserved");
    buf_free(&out);
}

/*
 * A tool returning a stray high byte must not be able to abort a request.
 * The Haiku build crashed on exactly this with a strict encoder; here the bad
 * byte becomes U+FFFD and the emit continues.
 */
static void
test_writer_bad_utf8(void)
{
    buf         out;
    json_writer w;
    static const char bad[] = { 'a', (char)0x80, 'b',
                                (char)0xC3, '\0' };           /* truncated */
    static const char overlong[] = { (char)0xC0, (char)0xAF, '\0' };
    static const char surrogate[] = { (char)0xED, (char)0xA0,
                                      (char)0x80, '\0' };

    buf_init(&out);
    json_w_init(&w, json_buf_sink, &out);
    json_w_str(&w, bad);
    OK(json_w_finish(&w) == 0, "lone continuation byte does not fail the emit");
    EQSTR(buf_cstr(&out), "\"a\357\277\275b\357\277\275\"",
          "invalid bytes become U+FFFD");
    buf_free(&out);

    buf_init(&out);
    json_w_init(&w, json_buf_sink, &out);
    json_w_str(&w, overlong);
    EQSTR(buf_cstr(&out), "\"\357\277\275\357\277\275\"",
          "overlong encoding rejected");
    buf_free(&out);

    buf_init(&out);
    json_w_init(&w, json_buf_sink, &out);
    json_w_str(&w, surrogate);
    EQSTR(buf_cstr(&out), "\"\357\277\275\357\277\275\357\277\275\"",
          "encoded surrogate rejected");
    buf_free(&out);
}

/* Numbers must be compact *and* exact: %.17g is exact but renders 0.95 as
 * 0.94999999999999996, which costs bytes on every request. */
static void
test_writer_doubles(void)
{
    static const double vals[] = {
        0.95, 0.1, 1.0, 0.0, -2.5, 1e-7, 1.5e300, 0.3333333333333333
    };
    buf         out;
    json_writer w;
    json_arena *a;
    json_value *v;
    int         i;

    for (i = 0; i < 8; i++) {
        buf_init(&out);
        json_w_init(&w, json_buf_sink, &out);
        json_w_double(&w, vals[i]);
        a = json_arena_new();
        v = json_parse(a, out.data, out.len);
        OK(v != NULL && json_as_num(v, -1.0) == vals[i],
           "double round-trips exactly");
        json_arena_free(a);
        buf_free(&out);
    }

    buf_init(&out);
    json_w_init(&w, json_buf_sink, &out);
    json_w_double(&w, 0.95);
    EQSTR(buf_cstr(&out), "0.95", "0.95 renders compactly");
    buf_free(&out);

    buf_init(&out);
    json_w_init(&w, json_buf_sink, &out);
    json_w_double(&w, 1.0 / 0.0 * 0.0);   /* NaN */
    EQSTR(buf_cstr(&out), "null", "NaN becomes null (JSON has no NaN)");
    buf_free(&out);

    buf_init(&out);
    json_w_init(&w, json_buf_sink, &out);
    json_w_double(&w, 1e308 * 10.0);      /* +inf */
    EQSTR(buf_cstr(&out), "null", "infinity becomes null");
    buf_free(&out);
}

static void
test_writer_misuse(void)
{
    buf         out;
    json_writer w;

    /* A value where a key belongs must be caught, not silently emitted. */
    buf_init(&out);
    json_w_init(&w, json_buf_sink, &out);
    json_w_obj_open(&w);
    json_w_str(&w, "orphan");
    OK(json_w_finish(&w) != 0, "bare value inside object is an error");
    buf_free(&out);

    /* An unclosed container must not report success. */
    buf_init(&out);
    json_w_init(&w, json_buf_sink, &out);
    json_w_arr_open(&w);
    json_w_long(&w, 1);
    OK(json_w_finish(&w) != 0, "unclosed array is an error");
    buf_free(&out);

    /* A key with no value must not close. */
    buf_init(&out);
    json_w_init(&w, json_buf_sink, &out);
    json_w_obj_open(&w);
    json_w_key(&w, "k");
    json_w_obj_close(&w);
    OK(json_w_finish(&w) != 0, "dangling key is an error");
    buf_free(&out);
}

static void
test_parse_basic(void)
{
    json_arena *a = json_arena_new();
    const char *src =
        "{ \"a\": 1, \"b\": [true, false, null, -2.5e3], "
        "\"c\": {\"d\": \"x\"} }";
    json_value *v = json_parse(a, src, strlen(src));

    OK(v != NULL, "parses");
    EQLONG(json_type_of(v), JSON_OBJ, "root is object");
    EQLONG(json_as_long(json_get(v, "a"), -1), 1, "a == 1");
    EQLONG(json_len(json_get(v, "b")), 4, "b has 4 items");
    OK(json_as_bool(json_at(json_get(v, "b"), 0), 0) == 1, "b[0] true");
    OK(json_as_bool(json_at(json_get(v, "b"), 1), 1) == 0, "b[1] false");
    EQLONG(json_type_of(json_at(json_get(v, "b"), 2)), JSON_NULL, "b[2] null");
    EQLONG(json_as_long(json_at(json_get(v, "b"), 3), 0), -2500, "b[3] -2.5e3");
    EQSTR(json_as_str(json_path(v, "c.d"), NULL), "x", "dotted path");
    OK(json_path(v, "c.nope") == NULL, "missing path is NULL");
    OK(json_get(v, "zzz") == NULL, "missing key is NULL");

    json_arena_free(a);
}

static void
test_parse_strings(void)
{
    json_arena *a = json_arena_new();
    const char *src = "[\"a\\\"b\", \"\\u00e9\", \"\\ud83d\\ude80\", "
                      "\"\\u0041\", \"\\/\\b\\f\\n\\r\\t\"]";
    json_value *v = json_parse(a, src, strlen(src));

    OK(v != NULL, "string array parses");
    EQSTR(json_as_str(json_at(v, 0), NULL), "a\"b", "escaped quote");
    EQSTR(json_as_str(json_at(v, 1), NULL), "\303\251", "\\u00e9 -> UTF-8");
    EQSTR(json_as_str(json_at(v, 2), NULL), "\360\237\232\200",
          "surrogate pair -> UTF-8");
    EQSTR(json_as_str(json_at(v, 3), NULL), "A", "\\u0041");
    EQSTR(json_as_str(json_at(v, 4), NULL), "/\b\f\n\r\t", "simple escapes");

    json_arena_free(a);
}

static void
test_parse_rejects(void)
{
    json_arena *a = json_arena_new();
    static const char *bad[] = {
        "{",  "}", "[1,]", "{\"a\":}", "{'a':1}", "01", "1.", ".5", "+1",
        "tru", "\"unterminated", "[1 2]", "{\"a\" 1}", "", "   ",
        "{\"a\":1}x", "\"a\\qb\"", "[1,2]]", "-", "1e", "NaN", "Infinity",
        NULL
    };
    int i;

    for (i = 0; bad[i] != NULL; i++) {
        json_value *v = json_parse(a, bad[i], strlen(bad[i]));
        OK(v == NULL, bad[i][0] ? bad[i] : "(empty) rejected");
    }

    json_arena_free(a);
}

static void
test_parse_depth(void)
{
    json_arena *a = json_arena_new();
    buf         deep;
    int         i;
    json_value *v;

    /* Nesting past the cap must be refused rather than recursing the stack
     * into oblivion -- this is reachable from untrusted model output. */
    buf_init(&deep);
    for (i = 0; i < JSON_MAX_DEPTH + 10; i++)
        buf_putc(&deep, '[');
    for (i = 0; i < JSON_MAX_DEPTH + 10; i++)
        buf_putc(&deep, ']');

    v = json_parse(a, deep.data, deep.len);
    OK(v == NULL, "over-deep nesting rejected");

    buf_free(&deep);
    json_arena_free(a);
}

static void
test_parse_ws_and_empty(void)
{
    json_arena *a = json_arena_new();
    const char *src = " \t\r\n { \"a\" : [ ] , \"b\" : { } } \n";
    json_value *v = json_parse(a, src, strlen(src));

    OK(v != NULL, "whitespace tolerated");
    EQLONG(json_len(json_get(v, "a")), 0, "empty array");
    EQLONG(json_len(json_get(v, "b")), 0, "empty object");
    EQLONG(json_type_of(json_get(v, "a")), JSON_ARR, "empty array typed");
    EQLONG(json_type_of(json_get(v, "b")), JSON_OBJ, "empty object typed");

    json_arena_free(a);
}

/* Anything the writer emits, the parser must accept. */
static void
test_round_trip(void)
{
    buf          out;
    json_writer  w;
    json_arena  *a;
    json_value  *v;

    buf_init(&out);
    json_w_init(&w, json_buf_sink, &out);
    json_w_obj_open(&w);
      json_w_key(&w, "weird \"key\"\n");
      json_w_str(&w, "caf\303\251\t\001");
      json_w_key(&w, "d");
      json_w_double(&w, 1.5);
    json_w_obj_close(&w);
    json_w_finish(&w);

    a = json_arena_new();
    v = json_parse(a, out.data, out.len);
    OK(v != NULL, "writer output re-parses");
    EQSTR(json_as_str(json_get(v, "weird \"key\"\n"), NULL),
          "caf\303\251\t\001", "escaped key and value survive");
    OK(json_as_num(json_get(v, "d"), 0.0) == 1.5, "double survives");

    json_arena_free(a);
    buf_free(&out);
}

int
main(void)
{
    test_writer_basic();
    test_writer_count_matches();
    test_writer_escapes();
    test_writer_bad_utf8();
    test_writer_doubles();
    test_writer_misuse();
    test_parse_basic();
    test_parse_strings();
    test_parse_rejects();
    test_parse_depth();
    test_parse_ws_and_empty();
    test_round_trip();
    TAP_REPORT("test_json");
}
