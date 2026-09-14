/* sock.c - TCP client sockets. C89. POSIX and OS/2 from one source. */

#include "sock.h"
#include "http.h"

#include <stdio.h>
#include <string.h>

/* ===================================================== platform block ==== */

#if defined(__OS2__) || defined(OS2) || defined(__EMX__)

/* ---- OS/2 -------------------------------------------------------------
 * UNTESTED. No OS/2 toolchain was available; the POSIX half of this file is
 * exercised by the test suite, this half is not. The calls are from the
 * TCP/IP Programming Toolkit and should be right, but expect to fix an
 * include path or a cast.
 *
 * BSD4.4 gets the sockaddr_in layout the toolkit headers expect.
 */
#define BSD_SELECT
#include <types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/select.h>
#include <nerrno.h>

#define SOCK_STARTUP()      sock_init()
#define SOCK_CLOSE(fd)      soclose(fd)
#define SOCK_ERRNO()        sock_errno()
#define SOCK_INVALID        (-1)

/*
 * OS/2's native select() is not BSD's:
 *
 *     int select(int *sockets, int nread, int nwrite, int nexcept, long ms);
 *
 * It takes a flat array of handles rather than fd_sets, and its timeout is a
 * plain millisecond count. Defining BSD_SELECT above asks the toolkit headers
 * for the BSD-compatible form instead; if that turns out not to be available
 * in the installed toolkit, delete the define and switch this function to the
 * native call -- the rest of the file does not care which is used.
 */
static int
sock_wait_readable(int fd, long ms)
{
    int    socks[1];
    int    rc;

    socks[0] = fd;
    rc = select(socks, 1, 0, 0, ms);
    if (rc > 0)  return 1;
    if (rc == 0) return 0;      /* timed out */
    return -1;
}

#else

/* ---- POSIX ------------------------------------------------------------ */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

#define SOCK_STARTUP()      0
#define SOCK_CLOSE(fd)      close(fd)
#define SOCK_ERRNO()        errno
#define SOCK_INVALID        (-1)

static int
sock_wait_readable(int fd, long ms)
{
    fd_set         rd;
    struct timeval tv;
    int            rc;

    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;

    rc = select(fd + 1, &rd, NULL, NULL, &tv);
    if (rc > 0)  return 1;
    if (rc == 0) return 0;
    return -1;
}

#endif

/* ========================================================== common ======= */

#define SOCK_DEFAULT_TIMEOUT_MS 60000L

int
sock_startup(void)
{
    static int done = 0;
    int        rc;

    if (done)
        return 0;
    rc = SOCK_STARTUP();
    if (rc != 0)
        return -1;
    done = 1;
    return 0;
}

void
sock_init(sock *s)
{
    s->fd         = SOCK_INVALID;
    s->timeout_ms = SOCK_DEFAULT_TIMEOUT_MS;
    s->err[0]     = '\0';
}

int
sock_is_open(const sock *s)
{
    return (s->fd != SOCK_INVALID);
}

void
sock_set_timeout(sock *s, long ms)
{
    s->timeout_ms = (ms > 0) ? ms : 0;
}

static void
sock_seterr(sock *s, const char *what, int code)
{
    char num[32];

    sprintf(num, "%d", code);
    s->err[0] = '\0';
    strncpy(s->err, what, SOCK_ERRLEN - 1);
    s->err[SOCK_ERRLEN - 1] = '\0';
    if (code != 0 && strlen(s->err) + strlen(num) + 3 < SOCK_ERRLEN) {
        strcat(s->err, " (");
        strcat(s->err, num);
        strcat(s->err, ")");
    }
}

void
sock_close(sock *s)
{
    if (s->fd != SOCK_INVALID) {
        SOCK_CLOSE(s->fd);
        s->fd = SOCK_INVALID;
    }
}

/*
 * Resolves and connects.
 *
 * gethostbyname() rather than getaddrinfo(): OS/2 has no getaddrinfo, and
 * using the older call on both platforms keeps one tested code path instead of
 * two. A dotted-quad address resolves through it fine on every stack, so there
 * is no separate inet_addr path to get wrong.
 */
int
sock_connect(sock *s, const char *host, int port)
{
    struct hostent    *he;
    struct sockaddr_in addr;
    int                fd;

    sock_close(s);
    s->err[0] = '\0';

    if (host == NULL || *host == '\0' || port <= 0 || port > 65535) {
        sock_seterr(s, "bad host or port", 0);
        return -1;
    }
    if (sock_startup() != 0) {
        sock_seterr(s, "TCP/IP stack unavailable", 0);
        return -1;
    }

    he = gethostbyname(host);
    if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL) {
        sock_seterr(s, "cannot resolve host", 0);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == SOCK_INVALID) {
        sock_seterr(s, "cannot create socket", SOCK_ERRNO());
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        sock_seterr(s, "connection refused", SOCK_ERRNO());
        SOCK_CLOSE(fd);
        return -1;
    }

    s->fd = fd;
    return 0;
}

int
sock_send_all(void *sv, const char *p, size_t n)
{
    sock  *s = (sock *)sv;
    size_t off = 0;

    if (s->fd == SOCK_INVALID)
        return -1;

    while (off < n) {
        int sent = send(s->fd, (char *)(p + off), (int)(n - off), 0);
        if (sent <= 0) {
            /* A partial write followed by an error is still an error; the
             * caller must not assume anything reached the server. */
            sock_seterr(s, "send failed", SOCK_ERRNO());
            return -1;
        }
        off += (size_t)sent;
    }
    return 0;
}

int
sock_recv_some(void *sv, char *p, size_t n)
{
    sock *s = (sock *)sv;
    int   got;

    if (s->fd == SOCK_INVALID)
        return -1;

    if (s->timeout_ms > 0) {
        int ready = sock_wait_readable(s->fd, s->timeout_ms);
        if (ready == 0) {
            sock_seterr(s, "timed out waiting for the server", 0);
            return -1;
        }
        if (ready < 0) {
            sock_seterr(s, "select failed", SOCK_ERRNO());
            return -1;
        }
    }

    got = recv(s->fd, p, (int)n, 0);
    if (got < 0) {
        sock_seterr(s, "recv failed", SOCK_ERRNO());
        return -1;
    }
    return got;      /* 0 is a clean close, which http_pump handles */
}

http_transport
sock_transport(sock *s)
{
    http_transport t;
    t.ctx   = s;
    t.xsend = sock_send_all;
    t.xrecv = sock_recv_some;
    return t;
}

/* ------------------------------------------------------------- loop_net -- */

void
sock_net_init(sock_net *n, const char *host, int port, long timeout_ms)
{
    n->host[0] = '\0';
    if (host != NULL) {
        strncpy(n->host, host, sizeof(n->host) - 1);
        n->host[sizeof(n->host) - 1] = '\0';
    }
    n->port       = port;
    n->timeout_ms = (timeout_ms > 0) ? timeout_ms : SOCK_DEFAULT_TIMEOUT_MS;
    sock_init(&n->s);
}

static int
sock_net_open(void *ctx, http_transport *t)
{
    sock_net *n = (sock_net *)ctx;

    if (sock_connect(&n->s, n->host, n->port) != 0)
        return -1;
    sock_set_timeout(&n->s, n->timeout_ms);
    *t = sock_transport(&n->s);
    return 0;
}

static void
sock_net_close(void *ctx)
{
    sock_close(&((sock_net *)ctx)->s);
}

loop_net
sock_net_loop(sock_net *n)
{
    loop_net ln;
    ln.ctx   = n;
    ln.open  = sock_net_open;
    ln.close = sock_net_close;
    return ln;
}
