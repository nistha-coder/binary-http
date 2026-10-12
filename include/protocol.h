/*
 * protocol.h - BHTP/1 wire format. SPEC.md is the source of truth.
 *
 * A frame is a fixed 12-byte header followed by `payload_length` payload
 * bytes. All multi-byte integers are big-endian (network byte order).
 *
 *   offset  size  field
 *   0       4     magic           "BHTP"  (0x42 0x48 0x54 0x50)
 *   4       1     version         1
 *   5       1     type            1 REQUEST, 2 RESPONSE, 3 DATA, 4 ERROR
 *   6       2     flags           bit 0 = END_STREAM, other bits reserved
 *   8       4     payload_length  bytes that follow the header
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define BHTP_MAGIC               "BHTP"
#define BHTP_MAGIC_SIZE          4
#define BHTP_VERSION             1
#define FRAME_HEADER_SIZE        12

#define MAX_FRAME_PAYLOAD        16384   /* limit for KNOWN frame types       */
#define MAX_PATH_LENGTH          4096
#define MAX_HEADERS              32
#define MAX_HEADER_NAME_LENGTH   255
#define MAX_HEADER_VALUE_LENGTH  4096
#define MAX_REASON_LENGTH        256
#define STATIC_HEADER_COUNT      10

#define FLAG_END_STREAM          0x0001
#define METHOD_GET               0x01

typedef enum {
    FRAME_REQUEST  = 0x01,
    FRAME_RESPONSE = 0x02,
    FRAME_DATA     = 0x03,
    FRAME_ERROR    = 0x04
} FrameType;

typedef struct {
    uint8_t  version;
    uint8_t  type;
    uint16_t flags;
    uint32_t payload_length;
} FrameHeader;

typedef enum {
    BHTP_OK = 0,
    BHTP_ERR_ARGUMENT,       /* NULL pointer / bad argument (programming error) */
    BHTP_ERR_MAGIC,          /* first four bytes are not "BHTP"                 */
    BHTP_ERR_VERSION,        /* version byte is not 1                           */
    BHTP_ERR_TRUNCATED,      /* payload ended before a field was complete       */
    BHTP_ERR_METHOD,         /* method id is not 0x01                           */
    BHTP_ERR_PATH_LENGTH,    /* path length 0 or > MAX_PATH_LENGTH              */
    BHTP_ERR_HEADER_COUNT,   /* more than MAX_HEADERS headers                   */
    BHTP_ERR_HEADER_ID,      /* header tag in the reserved range 11..255        */
    BHTP_ERR_HEADER_NAME,    /* literal name empty / illegal characters         */
    BHTP_ERR_VALUE_LENGTH,   /* header value or reason too long                 */
    BHTP_ERR_STATUS,         /* status outside 100..599                         */
    BHTP_ERR_TRAILING,       /* bytes left after the last field                 */
    BHTP_ERR_BUFFER          /* (encoder) output buffer too small               */
} BhtpError;

/*
 * One header field. `name` is always filled in (copied from the static table
 * when name_id != 0). `value` points into the buffer that was decoded, or into
 * caller memory when encoding; it is never copied.
 */
typedef struct {
    uint8_t        name_id;                           /* 0 = literal, 1..10 */
    char           name[MAX_HEADER_NAME_LENGTH + 1];
    const uint8_t *value;
    uint16_t       value_length;
} Header;

typedef struct {
    uint8_t        method;
    const uint8_t *path;
    uint16_t       path_length;
    uint8_t        header_count;
    Header         headers[MAX_HEADERS];
} Request;

typedef struct {
    uint16_t status_code;
    uint8_t  header_count;
    Header   headers[MAX_HEADERS];
} Response;

typedef struct {
    uint16_t       status_code;
    const uint8_t *reason;
    size_t         reason_length;
} ErrorMessage;

/* ---- big-endian integer helpers ------------------------------------ */
void     write_u16(uint8_t *buffer, uint16_t value);
void     write_u32(uint8_t *buffer, uint32_t value);
uint16_t read_u16(const uint8_t *buffer);
uint32_t read_u32(const uint8_t *buffer);

/* ---- frame header --------------------------------------------------- */
int         encode_frame_header(uint8_t *buffer, const FrameHeader *header);
/* Decodes the 12 bytes and validates magic + version only (returns BHTP_OK,
 * BHTP_ERR_MAGIC or BHTP_ERR_VERSION). Type, flags and length are validated by
 * frame_read(). *header is filled in unless the magic is wrong. */
BhtpError   decode_frame_header(const uint8_t *buffer, FrameHeader *header);
int         frame_type_known(uint8_t type);
const char *frame_type_name(uint8_t type);
uint16_t    frame_allowed_flags(uint8_t type);

/* header + payload into out; returns total length, or 0 on overflow. Accepts
 * any `type` (tests use this to build unknown frames). */
size_t build_frame(uint8_t *out, size_t capacity, uint8_t type, uint16_t flags,
                   const uint8_t *payload, size_t payload_length);

/* ---- header fields -------------------------------------------------- */
extern const char *const static_header_names[STATIC_HEADER_COUNT + 1];
unsigned      static_header_id(const char *name);          /* 0 if not static */
/* Fills h. Picks the static tag automatically when the name is in the table;
 * that is the whole "compression". Returns 0, or -1 on invalid name/value. */
int           header_set(Header *h, const char *name, const void *value, size_t length);
const Header *find_header(const Header *headers, unsigned count, const char *name);

/* ---- payload codecs ------------------------------------------------- */
const char *bhtp_error_text(BhtpError e);

BhtpError encode_request(const Request *r, uint8_t *out, size_t capacity, size_t *length);
BhtpError decode_request(const uint8_t *buffer, size_t length, Request *r);
BhtpError encode_response(const Response *r, uint8_t *out, size_t capacity, size_t *length);
BhtpError decode_response(const uint8_t *buffer, size_t length, Response *r);
BhtpError encode_error(uint16_t status, const char *reason, uint8_t *out,
                       size_t capacity, size_t *length);
BhtpError decode_error(const uint8_t *buffer, size_t length, ErrorMessage *e);

#endif
