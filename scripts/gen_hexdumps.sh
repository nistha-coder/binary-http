#!/usr/bin/env bash
# Regenerate examples/request.hexdump and examples/response.hexdump from REAL
# traffic: start ./bserve, capture the raw bytes of one exchange with a bare
# socket client (tools/capture_frames.py), then verify and annotate them with
# tools/make_examples.py (which refuses to write anything if a check fails).
#
#   scripts/gen_hexdumps.sh [output-directory]       (default: examples)
set -euo pipefail
cd "$(dirname "$0")/.."

OUT="${1:-examples}"
PORT="${PORT:-9000}"
make -s bserve

TMP="$(mktemp -d)"
SRV=""
cleanup() { [ -n "$SRV" ] && kill "$SRV" 2>/dev/null || true; rm -rf "$TMP"; }
trap cleanup EXIT

mkdir "$TMP/www"
cp www/index.html "$TMP/www/index.html"
touch -d '2026-01-01 00:00:00 UTC' "$TMP/www/index.html"   # fixed last-modified => reproducible bytes

./bserve "$TMP/www" "$PORT" 2>"$TMP/server.log" &
SRV=$!
for _ in $(seq 1 50); do
    grep -q "serving" "$TMP/server.log" 2>/dev/null && break
    sleep 0.1
done
grep -q "serving" "$TMP/server.log" || { echo "server did not start:"; cat "$TMP/server.log"; exit 1; }

python3 tools/capture_frames.py localhost "$PORT" /index.html "$TMP"

python3 tools/make_examples.py \
    --from-raw "$TMP/request.bin" "$TMP/response.bin" \
    --served-file "$TMP/www/index.html" \
    --path /index.html \
    --command "python3 tools/capture_frames.py localhost $PORT /index.html   (same request as: bcurl -vv localhost:$PORT/index.html)" \
    --out "$OUT"
