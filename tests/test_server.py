#!/usr/bin/env python3
"""
test_server.py - black-box tests of ./bserve.

The server is started as a real process and talked to with tests/bhtp.py, an
independent implementation written from SPEC.md only. Nothing here links
against the C code.

    python3 tests/test_server.py            # run everything
    python3 tests/test_server.py -v         # list every test
    BSERVE=./build/bserve_asan python3 tests/test_server.py   # other binary

Test numbers in the docstrings refer to the list in the assignment brief.
"""
import hashlib
import os
import random
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bhtp  # noqa: E402

REPO = os.path.dirname(HERE)
WWW = os.path.join(REPO, "www")
BSERVE = os.path.abspath(os.environ.get("BSERVE", os.path.join(REPO, "bserve")))

NOBODY = 65534
SECRET = b"TOP-SECRET-OUTSIDE-THE-ROOT"


# --------------------------------------------------------------------------
# fixture: a document root with awkward things in it
# --------------------------------------------------------------------------

class Fixture(object):
    base = root = outside = None
    can_drop = False          # server runs as an unprivileged user
    big = b""


FIX = Fixture()


def read_www(rel):
    with open(os.path.join(WWW, rel), "rb") as f:
        return f.read()


def _drop():
    os.setgroups([])
    os.setgid(NOBODY)
    os.setuid(NOBODY)


def setUpModule():
    if not os.path.exists(BSERVE):
        raise RuntimeError("%s not found - run `make` first" % BSERVE)
    base = tempfile.mkdtemp(prefix="bhtp-test-")
    os.chmod(base, 0o755)
    FIX.base = base
    FIX.root = os.path.join(base, "site")
    FIX.outside = os.path.join(base, "outside")
    shutil.copytree(WWW, FIX.root)
    os.mkdir(FIX.outside)
    with open(os.path.join(FIX.outside, "secret.txt"), "wb") as f:
        f.write(SECRET)

    r = FIX.root
    os.mkdir(os.path.join(r, "emptydir"))
    with open(os.path.join(r, "a..b.txt"), "w") as f:
        f.write("dots inside a name\n")
    with open(os.path.join(r, "%2e%2e"), "w") as f:
        f.write("literal percent name\n")
    with open(os.path.join(r, "%41.txt"), "w") as f:
        f.write("literal %41\n")
    FIX.big = bytes((i * 131 + (i >> 7)) & 0xFF for i in range(3 * 1024 * 1024 + 123))
    with open(os.path.join(r, "big.bin"), "wb") as f:
        f.write(FIX.big)
    os.symlink(os.path.join(FIX.outside, "secret.txt"), os.path.join(r, "escape"))
    os.symlink(FIX.outside, os.path.join(r, "escapedir"))
    os.symlink("hello.txt", os.path.join(r, "inlink"))
    with open(os.path.join(r, "noperm.txt"), "w") as f:
        f.write("you cannot read me\n")
    os.chmod(os.path.join(r, "noperm.txt"), 0)

    for d, _, files in os.walk(r):
        os.chmod(d, 0o755)
        for n in files:
            p = os.path.join(d, n)
            if n != "noperm.txt" and not os.path.islink(p):
                os.chmod(p, 0o644)
    os.chmod(FIX.outside, 0o755)
    os.chmod(os.path.join(FIX.outside, "secret.txt"), 0o644)

    # Can the server be started as an unprivileged user? (Needed to make a
    # chmod-000 file really unreadable when the tests run as root.)
    FIX.can_drop = False
    if os.geteuid() == 0:
        try:
            p = subprocess.run([BSERVE], stderr=subprocess.DEVNULL, preexec_fn=_drop, timeout=5)
            FIX.can_drop = (p.returncode == 64)
        except Exception:
            FIX.can_drop = False


def tearDownModule():
    if FIX.base:
        shutil.rmtree(FIX.base, ignore_errors=True)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


# --------------------------------------------------------------------------
# base class: one fresh server per test
# --------------------------------------------------------------------------

