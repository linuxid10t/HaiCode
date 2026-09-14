/* buf.c - growable byte buffer. C89. */

#include "buf.h"

#include <stdlib.h>
#include <string.h>

#define BUF_MIN_CAP 64

void
buf_init(buf *b)
{
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
    b->oom  = 0;
}

void
buf_free(buf *b)
{
    if (b->data != NULL)
        free(b->data);
    buf_init(b);
}

void
buf_clear(buf *b)
{
    b->len = 0;
}

/* Ensure room for `extra` more bytes plus one spare for a NUL terminator. */
static int
buf_reserve(buf *b, size_t extra)
{
    size_t  need;
    size_t  cap;
    char   *p;

    if (b->oom)
        return -1;

    need = b->len + extra + 1;
    if (need <= b->cap)
        return 0;

    cap = (b->cap != 0) ? b->cap : BUF_MIN_CAP;
    while (cap < need) {
        /* Guard against size_t overflow on absurd growth. */
        if (cap > (size_t)-1 / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }

    p = (char *)realloc(b->data, cap);
    if (p == NULL) {
        b->oom = 1;
        return -1;
    }
    b->data = p;
    b->cap  = cap;
    return 0;
}

int
buf_append(buf *b, const char *p, size_t n)
{
    if (n == 0)
        return b->oom ? -1 : 0;
    if (buf_reserve(b, n) != 0)
        return -1;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 0;
}

int
buf_putc(buf *b, char c)
{
    if (buf_reserve(b, 1) != 0)
        return -1;
    b->data[b->len++] = c;
    return 0;
}

int
buf_puts(buf *b, const char *s)
{
    return buf_append(b, s, strlen(s));
}

char *
buf_cstr(buf *b)
{
    if (buf_reserve(b, 0) != 0)
        return NULL;
    b->data[b->len] = '\0';
    return b->data;
}
