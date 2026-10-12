/*
 * io.h - reliable byte-stream I/O on top of read()/write().
 *
 * TCP is a byte stream: one read() may return fewer bytes than asked for, and
 * one write() may accept fewer bytes than offered. These helpers loop until
 * the whole request is satisfied, retry on EINTR, and tell "clean EOF" apart
 * from "EOF in the middle of something".
 */
#ifndef IO_H
#define IO_H

#include <stddef.h>

typedef enum {
    IO_OK = 0,   /* all bytes transferred                                   */
    IO_EOF,      /* peer closed before the FIRST byte (clean boundary)      */
    IO_TRUNC,    /* peer closed after SOME bytes but before all of them     */
    IO_ERR       /* system error, errno is set (EAGAIN = receive timeout)   */
} IoResult;

/* Read exactly n bytes. n == 0 returns IO_OK immediately. */
IoResult io_read_exact(int fd, void *buf, size_t n);

/* Write exactly n bytes. Returns 0 on success, -1 on error (errno set). */
int io_write_all(int fd, const void *buf, size_t n);

#endif
