/*
 * test_frame.c - frame reader / writer tests over real socketpairs.
 *
 * These tests treat TCP as what it is: a byte stream. They deliver frames in
 * single bytes, in arbitrary splits, many at once, truncated, interrupted by
 * signals, and through a socket whose send buffer is tiny so that write() can
 * only make partial progress.
 */
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "frame.h"
#include "io.h"
#include "protocol.h"

/* Deliberately ignore the result of test-fixture writes. */
#define W(x) ((void)!(x))

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

#define RUN(fn)                                                              \
    do {                                                                     \
        int before = failures;                                               \
        fn();                                                                \
        printf("%s  %s\n", failures == before ? "PASS" : "FAIL", #fn);       \
    } while (0)

static void make_pair(int sv[2])
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        perror("socketpair");
        exit(2);
    }
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* One valid frame (type, flags, payload) as bytes; returns its length. */
static size_t frame_bytes(uint8_t *out, uint8_t type, uint16_t flags,
                          const void *payload, size_t n)
{
    return build_frame(out, FRAME_HEADER_SIZE + n, type, flags, payload, n);
}

/* Fork a child that writes `data` in pieces of `piece` bytes with a short
 * pause between pieces, then closes. Returns the pid; read from sv[0]. */
static pid_t spawn_slow_writer(int sv[2], const uint8_t *data, size_t n,
                               size_t piece, long pause_ms)
{
    pid_t pid = fork();
    if (pid == 0) {
        size_t off = 0;
        close(sv[0]);
        while (off < n) {
            size_t k = n - off < piece ? n - off : piece;
            if (write(sv[1], data + off, k) != (ssize_t)k)
                _exit(1);
            off += k;
            if (pause_ms > 0)
                sleep_ms(pause_ms);
        }
        close(sv[1]);
        _exit(0);
    }
    close(sv[1]);
    return pid;
}

static void reap(pid_t pid)
{
    int st;
    waitpid(pid, &st, 0);
}

/* ------------------------------------------------------------------ */

static void test_read_one_known_frame(void)
{
    int sv[2];
    uint8_t buf[64], raw[12], payload[MAX_FRAME_PAYLOAD];
    FrameHeader h;
    size_t n = frame_bytes(buf, FRAME_DATA, FLAG_END_STREAM, "hello", 5);

    make_pair(sv);
    CHECK(write(sv[1], buf, n) == (ssize_t)n);
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_OK);
    CHECK(h.type == FRAME_DATA && h.flags == FLAG_END_STREAM &&
          h.payload_length == 5 && memcmp(payload, "hello", 5) == 0);
    close(sv[0]); close(sv[1]);
}

static void test_zero_length_payload(void)
{
    int sv[2];
    uint8_t buf[64], raw[12], payload[MAX_FRAME_PAYLOAD];
    FrameHeader h;
    size_t n = frame_bytes(buf, FRAME_DATA, 0, NULL, 0);

    make_pair(sv);
    W(write(sv[1], buf, n));
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_OK);
    CHECK(h.payload_length == 0);
    close(sv[0]); close(sv[1]);
}

/* The headline requirement: known / UNKNOWN / known, all in one write. */
static void test_unknown_frame_is_skipped_and_next_frame_processed(void)
{
    int sv[2];
    uint8_t all[512], raw[12], payload[MAX_FRAME_PAYLOAD], junk[100];
    FrameHeader h;
    size_t n = 0;

    memset(junk, 0xEE, sizeof junk);
    n += frame_bytes(all + n, FRAME_DATA, 0, "first", 5);
    n += frame_bytes(all + n, 0x7f, 0xABCD, junk, sizeof junk);   /* unknown */
    n += frame_bytes(all + n, FRAME_DATA, FLAG_END_STREAM, "second", 6);

    make_pair(sv);
    CHECK(write(sv[1], all, n) == (ssize_t)n);

    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_OK);
    CHECK(memcmp(payload, "first", 5) == 0);

    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_SKIPPED);
    CHECK(h.type == 0x7f && h.payload_length == 100);

    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_OK);
    CHECK(h.type == FRAME_DATA && h.payload_length == 6 &&
          memcmp(payload, "second", 6) == 0);
    close(sv[0]); close(sv[1]);
}

