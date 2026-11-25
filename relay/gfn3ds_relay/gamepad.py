"""Traduction des commandes 3DS en manette Xbox 360 virtuelle (ViGEmBus, via vgamepad).

GeForce NOW voit cette manette comme une vraie manette branchee au PC (XInput).
map_input() est une fonction pure (testable sans Windows) ; VirtualPad applique l'etat au pilote.
"""
from __future__ import annotations

import logging
import math
import threading
from dataclasses import dataclass
from typing import Any, Dict, FrozenSet

from . import protocol as P

log = logging.getLogger("gamepad")

# Noms utilisables dans config.json -> bits du protocole
BUTTON_BITS: Dict[str, int] = {
    "A": P.B_A, "B": P.B_B, "X": P.B_X, "Y": P.B_Y,
    "L": P.B_L, "R": P.B_R, "ZL": P.B_ZL, "ZR": P.B_ZR,
    "START": P.B_START, "SELECT": P.B_SELECT,
    "DUP": P.B_DUP, "DDOWN": P.B_DDOWN, "DLEFT": P.B_DLEFT, "DRIGHT": P.B_DRIGHT,
}
# Boutons virtuels (zones tactiles) : toujours associes a la meme commande Xbox
VIRTUAL_BITS: Dict[int, str] = {
    P.B_VLT: "LT", P.B_VRT: "RT", P.B_L3: "L3", P.B_R3: "R3",
    P.B_VSTART: "START", P.B_VBACK: "BACK", P.B_GUIDE: "GUIDE",
}
# Commandes Xbox acceptees dans le mapping
XBOX_CONTROLS = ("A", "B", "X", "Y", "LB", "RB", "LT", "RT", "START", "BACK", "GUIDE",
                 "L3", "R3", "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT")
# Echange "position physique" : le bouton du bas de la 3DS (B) devient le A de la Xbox, etc.
POSITION_SWAP = {"A": "B", "B": "A", "X": "Y", "Y": "X"}

DEFAULT_MAPPING: Dict[str, str] = {
    "A": "A", "B": "B", "X": "X", "Y": "Y",
    "L": "LB", "R": "RB", "ZL": "LT", "ZR": "RT",
    "START": "START", "SELECT": "BACK",
    "DUP": "DPAD_UP", "DDOWN": "DPAD_DOWN", "DLEFT": "DPAD_LEFT", "DRIGHT": "DPAD_RIGHT",
}

STICK_MAX = 32767


@dataclass(frozen=True)
class PadState:
    buttons: FrozenSet[str] = frozenset()
    lt: int = 0
    rt: int = 0
    lx: int = 0
    ly: int = 0
    rx: int = 0
    ry: int = 0

    @property
    def neutral(self) -> bool:
        return not (self.buttons or self.lt or self.rt or self.lx or self.ly or self.rx or self.ry)


def stick(x: int, y: int, max_raw: float, dead: float):
    """Deadzone radiale puis mise a l'echelle sur +-32767. y positif = haut (comme XInput)."""
    r = math.hypot(x, y)
    if r <= dead or max_raw <= dead:
        return 0, 0
    scale = min(1.0, (r - dead) / (max_raw - dead))
    nx, ny = x / r, y / r
    return (max(-STICK_MAX, min(STICK_MAX, int(round(nx * scale * STICK_MAX)))),
            max(-STICK_MAX, min(STICK_MAX, int(round(ny * scale * STICK_MAX)))))


def map_input(pkt: P.InputPacket, icfg: Dict[str, Any]) -> PadState:
    mapping = dict(DEFAULT_MAPPING)
    mapping.update({k.upper(): v for k, v in icfg.get("mapping", {}).items()})
    swap = icfg.get("face_layout", "label") == "position"

    pressed = set()
    for name, bit in BUTTON_BITS.items():
        if not pkt.buttons & bit:
            continue
        target = mapping.get(name)
        if not target or target.upper() == "NONE":
            continue
        target = target.upper()
        if swap and target in POSITION_SWAP:
            target = POSITION_SWAP[target]
        pressed.add(target)
    for bit, target in VIRTUAL_BITS.items():
        if pkt.buttons & bit:
            pressed.add(target)

    lt = 255 if "LT" in pressed else 0
    rt = 255 if "RT" in pressed else 0
    pressed.discard("LT")
    pressed.discard("RT")

    lx, ly = stick(pkt.cpad_x, pkt.cpad_y, icfg.get("cpad_max", 150), icfg.get("deadzone_cpad", 12))
    rx, ry = stick(pkt.cstick_x, pkt.cstick_y, icfg.get("cstick_max", 120),
                   icfg.get("deadzone_cstick", 12))
    return PadState(frozenset(pressed), lt, rt, lx, ly, rx, ry)


