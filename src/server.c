#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "server.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "frame.h"
#include "io.h"
#include "network.h"
#include "pathres.h"
#include "protocol.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* State of one client connection. Each connection is served by its own
 * process, so these are plain variables. */
typedef struct {
    int           fd;
    unsigned long id;          /* [conn=N]: assigned in accept order, from 1   */
    unsigned long exchanges;   /* request/response pairs so far                */
    const char   *root;        /* canonical (realpath) document root           */
} Conn;

/* ------------------------------------------------------------------ */
/* logging: one write() per line so lines from different connection
 * processes never interleave                                          */

static void log_line(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n > sizeof line - 2)
        n = (int)sizeof line - 2;
    line[n++] = '\n';
    if (write(STDERR_FILENO, line, (size_t)n) < 0) {
        /* nothing sensible to do if stderr is gone */
    }
}

/* Printable form of a request path for the log (never log raw bytes). */
static void printable(const uint8_t *p, size_t n, char *out, size_t cap)
{
    size_t i, o = 0;
    for (i = 0; i < n && o + 5 < cap; i++) {
        if (p[i] >= 0x20 && p[i] < 0x7f && p[i] != '%')
            out[o++] = (char)p[i];
        else
            o += (size_t)snprintf(out + o, cap - o, "%%%02x", p[i]);
    }
    if (i < n && o + 4 < cap) {
        memcpy(out + o, "...", 3);
        o += 3;
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* sending                                                             */

static const char *status_text(unsigned s)
{
    switch (s) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    default:  return "";
    }
}

/* Send a RESPONSE frame. `headers` must already be filled in. */
static int send_response_frame(Conn *c, uint16_t status, Header *headers,
                               unsigned count, int end_stream)
{
    Response r;
    uint8_t payload[MAX_FRAME_PAYLOAD];
    size_t len = 0;

    memset(&r, 0, sizeof r);
    r.status_code = status;
    r.header_count = (uint8_t)count;
    memcpy(r.headers, headers, count * sizeof headers[0]);
    if (encode_response(&r, payload, sizeof payload, &len) != BHTP_OK)
        return -1;
    return frame_write(c->fd, FRAME_RESPONSE,
                       end_stream ? FLAG_END_STREAM : 0, payload, len);
}

/* A body-less response: content-length 0, END_STREAM on the RESPONSE frame. */
static int send_status_only(Conn *c, uint16_t status, int keep_alive)
{
    Header h[3];

    header_set(&h[0], "server", SERVER_NAME, strlen(SERVER_NAME));
    header_set(&h[1], "content-length", "0", 1);
    if (keep_alive)
        header_set(&h[2], "connection", "keep-alive", 10);
    else
        header_set(&h[2], "connection", "close", 5);
    return send_response_frame(c, status, h, 3, 1);
}

static void send_error_frame(Conn *c, uint16_t status, const char *reason)
{
    uint8_t payload[MAX_REASON_LENGTH + 2];
    size_t len = 0;

    if (encode_error(status, reason, payload, sizeof payload, &len) == BHTP_OK)
        (void)frame_write(c->fd, FRAME_ERROR, 0, payload, len);
}

static const char *content_type_for(const char *path)
{
    static const struct { const char *ext, *type; } map[] = {
        { ".html", "text/html" },       { ".htm",  "text/html" },
        { ".txt",  "text/plain" },      { ".css",  "text/css" },
        { ".js",   "application/javascript" },
        { ".json", "application/json" },{ ".png",  "image/png" },
        { ".jpg",  "image/jpeg" },      { ".jpeg", "image/jpeg" },
        { ".gif",  "image/gif" },       { ".svg",  "image/svg+xml" },
        { ".pdf",  "application/pdf" },
    };
    const char *dot = strrchr(path, '.');
    size_t i;

    if (dot != NULL)
        for (i = 0; i < sizeof map / sizeof map[0]; i++)
            if (strcasecmp(dot, map[i].ext) == 0)
                return map[i].type;
    return "application/octet-stream";
}

/* IMF-fixdate, always in the C locale and GMT. */
static void http_date(time_t t, char *out, size_t cap)
{
    static const char *const days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const mons[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    struct tm tm;

    gmtime_r(&t, &tm);
    snprintf(out, cap, "%s, %02d %s %04d %02d:%02d:%02d GMT", days[tm.tm_wday],
             tm.tm_mday, mons[tm.tm_mon], tm.tm_year + 1900,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* Stream exactly `size` bytes of `fd` as DATA frames; END_STREAM on the last.
 * Returns 0, or -1 if the connection must be abandoned (the promised
 * content-length can no longer be honoured). */
static int stream_body(Conn *c, int fd, off_t size)
{
    uint8_t buf[FRAME_HEADER_SIZE + MAX_FRAME_PAYLOAD];
    off_t remaining = size;

    while (remaining > 0) {
        size_t want = remaining > (off_t)MAX_FRAME_PAYLOAD ? MAX_FRAME_PAYLOAD
                                                          : (size_t)remaining;
        size_t got = 0;
        FrameHeader h;

        while (got < want) {                   /* read() may return less */
            ssize_t r = read(fd, buf + FRAME_HEADER_SIZE + got, want - got);
            if (r > 0) {
                got += (size_t)r;
            } else if (r < 0 && errno == EINTR) {
                continue;
            } else {
                return -1;                     /* file shrank or I/O error */
            }
        }
        remaining -= (off_t)want;

        h.version = BHTP_VERSION;
        h.type = FRAME_DATA;
        h.flags = remaining == 0 ? FLAG_END_STREAM : 0;
        h.payload_length = (uint32_t)want;
        encode_frame_header(buf, &h);
        if (io_write_all(c->fd, buf, FRAME_HEADER_SIZE + want) != 0)
            return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* request handling                                                    */

static int header_is_close(const Request *rq)
{
    const Header *h = find_header(rq->headers, rq->header_count, "connection");
    size_t i;
    char v[8];

    if (h == NULL || h->value_length != 5)
        return 0;
    for (i = 0; i < 5; i++)
        v[i] = (char)tolower(h->value[i]);
    return memcmp(v, "close", 5) == 0;
}

/* Complete one exchange and log it. Returns 0 to keep the connection open,
 * -1 to close it. */
static int finish_status(Conn *c, uint16_t status, int keep_alive)
{
    if (send_status_only(c, status, keep_alive) != 0)
        return -1;
    log_line("[conn=%lu] response #%lu %u %s", c->id, c->exchanges, status,
             status_text(status));
    return keep_alive ? 0 : -1;
}

static int handle_request(Conn *c, const uint8_t *payload, size_t len)
{
    Request rq;
    char file[PATH_MAX], shown[200], date[40], clen[24];
    PathStatus ps;
    BhtpError e;
    struct stat st;
    Header hs[6];
    int fd, keep_alive;

    c->exchanges++;

    e = decode_request(payload, len, &rq);
    if (e != BHTP_OK) {
        log_line("[conn=%lu] request #%lu malformed (%s)", c->id, c->exchanges,
                 bhtp_error_text(e));
        return finish_status(c, 400, 1);        /* framing is intact: stay open */
    }
    keep_alive = !header_is_close(&rq);
    printable(rq.path, rq.path_length, shown, sizeof shown);
    log_line("[conn=%lu] request #%lu GET %s", c->id, c->exchanges, shown);

    ps = resolve_path(c->root, rq.path, rq.path_length, file, sizeof file);
    if (ps == PATH_BAD)
        return finish_status(c, 400, keep_alive);
    if (ps == PATH_NOT_FOUND)
        return finish_status(c, 404, keep_alive);
    if (ps == PATH_ERROR)
        return finish_status(c, 500, keep_alive);

    /* O_NONBLOCK so that opening a FIFO that slipped through could not hang us. */
    fd = open(file, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return finish_status(c, errno == ENOENT ? 404 : 500, keep_alive);
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return finish_status(c, 404, keep_alive);
    }

    snprintf(clen, sizeof clen, "%llu", (unsigned long long)st.st_size);
    http_date(st.st_mtime, date, sizeof date);
    header_set(&hs[0], "content-type", content_type_for(file), strlen(content_type_for(file)));
    header_set(&hs[1], "content-length", clen, strlen(clen));
    header_set(&hs[2], "server", SERVER_NAME, strlen(SERVER_NAME));
    header_set(&hs[3], "last-modified", date, strlen(date));
    header_set(&hs[4], "cache-control", "no-cache", 8);
    if (keep_alive)
        header_set(&hs[5], "connection", "keep-alive", 10);
    else
        header_set(&hs[5], "connection", "close", 5);

    if (send_response_frame(c, 200, hs, 6, st.st_size == 0) != 0) {
        close(fd);
        return -1;
    }
    if (stream_body(c, fd, st.st_size) != 0) {
        close(fd);
        log_line("[conn=%lu] response #%lu aborted while sending body", c->id, c->exchanges);
        return -1;
    }
    close(fd);
    log_line("[conn=%lu] response #%lu 200 OK (%llu bytes)", c->id, c->exchanges,
             (unsigned long long)st.st_size);
    return keep_alive ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* the per-connection loop                                             */

static void serve_connection(Conn *c)
{
    uint8_t raw[FRAME_HEADER_SIZE], payload[MAX_FRAME_PAYLOAD];
    FrameHeader h;
    const char *why = "client closed the connection";
    int server_initiated = 0;

    for (;;) {
        FrameStatus st = frame_read(c->fd, &h, raw, payload, NULL, NULL);

        if (st == FR_EOF)
            break;

        if (st == FR_SKIPPED) {
            /* Unknown frame type: already consumed. Carry on, no reply. */
            log_line("[conn=%lu] skipped unknown frame type 0x%02x (%u payload bytes)",
                     c->id, h.type, (unsigned)h.payload_length);
            continue;
        }

        if (st == FR_OK && h.type == FRAME_REQUEST) {
            if (handle_request(c, payload, h.payload_length) != 0) {
                why = "server closed the connection after the response";
                server_initiated = 1;
                break;
            }
            continue;
        }

        if (st == FR_OK || st == FR_BAD_FLAGS) {
            /* A well-framed frame we did not expect (RESPONSE/DATA/ERROR from a
             * client) or with undefined flags. Payload consumed, framing intact:
             * answer 400 and keep the connection. */
            c->exchanges++;
            log_line("[conn=%lu] request #%lu invalid frame (%s type 0x%02x flags 0x%04x)",
                     c->id, c->exchanges,
                     st == FR_OK ? "unexpected" : "bad flags", h.type, h.flags);
            if (finish_status(c, 400, 1) != 0) {
                why = "write error";
                break;
            }
            continue;
        }

        /* From here on the byte stream can no longer be trusted. */
        if (st == FR_BAD_MAGIC || st == FR_BAD_VERSION || st == FR_TOO_BIG) {
            log_line("[conn=%lu] fatal protocol error: %s", c->id, frame_status_text(st));
            send_error_frame(c, 400, st == FR_BAD_MAGIC   ? "bad magic" :
                                     st == FR_BAD_VERSION ? "unsupported version" :
                                                            "frame too large");
            log_line("[conn=%lu] closed (protocol error)", c->id);
            linger_close(c->fd);
            return;
        }
        if (st == FR_TRUNC) {
            why = "connection lost in the middle of a frame";
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            why = "idle timeout";
        } else {
            why = "I/O error";
        }
        break;
    }
    log_line("[conn=%lu] closed (%s)", c->id, why);
    if (server_initiated)
        linger_close(c->fd);       /* let the last response reach the client */
    else
        close(c->fd);
}

/* ------------------------------------------------------------------ */
/* accept loop                                                         */

int run_server(const char *root, const char *port)
{
    char root_real[PATH_MAX];
    struct stat st;
    int listen_fd;
    unsigned long next_id = 1;

    if (realpath(root, root_real) == NULL || stat(root_real, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "bserve: root '%s' is not a readable directory\n", root);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);                  /* write errors come back as EPIPE */
    signal(SIGCHLD, SIG_IGN);                  /* children are reaped automatically */

    listen_fd = create_server_socket(port);
    if (listen_fd < 0)
        return 1;
    log_line("bserve: serving %s on port %s", root_real, port);

    for (;;) {
        struct sockaddr_storage peer;
        socklen_t plen = sizeof peer;
        int fd = accept(listen_fd, (struct sockaddr *)&peer, &plen);
        pid_t pid;

        if (fd < 0) {
            if (errno != EINTR && errno != ECONNABORTED)
                perror("bserve: accept");
            continue;
        }

        log_line("[conn=%lu] accepted", next_id);

        pid = fork();
        if (pid < 0) {
            perror("bserve: fork");
            close(fd);
        } else if (pid == 0) {                 /* child: owns this connection */
            Conn c;
            close(listen_fd);
            tune_connection(fd, IDLE_TIMEOUT_SECONDS, SEND_TIMEOUT_SECONDS);
            c.fd = fd;
            c.id = next_id;
            c.exchanges = 0;
            c.root = root_real;
            serve_connection(&c);
            _exit(0);
        } else {
            close(fd);                         /* parent: keep accepting */
        }
        next_id++;
    }
}
