#!/bin/sh
# Pusta test_http_list u oba rezima: WebDAV i autoindex fallback.
set -e

BIN="${1:?zadaj putanju do izgradjenog test binarija}"
TMP=$(mktemp -d)
PID=
trap 'rm -rf "$TMP"; [ -n "$PID" ] && kill "$PID" 2>/dev/null || true' EXIT

mkdir -p "$TMP/Serija"
printf 'x' > "$TMP/a.cbz"
printf 'x' > "$TMP/readme.txt"
printf 'x' > "$TMP/Serija/b.cbz"

PORT=8099

for mode in webdav autoindex; do
    if [ "$mode" = autoindex ]; then
        SRV_NO_DAV=1; export SRV_NO_DAV
    else
        unset SRV_NO_DAV
    fi

    SRV_ROOT="$TMP" SRV_PORT="$PORT" python3 tests/http_server.py &
    PID=$!
    sleep 1

    SRV_URL="http://127.0.0.1:$PORT/" "$BIN"

    kill "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
    PID=
    PORT=$((PORT + 1))
done

echo "oba rezima prosla"
