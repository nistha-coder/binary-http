#!/usr/bin/env bash
# verify.sh - the verification procedure from the README, as one script.
#
# It starts a server on a free port, drives it with a client, and prints
# PASS/FAIL for each step. Programs used (first match wins):
#
#   server: $BSERVE, ./bserve, tools/refserver.py
#   client: $BCURL,  ./bcurl,  tools/pyfetch.py
#
# so it works with just this repository, and also tests a PAIR: build both
# projects and point BSERVE / BCURL at the binaries.
#
#   scripts/verify.sh                     # this repo's program + a reference peer
#   BCURL=/path/to/bcurl scripts/verify.sh
#   SKIP_TESTS=1 scripts/verify.sh        # skip step 12 (the full test suite)
set -u
cd "$(dirname "$0")/.."

PASS=0; FAIL=0
ok()  { printf 'PASS  %s\n' "$1"; PASS=$((PASS + 1)); }
bad() { printf 'FAIL  %s\n' "$1"; FAIL=$((FAIL + 1)); [ -n "${2:-}" ] && printf '        %s\n' "$2"; }
step() { printf '\n== STEP %s ==\n' "$1"; }

# ---- STEP 1: build ---------------------------------------------------------
step "1  build"
if make -s all 2>"${TMPDIR:-/tmp}/verify-build.err"; then ok "make: build succeeded, no compiler errors"
else bad "make failed"; cat "${TMPDIR:-/tmp}/verify-build.err"; exit 1; fi

if   [ -n "${BSERVE:-}" ]; then SERVER=("$BSERVE")
elif [ -x ./bserve ];      then SERVER=(./bserve)
else                            SERVER=(python3 tools/refserver.py); fi
if   [ -n "${BCURL:-}" ];  then CLIENT=("$BCURL")
elif [ -x ./bcurl ];       then CLIENT=(./bcurl)
else                            CLIENT=(python3 tools/pyfetch.py); fi
echo "server: ${SERVER[*]}"
echo "client: ${CLIENT[*]}"

