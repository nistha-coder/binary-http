/*
 * network.h - server-side TCP socket helpers (no protocol knowledge here).
 */
#ifndef NETWORK_H
#define NETWORK_H

/* Create a TCP socket bound to `port` on all interfaces and listening.
 * Returns the listening fd, or -1 (errno / message on stderr set). */
int create_server_socket(const char *port);

/* Per-connection tuning: TCP_NODELAY plus receive/send timeouts (seconds). */
void tune_connection(int fd, int recv_timeout_s, int send_timeout_s);

/* Close a connection politely after we have sent a final frame: half-close the
 * write side, drain what the peer still sends (bounded), then close. This
 * avoids a TCP RST that could destroy the final ERROR frame in flight. */
void linger_close(int fd);

#endif
