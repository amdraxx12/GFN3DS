#!/usr/bin/env bash
# Verifie la SYNTAXE de tout le client contre les vrais en-tetes de libctru, SANS devkitARM.
#
# Ce n'est PAS une compilation ARM : pas d'edition de liens, pas d'optimisation, rien n'est execute.
# Cela attrape les fonctions libctru inexistantes ou mal appelees (c'est son seul but) ; la vraie
# compilation se fait avec "make" sur une machine ou devkitARM est installe.
#
# Usage : tools/check_3ds_syntax.sh [dossier include de libctru]
#   (defaut : $CTRULIB_INC, puis $DEVKITPRO/libctru/include, puis ~/devkitpro/libctru/libctru/include)
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
CTRU="${1:-${CTRULIB_INC:-}}"
if [ -z "$CTRU" ]; then
    for c in "${DEVKITPRO:-/opt/devkitpro}/libctru/include" "$HOME/devkitpro/libctru/libctru/include"; do
        [ -d "$c/3ds" ] && CTRU="$c" && break
    done
fi
if [ -z "$CTRU" ] || [ ! -d "$CTRU/3ds" ]; then
    echo "en-tetes de libctru introuvables : passez le dossier include en argument" >&2
    exit 2
fi
# Facultatif : dossier(s) de jpeglib.h / jconfig.h (3ds-libjpeg-turbo), separes par ':'
JPEG_FLAGS=()
IFS=':' read -ra _jdirs <<< "${JPEG_INC:-}"
for d in ${_jdirs[@]+"${_jdirs[@]}"}; do
    [ -n "$d" ] && JPEG_FLAGS+=(-I "$d")
done

fail=0
for f in "$HERE"/source/*.cpp; do
    printf '%-28s' "$(basename "$f")"
    # -fpermissive : certains en-tetes de libctru convertissent des pointeurs en u32, valable seulement en 32 bits
    # (ces messages viennent des en-tetes systeme et restent masques ; ceux de notre code, eux, s'affichent).
    if g++ -std=gnu++17 -fsyntax-only -fpermissive -fno-exceptions -fno-rtti -Wall -Wextra -D__3DS__ -D_GNU_SOURCE \
        -I "$HERE/include" -I "$HERE/source" -I "$HERE/tools/hoststubs" ${JPEG_FLAGS[@]+"${JPEG_FLAGS[@]}"} \
        -isystem "$CTRU" "$f" 2>"${TMPDIR:-/tmp}/gfn3ds_syntax.err"; then
        echo "ok"
    else
        echo "ERREUR"
        head -30 "${TMPDIR:-/tmp}/gfn3ds_syntax.err"
        fail=1
    fi
done
exit $fail
