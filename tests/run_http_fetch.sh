#!/bin/sh
set -e

BIN="${1:?zadaj putanju do izgradjenog test binarija}"
PORT="${PORT:-8140}"
TMP=$(mktemp -d)
PID=
trap 'rm -rf "$TMP"; [ -n "$PID" ] && kill "$PID" 2>/dev/null || true' EXIT

mkdir -p "$TMP/srv" "$TMP/cache"
head -c 200000 /dev/urandom > "$TMP/srv/strip.cbz"
head -c 150000 /dev/urandom > "$TMP/srv/knjiga.pdf"

SRV_ROOT="$TMP/srv" SRV_PORT="$PORT" python3 tests/http_server.py &
PID=$!
sleep 1

SRV_URL="http://127.0.0.1:$PORT/" CR_TMP="$TMP/cache" "$BIN"
