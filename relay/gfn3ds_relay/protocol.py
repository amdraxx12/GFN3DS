"""Protocole relais PC <-> client 3DS (version 1).

Quatre canaux :
  - controle : TCP, lignes de texte ASCII, champs separes par des tabulations
  - video    : UDP, relais -> 3DS, images JPEG fragmentees (12 octets d'en-tete)
  - entrees  : UDP, 3DS -> relais, paquets de 16 octets
  - balises  : UDP, relais -> broadcast, ecoutees par la 3DS (decouverte)

Tout est en little-endian (ARM11 de la 3DS et PC x86 : pas de conversion).
Ce fichier est la reference ; client3ds/include/protocol.hpp doit rester identique
(tests/test_protocol_sync.py verifie les constantes).
"""
from __future__ import annotations

import struct
import unicodedata
from dataclasses import dataclass
from typing import List, Optional, Tuple

PROTO_VERSION = 1

# Ports par defaut (hors plage 47984-48010 utilisee par Sunshine/GameStream)
PORT_CONTROL = 47970  # TCP, le relais ecoute
PORT_INPUT = 47971    # UDP, le relais ecoute (entrees 3DS -> PC)
PORT_VIDEO = 47972    # UDP, la 3DS ecoute (video PC -> 3DS)
PORT_BEACON = 47973   # UDP, la 3DS ecoute (balises du relais)

# ---------------------------------------------------------------- video
SCREEN_W, SCREEN_H = 400, 240  # ecran du haut de la 3DS

VIDEO_MAGIC = 0x47
KIND_JPEG = 1
FRAG_PAYLOAD = 1200           # octets de JPEG par datagramme
MAX_FRAGS = 64                # 64 * 1200 > MAX_FRAME_BYTES
MAX_FRAME_BYTES = 64 * 1024   # taille max d'une image JPEG
VIDEO_HDR = struct.Struct("<BBHHHI")  # magic, kind, frame_id, frag_idx, frag_cnt, ts_ms
assert VIDEO_HDR.size == 12


def fragment_frame(frame_id: int, jpeg: bytes, ts_ms: int) -> List[bytes]:
    """Decoupe une image JPEG en datagrammes prets a l'envoi."""
    n = (len(jpeg) + FRAG_PAYLOAD - 1) // FRAG_PAYLOAD
    if n == 0 or n > MAX_FRAGS or len(jpeg) > MAX_FRAME_BYTES:
        raise ValueError("image JPEG vide ou trop grosse: %d octets" % len(jpeg))
    out = []
    for i in range(n):
        hdr = VIDEO_HDR.pack(VIDEO_MAGIC, KIND_JPEG, frame_id & 0xFFFF, i, n,
                             ts_ms & 0xFFFFFFFF)
        out.append(hdr + jpeg[i * FRAG_PAYLOAD:(i + 1) * FRAG_PAYLOAD])
    return out


def parse_video_packet(data: bytes) -> Optional[Tuple[int, int, int, int, bytes]]:
    """Retourne (frame_id, frag_idx, frag_cnt, ts_ms, payload) ou None si invalide."""
    if len(data) <= VIDEO_HDR.size:
        return None
    magic, kind, fid, idx, cnt, ts = VIDEO_HDR.unpack_from(data)
    if magic != VIDEO_MAGIC or kind != KIND_JPEG or cnt == 0 or cnt > MAX_FRAGS or idx >= cnt:
        return None
    return fid, idx, cnt, ts, data[VIDEO_HDR.size:]


# --------------------------------------------------------------- entrees
# Bits du champ "buttons". Numerotation propre au protocole : le client 3DS traduit
# explicitement les KEY_* de libctru (input.cpp), on ne suppose donc rien sur leurs valeurs.
B_A, B_B, B_X, B_Y = 1 << 0, 1 << 1, 1 << 2, 1 << 3
B_L, B_R, B_ZL, B_ZR = 1 << 4, 1 << 5, 1 << 6, 1 << 7
B_START, B_SELECT = 1 << 8, 1 << 9
B_DUP, B_DDOWN, B_DLEFT, B_DRIGHT = 1 << 10, 1 << 11, 1 << 12, 1 << 13
# Boutons virtuels (zones tactiles de l'ecran du bas, dessinees par le client)
B_VLT, B_VRT = 1 << 16, 1 << 17      # gachettes gauche/droite analogiques (Old 3DS sans ZL/ZR)
B_L3, B_R3 = 1 << 18, 1 << 19        # clic des sticks
B_VSTART, B_VBACK, B_GUIDE = 1 << 20, 1 << 21, 1 << 22

INPUT_MAGIC = 0x49
INPUT_FLAG_NEW3DS = 0x01
INPUT_PKT = struct.Struct("<BBHIhhhh")  # magic, flags, seq, buttons, cpad x/y, cstick x/y
assert INPUT_PKT.size == 16


@dataclass
class InputPacket:
    flags: int = 0
    seq: int = 0
    buttons: int = 0
    cpad_x: int = 0
    cpad_y: int = 0       # positif = haut (comme libctru et XInput)
    cstick_x: int = 0
    cstick_y: int = 0

    @property
    def new3ds(self) -> bool:
        return bool(self.flags & INPUT_FLAG_NEW3DS)

    def pack(self) -> bytes:
        return INPUT_PKT.pack(INPUT_MAGIC, self.flags, self.seq & 0xFFFF, self.buttons & 0xFFFFFFFF,
                              self.cpad_x, self.cpad_y, self.cstick_x, self.cstick_y)

    @staticmethod
    def unpack(data: bytes) -> Optional["InputPacket"]:
        if len(data) != INPUT_PKT.size:
            return None
        magic, flags, seq, buttons, cx, cy, sx, sy = INPUT_PKT.unpack(data)
        if magic != INPUT_MAGIC:
            return None
        return InputPacket(flags, seq, buttons, cx, cy, sx, sy)


def seq_newer(seq: int, last: int) -> bool:
    """Vrai si seq est plus recent que last (compteurs de 16 bits qui bouclent)."""
    d = (seq - last) & 0xFFFF
    return 0 < d < 0x8000


# -------------------------------------------------------------- controle
class State:
    IDLE = "IDLE"
    LAUNCHING = "LAUNCHING"
    QUEUE = "QUEUE"
    LOADING = "LOADING"
    STREAMING = "STREAMING"
    ERROR = "ERROR"
    # Jeu lance, mais le relais ne sait pas suivre son avancement (expressions de journal non
    # calibrees) : la console affiche l'ecran de GeForce NOW du PC tel quel, file d'attente comprise.
    UNTRACKED = "UNTRACKED"


MAX_LINE = 512


def ascii_clean(text: str, maxlen: int = 80) -> str:
    """Texte ASCII imprimable sans tabulation ni saut de ligne (la police de la 3DS est ASCII)."""
    t = unicodedata.normalize("NFKD", str(text)).encode("ascii", "ignore").decode("ascii")
    t = "".join(c if 32 <= ord(c) < 127 else " " for c in t)
    return " ".join(t.split())[:maxlen]


def make_line(*fields) -> bytes:
    """Une ligne du canal de controle : champs separes par \\t, terminee par \\n."""
    clean = (str(f).replace("\t", " ").replace("\r", " ").replace("\n", " ") for f in fields)
    return ("\t".join(clean) + "\n").encode("ascii", "replace")


def split_line(raw: bytes) -> List[str]:
    return raw.decode("ascii", "replace").rstrip("\r\n").split("\t")


def make_beacon(name: str, tcp_port: int) -> bytes:
    return make_line("GFN3DS", PROTO_VERSION, ascii_clean(name, 24), tcp_port)
