/*
 * test_protocol.c - unit tests for the BHTP/1 codec (no sockets involved).
 *
 *   build + run:  make test-unit      (see Makefile)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"

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

static int bytes_eq(const uint8_t *a, const uint8_t *b, size_t n)
{
    return memcmp(a, b, n) == 0;
}

/* ------------------------------------------------------------------ */

static void test_integers_big_endian(void)
{
    uint8_t b[4];
    write_u16(b, 0x1234);
    CHECK(b[0] == 0x12 && b[1] == 0x34);
    CHECK(read_u16(b) == 0x1234);
    write_u32(b, 0x12345678u);
    CHECK(b[0] == 0x12 && b[1] == 0x34 && b[2] == 0x56 && b[3] == 0x78);
    CHECK(read_u32(b) == 0x12345678u);
    write_u32(b, 0xFFFFFFFFu);
    CHECK(read_u32(b) == 0xFFFFFFFFu);
}

static void test_frame_header_golden_bytes(void)
{
    /* REQUEST, no flags, payload 45 bytes */
    static const uint8_t golden[12] = {
        0x42, 0x48, 0x54, 0x50, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2d
    };
    FrameHeader h = { BHTP_VERSION, FRAME_REQUEST, 0, 45 }, d;
    uint8_t out[12];

    CHECK(encode_frame_header(out, &h) == 0);
    CHECK(bytes_eq(out, golden, 12));
    CHECK(decode_frame_header(golden, &d) == BHTP_OK);
    CHECK(d.version == 1 && d.type == FRAME_REQUEST && d.flags == 0 &&
          d.payload_length == 45);

    /* DATA + END_STREAM, 16384 bytes */
    h.type = FRAME_DATA; h.flags = FLAG_END_STREAM; h.payload_length = 16384;
    encode_frame_header(out, &h);
    CHECK(out[5] == 0x03 && out[6] == 0x00 && out[7] == 0x01);
    CHECK(out[8] == 0x00 && out[9] == 0x00 && out[10] == 0x40 && out[11] == 0x00);
}

static void test_frame_header_rejects_bad_magic_and_version(void)
{
    uint8_t buf[12] = { 'B', 'H', 'T', 'P', 1, 1, 0, 0, 0, 0, 0, 0 };
    FrameHeader h;

    CHECK(decode_frame_header(buf, &h) == BHTP_OK);
    buf[0] = 'X';
    CHECK(decode_frame_header(buf, &h) == BHTP_ERR_MAGIC);
    buf[0] = 'B'; buf[3] = 'p';                      /* case matters */
    CHECK(decode_frame_header(buf, &h) == BHTP_ERR_MAGIC);
    buf[3] = 'P'; buf[4] = 2;
    CHECK(decode_frame_header(buf, &h) == BHTP_ERR_VERSION);
    buf[4] = 0;
    CHECK(decode_frame_header(buf, &h) == BHTP_ERR_VERSION);
    CHECK(decode_frame_header(NULL, &h) == BHTP_ERR_ARGUMENT);
}

static void test_frame_type_table(void)
{
    CHECK(frame_type_known(1) && frame_type_known(2) &&
          frame_type_known(3) && frame_type_known(4));
    CHECK(!frame_type_known(0) && !frame_type_known(5) && !frame_type_known(255));
    CHECK(strcmp(frame_type_name(FRAME_REQUEST), "REQUEST") == 0);
    CHECK(strcmp(frame_type_name(0x7f), "UNKNOWN") == 0);
    CHECK(frame_allowed_flags(FRAME_REQUEST) == 0);
    CHECK(frame_allowed_flags(FRAME_RESPONSE) == FLAG_END_STREAM);
    CHECK(frame_allowed_flags(FRAME_DATA) == FLAG_END_STREAM);
    CHECK(frame_allowed_flags(FRAME_ERROR) == 0);
}

