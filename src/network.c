#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "network.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int create_server_socket(const char *port)
{
    struct addrinfo hints, *result = NULL, *rp;
    int fd = -1, on = 1, off = 0, rc;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    rc = getaddrinfo(NULL, port, &hints, &result);
    if (rc != 0) {
        fprintf(stderr, "bserve: getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        if (rp->ai_family == AF_INET6)          /* accept IPv4 clients too */
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0 && listen(fd, 64) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);

    if (fd < 0)
        fprintf(stderr, "bserve: cannot listen on port %s: %s\n", port, strerror(errno));
    return fd;
}

void tune_connection(int fd, int recv_timeout_s, int send_timeout_s)
{
    int on = 1;
    struct timeval tv;

    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    tv.tv_usec = 0;
    tv.tv_sec = recv_timeout_s;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    tv.tv_sec = send_timeout_s;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

void linger_close(int fd)
{
    uint8_t scratch[4096];
    struct timeval tv = { 2, 0 };
    size_t drained = 0;

    shutdown(fd, SHUT_WR);                       /* peer sees EOF after our data */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (drained < (1u << 20)) {
        ssize_t r = read(fd, scratch, sizeof scratch);
        if (r > 0)
            drained += (size_t)r;
        else if (r < 0 && errno == EINTR)
            continue;
        else
            break;                               /* EOF, error or timeout */
    }
    close(fd);
}
