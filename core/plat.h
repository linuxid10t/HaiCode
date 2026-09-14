/* plat.h - the whole operating-system surface, in one header.
 *
 * C89. Everything else in core/ is OS-agnostic; this is the seam. There are
 * two implementations:
 *
 *   plat_posix.c  built and tested on the development host
 *   plat_os2.c    built with VisualAge / Borland / Watcom on the target
 *
 * Keeping the surface this small is the point: when the OS/2 build misbehaves,
 * there are only a dozen functions it can be.
 */
#ifndef HAIOS2_PLAT_H
#define HAIOS2_PLAT_H

#include <stddef.h>
#include "buf.h"

typedef struct plat_info {
    int  exists;
    int  is_dir;
    long size;
    long mtime;      /* seconds since epoch, 0 if unknown */
} plat_info;

int  plat_stat(const char *path, plat_info *out);
int  plat_mkdir(const char *path);      /* 0 on success or already-present */
int  plat_mkdir_p(const char *path);    /* creates intermediate components */
int  plat_remove(const char *path);
int  plat_rename(const char *from, const char *to);

/* Directory enumeration. plat_readdir returns NULL when exhausted; the name it
 * returns is valid until the next call. "." and ".." are never returned. */
typedef struct plat_dir plat_dir;
plat_dir   *plat_opendir(const char *path);
const char *plat_readdir(plat_dir *d, int *is_dir);
void        plat_closedir(plat_dir *d);

/*
 * Runs `cmd` through the system shell with `workdir` as the current directory,
 * appending combined stdout+stderr to `out` up to `max_out` bytes.
 *
 * Returns the command's exit status, or -1 if it could not be started. The
 * engine thread blocks for the duration, so callers must keep commands short.
 * timeout_s is advisory: POSIX honours it via timeout(1) when available, OS/2
 * ignores it, so do not rely on it to bound anything that matters.
 */
int plat_run(const char *cmd, const char *workdir, long timeout_s,
             buf *out, long max_out);

/* Per-user settings directory, created if absent. NULL if it cannot be
 * determined. The returned string is owned by plat and stays valid. */
const char *plat_settings_dir(void);

/* Seconds since the epoch. */
long plat_time(void);

/* Milliseconds of monotonic-ish elapsed time, for measuring, not for dates. */
long plat_ticks(void);

#endif
