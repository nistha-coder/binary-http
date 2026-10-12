#!/usr/bin/env python3
"""
pyfetch.py - a small BHTP/1 command-line client in Python, written from SPEC.md
alone. It exists so the server can be demonstrated and verified without any
other program: "a server that only works against your own client is an
implementation, not a protocol".

    python3 tools/pyfetch.py [-v] host:port/path [/more/paths ...]

Same behaviour as bcurl: ONE connection for all paths, bodies to stdout,
-v prints a hex dump of every frame to stderr, and the exit status is
0 = all below 400, 1 = a 4xx/5xx response, 2 = network/protocol error, 64 = usage.
"""
import os
import re
import socket
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests"))
import bhtp  # noqa: E402


def dump(direction, n, data):
    ftype, flags, length = data[5], struct.unpack(">H", data[6:8])[0], struct.unpack(">I", data[8:12])[0]
    name = {1: "REQUEST", 2: "RESPONSE", 3: "DATA", 4: "ERROR"}.get(ftype, "UNKNOWN(0x%02x)" % ftype)
    sys.stderr.write("%s FRAME #%d  %s  flags=0x%04x  payload=%d  total=%d\n" % (
        direction, n, name, flags, length, len(data)))
    for off in range(0, min(len(data), 256), 16):
        chunk = data[off:off + 16]
        hexpart = ""
        for i in range(16):
            hexpart += ("%02x " % chunk[i]) if i < len(chunk) else "   "
            if i == 7:
                hexpart += " "
        sys.stderr.write("%04x: %s |%s|\n" % (off, hexpart, "".join(
            chr(c) if 0x20 <= c < 0x7F else "." for c in chunk)))
    if len(data) > 256:
        sys.stderr.write("      ... %d more bytes not shown\n" % (len(data) - 256))
    sys.stderr.write("\n")


def main(argv):
    verbose = False
    if argv and argv[0] == "-v":
        verbose, argv = True, argv[1:]
    m = re.match(r"^([^:/]+):(\d{1,5})(/.*)?$", argv[0]) if argv else None
    if not m:
        sys.stderr.write(__doc__)
        return 64
    host, port, first = m.group(1), int(m.group(2)), m.group(3) or "/"
    paths = [first] + argv[1:]
    if any(not p.startswith("/") for p in paths):
        sys.stderr.write("pyfetch: extra arguments must be paths starting with '/'\n")
        return 64

    try:
        s = socket.create_connection((host, port), timeout=30)
    except OSError as e:
        sys.stderr.write("pyfetch: cannot connect: %s\n" % e)
        return 2
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    code, tx, rx = 0, 0, 0

    def need(n):
        buf = b""
        while len(buf) < n:
            chunk = s.recv(n - len(buf))
            if not chunk:
                raise EOFError("the server closed the connection")
            buf += chunk
        return buf

    try:
        for path in paths:
            req = bhtp.request_frame(path, [
                ("host", "%s:%d" % (host, port)), ("user-agent", "pyfetch/1.0"), ("accept", "*/*"),
                ("accept-encoding", "identity"), ("connection", "keep-alive")])
            tx += 1
            if verbose:
                dump("TX", tx, req)
            s.sendall(req)
            status = None
            while True:
                head = need(12)
                _, ftype, flags, length = bhtp.decode_header_block(head)
                body = need(length)
                rx += 1
                if verbose:
                    dump("RX", rx, head + body)
                if ftype == bhtp.RESPONSE:
                    status, _ = bhtp.decode_response(body)
                elif ftype == bhtp.DATA:
                    sys.stdout.buffer.write(body)
                elif ftype == bhtp.ERROR:
                    sys.stderr.write("pyfetch: server error %d %s\n" % (
                        struct.unpack(">H", body[:2])[0], body[2:].decode("utf-8", "replace")))
                    sys.stdout.flush()
                    return 1
                else:
                    continue                                   # unknown type: skipped
                if flags & bhtp.END_STREAM:
                    break
            sys.stdout.flush()
            if status >= 400:
                sys.stderr.write("pyfetch: GET %s -> %d\n" % (path, status))
                code = 1
    except (EOFError, OSError, bhtp.ProtocolError, struct.error) as e:
        sys.stderr.write("pyfetch: %s\n" % e)
        return 2
    finally:
        s.close()
    if verbose:
        sys.stderr.write("* done: %d request(s) over 1 TCP connection(s)\n" % len(paths))
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
