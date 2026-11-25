#!/usr/bin/env python3
"""Genere la banniere 3D du CIA (256x128, affichee en relief dans le menu HOME) et un court jingle.

    python3 tools/make_banner.py          # ecrit assets/banner.png et assets/audio.wav

Dependances : Pillow et numpy (uniquement pour regenerer ; les fichiers generes sont deja dans assets/).
Aucun logo NVIDIA ou Nintendo n'est reproduit.
"""
import math
import os
import struct
import sys
import wave

from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 256, 128
S = 4                                   # sur-echantillonnage pour des bords lisses
GREEN = (118, 185, 0)
FONTS = ["DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
         "C:/Windows/Fonts/arialbd.ttf", "C:/Windows/Fonts/segoeuib.ttf",
         "/System/Library/Fonts/Supplemental/Arial Bold.ttf"]


def font(size):
    for f in FONTS:
        try:
            return ImageFont.truetype(f, size)
        except OSError:
            continue
    return ImageFont.load_default()


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def background(d):
    for y in range(H * S):                                   # degrade sombre vertical
        d.line([(0, y), (W * S, y)], fill=lerp((10, 14, 20), (22, 36, 30), y / (H * S)))
    horizon = int(H * S * 0.60)
    cx = W * S // 2
    for i in range(-14, 15):                                 # sol en perspective : lignes de fuite
        d.line([(cx + i * 10 * S, horizon), (cx + i * 54 * S, H * S)], fill=(34, 74, 40), width=S)
    for k in range(1, 8):                                    # lignes horizontales de plus en plus espacees
        y = horizon + int((H * S - horizon) * (k / 7) ** 1.7)
        d.line([(0, y), (W * S, y)], fill=(34, 74, 40), width=S)
    d.line([(0, horizon), (W * S, horizon)], fill=GREEN, width=S)


def extruded_text(img, text, size, center, depth):
    f = font(size * S)
    d = ImageDraw.Draw(img)
    bbox = d.textbbox((0, 0), text, font=f)
    x = center[0] * S - (bbox[0] + bbox[2]) // 2
    y = center[1] * S - (bbox[1] + bbox[3]) // 2
    sh = Image.new("RGBA", img.size, (0, 0, 0, 0))           # ombre portee floue
    ImageDraw.Draw(sh).text((x + depth * S, y + (depth + 6) * S), text, font=f, fill=(0, 0, 0, 200))
    img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(3 * S)))
    for i in range(depth * S, 0, -1):                        # epaisseur : copies decalees, du fond vers l'avant
        t = i / (depth * S)
        d.text((x + i * 0.7, y + i * 0.9), text, font=f, fill=lerp((40, 80, 0), (78, 124, 0), 1 - t))
    d.text((x, y), text, font=f, fill=(255, 255, 255))      # face avant blanche
    face = Image.new("L", img.size, 0)
    ImageDraw.Draw(face).text((x, y), text, font=f, fill=255)
    grad = Image.new("RGBA", img.size, (0, 0, 0, 0))
    gd = ImageDraw.Draw(grad)
    for yy in range(img.size[1]):
        gd.line([(0, yy), (img.size[0], yy)], fill=lerp((235, 255, 200), GREEN, min(1, max(0, (yy - y) / (size * S)))) + (255,))
    img.paste(grad, (0, 0), face)                           # face avant en degrade vert clair -> vert


def banner():
    """Banniere simple : fond uni sombre, texte 'gfn3ds' extrude. Aucun logo."""
    img = Image.new("RGBA", (W * S, H * S))
    d = ImageDraw.Draw(img)
    for y in range(H * S):
        d.line([(0, y), (W * S, y)], fill=lerp((14, 18, 24), (26, 34, 30), y / (H * S)))
    extruded_text(img, "gfn3ds", 46, (W // 2, H // 2 - 4), 6)
    return img.resize((W, H), Image.LANCZOS).convert("RGB")


def jingle(path, rate=22050):
    """Petit carillon de 1,5 s (3 notes), volume modere : le menu HOME le joue quand on survole le titre."""
    out = []
    for freq, dur in ((659.3, 0.4), (830.6, 0.4), (987.8, 0.7)):
        n = int(rate * dur)
        for i in range(n):
            env = min(1.0, i / 200) * math.exp(-3.2 * i / n)
            v = math.sin(2 * math.pi * freq * i / rate) + 0.3 * math.sin(4 * math.pi * freq * i / rate)
            out.append(int(7000 * env * v))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", max(-32768, min(32767, s))) for s in out))


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets")
    os.makedirs(out, exist_ok=True)
    banner().save(os.path.join(out, "banner.png"))
    jingle(os.path.join(out, "audio.wav"))
    print("banniere et jingle ecrits dans", os.path.abspath(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
