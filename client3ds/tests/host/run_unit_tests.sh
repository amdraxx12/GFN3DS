#!/usr/bin/env bash
# Compile et lance les tests unitaires du client sur PC (le VRAI code, sans libctru).
#
#   tests/host/run_unit_tests.sh [dossier de sortie]
#
# Il faut g++ et libjpeg-turbo pour PC (Debian/Ubuntu : libjpeg-turbo8-dev ou libjpeg-dev ; Windows : MSYS2
# ou WSL). Sinon, passez JPEG_CFLAGS et JPEG_LIBS (ex. JPEG_CFLAGS="-I/chemin/src" JPEG_LIBS="/chemin/libjpeg.a").
# Si le compilateur sait faire les controles AddressSanitizer / UBSan, ils sont actives automatiquement.
set -eu
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$HERE/tests/host/out}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
CFLAGS_J="${JPEG_CFLAGS:-$(pkg-config --cflags libjpeg 2>/dev/null || true)}"
LIBS_J="${JPEG_LIBS:-$(pkg-config --libs libjpeg 2>/dev/null || echo -ljpeg)}"
SRCS=$(ls "$HERE"/source/*.cpp | grep -v '/main\.cpp$')

build() {   # $1 : options supplementaires
    # shellcheck disable=SC2086
    g++ -std=gnu++17 -O1 -g -Wall -Wextra -fno-rtti -pthread $1 \
        -I "$HERE/include" -I "$HERE/source" $CFLAGS_J \
        "$HERE/tests/host/unit_tests.cpp" $SRCS -o "$OUT/unit_tests" $LIBS_J
}

if build "-fsanitize=address,undefined -fno-sanitize-recover=undefined" 2>/dev/null; then
    echo "(controles AddressSanitizer + UBSan actifs)"
else
    build ""
fi
cd "$OUT"      # les tests de reglages ecrivent de petits fichiers temporaires dans le dossier courant
./unit_tests