class ServerTestCase(unittest.TestCase):
    _n = 0

    def setUp(self):
        ServerTestCase._n += 1
        self.port = free_port()
        self.logpath = os.path.join(FIX.base, "server-%d.log" % ServerTestCase._n)
        self.logf = open(self.logpath, "wb")
        kw = {}
        if FIX.can_drop:
            kw["preexec_fn"] = _drop
        self.proc = subprocess.Popen([BSERVE, FIX.root, str(self.port)],
                                     stdout=subprocess.DEVNULL, stderr=self.logf,
                                     start_new_session=True, **kw)
        self.conns = []
        deadline = time.time() + 5
        while "bserve: serving" not in self.log():
            if self.proc.poll() is not None or time.time() > deadline:
                self.fail("server did not start:\n" + self.log())
            time.sleep(0.01)

    def tearDown(self):
        for c in self.conns:
            c.close()
        alive = self.proc.poll() is None
        balanced = self.wait_balanced(4.0)
        log = self.log()
        try:
            os.killpg(self.proc.pid, signal.SIGKILL)
        except OSError:
            pass
        self.proc.wait()
        self.logf.close()
        self.assertTrue(alive, "server process died:\n" + log)
        self.assertTrue(balanced,
                        "a connection process crashed or hung (accepted != closed):\n" + log)

    # -- helpers -----------------------------------------------------------
    def log(self):
        try:
            with open(self.logpath, "r", errors="replace") as f:
                return f.read()
        except IOError:
            return ""

    def counts(self):
        text = self.log()
        return (len(re.findall(r"^\[conn=\d+\] accepted$", text, re.M)),
                len(re.findall(r"^\[conn=\d+\] closed \(", text, re.M)))

    def wait_balanced(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            a, c = self.counts()
            if a == c:
                return True
            time.sleep(0.02)
        a, c = self.counts()
        return a == c

    def wait_log(self, pattern, timeout=3.0):
        end = time.time() + timeout
        while time.time() < end:
            if re.search(pattern, self.log(), re.M):
                return True
            time.sleep(0.02)
        return False

    def connect(self, **kw):
        c = bhtp.Conn("127.0.0.1", self.port, **kw)
        self.conns.append(c)
        return c

    def get(self, path, headers=None):
        c = self.connect()
        c.send_request(path, headers)
        return c.read_response()

    def assert_get(self, path, status, body=None):
        r = self.get(path)
        self.assertEqual(r.status, status, "GET %r" % (path,))
        if body is not None:
            self.assertEqual(r.body, body, "GET %r" % (path,))
        return r

    def assert_status_only(self, r, status):
        """A body-less response: content-length 0, END_STREAM on the RESPONSE."""
        self.assertEqual(r.status, status)
        self.assertEqual(r.header("content-length"), "0")
        self.assertEqual(len(r.frames), 1)
        self.assertEqual(r.frames[0][0], bhtp.RESPONSE)
        self.assertEqual(r.frames[0][1], bhtp.END_STREAM)
        self.assertEqual(r.body, b"")

    def assert_still_serving(self, conn=None):
        conn = conn or self.connect()
        conn.send_request("/hello.txt")
        r = conn.read_response()
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body, read_www("hello.txt"))

    def assert_fatal(self, data, reason=None, trailing=b""):
        """The server answers with an ERROR frame, then closes the connection;
        the server as a whole must keep working."""
        c = self.connect()
        c.send(data)
        if trailing:
            try:
                c.send(trailing)
            except (BrokenPipeError, ConnectionResetError):
                pass
        ftype, flags, payload = c.read_frame_raw()
        self.assertEqual(ftype, bhtp.ERROR)
        self.assertEqual(flags, 0)
        (status,) = struct.unpack(">H", payload[:2])
        self.assertEqual(status, 400)
        if reason:
            self.assertEqual(payload[2:].decode(), reason)
        self.assertTrue(c.expect_closed(), "connection must be closed after a fatal error")
        self.assert_still_serving()

    def assert_malformed_payload_gives_400(self, payload, flags=0):
        """Well-framed but bad contents: 400 RESPONSE and the connection stays open."""
        c = self.connect()
        c.send(bhtp.frame(bhtp.REQUEST, payload, flags=flags))
        self.assert_status_only(c.read_response(), 400)
        self.assert_still_serving(c)             # SAME connection


# --------------------------------------------------------------------------
# BASIC FUNCTIONAL TESTS  (brief tests 1-7)
# --------------------------------------------------------------------------

