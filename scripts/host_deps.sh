#!/bin/sh
# Dovlaci -dev zaglavlja za host build i testove, bez root privilegija.
# Runtime biblioteke (libarchive.so.13 i sl.) vec postoje na sistemu; ovdje
# nedostaju samo zaglavlja i .so simlinkovi na koje linker cilja.
#
# Isti obrazac koji SETUP.md propisuje za LLVM: apt-get download + dpkg-deb -x.
set -e

HOSTROOT="${HOSTROOT:-$HOME/.cache/ps5cr-hostdeps}"
# SDL2 ide i kao -dev i kao runtime: na ovom hostu nema nijednog od njih,
# a host build bez SDL2 ne postoji.
PKGS="libarchive-dev libxml2-dev libcurl4-openssl-dev libsdl2-dev libsdl2-2.0-0 libsdl2-classic"

mkdir -p "$HOSTROOT/debs" "$HOSTROOT/root/usr/lib"
cd "$HOSTROOT/debs"

for p in $PKGS; do
    if ! ls "${p}"_*.deb >/dev/null 2>&1; then
        echo "dovlacim $p"
        apt-get download "$p"
    fi
done

for d in *.deb; do
    dpkg-deb -x "$d" "$HOSTROOT/root"
done

# Dev paket donosi zaglavlja, ali .so simlink u njemu pokazuje na fajl koji
# u ovom rootu ne postoji. Zato se simlink pravi rucno, na sistemsku biblioteku.
for n in archive xml2 curl; do
    real=$(ldconfig -p | awk -v pat="lib$n.so." '$1 ~ ("^" pat) { print $NF; exit }')
    if [ -z "$real" ]; then
        echo "nema runtime biblioteke lib$n.so na sistemu" >&2
        exit 1
    fi
    ln -sf "$real" "$HOSTROOT/root/usr/lib/lib$n.so"
    echo "  lib$n.so -> $real"
done

echo
echo "Gotovo. Makefile ovo nalazi sam preko HOSTROOT=$HOSTROOT"
