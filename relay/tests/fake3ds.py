"""Faux client 3DS : parle le vrai protocole (TCP controle, UDP video, UDP entrees).

Sert aux tests automatiques du relais, et de modele de reference pour le client C++
(la logique de reassemblage de Reassembler est reproduite dans video_decoder.cpp).
"""
from __future__ import annotations

import queue
import random
import socket
import threading
import time
from typing import List, Optional

from gfn3ds_relay import protocol as P


class Reassembler:
    """Politique : une seule image en cours ; une image plus recente abandonne l'ancienne."""

    def __init__(self) -> None:
        self.cur_id: Optional[int] = None
        self.cnt = 0
        self.got = {}
        self.done = False
        self.ok = 0
        self.lost = 0

    def feed(self, data: bytes) -> Optional[bytes]:
        parsed = P.parse_video_packet(data)
        if parsed is None:
            return None
        fid, idx, cnt, _ts, payload = parsed
        if fid != self.cur_id:
            if self.cur_id is not None and not P.seq_newer(fid, self.cur_id):
                return None                      # fragment d'une image plus ancienne
            if self.cur_id is not None and not self.done:
                self.lost += 1                   # l'image en cours n'a jamais ete completee
            self.cur_id, self.cnt, self.got, self.done = fid, cnt, {}, False
        if self.done or cnt != self.cnt:
            return None
        self.got[idx] = payload
        if len(self.got) == self.cnt:
            self.done = True
            self.ok += 1
            return b"".join(self.got[i] for i in range(self.cnt))
        return None


class Fake3DS:
    def __init__(self, control_port: int, device: str = "new", ip: str = "127.0.0.1"):
        self.ip = ip
        self.device = device
        self.vsock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.vsock.bind((ip, 0))
        self.vsock.settimeout(0.2)
        self.video_port = self.vsock.getsockname()[1]
        self.usock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.usock.bind((ip, 0))
        self.sock = socket.create_connection((ip, control_port), timeout=5)
        self.sock.settimeout(0.2)
        self.lines: "queue.Queue[List[str]]" = queue.Queue()
        self.history: List[List[str]] = []
        self.frames: List[bytes] = []
        self.frame_times: List[float] = []
        self.rx = Reassembler()
        self.drop_prob = 0.0
        self.input_port = 0
        self.seq = 0
        self._stop = threading.Event()
        self._tcp = threading.Thread(target=self._read_tcp, daemon=True)
        self._vid = threading.Thread(target=self._read_video, daemon=True)
        self._tcp.start()
        self._vid.start()

    # ----------------------------------------------------------------- tcp
    def _read_tcp(self) -> None:
        buf = b""
        while not self._stop.is_set():
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                return
            if not chunk:
                self.lines.put(["__CLOSED__"])
                return
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                self.lines.put(P.split_line(raw))

    def send(self, *fields) -> None:
        self.sock.sendall(P.make_line(*fields))

    def wait(self, prefix: str, timeout: float = 3.0, match=None) -> Optional[List[str]]:
        """Premiere ligne commencant par prefix (et acceptee par match) ; les autres sont gardees."""
        end = time.monotonic() + timeout
        for old in self.history:                 # peut-etre deja recue
            if old[0] == prefix and (match is None or match(old)):
                self.history.remove(old)
                return old
        while time.monotonic() < end:
            try:
                f = self.lines.get(timeout=0.1)
            except queue.Empty:
                continue
            if f[0] == prefix and (match is None or match(f)):
                return f
            self.history.append(f)
        return None

    def hello(self, pin: str = "") -> List[str]:
        self.send("HELLO", P.PROTO_VERSION, self.device, pin, self.video_port)
        for _ in range(50):
            try:
                f = self.lines.get(timeout=0.2)
            except queue.Empty:
                continue
            if f[0] in ("OK", "ERR"):
                if f[0] == "OK":
                    self.input_port = int(f[3])
                return f
            self.history.append(f)
        raise TimeoutError("pas de reponse a HELLO")

    def drain(self) -> None:
        while True:
            try:
                self.history.append(self.lines.get_nowait())
            except queue.Empty:
                return

    def all_lines(self, prefix: str) -> List[List[str]]:
        self.drain()
        return [h for h in self.history if h[0] == prefix]

    # --------------------------------------------------------------- video
    def _read_video(self) -> None:
        rng = random.Random(1234)
        while not self._stop.is_set():
            try:
                data, _ = self.vsock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                return
            if self.drop_prob and rng.random() < self.drop_prob:
                continue
            jpeg = self.rx.feed(data)
            if jpeg is not None:
                self.frames.append(jpeg)
                self.frame_times.append(time.monotonic())

    # -------------------------------------------------------------- entrees
    def send_input(self, buttons: int = 0, cpad=(0, 0), cstick=(0, 0), seq: Optional[int] = None) -> None:
        if seq is None:
            self.seq = (self.seq + 1) & 0xFFFF
            seq = self.seq
        pkt = P.InputPacket(P.INPUT_FLAG_NEW3DS if self.device == "new" else 0, seq, buttons,
                            cpad[0], cpad[1], cstick[0], cstick[1])
        self.usock.sendto(pkt.pack(), (self.ip, self.input_port))

    def close(self) -> None:
        self._stop.set()
        for s in (self.sock, self.vsock, self.usock):
            try:
                s.close()
            except OSError:
                pass
