# VIVA.md - questions you should be able to answer

Short answers, with the place to point at. "Spec" means `SPEC.md`.

## The frame header

**Why a fixed-size header?** Because then `read_exact(12)` always works: the receiver never has to guess how many bytes
to read next. Everything variable lives in the payload, whose size is in the header.

**Why 12 bytes, and why these fields?** `magic`(4) `version`(1) `type`(1) `flags`(2) `payload_length`(4). The first three
make the stream self-checking and extensible; the last two describe what follows. Nothing else is needed for a sequential protocol.

**HTTP/2 chose 24 / 8 / 8 / 31. Why is yours different?**
HTTP/2's 24 bits is the length (16 MiB), the two 8s are type and flags, the 31 is the **stream identifier**, which exists because
HTTP/2 interleaves many requests on one connection. We deliberately do not multiplex - a response always belongs to the oldest
request - so a stream ID would be 31 bits of dead weight. In its place we spend bytes on a magic and a version, which HTTP/2 does not need
because its connection preface does that job.

**Why is `payload_length` 32 bits and not 16 or 24?** 16 bits would cap a frame at 64 KiB and be hard to widen; 24 (HTTP/2) is fine
but unusual to read in a hexdump. 32 bits is a natural word, `00 00 00 4a` reads as 74, and the field will never have to change.
What actually limits memory is not the field width but the **policy** of 16 384 bytes, which is a constant a later version may raise.

**Why 16 384 as the maximum?** It is HTTP/2's default frame size, it keeps every buffer (`payload[16384]`) on the stack, and a large file
simply becomes several DATA frames. The limit applies *before* reading: a larger known frame is refused without reading or allocating it.

**Why is `flags` 16 bits when only one bit is used?** One extra byte buys 15 spare bits for later; changing a header's layout later costs far more.

**Why big-endian?** It is network byte order, the Internet convention; it makes hexdumps readable (`00 00 00 4a` is 74) and the code
(`buf[0] << 24 | ...`) gives the same result on any CPU.

**Why a magic number?** If a browser, a port scanner or a person with `telnet` talks to the port, the first 4 bytes are not `BHTP`, so we
reject it at once with an ERROR instead of misreading text as a length. Test: `test_09b_http_text_sent_to_the_port_is_rejected`.

**Why a version byte?** To change the format incompatibly later. A receiver that sees a version it does not know cannot interpret the rest,
so it refuses (fatal ERROR). Compatible additions need no new version - that is what skipping unknown frames is for.

## Framing and TCP

**How does the receiver know where a frame ends?** Only from `payload_length`: frame end = start + 12 + length. There are no delimiters and
no scanning for bytes. `frame_read()` in `src/frame.c` is exactly: read 12, check, read `length`.

**TCP delivers a stream. What if a frame is split, or two frames arrive together?** `io_read_exact()` loops until it has exactly the
number of bytes asked for (a `read()` may return less), retries on `EINTR`, and distinguishes "EOF before the first byte" (clean close)
from "EOF in the middle" (truncated frame). Tests: `test_shortio.c` feeds frames one byte at a time; the integration tests split a request at
**every** offset, send 50 requests in one write, and dribble frames in slowly. `io_write_all()` does the same for partial writes.

**How does the client know the response is complete?** `END_STREAM` on the RESPONSE (no body) or on the last DATA frame. If `content-length`
is present the client also checks that the DATA bytes add up to it. The connection is not closed to mark the end, which is what makes
persistence possible.

## Frame types, requests, responses

**Why four frame types?** REQUEST, RESPONSE, DATA, ERROR is the smallest set that covers a file transfer: ask, describe, deliver, and report a
broken stream. DATA is separate from RESPONSE so that headers (small, fully buffered) and the body (any size, streamed) have different
size rules, and so a big file can span many frames.

**Why is the status in RESPONSE and not in the header?** The status belongs to the application, not to framing; the frame header stays the same for
every frame type.

**Why a separate ERROR frame? Isn't a 400 RESPONSE enough?** Two different situations. If the *payload* is bad but the frame boundaries
were honoured, the stream is still in sync, so a normal `RESPONSE 400` and the connection stays open. If the *framing* is bad (wrong magic, absurd
length) the receiver cannot tell where the next frame starts, so nothing else on that connection can be trusted: the server says `ERROR 400` and closes.

**Where do 200 / 400 / 404 / 500 come from?** Same numbers as HTTP so they are familiar. 400: malformed request or bad path. 404: no such file (also
used for a symlink escape, so existence outside the root is not revealed). 500: the file exists but the server cannot read it.

## Headers

**Why are headers encoded this way?** HPACK's first two ideas, in an evening (the assignment's words): a **static table** numbers the ten names we
really send, so `content-type` costs 1 byte instead of 13; every other name is a **length-prefixed literal**. No dynamic table, no Huffman, so a
decoder is small and cannot be confused by state.

**Which ten names, and why?** `host user-agent accept accept-encoding connection` (sent by the client) and `content-type content-length server
last-modified cache-control connection` (sent by the server). All ten are actually sent in a normal exchange; `connection` is shared.