static void test_build_frame(void)
{
    uint8_t out[64];
    const uint8_t pl[3] = { 1, 2, 3 };
    size_t n = build_frame(out, sizeof out, 0x7f, 0x8000, pl, 3);   /* unknown type OK */

    CHECK(n == 15);
    CHECK(out[5] == 0x7f && out[6] == 0x80 && out[11] == 3);
    CHECK(out[12] == 1 && out[14] == 3);
    CHECK(build_frame(out, 14, 1, 0, pl, 3) == 0);                  /* too small */
    CHECK(build_frame(out, sizeof out, 1, 0, pl, MAX_FRAME_PAYLOAD + 1) == 0);
}

/* ------------------------------------------------------------------ */
/* static header table                                                 */

static void test_static_table_is_exactly_ten_and_stable(void)
{
    static const char *const expect[11] = {
        NULL, "host", "user-agent", "content-type", "content-length",
        "accept-encoding", "accept", "server", "connection",
        "last-modified", "cache-control"
    };
    unsigned i, j;

    CHECK(STATIC_HEADER_COUNT == 10);
    for (i = 1; i <= 10; i++) {
        CHECK(strcmp(static_header_names[i], expect[i]) == 0);
        CHECK(static_header_id(expect[i]) == i);
        for (j = i + 1; j <= 10; j++)                 /* no duplicates */
            CHECK(strcmp(static_header_names[i], static_header_names[j]) != 0);
    }
    CHECK(static_header_id("x-custom") == 0);
    CHECK(static_header_id("Host") == 0);             /* case-sensitive lookup */
    CHECK(static_header_id("") == 0);
}

static void test_header_set_picks_static_or_literal(void)
{
    Header h;
    CHECK(header_set(&h, "content-type", "text/html", 9) == 0);
    CHECK(h.name_id == 3 && h.value_length == 9);
    CHECK(header_set(&h, "x-demo", "1", 1) == 0);
    CHECK(h.name_id == 0 && strcmp(h.name, "x-demo") == 0);
    CHECK(header_set(&h, "X-Upper", "1", 1) == -1);
    CHECK(header_set(&h, "bad name", "1", 1) == -1);
    CHECK(header_set(&h, "", "1", 1) == -1);
    CHECK(header_set(&h, "x", NULL, 5) == -1);
}

/* ------------------------------------------------------------------ */
/* request / response / error: golden bytes and round trips            */

static void test_request_golden_bytes(void)
{
    /* GET /a   host: h   x-a: 1 */
    static const uint8_t golden[] = {
        0x01,                          /* method GET            */
        0x00, 0x02, '/', 'a',          /* path_length=2, "/a"   */
        0x02,                          /* header_count = 2      */
        0x01, 0x00, 0x01, 'h',         /* static #1 host        */
        0x00, 0x03, 'x', '-', 'a',     /* literal name, len 3   */
        0x00, 0x01, '1'                /* value_length=1, "1"   */
    };
    Request r;
    uint8_t out[128];
    size_t n = 0;

    memset(&r, 0, sizeof r);
    r.method = METHOD_GET;
    r.path = (const uint8_t *)"/a";
    r.path_length = 2;
    r.header_count = 2;
    header_set(&r.headers[0], "host", "h", 1);
    header_set(&r.headers[1], "x-a", "1", 1);

    CHECK(encode_request(&r, out, sizeof out, &n) == BHTP_OK);
    CHECK(n == sizeof golden);
    CHECK(bytes_eq(out, golden, sizeof golden));
}