class TestBasic(ServerTestCase):

    def test_01_get_html_is_200_with_correct_headers_and_frames(self):
        """#1 GET existing HTML file -> 200"""
        body = read_www("index.html")
        r = self.assert_get("/index.html", 200, body)
        self.assertEqual(r.header("content-type"), "text/html")
        self.assertEqual(r.header("content-length"), str(len(body)))
        self.assertEqual(r.header("server"), "bserve/1.0")
        self.assertEqual(r.header("connection"), "keep-alive")
        self.assertEqual(r.header("cache-control"), "no-cache")
        self.assertRegex(r.header("last-modified"),
                         r"^(Mon|Tue|Wed|Thu|Fri|Sat|Sun), \d\d [A-Z][a-z]{2} \d{4} \d\d:\d\d:\d\d GMT$")
        # header compression: every name the server sends is one of the ten
        # static names, so every tag is 1..10 and no literal name is used
        self.assertTrue(all(1 <= tag <= 10 for _, _, tag in r.headers), r.headers)
        self.assertEqual(sorted(n for n, _, _ in r.headers),
                         sorted(["content-type", "content-length", "server",
                                 "last-modified", "cache-control", "connection"]))
        # framing: RESPONSE (no END_STREAM) then DATA with END_STREAM
        self.assertEqual(r.frames[0][:2], (bhtp.RESPONSE, 0))
        self.assertEqual(r.frames[-1][:2], (bhtp.DATA, bhtp.END_STREAM))

    def test_02_get_text_file_is_200(self):
        """#2 GET existing text file -> 200"""
        r = self.assert_get("/hello.txt", 200, read_www("hello.txt"))
        self.assertEqual(r.header("content-type"), "text/plain")

    def test_03_get_empty_file_is_200_with_no_data_frame(self):
        """#3 GET empty file -> 200"""
        r = self.assert_get("/empty.txt", 200, b"")
        self.assertEqual(r.header("content-length"), "0")
        self.assertEqual(len(r.frames), 1)                    # RESPONSE only ...
        self.assertEqual(r.frames[0][1], bhtp.END_STREAM)     # ... carrying END_STREAM

    def test_04_get_binary_file_spans_data_frames_and_is_byte_exact(self):
        """#4 GET binary file -> 200 (and exact bytes)"""
        data = read_www("binary.bin")
        r = self.assert_get("/binary.bin", 200)
        self.assertEqual(hashlib.sha256(r.body).hexdigest(), hashlib.sha256(data).hexdigest())
        self.assertEqual(r.header("content-type"), "application/octet-stream")
        lengths = [n for t, f, n in r.frames if t == bhtp.DATA]
        self.assertEqual(lengths, [16384, 16384, 40000 - 2 * 16384])
        flags = [f for t, f, n in r.frames if t == bhtp.DATA]
        self.assertEqual(flags, [0, 0, bhtp.END_STREAM])      # END_STREAM only on the last

    def test_04b_large_file_over_three_megabytes(self):
        """#4 (large) 3 MiB file arrives intact"""
        r = self.assert_get("/big.bin", 200)
        self.assertEqual(len(r.body), len(FIX.big))
        self.assertEqual(hashlib.sha256(r.body).hexdigest(), hashlib.sha256(FIX.big).hexdigest())
        self.assertTrue(all(n <= 16384 for t, f, n in r.frames))
        self.assertEqual(sum(1 for t, f, n in r.frames if f & bhtp.END_STREAM), 1)

    def test_05_missing_file_is_404(self):
        """#5 GET nonexistent file -> 404"""
        r = self.get("/does-not-exist.html")
        self.assert_status_only(r, 404)
        self.assertEqual(r.header("connection"), "keep-alive")

    def test_06_malformed_request_is_400_and_connection_survives(self):
        """#6 malformed request -> 400, connection stays open"""
        c = self.connect()
        c.send(bhtp.frame(bhtp.REQUEST, b"\x01\x00"))        # payload cut off after 2 bytes
        self.assert_status_only(c.read_response(), 400)
        c.send(bhtp.frame(bhtp.REQUEST, b""))                # empty payload
        self.assert_status_only(c.read_response(), 400)
        self.assert_still_serving(c)

    def test_07_internal_error_is_500(self):
        """#7 server-side failure -> 500 (an unreadable file)"""
        if not FIX.can_drop and os.geteuid() == 0:
            self.skipTest("running as root and cannot drop privileges: chmod 000 would not block")
        r = self.get("/noperm.txt")
        self.assert_status_only(r, 500)
        self.assert_still_serving()

    def test_08_default_document_for_root(self):
        r = self.assert_get("/", 200, read_www("index.html"))
        self.assertEqual(r.header("content-type"), "text/html")

    def test_09_unusual_request_headers_are_accepted(self):
        """Unknown (literal) request headers are ignored, not rejected."""
        hdrs = bhtp.DEFAULT_REQUEST_HEADERS + [("x-demo", "1"), ("x-another-header", "v" * 300)]
        self.assert_get_with_headers = None
        r = self.get("/hello.txt", hdrs)
        self.assertEqual(r.status, 200)
        r = self.get("/hello.txt", [])                       # no headers at all
        self.assertEqual(r.status, 200)
        many = [("x-h%d" % i, "v") for i in range(32)]       # exactly 32 headers
        self.assertEqual(self.get("/hello.txt", many).status, 200)


# --------------------------------------------------------------------------
# PROTOCOL TESTS  (brief tests 8-17)
# --------------------------------------------------------------------------

