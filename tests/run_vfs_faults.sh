#!/bin/sh
# Isto kao run_vfs.sh, ali server obara svaki treci zahtjev.
set -e

BIN="${1:?zadaj putanju do izgradjenog test binarija}"
PORT="${PORT:-8120}"
TMP=$(mktemp -d)
PID=
trap 'rm -rf "$TMP"; [ -n "$PID" ] && kill "$PID" 2>/dev/null || true' EXIT

head -c 2097152 /dev/urandom > "$TMP/data.bin"

SRV_ROOT="$TMP" SRV_PORT="$PORT" SRV_FAIL_EVERY=3 python3 tests/http_server.py &
PID=$!
sleep 1

SRV_FILE_URL="http://127.0.0.1:$PORT/data.bin" "$BIN"
