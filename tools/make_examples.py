#!/usr/bin/env python3
"""
make_examples.py - turn REAL captured frames into the annotated hexdumps
examples/request.hexdump and examples/response.hexdump.

Nothing here invents bytes. The input is either

  --from-bcurl STDERR_FILE      the stderr of `bcurl -vv ...` (TX/RX frame dumps)
  --from-raw REQ.bin RESP.bin   raw bytes recorded from a socket

and the frames are decoded strictly according to SPEC.md. While annotating, the
script CHECKS what the assignment asks to be verified, and refuses to write
anything if a check fails:

  * the magic and version are right, the type and flags are the ones expected
  * payload_length equals the number of payload bytes that really follow
  * the frame carries exactly the bytes that were captured (nothing left over)
  * the request's path/headers are what the command asked for
  * the response's content-length equals the length of the DATA payload bytes,
    and those bytes equal the body that bcurl wrote to stdout / the file served
  * END_STREAM is set on the last frame of the response and on no other
"""
import argparse
import os
import re
import struct
import sys

STATIC = ["host", "user-agent", "content-type", "content-length", "accept-encoding",
          "accept", "server", "connection", "last-modified", "cache-control"]
TYPES = {1: "REQUEST", 2: "RESPONSE", 3: "DATA", 4: "ERROR"}
STATUS = {200: "OK", 400: "Bad Request", 404: "Not Found", 500: "Internal Server Error"}


class Bad(Exception):
    pass


# ---------------------------------------------------------------------------
# hex dump in exactly the format `bcurl -v` prints
# ---------------------------------------------------------------------------

def hexdump(data):
    rows = []
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        hexpart = ""
        for i in range(16):
            hexpart += ("%02x " % chunk[i]) if i < len(chunk) else "   "
            if i == 7:
                hexpart += " "
        text = "".join(chr(c) if 0x20 <= c < 0x7F else "." for c in chunk)
        rows.append("%04x: %s |%s|" % (off, hexpart, text))
    return rows


# ---------------------------------------------------------------------------
# strict decoder + annotator
# ---------------------------------------------------------------------------

class Annotator(object):
    def __init__(self, data):
        self.d = data
        self.rows = []            # (offset, bytes, meaning)
        self.pos = 0

    def take(self, n, meaning):
        if self.pos + n > len(self.d):
            raise Bad("frame ends inside field: %s" % meaning)
        chunk = self.d[self.pos:self.pos + n]
        # long fields are shown 8 bytes per row; the meaning goes on the first row
        for i in range(0, max(n, 1), 8):
            part = chunk[i:i + 8]
            self.rows.append((self.pos + i, part, meaning if i == 0 else "  (continued)"))
        self.pos += n
        return chunk

    def section(self, title):
        self.rows.append((None, b"", title))


def q(b):
    return '"%s"' % b.decode("utf-8", "replace").replace("\n", "\\n").replace("\r", "\\r")


def annotate_headers(a):
    count = a.take(1, "header_count")[0]
    a.rows[-1] = (a.rows[-1][0], a.rows[-1][1], "header_count = %d" % count)
    for i in range(count):
        tag = a.take(1, "?")[0]
        if tag == 0:
            a.rows[-1] = (a.rows[-1][0], a.rows[-1][1], "header[%d] tag = 0  -> literal name follows" % i)
            nl = a.take(1, "?")[0]
            a.rows[-1] = (a.rows[-1][0], a.rows[-1][1], "header[%d] name_length = %d" % (i, nl))
            n0 = len(a.rows)
            name = a.take(nl, "?")
            a.rows[n0] = (a.rows[n0][0], a.rows[n0][1], "header[%d] name = %s" % (i, q(name)))
            name = name.decode()
        elif 1 <= tag <= 10:
            name = STATIC[tag - 1]
            a.rows[-1] = (a.rows[-1][0], a.rows[-1][1],
                          "header[%d] tag = %d  -> static name \"%s\"" % (i, tag, name))
        else:
            raise Bad("reserved header tag %d" % tag)
        vl = struct.unpack(">H", a.take(2, "?"))[0]
        a.rows[-1] = (a.rows[-1][0], a.rows[-1][1], "header[%d] value_length = %d" % (i, vl))
        n_before = len(a.rows)
        value = a.take(vl, "?")
        if vl:
            a.rows[n_before] = (a.rows[n_before][0], a.rows[n_before][1],
                                "header[%d] value = %s" % (i, q(value)))
        yield name, value