static void test_unknown_type_zero_and_255_and_empty_payload(void)
{
    int sv[2];
    uint8_t all[256], raw[12], payload[MAX_FRAME_PAYLOAD];
    FrameHeader h;
    size_t n = 0;

    n += frame_bytes(all + n, 0x00, 0, NULL, 0);          /* type 0, empty */
    n += frame_bytes(all + n, 0xFF, 0xFFFF, "xyz", 3);    /* type 255      */
    n += frame_bytes(all + n, FRAME_ERROR, 0, "\x01\x90ok", 4);

    make_pair(sv);
    W(write(sv[1], all, n));
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_SKIPPED);
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_SKIPPED);
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_OK);
    CHECK(h.type == FRAME_ERROR);
    close(sv[0]); close(sv[1]);
}

/* An unknown frame may be MUCH bigger than the known-frame limit: it is
 * streamed through a 4 KiB buffer and never stored. */
static void test_huge_unknown_frame_is_skipped_in_constant_memory(void)
{
    int sv[2];
    uint8_t hdr[12], tail[64], raw[12], payload[MAX_FRAME_PAYLOAD];
    FrameHeader h;
    pid_t pid;
    const uint32_t big = 3u * 1024 * 1024 + 17;          /* 3 MiB + 17 bytes */
    uint8_t *data = malloc(big + 12 + sizeof tail);
    size_t n, off = 0;
    FrameHeader fh = { BHTP_VERSION, 0x42, 0, big };

    encode_frame_header(data, &fh);
    memset(data + 12, 0x5A, big);
    off = 12 + big;
    n = frame_bytes(tail, FRAME_DATA, FLAG_END_STREAM, "after", 5);
    memcpy(data + off, tail, n);
    off += n;
    (void)hdr;

    make_pair(sv);
    pid = spawn_slow_writer(sv, data, off, 65536, 0);
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_SKIPPED);
    CHECK(h.payload_length == big);
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_OK);
    CHECK(memcmp(payload, "after", 5) == 0);
    close(sv[0]);
    reap(pid);
    free(data);
}

typedef struct { size_t total; uint8_t first; } SinkCtx;
static void count_sink(void *ctx, const uint8_t *chunk, size_t n)
{
    SinkCtx *s = ctx;
    if (s->total == 0 && n > 0)
        s->first = chunk[0];
    s->total += n;
}

static void test_skip_sink_sees_every_skipped_byte(void)
{
    int sv[2];
    uint8_t buf[9000], raw[12], payload[MAX_FRAME_PAYLOAD], body[8000];
    FrameHeader h;
    SinkCtx ctx = { 0, 0 };
    pid_t pid;
    size_t n;

    memset(body, 0x33, sizeof body);
    n = frame_bytes(buf, 0x70, 0, body, sizeof body);
    make_pair(sv);
    pid = spawn_slow_writer(sv, buf, n, 1000, 1);
    CHECK(frame_read(sv[0], &h, raw, payload, count_sink, &ctx) == FR_SKIPPED);
    CHECK(ctx.total == sizeof body && ctx.first == 0x33);
    close(sv[0]);
    reap(pid);
}

/* One frame delivered a single byte at a time. */
static void test_frame_split_across_many_reads_byte_by_byte(void)
{
    int sv[2];
    uint8_t buf[128], raw[12], payload[MAX_FRAME_PAYLOAD];
    FrameHeader h;
    pid_t pid;
    size_t n = frame_bytes(buf, FRAME_RESPONSE, FLAG_END_STREAM, "0123456789", 10);

    make_pair(sv);
    pid = spawn_slow_writer(sv, buf, n, 1, 2);
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_OK);
    CHECK(h.type == FRAME_RESPONSE && h.payload_length == 10 &&
          memcmp(payload, "0123456789", 10) == 0);
    close(sv[0]);
    reap(pid);
}

/* Split the byte stream at EVERY possible offset. */
static void test_frame_split_at_every_offset(void)
{
    uint8_t buf[128], raw[12], payload[MAX_FRAME_PAYLOAD];
    size_t n = frame_bytes(buf, FRAME_DATA, 0, "split-me-anywhere", 17), cut;

    for (cut = 1; cut < n; cut++) {
        int sv[2];
        FrameHeader h;
        pid_t pid;

        make_pair(sv);
        pid = fork();
        if (pid == 0) {
            close(sv[0]);
            W(write(sv[1], buf, cut));
            sleep_ms(3);
            W(write(sv[1], buf + cut, n - cut));
            _exit(0);
        }
        close(sv[1]);
        CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_OK);
        CHECK(h.payload_length == 17 && memcmp(payload, "split-me-anywhere", 17) == 0);
        close(sv[0]);
        reap(pid);
    }
}