class TestProtocol(ServerTestCase):

    def test_08_valid_frame_parsing_every_header_form(self):
        """#8 valid frame: static tags, literal names, empty values"""
        payload = bhtp.encode_request("/hello.txt", [
            ("host", "x"), ("user-agent", ""), ("x-literal-name", "value"), ("connection", "keep-alive")])
        c = self.connect()
        c.send(bhtp.frame(bhtp.REQUEST, payload))
        r = c.read_response()
        self.assertEqual((r.status, r.body), (200, read_www("hello.txt")))

    def test_09_bad_magic_is_fatal(self):
        """#9 malformed frame header (magic) -> ERROR 400, then close"""
        self.assert_fatal(b"XHTP" + bhtp.request_frame("/hello.txt")[4:], "bad magic")
        self.assert_fatal(b"bhtp" + bhtp.request_frame("/hello.txt")[4:], "bad magic")

    def test_09b_http_text_sent_to_the_port_is_rejected(self):
        self.assert_fatal(b"GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n", "bad magic")

    def test_09c_wrong_version_is_fatal(self):
        for v in (0, 2, 255):
            self.assert_fatal(bhtp.request_frame("/hello.txt", version=v), "unsupported version")

    def test_10_truncated_frame_closes_without_reply(self):
        """#10 truncated frame: peer's EOF in the middle of a frame"""
        full = bhtp.request_frame("/hello.txt")
        for cut in (1, 5, 11, 12, 15, len(full) - 1):          # inside header and inside payload
            c = self.connect()
            c.send(full[:cut])
            c.sock.shutdown(socket.SHUT_WR)
            data = b""
            c.sock.settimeout(3)
            while True:
                chunk = c.sock.recv(4096)
                if not chunk:
                    break
                data += chunk
            self.assertEqual(data, b"", "no reply to a truncated frame (cut=%d)" % cut)
        self.assertTrue(self.wait_log(r"lost in the middle of a frame"))
        self.assert_still_serving()

    def test_10b_truncated_unknown_frame(self):
        c = self.connect()
        c.send(bhtp.frame(0x70, b"x" * 50)[:30])
        c.sock.shutdown(socket.SHUT_WR)
        self.assertTrue(c.expect_closed())
        self.assert_still_serving()

    def test_11_invalid_frame_length_over_the_limit(self):
        """#11 length field larger than 16384 on a known type -> fatal"""
        for length in (16385, 65536, 1 << 24, 0x7FFFFFFF, 0xFFFFFFFF):
            self.assert_fatal(bhtp.frame(bhtp.REQUEST, b"", length=length), "frame too large")

    def test_11b_length_longer_than_the_data_that_follows(self):
        """length says 100 but the peer only ever sends 10 bytes, then EOF"""
        c = self.connect()
        c.send(bhtp.frame(bhtp.REQUEST, b"0123456789", length=100))
        c.sock.shutdown(socket.SHUT_WR)
        self.assertTrue(c.expect_closed())
        self.assert_still_serving()

    def test_12_oversized_frame_with_the_payload_actually_sent(self):
        """#12 a 20000-byte REQUEST: rejected from the header alone"""
        data = bhtp.frame(bhtp.REQUEST, b"A" * 20000, length=20000)
        self.assert_fatal(data[:12], "frame too large", trailing=data[12:])
        # a known frame exactly at the limit is fine (contents are still garbage -> 400)
        self.assert_malformed_payload_gives_400(b"\xff" * 16384)

    def test_13_unknown_frame_type_is_skipped_silently(self):
        """#13 unknown frame type: no reply, no error, connection continues"""
        c = self.connect()
        c.send(bhtp.frame(0x7F, b"future extension data", flags=0xBEEF))
        c.send_request("/hello.txt")
        r = c.read_response()
        self.assertEqual(r.status, 200)
        self.assertEqual([t for t, _, _ in r.frames], [bhtp.RESPONSE, bhtp.DATA])  # only ONE response
        self.assertTrue(self.wait_log(r"skipped unknown frame type 0x7f \(21 payload bytes\)"))

    def test_14_known_unknown_known_in_one_write(self):
        """#14 request, UNKNOWN frame, request - sent in a single send()"""
        blob = (bhtp.request_frame("/hello.txt") +
                bhtp.frame(0x55, b"\xde\xad\xbe\xef" * 100, flags=0x1234) +
                bhtp.request_frame("/index.html"))
        c = self.connect()
        c.send(blob)
        r1, r2 = c.read_response(), c.read_response()
        self.assertEqual((r1.status, r1.body), (200, read_www("hello.txt")))
        self.assertEqual((r2.status, r2.body), (200, read_www("index.html")))

    def test_14b_every_other_unknown_type_and_edge_lengths(self):
        c = self.connect()
        for t in [0, 5, 6, 9, 0x10, 0x7F, 0x80, 0xFE, 0xFF]:
            c.send(bhtp.frame(t, b""))                          # empty
            c.send(bhtp.frame(t, b"x" * 16384))                 # known-frame limit
            c.send(bhtp.frame(t, b"y" * 16385))                 # just over it: still skipped
            c.send(bhtp.frame(t, b"z", flags=0xFFFF))
        self.assert_still_serving(c)

    def test_14c_huge_unknown_frame_is_skipped_not_buffered(self):
        """An unknown frame may be far larger than any known one."""
        c = self.connect()
        size = 5 * 1024 * 1024
        c.send(bhtp.frame(0x42, b"", length=size))
        sent = 0
        chunk = b"Q" * 65536
        while sent < size:
            c.send(chunk[:min(len(chunk), size - sent)])
            sent += len(chunk)
        self.assert_still_serving(c)

    def test_14d_unknown_frame_declared_but_never_completed(self):
        c = self.connect()
        c.send(bhtp.frame(0x42, b"abc", length=1000))
        c.sock.shutdown(socket.SHUT_WR)
        self.assertTrue(c.expect_closed())
        self.assert_still_serving()

    def test_15_invalid_flags_give_400_and_keep_the_connection(self):
        """#15 undefined flag bits on a known frame type"""
        for flags in (0x0001, 0x0002, 0x0100, 0x8000, 0xFFFF):   # REQUEST allows NO flags
            self.assert_malformed_payload_gives_400(bhtp.encode_request("/hello.txt"), flags=flags)

    def test_15b_unexpected_known_frame_types_from_a_client(self):
        c = self.connect()
        for ftype in (bhtp.RESPONSE, bhtp.DATA, bhtp.ERROR):
            c.send(bhtp.frame(ftype, b"\x00\xc8\x00"))
            self.assert_status_only(c.read_response(), 400)
        self.assert_still_serving(c)

    def test_16_invalid_headers_give_400(self):
        """#16 every kind of broken header block"""
        ok = b"\x01\x00\x01/"                      # GET "/" ...
        cases = {
            "tag 11 (reserved)":          ok + b"\x01\x0b\x00\x00",
            "tag 255 (reserved)":         ok + b"\x01\xff\x00\x00",
            "33 headers":                 ok + b"\x21" + (b"\x01\x00\x01v" * 33),
            "count > headers present":    ok + b"\x05\x01\x00\x01v",
            "missing count":              ok,
            "uppercase literal name":     ok + b"\x01\x00\x01A\x00\x00",
            "empty literal name":         ok + b"\x01\x00\x00\x00\x00",
            "literal name with space":    ok + b"\x01\x00\x03a b\x00\x00",
            "literal name truncated":     ok + b"\x01\x00\x09ab",
            "value length 65535":         ok + b"\x01\x01\xff\xff",
            "value length 4097":          ok + b"\x01\x01\x10\x01" + b"v" * 4097,
            "value shorter than length":  ok + b"\x01\x01\x00\x05ab",
            "trailing garbage":           ok + b"\x00" + b"\x00",
        }
        for name, payload in cases.items():
            with self.subTest(name):
                self.assert_malformed_payload_gives_400(payload)

    def test_16b_value_of_exactly_4096_bytes_is_accepted(self):
        payload = bhtp.encode_request("/hello.txt", [("x-long", "v" * 4096)])
        c = self.connect()
        c.send(bhtp.frame(bhtp.REQUEST, payload))
        self.assertEqual(c.read_response().status, 200)

    def test_17_invalid_paths_give_400(self):
        """#17 syntactically invalid paths"""
        bad = [b"index.html", b"hello.txt", b"./hello.txt", b"../hello.txt", b"",
               b"/a\x00b", b"/hello.txt\x00", b"/a\nb", b"/a\tb", b"/a\x7f",
               b"/a\\b", b"\\hello.txt", b"/" + b"a" * 4096 + b"b"]
        for p in bad:
            with self.subTest(path=p[:40]):
                if p == b"":                       # zero-length path is a payload error
                    payload = b"\x01\x00\x00\x00"
                    self.assert_malformed_payload_gives_400(payload)
                else:
                    r = self.get(p)
                    self.assert_status_only(r, 400)

    def test_17b_method_other_than_get_is_400(self):
        for method in (0, 2, 3, 0x7F, 0xFF):
            self.assert_malformed_payload_gives_400(
                struct.pack(">BH", method, 1) + b"/" + b"\x00")

    def test_17c_path_of_4096_bytes_is_valid_but_absent(self):
        r = self.get("/" + "a" * 4095)
        self.assert_status_only(r, 404)