def decode_frame(data):
    """Returns (info dict, annotation rows). Raises Bad on anything the spec forbids."""
    a = Annotator(data)
    if len(data) < 12:
        raise Bad("shorter than the 12-byte frame header")
    magic = a.take(4, "magic")
    if magic != b"BHTP":
        raise Bad("bad magic")
    a.rows[-1] = (0, magic, 'magic = "BHTP"')
    ver = a.take(1, "version")[0]
    a.rows[-1] = (4, a.rows[-1][1], "version = %d" % ver)
    if ver != 1:
        raise Bad("unsupported version")
    ftype = a.take(1, "type")[0]
    a.rows[-1] = (5, a.rows[-1][1], "type = 0x%02x  %s" % (ftype, TYPES.get(ftype, "UNKNOWN")))
    flags = struct.unpack(">H", a.take(2, "flags"))[0]
    names = [n for bit, n in ((1, "END_STREAM"),) if flags & bit]
    a.rows[-1] = (6, a.rows[-1][1], "flags = 0x%04x  %s" % (flags, "+".join(names) if names else "(none set)"))
    plen = struct.unpack(">I", a.take(4, "payload_length"))[0]
    a.rows[-1] = (8, a.rows[-1][1], "payload_length = %d" % plen)
    if len(data) - 12 != plen:
        raise Bad("payload_length says %d but %d payload bytes were captured" % (plen, len(data) - 12))

    info = {"type": ftype, "flags": flags, "payload_length": plen, "headers": [], "body": b""}
    a.section("payload (%s)" % TYPES.get(ftype, "UNKNOWN"))
    if ftype == 1:
        method = a.take(1, "method")[0]
        a.rows[-1] = (a.rows[-1][0], a.rows[-1][1], "method = 0x%02x  %s" % (method, "GET" if method == 1 else "?"))
        pl = struct.unpack(">H", a.take(2, "path_length"))[0]
        a.rows[-1] = (a.rows[-1][0], a.rows[-1][1], "path_length = %d" % pl)
        n0 = len(a.rows)
        path = a.take(pl, "path")
        a.rows[n0] = (a.rows[n0][0], a.rows[n0][1], "path = %s" % q(path))
        info["method"], info["path"] = method, path
        info["headers"] = list(annotate_headers(a))
    elif ftype == 2:
        status = struct.unpack(">H", a.take(2, "status"))[0]
        a.rows[-1] = (a.rows[-1][0], a.rows[-1][1], "status = %d  %s" % (status, STATUS.get(status, "")))
        info["status"] = status
        info["headers"] = list(annotate_headers(a))
    elif ftype == 3:
        n0 = len(a.rows)
        info["body"] = a.take(plen, "body")
        for i in range(n0, len(a.rows)):
            a.rows[i] = (a.rows[i][0], a.rows[i][1], ("body bytes: " if i == n0 else "  (continued) ") + q(a.rows[i][1]))
    else:
        raise Bad("example frames must be REQUEST, RESPONSE or DATA")
    if a.pos != len(data):
        raise Bad("%d trailing bytes after the last field" % (len(data) - a.pos))
    return info, a.rows


def render_table(rows):
    out = ["Offset  Bytes                      Meaning",
           "------  -------------------------  ---------------------------------------------"]
    for off, b, meaning in rows:
        if off is None:
            out.append("")
            out.append("        -- %s --" % meaning)
            continue
        out.append("0x%04x  %-25s  %s" % (off, " ".join("%02x" % x for x in b), meaning))
    return out


# ---------------------------------------------------------------------------
# splitting a byte stream into frames, and the cross-checks
# ---------------------------------------------------------------------------

def split_frames(stream):
    frames, pos = [], 0
    while pos < len(stream):
        if len(stream) - pos < 12:
            raise Bad("stream ends inside a frame header")
        n = struct.unpack(">I", stream[pos + 8:pos + 12])[0]
        if pos + 12 + n > len(stream):
            raise Bad("stream ends inside a frame payload")
        frames.append(stream[pos:pos + 12 + n])
        pos += 12 + n
    return frames


