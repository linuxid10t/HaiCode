/* plat_os2.c - OS/2 implementation of plat.h.
 *
 * ============================ NOT YET COMPILED ============================
 * Written against the OS/2 Control Program API, but never built or run: no
 * OS/2 toolchain was available. Treat it as a detailed starting point, not as
 * working code. Expect to fix header names, PSZ casts and at least one
 * DosFindFirst argument. Everything else in core/ is tested; this file is not.
 * ==========================================================================
 *
 * Uses only the Dos* Control Program calls plus ANSI C, so it should build
 * with VisualAge C++ 3.0, Borland C++ 2.0 for OS/2 and Open Watcom alike --
 * none of them agree on what the POSIX-ish layer provides, but all three ship
 * <os2.h> and all three have system().
 *
 * Notes that cost time if you rediscover them:
 *   - INCL_DOSFILEMGR / INCL_DOSMISC must be defined BEFORE <os2.h>.
 *   - DosFindFirst wants a pointer to an HDIR initialised to HDIR_CREATE.
 *   - A path ending in a backslash makes DosQueryPathInfo fail; strip it.
 *   - DosMove will not overwrite an existing target, so unlink first. The
 *     portable code already does, but do not "simplify" that away.
 */

#define INCL_DOSFILEMGR
#define INCL_DOSMISC
#define INCL_DOSPROCESS
#define INCL_DOSERRORS
#include <os2.h>

#include "plat.h"
#include "buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* OS/2 accepts forward slashes in most APIs, but not all of them, so convert
 * to backslashes on the way out. The portable half always works in slashes. */
static void
to_os2(const char *in, char *out, size_t n)
{
    size_t i;

    for (i = 0; i + 1 < n && in[i] != '\0'; i++)
        out[i] = (in[i] == '/') ? '\\' : in[i];
    out[i] = '\0';
    /* A trailing separator makes DosQueryPathInfo fail on a directory. */
    while (i > 1 && out[i - 1] == '\\' && !(i == 3 && out[1] == ':'))
        out[--i] = '\0';
}

int
plat_stat(const char *path, plat_info *out)
{
    FILESTATUS3 fs;
    char        p[CCHMAXPATH];
    APIRET      rc;

    out->exists = 0;
    out->is_dir = 0;
    out->size   = 0;
    out->mtime  = 0;

    if (path == NULL || *path == '\0')
        return -1;
    to_os2(path, p, sizeof(p));

    rc = DosQueryPathInfo((PSZ)p, FIL_STANDARD, &fs, sizeof(fs));
    if (rc != NO_ERROR)
        return 0;                 /* absent is not an error */

    out->exists = 1;
    out->is_dir = (fs.attrFile & FILE_DIRECTORY) ? 1 : 0;
    out->size   = (long)fs.cbFile;

    /* FDATE/FTIME are DOS-packed; convert via mktime rather than by hand. */
    {
        struct tm t;
        memset(&t, 0, sizeof(t));
        t.tm_year = fs.fdateLastWrite.year + 80;   /* epoch 1980 */
        t.tm_mon  = fs.fdateLastWrite.month - 1;
        t.tm_mday = fs.fdateLastWrite.day;
        t.tm_hour = fs.ftimeLastWrite.hours;
        t.tm_min  = fs.ftimeLastWrite.minutes;
        t.tm_sec  = fs.ftimeLastWrite.twosecs * 2;
        t.tm_isdst = -1;
        out->mtime = (long)mktime(&t);
    }
    return 0;
}

int
plat_mkdir(const char *path)
{
    char      p[CCHMAXPATH];
    plat_info info;
    APIRET    rc;

    to_os2(path, p, sizeof(p));
    rc = DosCreateDir((PSZ)p, NULL);
    if (rc == NO_ERROR)
        return 0;
    if (plat_stat(path, &info) == 0 && info.exists && info.is_dir)
        return 0;
    return -1;
}

