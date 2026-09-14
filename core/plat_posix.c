/* plat_posix.c - host implementation of plat.h. C89 + POSIX.
 *
 * Built on the development machine so the rest of core/ can be exercised
 * end to end. The target uses plat_os2.c instead.
 */

#include "plat.h"
#include "buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

int
plat_stat(const char *path, plat_info *out)
{
    struct stat st;

    out->exists = 0;
    out->is_dir = 0;
    out->size   = 0;
    out->mtime  = 0;

    if (path == NULL || *path == '\0')
        return -1;
    if (stat(path, &st) != 0)
        return 0;              /* absent is not an error */

    out->exists = 1;
    out->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    out->size   = (long)st.st_size;
    out->mtime  = (long)st.st_mtime;
    return 0;
}

int
plat_mkdir(const char *path)
{
    plat_info info;

    if (mkdir(path, 0700) == 0)
        return 0;
    /* Already present and a directory counts as success. */
    if (plat_stat(path, &info) == 0 && info.exists && info.is_dir)
        return 0;
    return -1;
}

int
plat_mkdir_p(const char *path)
{
    buf    work;
    size_t i;
    int    rc = 0;

    if (path == NULL || *path == '\0')
        return -1;

    buf_init(&work);
    if (buf_puts(&work, path) != 0 || buf_cstr(&work) == NULL) {
        buf_free(&work);
        return -1;
    }

    for (i = 0; i < work.len; i++) {
        if (work.data[i] != '/' || i == 0)
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
    return (remove(path) == 0) ? 0 : -1;
}

int
plat_rename(const char *from, const char *to)
{
    return (rename(from, to) == 0) ? 0 : -1;
}

struct plat_dir {
    DIR  *d;
    char  path[1024];
    char  name[512];
};

plat_dir *
plat_opendir(const char *path)
{
    plat_dir *pd;
    DIR      *d;

    d = opendir(path);
    if (d == NULL)
        return NULL;

    pd = (plat_dir *)malloc(sizeof(plat_dir));
    if (pd == NULL) {
        closedir(d);
        return NULL;
    }
    pd->d = d;
    strncpy(pd->path, path, sizeof(pd->path) - 1);
    pd->path[sizeof(pd->path) - 1] = '\0';
    pd->name[0] = '\0';
    return pd;
}

const char *
plat_readdir(plat_dir *d, int *is_dir)
{
    struct dirent *e;
    buf            full;
    plat_info      info;

    for (;;) {
        e = readdir(d->d);
        if (e == NULL)
            return NULL;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        break;
    }

    strncpy(d->name, e->d_name, sizeof(d->name) - 1);
    d->name[sizeof(d->name) - 1] = '\0';

    if (is_dir != NULL) {
        buf_init(&full);
        buf_puts(&full, d->path);
        buf_putc(&full, '/');
        buf_puts(&full, d->name);
        *is_dir = 0;
        if (buf_cstr(&full) != NULL && plat_stat(full.data, &info) == 0)
            *is_dir = info.is_dir;
        buf_free(&full);
    }
    return d->name;
}

void
plat_closedir(plat_dir *d)
{
    if (d == NULL)
        return;
    closedir(d->d);
    free(d);
}

int
plat_run(const char *cmd, const char *workdir, long timeout_s,
         buf *out, long max_out)
{
    buf   full;
    FILE *pipe;
    char  chunk[1024];
    long  total = 0;
    int   status;

    if (cmd == NULL || *cmd == '\0')
        return -1;

    buf_init(&full);
    if (timeout_s > 0) {
        char t[32];
        sprintf(t, "timeout %ld ", timeout_s);
        buf_puts(&full, t);
    }
    buf_puts(&full, "sh -c '");
    if (workdir != NULL && *workdir != '\0') {
        /* Single-quote the directory so spaces and shell metacharacters in it
         * cannot break out; an embedded quote is escaped the POSIX way. */
        const char *p;
        buf_puts(&full, "cd \"");
        for (p = workdir; *p != '\0'; p++) {
            if (*p == '"' || *p == '\\' || *p == '$' || *p == '`')
                buf_putc(&full, '\\');
            buf_putc(&full, *p);
        }
        buf_puts(&full, "\" && ");
    }
    /* The command itself is inside single quotes; escape any it contains. */
    {
        const char *p;
        for (p = cmd; *p != '\0'; p++) {
            if (*p == '\'')
                buf_puts(&full, "'\\''");
            else
                buf_putc(&full, *p);
        }
    }
    buf_puts(&full, "' 2>&1");

    if (full.oom || buf_cstr(&full) == NULL) {
        buf_free(&full);
        return -1;
    }

    pipe = popen(full.data, "r");
    buf_free(&full);
    if (pipe == NULL)
        return -1;

    while (fgets(chunk, (int)sizeof(chunk), pipe) != NULL) {
        size_t n = strlen(chunk);
        if (max_out > 0 && total + (long)n > max_out) {
            n = (size_t)(max_out - total);
            if (n > 0)
                buf_append(out, chunk, n);
            total = max_out;
            break;
        }
        buf_append(out, chunk, n);
        total += (long)n;
    }

    status = pclose(pipe);
    if (status == -1)
        return -1;
    /* WEXITSTATUS without <sys/wait.h> pulling in more than we need. */
    return (status >> 8) & 0xFF;
}

const char *
plat_settings_dir(void)
{
    static char dir[1024];
    const char *home;

    if (dir[0] != '\0')
        return dir;

    home = getenv("HAIOS2_HOME");
    if (home == NULL || *home == '\0')
        home = getenv("HOME");
    if (home == NULL || *home == '\0')
        return NULL;

    sprintf(dir, "%.900s/.haios2", home);
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
    return (long)(clock() / (CLOCKS_PER_SEC / 1000));
}
