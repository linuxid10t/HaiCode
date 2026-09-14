/* tap.h - trivial test harness. C89. */
#ifndef HAIOS2_TAP_H
#define HAIOS2_TAP_H

#include <stdio.h>
#include <string.h>

static int tap_pass = 0;
static int tap_fail = 0;

#define OK(cond, what)                                                    \
    do {                                                                  \
        if (cond) {                                                       \
            tap_pass++;                                                   \
        } else {                                                          \
            tap_fail++;                                                   \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, (what));     \
        }                                                                 \
    } while (0)

#define EQSTR(got, want, what)                                            \
    do {                                                                  \
        const char *g_ = (got);                                           \
        const char *w_ = (want);                                          \
        if (g_ != NULL && strcmp(g_, w_) == 0) {                          \
            tap_pass++;                                                   \
        } else {                                                          \
            tap_fail++;                                                   \
            printf("  FAIL %s:%d  %s\n       got: %s\n      want: %s\n",  \
                   __FILE__, __LINE__, (what),                            \
                   (g_ != NULL) ? g_ : "(null)", w_);                     \
        }                                                                 \
    } while (0)

#define EQLONG(got, want, what)                                           \
    do {                                                                  \
        long g_ = (long)(got);                                            \
        long w_ = (long)(want);                                           \
        if (g_ == w_) {                                                   \
            tap_pass++;                                                   \
        } else {                                                          \
            tap_fail++;                                                   \
            printf("  FAIL %s:%d  %s (got %ld, want %ld)\n",              \
                   __FILE__, __LINE__, (what), g_, w_);                   \
        }                                                                 \
    } while (0)

#define TAP_REPORT(name)                                                  \
    do {                                                                  \
        printf("%s: %d passed, %d failed\n", (name), tap_pass, tap_fail); \
        return (tap_fail == 0) ? 0 : 1;                                   \
    } while (0)

#endif