int
plat_mkdir_p(const char *path)
{
    buf    work;
    size_t i;
    size_t start = 0;
    int    rc;

    if (path == NULL || *path == '\0')
        return -1;

    buf_init(&work);
    if (buf_puts(&work, path) != 0 || buf_cstr(&work) == NULL) {
        buf_free(&work);
        return -1;
    }

    /* Never try to create "c:" -- skip past a drive prefix. */
    if (work.len >= 2 && work.data[1] == ':')
        start = 2;

    for (i = start; i < work.len; i++) {
        char c = work.data[i];
        if ((c != '/' && c != '\\') || i == start)
            continue;
        work.data[i] = '\0';
        if (plat_mkdir(work.data) != 0) {
            work.data[i] = '/';
            buf_free(&work);
            return -1;
        }
        work.data[i] = '/';
    }
    rc = plat_mkdir(work.data);
    buf_free(&work);
    return rc;
}

int
plat_remove(const char *path)
{
    char      p[CCHMAXPATH];
    plat_info info;

    to_os2(path, p, sizeof(p));
    if (plat_stat(path, &info) == 0 && info.exists && info.is_dir)
        return (DosDeleteDir((PSZ)p) == NO_ERROR) ? 0 : -1;
    return (DosDelete((PSZ)p) == NO_ERROR) ? 0 : -1;
}

int
plat_rename(const char *from, const char *to)
{
    char f[CCHMAXPATH];
    char t[CCHMAXPATH];

    to_os2(from, f, sizeof(f));
    to_os2(to, t, sizeof(t));
    /* DosMove refuses an existing target; callers unlink first. */
    return (DosMove((PSZ)f, (PSZ)t) == NO_ERROR) ? 0 : -1;
}

struct plat_dir {
    HDIR         h;
    FILEFINDBUF3 buf;
    ULONG        count;
    int          first;
    int          done;
    char         name[CCHMAXPATH];
};

plat_dir *
plat_opendir(const char *path)
{
    plat_dir *d;
    char      pat[CCHMAXPATH];
    APIRET    rc;

    d = (plat_dir *)malloc(sizeof(plat_dir));
    if (d == NULL)
        return NULL;

    to_os2(path, pat, sizeof(pat));
    strncat(pat, "\\*", sizeof(pat) - strlen(pat) - 1);

    d->h     = HDIR_CREATE;
    d->count = 1;
    d->first = 1;
    d->done  = 0;

    rc = DosFindFirst((PSZ)pat, &d->h,
                      FILE_NORMAL | FILE_DIRECTORY | FILE_READONLY |
                      FILE_ARCHIVED,
                      &d->buf, sizeof(d->buf), &d->count, FIL_STANDARD);
    if (rc != NO_ERROR) {
        free(d);
        return NULL;
    }
    return d;
}

const char *
plat_readdir(plat_dir *d, int *is_dir)
{
    for (;;) {
        if (d->done)
            return NULL;

        if (!d->first) {
            d->count = 1;
            if (DosFindNext(d->h, &d->buf, sizeof(d->buf), &d->count)
                != NO_ERROR || d->count == 0) {
                d->done = 1;
                return NULL;
            }
        }
        d->first = 0;

        if (strcmp(d->buf.achName, ".") == 0 ||
            strcmp(d->buf.achName, "..") == 0)
            continue;

        strncpy(d->name, d->buf.achName, sizeof(d->name) - 1);
        d->name[sizeof(d->name) - 1] = '\0';
        if (is_dir != NULL)
            *is_dir = (d->buf.attrFile & FILE_DIRECTORY) ? 1 : 0;
        return d->name;
    }
}

void
plat_closedir(plat_dir *d)
{
    if (d == NULL)
        return;
    DosFindClose(d->h);
    free(d);
}