**What if a header name is not in the table?** It is sent as tag `0`, a one-byte length, and the name. Example: `-H 'x-id: 7'` -> `00 04 78 2d 69 64 00 01 37`.

**Why `value_len` u16 and `count` u8?** 4096 and 32 are generous limits for this protocol and both fit their types with room to spare; the receiver checks
each against the bytes actually remaining, so a lying length can never read past the payload.

## Unknown frames

**What happens if a client receives a frame type it does not know?** It reads the 12-byte header, takes `payload_length`, discards exactly that many bytes,
and carries on with the next frame - no error, no crash, no reply. Same on the server. (`frame_read()` returns `FR_SKIPPED`.)

**Why does that matter?** It is how version 2 can add frame types (say `PING`) and still talk to version-1 programs: they will skip what they do not understand.
Without the rule, any new frame would break every old peer.

**Even a 100 MB unknown frame?** The bytes are streamed through a 4 KiB buffer and thrown away; nothing is allocated for the claimed length (tests:
`test_14c_huge_unknown_frame_is_skipped_not_buffered`, `test_huge_unknown_frame_is_skipped`). A frame that never completes ends with the connection's timeout.

**Where is it tested?** Known / unknown / known in one write: `test_14_known_unknown_known_in_one_write` (server) and
`TestUnknownFrames.test_known_unknown_known` (client); also unknown frames before the response, after the last DATA, between frames, dribbled in a byte at a time.

## Persistent connections

**How does the persistent connection work?** After a response the server simply loops back to `frame_read()` on the same socket. Nothing in the protocol
ends a connection except EOF, `connection: close`, an ERROR frame, or an idle timeout.

**How do you *prove* the client uses only one connection?** The server numbers connections and logs `[conn=N] accepted`, `request #k`, `response #k`, `closed`;
three requests produce one `accepted`. The client prints `TCP connection #1, the only one` and its `client_connect()` refuses a second call. Automated:
`test_three_exchanges_use_exactly_one_connection` (server) and `TestOneConnection` (client, counts `accept()` calls on a mock).

**What if a request fails midway?** The client stops and exits 2 without reconnecting (`test_failure_in_the_middle_stops_without_reconnecting`).

**Why not multiplex?** The assignment prefers simple sequential request/response. It removes stream IDs, flow control and ordering questions.

## Security and robustness

**How is directory traversal prevented?** Three layers (spec section 11): reject `..` segments and control characters up front; join under the root and
`realpath()` it; then require that the canonical path still lies inside the canonical root (whole-component comparison, so `/srv/www2` is not "inside" `/srv/www`).
Percent-encoding is not decoded, so `%2e%2e` is just a file name. A symlink pointing out of the root is 404.

**Why 400 for `..` but 404 for `/etc/passwd`?** `..` is never legitimate, so it is a bad request. `/etc/passwd` is a valid path that means `<root>/etc/passwd`, which does not exist.

**What stops a huge `payload_length` from exhausting memory?** The length is checked against 16 384 before any read or allocation; unknown frames are streamed; every
inner length (path, header count, name, value) is checked against the bytes that remain in the payload.

**How do you know it does not crash on garbage?** Fuzz test: 400 random frames (random types, lengths, truncations, random bytes) against a live server with a fixed seed,
the server must still answer afterwards; `make asan` runs everything under AddressSanitizer and UBSan.

## If the examiner asks about the process

**What did the two of you share?** Only `SPEC.md`. Each repository carries its own copy of the small protocol layer (`protocol.c`, `frame.c`, `io.c`, identical files),
and **each side is also tested against a program written from the spec alone**: the server against `tests/bhtp.py`/`tools/pyfetch.py`, the client against
`tests/bhtp.py`-based mock servers and `tools/refserver.py`. That is how we know it is a protocol and not one implementation.

**Is the hexdump real?** Yes: `scripts/gen_hexdumps.sh` runs the real programs and annotates the captured bytes; `tests/test_examples.py` replays the request against a live
server and fails if the bytes or the annotations differ.

## Server-specific

**Why `fork()` per connection?** A persistent connection occupies the server for as long as the client keeps it open, so a single-threaded loop would block everyone else.
One process per connection is the simplest correct answer: isolation for free, no locks. (`test_connections_are_served_concurrently`.) Its cost is scalability, noted in the README.

**What does the server log, and why?** One line per event with a connection number: `[conn=N] accepted / request #k GET path / response #k status / closed (reason)`. It is how a
second connection would be caught, and how you watch the protocol work.

**What if the client sends a request and immediately closes?** The server still answers (half-close is allowed) and then sees EOF
(`test_client_half_close_still_gets_its_response`); if the client vanishes mid-download the write fails with `EPIPE`, the child exits, the server lives (`SIGPIPE` is ignored).

**What if the file changes while it is being sent?** `content-length` was promised from `fstat`; if the file shrinks the server cannot keep its promise, so it abandons that connection
rather than send a short body.

**What happens on an unreadable file?** `500` (`test_07`), connection stays open. (As an unprivileged user; as root `chmod 000` does not block reads.)
