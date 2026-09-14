/* test_path.c - path normalisation, containment and globbing. */

#include "../path.h"
#include "../buf.h"
#include "tap.h"

#include <string.h>

static void
norm(const char *in, const char *want, const char *what)
{
    buf out;
    buf_init(&out);
    if (path_normalize(&out, in) != 0) {
        if (want == NULL) { tap_pass++; }
        else { tap_fail++; printf("  FAIL %s (rejected, wanted %s)\n", what, want); }
    } else if (want == NULL) {
        tap_fail++;
        printf("  FAIL %s (accepted '%s', wanted rejection)\n", what,
               buf_cstr(&out));
    } else {
        EQSTR(buf_cstr(&out), want, what);
    }
    buf_free(&out);
}

static void
test_normalize(void)
{
    norm("/a/b/c",         "/a/b/c",  "plain absolute");
    norm("/a//b///c",      "/a/b/c",  "duplicate separators");
    norm("/a/./b/./c",     "/a/b/c",  "dot components");
    norm("/a/b/../c",      "/a/c",    "parent component");
    norm("/a/b/../../c",   "/c",      "two parents unwind to the root");
    norm("/a/b/",          "/a/b",    "trailing slash dropped");
    norm("/",              "/",       "bare root");
    norm("a/b",            "a/b",     "relative");
    norm("./a",            "a",       "leading dot");
    norm("a/../b",         "b",       "relative parent");
    norm("../a",           "../a",    "relative may start with ..");
    norm("C:\\OS2\\X",     "c:/OS2/X","drive lowercased, case otherwise kept");
    norm("C:/OS2/../MDOS", "c:/MDOS", "drive with parent");
    norm("C:\\",           "c:/",     "bare drive root");
    norm("",               NULL,      "empty rejected");
    norm("/..",            NULL,      "escape above absolute root rejected");
    norm("/a/../..",       NULL,      "escape via parents rejected");
    norm("C:\\..",         NULL,      "escape above drive root rejected");
    norm("C:foo",          NULL,      "drive-relative rejected");
    norm("\\\\srv\\share", NULL,      "UNC rejected");

    /* Normalisation must not fold case: the result is used to open files, and
     * on POSIX "Makefile" and "makefile" are different files. */
    norm("/Proj/SrcFile.C", "/Proj/SrcFile.C", "case preserved exactly");

    OK(path_is_abs("/x"), "/x is absolute");
    OK(path_is_abs("C:\\x"), "C:\\x is absolute");
    OK(!path_is_abs("C:x"), "C:x is not absolute");
    OK(!path_is_abs("x"), "x is not absolute");
}

static void
test_resolve(void)
{
    buf out;

    buf_init(&out);
    path_resolve(&out, "/proj", "src/a.c");
    EQSTR(buf_cstr(&out), "/proj/src/a.c", "relative resolved against base");

    path_resolve(&out, "/proj", "/etc/passwd");
    EQSTR(buf_cstr(&out), "/etc/passwd", "absolute replaces base");

    path_resolve(&out, "/proj/", "a");
    EQSTR(buf_cstr(&out), "/proj/a", "base with trailing slash");

    path_resolve(&out, "C:\\PROJ", "SRC\\B.C");
    EQSTR(buf_cstr(&out), "c:/PROJ/SRC/B.C", "OS/2 style, case preserved");

    OK(path_resolve(&out, "/proj", "../../etc") != 0,
       "resolution escaping the root fails");

    buf_free(&out);
}

/* The containment check the permission gate depends on. */
static void
test_within(void)
{
    OK(path_within_ex("/proj", "/proj", 0),        "root contains itself");
    OK(path_within_ex("/proj", "/proj/a", 0),      "direct child");
    OK(path_within_ex("/proj", "/proj/a/b/c", 0),  "deep child");
    OK(path_within_ex("/proj", "/proj/./a", 0),    "child via dot");

    OK(!path_within_ex("/proj", "/project2/x", 0),
       "sibling with a shared prefix is NOT inside");
    OK(!path_within_ex("/proj", "/projx", 0),
       "prefix without a separator boundary is NOT inside");
    OK(!path_within_ex("/proj", "/etc/passwd", 0), "unrelated path");
    OK(!path_within_ex("/proj", "/proj/../etc/passwd", 0),
       "traversal out of the root is NOT inside");
    OK(!path_within_ex("/proj/a", "/proj", 0), "parent is not inside child");

    OK(path_within_ex("/", "/anything", 0), "everything is under /");

    /* Case folding: correct on OS/2, wrong on POSIX, so it is explicit. */
    OK(path_within_ex("C:\\PROJ", "c:\\proj\\a", 1),
       "case-insensitive match when folding (OS/2 filesystems)");
    OK(!path_within_ex("/proj", "/PROJ/a", 0),
       "case-sensitive mismatch when not folding (POSIX)");
    OK(path_within_ex("c:/proj", "C:/PROJ/SUB/F.TXT", 1),
       "folded deep child");

    OK(!path_within_ex("/proj", "C:foo", 0), "rejected path is inside nothing");
    OK(!path_within_ex("/proj", "", 0),      "empty is inside nothing");
    OK(!path_within_ex("", "/proj/a", 0),    "nothing contains anything");
}

static void
test_basename(void)
{
    EQSTR(path_basename("/a/b/c.txt"), "c.txt", "basename");
    EQSTR(path_basename("c.txt"), "c.txt", "bare name");
    EQSTR(path_basename("C:\\OS2\\X.SYS"), "X.SYS", "backslash basename");
    EQSTR(path_basename("/a/"), "", "trailing slash gives empty");
}

static void
test_glob(void)
{
    OK(path_glob("*.c", "main.c", 0),          "*.c matches main.c");
    OK(!path_glob("*.c", "main.h", 0),         "*.c rejects main.h");
    OK(path_glob("*", "anything", 0),          "* matches all");
    OK(path_glob("a?c", "abc", 0),             "? matches one");
    OK(!path_glob("a?c", "ac", 0),             "? needs one");
    OK(path_glob("[abc]x", "bx", 0),           "class");
    OK(!path_glob("[abc]x", "dx", 0),          "class miss");
    OK(path_glob("[a-f]x", "cx", 0),           "range");
    OK(path_glob("[!a-f]x", "zx", 0),          "negated range");
    OK(!path_glob("[!a-f]x", "cx", 0),         "negated range miss");
    OK(path_glob("src/*/main.c", "src/app/main.c", 0), "* spans a component");
    OK(path_glob("git:*", "git:status", 0),    "action:resource form");
    OK(path_glob("", "", 0),                   "empty matches empty");
    OK(!path_glob("", "x", 0),                 "empty rejects non-empty");
    OK(path_glob("a\\*b", "a*b", 0),           "backslash escapes *");
    OK(!path_glob("a\\*b", "axb", 0),          "escaped * is literal");
    OK(path_glob("*.C", "main.c", 1),          "folded match");
    OK(!path_glob("*.C", "main.c", 0),         "unfolded mismatch");

    /* A pattern that backtracks heavily must still terminate promptly. */
    OK(!path_glob("*a*a*a*a*a*a*a*a*b",
                  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac", 0),
       "pathological pattern terminates without blowing up");
}

int
main(void)
{
    test_normalize();
    test_resolve();
    test_within();
    test_basename();
    test_glob();
    TAP_REPORT("test_path");
}
