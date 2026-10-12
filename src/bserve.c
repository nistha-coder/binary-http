/*
 * bserve - BHTP/1 file server.
 *
 *   usage: bserve <root_directory> <port>
 *
 * This file only parses the command line; everything else lives in server.c.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "server.h"

#define EXIT_USAGE 64

int main(int argc, char **argv)
{
    char *end = NULL;
    long port;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <root_directory> <port>\n", argv[0]);
        return EXIT_USAGE;
    }
    errno = 0;
    port = strtol(argv[2], &end, 10);
    if (errno != 0 || end == argv[2] || *end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "bserve: invalid port '%s' (expected 1-65535)\n", argv[2]);
        return EXIT_USAGE;
    }
    return run_server(argv[1], argv[2]);
}
