#include "frame.h"

#include <errno.h>
#include <string.h>

#include "io.h"

const char *frame_status_text(FrameStatus s)
{
    switch (s) {
    case FR_OK:          return "ok";
    case FR_SKIPPED:     return "unknown frame type skipped";
    case FR_EOF:         return "connection closed";
    case FR_TRUNC:       return "connection closed in the middle of a frame";
    case FR_IOERR:       return "I/O error";
    case FR_BAD_MAGIC:   return "bad magic (not a BHTP frame)";
    case FR_BAD_VERSION: return "unsupported protocol version";
    case FR_TOO_BIG:     return "frame payload too large";
    case FR_BAD_FLAGS:   return "undefined flag bits set";
    }
    return "?";
}

/* Read and throw away exactly n bytes using a fixed 4 KiB buffer, so the
 * memory used does not depend on the (untrusted) length. */
static FrameStatus skip_payload(int fd, uint32_t n, SkipSink sink, void *ctx)
{
    uint8_t scratch[4096];

    while (n > 0) {
        size_t chunk = n > sizeof scratch ? sizeof scratch : n;
        IoResult r = io_read_exact(fd, scratch, chunk);
        if (r == IO_EOF || r == IO_TRUNC)
            return FR_TRUNC;            /* EOF inside a frame is never clean */
        if (r == IO_ERR)
            return FR_IOERR;
        if (sink != NULL)
            sink(ctx, scratch, chunk);
        n -= (uint32_t)chunk;
    }
    return FR_SKIPPED;
}

FrameStatus frame_read(int fd, FrameHeader *h, uint8_t raw[FRAME_HEADER_SIZE],
                       uint8_t *payload, SkipSink sink, void *ctx)
{
    IoResult r;
    BhtpError e;

    memset(h, 0, sizeof *h);

    r = io_read_exact(fd, raw, FRAME_HEADER_SIZE);
    if (r == IO_EOF)   return FR_EOF;       /* clean: nothing of a frame seen */
    if (r == IO_TRUNC) return FR_TRUNC;
    if (r == IO_ERR)   return FR_IOERR;

    e = decode_frame_header(raw, h);
    if (e == BHTP_ERR_MAGIC)   return FR_BAD_MAGIC;
    if (e == BHTP_ERR_VERSION) return FR_BAD_VERSION;

    if (!frame_type_known(h->type)) {
        /* Forward compatibility: skip exactly payload_length bytes whatever
         * the type, flags and length are, then carry on with the next frame. */
        return skip_payload(fd, h->payload_length, sink, ctx);
    }

    if (h->payload_length > MAX_FRAME_PAYLOAD)
        return FR_TOO_BIG;                  /* never read or allocate it */

    r = io_read_exact(fd, payload, h->payload_length);
    if (r == IO_ERR)
        return FR_IOERR;
    if (r == IO_EOF || r == IO_TRUNC)
        return FR_TRUNC;

    if ((h->flags & (uint16_t)~frame_allowed_flags(h->type)) != 0)
        return FR_BAD_FLAGS;                /* payload consumed: still in sync */
    return FR_OK;
}

int frame_write(int fd, uint8_t type, uint16_t flags,
                const uint8_t *payload, size_t length)
{
    uint8_t buf[FRAME_HEADER_SIZE + MAX_FRAME_PAYLOAD];
    size_t n = build_frame(buf, sizeof buf, type, flags, payload, length);

    if (n == 0) {
        errno = EMSGSIZE;
        return -1;
    }
    return io_write_all(fd, buf, n);
}
