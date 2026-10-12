/*
 * test_shortio.c - deterministic partial reads, partial writes and EINTR.
 *
 * This binary is linked with  -Wl,--wrap=read -Wl,--wrap=write  so every
 * read()/write() issued by io.c / frame.c goes through the wrappers below,
 * which can
 *   - cap each transfer at N bytes   (a "short read" / "short write"), and
 *   - fail every second call with EINTR.
 * That makes "TCP is a byte stream" testable without relying on timing.
 */
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "frame.h"
#include "io.h"
#include "protocol.h"

ssize_t __real_read(int fd, void *buf, size_t n);
ssize_t __real_write(int fd, const void *buf, size_t n);

static size_t read_cap = 0, write_cap = 0;     /* 0 = no cap */
static int inject_eintr = 0;
static unsigned read_calls = 0, write_calls = 0, eintr_hits = 0;

ssize_t __wrap_read(int fd, void *buf, size_t n)
{
    read_calls++;
    if (inject_eintr && read_calls % 2 == 0) {
        eintr_hits++;
        errno = EINTR;
        return -1;
    }
    if (read_cap && n > read_cap)
        n = read_cap;
    return __real_read(fd, buf, n);
}

ssize_t __wrap_write(int fd, const void *buf, size_t n)
{
    write_calls++;
    if (inject_eintr && write_calls % 2 == 0) {
        eintr_hits++;
        errno = EINTR;
        return -1;
    }
    if (write_cap && n > write_cap)
        n = write_cap;
    return __real_write(fd, buf, n);
}

static int failures = 0, checks = 0;

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

static void reset(size_t rcap, size_t wcap, int eintr)
{
    read_cap = rcap; write_cap = wcap; inject_eintr = eintr;
    read_calls = write_calls = eintr_hits = 0;
}

/* A connected loopback TCP pair (TCP coalesces tiny writes like a real
 * connection does; AF_UNIX would allocate one buffer per 1-byte write). */
static void make_pair(int sv[2])
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int l = socket(AF_INET, SOCK_STREAM, 0);

    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (l < 0 || bind(l, (struct sockaddr *)&a, sizeof a) != 0 || listen(l, 1) != 0 ||
        getsockname(l, (struct sockaddr *)&a, &al) != 0) {
        perror("loopback listen");
        exit(2);
    }
    sv[1] = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(sv[1], (struct sockaddr *)&a, sizeof a) != 0) { perror("connect"); exit(2); }
    sv[0] = accept(l, NULL, NULL);
    if (sv[0] < 0) { perror("accept"); exit(2); }
    close(l);
}

static void fill(uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        p[i] = (uint8_t)(i * 131 + (i >> 5));
}

/* every combination of read cap, write cap and EINTR injection */
static void test_write_then_read_with_short_transfers(void)
{
    static const size_t caps[] = { 1, 2, 3, 7, 12, 13, 500, 0 };
    uint8_t body[MAX_FRAME_PAYLOAD], got[MAX_FRAME_PAYLOAD], raw[12];
    size_t wi, ri;
    int eintr;

    fill(body, sizeof body);
    for (eintr = 0; eintr <= 1; eintr++)
        for (wi = 0; wi < sizeof caps / sizeof caps[0]; wi++)
            for (ri = 0; ri < sizeof caps / sizeof caps[0]; ri++) {
                int sv[2];
                FrameHeader h;
                size_t len = (wi * 7 + ri * 3) % 2 ? sizeof body : 1000;

                make_pair(sv);
                reset(0, caps[wi], eintr);
                CHECK(frame_write(sv[1], FRAME_DATA, FLAG_END_STREAM, body, len) == 0);
                if (caps[wi] != 0 && caps[wi] < 100)
                    CHECK(write_calls > (len + 12) / caps[wi] / 2);   /* many short writes really happened */

                reset(caps[ri], 0, eintr);
                CHECK(frame_read(sv[0], &h, raw, got, NULL, NULL) == FR_OK);
                CHECK(h.payload_length == len && memcmp(got, body, len) == 0);
                if (eintr)
                    CHECK(eintr_hits > 0);
                close(sv[0]); close(sv[1]);
            }
    reset(0, 0, 0);
}

static void test_unknown_frame_skipped_with_one_byte_reads(void)
{
    uint8_t all[512], junk[200], got[MAX_FRAME_PAYLOAD], raw[12];
    int sv[2];
    FrameHeader h;
    size_t n = 0;

    memset(junk, 0xEE, sizeof junk);
    n += build_frame(all + n, sizeof all - n, FRAME_DATA, 0, (const uint8_t *)"one", 3);
    n += build_frame(all + n, sizeof all - n, 0x55, 0x1234, junk, sizeof junk);
    n += build_frame(all + n, sizeof all - n, FRAME_DATA, FLAG_END_STREAM, (const uint8_t *)"two", 3);

    make_pair(sv);
    reset(0, 0, 0);
    CHECK(write(sv[1], all, n) == (ssize_t)n);
    reset(1, 0, 1);                                    /* 1-byte reads + EINTR */
    CHECK(frame_read(sv[0], &h, raw, got, NULL, NULL) == FR_OK);
    CHECK(frame_read(sv[0], &h, raw, got, NULL, NULL) == FR_SKIPPED);
    CHECK(h.type == 0x55 && h.payload_length == 200);
    CHECK(frame_read(sv[0], &h, raw, got, NULL, NULL) == FR_OK);
    CHECK(memcmp(got, "two", 3) == 0);
    reset(0, 0, 0);
    close(sv[0]); close(sv[1]);
}

static void test_truncation_detected_with_short_reads(void)
{
    uint8_t buf[64], got[MAX_FRAME_PAYLOAD], raw[12];
    size_t n = build_frame(buf, sizeof buf, FRAME_DATA, 0, (const uint8_t *)"abcdefghij", 10), cut;

    for (cut = 1; cut < n; cut++) {
        int sv[2];
        FrameHeader h;
        make_pair(sv);
        reset(0, 0, 0);
        CHECK(write(sv[1], buf, cut) == (ssize_t)cut);
        close(sv[1]);
        reset(2, 0, 1);
        CHECK(frame_read(sv[0], &h, raw, got, NULL, NULL) == FR_TRUNC);
        close(sv[0]);
    }
    reset(0, 0, 0);
}

static void test_io_helpers_directly(void)
{
    int sv[2];
    uint8_t out[1000], in[1000];
    size_t i;

    for (i = 0; i < sizeof out; i++)
        out[i] = (uint8_t)i;
    make_pair(sv);
    reset(0, 5, 1);
    CHECK(io_write_all(sv[1], out, sizeof out) == 0);
    CHECK(write_calls >= 200);                         /* 1000 bytes / 5 per call */
    reset(3, 0, 1);
    CHECK(io_read_exact(sv[0], in, sizeof in) == IO_OK);
    CHECK(read_calls >= 333);
    CHECK(memcmp(in, out, sizeof out) == 0);
    reset(0, 0, 0);
    close(sv[0]); close(sv[1]);
}

int main(void)
{
    RUN(test_write_then_read_with_short_transfers);
    RUN(test_unknown_frame_skipped_with_one_byte_reads);
    RUN(test_truncation_detected_with_short_reads);
    RUN(test_io_helpers_directly);

    printf("\n%d checks, %d failed\n", checks, failures);
    if (failures) {
        printf("UNIT TESTS FAILED (short I/O)\n");
        return 1;
    }
    printf("ALL SHORT-I/O UNIT TESTS PASSED\n");
    return 0;
}