static void test_request_roundtrip(void)
{
    Request a, b;
    uint8_t out[MAX_FRAME_PAYLOAD];
    size_t n = 0;
    unsigned i;
    char big[MAX_HEADER_VALUE_LENGTH];

    memset(big, 'v', sizeof big);
    memset(&a, 0, sizeof a);
    a.method = METHOD_GET;
    a.path = (const uint8_t *)"/dir/file.txt";
    a.path_length = 13;
    a.header_count = 6;
    header_set(&a.headers[0], "host", "localhost:9000", 14);
    header_set(&a.headers[1], "user-agent", "t/1", 3);
    header_set(&a.headers[2], "accept", "*/*", 3);
    header_set(&a.headers[3], "x-custom-thing", "abc", 3);
    header_set(&a.headers[4], "x-empty", "", 0);
    header_set(&a.headers[5], "x-big", big, sizeof big);

    CHECK(encode_request(&a, out, sizeof out, &n) == BHTP_OK);
    CHECK(decode_request(out, n, &b) == BHTP_OK);
    CHECK(b.method == a.method && b.path_length == a.path_length);
    CHECK(memcmp(b.path, a.path, a.path_length) == 0);
    CHECK(b.header_count == a.header_count);
    for (i = 0; i < a.header_count; i++) {
        CHECK(strcmp(a.headers[i].name, b.headers[i].name) == 0);
        CHECK(a.headers[i].name_id == b.headers[i].name_id);
        CHECK(a.headers[i].value_length == b.headers[i].value_length);
        CHECK(memcmp(a.headers[i].value, b.headers[i].value, a.headers[i].value_length) == 0);
    }
}

static void test_request_edge_cases(void)
{
    Request r;
    uint8_t out[MAX_FRAME_PAYLOAD], p4096[MAX_PATH_LENGTH + 1];
    size_t n = 0;
    unsigned i;

    memset(p4096, 'a', sizeof p4096);
    p4096[0] = '/';

    /* maximum path length is accepted */
    memset(&r, 0, sizeof r);
    r.method = METHOD_GET; r.path = p4096; r.path_length = MAX_PATH_LENGTH;
    CHECK(encode_request(&r, out, sizeof out, &n) == BHTP_OK);
    CHECK(decode_request(out, n, &r) == BHTP_OK && r.path_length == MAX_PATH_LENGTH);

    /* one more is not */
    memset(&r, 0, sizeof r);
    r.method = METHOD_GET; r.path = p4096; r.path_length = MAX_PATH_LENGTH + 1;
    CHECK(encode_request(&r, out, sizeof out, &n) == BHTP_ERR_PATH_LENGTH);

    /* zero-length path */
    r.path_length = 0;
    CHECK(encode_request(&r, out, sizeof out, &n) == BHTP_ERR_PATH_LENGTH);

    /* exactly 32 headers is fine, 33 is not */
    memset(&r, 0, sizeof r);
    r.method = METHOD_GET; r.path = (const uint8_t *)"/"; r.path_length = 1;
    for (i = 0; i < MAX_HEADERS; i++)
        header_set(&r.headers[i], "x-n", "v", 1);
    r.header_count = MAX_HEADERS;
    CHECK(encode_request(&r, out, sizeof out, &n) == BHTP_OK);
    CHECK(decode_request(out, n, &r) == BHTP_OK && r.header_count == MAX_HEADERS);
    memset(&r, 0, sizeof r);
    r.method = METHOD_GET; r.path = (const uint8_t *)"/"; r.path_length = 1;
    r.header_count = MAX_HEADERS + 1;
    CHECK(encode_request(&r, out, sizeof out, &n) == BHTP_ERR_HEADER_COUNT);

    /* only GET exists */
    memset(&r, 0, sizeof r);
    r.method = 2; r.path = (const uint8_t *)"/"; r.path_length = 1;
    CHECK(encode_request(&r, out, sizeof out, &n) == BHTP_ERR_METHOD);
}

