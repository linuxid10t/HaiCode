/* test_sock.c - TCP client sockets against real listeners.
 *
 *     usage: test_sock <host> <echo> <silent> <eof> <closed>
 *
 * Exercises the same source that ships to OS/2; only the six calls inside the
 * platform block at the top of sock.c differ there.
 */

#include "../sock.h"
#include "../http.h"
#include "../buf.h"
#include "tap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_host;

static void
test_startup(void)
{
    OK(sock_startup() == 0, "stack initialises");
    OK(sock_startup() == 0, "and is safe to initialise twice");
}

static void
test_bad_arguments(void)
{
    sock s;

    sock_init(&s);
    OK(!sock_is_open(&s), "a fresh socket is closed");

    OK(sock_connect(&s, NULL, 80) != 0, "NULL host rejected");
    OK(sock_connect(&s, "", 80) != 0, "empty host rejected");
    OK(sock_connect(&s, g_host, 0) != 0, "port 0 rejected");
    OK(sock_connect(&s, g_host, 70000) != 0, "port above 65535 rejected");
    OK(s.err[0] != '\0', "and an error message is set");
    OK(!sock_is_open(&s), "nothing was left open");
    sock_close(&s);
}

static void
test_resolve_failure(void)
{
    sock s;

    sock_init(&s);
    OK(sock_connect(&s, "no-such-host.invalid", 80) != 0,
       "unresolvable host fails");
    OK(strstr(s.err, "resolve") != NULL, "and says so");
    sock_close(&s);
}

static void
test_connection_refused(int port)
{
    sock s;

    sock_init(&s);
    OK(sock_connect(&s, g_host, port) != 0, "closed port refuses");
    OK(s.err[0] != '\0', "with an error message");
    OK(!sock_is_open(&s), "and leaves nothing open");
    sock_close(&s);
}

static void
test_roundtrip(int port)
{
    sock  s;
    char  in[64];
    int   got;
    int   total = 0;

    sock_init(&s);
    OK(sock_connect(&s, g_host, port) == 0, "connects to the echo server");
    OK(sock_is_open(&s), "socket reports open");

    OK(sock_send_all(&s, "hello", 5) == 0, "send succeeds");

    while (total < 5) {
        got = sock_recv_some(&s, in + total, sizeof(in) - (size_t)total);
        if (got <= 0)
            break;
        total += got;
    }
    in[total > 0 ? total : 0] = '\0';
    EQSTR(in, "hello", "bytes come back intact");

    sock_close(&s);
    OK(!sock_is_open(&s), "closed after use");
    OK(sock_recv_some(&s, in, sizeof(in)) < 0, "recv on a closed socket fails");
    OK(sock_send_all(&s, "x", 1) != 0, "send on a closed socket fails");
}

/* A large write must not be lost to a partial send(). */
static void
test_large_send(int port)
{
    sock   s;
    buf    payload;
    buf    back;
    char   chunk[4096];
    int    i;
    int    got;

    buf_init(&payload);
    buf_init(&back);
    for (i = 0; i < 200000; i++)
        buf_putc(&payload, (char)('a' + (i % 26)));

    sock_init(&s);
    OK(sock_connect(&s, g_host, port) == 0, "connects for the large transfer");
    OK(sock_send_all(&s, payload.data, payload.len) == 0,
       "200 KB sent in full despite partial writes");

    while (back.len < payload.len) {
        got = sock_recv_some(&s, chunk, sizeof(chunk));
        if (got <= 0)
            break;
        buf_append(&back, chunk, (size_t)got);
    }
    EQLONG((long)back.len, (long)payload.len, "all of it came back");
    OK(back.len == payload.len &&
       memcmp(back.data, payload.data, payload.len) == 0,
       "and the bytes are identical");

    sock_close(&s);
    buf_free(&payload);
    buf_free(&back);
}

/*
 * A server that accepts and then says nothing must not hang the caller
 * forever: on a GUI worker thread that would freeze the Stop button too.
 */
static void
test_read_timeout(int port)
{
    sock s;
    char in[16];
    int  got;

    sock_init(&s);
    OK(sock_connect(&s, g_host, port) == 0, "connects to the silent server");
    sock_set_timeout(&s, 300);

    got = sock_recv_some(&s, in, sizeof(in));
    OK(got < 0, "a silent server times out rather than hanging");
    OK(strstr(s.err, "timed out") != NULL, "and reports a timeout");
    sock_close(&s);
}

/* A clean close must read as EOF, not as an error: http_pump relies on the
 * distinction to terminate an EOF-delimited body. */
static void
test_clean_eof(int port)
{
    sock s;
    char in[16];

    sock_init(&s);
    OK(sock_connect(&s, g_host, port) == 0, "connects to the closing server");
    sock_set_timeout(&s, 2000);
    EQLONG(sock_recv_some(&s, in, sizeof(in)), 0,
           "a clean close reads as 0, not as an error");
    sock_close(&s);
}

static void
test_transport_wiring(int port)
{
    sock           s;
    http_transport t;
    char           in[16];
    int            total = 0;
    int            got;

    sock_init(&s);
    sock_connect(&s, g_host, port);
    t = sock_transport(&s);

    OK(t.xsend != NULL && t.xrecv != NULL, "transport is populated");
    OK(t.xsend(t.ctx, "ping", 4) == 0, "transport send works");
    while (total < 4) {
        got = t.xrecv(t.ctx, in + total, sizeof(in) - (size_t)total);
        if (got <= 0) break;
        total += got;
    }
    in[total > 0 ? total : 0] = '\0';
    EQSTR(in, "ping", "transport recv works");
    sock_close(&s);
}

static void
test_loop_net(int echo_port, int closed_port)
{
    sock_net       n;
    loop_net       ln;
    http_transport t;

    sock_net_init(&n, g_host, echo_port, 2000);
    ln = sock_net_loop(&n);
    OK(ln.open != NULL && ln.close != NULL, "loop_net populated");
    OK(ln.open(ln.ctx, &t) == 0, "loop_net opens");
    OK(t.xsend(t.ctx, "z", 1) == 0, "and yields a working transport");
    ln.close(ln.ctx);
    OK(!sock_is_open(&n.s), "loop_net closes");

    /* Reopening after a close is what the loop does every step. */
    OK(ln.open(ln.ctx, &t) == 0, "reopens for the next step");
    ln.close(ln.ctx);

    sock_net_init(&n, g_host, closed_port, 2000);
    ln = sock_net_loop(&n);
    OK(ln.open(ln.ctx, &t) != 0, "loop_net reports a failed connection");
}

int
main(int argc, char **argv)
{
    int echo, silent, eof, closed;

    if (argc < 6) {
        printf("test_sock: skipped (no servers given)\n");
        return 0;
    }
    g_host = argv[1];
    echo   = atoi(argv[2]);
    silent = atoi(argv[3]);
    eof    = atoi(argv[4]);
    closed = atoi(argv[5]);

    test_startup();
    test_bad_arguments();
    test_resolve_failure();
    test_connection_refused(closed);
    test_roundtrip(echo);
    test_large_send(echo);
    test_read_timeout(silent);
    test_clean_eof(eof);
    test_transport_wiring(echo);
    test_loop_net(echo, closed);

    TAP_REPORT("test_sock");
}
