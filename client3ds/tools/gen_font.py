#!/usr/bin/env python3
"""Genere source/font_data.inc a partir de la police de la console de libctru (data/default_font.bin).

Format : 256 glyphes de 8 octets (une ligne par octet, de haut en bas ; le bit de poids fort est le
pixel le plus a gauche), disposition CP437. La police vient de libctru (licence zlib).

Usage : python3 tools/gen_font.py /chemin/vers/libctru/data/default_font.bin source/font_data.inc
"""
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    data = open(sys.argv[1], "rb").read()
    if len(data) != 2048:
        print("taille inattendue : %d (2048 attendus)" % len(data))
        return 1
    out = ["// Genere par tools/gen_font.py a partir de libctru/data/default_font.bin (licence zlib).",
           "// 256 glyphes CP437 de 8x8 pixels : un octet par ligne, bit 7 = pixel de gauche.",
           "{"]
    for c in range(256):
        row = ", ".join("0x%02X" % b for b in data[c * 8:(c + 1) * 8])
        out.append("    {%s},  // 0x%02X" % (row, c))
    out.append("}")
    open(sys.argv[2], "w", encoding="ascii").write("\n".join(out) + "\n")
    print("ecrit", sys.argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main())