# --------------------------------------------------------------------------
# TCP BYTE-STREAM TESTS  (brief tests 18-21)
# --------------------------------------------------------------------------

class TestTcp(ServerTestCase):

    def test_18_request_split_at_every_possible_offset(self):
        """#18/#19 one frame split across two reads, at every offset"""
        frame = bhtp.request_frame("/hello.txt")
        c = self.connect()                                   # ONE connection for all splits
        for cut in range(1, len(frame)):
            c.send(frame[:cut])
            time.sleep(0.003)
            c.send(frame[cut:])
            r = c.read_response()
            self.assertEqual((r.status, r.body), (200, read_www("hello.txt")), "cut=%d" % cut)

    def test_19_request_delivered_one_byte_at_a_time(self):
        frame = bhtp.request_frame("/index.html")
        c = self.connect()
        for i in range(len(frame)):
            c.send(frame[i:i + 1])
            time.sleep(0.001)
        r = c.read_response()
        self.assertEqual((r.status, r.body), (200, read_www("index.html")))

    def test_19b_unknown_frame_dribbled_in_before_a_request(self):
        data = bhtp.frame(0x66, b"abcdefghij" * 30) + bhtp.request_frame("/hello.txt")
        c = self.connect()
        for i in range(0, len(data), 7):
            c.send(data[i:i + 7])
            time.sleep(0.0005)
        self.assertEqual(c.read_response().body, read_www("hello.txt"))

    def test_20_many_frames_in_one_write_are_answered_in_order(self):
        """#20 multiple frames in one read: 3 requests, an unknown frame, a bad one"""
        blob = (bhtp.request_frame("/hello.txt") +
                bhtp.request_frame("/nope") +
                bhtp.frame(0x77, b"?" * 10) +
                bhtp.frame(bhtp.REQUEST, b"\x01\x00") +
                bhtp.request_frame("/index.html"))
        c = self.connect()
        c.send(blob)
        statuses = [c.read_response().status for _ in range(4)]
        self.assertEqual(statuses, [200, 404, 400, 200])

    def test_20b_fifty_pipelined_requests(self):
        c = self.connect()
        names = ["/hello.txt", "/index.html", "/missing", "/sub/page.txt", "/binary.bin"]
        c.send(b"".join(bhtp.request_frame(names[i % 5]) for i in range(50)))
        want = {"/hello.txt": (200, read_www("hello.txt")),
                "/index.html": (200, read_www("index.html")),
                "/missing": (404, b""),
                "/sub/page.txt": (200, read_www("sub/page.txt")),
                "/binary.bin": (200, read_www("binary.bin"))}
        for i in range(50):
            r = c.read_response()
            self.assertEqual((r.status, r.body), want[names[i % 5]], "response %d" % i)

    def test_21_partial_writes_server_must_cope_with_a_slow_reader(self):
        """#21 the server's writes are throttled by a client that reads slowly
        through a tiny receive buffer; every byte must still arrive."""
        c = self.connect(rcvbuf=4096)
        c.send_request("/big.bin")
        r = c.read_response(slow=0.0002)
        self.assertEqual(hashlib.sha256(r.body).hexdigest(), hashlib.sha256(FIX.big).hexdigest())
        self.assert_still_serving(c)

    def test_22_client_vanishing_mid_download_does_not_hurt_the_server(self):
        for _ in range(5):
            c = self.connect(rcvbuf=4096)
            c.send_request("/big.bin")
            c.recv_exact(5000)
            c.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            c.close()                                         # RST in the middle of the body
        self.assert_still_serving()
        self.assertTrue(self.wait_log(r"\[conn=1\] closed"))


