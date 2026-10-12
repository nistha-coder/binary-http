# BHTP/1 — Binary Hypertext Transfer Protocol, version 1

Status: complete specification of the wire protocol implemented by `bserve` (server)
and `bcurl` (client). It is written so that a client or server can be built from this
file alone. The key words MUST, MUST NOT, SHOULD and MAY are used as in RFC 2119.

BHTP/1 is a small, sequential request/response protocol for fetching files, in the spirit
of HTTP/2's framing but deliberately simpler: one request at a time, no multiplexing.

## 1. Transport and byte order

* Transport: TCP, any port (the examples use 9000). One TCP connection carries any number
  of exchanges, one after another (section 8).
* TCP is a byte stream. A frame may arrive split over many reads, and many frames may
  arrive in one read. Receivers MUST NOT assume that one `read()` is one frame.
* Every multi-byte integer is **unsigned and big-endian** (network byte order).

## 2. Frame

Every message is one frame: a fixed 12-byte header followed by `payload_length` bytes.

```
 byte:   0       1       2       3       4       5       6       7
      +-------+-------+-------+-------+-------+-------+-------+-------+
      |  'B'  |  'H'  |  'T'  |  'P'  |version| type  |     flags     |
      +-------+-------+-------+-------+-------+-------+-------+-------+
 byte:   8       9       10      11    12 ...
      +-------+-------+-------+-------+----------------------------- - -
      |         payload_length        |  payload (payload_length bytes)
      +-------+-------+-------+-------+----------------------------- - -
```

| Offset | Size | Field | Value | Why this width |
|---|---|---|---|---|
| 0 | 4 | `magic` | `42 48 54 50` ("BHTP") | A peer speaking something else (for example HTTP text) is rejected on the first frame instead of being misparsed; a fixed marker also makes hexdumps easy to read. |
| 4 | 1 | `version` | `1` | 255 versions are plenty; one byte keeps the header aligned. |
| 5 | 1 | `type` | 1 REQUEST, 2 RESPONSE, 3 DATA, 4 ERROR | Four types today; 256 values leave room for version 2 (section 10). |
| 6 | 2 | `flags` | bit 0 = `END_STREAM` (0x0001); bits 1–15 reserved | 16 flag bits cost one extra byte and avoid a future format change. |
| 8 | 4 | `payload_length` | 0 .. 4 294 967 295 on the wire; see section 3 for what is accepted | 32 bits so that the length field never has to change. The *accepted* maximum is a separate, smaller limit (16 384). |

Header size is always exactly 12 bytes. A receiver finds the end of a frame only from
`payload_length`: the frame ends 12 + `payload_length` bytes after it began.

