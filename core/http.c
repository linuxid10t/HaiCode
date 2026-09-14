/* http.c - minimal HTTP/1.1 client framing. C89. */

#include "http.h"

#include <string.h>
#include <stdio.h>

#define HTTP_E_SEND   (-1)
#define HTTP_E_PROTO  (-2)
#define HTTP_E_OOM    (-3)
#define HTTP_E_RECV   (-4)

/* ------------------------------------------------------------- request --- */

int
http_req_begin(http_req *r, http_transport t, const char *method,
               const char *path, const char *host)
{
    r->t            = t;
    r->chunked      = 0;
    r->headers_sent = 0;
    r->err          = 0;
    buf_init(&r->head);

    buf_puts(&r->head, method);
    buf_putc(&r->head, ' ');
    buf_puts(&r->head, path);
    buf_puts(&r->head, " HTTP/1.1\r\n");
    buf_puts(&r->head, "Host: ");
    buf_puts(&r->head, host);
    buf_puts(&r->head, "\r\n");
    /* No keep-alive negotiation here: the caller decides by sending
     * Connection: explicitly. HTTP/1.1 defaults to persistent. */

    if (r->head.oom)
        r->err = HTTP_E_OOM;
    return r->err;
}

int
http_req_header(http_req *r, const char *name, const char *value)
{
    if (r->err != 0)
        return r->err;
    if (r->headers_sent)
        return (r->err = HTTP_E_PROTO);

    buf_puts(&r->head, name);
    buf_puts(&r->head, ": ");
    buf_puts(&r->head, value);
    buf_puts(&r->head, "\r\n");

    if (r->head.oom)
        r->err = HTTP_E_OOM;
    return r->err;
}

int
http_req_body_begin(http_req *r, long content_length)
{
    char tmp[32];

    if (r->err != 0)
        return r->err;
    if (r->headers_sent)
        return (r->err = HTTP_E_PROTO);

    if (content_length < 0) {
        r->chunked = 1;
        buf_puts(&r->head, "Transfer-Encoding: chunked\r\n");
    } else {
        sprintf(tmp, "%ld", content_length);
        buf_puts(&r->head, "Content-Length: ");
        buf_puts(&r->head, tmp);
        buf_puts(&r->head, "\r\n");
    }
    buf_puts(&r->head, "\r\n");

    if (r->head.oom)
        return (r->err = HTTP_E_OOM);

    if (r->t.xsend(r->t.ctx, r->head.data, r->head.len) != 0)
        return (r->err = HTTP_E_SEND);

    r->headers_sent = 1;
    buf_free(&r->head);
    return 0;
}

int
http_req_body_write(void *rv, const char *p, size_t n)
{
    http_req *r = (http_req *)rv;
    char      hdr[32];

    if (r->err != 0)
        return r->err;
    if (!r->headers_sent)
        return (r->err = HTTP_E_PROTO);
    if (n == 0)
        return 0;

    if (r->chunked) {
        sprintf(hdr, "%lx\r\n", (unsigned long)n);
        if (r->t.xsend(r->t.ctx, hdr, strlen(hdr)) != 0)
            return (r->err = HTTP_E_SEND);
    }
    if (r->t.xsend(r->t.ctx, p, n) != 0)
        return (r->err = HTTP_E_SEND);
    if (r->chunked) {
        if (r->t.xsend(r->t.ctx, "\r\n", 2) != 0)
            return (r->err = HTTP_E_SEND);
    }
    return 0;
}

int
http_req_body_end(http_req *r)
{
    if (r->err != 0)
        return r->err;
    if (!r->headers_sent)
        return (r->err = HTTP_E_PROTO);
    if (r->chunked) {
        if (r->t.xsend(r->t.ctx, "0\r\n\r\n", 5) != 0)
            return (r->err = HTTP_E_SEND);
    }
    return 0;
}

void
http_req_free(http_req *r)
{
    buf_free(&r->head);
}

/* ------------------------------------------------------------ response --- */

void
http_resp_init(http_resp *r, http_body_cb cb, void *ctx)
{
    r->status         = 0;
    buf_init(&r->reason);
    buf_init(&r->headers);
    r->state          = HTTP_ST_STATUS;
    buf_init(&r->line);
    r->last_was_cr    = 0;
    r->chunked        = 0;
    r->content_length = -1;
    r->body_seen      = 0;
    r->chunk_left     = 0;
    r->cb             = cb;
    r->ctx            = ctx;
    r->err            = 0;
}

void
http_resp_free(http_resp *r)
{
    buf_free(&r->reason);
    buf_free(&r->headers);
    buf_free(&r->line);
}

