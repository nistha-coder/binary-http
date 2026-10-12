#include "protocol.h"

#include <string.h>

/* The ten header names the protocol actually sends (tags 1..10). */
const char *const static_header_names[STATIC_HEADER_COUNT + 1] = {
    NULL,
    "host",             /*  1  client -> server */
    "user-agent",       /*  2  client -> server */
    "content-type",     /*  3  server -> client */
    "content-length",   /*  4  server -> client (always) */
    "accept-encoding",  /*  5  client -> server */
    "accept",           /*  6  client -> server */
    "server",           /*  7  server -> client */
    "connection",       /*  8  both directions  */
    "last-modified",    /*  9  server -> client */
    "cache-control",    /* 10  server -> client */
};

/* ------------------------------------------------------------------ */
/* integers                                                            */

void write_u16(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)(value >> 8);
    buffer[1] = (uint8_t)value;
}

void write_u32(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value >> 24);
    buffer[1] = (uint8_t)(value >> 16);
    buffer[2] = (uint8_t)(value >> 8);
    buffer[3] = (uint8_t)value;
}

uint16_t read_u16(const uint8_t *buffer)
{
    return (uint16_t)(((unsigned)buffer[0] << 8) | buffer[1]);
}

uint32_t read_u32(const uint8_t *buffer)
{
    return ((uint32_t)buffer[0] << 24) | ((uint32_t)buffer[1] << 16) |
           ((uint32_t)buffer[2] << 8)  |  (uint32_t)buffer[3];
}

/* ------------------------------------------------------------------ */
/* frame header                                                        */

int frame_type_known(uint8_t type)
{
    return type >= FRAME_REQUEST && type <= FRAME_ERROR;
}

const char *frame_type_name(uint8_t type)
{
    switch (type) {
    case FRAME_REQUEST:  return "REQUEST";
    case FRAME_RESPONSE: return "RESPONSE";
    case FRAME_DATA:     return "DATA";
    case FRAME_ERROR:    return "ERROR";
    default:             return "UNKNOWN";
    }
}

/* Flag bits a sender may set on each KNOWN type. */
uint16_t frame_allowed_flags(uint8_t type)
{
    return (type == FRAME_RESPONSE || type == FRAME_DATA) ? FLAG_END_STREAM : 0;
}

int encode_frame_header(uint8_t *buffer, const FrameHeader *header)
{
    if (buffer == NULL || header == NULL)
        return -1;
    memcpy(buffer, BHTP_MAGIC, BHTP_MAGIC_SIZE);
    buffer[4] = header->version;
    buffer[5] = header->type;
    write_u16(buffer + 6, header->flags);
    write_u32(buffer + 8, header->payload_length);
    return 0;
}

BhtpError decode_frame_header(const uint8_t *buffer, FrameHeader *header)
{
    if (buffer == NULL || header == NULL)
        return BHTP_ERR_ARGUMENT;
    if (memcmp(buffer, BHTP_MAGIC, BHTP_MAGIC_SIZE) != 0)
        return BHTP_ERR_MAGIC;
    header->version        = buffer[4];
    header->type           = buffer[5];
    header->flags          = read_u16(buffer + 6);
    header->payload_length = read_u32(buffer + 8);
    if (header->version != BHTP_VERSION)
        return BHTP_ERR_VERSION;
    return BHTP_OK;
}

size_t build_frame(uint8_t *out, size_t capacity, uint8_t type, uint16_t flags,
                   const uint8_t *payload, size_t payload_length)
{
    FrameHeader h;

    if (payload_length > MAX_FRAME_PAYLOAD ||
        capacity < FRAME_HEADER_SIZE + payload_length)
        return 0;
    h.version = BHTP_VERSION;
    h.type = type;
    h.flags = flags;
    h.payload_length = (uint32_t)payload_length;
    encode_frame_header(out, &h);
    if (payload_length > 0)
        memmove(out + FRAME_HEADER_SIZE, payload, payload_length);
    return FRAME_HEADER_SIZE + payload_length;
}

/* ------------------------------------------------------------------ */
/* header fields                                                       */

const char *bhtp_error_text(BhtpError e)
{
    switch (e) {
    case BHTP_OK:               return "ok";
    case BHTP_ERR_ARGUMENT:     return "bad argument";
    case BHTP_ERR_MAGIC:        return "bad magic";
    case BHTP_ERR_VERSION:      return "unsupported version";
    case BHTP_ERR_TRUNCATED:    return "payload truncated";
    case BHTP_ERR_METHOD:       return "unknown method";
    case BHTP_ERR_PATH_LENGTH:  return "invalid path length";
    case BHTP_ERR_HEADER_COUNT: return "too many headers";
    case BHTP_ERR_HEADER_ID:    return "reserved header tag";
    case BHTP_ERR_HEADER_NAME:  return "invalid header name";
    case BHTP_ERR_VALUE_LENGTH: return "field too long";
    case BHTP_ERR_STATUS:       return "invalid status";
    case BHTP_ERR_TRAILING:     return "trailing bytes in payload";
    case BHTP_ERR_BUFFER:       return "output buffer too small";
    }
    return "unknown error";
}

