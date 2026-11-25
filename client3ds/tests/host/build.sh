#!/usr/bin/env bash
# Compile sur PC le client sans ecran (tests/host/headless.cpp) avec le VRAI code du client :
# tout source/*.cpp sauf main.cpp, la couche plate-forme etant sa branche PC (sans libctru).
#
#   tests/host/build.sh [dossier de sortie]      -> <sortie>/headless
#
# Il faut libjpeg-turbo pour PC (Debian/Ubuntu : libjpeg-turbo8-dev ou libjpeg-dev). Sinon, passez
# JPEG_CFLAGS et JPEG_LIBS (ex. JPEG_CFLAGS="-I/chemin/src" JPEG_LIBS="/chemin/libjpeg.a").
set -eu
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$HERE/tests/host/out}"
mkdir -p "$OUT"
CFLAGS_J="${JPEG_CFLAGS:-$(pkg-config --cflags libjpeg 2>/dev/null || true)}"
LIBS_J="${JPEG_LIBS:-$(pkg-config --libs libjpeg 2>/dev/null || echo -ljpeg)}"
SRCS=$(ls "$HERE"/source/*.cpp | grep -v '/main\.cpp$')
# shellcheck disable=SC2086
g++ -std=gnu++17 -O1 -g -Wall -Wextra -fno-rtti -pthread \
    -I "$HERE/include" -I "$HERE/source" $CFLAGS_J \
    "$HERE/tests/host/headless.cpp" $SRCS -o "$OUT/headless" $LIBS_J
echo "$OUT/headless"