/* Many frames arrive in a single read(): the reader must hand them out one
 * at a time and never merge or lose one. */
static void test_many_frames_in_one_write(void)
{
    int sv[2];
    uint8_t all[4096], raw[12], payload[MAX_FRAME_PAYLOAD];
    FrameHeader h;
    size_t n = 0;
    int i;
    char msg[16];

    for (i = 0; i < 50; i++) {
        int len = snprintf(msg, sizeof msg, "frame-%d", i);
        n += frame_bytes(all + n, FRAME_DATA, 0, msg, (size_t)len);
        if (i % 7 == 3)
            n += frame_bytes(all + n, 0x90 + (i % 5), 0, "unk", 3);
    }
    make_pair(sv);
    CHECK(write(sv[1], all, n) == (ssize_t)n);
    close(sv[1]);

    for (i = 0; i < 50; i++) {
        int len = snprintf(msg, sizeof msg, "frame-%d", i);
        FrameStatus st = frame_read(sv[0], &h, raw, payload, NULL, NULL);
        if (st == FR_SKIPPED)
            st = frame_read(sv[0], &h, raw, payload, NULL, NULL);
        CHECK(st == FR_OK);
        CHECK(h.payload_length == (uint32_t)len && memcmp(payload, msg, (size_t)len) == 0);
    }
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_EOF);
    close(sv[0]);
}

static void test_clean_eof_vs_truncation(void)
{
    int sv[2];
    uint8_t buf[64], raw[12], payload[MAX_FRAME_PAYLOAD];
    FrameHeader h;
    size_t n = frame_bytes(buf, FRAME_DATA, 0, "abcdefgh", 8), cut;

    /* EOF exactly between frames is clean */
    make_pair(sv);
    close(sv[1]);
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_EOF);
    close(sv[0]);

    /* EOF after any proper prefix of a frame is a truncation */
    for (cut = 1; cut < n; cut++) {
        make_pair(sv);
        W(write(sv[1], buf, cut));
        close(sv[1]);
        CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_TRUNC);
        close(sv[0]);
    }

    /* truncated inside an UNKNOWN frame is a truncation too */
    n = frame_bytes(buf, 0x70, 0, "abcdefgh", 8);
    make_pair(sv);
    W(write(sv[1], buf, n - 3));
    close(sv[1]);
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_TRUNC);
    close(sv[0]);
}

static void test_bad_magic_and_version(void)
{
    int sv[2];
    uint8_t buf[64], raw[12], payload[MAX_FRAME_PAYLOAD];
    FrameHeader h;
    size_t n = frame_bytes(buf, FRAME_DATA, 0, "abc", 3);

    buf[1] = 'X';
    make_pair(sv);
    W(write(sv[1], buf, n));
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_BAD_MAGIC);
    close(sv[0]); close(sv[1]);

    n = frame_bytes(buf, FRAME_DATA, 0, "abc", 3);
    buf[4] = 9;
    make_pair(sv);
    W(write(sv[1], buf, n));
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_BAD_VERSION);
    CHECK(h.version == 9);
    close(sv[0]); close(sv[1]);

    /* an HTTP request sent to a BHTP port is not mistaken for a frame */
    make_pair(sv);
    W(write(sv[1], "GET /index.html HTTP/1.1\r\n\r\n", 28));
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_BAD_MAGIC);
    close(sv[0]); close(sv[1]);
}