int
http_resp_done(const http_resp *r)
{
    return (r->state == HTTP_ST_DONE);
}

static char
http_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
    return c;
}

int
http_resp_header(const http_resp *r, const char *name, char *out, size_t outsz)
{
    size_t      nlen = strlen(name);
    const char *p    = r->headers.data;
    const char *end;

    if (p == NULL || outsz == 0)
        return 0;
    end = p + r->headers.len;

    while (p < end) {
        const char *eol = p;
        const char *colon;
        size_t      i;
        int         match;

        while (eol < end && *eol != '\n')
            eol++;

        colon = p;
        while (colon < eol && *colon != ':')
            colon++;

        match = ((size_t)(colon - p) == nlen);
        if (match) {
            for (i = 0; i < nlen; i++) {
                if (p[i] != http_lower(name[i])) {
                    match = 0;
                    break;
                }
            }
        }

        if (match) {
            const char *v = (colon < eol) ? colon + 1 : eol;
            size_t      vlen;
            while (v < eol && (*v == ' ' || *v == '\t'))
                v++;
            vlen = (size_t)(eol - v);
            if (vlen > outsz - 1)
                vlen = outsz - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return 1;
        }

        p = (eol < end) ? eol + 1 : end;
    }
    return 0;
}

static void
http_parse_status(http_resp *r, const char *s, size_t n)
{
    size_t i = 0;

    /* "HTTP/1.x SSS Reason" */
    while (i < n && s[i] != ' ')
        i++;
    while (i < n && s[i] == ' ')
        i++;

    r->status = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        r->status = r->status * 10 + (s[i] - '0');
        i++;
    }
    while (i < n && s[i] == ' ')
        i++;
    if (i < n)
        buf_append(&r->reason, s + i, n - i);

    if (r->status < 100 || r->status > 599) {
        r->state = HTTP_ST_ERROR;
        r->err   = HTTP_E_PROTO;
    }
}

static void
http_store_header(http_resp *r, const char *s, size_t n)
{
    size_t i;
    size_t colon = n;

    for (i = 0; i < n; i++) {
        if (s[i] == ':') {
            colon = i;
            break;
        }
    }
    if (colon == n)
        return;   /* no colon: ignore rather than fail the response */

    for (i = 0; i < colon; i++)
        buf_putc(&r->headers, http_lower(s[i]));
    buf_append(&r->headers, s + colon, n - colon);
    buf_putc(&r->headers, '\n');
}

/* Called once the blank line after the headers is seen. */
static void
http_headers_done(http_resp *r)
{
    char v[64];

    if (http_resp_header(r, "transfer-encoding", v, sizeof(v))) {
        size_t i;
        for (i = 0; v[i] != '\0'; i++)
            v[i] = http_lower(v[i]);
        if (strstr(v, "chunked") != NULL)
            r->chunked = 1;
    }
    if (!r->chunked && http_resp_header(r, "content-length", v, sizeof(v))) {
        long  cl = 0;
        size_t i = 0;
        while (v[i] >= '0' && v[i] <= '9') {
            cl = cl * 10 + (v[i] - '0');
            i++;
        }
        r->content_length = cl;
    }

    /* Responses that carry no body regardless of headers. */
    if (r->status == 204 || r->status == 304 ||
        (r->status >= 100 && r->status < 200)) {
        r->state = HTTP_ST_DONE;
        return;
    }

    if (r->chunked)
        r->state = HTTP_ST_CHUNK_SIZE;
    else if (r->content_length == 0)
        r->state = HTTP_ST_DONE;
    else if (r->content_length > 0)
        r->state = HTTP_ST_BODY_LEN;
    else
        r->state = HTTP_ST_BODY_EOF;   /* delimited by connection close */
}

static void
http_emit(http_resp *r, const char *p, size_t n)
{
    if (n > 0 && r->cb != NULL)
        r->cb(r->ctx, p, n);
}

/*
 * Pulls one complete line out of p[*i .. n). Returns 1 when a line was
 * completed (contents in r->line), 0 when the input ran out first. As with the
 * SSE parser, r->line persists across calls so a header split across two reads
 * reassembles correctly.
 */
static int
http_take_line(http_resp *r, const char *p, size_t n, size_t *i)
{
    while (*i < n) {
        char c = p[*i];

        if (r->last_was_cr) {
            r->last_was_cr = 0;
            if (c == '\n') {
                (*i)++;
                return 1;
            }
            return 1;   /* bare CR terminated the line */
        }

        if (c == '\r') {
            r->last_was_cr = 1;
            (*i)++;
            continue;
        }
        if (c == '\n') {
            (*i)++;
            return 1;
        }

        if (buf_putc(&r->line, c) != 0) {
            r->state = HTTP_ST_ERROR;
            r->err   = HTTP_E_OOM;
            return 0;
        }
        (*i)++;
    }
    return 0;
}