static void test_response_roundtrip(void)
{
    Response a, b;
    uint8_t out[512];
    size_t n = 0;
    unsigned i;

    memset(&a, 0, sizeof a);
    a.status_code = 200;
    a.header_count = 3;
    header_set(&a.headers[0], "content-type", "text/html", 9);
    header_set(&a.headers[1], "content-length", "233", 3);
    header_set(&a.headers[2], "x-trace", "abc", 3);

    CHECK(encode_response(&a, out, sizeof out, &n) == BHTP_OK);
    CHECK(out[0] == 0x00 && out[1] == 200);          /* status is u16 BE */
    CHECK(decode_response(out, n, &b) == BHTP_OK);
    CHECK(b.status_code == 200 && b.header_count == 3);
    for (i = 0; i < 3; i++) {
        CHECK(strcmp(a.headers[i].name, b.headers[i].name) == 0);
        CHECK(memcmp(a.headers[i].value, b.headers[i].value, a.headers[i].value_length) == 0);
    }

    /* every status 100..599 round-trips; the edges outside do not encode */
    for (i = 100; i <= 599; i++) {
        Response s;
        memset(&s, 0, sizeof s);
        s.status_code = (uint16_t)i;
        CHECK(encode_response(&s, out, sizeof out, &n) == BHTP_OK);
        CHECK(decode_response(out, n, &b) == BHTP_OK && b.status_code == i);
    }
    memset(&a, 0, sizeof a);
    a.status_code = 99;
    CHECK(encode_response(&a, out, sizeof out, &n) == BHTP_ERR_STATUS);
    a.status_code = 600;
    CHECK(encode_response(&a, out, sizeof out, &n) == BHTP_ERR_STATUS);
}

static void test_error_roundtrip(void)
{
    uint8_t out[512], max[MAX_REASON_LENGTH + 2];
    size_t n = 0;
    ErrorMessage e;

    CHECK(encode_error(400, "bad magic", out, sizeof out, &n) == BHTP_OK);
    CHECK(n == 2 + 9 && out[0] == 0x01 && out[1] == 0x90);
    CHECK(decode_error(out, n, &e) == BHTP_OK);
    CHECK(e.status_code == 400 && e.reason_length == 9 &&
          memcmp(e.reason, "bad magic", 9) == 0);

    CHECK(encode_error(500, "", out, sizeof out, &n) == BHTP_OK && n == 2);
    CHECK(decode_error(out, n, &e) == BHTP_OK && e.reason_length == 0);

    memset(max, 'x', sizeof max);
    max[MAX_REASON_LENGTH] = '\0';
    CHECK(encode_error(400, (char *)max, out, sizeof out, &n) == BHTP_OK);
    max[MAX_REASON_LENGTH] = 'x'; max[MAX_REASON_LENGTH + 1] = '\0';
    CHECK(encode_error(400, (char *)max, out, sizeof out, &n) == BHTP_ERR_VALUE_LENGTH);
    CHECK(encode_error(99, "x", out, sizeof out, &n) == BHTP_ERR_STATUS);
    CHECK(decode_error(out, 1, &e) == BHTP_ERR_TRUNCATED);
}

/* ------------------------------------------------------------------ */
/* malformed input                                                     */

static size_t make_valid_request(uint8_t *out, size_t cap)
{
    Request r;
    size_t n = 0;
    memset(&r, 0, sizeof r);
    r.method = METHOD_GET;
    r.path = (const uint8_t *)"/index.html";
    r.path_length = 11;
    r.header_count = 3;
    header_set(&r.headers[0], "host", "localhost:9000", 14);
    header_set(&r.headers[1], "accept", "*/*", 3);
    header_set(&r.headers[2], "x-demo", "hello", 5);
    if (encode_request(&r, out, cap, &n) != BHTP_OK)
        abort();
    return n;
}

static void test_every_truncation_of_a_request_is_rejected(void)
{
    uint8_t out[256];
    size_t n = make_valid_request(out, sizeof out), cut;
    Request r;

    CHECK(decode_request(out, n, &r) == BHTP_OK);
    for (cut = 0; cut < n; cut++)
        CHECK(decode_request(out, cut, &r) != BHTP_OK);   /* never "ok", never a crash */
}

static void test_trailing_bytes_are_rejected(void)
{
    uint8_t out[256];
    size_t n = make_valid_request(out, 255);
    Request r;

    out[n] = 0x00;
    CHECK(decode_request(out, n + 1, &r) == BHTP_ERR_TRAILING);
}

