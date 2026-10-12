#include "io.h"

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

IoResult io_read_exact(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t got = 0;

    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r > 0) {
            got += (size_t)r;           /* partial read: keep going */
        } else if (r == 0) {
            return got == 0 ? IO_EOF : IO_TRUNC;
        } else if (errno != EINTR) {
            return IO_ERR;
        }                               /* EINTR: just retry */
    }
    return IO_OK;
}

int io_write_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    size_t done = 0;

    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);
        if (w > 0) {
            done += (size_t)w;          /* partial write: offer the rest */
        } else if (w == 0) {
            errno = EIO;
            return -1;
        } else if (errno != EINTR) {
            return -1;
        }
    }
    return 0;
}