static void test_oversized_known_frame_is_rejected_without_reading_payload(void)
{
    int sv[2];
    uint8_t raw[12], payload[MAX_FRAME_PAYLOAD], hdr[12];
    FrameHeader h, in = { BHTP_VERSION, FRAME_DATA, 0, MAX_FRAME_PAYLOAD + 1 };

    encode_frame_header(hdr, &in);
    make_pair(sv);
    W(write(sv[1], hdr, 12));
    W(write(sv[1], "PAYLOAD-NOT-CONSUMED", 20));
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_TOO_BIG);
    CHECK(h.payload_length == MAX_FRAME_PAYLOAD + 1);

    /* the 20 bytes are still unread in the socket */
    CHECK(io_read_exact(sv[0], payload, 20) == IO_OK);
    CHECK(memcmp(payload, "PAYLOAD-NOT-CONSUMED", 20) == 0);
    close(sv[0]); close(sv[1]);

    /* absurd length: 0xFFFFFFFF */
    in.payload_length = 0xFFFFFFFFu;
    encode_frame_header(hdr, &in);
    make_pair(sv);
    W(write(sv[1], hdr, 12));
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_TOO_BIG);
    close(sv[0]); close(sv[1]);
}

static void test_max_size_known_frame_is_accepted(void)
{
    int sv[2];
    uint8_t *all = malloc(FRAME_HEADER_SIZE + MAX_FRAME_PAYLOAD);
    uint8_t raw[12], payload[MAX_FRAME_PAYLOAD], body[MAX_FRAME_PAYLOAD];
    FrameHeader h;
    pid_t pid;
    size_t i;

    for (i = 0; i < sizeof body; i++)
        body[i] = (uint8_t)(i * 7);
    build_frame(all, FRAME_HEADER_SIZE + MAX_FRAME_PAYLOAD, FRAME_DATA, 0, body, sizeof body);
    make_pair(sv);
    pid = spawn_slow_writer(sv, all, FRAME_HEADER_SIZE + MAX_FRAME_PAYLOAD, 1500, 1);
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_OK);
    CHECK(h.payload_length == MAX_FRAME_PAYLOAD && memcmp(payload, body, sizeof body) == 0);
    close(sv[0]);
    reap(pid);
    free(all);
}

/* Bad flag bits on a KNOWN frame: payload IS consumed so the stream stays
 * in sync and the next frame can still be read. */
static void test_invalid_flags_keep_stream_in_sync(void)
{
    int sv[2];
    uint8_t all[256], raw[12], payload[MAX_FRAME_PAYLOAD];
    FrameHeader h;
    size_t n = 0;

    n += frame_bytes(all + n, FRAME_REQUEST, 0x0001, "bad", 3);   /* REQUEST allows no flags */
    n += frame_bytes(all + n, FRAME_DATA, 0x0002, "bad", 3);      /* undefined bit 1 */
    n += frame_bytes(all + n, FRAME_DATA, 0x8000, "bad", 3);
    n += frame_bytes(all + n, FRAME_DATA, FLAG_END_STREAM, "good", 4);

    make_pair(sv);
    W(write(sv[1], all, n));
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_BAD_FLAGS);
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_BAD_FLAGS);
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_BAD_FLAGS);
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_OK);
    CHECK(memcmp(payload, "good", 4) == 0);
    close(sv[0]); close(sv[1]);
}

static void test_frame_write_roundtrip_and_limits(void)
{
    int sv[2];
    uint8_t raw[12], payload[MAX_FRAME_PAYLOAD], big[MAX_FRAME_PAYLOAD + 1];
    FrameHeader h;

    memset(big, 1, sizeof big);
    make_pair(sv);
    CHECK(frame_write(sv[1], FRAME_ERROR, 0, (const uint8_t *)"\x01\x90zz", 4) == 0);
    CHECK(frame_read(sv[0], &h, raw, payload, NULL, NULL) == FR_OK);
    CHECK(h.type == FRAME_ERROR && h.payload_length == 4);
    CHECK(frame_write(sv[1], FRAME_DATA, 0, big, sizeof big) == -1 && errno == EMSGSIZE);
    close(sv[0]); close(sv[1]);
}

/* ------------------------------------------------------------------ */
/* write side: partial writes and EINTR                                 */

