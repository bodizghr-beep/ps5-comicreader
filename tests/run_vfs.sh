#!/bin/sh
# Dize http_server.py nad privremenim fajlom i pusta zadati test binarij.
set -e

BIN="${1:?zadaj putanju do izgradjenog test binarija}"
PORT="${PORT:-8110}"
TMP=$(mktemp -d)
PID=
trap 'rm -rf "$TMP"; [ -n "$PID" ] && kill "$PID" 2>/dev/null || true' EXIT

# 2 MB pseudoslucajnog sadrzaja - nestisljiv, pa Range greske odmah pucaju
head -c 2097152 /dev/urandom > "$TMP/data.bin"

SRV_ROOT="$TMP" SRV_PORT="$PORT" python3 tests/http_server.py &
PID=$!
sleep 1

SRV_FILE_URL="http://127.0.0.1:$PORT/data.bin" \
SRV_FILE_LOCAL="$TMP/data.bin" \
"$BIN"
