/* sock.h - TCP client sockets, and the loop_net that sits on top. C89.
 *
 * One implementation covers POSIX and OS/2. That is deliberate: the two
 * platforms differ in about six places, and isolating those behind macros
 * means the code path the host tests exercise is the code path that ships,
 * rather than an OS/2 twin that nobody has ever run.
 *
 * The six differences, all at the top of sock.c:
 *
 *   1. sock_init() must be called once before anything else on OS/2.
 *   2. Socket handles are NOT file descriptors there: read/write/close do not
 *      work on them. Use recv/send and soclose().
 *   3. errno is not set; the error code comes from sock_errno().
 *   4. select() takes an entirely different argument list.
 *   5. There is no getaddrinfo, so name resolution goes through
 *      gethostbyname() -- which is also used on POSIX, on purpose, so both
 *      platforms run the same resolution code. IPv4 only; OS/2's stack is
 *      IPv4 anyway.
 *   6. Link tcpip32.lib, and the machine needs the 32-bit stack
 *      (so32dll.dll / tcp32dll.dll), not Warp 3's original 16-bit IAK.
 *
 * gethostbyname() is not re-entrant. Only the engine worker thread may call
 * into this file; the UI thread must not.
 */
#ifndef HAIOS2_SOCK_H
#define HAIOS2_SOCK_H

#include <stddef.h>
#include "http.h"
#include "loop.h"

#define SOCK_ERRLEN 128

typedef struct sock {
    int  fd;                     /* -1 when closed */
    long timeout_ms;             /* 0 disables the wait; see sock_set_timeout */
    char err[SOCK_ERRLEN];
} sock;

/* Call once at startup. Returns 0 on success. Harmless to call twice. */
int  sock_startup(void);

void sock_init(sock *s);
/* Resolves and connects. Returns 0, or -1 with s->err set. */
int  sock_connect(sock *s, const char *host, int port);
void sock_close(sock *s);
int  sock_is_open(const sock *s);

/* Bounds how long a single read waits. 0 means block indefinitely, which on a
 * GUI worker thread means a hung server hangs the Stop button too. Default is
 * 60s: long enough for a slow first token, short enough to recover. */
void sock_set_timeout(sock *s, long ms);

/* Sends everything or fails. Returns 0 or -1. */
int  sock_send_all(void *s, const char *p, size_t n);
/* Returns bytes read, 0 on clean EOF, negative on error or timeout. */
int  sock_recv_some(void *s, char *p, size_t n);

/* An http_transport backed by this socket. */
http_transport sock_transport(sock *s);

/* ------------------------------------------------------------- loop_net -- */

/*
 * Connects once per step and closes afterwards. No keep-alive: on a LAN the
 * handshake is lost in the noise next to inference time, and a reused socket
 * that the server has quietly dropped needs retry-on-first-write logic that is
 * easy to get subtly wrong. Simplicity wins here.
 */
typedef struct sock_net {
    char host[128];
    int  port;
    long timeout_ms;
    sock s;
} sock_net;

void     sock_net_init(sock_net *n, const char *host, int port, long timeout_ms);
loop_net sock_net_loop(sock_net *n);

#endif
