"""Lancement d'un jeu dans le client GeForce NOW officiel et suivi de son etat (file d'attente).

Le relais ne parle jamais aux serveurs NVIDIA : connexion, captcha et session restent dans le
client officiel, sur le PC. Ici on se contente de
  1. lancer le client (raccourci / commande),
  2. lire les nouvelles lignes de ses journaux (%LOCALAPPDATA%\\NVIDIA Corporation\\GeForceNOW)
     et les confronter aux expressions regulieres de config.json (section "gfn").

IMPORTANT : le format des lignes de journal qui annoncent la position dans la file n'est pas
documente par NVIDIA. Les expressions sont donc VIDES par defaut ; on les calibre une fois avec
`python -m gfn3ds_relay.probe_queue` (voir README). Tant qu'elles sont vides, le relais ne sait
pas ou en est la session : l'etat passe a UNTRACKED apres le lancement et la 3DS affiche
simplement l'ecran du client GeForce NOW (la file d'attente y est visible telle quelle).
"""
from __future__ import annotations

import glob
import logging
import os
import re
import subprocess
import threading
from dataclasses import dataclass, replace
from typing import Any, Callable, Dict, List, Optional, Tuple

from . import winutil
from .games import Game
from .protocol import State, ascii_clean

log = logging.getLogger("gfn")

MAX_READ_PER_POLL = 256 * 1024
RX_KEYS = ("queue_regex", "eta_regex", "loading_regex", "streaming_regex", "ended_regex",
           "error_regex")


def default_log_dir() -> str:
    base = os.environ.get("LOCALAPPDATA", "")
    return os.path.join(base, "NVIDIA Corporation", "GeForceNOW") if base else ""


class LogTail:
    """Lit les lignes ajoutees aux fichiers *.log d'un dossier depuis le dernier prime()."""

    def __init__(self, log_dir: str):
        self.dir = log_dir
        self.offsets: Dict[str, int] = {}
        self.rest: Dict[str, str] = {}

    def _files(self) -> List[str]:
        if not self.dir or not os.path.isdir(self.dir):
            return []
        return sorted(glob.glob(os.path.join(self.dir, "*.log")))

    def prime(self) -> None:
        """Ignore tout le contenu actuel : seules les lignes futures seront lues."""
        self.offsets.clear()
        self.rest.clear()
        for p in self._files():
            try:
                self.offsets[p] = os.path.getsize(p)
            except OSError:
                pass

    def poll(self) -> List[Tuple[str, str]]:
        """Nouvelles lignes completes : liste de (nom de fichier, ligne)."""
        out: List[Tuple[str, str]] = []
        for p in self._files():
            try:
                size = os.path.getsize(p)
                off = self.offsets.get(p, 0)   # fichier apparu apres prime() : tout est nouveau
                if size < off:                 # tronque ou renouvele
                    off = 0
                    self.rest.pop(p, None)
                if size == off:
                    self.offsets[p] = off
                    continue
                with open(p, "rb") as f:
                    f.seek(off)
                    raw = f.read(min(size - off, MAX_READ_PER_POLL))
                self.offsets[p] = off + len(raw)
            except OSError:
                continue
            # les journaux UTF-16 deviennent lisibles une fois les octets nuls retires
            text = self.rest.pop(p, "") + raw.decode("utf-8", "replace").replace("\x00", "")
            lines = text.split("\n")
            self.rest[p] = lines.pop()         # la derniere ligne peut etre incomplete
            name = os.path.basename(p)
            out.extend((name, ln.rstrip("\r")) for ln in lines if ln.strip())
        return out


@dataclass(frozen=True)
class Status:
    state: str = State.IDLE
    queue_pos: int = -1
    eta_s: int = -1
    text: str = ""
    game: str = ""