static uint32_t checksum(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        a = (a + p[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

/* The socket buffer is shrunk so a single write() can only be accepted
 * partially; io_write_all must keep offering the remainder. */
static void test_partial_writes_with_tiny_send_buffer(void)
{
    int sv[2], tiny = 2048;
    const size_t total = 2 * 1024 * 1024;
    uint8_t *data = malloc(total), *got = malloc(total);
    pid_t pid;
    size_t i;

    for (i = 0; i < total; i++)
        data[i] = (uint8_t)((i * 31) ^ (i >> 8));
    make_pair(sv);
    setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &tiny, sizeof tiny);
    setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &tiny, sizeof tiny);

    pid = fork();
    if (pid == 0) {                       /* slow reader */
        size_t off = 0;
        close(sv[1]);
        while (off < total) {
            ssize_t r = read(sv[0], got + off, off + 1500 < total ? 1500 : total - off);
            if (r <= 0)
                _exit(1);
            off += (size_t)r;
            if (off % 300000 < 1500)
                sleep_ms(1);
        }
        _exit(checksum(got, total) == checksum(data, total) ? 0 : 3);
    }
    close(sv[0]);
    CHECK(io_write_all(sv[1], data, total) == 0);
    close(sv[1]);
    {
        int st = 0;
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    free(data); free(got);
}

static volatile sig_atomic_t alarms = 0;
static void on_alarm(int sig) { (void)sig; alarms++; }

/* Signals (without SA_RESTART) interrupt read() with EINTR over and over;
 * io_read_exact must retry and still deliver every byte. */
static void test_eintr_is_retried(void)
{
    int sv[2];
    struct sigaction sa;
    struct itimerval tv;
    uint8_t buf[300], out[300];
    pid_t pid;
    size_t i;

    for (i = 0; i < sizeof buf; i++)
        buf[i] = (uint8_t)i;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                                  /* NO SA_RESTART */
    sigaction(SIGALRM, &sa, NULL);

    make_pair(sv);
    pid = spawn_slow_writer(sv, buf, sizeof buf, 100, 120);   /* 3 pieces, 120 ms apart */

    memset(&tv, 0, sizeof tv);
    tv.it_value.tv_usec = 5000;
    tv.it_interval.tv_usec = 5000;
    setitimer(ITIMER_REAL, &tv, NULL);

    CHECK(io_read_exact(sv[0], out, sizeof out) == IO_OK);

    memset(&tv, 0, sizeof tv);
    setitimer(ITIMER_REAL, &tv, NULL);
    CHECK(alarms > 5);                                /* we really were interrupted */
    CHECK(memcmp(out, buf, sizeof buf) == 0);
    close(sv[0]);
    reap(pid);
}

static void test_io_read_exact_result_codes(void)
{
    int sv[2];
    uint8_t b[10];

    make_pair(sv);
    CHECK(io_read_exact(sv[0], b, 0) == IO_OK);       /* zero bytes: trivially ok */
    W(write(sv[1], "abc", 3));
    close(sv[1]);
    CHECK(io_read_exact(sv[0], b, 10) == IO_TRUNC);   /* some bytes, then EOF */
    CHECK(io_read_exact(sv[0], b, 10) == IO_EOF);     /* no bytes, EOF       */
    close(sv[0]);
    CHECK(io_read_exact(-1, b, 1) == IO_ERR);         /* EBADF               */
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);

    RUN(test_read_one_known_frame);
    RUN(test_zero_length_payload);
    RUN(test_unknown_frame_is_skipped_and_next_frame_processed);
    RUN(test_unknown_type_zero_and_255_and_empty_payload);
    RUN(test_huge_unknown_frame_is_skipped_in_constant_memory);
    RUN(test_skip_sink_sees_every_skipped_byte);
    RUN(test_frame_split_across_many_reads_byte_by_byte);
    RUN(test_frame_split_at_every_offset);
    RUN(test_many_frames_in_one_write);
    RUN(test_clean_eof_vs_truncation);
    RUN(test_bad_magic_and_version);
    RUN(test_oversized_known_frame_is_rejected_without_reading_payload);
    RUN(test_max_size_known_frame_is_accepted);
    RUN(test_invalid_flags_keep_stream_in_sync);
    RUN(test_frame_write_roundtrip_and_limits);
    RUN(test_partial_writes_with_tiny_send_buffer);
    RUN(test_eintr_is_retried);
    RUN(test_io_read_exact_result_codes);

    printf("\n%d checks, %d failed\n", checks, failures);
    if (failures) {
        printf("UNIT TESTS FAILED (frame/io)\n");
        return 1;
    }
    printf("ALL FRAME/IO UNIT TESTS PASSED\n");
    return 0;
}