class VirtualPad:
    """Manette Xbox 360 virtuelle. Si vgamepad / ViGEmBus manque, les commandes sont ignorees."""

    def __init__(self, enabled: bool = True):
        self.enabled = enabled
        self.available = False
        self._vg: Any = None
        self._pad: Any = None
        self._consts: Dict[str, Any] = {}
        self._last = PadState()
        self._lock = threading.Lock()
        self._warned = False
        self.history: list = []   # utilise par les tests

    def open(self) -> bool:
        with self._lock:
            if not self.enabled:
                return False
            if self._pad is not None:
                return True
            try:
                import vgamepad as vg  # type: ignore[import-not-found]
                self._vg = vg
                btn = vg.XUSB_BUTTON
                self._consts = {
                    "A": btn.XUSB_GAMEPAD_A, "B": btn.XUSB_GAMEPAD_B,
                    "X": btn.XUSB_GAMEPAD_X, "Y": btn.XUSB_GAMEPAD_Y,
                    "LB": btn.XUSB_GAMEPAD_LEFT_SHOULDER, "RB": btn.XUSB_GAMEPAD_RIGHT_SHOULDER,
                    "START": btn.XUSB_GAMEPAD_START, "BACK": btn.XUSB_GAMEPAD_BACK,
                    "GUIDE": btn.XUSB_GAMEPAD_GUIDE,
                    "L3": btn.XUSB_GAMEPAD_LEFT_THUMB, "R3": btn.XUSB_GAMEPAD_RIGHT_THUMB,
                    "DPAD_UP": btn.XUSB_GAMEPAD_DPAD_UP, "DPAD_DOWN": btn.XUSB_GAMEPAD_DPAD_DOWN,
                    "DPAD_LEFT": btn.XUSB_GAMEPAD_DPAD_LEFT, "DPAD_RIGHT": btn.XUSB_GAMEPAD_DPAD_RIGHT,
                }
                self._pad = vg.VX360Gamepad()   # RuntimeError si ViGEmBus est absent
                self._last = PadState()
                self.available = True
                log.info("manette virtuelle Xbox 360 branchee")
                return True
            except Exception as e:  # noqa: BLE001 - ImportError, RuntimeError, OSError...
                self.available = False
                if not self._warned:
                    self._warned = True
                    log.warning("manette virtuelle indisponible (%s) : les commandes de la 3DS "
                                "seront ignorees. Installe vgamepad et le pilote ViGEmBus.", e)
                return False

    def close(self) -> None:
        with self._lock:
            if self._pad is not None:
                try:
                    self._pad.reset()
                    self._pad.update()
                except Exception:  # noqa: BLE001
                    pass
                self._pad = None   # vgamepad debranche la manette a la destruction de l'objet
                log.info("manette virtuelle debranchee")
            self._last = PadState()
            self.available = False

    def apply(self, st: PadState) -> None:
        with self._lock:
            self.history.append(st)
            if len(self.history) > 1000:
                del self.history[:500]
            if self._pad is None or st == self._last:
                return
            try:
                self._write(st)
            except Exception:  # noqa: BLE001
                log.exception("ecriture manette")
            self._last = st

    def neutral(self) -> None:
        self.apply(PadState())

    def _write(self, st: PadState) -> None:
        pad, last = self._pad, self._last
        for name, const in self._consts.items():
            now, before = name in st.buttons, name in last.buttons
            if now and not before:
                pad.press_button(button=const)
            elif before and not now:
                pad.release_button(button=const)
        if st.lt != last.lt:
            pad.left_trigger(value=st.lt)
        if st.rt != last.rt:
            pad.right_trigger(value=st.rt)
        if (st.lx, st.ly) != (last.lx, last.ly):
            pad.left_joystick(x_value=st.lx, y_value=st.ly)
        if (st.rx, st.ry) != (last.rx, last.ry):
            pad.right_joystick(x_value=st.rx, y_value=st.ry)
        pad.update()
