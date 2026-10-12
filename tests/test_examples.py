#!/usr/bin/env python3
"""
test_examples.py - the documented hexdumps must be what bserve really sends.

examples/request.hexdump and examples/response.hexdump are replayed against a
real bserve process:

  * the request bytes taken from the file are sent to the server
  * the raw bytes that come back must equal the response bytes in the file
  * the annotation tables in the files must be exactly what the spec-only
    decoder (tools/make_examples.py) produces from those same bytes

    python3 tests/test_examples.py
"""
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "tools"))
import bhtp  # noqa: E402
import make_examples  # noqa: E402

BSERVE = os.path.abspath(os.environ.get("BSERVE", os.path.join(REPO, "bserve")))
EXAMPLES = os.path.join(REPO, "examples")

# ---- parsing of the checked-in example files -------------------------------

def read_example_frames(path):
    """[(label, bytes, annotation-table-lines)] from examples/*.hexdump."""
    frames, cur, in_table = [], None, False
    with open(path) as f:
        for line in f.read().splitlines():
            m = re.match(r"^([TR]X) FRAME #(\d+)\s+(\w+)", line)
            if m:
                cur = {"label": m.group(0), "bytes": b"", "table": []}
                frames.append(cur)
                in_table = False
                continue
            if cur is None:
                continue
            m = re.match(r"^\s*[0-9a-f]{4}: ((?:[0-9a-f]{2} ?| )+?) +\|", line)
            if m and not in_table:
                cur["bytes"] += bytes.fromhex(m.group(1).replace(" ", ""))
                continue
            if line.strip().startswith("Offset"):
                in_table = True
            if in_table:
                cur["table"].append(line.strip())
    for fr in frames:                      # blank lines after a table are just spacing
        while fr["table"] and fr["table"][-1] == "":
            fr["table"].pop()
    return frames


def table_lines(rows):
    return [l.strip() for l in make_examples.render_table(rows)]


class TestExamples(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="bhtp-examples-")
        os.chmod(cls.tmp, 0o755)
        cls.www = os.path.join(cls.tmp, "www")
        os.mkdir(cls.www)
        shutil.copy(os.path.join(REPO, "www", "index.html"), cls.www)
        os.utime(os.path.join(cls.www, "index.html"), (1767225600, 1767225600))   # 2026-01-01 00:00:00 UTC
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        cls.port = s.getsockname()[1]
        s.close()
        cls.proc = subprocess.Popen([BSERVE, cls.www, str(cls.port)], stderr=subprocess.PIPE, text=True)
        if "serving" not in cls.proc.stderr.readline():
            raise RuntimeError("bserve did not start")
        cls.req = read_example_frames(os.path.join(EXAMPLES, "request.hexdump"))
        cls.resp = read_example_frames(os.path.join(EXAMPLES, "response.hexdump"))

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(timeout=5)
        cls.proc.stderr.close()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_example_files_contain_one_request_and_a_response_with_data(self):
        self.assertEqual(len(self.req), 1)
        self.assertEqual([f["label"].split()[-1] for f in self.resp], ["RESPONSE", "DATA"])

    def test_server_answers_the_example_request_with_exactly_the_example_response(self):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        s.sendall(self.req[0]["bytes"])
        got = b""

        def need(n):
            buf = b""
            while len(buf) < n:
                chunk = s.recv(n - len(buf))
                self.assertTrue(chunk, "server closed early")
                buf += chunk
            return buf
        while True:
            head = need(12)
            _, ftype, flags, length = struct.unpack(">BBHI", head[4:12])
            got += head + need(length)
            if flags & bhtp.END_STREAM:
                break
        s.close()
        want = b"".join(f["bytes"] for f in self.resp)
        self.assertEqual(got, want)

    def test_annotation_tables_match_what_the_bytes_decode_to(self):
        for f in self.req + self.resp:
            info, rows = make_examples.decode_frame(f["bytes"])
            self.assertEqual(f["table"], table_lines(rows), f["label"])

    def test_example_body_is_the_served_file(self):
        info, _ = make_examples.decode_frame(self.resp[1]["bytes"])
        with open(os.path.join(REPO, "www", "index.html"), "rb") as fh:
            self.assertEqual(info["body"], fh.read())

    def test_example_request_is_what_the_spec_says_bcurl_sends(self):
        want = bhtp.request_frame("/index.html", [
            ("host", "localhost:9000"), ("user-agent", "bcurl/1.0"), ("accept", "*/*"),
            ("accept-encoding", "identity"), ("connection", "keep-alive")])
        self.assertEqual(self.req[0]["bytes"], want)


if __name__ == "__main__":
    if not os.path.exists(BSERVE):
        sys.exit("bserve not found at %s - run 'make' first" % BSERVE)
    unittest.main(verbosity=1)
