/* buf.h - growable byte buffer.
 *
 * C89. No dependencies beyond <stddef.h>.
 *
 * Allocation failure is sticky: once `oom` is set every further append is a
 * no-op and the flag stays set, so callers may chain many appends and check
 * once at the end instead of testing every call.
 */
#ifndef HAIOS2_BUF_H
#define HAIOS2_BUF_H

#include <stddef.h>

typedef struct buf {
    char   *data;
    size_t  len;
    size_t  cap;
    int     oom;
} buf;

void  buf_init(buf *b);
void  buf_free(buf *b);
void  buf_clear(buf *b);          /* keeps the allocation, resets len */
int   buf_append(buf *b, const char *p, size_t n);
int   buf_putc(buf *b, char c);
int   buf_puts(buf *b, const char *s);
char *buf_cstr(buf *b);           /* NUL-terminates in place; NULL on oom */

#endif
