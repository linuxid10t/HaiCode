/* path.h - path normalisation and containment. C89.
 *
 * This file is a security boundary, not a convenience. The permission gate
 * lets read-only tools run unprompted when their target is inside the project
 * directory, and path_within() is what decides that. A string prefix compare
 * is not good enough:
 *
 *   - OS/2 filesystems are case-insensitive, so C:\PROJ and c:\proj are the
 *     same directory and must compare equal there (and must NOT on POSIX,
 *     where they are different directories).
 *   - "/proj/../etc/passwd" must not count as inside "/proj".
 *   - "/project2/x" must not count as inside "/proj".
 *   - Drive-relative ("C:foo") and UNC ("\\\\srv\\share") paths are rejected
 *     outright: they resolve against hidden state, and a coding agent has no
 *     reason to use them.
 *
 * Normalised form is forward-slash separated, with no trailing slash (except a
 * bare root), no "." components, and ".." resolved textually.
 *
 * Case is PRESERVED, because the result is used to actually open files and on
 * POSIX case is significant. Only the drive letter is lowercased, since a
 * drive letter is case-insensitive on every system that has one. Case-blind
 * comparison happens in path_within_ex(), not here.
 *
 * Symlinks are NOT resolved -- on OS/2 that is nearly moot, but on POSIX a
 * caller that cares must realpath() first.
 */
#ifndef HAIOS2_PATH_H
#define HAIOS2_PATH_H

#include <stddef.h>
#include "buf.h"

/* Case folding follows the target filesystem. Override at build time. */
#ifndef PATH_FOLD_CASE
#  if defined(__OS2__) || defined(OS2) || defined(_WIN32)
#    define PATH_FOLD_CASE 1
#  else
#    define PATH_FOLD_CASE 0
#  endif
#endif

/* 1 if `p` is rooted: "/x", "C:/x" or "C:\x". Drive-relative "C:x" is not. */
int path_is_abs(const char *p);

/* 1 for forms we refuse to interpret: drive-relative and UNC. */
int path_is_rejected(const char *p);

/* Writes the normalised form of `p` into `out` (cleared first).
 * Returns 0, or -1 if `p` is rejected or escapes above its own root. */
int path_normalize(buf *out, const char *p);

/* Resolves `rel` against `base` and normalises. An absolute `rel` replaces
 * `base` entirely. Returns 0 or -1. */
int path_resolve(buf *out, const char *base, const char *rel);

/* 1 when `cand` is `root` or lies beneath it. Both are normalised first; a
 * rejected or unnormalisable path is never within anything. */
int path_within(const char *root, const char *cand);

/* Same, with explicit case folding, so both behaviours can be tested on one
 * host. `fold` non-zero compares case-insensitively. */
int path_within_ex(const char *root, const char *cand, int fold);

/* Trailing component of a normalised path ("" for a root). */
const char *path_basename(const char *p);

/* 1 when `name` matches the glob `pat`: * ? [abc] [a-z] [!abc], and \ escapes.
 * Iterative with backtracking, so a pathological pattern cannot blow up. */
int path_glob(const char *pat, const char *name, int fold);

#endif
