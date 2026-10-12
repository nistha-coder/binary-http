# BHTP/1 server - `bserve` (Nistha)

Track 1 of the course project **"HTTP, in binary"**: a TCP file server that speaks a binary
protocol of our own design, written in C11 with plain POSIX sockets. Its partner is
Veekshitha's client, `bcurl` (`veekshitha-binary-http`). **The only thing that crosses between
the two projects is [SPEC.md](SPEC.md)** (the same file in both repositories).

## Quick start

```sh
git clone <your-repo-url> nistha-binary-http && cd nistha-binary-http
make                      # builds ./bserve
make test                 # unit + integration tests; ends with "ALL TESTS PASSED"
./bserve ./www 9000       # terminal 1: serve ./www on port 9000
python3 tools/pyfetch.py localhost:9000/index.html      # terminal 2 (or Veekshitha's ./bcurl, same arguments)
```

## 1. Project overview

`bserve <root_directory> <port>` accepts TCP connections, reads binary request frames, maps the
requested path to a file below the root, and answers with a status, headers and the file's bytes. It answers
404 for a missing file, 400 for a malformed request, and **keeps the connection open** for the next request.
It never trusts a length it read from the network, and it survives garbage input (see the fuzz test).

## 2. Assignment requirements (Track 1 - the server)

```
$ ./bserve ./www 9000
accept a TCP connection        read one binary request frame     map the path to a file under a root
reply: status, headers, bytes  404 if it is not there            400 if the frame is malformed
and keep the connection open
```

The part the assignment calls "the actual project" is the protocol: a fixed-size frame header whose
field widths are defended (HTTP/2 chose 24/8/8/31; see *Design decisions* and `VIVA.md`), ten numbered
header names with the rest length-prefixed, and **one line that may not be skipped: a receiver meeting
a frame type it does not know must skip it cleanly.** Hand-in: the spec (`SPEC.md`), the program, and an annotated hexdump
of one complete request and response (`examples/`).

## 3. Protocol overview