# ---- STEP 2: test files ----------------------------------------------------
step "2  prepare test files"
TMP="$(mktemp -d)"
SRV=""
cleanup() { [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
WWW="$TMP/www"; mkdir -p "$WWW/sub"
SRC_INDEX=www/index.html; [ -f "$SRC_INDEX" ] || SRC_INDEX=fixtures/www/index.html
cp "$SRC_INDEX" "$WWW/index.html"
printf 'Hello from the verification script.\n' > "$WWW/hello.txt"
: > "$WWW/empty.txt"
printf 'in a subdirectory\n' > "$WWW/sub/page.txt"
head -c 2097152 /dev/urandom > "$WWW/random.bin"          # 2 MiB of random bytes
ok "created index.html, hello.txt, empty.txt, sub/page.txt, random.bin (2 MiB) in $WWW"

# ---- STEP 3: start the server ---------------------------------------------
step "3  start the server"
PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')"
LOG="$TMP/server.log"
"${SERVER[@]}" "$WWW" "$PORT" 2>"$LOG" &
SRV=$!
for _ in $(seq 1 50); do grep -q serving "$LOG" 2>/dev/null && break; sleep 0.1; done
if grep -q serving "$LOG"; then ok "server is listening on port $PORT"; else bad "server did not start" "$(cat "$LOG")"; exit 1; fi
URL="localhost:$PORT"
fetch() { timeout 30 "${CLIENT[@]}" "$@"; }

# ---- STEP 4: basic request -------------------------------------------------
step "4  basic request"
fetch "$URL/index.html" > "$TMP/o" 2>"$TMP/e"; rc=$?
if [ $rc -eq 0 ] && cmp -s "$TMP/o" "$WWW/index.html"; then ok "GET /index.html: exit 0, output identical to the file"
else bad "GET /index.html (exit $rc)" "$(cat "$TMP/e")"; fi

# ---- STEP 5: verbose -------------------------------------------------------
step "5  verbose request (hexdump of every frame)"
fetch -v "$URL/hello.txt" > "$TMP/o" 2>"$TMP/e"; rc=$?
if grep -q '^TX FRAME #1' "$TMP/e" && grep -q '^RX FRAME #1' "$TMP/e" && grep -q '^RX FRAME #2' "$TMP/e"; then
    ok "-v printed TX FRAME #1, RX FRAME #1 (RESPONSE) and RX FRAME #2 (DATA)"
else bad "-v output lacks TX/RX frames"; fi
cmp -s "$TMP/o" "$WWW/hello.txt" && ok "stdout still carries only the body (dump goes to stderr)" || bad "stdout polluted by -v"
echo "--- first lines of the dump:"; head -4 "$TMP/e" | cut -c1-100

# ---- STEP 6: 404 -----------------------------------------------------------
step "6  404"
fetch "$URL/no-such-file.html" > "$TMP/o" 2>"$TMP/e"; rc=$?
if [ $rc -ne 0 ]; then ok "missing file: client exit status $rc (non-zero)"; else bad "404 gave exit status 0"; fi
grep -q "404" "$TMP/e" && ok "client reported 404: $(head -1 "$TMP/e")" || bad "404 not reported" "$(cat "$TMP/e")"

# ---- STEP 7: persistent connection ----------------------------------------
step "7  one TCP connection for several requests"
before=$(grep -c accepted "$LOG")
fetch "$URL/index.html" /hello.txt /sub/page.txt > "$TMP/o" 2>"$TMP/e"; rc=$?
after=$(grep -c accepted "$LOG")
cat "$WWW/index.html" "$WWW/hello.txt" "$WWW/sub/page.txt" > "$TMP/want"
[ $rc -eq 0 ] && cmp -s "$TMP/o" "$TMP/want" && ok "three bodies received, in order" || bad "three-request fetch failed (exit $rc)"
if [ $((after - before)) -eq 1 ]; then ok "server log: exactly ONE new connection for the 3 requests"; else bad "server accepted $((after - before)) connections for 3 requests"; fi
echo "--- server log for that connection:"; tail -n 8 "$LOG" | grep -E "conn=$after\]|accepted" | tail -8

# ---- STEP 8 and 9: unknown frame, malformed frame ---------------------------
step "8  unknown frame is skipped / 9  malformed frame gives 400"
python3 - "$PORT" "$WWW" <<'PYEOF'
import os, sys
sys.path.insert(0, "tests")
import bhtp
port, www = int(sys.argv[1]), sys.argv[2]
want_hello = open(os.path.join(www, "hello.txt"), "rb").read()
want_index = open(os.path.join(www, "index.html"), "rb").read()
bad = 0
def report(ok, text):
    global bad
    print(("PASS  " if ok else "FAIL  ") + text)
    bad += 0 if ok else 1

# 8: known / unknown / known, all in one write
c = bhtp.Conn("127.0.0.1", port)
c.send(bhtp.request_frame("/hello.txt") + bhtp.frame(0x7F, b"a frame type from the future") +
       bhtp.request_frame("/index.html"))
r1, r2 = c.read_response(), c.read_response()
report(r1.status == 200 and r1.body == want_hello and r2.status == 200 and r2.body == want_index,
       "known, UNKNOWN (type 0x7f), known: both requests answered, the unknown frame was skipped")
c.close()

# 9: malformed REQUEST payloads -> 400, connection stays usable
c = bhtp.Conn("127.0.0.1", port)
for name, payload in [("garbage payload", b"\xff\xff\xff"), ("empty payload", b""),
                      ("path longer than the payload", b"\x01\x00\x50/a"),
                      ("header tag 11 (reserved)", b"\x01\x00\x01/\x01\x0b\x00\x00")]:
    c.send(bhtp.frame(bhtp.REQUEST, payload))
    r = c.read_response()
    report(r.status == 400, "malformed request (%s) -> 400" % name)
c.send(bhtp.request_frame("/hello.txt"))
r = c.read_response()
report(r.status == 200 and r.body == want_hello, "...and the SAME connection still serves a valid request afterwards")
c.close()
sys.exit(1 if bad else 0)
PYEOF
rc=$?; if [ $rc -eq 0 ]; then PASS=$((PASS + 6)); else FAIL=$((FAIL + 1)); fi
kill -0 "$SRV" 2>/dev/null && ok "server process is still running" || bad "server died"

# ---- STEP 10: path traversal -----------------------------------------------
step "10  path traversal"
echo "secret outside the root" > "$TMP/outside.txt"
for p in "/../../etc/passwd" "/../outside.txt" "/sub/../../outside.txt" "/%2e%2e/%2e%2e/etc/passwd" "//etc/passwd" "/..%2f..%2fetc/passwd" "/sub/..\\..\\outside.txt"; do
    fetch "$URL$p" > "$TMP/o" 2>"$TMP/e"; rc=$?
    if [ $rc -ne 0 ] && ! grep -q "root:" "$TMP/o" && ! grep -q "secret outside" "$TMP/o"; then
        ok "GET $p: refused (exit $rc), nothing leaked"
    else bad "GET $p: exit $rc, output: $(head -c 80 "$TMP/o")"; fi
done

# ---- STEP 11: binary file --------------------------------------------------
step "11  large binary file, byte for byte"
fetch "$URL/random.bin" > "$TMP/o" 2>"$TMP/e"; rc=$?
if [ $rc -eq 0 ] && cmp -s "$TMP/o" "$WWW/random.bin"; then
    ok "2 MiB random file: cmp says identical"
    echo "        sha256 original: $(sha256sum < "$WWW/random.bin" | cut -d' ' -f1)"
    echo "        sha256 received: $(sha256sum < "$TMP/o" | cut -d' ' -f1)"
else bad "binary file differs (exit $rc)"; fi
fetch "$URL/empty.txt" > "$TMP/o" 2>/dev/null; rc=$?
[ $rc -eq 0 ] && [ ! -s "$TMP/o" ] && ok "empty file: exit 0, no output" || bad "empty file"

kill -0 "$SRV" 2>/dev/null && ok "server survived the whole run" || bad "server died during the run"

# ---- STEP 12: the test suite ----------------------------------------------
step "12  complete test suite"
if [ "${SKIP_TESTS:-0}" = "1" ]; then
    echo "skipped (SKIP_TESTS=1)"
elif make test > "$TMP/make-test.log" 2>&1; then
    ok "make test: $(tail -1 "$TMP/make-test.log")"
else bad "make test failed"; tail -30 "$TMP/make-test.log"; fi

printf '\n==== %d passed, %d failed ====\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
