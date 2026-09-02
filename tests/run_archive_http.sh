#!/bin/sh
# Pravi CBZ, servira ga, i pusta test koji poredi lokalno i mrezno citanje.
set -e

BIN="${1:?zadaj putanju do izgradjenog test binarija}"
PORT="${PORT:-8130}"
TMP=$(mktemp -d)
PID=
trap 'rm -rf "$TMP"; [ -n "$PID" ] && kill "$PID" 2>/dev/null || true' EXIT

mkdir -p "$TMP/pages"
python3 - "$TMP/pages" <<'PY'
import struct, sys, zlib, os
out = sys.argv[1]

def png(path, w, h, val):
    raw = b"".join(b"\x00" + bytes([val, val, val] * w) for _ in range(h))
    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c))
    data = (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw))
            + chunk(b"IEND", b""))
    open(path, "wb").write(data)

for i in range(1, 13):
    png(os.path.join(out, "page%02d.png" % i), 64, 64, i * 10)
PY

( cd "$TMP" && zip -0 -q -j strip.cbz pages/*.png )

SRV_ROOT="$TMP" SRV_PORT="$PORT" python3 tests/http_server.py &
PID=$!
sleep 1

SRV_CBZ_URL="http://127.0.0.1:$PORT/strip.cbz" \
SRV_CBZ_LOCAL="$TMP/strip.cbz" \
"$BIN"
