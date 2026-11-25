"""Sources d'images : fenetre GeForce NOW (dxcam), image de test, fichier video.

Toutes les sources renvoient des images BGR uint8 (H x W x 3) de taille quelconque ;
le redimensionnement vers 400x240 est fait par encoder.py.
"""
from __future__ import annotations

import logging
import time
from typing import Any, Dict, Optional

import cv2
import numpy as np

from . import winutil

log = logging.getLogger("capture")


class FrameSource:
    name = "source"

    def start(self) -> None:
        pass

    def read(self) -> Optional[np.ndarray]:
        raise NotImplementedError

    def stop(self) -> None:
        pass


class TestPatternSource(FrameSource):
    """Image 1280x720 asymetrique : coins colores, texte, barre mobile, compteur.

    Sert a verifier l'orientation, les couleurs et la fluidite sans GeForce NOW.
    Coins : haut-gauche rouge, haut-droit vert, bas-gauche bleu, bas-droit jaune.
    """
    name = "image de test"

    def __init__(self, width: int = 1280, height: int = 720):
        self.w, self.h = width, height
        self.n = 0
        xs = np.linspace(40, 200, width, dtype=np.uint8)[None, :]
        ys = np.linspace(40, 200, height, dtype=np.uint8)[:, None]
        base = np.zeros((height, width, 3), np.uint8)
        base[:, :, 0] = xs
        base[:, :, 1] = ys
        s = 120
        base[0:s, 0:s] = (0, 0, 255)                    # haut-gauche : rouge (BGR)
        base[0:s, width - s:width] = (0, 255, 0)        # haut-droit : vert
        base[height - s:height, 0:s] = (255, 0, 0)      # bas-gauche : bleu
        base[height - s:height, width - s:width] = (0, 255, 255)  # bas-droit : jaune
        cv2.putText(base, "TOP", (width // 2 - 150, 150), cv2.FONT_HERSHEY_SIMPLEX, 4.0,
                    (255, 255, 255), 8, cv2.LINE_AA)
        cv2.putText(base, "GFN 3DS", (width // 2 - 260, height // 2 + 40),
                    cv2.FONT_HERSHEY_SIMPLEX, 3.5, (255, 255, 255), 8, cv2.LINE_AA)
        self.base = base

    def read(self) -> np.ndarray:
        f = self.base.copy()
        x = (self.n * 12) % self.w
        cv2.rectangle(f, (x, self.h - 220), (min(x + 40, self.w - 1), self.h - 150), (255, 255, 255), -1)
        cv2.putText(f, "%06d" % self.n, (self.w // 2 - 170, self.h - 40),
                    cv2.FONT_HERSHEY_SIMPLEX, 3.0, (255, 255, 255), 6, cv2.LINE_AA)
        self.n += 1
        return f


class VideoFileSource(FrameSource):
    """Lit un fichier video en boucle (pour tester le decodage sur la 3DS)."""

    def __init__(self, path: str):
        self.path = path
        self.name = "video " + path
        self.cap: Any = None

    def start(self) -> None:
        self.cap = cv2.VideoCapture(self.path)
        if not self.cap.isOpened():
            raise RuntimeError("impossible d'ouvrir la video %s" % self.path)

    def read(self) -> Optional[np.ndarray]:
        ok, frame = self.cap.read()
        if not ok:
            self.cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
            ok, frame = self.cap.read()
        return frame if ok else None

    def stop(self) -> None:
        if self.cap is not None:
            self.cap.release()
            self.cap = None


class DxcamSource(FrameSource):
    """Capture DXGI (Windows) de la fenetre GeForce NOW, sinon de l'ecran entier."""
    name = "fenetre GeForce NOW (dxcam)"

    def __init__(self, window_title: str, monitor: int = 0, region=None, fps: int = 60,
                 window_process: str = "geforcenow"):
        self.window_title = window_title
        self.window_process = window_process
        self.monitor = monitor
        self.forced_region = tuple(region) if region else None
        self.fps = fps
        self._cam: Any = None
        self._region = None
        self._last_check = 0.0
        self._hwnd = 0

    def start(self) -> None:
        if not winutil.IS_WINDOWS:
            raise RuntimeError("la capture GeForce NOW demande Windows "
                               "(utilise --source testpattern pour tester ailleurs)")
        winutil.set_dpi_aware()
        import dxcam  # installe par requirements.txt sur Windows
        self._cam = dxcam.create(output_idx=self.monitor, output_color="BGR")
        if self._cam is None:
            raise RuntimeError("dxcam.create() a echoue (moniteur %d ?)" % self.monitor)
        self._region = "unset"
        self._apply_region()

    def _wanted_region(self):
        if self.forced_region:
            return self.forced_region
        if self.monitor != 0:
            # Les coordonnees de dxcam sont relatives a l'ecran capture, alors que celles de la
            # fenetre sont relatives au bureau virtuel : sans info supplementaire on capture tout.
            return None
        w = winutil.find_window(self.window_title, self.window_process)
        if w is None:
            return None
        self._hwnd = w[0]
        l, t, r, b = w[1]
        sw, sh = winutil.screen_size()   # ecran principal : on borne la fenetre a ses dimensions
        l, t, r, b = max(0, l), max(0, t), min(sw, r), min(sh, b)
        if r - l < 64 or b - t < 64:
            return None
        return (l, t, r, b)

    def _apply_region(self) -> None:
        want = self._wanted_region()
        if want == self._region:
            return
        if self._cam.is_capturing:
            self._cam.stop()
        kw: Dict[str, Any] = {"target_fps": self.fps, "video_mode": True}
        if want:
            kw["region"] = want
        try:
            self._cam.start(**kw)
        except Exception as e:  # region refusee : repli sur l'ecran entier
            log.warning("region %s refusee (%s), capture de tout l'ecran", want, e)
            want = None
            kw.pop("region", None)
            self._cam.start(**kw)
        self._region = want
        log.info("capture : %s", "fenetre %s" % (want,) if want else "ecran entier")

    def read(self) -> Optional[np.ndarray]:
        now = time.monotonic()
        if now - self._last_check > 2.0:
            self._last_check = now
            self._apply_region()
        return self._cam.get_latest_frame()

    def stop(self) -> None:
        if self._cam is not None:
            try:
                if self._cam.is_capturing:
                    self._cam.stop()
                self._cam.release()
            except Exception:  # noqa: BLE001
                pass
            self._cam = None


def make_source(cfg: Dict[str, Any], override: Any = None) -> FrameSource:
    """override : nom de source (testpattern, video:<fichier>, gfn) ou instance de FrameSource."""
    if isinstance(override, FrameSource):
        return override
    src = override or cfg["capture"]["source"]
    if src == "testpattern":
        return TestPatternSource()
    if src.startswith("video:"):
        return VideoFileSource(src[len("video:"):])
    if src == "gfn":
        c = cfg["capture"]
        return DxcamSource(c["window_title"], c["monitor"], c["region"],
                           window_process=c.get("window_process", "geforcenow"))
    raise ValueError("source de capture inconnue: %r (gfn, testpattern, video:<fichier>)" % src)