# --------------------------------------------------------------------------
# PERSISTENT CONNECTION  (the critical test)
# --------------------------------------------------------------------------

class TestPersistentConnection(ServerTestCase):

    def test_three_exchanges_use_exactly_one_connection(self):
        """request #1/#2/#3 and their responses all happen on ONE TCP connection,
        proven from the server's own log."""
        c = self.connect()
        plan = [("/index.html", 200), ("/missing.txt", 404), ("/hello.txt", 200)]
        for path, status in plan:
            c.send_request(path)
            self.assertEqual(c.read_response().status, status)
        c.close()
        self.assertTrue(self.wait_log(r"^\[conn=1\] closed \(client closed the connection\)$"))

        log = self.log()
        self.assertEqual(re.findall(r"^\[conn=(\d+)\] accepted$", log, re.M), ["1"])
        self.assertNotIn("conn=2", log)
        exchange = re.findall(r"^\[conn=1\] (request|response) #(\d) (.*)$", log, re.M)
        n = len(read_www("index.html"))
        h = len(read_www("hello.txt"))
        self.assertEqual(exchange, [
            ("request", "1", "GET /index.html"), ("response", "1", "200 OK (%d bytes)" % n),
            ("request", "2", "GET /missing.txt"), ("response", "2", "404 Not Found"),
            ("request", "3", "GET /hello.txt"), ("response", "3", "200 OK (%d bytes)" % h),
        ])

    def test_two_sequential_connections_get_distinct_ids(self):
        for _ in range(2):
            c = self.connect()
            self.assertEqual(c.read_response if False else None, None)
            self.assert_still_serving(c)
            c.close()
        self.assertTrue(self.wait_log(r"\[conn=2\] closed"))
        self.assertEqual(re.findall(r"^\[conn=(\d+)\] accepted$", self.log(), re.M), ["1", "2"])

    def test_connection_stays_open_while_idle(self):
        c = self.connect()
        self.assert_still_serving(c)
        time.sleep(1.5)                                       # idle, but not closed
        self.assert_still_serving(c)
        self.assertEqual(self.counts()[1], 0, "server must not have closed the idle connection")

    def test_many_requests_on_one_connection(self):
        c = self.connect()
        for i in range(300):
            c.send_request("/hello.txt" if i % 3 else "/nope")
            self.assertEqual(c.read_response().status, 200 if i % 3 else 404)
        c.close()
        self.assertTrue(self.wait_log(r"\[conn=1\] closed"))
        self.assertEqual(self.counts()[0], 1)
        self.assertIn("[conn=1] request #300 ", self.log())

    def test_errors_do_not_end_the_connection(self):
        """400 and 404 keep the connection; the request counter just goes on."""
        c = self.connect()
        c.send(bhtp.frame(bhtp.REQUEST, b"junk"))
        self.assertEqual(c.read_response().status, 400)
        c.send_request("/nope")
        self.assertEqual(c.read_response().status, 404)
        c.send_request("/../x")
        self.assertEqual(c.read_response().status, 400)
        self.assert_still_serving(c)
        self.assertEqual(self.counts()[1], 0)

    def test_connections_are_served_concurrently(self):
        """An idle open connection must not block other clients."""
        idle = self.connect()
        for _ in range(3):
            self.assert_still_serving()                       # new connection each time
        self.assert_still_serving(idle)                       # the idle one still works
        self.assertEqual(self.counts()[0], 4)

    def test_connection_close_request_header(self):
        c = self.connect()
        c.send_request("/hello.txt", bhtp.DEFAULT_REQUEST_HEADERS[:-1] + [("connection", "close")])
        r = c.read_response()
        self.assertEqual(r.status, 200)
        self.assertEqual(r.header("connection"), "close")
        self.assertTrue(c.expect_closed(), "server must close after 'connection: close'")
        self.assertTrue(self.wait_log(r"closed \(server closed the connection after the response\)"))

    def test_client_half_close_still_gets_its_response(self):
        c = self.connect()
        c.send_request("/index.html")
        c.sock.shutdown(socket.SHUT_WR)                       # "I am done sending"
        r = c.read_response()
        self.assertEqual((r.status, r.body), (200, read_www("index.html")))
        self.assertTrue(c.expect_closed())


