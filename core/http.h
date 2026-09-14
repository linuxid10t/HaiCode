/* http.h - minimal HTTP/1.1 client framing.
 *
 * C89. Depends on buf.h. Knows nothing about sockets: all I/O goes through a
 * caller-supplied http_transport, so the same code drives a POSIX socket
 * during development and OS/2's so32dll sockets on the target (where handles
 * are not file descriptors and you must use recv/send/soclose).
 *
 * Requests stream. http_req_body_write has exactly the json_sink signature, so
 * a JSON writer can emit a multi-megabyte body straight onto the wire without
 * it ever existing in memory:
 *
 *     long len = 0;
 *     json_writer w;
 *     json_w_init(&w, json_count_sink, &len);   
 *     emit_request(&w);                         
 *
 *     http_req_begin(&r, t, "POST", "/v1/chat/completions", "10.0.0.2:8080");
 *     http_req_header(&r, "Content-Type", "application/json");
 *     http_req_body_begin(&r, len);
 *     json_w_init(&w, http_req_body_write, &r);  
 *     emit_request(&w);
 *     http_req_body_end(&r);
 *
 * Passing -1 to http_req_body_begin uses chunked transfer instead, which skips
 * the counting pass but requires a server that accepts chunked request bodies.
 * The two-pass form works everywhere; prefer it unless you have measured.
 *
 * Responses are pushed: feed bytes to http_resp_feed and body bytes arrive via
 * a callback, which is what lets an SSE parser sit directly behind it without
 * an intermediate buffer.
 */
#ifndef HAIOS2_HTTP_H
#define HAIOS2_HTTP_H

#include <stddef.h>
#include "buf.h"

#define HTTP_READ_CHUNK 2048

typedef struct http_transport {
    void *ctx;
    /* Send exactly n bytes. Return 0 on success, non-zero on error. */
    int (*xsend)(void *ctx, const char *p, size_t n);
    /* Read up to n bytes. Return count, 0 on clean EOF, negative on error. */
    int (*xrecv)(void *ctx, char *p, size_t n);
} http_transport;

/* ------------------------------------------------------------- request --- */

typedef struct http_req {
    http_transport t;
    buf            head;       /* request line + headers, sent on body_begin */
    int            chunked;
    int            headers_sent;
    int            err;
} http_req;

int  http_req_begin(http_req *r, http_transport t, const char *method,
                    const char *path, const char *host);
int  http_req_header(http_req *r, const char *name, const char *value);
/* content_length >= 0 sets Content-Length; -1 selects chunked encoding. */
int  http_req_body_begin(http_req *r, long content_length);
/* Signature-compatible with json_sink. `r` is an http_req *. */
int  http_req_body_write(void *r, const char *p, size_t n);
int  http_req_body_end(http_req *r);
void http_req_free(http_req *r);

/* ------------------------------------------------------------ response --- */

typedef void (*http_body_cb)(void *ctx, const char *p, size_t n);

enum {
    HTTP_ST_STATUS = 0,
    HTTP_ST_HEADERS,
    HTTP_ST_BODY_LEN,
    HTTP_ST_CHUNK_SIZE,
    HTTP_ST_CHUNK_DATA,
    HTTP_ST_CHUNK_CRLF,
    HTTP_ST_TRAILER,
    HTTP_ST_BODY_EOF,
    HTTP_ST_DONE,
    HTTP_ST_ERROR
};

typedef struct http_resp {
    int           status;          /* e.g. 200 */
    buf           reason;          /* e.g. "OK" */
    buf           headers;         /* "name: value\n" lines, name lowercased */

    int           state;
    buf           line;            /* partial line across reads */
    int           last_was_cr;
    int           chunked;
    long          content_length;  /* -1 when absent */
    long          body_seen;
    long          chunk_left;

    http_body_cb  cb;
    void         *ctx;
    int           err;

    char          rbuf[HTTP_READ_CHUNK];
} http_resp;

void http_resp_init(http_resp *r, http_body_cb cb, void *ctx);
void http_resp_free(http_resp *r);
int  http_resp_feed(http_resp *r, const char *p, size_t n);
int  http_resp_done(const http_resp *r);   /* 1 when the message is complete */

/* Case-insensitive header lookup. Copies at most outsz-1 bytes plus a NUL.
 * Returns 1 when found. */
int  http_resp_header(const http_resp *r, const char *name,
                      char *out, size_t outsz);

/* Reads from the transport until the response completes, EOF, or error.
 * Returns 0 on success, non-zero otherwise. */
int  http_pump(http_resp *r, http_transport t);

#endif