static void test_specific_malformed_requests(void)
{
    Request r;
    static const uint8_t bad_method[]  = { 0x02, 0x00, 0x01, '/', 0x00 };
    static const uint8_t path_zero[]   = { 0x01, 0x00, 0x00, 0x00 };
    static const uint8_t path_huge[]   = { 0x01, 0xff, 0xff, '/', 0x00 };
    static const uint8_t path_short[]  = { 0x01, 0x00, 0x05, '/', 'a', 0x00 };
    static const uint8_t no_count[]    = { 0x01, 0x00, 0x01, '/' };
    static const uint8_t count_33[]    = { 0x01, 0x00, 0x01, '/', 33 };
    static const uint8_t tag_11[]      = { 0x01, 0x00, 0x01, '/', 1, 11, 0x00, 0x00 };
    static const uint8_t tag_255[]     = { 0x01, 0x00, 0x01, '/', 1, 255, 0x00, 0x00 };
    static const uint8_t lit_empty[]   = { 0x01, 0x00, 0x01, '/', 1, 0, 0, 0x00, 0x00 };
    static const uint8_t lit_upper[]   = { 0x01, 0x00, 0x01, '/', 1, 0, 1, 'A', 0x00, 0x00 };
    static const uint8_t lit_space[]   = { 0x01, 0x00, 0x01, '/', 1, 0, 1, ' ', 0x00, 0x00 };
    static const uint8_t lit_trunc[]   = { 0x01, 0x00, 0x01, '/', 1, 0, 9, 'a', 'b' };
    static const uint8_t val_huge[]    = { 0x01, 0x00, 0x01, '/', 1, 1, 0xff, 0xff };
    static const uint8_t val_4097[]    = { 0x01, 0x00, 0x01, '/', 1, 1, 0x10, 0x01 };
    static const uint8_t val_short[]   = { 0x01, 0x00, 0x01, '/', 1, 1, 0x00, 0x05, 'a' };

#define EXPECT(arr, err) CHECK(decode_request(arr, sizeof arr, &r) == (err))
    EXPECT(bad_method, BHTP_ERR_METHOD);
    EXPECT(path_zero,  BHTP_ERR_PATH_LENGTH);
    EXPECT(path_huge,  BHTP_ERR_PATH_LENGTH);
    EXPECT(path_short, BHTP_ERR_TRUNCATED);
    EXPECT(no_count,   BHTP_ERR_TRUNCATED);
    EXPECT(count_33,   BHTP_ERR_HEADER_COUNT);
    EXPECT(tag_11,     BHTP_ERR_HEADER_ID);
    EXPECT(tag_255,    BHTP_ERR_HEADER_ID);
    EXPECT(lit_empty,  BHTP_ERR_HEADER_NAME);
    EXPECT(lit_upper,  BHTP_ERR_HEADER_NAME);
    EXPECT(lit_space,  BHTP_ERR_HEADER_NAME);
    EXPECT(lit_trunc,  BHTP_ERR_TRUNCATED);
    EXPECT(val_huge,   BHTP_ERR_VALUE_LENGTH);
    EXPECT(val_4097,   BHTP_ERR_VALUE_LENGTH);
    EXPECT(val_short,  BHTP_ERR_TRUNCATED);
#undef EXPECT
    CHECK(decode_request(NULL, 0, &r) == BHTP_ERR_ARGUMENT);
}