# --------------------------------------------------------------------------
# SECURITY / PATH TESTS  (brief tests 22-27)
# --------------------------------------------------------------------------

class TestPathSecurity(ServerTestCase):

    def assert_not_leaked(self, r):
        self.assertNotIn(SECRET, r.body)
        self.assertNotIn(b"root:", r.body)

    def test_22_dot_dot_etc_passwd(self):
        """#22 ../../etc/passwd"""
        for p in ("/../../etc/passwd", "/../../../../../../etc/passwd", "../../etc/passwd",
                  "/..//..//etc/passwd", "/./../etc/passwd", "/sub/../../../etc/passwd"):
            r = self.get(p)
            self.assertEqual(r.status, 400, p)
            self.assert_not_leaked(r)

    def test_23_dot_dot_out_of_the_root(self):
        """#23 ../ to the outside secret file"""
        for p in ("/../outside/secret.txt", "/sub/../../outside/secret.txt", "/..", "/../",
                  "/a/b/c/../../../../outside/secret.txt", "/hello.txt/.."):
            r = self.get(p)
            self.assertEqual(r.status, 400, p)
            self.assert_not_leaked(r)

    def test_23b_dot_dot_is_refused_even_when_harmless(self):
        self.assertEqual(self.get("/sub/../hello.txt").status, 400)

    def test_24_absolute_paths_are_relative_to_the_root(self):
        """#24 absolute filesystem paths are NOT honoured"""
        for p in ("/etc/passwd", "//etc/passwd", "/etc/../etc/passwd" if False else "/etc/shadow",
                  FIX.outside + "/secret.txt"):
            r = self.get(p)
            self.assertEqual(r.status, 404, p)
            self.assert_not_leaked(r)

    def test_25_percent_encoded_traversal_is_not_decoded(self):
        """#25 %2e%2e is a literal name; BHTP never percent-decodes"""
        for p in ("/%2e%2e/%2e%2e/etc/passwd", "/%2e%2e%2f%2e%2e%2fetc%2fpasswd",
                  "/..%2f..%2fetc/passwd", "/%2E%2E/", "/%252e%252e/", "/%00", "/hello.txt%00"):
            r = self.get(p)
            self.assertIn(r.status, (404,), p)
            self.assert_not_leaked(r)
        # %2e%2e exists as a plain file here and is served as such: proof of "no decoding"
        self.assert_get("/%2e%2e", 200, b"literal percent name\n")
        self.assert_get("/%41.txt", 200, b"literal %41\n")
        self.assert_get("/A.txt", 404)

    def test_26_directories(self):
        """#26 directory requests"""
        sub = read_www("sub/index.html")
        self.assert_get("/sub", 200, sub)
        self.assert_get("/sub/", 200, sub)
        self.assert_get("/", 200, read_www("index.html"))
        self.assert_get("//", 200, read_www("index.html"))
        self.assertEqual(self.get("/emptydir").status, 404)    # no index.html
        self.assertEqual(self.get("/emptydir/").status, 404)
        self.assertEqual(self.get("/hello.txt/").status, 404)  # file with a trailing slash

    def test_27_path_normalisation(self):
        """#27 repeated slashes and '.' segments"""
        hello = read_www("hello.txt")
        for p in ("//hello.txt", "/./hello.txt", "///./hello.txt", "/sub/../hello.txt"[:0] or "/././hello.txt"):
            self.assert_get(p, 200, hello)
        self.assert_get("/sub//page.txt", 200, read_www("sub/page.txt"))
        self.assert_get("/sub/./page.txt", 200, read_www("sub/page.txt"))
        self.assert_get("/a..b.txt", 200, b"dots inside a name\n")   # ".." inside a name is fine

    def test_28_symlinks(self):
        r = self.get("/escape")
        self.assertEqual(r.status, 404)
        self.assert_not_leaked(r)
        r = self.get("/escapedir/secret.txt")
        self.assertEqual(r.status, 404)
        self.assert_not_leaked(r)
        self.assert_get("/inlink", 200, read_www("hello.txt"))   # a link that stays inside is fine

    def test_29_nothing_outside_the_root_was_ever_served(self):
        attempts = ["/../../etc/passwd", "/etc/passwd", "/%2e%2e/etc/passwd", "/escape",
                    "/escapedir/secret.txt", FIX.outside + "/secret.txt", "//..//etc//passwd"]
        for p in attempts:
            self.assert_not_leaked(self.get(p))
        self.assertNotIn("TOP-SECRET", self.log())