unsigned static_header_id(const char *name)
{
    unsigned i;
    for (i = 1; i <= STATIC_HEADER_COUNT; i++)
        if (strcmp(static_header_names[i], name) == 0)
            return i;
    return 0;
}

/* Literal header names: 1..255 bytes of [a-z0-9-] (lowercase, like HTTP/2). */
static int name_valid(const char *s, size_t n)
{
    size_t i;
    if (n < 1 || n > MAX_HEADER_NAME_LENGTH)
        return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return 0;
    }
    return 1;
}

int header_set(Header *h, const char *name, const void *value, size_t length)
{
    size_t n;

    if (h == NULL || name == NULL || (value == NULL && length > 0))
        return -1;
    n = strlen(name);
    if (!name_valid(name, n) || length > MAX_HEADER_VALUE_LENGTH)
        return -1;
    memcpy(h->name, name, n + 1);
    h->name_id = (uint8_t)static_header_id(name);   /* the "compression" */
    h->value = value;
    h->value_length = (uint16_t)length;
    return 0;
}

const Header *find_header(const Header *headers, unsigned count, const char *name)
{
    unsigned i;
    for (i = 0; i < count; i++)
        if (strcmp(headers[i].name, name) == 0)
            return &headers[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* encoder                                                             */

typedef struct { uint8_t *p; size_t cap; size_t len; } Writer;

static int w_bytes(Writer *w, const void *b, size_t n)
{
    if (n > w->cap - w->len)
        return 0;
    if (n > 0)
        memcpy(w->p + w->len, b, n);
    w->len += n;
    return 1;
}
static int w_u8(Writer *w, uint8_t v) { return w_bytes(w, &v, 1); }
static int w_u16(Writer *w, uint16_t v)
{
    uint8_t b[2];
    write_u16(b, v);
    return w_bytes(w, b, 2);
}

/*
 * header block := count:u8 { tag:u8 [name_len:u8 name] value_len:u16 value }*
 *   tag 1..10 -> static name: nothing else is sent for the name
 *   tag 0     -> literal name follows, length-prefixed
 */
static BhtpError enc_headers(Writer *w, const Header *hs, unsigned n)
{
    unsigned i;

    if (n > MAX_HEADERS)
        return BHTP_ERR_HEADER_COUNT;
    if (!w_u8(w, (uint8_t)n))
        return BHTP_ERR_BUFFER;

    for (i = 0; i < n; i++) {
        const Header *h = &hs[i];

        if (h->name_id > STATIC_HEADER_COUNT)
            return BHTP_ERR_HEADER_ID;
        if (h->value_length > MAX_HEADER_VALUE_LENGTH)
            return BHTP_ERR_VALUE_LENGTH;

        if (h->name_id != 0) {
            if (!w_u8(w, h->name_id))
                return BHTP_ERR_BUFFER;
        } else {
            size_t nl = strlen(h->name);
            if (!name_valid(h->name, nl))
                return BHTP_ERR_HEADER_NAME;
            if (!w_u8(w, 0) || !w_u8(w, (uint8_t)nl) || !w_bytes(w, h->name, nl))
                return BHTP_ERR_BUFFER;
        }
        if (!w_u16(w, h->value_length) || !w_bytes(w, h->value, h->value_length))
            return BHTP_ERR_BUFFER;
    }
    return BHTP_OK;
}

BhtpError encode_request(const Request *r, uint8_t *out, size_t capacity, size_t *length)
{
    Writer w = { out, capacity, 0 };
    BhtpError e;

    if (r == NULL || out == NULL || length == NULL || r->path == NULL)
        return BHTP_ERR_ARGUMENT;
    if (r->method != METHOD_GET)
        return BHTP_ERR_METHOD;
    if (r->path_length < 1 || r->path_length > MAX_PATH_LENGTH)
        return BHTP_ERR_PATH_LENGTH;
    if (!w_u8(&w, r->method) || !w_u16(&w, r->path_length) ||
        !w_bytes(&w, r->path, r->path_length))
        return BHTP_ERR_BUFFER;
    e = enc_headers(&w, r->headers, r->header_count);
    if (e != BHTP_OK)
        return e;
    *length = w.len;
    return BHTP_OK;
}

BhtpError encode_response(const Response *r, uint8_t *out, size_t capacity, size_t *length)
{
    Writer w = { out, capacity, 0 };
    BhtpError e;

    if (r == NULL || out == NULL || length == NULL)
        return BHTP_ERR_ARGUMENT;
    if (r->status_code < 100 || r->status_code > 599)
        return BHTP_ERR_STATUS;
    if (!w_u16(&w, r->status_code))
        return BHTP_ERR_BUFFER;
    e = enc_headers(&w, r->headers, r->header_count);
    if (e != BHTP_OK)
        return e;
    *length = w.len;
    return BHTP_OK;
}

BhtpError encode_error(uint16_t status, const char *reason, uint8_t *out,
                       size_t capacity, size_t *length)
{
    Writer w = { out, capacity, 0 };
    size_t n;

    if (reason == NULL || out == NULL || length == NULL)
        return BHTP_ERR_ARGUMENT;
    n = strlen(reason);
    if (status < 100 || status > 599)
        return BHTP_ERR_STATUS;
    if (n > MAX_REASON_LENGTH)
        return BHTP_ERR_VALUE_LENGTH;
    if (!w_u16(&w, status) || !w_bytes(&w, reason, n))
        return BHTP_ERR_BUFFER;
    *length = w.len;
    return BHTP_OK;
}

/* ------------------------------------------------------------------ */
/* decoder: every length is checked against the bytes that remain      */

static BhtpError dec_headers(const uint8_t *p, size_t n, size_t *off,
                             uint8_t *count, Header *hs)
{
    unsigned cnt, i;

    if (*off >= n)
        return BHTP_ERR_TRUNCATED;
    cnt = p[(*off)++];
    if (cnt > MAX_HEADERS)
        return BHTP_ERR_HEADER_COUNT;

    for (i = 0; i < cnt; i++) {
        Header *h = &hs[i];
        unsigned tag;
        size_t vl;

        if (*off >= n)
            return BHTP_ERR_TRUNCATED;
        tag = p[(*off)++];

        if (tag == 0) {                              /* literal name */
            size_t nl;
            if (*off >= n)
                return BHTP_ERR_TRUNCATED;
            nl = p[(*off)++];
            if (nl < 1)
                return BHTP_ERR_HEADER_NAME;
            if (n - *off < nl)
                return BHTP_ERR_TRUNCATED;
            if (!name_valid((const char *)p + *off, nl))
                return BHTP_ERR_HEADER_NAME;
            memcpy(h->name, p + *off, nl);
            h->name[nl] = '\0';
            *off += nl;
        } else if (tag <= STATIC_HEADER_COUNT) {     /* static name */
            strcpy(h->name, static_header_names[tag]);
        } else {
            return BHTP_ERR_HEADER_ID;               /* 11..255 reserved */
        }
        h->name_id = (uint8_t)tag;

        if (n - *off < 2)
            return BHTP_ERR_TRUNCATED;
        vl = read_u16(p + *off);
        *off += 2;
        if (vl > MAX_HEADER_VALUE_LENGTH)
            return BHTP_ERR_VALUE_LENGTH;
        if (n - *off < vl)
            return BHTP_ERR_TRUNCATED;
        h->value = p + *off;
        h->value_length = (uint16_t)vl;
        *off += vl;
    }
    *count = (uint8_t)cnt;
    return BHTP_OK;
}

BhtpError decode_request(const uint8_t *p, size_t n, Request *r)
{
    size_t off;
    BhtpError e;

    if (p == NULL || r == NULL)
        return BHTP_ERR_ARGUMENT;
    memset(r, 0, sizeof *r);
    if (n < 3)
        return BHTP_ERR_TRUNCATED;
    r->method = p[0];
    if (r->method != METHOD_GET)
        return BHTP_ERR_METHOD;
    r->path_length = read_u16(p + 1);
    if (r->path_length < 1 || r->path_length > MAX_PATH_LENGTH)
        return BHTP_ERR_PATH_LENGTH;
    off = 3;
    if (n - off < r->path_length)
        return BHTP_ERR_TRUNCATED;
    r->path = p + off;
    off += r->path_length;

    e = dec_headers(p, n, &off, &r->header_count, r->headers);
    if (e != BHTP_OK)
        return e;
    return off == n ? BHTP_OK : BHTP_ERR_TRAILING;
}

BhtpError decode_response(const uint8_t *p, size_t n, Response *r)
{
    size_t off = 2;
    BhtpError e;

    if (p == NULL || r == NULL)
        return BHTP_ERR_ARGUMENT;
    memset(r, 0, sizeof *r);
    if (n < 2)
        return BHTP_ERR_TRUNCATED;
    r->status_code = read_u16(p);
    if (r->status_code < 100 || r->status_code > 599)
        return BHTP_ERR_STATUS;
    e = dec_headers(p, n, &off, &r->header_count, r->headers);
    if (e != BHTP_OK)
        return e;
    return off == n ? BHTP_OK : BHTP_ERR_TRAILING;
}

BhtpError decode_error(const uint8_t *p, size_t n, ErrorMessage *e)
{
    if (p == NULL || e == NULL)
        return BHTP_ERR_ARGUMENT;
    memset(e, 0, sizeof *e);
    if (n < 2)
        return BHTP_ERR_TRUNCATED;
    e->status_code = read_u16(p);
    if (e->status_code < 100 || e->status_code > 599)
        return BHTP_ERR_STATUS;
    e->reason = p + 2;
    e->reason_length = n - 2;
    if (e->reason_length > MAX_REASON_LENGTH)
        return BHTP_ERR_VALUE_LENGTH;
    return BHTP_OK;
}