/*
 * Runs a command and captures its output.
 *
 * Redirects to a temporary file and reads it back rather than using popen():
 * VisualAge, Borland and Watcom disagree about whether popen exists and how it
 * behaves, but all three have system(), and cmd.exe's "&" chaining handles the
 * drive-and-directory change that OS/2 needs (cd alone will not switch drives).
 *
 * timeout_s is accepted and ignored: OS/2 has no timeout(1) equivalent, and
 * DosExecPgm with a watchdog thread would be a real piece of work. Do not rely
 * on it to bound anything.
 */
int
plat_run(const char *cmd, const char *workdir, long timeout_s,
         buf *out, long max_out)
{
    buf    line;
    char   tmpdir[CCHMAXPATH];
    char   tmpfile[CCHMAXPATH];
    FILE  *f;
    char   chunk[512];
    size_t n;
    long   total = 0;
    int    status;

    (void)timeout_s;

    if (cmd == NULL || *cmd == '\0')
        return -1;

    if (getenv("TMP") != NULL)
        strncpy(tmpdir, getenv("TMP"), sizeof(tmpdir) - 1);
    else if (getenv("TEMP") != NULL)
        strncpy(tmpdir, getenv("TEMP"), sizeof(tmpdir) - 1);
    else
        strcpy(tmpdir, ".");
    tmpdir[sizeof(tmpdir) - 1] = '\0';

    sprintf(tmpfile, "%.200s\\haiout.tmp", tmpdir);

    buf_init(&line);
    buf_puts(&line, "cmd.exe /C \"");
    if (workdir != NULL && *workdir != '\0') {
        char wd[CCHMAXPATH];
        to_os2(workdir, wd, sizeof(wd));
        if (wd[1] == ':') {
            /* Switch drive first: cd cannot do it on OS/2. */
            char drive[4];
            drive[0] = wd[0];
            drive[1] = ':';
            drive[2] = '\0';
            buf_puts(&line, drive);
            buf_puts(&line, " & ");
        }
        buf_puts(&line, "cd \"");
        buf_puts(&line, wd);
        buf_puts(&line, "\" & ");
    }
    buf_puts(&line, cmd);
    buf_puts(&line, " > \"");
    buf_puts(&line, tmpfile);
    buf_puts(&line, "\" 2>&1\"");

    if (line.oom || buf_cstr(&line) == NULL) {
        buf_free(&line);
        return -1;
    }

    status = system(line.data);
    buf_free(&line);

    f = fopen(tmpfile, "rb");
    if (f != NULL) {
        while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
            if (max_out > 0 && total + (long)n > max_out) {
                buf_append(out, chunk, (size_t)(max_out - total));
                total = max_out;
                break;
            }
            buf_append(out, chunk, n);
            total += (long)n;
        }
        fclose(f);
    }
    remove(tmpfile);

    return status;
}

const char *
plat_settings_dir(void)
{
    static char dir[CCHMAXPATH];
    const char *home;

    if (dir[0] != '\0')
        return dir;

    home = getenv("HAIOS2_HOME");
    if (home == NULL || *home == '\0')
        home = getenv("HOME");
    if (home == NULL || *home == '\0')
        home = getenv("USERPROFILE");
    if (home == NULL || *home == '\0') {
        /* Last resort: alongside the boot drive, which always exists. */
        ULONG boot = 3;
        DosQuerySysInfo(QSV_BOOT_DRIVE, QSV_BOOT_DRIVE, &boot, sizeof(boot));
        sprintf(dir, "%c:\\HAIOS2", (char)('A' + boot - 1));
    } else {
        sprintf(dir, "%.200s\\haios2", home);
    }

    if (plat_mkdir_p(dir) != 0) {
        dir[0] = '\0';
        return NULL;
    }
    return dir;
}

long
plat_time(void)
{
    return (long)time(NULL);
}

long
plat_ticks(void)
{
    ULONG ms = 0;
    DosQuerySysInfo(QSV_MS_COUNT, QSV_MS_COUNT, &ms, sizeof(ms));
    return (long)ms;
}