# --------------------------------------------------------------------------
# FUZZ / ROBUSTNESS
# --------------------------------------------------------------------------

class TestFuzz(ServerTestCase):
    ITERATIONS = int(os.environ.get("FUZZ_ITERATIONS", "400"))
    SEED = int(os.environ.get("FUZZ_SEED", "20240607"))

    def _victim_payload(self, rnd):
        """A valid request payload to mutate."""
        hdrs = [("host", "h"), ("x-fuzz", "v" * rnd.randint(0, 40))]
        return bytearray(bhtp.encode_request("/" + "a" * rnd.randint(0, 20), hdrs))

    def _case(self, rnd):
        kind = rnd.choice(["random-type", "random-length", "truncated", "max-payload",
                           "oversized", "bad-header-count", "bad-string-length",
                           "random-bytes", "mutated-request", "unknown-then-valid"])
        if kind == "random-type":
            return kind, bhtp.frame(rnd.randrange(256), os.urandom(rnd.randint(0, 200)),
                                    flags=rnd.randrange(65536))
        if kind == "random-length":
            return kind, bhtp.frame(rnd.choice([1, 2, 3, 4]), os.urandom(rnd.randint(0, 50)),
                                    length=rnd.choice([0, 1, 255, 16384, 16385, 1 << 20, 0xFFFFFFFF,
                                                       rnd.randrange(1 << 32)]))
        if kind == "truncated":
            f = bhtp.request_frame("/hello.txt")
            return kind, f[:rnd.randint(1, len(f) - 1)]
        if kind == "max-payload":
            return kind, bhtp.frame(bhtp.REQUEST, os.urandom(16384))
        if kind == "oversized":
            return kind, bhtp.frame(bhtp.REQUEST, os.urandom(16385))
        if kind == "bad-header-count":
            p = bytearray(bhtp.encode_request("/hello.txt", [("host", "x")]))
            p[3 + len("/hello.txt")] = rnd.choice([0, 2, 31, 32, 33, 128, 255])
            return kind, bhtp.frame(bhtp.REQUEST, bytes(p))
        if kind == "bad-string-length":
            p = self._victim_payload(rnd)
            i = rnd.randrange(len(p))
            p[i] = rnd.choice([0, 1, 0x7F, 0x80, 0xFF])
            return kind, bhtp.frame(bhtp.REQUEST, bytes(p))
        if kind == "random-bytes":
            return kind, os.urandom(rnd.randint(1, 300))
        if kind == "mutated-request":
            f = bytearray(bhtp.request_frame("/index.html"))
            for _ in range(rnd.randint(1, 4)):
                f[rnd.randrange(len(f))] = rnd.randrange(256)
            return kind, bytes(f)
        return kind, bhtp.frame(rnd.randint(5, 255), os.urandom(rnd.randint(0, 100))) + \
            bhtp.request_frame("/hello.txt")

    def test_server_survives_malformed_input(self):
        rnd = random.Random(self.SEED)
        sys.stderr.write("\n[fuzz] seed=%d iterations=%d\n" % (self.SEED, self.ITERATIONS))
        kinds = {}
        for i in range(self.ITERATIONS):
            kind, data = self._case(rnd)
            kinds[kind] = kinds.get(kind, 0) + 1
            c = self.connect(timeout=2.0)
            try:
                c.send(data)
                if rnd.random() < 0.5:
                    c.sock.shutdown(socket.SHUT_WR)
                c.sock.settimeout(0.05)
                try:
                    while c.sock.recv(65536):
                        pass
                except (socket.timeout, ConnectionResetError, BrokenPipeError):
                    pass
            except (BrokenPipeError, ConnectionResetError):
                pass
            c.close()
            if i % 50 == 49:
                self.assertIsNone(self.proc.poll(), "server died during fuzzing")
                self.assert_still_serving()
        self.assertEqual(len(kinds), 10, "every fuzz strategy must be exercised: %r" % kinds)
        self.assert_still_serving()
        # tearDown additionally verifies accepted == closed, i.e. no child crashed


if __name__ == "__main__":
    unittest.main(verbosity=1)
