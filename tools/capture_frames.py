#!/usr/bin/env python3
"""
capture_frames.py - record the RAW bytes of one request/response exchange.

It is a bare socket client: it sends one REQUEST frame (the same one bcurl
sends: five static-name headers) and then reads from the socket, byte for
byte, until the response is complete (END_STREAM). The bytes are written to
files exactly as they crossed the connection, so no decoder can have changed
them. tools/make_examples.py then verifies and annotates them.

    python3 tools/capture_frames.py HOST PORT PATH OUT_DIR
"""
import os
import socket
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests"))
import bhtp  # noqa: E402


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    host, port, path, out = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    request = bhtp.request_frame(path, [
        ("host", "%s:%d" % (host, port)), ("user-agent", "bcurl/1.0"), ("accept", "*/*"),
        ("accept-encoding", "identity"), ("connection", "keep-alive")])

    s = socket.create_connection((host, port), timeout=10)
    s.sendall(request)
    received = b""

    def need(n):
        nonlocal received
        buf = b""
        while len(buf) < n:
            chunk = s.recv(n - len(buf))
            if not chunk:
                sys.exit("capture_frames: connection closed early")
            buf += chunk
        received += buf
        return buf

    while True:                                   # frame by frame, until END_STREAM
        head = need(bhtp.HEADER_SIZE)
        _, ftype, flags, length = struct.unpack(">BBHI", head[4:12])
        need(length)
        if ftype in (bhtp.RESPONSE, bhtp.DATA) and flags & bhtp.END_STREAM:
            break
    s.close()

    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "request.bin"), "wb") as f:
        f.write(request)
    with open(os.path.join(out, "response.bin"), "wb") as f:
        f.write(received)
    print("captured %d request bytes and %d response bytes" % (len(request), len(received)))


if __name__ == "__main__":
    main()