BHTP/1 (Binary Hypertext Transfer Protocol, version 1) is a small sequential
request/response protocol over TCP. A client sends one **REQUEST** frame; the server
answers with one **RESPONSE** frame (status + headers) followed by zero or more **DATA**
frames (the file's bytes). A server that sees a broken byte stream says so with one
**ERROR** frame and closes. Everything is binary and big-endian. The complete,
implementation-independent definition is [SPEC.md](SPEC.md); this README only summarises it.

## 4. Architecture

```
            bserve (this repo)                              bcurl (Veekshitha's repo)
   +------------------------------+                 +------------------------------+
   | server.c   accept loop, one  |                 | client.c   one connection    |
   |            process per conn, |     TCP         |            sequential fetch  |
   |            request handling  | <-------------> | bcurl.c    command line      |
   | pathres.c  path -> file      |   BHTP/1 frames | hexdump.c  -v output         |
   +--------------+---------------+   (SPEC.md)     +--------------+---------------+
   | frame.c  read/write a frame  |                 | frame.c      (same layer)    |
   | protocol.c  encode/decode    |                 | protocol.c                   |
   | io.c   read_exact/write_all  |                 | io.c                         |
   +------------------------------+                 +------------------------------+
```

| File | Responsibility |
|---|---|
| `src/bserve.c` | command line only |
| `src/server.c` | accept loop, per-connection request loop, request handling, logging |
| `src/pathres.c` | URL path -> file path, all security rules |
| `src/network.c` | listening socket (IPv4 + IPv6), timeouts, graceful close |
| `src/frame.c` | read one whole frame (header, validation, skip unknown), write one frame |
| `src/protocol.c` | pure encode/decode of header, REQUEST, RESPONSE, ERROR and header blocks (no I/O) |
| `src/io.c` | `io_read_exact`, `io_write_all`: partial transfers, EINTR, EOF vs truncation |

## 5. Client/server flow

1. `bserve` resolves the root with `realpath`, creates a listening socket, and logs `serving ... on port N`.
2. `accept()` -> logs `[conn=N] accepted` -> `fork()`; the child serves that connection, the parent keeps accepting.
3. The child loops: read one frame -> if the type is unknown, skip it -> if it is a REQUEST, decode it,
   resolve the path, send RESPONSE (+ DATA frames) -> log -> read the next frame.
4. EOF on a frame boundary ends the loop normally (`closed (client closed the connection)`).

## 6. Frame format

Every frame is a **fixed 12-byte header** followed by `payload_length` bytes:

```
Offset  Size  Field           Value
0       4     magic           "BHTP" (42 48 54 50)
4       1     version         1
5       1     type            1 REQUEST, 2 RESPONSE, 3 DATA, 4 ERROR (anything else: unknown)
6       2     flags           bit 0 = END_STREAM; other bits reserved (must be 0)
8       4     payload_length  bytes that follow the header (known types: at most 16 384)
```

| Type | Sender | Payload |
|---|---|---|
| REQUEST (1) | client | `method u8 (1 = GET)`, `path_length u16`, `path`, header block |
| RESPONSE (2) | server | `status u16` (200/400/404/500), header block. `END_STREAM` = "no DATA follows" |
| DATA (3) | server | raw body bytes (0–16 384). `END_STREAM` on the last one |
| ERROR (4) | server | `status u16` + UTF-8 reason; then the server closes |

The receiver finds the end of a frame only from `payload_length`: read exactly 12 bytes,
decode the length, validate it, read exactly that many bytes, process, repeat. A client
knows a response is complete when it sees `END_STREAM`.

## 7. Header encoding

Headers follow the first two ideas of HPACK, and nothing else (no dynamic table, no Huffman):

1. **Static table** - the ten names the protocol actually sends are numbered, so
   `content-type` costs 1 byte instead of 13.
2. **Length-prefixed literals** - any other name is sent as `0`, a 1-byte length, and the name.

```
header block := count:u8 (max 32)  { tag:u8  [name_len:u8 name]  value_len:u16  value }*
                tag 1..10 = static name | tag 0 = literal name follows | tag 11..255 = reserved (malformed)
```

| Tag | Name | Tag | Name |
|---|---|---|---|
| 1 | `host` | 6 | `accept` |
| 2 | `user-agent` | 7 | `server` |
| 3 | `content-type` | 8 | `connection` |
| 4 | `content-length` | 9 | `last-modified` |
| 5 | `accept-encoding` | 10 | `cache-control` |

All ten are really sent: the client sends tags 1, 2, 5, 6, 8 and the server sends tags 3, 4, 7, 8, 9, 10.
Limits: 32 headers per block, literal names 1-255 bytes of `[a-z0-9-]`, values at most 4096 bytes.

## 8. Persistent connections

One TCP connection carries any number of exchanges:

```
client                                  server
  | --- REQUEST #1 ------------------->  |
  | <-- RESPONSE #1 (+ DATA ...) -------  |
  | --- REQUEST #2 ------------------->  |      same TCP connection,
  | <-- RESPONSE #2 (+ DATA ...) -------  |      no second connect()
  | --- REQUEST #3 ------------------->  |
  | <-- RESPONSE #3 (+ DATA ...) -------  |
  | --- FIN (client closes) ----------->  |
```

* **Opened** by the client's single `connect()`.
* **Stays open** after every response, including 400, 404 and 500 responses.
* **Closes** when the client closes (the server sees EOF on a frame boundary), after a request
  carrying `connection: close`, after a fatal protocol error (ERROR frame first), when the
  connection is idle for 300 s, or when the peer vanishes in the middle of a frame.
* Any number of requests and responses per connection; exchanges are strictly sequential.

## 9. Unknown frame handling

A receiver that meets a frame type it does not know **must not fail**. It reads the 12-byte
header, takes `payload_length`, **skips exactly that many bytes**, and continues with the next
frame. That is what lets a future version 2 add frame types without breaking version-1 peers.
It works for any length (the bytes are streamed through a 4 KiB buffer, never allocated), any
flags, and at any position, including between the RESPONSE and DATA frames of one answer. The
server sends no reply to a skipped frame.

## 10. Error handling

| Layer | Examples | What happens |
|---|---|---|
| **A. Transport** | EOF mid-frame, reset, timeout | connection closed; `bcurl` exits 2 |
| **B. Protocol, fatal** (framing can no longer be trusted) | bad magic, version != 1, known type with length > 16 384 | server sends `ERROR 400`, closes |
| **C. Application, recoverable** (framing intact) | malformed REQUEST payload, undefined flag bits, bad path, method != GET | `RESPONSE 400`, **connection stays open** |
| **C. Application** | file missing / directory without index / outside root | `RESPONSE 404`, stays open |
| **C. Application** | file exists but cannot be read | `RESPONSE 500`, stays open |

`bcurl` exit status: **0** every response below 400 - **1** some response was 4xx/5xx (or the
server sent an ERROR frame) - **2** transport or protocol failure - **64** bad command line.

## 11. Security and path handling

The server serves only regular files under the root directory it was started with
(full rules: SPEC.md section 11). The path is an opaque byte string and is **not** percent-decoded.

| Request path | Result | Rule |
|---|---|---|
| `/` , `/sub/` , `/sub` | `index.html` of that directory, else 404 | directories map to `index.html` |
| `/../../etc/passwd` , `/sub/../../x` | **400** | any `..` segment is refused, even a harmless one |
| `/%2e%2e/%2e%2e/etc/passwd` | 404 | `%` is an ordinary byte: it names a directory that does not exist |
| `//etc/passwd` , `/etc/passwd` | 404 | absolute-looking paths are relative to the root: `<root>/etc/passwd` |
| `etc/passwd` , empty path | 400 | must begin with `/` |
| `/a//b` , `/a/./b` | same as `/a/b` | repeated slashes and `.` segments are dropped |
| bytes < 0x20, 0x7F, `\` | 400 | control characters and backslashes |
| symlink pointing outside the root | 404 | the canonical (`realpath`) result must lie inside the root |
| device, FIFO, socket | 404 | regular files only |

## 12. Requirements and dependencies

* A C11 compiler (`cc`/`gcc`/`clang`) and `make`.
* Python 3 for the integration tests and the helper client `tools/pyfetch.py` (standard library only; tested with 3.12).
* Linux or another POSIX system. Developed and tested on Ubuntu 24.04 with gcc 13. `make test-unit` links one test
  with GNU ld's `--wrap`, so the unit tests need GNU binutils (Linux); `make` itself needs only POSIX.
* No other libraries, no network access, no configuration files.

## 13. Build

```sh
make            # ./bserve   (warnings are enabled: -Wall -Wextra -Wpedantic; there are none)
make clean      # remove ./bserve and ./build
```

## 14. Run

```sh
./bserve <root_directory> <port>     # e.g. ./bserve ./www 9000   (defaults: ./www and 9000)
```

The server logs to **stderr**, one line per event, so you can watch connections live:

```
bserve: serving /abs/path/to/www on port 9000
[conn=1] accepted
[conn=1] request #1 GET /index.html
[conn=1] response #1 200 OK (213 bytes)
[conn=1] closed (client closed the connection)
```

## 15. Tests

```sh
make test          # everything below
make test-unit     # C unit tests only
make asan          # the same tests again, with AddressSanitizer + UBSan
```

| Suite | What it covers |
|---|---|
| `tests/test_protocol.c` | encode/decode round trips (request, response, headers, errors, edge cases), every truncation, invalid tags/lengths/names, random-byte decoding |
| `tests/test_frame.c` | frame reading: valid, unknown skipped, bad magic/version, oversize, truncation, flags, EINTR |
| `tests/test_shortio.c` | forced short reads and writes: frames delivered 1 byte at a time |
| `tests/test_pathres.c` | the path resolver: `..`, `.`, `//`, `%2e`, symlinks, directories, special files, length limits |
| `tests/test_server.py` | **integration**: starts the real `bserve`, talks raw BHTP frames from a Python peer written from SPEC.md alone: all required HTTP-like cases, protocol cases, TCP cases, persistent connection, security, fuzzing |
| `tests/test_examples.py` | the checked-in hexdumps are exactly what the live server sends |

## 16. Verification - "is it actually working?"

`scripts/verify.sh` runs the whole procedure below and prints PASS/FAIL per step. By hand:
(terminal 1: `./bserve ./www 9000`; terminal 2: `CLIENT="python3 tools/pyfetch.py"`, or `CLIENT=../veekshitha-binary-http/bcurl`).

| Step | Command | Expected |
|---|---|---|
| 1 Build | `make` | no compiler errors or warnings |
| 2 Files | `ls www` | `binary.bin empty.txt hello.txt index.html sub` |
| 3 Start | `./bserve ./www 9000` | `bserve: serving .../www on port 9000` |
| 4 Basic | `$CLIENT localhost:9000/index.html` | the contents of `www/index.html`, exit 0 |
| 5 Verbose | `$CLIENT -v localhost:9000/hello.txt` | `TX FRAME #1 REQUEST ...`, `RX FRAME #1 RESPONSE ...`, `RX FRAME #2 DATA ...` with hex lines |
| 6 404 | `$CLIENT localhost:9000/nope.html; echo $?` | `-> 404 Not Found` and `1` |
| 7 One connection | `$CLIENT localhost:9000/index.html /hello.txt /missing.txt` | server log: **one** `accepted`, `request #1..#3`, no `[conn=N+1]` |
| 8 Unknown frame | snippet A below | `200 200`, log `skipped unknown frame type 0x7f` |
| 9 Malformed | snippet B below | `400` then `200` on the same connection; server alive |
| 10 Traversal | `$CLIENT localhost:9000/../../etc/passwd; echo $?` | `-> 400 Bad Request`, `1`, nothing leaked |
| 11 Binary | `$CLIENT localhost:9000/binary.bin > /tmp/out.bin; cmp www/binary.bin /tmp/out.bin && echo identical` | `identical` (and equal `sha256sum`) |
| 12 Suite | `make test` | `==== ALL TESTS PASSED ====` |

Snippet A (step 8): known frame, **unknown frame (type 0x7f)**, known frame, in one write:
```sh
python3 - <<'EOF'
import sys; sys.path.insert(0, "tests"); import bhtp
c = bhtp.Conn("127.0.0.1", 9000)
c.send(bhtp.request_frame("/hello.txt") + bhtp.frame(0x7F, b"future") + bhtp.request_frame("/index.html"))
print("unknown-frame demo ->", c.read_response().status, c.read_response().status)
EOF
```
Snippet B (step 9): a REQUEST frame whose payload is garbage, then a good request on the same connection:
```sh
python3 - <<'EOF'
import sys; sys.path.insert(0, "tests"); import bhtp
c = bhtp.Conn("127.0.0.1", 9000)
c.send(bhtp.frame(bhtp.REQUEST, b"\xff\xff\xff")); print("malformed request ->", c.read_response().status)
c.send(bhtp.request_frame("/hello.txt"));            print("same connection    ->", c.read_response().status)
EOF
```

**Proving "one TCP connection"**, two independent ways: (a) the server log above has a single
`[conn=N] accepted` for all three requests; (b) `test_three_exchanges_use_exactly_one_connection` asserts it
automatically.

**Binary correctness:** step 11 compares bytes with `cmp`; the integration tests compare a 3 MB random file
byte for byte and its SHA-256.

## 17. Example commands and output

Real output, captured from this server with Veekshitha's `bcurl`:

```
$ ./bcurl localhost:9000/index.html
<!DOCTYPE html>
<html>
<head>
    <title>Nistha BHTP Server</title>
</head>
<body>
    <h1>Hello from Nistha's BHTP Server!</h1>
    <p>This page was served using the Binary HTTP-like Protocol.</p>
</body>
</html>
[exit=0]
```
```
$ ./bcurl localhost:9000/nope.html
bcurl: GET /nope.html -> 404 Not Found
[exit=1]
```
```
$ ./bcurl localhost:9000/../../etc/passwd
bcurl: GET /../../etc/passwd -> 400 Bad Request
[exit=1]
$ ./bcurl localhost:9000/%2e%2e/%2e%2e/etc/passwd
bcurl: GET /%2e%2e/%2e%2e/etc/passwd -> 404 Not Found
[exit=1]
$ ./bcurl localhost:9000/sub/../../x
bcurl: GET /sub/../../x -> 400 Bad Request
[exit=1]
$ ./bcurl localhost:9000//etc/passwd
bcurl: GET //etc/passwd -> 404 Not Found
[exit=1]
$ ./bcurl localhost:9000/sub/
<!DOCTYPE html>
<html><body><h1>Sub directory index</h1></body></html>
[exit=0]
$ ./bcurl localhost:9000/sub
<!DOCTYPE html>
<html><body><h1>Sub directory index</h1></body></html>
[exit=0]
```
Server log for `bcurl localhost:9000/index.html /hello.txt /missing.txt` (three requests, **one** connection):
```
[conn=4] accepted
[conn=4] request #1 GET /index.html
[conn=4] response #1 200 OK (213 bytes)
[conn=4] request #2 GET /hello.txt
[conn=4] response #2 200 OK (87 bytes)
[conn=4] request #3 GET /missing.txt
[conn=4] response #3 404 Not Found
[conn=4] closed (client closed the connection)
```
Server log for snippets A and B:
```
bserve: serving /abs/path/to/www on port 9000
[conn=1] accepted
[conn=1] request #1 GET /hello.txt
[conn=1] response #1 200 OK (87 bytes)
[conn=1] skipped unknown frame type 0x7f (6 payload bytes)
[conn=1] request #2 GET /index.html
[conn=1] response #2 200 OK (213 bytes)
[conn=1] closed (client closed the connection)
[conn=2] accepted
[conn=2] request #1 malformed (unknown method)
[conn=2] response #1 400 Bad Request
[conn=2] request #2 GET /hello.txt
[conn=2] response #2 200 OK (87 bytes)
[conn=2] closed (client closed the connection)
[conn=3] accepted
[conn=3] fatal protocol error: bad magic (not a BHTP frame)
[conn=3] closed (protocol error)
```

## 18. Hexdump example

`examples/request.hexdump` and `examples/response.hexdump` hold one complete, real exchange
(`GET /index.html`): the raw bytes as they crossed the TCP connection, followed by a table that
annotates every byte. They are **generated, never typed**: `scripts/gen_hexdumps.sh` runs the
real programs, and `tests/test_examples.py` replays the example request against a live `bserve` and fails if the response bytes or the annotations differ. An excerpt of the request:

```
TX FRAME #1  REQUEST  (86 bytes)
  0000: 42 48 54 50 01 01 00 00  00 00 00 4a 01 00 0b 2f  |BHTP.......J.../|
  ...
  Offset  Bytes                      Meaning
  0x0000  42 48 54 50                magic = "BHTP"
  0x0004  01                         version = 1
  0x0005  01                         type = 0x01  REQUEST
  0x0006  00 00                      flags = 0x0000  (none set)
  0x0008  00 00 00 4a                payload_length = 74
  0x000c  01                         method = 0x01  GET
  0x000d  00 0b                      path_length = 11
  0x000f  2f 69 6e 64 65 78 2e 68    path = "/index.html"
  0x001a  05                         header_count = 5
  0x001b  01                         header[0] tag = 1  -> static name "host"
```

(The `last-modified` value inside the response example depends on the file's modification time;
the generator pins it to 2026-01-01 so the bytes are reproducible.)

## 19. Repository structure

```
nistha-binary-http/
├── README.md  SPEC.md  VIVA.md  Makefile  .gitignore
├── include/    frame.h  io.h  network.h  pathres.h  protocol.h  server.h
├── src/        bserve.c  server.c  network.c  pathres.c  protocol.c  frame.c  io.c
├── tests/      test_protocol.c  test_frame.c  test_shortio.c  test_pathres.c
│               test_server.py  test_examples.py  bhtp.py (Python peer written from SPEC.md)
├── tools/      pyfetch.py (helper client)  capture_frames.py  make_examples.py
├── scripts/    verify.sh  gen_hexdumps.sh
├── examples/   request.hexdump  response.hexdump
└── www/        index.html  hello.txt  empty.txt  binary.bin  sub/index.html  sub/page.txt
```

## 20. Design decisions

| Decision | Why |
|---|---|
| 12-byte fixed header with magic and version | A fixed size means one `read_exact(12)` always works. Magic rejects non-BHTP peers (HTTP text, port scanners) on the very first frame; version gives a path to version 2. |
| `payload_length` is u32, accepted limit 16 384 | The field never has to change; the limit is a policy that can be raised later. 16 384 is the HTTP/2 default frame size and keeps every buffer on the stack. |
| `type` u8, `flags` u16 | 256 types and 16 flag bits leave room to grow at a cost of three bytes. |
| No stream/request ID (HTTP/2 spends 31 bits on it) | Exchanges are strictly sequential, so a response always belongs to the oldest request. An ID would be dead weight. |
| Big-endian | Network byte order: `00 00 00 4a` reads as 74 in a hexdump, and no byte swapping depends on the host CPU. |
| Body split into DATA frames with `END_STREAM` | Memory stays bounded for any file size; the client knows exactly when the response ends without closing the connection. |
| Static table of 10 + literals | HPACK's two cheapest ideas: the common names cost one byte, anything else still works. |
| Unknown frame types are skipped | The one rule that makes extension possible. |
| Malformed payload -> 400 and stay open; broken framing -> ERROR and close | If the length field was honoured the stream is still in sync, so there is no reason to drop a healthy connection. If it was not, nothing after it can be trusted. |
| Server forks one process per connection | The simplest thing that lets a persistent connection coexist with other clients; each process has its own stack, so a bug cannot corrupt another connection. |

## 21. Limitations and future improvements

* One process per connection (`fork`): simple and isolated, but not for thousands of clients. `poll`/`epoll` or threads would scale better.
* GET only; no HEAD, ranges, conditional requests or compression.
* No TLS. No authentication. Connections are cut after 300 s idle; there is no limit on the number of connections.
* The path is resolved and then opened (a classic time-of-check/time-of-use window): fine for a static tree, but a hostile local user who can swap symlinks inside the root could race it.
* Header values are not validated beyond their length; `content-length` is produced by the server, parsed strictly by the client.
* Natural version 2: new frame types (e.g. `PING`, `SETTINGS`) can be added today because version-1 peers skip them.

## 22. Requirements matrix

| Requirement | Implemented | Tested | How to verify |
|---|---|---|---|
| TCP server accepts connections | YES | YES | `./bserve ./www 9000`; every test in `test_server.py` starts a real server |
| Reads binary request frames | YES | YES | `TestProtocol.test_08_valid_frame_parsing_every_header_form` |
| Maps path to a file under a root; replies status, headers, bytes | YES | YES | `TestBasic` 01-04 (HTML, text, empty file, binary spanning DATA frames, 3 MB file) |
| 404 if missing | YES | YES | `TestBasic.test_05_missing_file_is_404`; step 6 |
| 400 if the frame is malformed | YES | YES | `TestBasic.test_06...` (connection survives), `TestProtocol` 09-17; step 9 |
| 500 on internal failure | YES | YES | `TestBasic.test_07_internal_error_is_500` (unreadable file; runs as an unprivileged user) |
| Keeps the connection open | YES | YES | `TestPersistentConnection` (`test_three_exchanges_use_exactly_one_connection`, `test_connection_stays_open_while_idle`, `test_errors_do_not_end_the_connection`) |
| Several requests, one TCP connection (log `[conn=N]`) | YES | YES | step 7; `test_three_exchanges_use_exactly_one_connection`, `test_many_requests_on_one_connection` |
| Path traversal protection | YES | YES | `TestPathSecurity`, `test_pathres.c`; step 10 |
| Path rules: `/`, relative, `../`, absolute, encoded, directories, empty, `//` | YES | YES | `TestPathSecurity` 22-29, `TestProtocol.test_17...` |
| Serves other clients while one connection is idle | YES | YES | `test_connections_are_served_concurrently` |
| Survives malformed input / fuzzing | YES | YES | `TestFuzz.test_server_survives_malformed_input` (400 random frames, fixed seed) |
| Binary frames, fixed-size header, fields defended | YES | YES | `SPEC.md` s2-3; `make test-unit` (`test_frame`, `test_protocol`) |
| Header compression/indexing (10 static names + length-prefixed literals) | YES | YES | `SPEC.md` s6; `test_protocol`; ``test_server.py`` header tests |
| Unknown frame type is skipped, next frame still processed | YES | YES | step 8 below; `test_frame`, `test_shortio` (one-byte reads), ``test_server.py`` unknown-frame tests |
| TCP is a byte stream: partial reads/writes, split and glued frames, EINTR | YES | YES | `test_shortio`, `test_frame` (`test_eintr_is_retried`), ``test_server.py`` TCP tests |
| Frame validation: lengths, flags, header count, truncation, no unchecked allocation | YES | YES | `test_frame`, `test_protocol`, ``test_server.py`` protocol tests |
| Annotated hexdump of one real request and response | YES | YES | `examples/*.hexdump`; `python3 tests/test_examples.py` |
| Spec ("two pages, enough for a stranger") | YES | YES | `SPEC.md`; ``tests/bhtp.py`` is a second implementation written from it alone |
| Memory safety | YES | YES | `make asan` (all tests under AddressSanitizer + UBSan) |
| Client side (`-v` hexdump, exit codes, never a second connection) | in the partner repo | YES (partner) | `veekshitha-binary-http`; `tools/pyfetch.py` here is a second client written from the spec |
| Pair works together | YES | YES | `BCURL=../veekshitha-binary-http/bcurl scripts/verify.sh` (all 27 checks pass) |

## 23. Demo for the professor (3-5 minutes)

1. **Server:** `make && ./bserve ./www 9000` (leave it running; the log scrolls in this terminal).
2. **Spec:** open `SPEC.md` - show the 12-byte header table and the "unknown frames" section.
3. **Request with `-v`:** `../veekshitha-binary-http/bcurl -v localhost:9000/hello.txt` (or `python3 tools/pyfetch.py -v ...`).
   Point at `TX FRAME #1 REQUEST`, the `42 48 54 50` magic, the length `00 00 00 49`, then `RX FRAME #1 RESPONSE` and `RX FRAME #2 DATA` with `flags=0x0001`.
4. **Annotated bytes:** `less examples/request.hexdump` - every byte explained.
5. **404 and exit status:** `$CLIENT localhost:9000/nope.html; echo $?` -> `404`, `1`.
6. **One connection:** `$CLIENT localhost:9000/index.html /hello.txt /missing.txt`, then show the server terminal: one `accepted`, three requests.
7. **Unknown frame:** run snippet A: `200 200`, and the log line `skipped unknown frame type 0x7f (6 payload bytes)`.
8. **Malformed, then recover:** snippet B: `400` then `200` on the same connection.
9. **Traversal:** `$CLIENT localhost:9000/../../etc/passwd; echo $?` -> `400`, `1`.
10. **Tests:** `make test` -> `==== ALL TESTS PASSED ====`.
