"""
bhtp.py - an independent, spec-only implementation of BHTP/1 in Python.

It was written from SPEC.md alone (it shares no code with the C programs) and
is used by the test-suites as a reference client / server. If this module and
the C code ever disagree, one of them contradicts SPEC.md.
"""
import socket
import struct

MAGIC = b"BHTP"
VERSION = 1
HEADER_SIZE = 12
MAX_PAYLOAD = 16384

REQUEST, RESPONSE, DATA, ERROR = 1, 2, 3, 4
END_STREAM = 0x0001
GET = 1

# The ten static header names; tag = index + 1.
STATIC = ["host", "user-agent", "content-type", "content-length",
          "accept-encoding", "accept", "server", "connection",
          "last-modified", "cache-control"]
STATIC_ID = {name: i + 1 for i, name in enumerate(STATIC)}


# --------------------------------------------------------------------------
# encoding
# --------------------------------------------------------------------------

def frame(ftype, payload=b"", flags=0, version=VERSION, magic=MAGIC, length=None):
    """Build one frame. `length` overrides the length field (to lie on purpose)."""
    if length is None:
        length = len(payload)
    return magic + struct.pack(">BBHI", version, ftype, flags, length) + payload


def _b(x):
    return x if isinstance(x, bytes) else x.encode()


def encode_headers(headers):
    """headers: list of (name, value). Static names get 1-byte tags."""
    out = bytearray([len(headers)])
    for name, value in headers:
        name, value = _b(name), _b(value)
        tag = STATIC_ID.get(name.decode(), 0)
        out.append(tag)
        if tag == 0:
            out.append(len(name))
            out += name
        out += struct.pack(">H", len(value)) + value
    return bytes(out)


def encode_request(path, headers=(), method=GET):
    path = _b(path)
    return struct.pack(">BH", method, len(path)) + path + encode_headers(list(headers))


DEFAULT_REQUEST_HEADERS = [
    ("host", "localhost:9000"), ("user-agent", "bhtp.py/1"), ("accept", "*/*"),
    ("accept-encoding", "identity"), ("connection", "keep-alive"),
]


def request_frame(path, headers=None, **kw):
    if headers is None:
        headers = DEFAULT_REQUEST_HEADERS
    return frame(REQUEST, encode_request(path, headers), **kw)


def encode_response(status, headers):
    return struct.pack(">H", status) + encode_headers(headers)


# --------------------------------------------------------------------------
# decoding
# --------------------------------------------------------------------------

class ProtocolError(Exception):
    pass


def decode_headers(buf, off=0):
    """Returns (list of (name, value, tag), new_offset)."""
    if off >= len(buf):
        raise ProtocolError("missing header count")
    count = buf[off]
    off += 1
    out = []
    for _ in range(count):
        tag = buf[off]
        off += 1
        if tag == 0:
            nlen = buf[off]
            off += 1
            name = buf[off:off + nlen].decode()
            off += nlen
        elif 1 <= tag <= 10:
            name = STATIC[tag - 1]
        else:
            raise ProtocolError("reserved header tag %d" % tag)
        (vlen,) = struct.unpack_from(">H", buf, off)
        off += 2
        value = bytes(buf[off:off + vlen])
        if len(value) != vlen:
            raise ProtocolError("truncated header value")
        off += vlen
        out.append((name, value, tag))
    return out, off


def decode_request(payload):
    method, plen = struct.unpack_from(">BH", payload, 0)
    path = bytes(payload[3:3 + plen])
    headers, off = decode_headers(payload, 3 + plen)
    if off != len(payload):
        raise ProtocolError("trailing bytes")
    return method, path, headers


def decode_response(payload):
    (status,) = struct.unpack_from(">H", payload, 0)
    headers, off = decode_headers(payload, 2)
    if off != len(payload):
        raise ProtocolError("trailing bytes")
    return status, headers


def decode_header_block(buf):
    """Parse the 12-byte header: returns (version, type, flags, length)."""
    if buf[:4] != MAGIC:
        raise ProtocolError("bad magic")
    return struct.unpack(">BBHI", buf[4:12])


# --------------------------------------------------------------------------
# connection helper
# --------------------------------------------------------------------------

class Closed(Exception):
    """The peer closed the connection."""


class Response(object):
    def __init__(self):
        self.status = None
        self.headers = []         # (name, value-bytes, tag)
        self.body = b""
        self.frames = []          # (type, flags, payload_length) in arrival order
        self.skipped = []         # unknown frame types that were skipped
        self.response_payload = b""

    def header(self, name):
        for n, v, _ in self.headers:
            if n == name:
                return v.decode()
        return None


class Conn(object):
    def __init__(self, host, port, timeout=5.0, rcvbuf=None):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if rcvbuf:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.settimeout(timeout)
        self.sock.connect((host, port))
        self.buf = b""

    # -- sending ---------------------------------------------------------
    def send(self, data):
        self.sock.sendall(data)

    def send_request(self, path, headers=None):
        self.send(request_frame(path, headers))

    # -- receiving -------------------------------------------------------
    def recv_exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise Closed("EOF after %d of %d bytes" % (len(self.buf), n))
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def read_frame(self):
        """Returns (type, flags, payload) of the next frame, skipping unknown
        types exactly as the spec requires."""
        while True:
            head = self.recv_exact(HEADER_SIZE)
            version, ftype, flags, length = decode_header_block(head)
            payload = self.recv_exact(length)
            if ftype in (REQUEST, RESPONSE, DATA, ERROR):
                return ftype, flags, payload
            # unknown type: skipped

    def read_frame_raw(self):
        """Like read_frame but returns unknown frames too: (type, flags, payload)."""
        head = self.recv_exact(HEADER_SIZE)
        version, ftype, flags, length = decode_header_block(head)
        return ftype, flags, self.recv_exact(length)

    def read_response(self, slow=0.0):
        """Read RESPONSE and its DATA frames up to END_STREAM."""
        import time
        r = Response()
        while True:
            head = self.recv_exact(HEADER_SIZE)
            version, ftype, flags, length = decode_header_block(head)
            payload = self.recv_exact(length)
            if ftype == RESPONSE:
                if r.status is not None:
                    raise ProtocolError("second RESPONSE")
                r.status, r.headers = decode_response(payload)
                r.response_payload = payload
                r.frames.append((ftype, flags, length))
                if flags & END_STREAM:
                    return r
            elif ftype == DATA:
                if r.status is None:
                    raise ProtocolError("DATA before RESPONSE")
                r.body += payload
                r.frames.append((ftype, flags, length))
                if flags & END_STREAM:
                    return r
                if slow:
                    time.sleep(slow)
            elif ftype == ERROR:
                raise ProtocolError("ERROR frame: %r" % payload)
            else:
                r.skipped.append(ftype)

    def expect_closed(self, timeout=3.0):
        """True if the peer closes the connection (EOF) within `timeout`."""
        self.sock.settimeout(timeout)
        try:
            while True:
                chunk = self.sock.recv(65536)
                if not chunk:
                    return True
        except socket.timeout:
            return False
        except (ConnectionResetError, BrokenPipeError):
            return True

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
