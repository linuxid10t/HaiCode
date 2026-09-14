/* path.c - path normalisation and containment. C89. */

#include "path.h"
#include "buf.h"

#include <string.h>

static char
p_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
    return c;
}

static int
p_sep(char c)
{
    return (c == '/' || c == '\\');
}

static int
p_drive(const char *p)
{
    if (p == NULL || p[0] == '\0' || p[1] != ':')
        return 0;
    if ((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z'))
        return 1;
    return 0;
}

int
path_is_rejected(const char *p)
{
    if (p == NULL || *p == '\0')
        return 1;
    /* UNC: two leading separators. */
    if (p_sep(p[0]) && p_sep(p[1]))
        return 1;
    /* Drive-relative: "C:foo" resolves against a per-drive current directory
     * that nothing here tracks. */
    if (p_drive(p) && p[2] != '\0' && !p_sep(p[2]))
        return 1;
    return 0;
}

int
path_is_abs(const char *p)
{
    if (p == NULL || *p == '\0')
        return 0;
    if (p_sep(p[0]))
        return 1;
    if (p_drive(p) && p_sep(p[2]))
        return 1;
    return 0;
}

/*
 * Normalises in place into `out`.
 *
 * Components are pushed onto a stack of offsets so ".." can pop one; that is
 * what stops "/proj/../etc" from being treated as living under /proj. A ".."
 * that would pop past the root is an error rather than a silent clamp, because
 * silently clamping turns an escape attempt into a valid path.
 */
#define PATH_MAX_COMP 64

int
path_normalize(buf *out, const char *p)
{
    size_t starts[PATH_MAX_COMP];
    int    ncomp = 0;
    size_t prefix_len;
    const char *s;

    buf_clear(out);
    if (path_is_rejected(p))
        return -1;

    s = p;

    /* Root prefix. */
    if (p_drive(s) && p_sep(s[2])) {
        buf_putc(out, p_lower(s[0]));
        buf_putc(out, ':');
        buf_putc(out, '/');
        s += 3;
    } else if (p_sep(s[0])) {
        buf_putc(out, '/');
        s += 1;
    }
    prefix_len = out->len;

    while (*s != '\0') {
        const char *comp;
        size_t      clen;

        while (p_sep(*s))
            s++;
        if (*s == '\0')
            break;

        comp = s;
        while (*s != '\0' && !p_sep(*s))
            s++;
        clen = (size_t)(s - comp);

        if (clen == 1 && comp[0] == '.')
            continue;

        if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
            if (ncomp > 0) {
                ncomp--;
                out->len = starts[ncomp];
                /* Drop the separator this component was preceded by. */
                if (out->len > prefix_len && out->data[out->len - 1] == '/')
                    out->len--;
                continue;
            }
            if (prefix_len > 0)
                return -1;   /* ".." above an absolute root */
            /* Relative path may legitimately begin with "..". */
            if (out->len > 0)
                buf_putc(out, '/');
            buf_append(out, "..", 2);
            continue;
        }

        if (ncomp >= PATH_MAX_COMP)
            return -1;
        if (out->len > prefix_len || (prefix_len == 0 && out->len > 0))
            buf_putc(out, '/');
        starts[ncomp++] = out->len;
        buf_append(out, comp, clen);
    }

    if (out->oom)
        return -1;
    if (buf_cstr(out) == NULL)
        return -1;
    /* A path that normalised to nothing is the current directory. */
    if (out->len == 0) {
        buf_putc(out, '.');
        buf_cstr(out);
    }
    return 0;
}

int
path_resolve(buf *out, const char *base, const char *rel)
{
    buf joined;
    int rc;

    if (rel == NULL || *rel == '\0')
        return path_normalize(out, (base != NULL) ? base : "");
    if (path_is_abs(rel))
        return path_normalize(out, rel);
    if (base == NULL || *base == '\0')
        return path_normalize(out, rel);

    buf_init(&joined);
    buf_puts(&joined, base);
    if (joined.len > 0 && !p_sep(joined.data[joined.len - 1]))
        buf_putc(&joined, '/');
    buf_puts(&joined, rel);

    if (joined.oom || buf_cstr(&joined) == NULL) {
        buf_free(&joined);
        return -1;
    }
    rc = path_normalize(out, joined.data);
    buf_free(&joined);
    return rc;
}

int
path_within_ex(const char *root, const char *cand, int fold)
{
    buf    nr;
    buf    nc;
    int    ok = 0;
    size_t i;

    buf_init(&nr);
    buf_init(&nc);

    if (path_normalize(&nr, (root != NULL) ? root : "") != 0)
        goto done;
    if (path_normalize(&nc, (cand != NULL) ? cand : "") != 0)
        goto done;
    if (nc.len < nr.len)
        goto done;

    for (i = 0; i < nr.len; i++) {
        char a = nr.data[i];
        char b = nc.data[i];
        if (fold) {
            a = p_lower(a);
            b = p_lower(b);
        }
        if (a != b)
            goto done;
    }

    if (nc.len == nr.len) {
        ok = 1;                       /* the root itself */
    } else if (nr.len > 0 && nr.data[nr.len - 1] == '/') {
        ok = 1;                       /* root is "/" or "c:/" */
    } else if (nc.data[nr.len] == '/') {
        ok = 1;                       /* a real component boundary, so
                                       * "/project2" is not inside "/proj" */
    }

done:
    buf_free(&nr);
    buf_free(&nc);
    return ok;
}

int
path_within(const char *root, const char *cand)
{
    return path_within_ex(root, cand, PATH_FOLD_CASE);
}

const char *
path_basename(const char *p)
{
    const char *last = p;
    const char *s;

    if (p == NULL)
        return "";
    for (s = p; *s != '\0'; s++) {
        if (p_sep(*s))
            last = s + 1;
    }
    return last;
}

/*
 * Glob matcher. Iterative with a single backtrack point, so a pattern like
 * "*a*a*a*a*b" costs O(n*m) rather than exploding -- the pattern can come from
 * a config file the model was able to influence.
 */
int
path_glob(const char *pat, const char *name, int fold)
{
    const char *p     = pat;
    const char *n     = name;
    const char *star  = NULL;
    const char *retry = NULL;

    if (pat == NULL || name == NULL)
        return 0;

    while (*n != '\0') {
        char pc = *p;
        char nc = fold ? p_lower(*n) : *n;

        if (pc == '\\' && p[1] != '\0') {
            char lit = fold ? p_lower(p[1]) : p[1];
            if (lit == nc) { p += 2; n++; continue; }
        } else if (pc == '?') {
            p++; n++; continue;
        } else if (pc == '*') {
            star  = ++p;
            retry = n;
            continue;
        } else if (pc == '[') {
            const char *q = p + 1;
            int neg = 0;
            int hit = 0;

            if (*q == '!' || *q == '^') { neg = 1; q++; }
            for (; *q != '\0' && (*q != ']' || q == p + 1 + neg); q++) {
                char lo = fold ? p_lower(*q) : *q;
                if (q[1] == '-' && q[2] != '\0' && q[2] != ']') {
                    char hi = fold ? p_lower(q[2]) : q[2];
                    if (nc >= lo && nc <= hi) hit = 1;
                    q += 2;
                } else if (lo == nc) {
                    hit = 1;
                }
            }
            if (*q == ']' && (hit != neg)) { p = q + 1; n++; continue; }
        } else if (pc != '\0' && (fold ? p_lower(pc) : pc) == nc) {
            p++; n++; continue;
        }

        if (star != NULL) {
            p = star;
            n = ++retry;
            continue;
        }
        return 0;
    }

    while (*p == '*')
        p++;
    return (*p == '\0');
}
