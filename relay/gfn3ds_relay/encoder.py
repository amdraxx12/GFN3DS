"""Mise a l'echelle 400x240, rotation et encodage JPEG pour la 3DS.

Pourquoi tourner l'image sur le PC : le framebuffer de la 3DS est range colonne par colonne
(libctru : pixel (x, y) a l'index x*240 + (239 - y)). Si l'image est tournee de 90 degres dans
le sens horaire avant l'encodage, chaque ligne du JPEG (240 pixels) est exactement une colonne
du framebuffer : la 3DS decode directement dans l'ecran, sans copie ni transposition.
"""
from __future__ import annotations

import cv2
import numpy as np

from .protocol import SCREEN_H, SCREEN_W


class FrameEncoder:
    def __init__(self, fit: str = "stretch"):
        if fit not in ("stretch", "letterbox", "crop"):
            raise ValueError("fit doit etre stretch, letterbox ou crop")
        self.fit = fit
        self._params_cache = {}

    def fit_frame(self, frame: np.ndarray) -> np.ndarray:
        """Image BGR 400x240 (hauteur 240, largeur 400)."""
        if frame.ndim == 3 and frame.shape[2] == 4:
            frame = cv2.cvtColor(frame, cv2.COLOR_BGRA2BGR)
        h, w = frame.shape[:2]
        if (w, h) == (SCREEN_W, SCREEN_H):
            return frame
        if self.fit == "letterbox":
            s = min(SCREEN_W / w, SCREEN_H / h)
            nw, nh = max(1, round(w * s)), max(1, round(h * s))
            small = cv2.resize(frame, (nw, nh), interpolation=cv2.INTER_AREA)
            canvas = np.zeros((SCREEN_H, SCREEN_W, 3), np.uint8)
            x0, y0 = (SCREEN_W - nw) // 2, (SCREEN_H - nh) // 2
            canvas[y0:y0 + nh, x0:x0 + nw] = small
            return canvas
        if self.fit == "crop":
            s = max(SCREEN_W / w, SCREEN_H / h)
            nw, nh = max(SCREEN_W, round(w * s)), max(SCREEN_H, round(h * s))
            big = cv2.resize(frame, (nw, nh), interpolation=cv2.INTER_AREA)
            x0, y0 = (nw - SCREEN_W) // 2, (nh - SCREEN_H) // 2
            return big[y0:y0 + SCREEN_H, x0:x0 + SCREEN_W]
        return cv2.resize(frame, (SCREEN_W, SCREEN_H), interpolation=cv2.INTER_AREA)

    def _params(self, quality: int):
        p = self._params_cache.get(quality)
        if p is None:
            p = [cv2.IMWRITE_JPEG_QUALITY, int(quality),
                 cv2.IMWRITE_JPEG_PROGRESSIVE, 0,   # JPEG de base : decodage le plus rapide
                 cv2.IMWRITE_JPEG_OPTIMIZE, 0]
            if hasattr(cv2, "IMWRITE_JPEG_SAMPLING_FACTOR"):  # chroma 4:2:0
                p += [cv2.IMWRITE_JPEG_SAMPLING_FACTOR, cv2.IMWRITE_JPEG_SAMPLING_FACTOR_420]
            self._params_cache[quality] = p
        return p

    def encode_fitted(self, fitted: np.ndarray, quality: int) -> bytes:
        """JPEG de 240 pixels de large sur 400 de haut a partir d'une image deja en 400x240."""
        rot = cv2.rotate(fitted, cv2.ROTATE_90_CLOCKWISE)
        ok, buf = cv2.imencode(".jpg", rot, self._params(quality))
        if not ok:
            raise RuntimeError("encodage JPEG impossible")
        return buf.tobytes()

    def encode(self, frame: np.ndarray, quality: int) -> bytes:
        """Comme encode_fitted, mais accepte une image de taille quelconque."""
        return self.encode_fitted(self.fit_frame(frame), quality)


class QualityController:
    """Ajuste la qualite JPEG pour que la taille des images reste proche de la cible."""

    def __init__(self, q_min: int, q_max: int):
        self.q_min, self.q_max = int(q_min), int(q_max)
        self.q = (self.q_min + self.q_max) // 2

    def set_bounds(self, q_min: int, q_max: int) -> None:
        self.q_min, self.q_max = int(q_min), int(q_max)
        self.q = max(self.q_min, min(self.q_max, self.q))

    def update(self, size: int, target: int) -> None:
        if size > target * 1.3:
            self.q = max(self.q_min, self.q - 4)
        elif size > target:
            self.q = max(self.q_min, self.q - 1)
        elif size < target * 0.7:
            self.q = min(self.q_max, self.q + 1)