Differences from HTTP/2 (24/8/8/31): there is no stream identifier because exchanges
are strictly sequential, so the header carries no 31-bit stream ID; instead it has a magic
and a version for early detection of non-BHTP peers; and the length is 32 bits wide while
a small separate cap (16 384, the same value as HTTP/2's default maximum frame size) bounds
what is actually accepted.

## 3. Reading a frame (normative algorithm)

1. Read exactly 12 bytes. EOF before the first byte: the peer closed cleanly between
   frames. EOF after 1–11 bytes: truncated frame.
2. If `magic` is not "BHTP": fatal protocol error (section 9).
3. If `version` is not 1: fatal protocol error.
4. If `type` is **not** 1–4: **skip** the frame (section 10): read and discard exactly
   `payload_length` bytes — whatever the length, flags or content — then go to step 1.
5. Otherwise (known type): if `payload_length` > **16 384**: fatal protocol error, and the
   payload MUST NOT be read or buffered. Else read exactly `payload_length` bytes.
6. If `flags` has any bit set that is not defined for this type (section 4): the frame is
   malformed, but it has been consumed completely, so the stream is still in sync.
7. Process the frame, then go to step 1.

Memory use is therefore bounded by 16 384 bytes of payload plus a small fixed skip
buffer, regardless of what a peer claims in `payload_length`.

## 4. Frame types

| Type | Name | Sender | Flags allowed | Payload | May repeat? |
|---|---|---|---|---|---|
| 0x01 | REQUEST | client | none (0x0000) | section 5.1 | once per exchange |
| 0x02 | RESPONSE | server | `END_STREAM` | section 5.2 | once per exchange |
| 0x03 | DATA | server | `END_STREAM` | 0–16 384 raw body bytes | zero or more times |
| 0x04 | ERROR | server | none | section 5.3 | at most once, then close |
| other | *unknown* | either | any (ignored) | any (skipped) | anywhere |

`END_STREAM` on RESPONSE means "no DATA frames follow"; on DATA it means "this is the
last body frame". Maximum payload of every known type: 16 384 bytes.

## 5. Payloads

### 5.1 REQUEST

```
method        u8       0x01 = GET            (anything else is malformed)
path_length   u16      1 .. 4096
path          bytes    path_length bytes (rules: section 11)
headers       header block (section 6)
```
The payload MUST end exactly after the last header; extra bytes are malformed.

### 5.2 RESPONSE

```
status        u16      100 .. 599
headers       header block (section 6)
```
Defined statuses: **200** OK, **400** Bad Request, **404** Not Found, **500** Internal
Server Error. A client receiving another status in 100–599 MUST treat it by class
(≥ 400 is a failure). The server SHOULD send `content-length` in every response; for a
response without body it is `0` and the RESPONSE carries `END_STREAM`.

### 5.3 DATA and ERROR

DATA payload = body bytes, verbatim (the body is binary-safe; no encoding is applied).
A body longer than 16 384 bytes is split over several DATA frames; only the last carries
`END_STREAM`. A zero-length DATA frame with `END_STREAM` is legal.

ERROR payload: `status u16` (100–599) followed by the rest of the payload as a UTF-8
reason text of 0–256 bytes. It reports a fatal protocol error; the sender then closes.

## 6. Header block and header names

```
count         u8       0 .. 32
count times:
  tag         u8       0 = literal name follows; 1..10 = static name; 11..255 reserved
  [name_len   u8       1..255 ]   only when tag = 0
  [name       bytes    name_len bytes of [a-z 0-9 -] ]   only when tag = 0
  value_len   u16      0 .. 4096
  value       bytes    value_len bytes
```

This is HPACK's first two ideas without the rest: a **static table** of the ten names the
protocol actually sends, referred to by a one-byte number, and **length-prefixed literals**
for everything else. No dynamic table, no Huffman coding.

| Tag | Name | Used by | Tag | Name | Used by |
|---|---|---|---|---|---|
| 1 | `host` | request | 6 | `accept` | request |
| 2 | `user-agent` | request | 7 | `server` | response |
| 3 | `content-type` | response | 8 | `connection` | both |
| 4 | `content-length` | response | 9 | `last-modified` | response |
| 5 | `accept-encoding` | request | 10 | `cache-control` | response |

* A sender SHOULD use the tag when the name is in the table (1 byte instead of 2 + length);
  a receiver MUST also accept the same name sent as a literal.
* Names are lowercase; a name with other characters, or an empty name, is malformed.
* A tag of 11–255, a `count` above 32, a `value_len` above 4096, or a length that runs past
  the end of the payload make the whole payload malformed. Duplicate names are allowed;
  for single-valued headers the first one wins.
* Header values are opaque bytes (normally ASCII). `content-length` is decimal digits only.
* `connection: close` in a request asks the server to close after answering;
  `connection: keep-alive` is the default.

## 7. Response completeness

A response is: one RESPONSE frame, then DATA frames until one carries `END_STREAM` (or none
if the RESPONSE itself carries it). The client knows the response is complete exactly when
it has seen `END_STREAM`. If `content-length` is present, the DATA payloads MUST add up to
it; a client that sees a different total, DATA before RESPONSE, or a second RESPONSE
treats the connection as failed.

## 8. Connection lifecycle

* **Open:** the client opens one TCP connection. A client MUST send all its requests on
  that one connection and MUST NOT open another to the same server.
* **Exchanges:** REQUEST → RESPONSE (+ DATA…), repeated any number of times. The connection
  stays open after every response; any number of requests are allowed. A client SHOULD wait
  for the complete response before sending the next request; a server MUST answer requests
  that arrive back-to-back, in order, one response each.
* **Idle:** a server MAY close a connection that stays silent for a long time (the
  reference server: 300 s).
* **Close, normal:** the client closes (the server sees EOF on a frame boundary), or the
  server closes after a request carrying `connection: close`.
* **Close, abnormal:** EOF in the middle of a frame, or a fatal protocol error (section 9).

## 9. Errors

Three layers, handled differently:

| Layer | Condition | Server action | Connection |
|---|---|---|---|
| Transport | EOF inside a frame, reset, timeout | none possible | closed |
| Protocol (fatal: stream cannot be trusted) | bad magic; version ≠ 1; known type with `payload_length` > 16 384 | send ERROR 400 | closed |
| Application, malformed but in sync | REQUEST payload malformed (section 5.1/6); undefined flag bits on a known type; RESPONSE/DATA/ERROR sent by a client | RESPONSE 400 (`content-length: 0`) | **stays open** |
| Application | method ≠ GET; path invalid (section 11) | RESPONSE 400 | stays open |
| Application | file missing, directory without `index.html`, outside root, not a regular file | RESPONSE 404 | stays open |
| Application | file exists but cannot be read; other server fault | RESPONSE 500 | stays open |
| Forward compatibility | unknown frame type | nothing (skipped) | stays open |

Reference client exit status (not part of the wire protocol): 0 when every response is
below 400; 1 when some response is 4xx/5xx or an ERROR frame arrived; 2 on a transport or
protocol failure; 64 for a bad command line.

## 10. Unknown frames and versioning

A receiver that meets a frame type it does not know **MUST NOT fail or close**. It MUST
read the 12-byte header, take `payload_length`, skip exactly that many bytes, and continue
with the next frame. This holds for any length (the bytes are discarded as they arrive, not
buffered), any flags, anywhere in the stream — including between the RESPONSE and DATA
frames of one response, or before the first byte of an answer. Skipped frames produce no
reply. This is how version-1 peers stay compatible with a future peer that adds frame
types: new types are invisible to old receivers. Type 0x00 and 0x05–0xFF are unknown in
version 1. Changes that old receivers cannot ignore require a new `version`.

## 11. Path rules (server)

The path is an opaque byte string; it is **not** percent-decoded (so `%2e%2e` is just a
file name). The server maps it to a file under its root directory:

1. It MUST be non-empty, begin with `/`, and be at most 4096 bytes; else 400.
2. Bytes below 0x20, 0x7F and `\` are rejected (400).
3. Split on `/`; empty segments (repeated slashes) and `.` segments are ignored.
4. A `..` segment is rejected with 400, even if it would stay inside the root.
5. The result is joined to the root, canonicalised (symbolic links resolved), and MUST lie
   inside the root; otherwise 404 (the server does not reveal that something exists).
6. `/` and any directory map to `index.html` inside it, else 404; `/file/` is 404.
7. Only regular files are served; anything else is 404.

## 12. Limits

Header 12 bytes · payload ≤ 16 384 (known types) · path ≤ 4096 · headers ≤ 32 per block ·
literal name ≤ 255 · header value ≤ 4096 · ERROR reason ≤ 256.

## 13. Worked example (real bytes; the full annotated dump is in `examples/`)

Request `GET /x` with `host: localhost` (static tag 1) and a custom header `x-id: 7`
(literal name). 39 bytes:

```
42 48 54 50 01 01 00 00 00 00 00 1b   magic "BHTP", version 1, type 1 REQUEST, flags 0, length 27
01                                     method GET
00 02 2f 78                            path_length 2, path "/x"
02                                     2 headers
01 00 09 6c 6f 63 61 6c 68 6f 73 74    tag 1 = host, value_len 9, "localhost"
00 04 78 2d 69 64 00 01 37             tag 0 = literal, name_len 4 "x-id", value_len 1, "7"
```

The server answers 404 (file does not exist). 45 bytes, one frame:

```
42 48 54 50 01 02 00 01 00 00 00 21   type 2 RESPONSE, flags 0x0001 END_STREAM, length 33
01 94                                  status 404
03                                     3 headers
07 00 0a 62 73 65 72 76 65 2f 31 2e 30 tag 7 = server, "bserve/1.0"
04 00 01 30                            tag 4 = content-length, "0"
08 00 0a 6b 65 65 70 2d 61 6c 69 76 65 tag 8 = connection, "keep-alive"
```
The connection stays open for the next request.