def parse_bcurl_dump(text):
    """[(dir, seq, bytes)] from the TX/RX dumps that `bcurl -vv` prints."""
    frames, cur = [], None
    for line in text.splitlines():
        m = re.match(r"^(TX|RX) FRAME #(\d+)", line)
        if m:
            cur = [m.group(1), int(m.group(2)), b""]
            frames.append(cur)
            continue
        m = re.match(r"^[0-9a-f]{4}: ((?:[0-9a-f]{2} ?| )+?) +\|", line)
        if m and cur is not None:
            cur[2] += bytes.fromhex(m.group(1).replace(" ", ""))
    return frames


def check_exchange(req, resp_infos, body_expected, path_expected):
    ri = req[0]
    if ri["type"] != 1 or ri["flags"] != 0 or ri["method"] != 1:
        raise Bad("request frame is not a flag-less GET REQUEST")
    if ri["path"] != path_expected:
        raise Bad("request path %r != %r" % (ri["path"], path_expected))
    hdr = dict((n, v) for n, v in ri["headers"])
    if "host" not in hdr:
        raise Bad("request has no host header")
    first = resp_infos[0]
    if first["type"] != 2 or first.get("status") != 200:
        raise Bad("first response frame is not RESPONSE 200")
    body = b"".join(i["body"] for i in resp_infos[1:])
    for i in resp_infos[1:]:
        if i["type"] != 3:
            raise Bad("a non-DATA frame follows the RESPONSE")
    rh = dict((n, v) for n, v in first["headers"])
    if int(rh["content-length"]) != len(body):
        raise Bad("content-length %s != %d DATA bytes" % (rh["content-length"].decode(), len(body)))
    if body != body_expected:
        raise Bad("DATA bytes differ from the file that was served")
    ends = [bool(i["flags"] & 1) for i in resp_infos]
    if ends[-1] is not True or any(ends[:-1]):
        raise Bad("END_STREAM must be set on the last frame only")


def write_example(path, title, command, frames_with_notes):
    lines = ["# " + title,
             "# command: " + command,
             "# These are the bytes that crossed the TCP connection, captured from the real",
             "# programs (see scripts/gen_hexdumps.sh). Nothing below was typed by hand.",
             ""]
    for label, data, rows in frames_with_notes:
        lines.append("%s  (%d bytes)" % (label, len(data)))
        lines.append("")
        lines += ["  " + r for r in hexdump(data)]
        lines.append("")
        lines += ["  " + r for r in render_table(rows)]
        lines.append("")
    with open(path, "w") as f:
        f.write("\n".join(lines).rstrip() + "\n")


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--from-bcurl", metavar="STDERR_FILE")
    g.add_argument("--from-raw", nargs=2, metavar=("REQ_BIN", "RESP_BIN"))
    ap.add_argument("--served-file", required=True, help="the file the server served (to verify the body)")
    ap.add_argument("--path", required=True)
    ap.add_argument("--command", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    if args.from_bcurl:
        dump = parse_bcurl_dump(open(args.from_bcurl).read())
        req_frames = [b for d, _, b in dump if d == "TX"]
        resp_frames = [b for d, _, b in dump if d == "RX"]
    else:
        req_frames = split_frames(open(args.from_raw[0], "rb").read())
        resp_frames = split_frames(open(args.from_raw[1], "rb").read())
    if len(req_frames) != 1:
        sys.exit("expected exactly one request frame, got %d" % len(req_frames))

    req = decode_frame(req_frames[0])
    resp = [decode_frame(f) for f in resp_frames]
    check_exchange(req, [i for i, _ in resp], open(args.served_file, "rb").read(), args.path.encode())

    os.makedirs(args.out, exist_ok=True)
    write_example(os.path.join(args.out, "request.hexdump"),
                  "BHTP/1 example: one complete REQUEST frame (client -> server)", args.command,
                  [("TX FRAME #1  REQUEST", req_frames[0], req[1])])
    write_example(os.path.join(args.out, "response.hexdump"),
                  "BHTP/1 example: the complete response (server -> client): RESPONSE + DATA", args.command,
                  [("RX FRAME #%d  %s" % (i + 1, TYPES[info["type"]]), resp_frames[i], rows)
                   for i, (info, rows) in enumerate(resp)])
    print("verified and wrote %s/request.hexdump and %s/response.hexdump (%d response frames)"
          % (args.out, args.out, len(resp)))


if __name__ == "__main__":
    try:
        main()
    except Bad as e:
        sys.exit("make_examples: CHECK FAILED: %s" % e)
