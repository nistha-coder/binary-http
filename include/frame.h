/*
 * frame.h - reading and writing whole frames on a byte stream.
 */
#ifndef FRAME_H
#define FRAME_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

typedef enum {
    FR_OK = 0,        /* known frame read completely; payload is valid        */
    FR_SKIPPED,       /* unknown frame type: payload read and DISCARDED       */
    FR_EOF,           /* clean EOF on a frame boundary                        */
    FR_TRUNC,         /* EOF in the middle of a frame                         */
    FR_IOERR,         /* system error (errno set; EAGAIN = timeout)           */
    FR_BAD_MAGIC,     /* first 4 bytes are not "BHTP": cannot resynchronise   */
    FR_BAD_VERSION,   /* version is not 1: cannot interpret the rest          */
    FR_TOO_BIG,       /* known type, length > MAX_FRAME_PAYLOAD (NOT read)    */
    FR_BAD_FLAGS      /* known type with undefined flag bits (payload WAS read,
                         so the stream is still in sync)                      */
} FrameStatus;

const char *frame_status_text(FrameStatus s);

/* Called with each chunk of a skipped (unknown) frame's payload; may be NULL. */
typedef void (*SkipSink)(void *ctx, const uint8_t *chunk, size_t n);

/*
 * Read ONE frame from fd.
 *
 *   1. read exactly 12 header bytes into raw[]
 *   2. check magic and version
 *   3. unknown type  -> read and discard exactly payload_length bytes through
 *                       a small fixed buffer, return FR_SKIPPED
 *   4. known type    -> reject payload_length > MAX_FRAME_PAYLOAD (without
 *                       reading or allocating it), read exactly that many bytes
 *                       into payload[] (which must hold MAX_FRAME_PAYLOAD bytes),
 *                       then reject undefined flag bits
 *
 * *h is valid for every status except FR_EOF, FR_IOERR, FR_BAD_MAGIC and a
 * FR_TRUNC that happened inside the 12 header bytes. raw[] always holds
 * whatever header bytes were received.
 */
FrameStatus frame_read(int fd, FrameHeader *h, uint8_t raw[FRAME_HEADER_SIZE],
                       uint8_t *payload, SkipSink sink, void *ctx);

/* Build header+payload and send it with ONE write_all (one TCP segment for
 * small frames). Returns 0 or -1 (errno set). */
int frame_write(int fd, uint8_t type, uint16_t flags,
                const uint8_t *payload, size_t length);

#endif
