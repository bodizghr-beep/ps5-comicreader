#!/bin/sh
# Dovlaci header-only zavisnosti u src/.
# Obe su public domain / MIT i namerno se ne cuvaju u repou.

set -e

SRC="$(dirname "$0")/../src"
cd "$SRC"

fetch() {
    url="$1"
    out="$2"
    if [ -f "$out" ]; then
        echo "  $out vec postoji, preskacem"
        return
    fi
    echo "  dovlacim $out"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$url" -o "$out"
    else
        wget -qO "$out" "$url"
    fi
}

echo "Zavisnosti:"
fetch "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h" \
      "stb_image.h"
fetch "https://raw.githubusercontent.com/dhepper/font8x8/master/font8x8_basic.h" \
      "font8x8_basic.h"

echo "Gotovo."
echo
echo "Preostale zavisnosti moraju biti izgradjene za PS5 toolchain:"
echo "  libarchive  (obavezno - CBZ/CBR/CB7/CBT)"
echo "  SDL2        (obavezno - prikaz i kontroler)"
echo "  mupdf       (opciono  - samo za WITH_PDF=1)"