static void test_malformed_responses(void)
{
    Response r;
    static const uint8_t status_99[]  = { 0x00, 0x63, 0x00 };
    static const uint8_t status_600[] = { 0x02, 0x58, 0x00 };
    static const uint8_t one_byte[]   = { 0x00 };
    static const uint8_t no_count[]   = { 0x00, 0xc8 };
    static const uint8_t trailing[]   = { 0x00, 0xc8, 0x00, 0x00 };

    CHECK(decode_response(status_99, sizeof status_99, &r) == BHTP_ERR_STATUS);
    CHECK(decode_response(status_600, sizeof status_600, &r) == BHTP_ERR_STATUS);
    CHECK(decode_response(one_byte, sizeof one_byte, &r) == BHTP_ERR_TRUNCATED);
    CHECK(decode_response(no_count, sizeof no_count, &r) == BHTP_ERR_TRUNCATED);
    CHECK(decode_response(trailing, sizeof trailing, &r) == BHTP_ERR_TRAILING);
}

static void test_encoder_never_overflows_small_buffers(void)
{
    uint8_t big[256], small[256];
    size_t n = make_valid_request(big, sizeof big), cap, got = 0;
    Request r;

    memset(&r, 0, sizeof r);
    r.method = METHOD_GET;
    r.path = (const uint8_t *)"/index.html";
    r.path_length = 11;
    r.header_count = 3;
    header_set(&r.headers[0], "host", "localhost:9000", 14);
    header_set(&r.headers[1], "accept", "*/*", 3);
    header_set(&r.headers[2], "x-demo", "hello", 5);

    for (cap = 0; cap < n; cap++) {
        memset(small, 0xAA, sizeof small);
        CHECK(encode_request(&r, small, cap, &got) == BHTP_ERR_BUFFER);
        CHECK(small[cap] == 0xAA);                     /* no write past cap */
    }
    CHECK(encode_request(&r, small, n, &got) == BHTP_OK && got == n);
}

/* Deterministic pseudo-random garbage into every decoder. Meaningful under
 * `make asan` (AddressSanitizer + UBSan): any out-of-bounds read aborts. */
static uint32_t rng_state = 12345;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void test_random_garbage_never_crashes_decoders(void)
{
    unsigned iter;
    uint8_t *buf;

    for (iter = 0; iter < 200000; iter++) {
        size_t n = rng() % 80, i;
        Request rq; Response rs; ErrorMessage em; FrameHeader fh;

        buf = malloc(n ? n : 1);                    /* exact size: ASan sees overreads */
        for (i = 0; i < n; i++)
            buf[i] = (uint8_t)rng();
        if (iter % 3 == 0 && n > 3) { buf[0] = 1; buf[1] = 0; buf[2] = (uint8_t)(rng() % 20); }
        if (iter % 3 == 1 && n > 3) { buf[3] = (uint8_t)(rng() % 34); }
        (void)decode_request(buf, n, &rq);
        (void)decode_response(buf, n, &rs);
        (void)decode_error(buf, n, &em);
        if (n >= FRAME_HEADER_SIZE)
            (void)decode_frame_header(buf, &fh);
        free(buf);
    }
    CHECK(1);                                       /* reaching here is the test */
}

int main(void)
{
    RUN(test_integers_big_endian);
    RUN(test_frame_header_golden_bytes);
    RUN(test_frame_header_rejects_bad_magic_and_version);
    RUN(test_frame_type_table);
    RUN(test_build_frame);
    RUN(test_static_table_is_exactly_ten_and_stable);
    RUN(test_header_set_picks_static_or_literal);
    RUN(test_request_golden_bytes);
    RUN(test_request_roundtrip);
    RUN(test_request_edge_cases);
    RUN(test_response_roundtrip);
    RUN(test_error_roundtrip);
    RUN(test_every_truncation_of_a_request_is_rejected);
    RUN(test_trailing_bytes_are_rejected);
    RUN(test_specific_malformed_requests);
    RUN(test_malformed_responses);
    RUN(test_encoder_never_overflows_small_buffers);
    RUN(test_random_garbage_never_crashes_decoders);

    printf("\n%d checks, %d failed\n", checks, failures);
    if (failures) {
        printf("UNIT TESTS FAILED (protocol)\n");
        return 1;
    }
    printf("ALL PROTOCOL UNIT TESTS PASSED\n");
    return 0;
}