static long
http_hex(const char *s, size_t n)
{
    long   v = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == ';' || c == ' ')
            break;          /* chunk extension */
        v <<= 4;
        if (c >= '0' && c <= '9')       v |= (c - '0');
        else if (c >= 'a' && c <= 'f')  v |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')  v |= (c - 'A' + 10);
        else return -1;
    }
    return v;
}

int
http_resp_feed(http_resp *r, const char *p, size_t n)
{
    size_t i = 0;

    while (i < n && r->state != HTTP_ST_DONE && r->state != HTTP_ST_ERROR) {
        switch (r->state) {

        case HTTP_ST_STATUS:
            if (http_take_line(r, p, n, &i)) {
                http_parse_status(r, r->line.data ? r->line.data : "",
                                  r->line.len);
                buf_clear(&r->line);
                if (r->state != HTTP_ST_ERROR)
                    r->state = HTTP_ST_HEADERS;
            }
            break;

        case HTTP_ST_HEADERS:
            if (http_take_line(r, p, n, &i)) {
                if (r->line.len == 0) {
                    http_headers_done(r);
                } else {
                    http_store_header(r, r->line.data, r->line.len);
                }
                buf_clear(&r->line);
            }
            break;

        case HTTP_ST_BODY_LEN: {
            size_t avail = n - i;
            long   want  = r->content_length - r->body_seen;
            size_t take  = ((long)avail < want) ? avail : (size_t)want;

            http_emit(r, p + i, take);
            i += take;
            r->body_seen += (long)take;
            if (r->body_seen >= r->content_length)
                r->state = HTTP_ST_DONE;
            break;
        }

        case HTTP_ST_CHUNK_SIZE:
            if (http_take_line(r, p, n, &i)) {
                long sz = http_hex(r->line.data ? r->line.data : "",
                                   r->line.len);
                buf_clear(&r->line);
                if (sz < 0) {
                    r->state = HTTP_ST_ERROR;
                    r->err   = HTTP_E_PROTO;
                } else if (sz == 0) {
                    r->state = HTTP_ST_TRAILER;
                } else {
                    r->chunk_left = sz;
                    r->state      = HTTP_ST_CHUNK_DATA;
                }
            }
            break;

        case HTTP_ST_CHUNK_DATA: {
            size_t avail = n - i;
            size_t take  = ((long)avail < r->chunk_left)
                         ? avail : (size_t)r->chunk_left;

            http_emit(r, p + i, take);
            i += take;
            r->chunk_left -= (long)take;
            if (r->chunk_left == 0)
                r->state = HTTP_ST_CHUNK_CRLF;
            break;
        }

        case HTTP_ST_CHUNK_CRLF:
            if (http_take_line(r, p, n, &i)) {
                buf_clear(&r->line);
                r->state = HTTP_ST_CHUNK_SIZE;
            }
            break;

        case HTTP_ST_TRAILER:
            if (http_take_line(r, p, n, &i)) {
                int blank = (r->line.len == 0);
                buf_clear(&r->line);
                if (blank)
                    r->state = HTTP_ST_DONE;
            }
            break;

        case HTTP_ST_BODY_EOF:
            http_emit(r, p + i, n - i);
            r->body_seen += (long)(n - i);
            i = n;
            break;

        default:
            i = n;
            break;
        }
    }

    return (r->state == HTTP_ST_ERROR) ? r->err : 0;
}

int
http_pump(http_resp *r, http_transport t)
{
    for (;;) {
        int got;

        if (r->state == HTTP_ST_DONE)
            return 0;
        if (r->state == HTTP_ST_ERROR)
            return r->err;

        got = t.xrecv(t.ctx, r->rbuf, sizeof(r->rbuf));
        if (got < 0) {
            r->err   = HTTP_E_RECV;
            r->state = HTTP_ST_ERROR;
            return r->err;
        }
        if (got == 0) {
            /* Clean close. Legal only where the body is EOF-delimited. */
            if (r->state == HTTP_ST_BODY_EOF) {
                r->state = HTTP_ST_DONE;
                return 0;
            }
            r->err   = HTTP_E_PROTO;
            r->state = HTTP_ST_ERROR;
            return r->err;
        }

        if (http_resp_feed(r, r->rbuf, (size_t)got) != 0)
            return r->err;
    }
}