class GfnMonitor:
    """Machine a etats alimentee par les journaux du client GeForce NOW."""

    def __init__(self, gfn_cfg: Dict[str, Any]):
        self.cfg = gfn_cfg
        self.tail = LogTail(gfn_cfg.get("log_dir") or default_log_dir())
        self._lock = threading.Lock()
        self._status = Status()
        self._listeners: List[Callable[[Status], None]] = []
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._rx = {k: self._compile(k) for k in RX_KEYS}
        self.calibrated = any(self._rx.values())

    def _compile(self, key: str):
        pat = self.cfg.get(key, "")
        if not pat:
            return None
        try:
            return re.compile(pat, re.IGNORECASE)
        except re.error as e:
            log.error("expression %s invalide (%s) : ignoree", key, e)
            return None

    # ------------------------------------------------------------- statut
    @property
    def status(self) -> Status:
        with self._lock:
            return self._status

    def add_listener(self, fn: Callable[[Status], None]) -> None:
        self._listeners.append(fn)

    def _set(self, **changes) -> None:
        with self._lock:
            new = replace(self._status, **changes)
            if new == self._status:
                return
            self._status = new
        log.info("etat : %s pos=%d eta=%ds %s", new.state, new.queue_pos, new.eta_s, new.text)
        for fn in list(self._listeners):
            try:
                fn(new)
            except Exception:  # noqa: BLE001
                log.exception("listener d'etat")

    def reset(self) -> None:
        self._set(state=State.IDLE, queue_pos=-1, eta_s=-1, text="", game="")

    # ------------------------------------------------------------ lancement
    def launch(self, game: Game) -> Tuple[bool, str]:
        self.tail.prime()
        self._set(state=State.LAUNCHING, queue_pos=-1, eta_s=-1, game=game.name,
                  text="Lancement de GeForce NOW")
        try:
            self._start_process(game)
        except Exception as e:  # noqa: BLE001 - toute erreur de lancement est renvoyee a la 3DS
            msg = ascii_clean("Lancement impossible : %s" % e, 80)
            self._set(state=State.ERROR, text=msg)
            return False, msg
        if not self.calibrated:
            # Aucune expression de journal : on ne peut ni lire la file d'attente ni savoir quand le
            # jeu demarre. On le dit franchement, et la console montre l'ecran du PC.
            self._set(state=State.UNTRACKED, text="File d'attente non suivie")
        return True, "ok"

    @staticmethod
    def _start_process(game: Game) -> None:
        if game.kind == "shortcut":
            if not os.path.exists(game.path):
                raise FileNotFoundError(os.path.basename(game.path))
            if not winutil.IS_WINDOWS:
                raise RuntimeError("ouvrir un raccourci demande Windows")
            os.startfile(game.path)  # type: ignore[attr-defined]  # noqa: S606
            return
        kw: Dict[str, Any] = {"close_fds": True, "stdin": subprocess.DEVNULL,
                              "stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL}
        if winutil.IS_WINDOWS:
            kw["creationflags"] = 0x00000008 | 0x00000200  # DETACHED_PROCESS | NEW_PROCESS_GROUP
        subprocess.Popen(game.argv, **kw)  # liste d'arguments : jamais de shell

    # ------------------------------------------------------------- journaux
    def feed(self, line: str) -> None:
        """Confronte une ligne de journal aux expressions et met l'etat a jour."""
        st = self.status
        if st.state == State.IDLE:
            return
        rx = self._rx
        if rx["error_regex"] and rx["error_regex"].search(line):
            self._set(state=State.ERROR, queue_pos=-1, eta_s=-1, text=ascii_clean(line, 70))
            return
        if rx["ended_regex"] and rx["ended_regex"].search(line):
            self.reset()
            return
        if rx["streaming_regex"] and rx["streaming_regex"].search(line):
            self._set(state=State.STREAMING, queue_pos=-1, eta_s=-1, text="En jeu")
            return
        if rx["loading_regex"] and rx["loading_regex"].search(line):
            self._set(state=State.LOADING, queue_pos=-1, eta_s=-1, text="Chargement du jeu")
            return
        m = rx["queue_regex"].search(line) if rx["queue_regex"] else None
        if m:
            pos = self._group_int(m, "pos")
            eta = self._eta_seconds(m)
            if pos >= 0:
                self._set(state=State.QUEUE, queue_pos=pos,
                          eta_s=eta if eta >= 0 else st.eta_s, text="File d'attente")
            return
        m = rx["eta_regex"].search(line) if rx["eta_regex"] else None
        if m and st.state in (State.QUEUE, State.LAUNCHING):
            eta = self._eta_seconds(m)
            if eta >= 0:
                self._set(state=State.QUEUE, eta_s=eta, text="File d'attente")

    @staticmethod
    def _group_int(m: "re.Match[str]", name: str) -> int:
        try:
            v = m.group(name)
        except IndexError:          # le groupe nomme n'existe pas dans cette expression
            return -1
        try:
            return int(v) if v is not None else -1
        except ValueError:
            return -1

    def _eta_seconds(self, m: "re.Match[str]") -> int:
        v = self._group_int(m, "eta")
        if v < 0:
            return -1
        return v * 60 if str(self.cfg.get("eta_unit", "min")).lower().startswith("m") else v

    # -------------------------------------------------------------- thread
    def start(self) -> None:
        if self._thread:
            return
        self._thread = threading.Thread(target=self._run, name="gfn-monitor", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()

    def _run(self) -> None:
        while not self._stop.wait(0.5):
            try:
                if self.status.state == State.IDLE:
                    continue
                for _name, line in self.tail.poll():
                    self.feed(line)
            except Exception:  # noqa: BLE001
                log.exception("lecture des journaux")
