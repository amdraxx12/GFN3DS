#!/usr/bin/env python3
"""Genere les images du projet : icone 48x48 (menu Homebrew), banniere 256x128 et son muet (CIA).

    python3 tools/make_assets.py          # ecrit dans assets/

Dependances : numpy et opencv-python (deja requis par le relais).
Les visuels sont volontairement simples et originaux : aucun logo NVIDIA n'est reproduit.
"""
import os
import struct
import sys
import wave

import cv2
import numpy as np

GREEN = (0, 185, 118)      # BGR de (118, 185, 0) : vert "GFN"
DARK = (28, 24, 22)
PANEL = (50, 44, 40)
WHITE = (240, 240, 240)


def put_center(img, text, cy, scale, color, thick):
    (w, h), _ = cv2.getTextSize(text, cv2.FONT_HERSHEY_DUPLEX, scale, thick)
    cv2.putText(img, text, ((img.shape[1] - w) // 2, cy + h // 2), cv2.FONT_HERSHEY_DUPLEX,
                scale, color, thick, cv2.LINE_AA)


def icon():
    img = np.full((48, 48, 3), DARK, np.uint8)
    cv2.rectangle(img, (2, 2), (45, 45), PANEL, -1)
    cv2.rectangle(img, (2, 2), (45, 45), GREEN, 2)
    put_center(img, "GFN", 18, 0.62, GREEN, 1)
    # petit ecran de console avec une ligne verte : evoque le streaming vers la 3DS
    cv2.rectangle(img, (10, 28), (37, 42), WHITE, 1)
    cv2.line(img, (13, 35), (34, 35), GREEN, 2)
    return img


def banner():
    img = np.full((128, 256, 3), DARK, np.uint8)
    for x in range(0, 256, 16):
        cv2.line(img, (x, 0), (x, 127), PANEL, 1)
    cv2.rectangle(img, (0, 0), (255, 127), GREEN, 3)
    put_center(img, "GFN 3DS", 52, 1.8, GREEN, 3)
    put_center(img, "client GeForce NOW via relais PC", 96, 0.5, WHITE, 1)
    return img


def silence(path, seconds=1, rate=22050):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<h", 0) * (rate * seconds))


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets")
    os.makedirs(out, exist_ok=True)
    cv2.imwrite(os.path.join(out, "icon.png"), icon())
    cv2.imwrite(os.path.join(out, "banner.png"), banner())
    silence(os.path.join(out, "audio.wav"))
    print("assets ecrits dans", os.path.abspath(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
