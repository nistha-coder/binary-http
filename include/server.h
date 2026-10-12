/*
 * server.h - the BHTP/1 server: accept loop and per-connection request loop.
 */
#ifndef SERVER_H
#define SERVER_H

#define SERVER_NAME "bserve/1.0"

/* A connection that sends nothing for this long is closed (SPEC section 9). */
#define IDLE_TIMEOUT_SECONDS 300
#define SEND_TIMEOUT_SECONDS 30

/* Serve `root` on TCP `port` until killed. Returns a process exit status. */
int run_server(const char *root, const char *port);

#endif
