"""Catalogue des jeux proposes a la 3DS.

La 3DS ne recoit que (id, nom) et ne renvoie qu'un id : aucune commande ne vient jamais de la
console. Tout ce qui peut etre lance est decide ici, sur le PC.

Types d'entrees (dans games.json) :
  shortcut  : raccourci .lnk / .url (par ex. cree par le client GeForce NOW), ouvert comme par
              un double-clic. C'est le mode recommande : le relais n'a pas a connaitre le format
              interne de GeForce NOW.
  url_route : GeForceNOW.exe --url-route=#?cmsId=...  (format documente pour le client Linux,
              NON verifie sous Windows : a tester avant de s'y fier)
  command   : ligne de commande libre, liste d'arguments (jamais passee a un shell)

Ajout automatique (sans rien ecrire) des raccourcis .lnk / .url / .gfnpc trouves dans :
  - le dossier games/ ;
  - les dossiers de config games.extra_dirs ;
  - si games.auto_scan : le dossier du menu Demarrer "NVIDIA Corporation\\Games" (tout est pris) et le
    Bureau (seuls les .gfnpc, le raccourci de jeu du client GeForce NOW, sont pris).
La liste est relue a chaque demande de la 3DS (bouton Actualiser).
"""
from __future__ import annotations

import json
import logging
import os
import re
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple

from . import winutil
from .config import resolve
from .protocol import ascii_clean

log = logging.getLogger("games")

HOME_ID = "__gfn_home__"
SHORTCUT_EXT = (".lnk", ".url", ".gfnpc")
DESKTOP_EXT = (".gfnpc",)
KINDS = ("shortcut", "url_route", "command")
MAX_SCAN_DEPTH = 2
_GFN_SUFFIX = re.compile(r"\s*[-(\[]*\s*(on\s+)?geforce\s*now\s*[)\]]*\s*$", re.IGNORECASE)


def effective_gfn_exe(cfg: Dict[str, Any]) -> str:
    """GeForceNOW.exe configure, sinon trouve dans les emplacements usuels, sinon ''."""
    exe = cfg["gfn"].get("exe", "")
    if exe and os.path.exists(exe):
        return exe
    if cfg["gfn"].get("auto_detect", True):
        return winutil.find_gfn_exe()
    return ""


def shortcut_name(stem: str) -> str:
    """Nom affiche : sans le suffixe ' (GeForce NOW)' que certains raccourcis portent."""
    cleaned = _GFN_SUFFIX.sub("", stem).strip()
    return ascii_clean(cleaned or stem, 38)


def scan_shortcuts(folder: str, accept: Tuple[str, ...] = SHORTCUT_EXT) -> List[Tuple[str, str]]:
    """[(nom, chemin)] des raccourcis d'un dossier, sous-dossiers compris (2 niveaux), tries."""
    found: List[Tuple[str, str]] = []
    if not os.path.isdir(folder):
        return found
    base_depth = folder.rstrip("\\/").count(os.sep)
    for d, dirs, names in os.walk(folder):
        if d.rstrip("\\/").count(os.sep) - base_depth >= MAX_SCAN_DEPTH:
            dirs[:] = []
        for fn in names:
            stem, ext = os.path.splitext(fn)
            if ext.lower() in accept:
                name = shortcut_name(stem)
                if name:
                    found.append((name, os.path.join(d, fn)))
    return sorted(found, key=lambda t: (t[0].lower(), t[1].lower()))


@dataclass
class Game:
    id: str
    name: str
    kind: str
    path: str = ""                                   # shortcut
    argv: List[str] = field(default_factory=list)    # url_route / command


def slug(text: str) -> str:
    s = re.sub(r"[^a-z0-9]+", "-", ascii_clean(text, 60).lower()).strip("-")
    return s[:32] or "jeu"


def build_url_route(cms_id: str, short_name: str, parent_game_id: str = "") -> str:
    return "--url-route=#?cmsId=%s&launchSource=External&shortName=%s&parentGameId=%s" % (
        cms_id, short_name, parent_game_id)


class GameCatalog:
    def __init__(self, cfg: Dict[str, Any]):
        self.cfg = cfg
        self._games: List[Game] = []
        self.reload()

    # ------------------------------------------------------------- lecture
    def reload(self) -> None:
        games: List[Game] = []
        used = set()

        def add(g: Game) -> None:
            base, n = g.id, 2
            while g.id in used:
                g.id = "%s-%d" % (base, n)
                n += 1
            used.add(g.id)
            games.append(g)

        gfn_exe = effective_gfn_exe(self.cfg)
        if gfn_exe:
            used.add(HOME_ID)
            games.append(Game(HOME_ID, "[ Ouvrir GeForce NOW ]", "command", argv=[gfn_exe]))

        for entry in self._read_json():
            g = self._from_entry(entry)
            if g:
                add(g)

        # Raccourcis trouves automatiquement : un seul jeu par nom (le premier source l'emporte)
        seen = {g.name.lower() for g in games}
        sources: List[Tuple[str, Tuple[str, ...]]] = [
            (resolve(self.cfg, self.cfg["games"]["folder"]), SHORTCUT_EXT)]
        for extra in self.cfg["games"].get("extra_dirs") or []:
            if isinstance(extra, str) and extra:
                sources.append((resolve(self.cfg, extra), SHORTCUT_EXT))
        if self.cfg["games"].get("auto_scan", True):
            for d, accept_all in winutil.gfn_shortcut_dirs():
                sources.append((d, SHORTCUT_EXT if accept_all else DESKTOP_EXT))
        for folder, accept in sources:
            for name, path in scan_shortcuts(folder, accept):
                if name.lower() in seen:
                    continue
                seen.add(name.lower())
                add(Game("lnk-" + slug(name), name, "shortcut", path=path))

        head = [g for g in games if g.id == HOME_ID]
        rest = sorted([g for g in games if g.id != HOME_ID], key=lambda g: g.name.lower())
        self._games = head + rest
        log.info("catalogue : %d jeu(x)", len(self._games))

    def _read_json(self) -> List[Dict[str, Any]]:
        path = resolve(self.cfg, self.cfg["games"]["json"])
        if not os.path.exists(path):
            return []
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError) as e:
            log.error("games.json illisible (%s) : %s", path, e)
            return []
        if not isinstance(data, list):
            log.error("games.json doit contenir une liste")
            return []
        return [e for e in data if isinstance(e, dict)]

    def _from_entry(self, e: Dict[str, Any]) -> Optional[Game]:
        if e.get("enabled", True) is False:
            return None
        name = ascii_clean(e.get("name", ""), 38)
        kind = e.get("type", "shortcut")
        if not name or kind not in KINDS:
            log.warning("entree ignoree (nom/type invalide) : %r", e)
            return None
        gid = slug(e.get("id") or name)
        if kind == "shortcut":
            p = e.get("path", "")
            if not p:
                log.warning("%s : 'path' manquant", name)
                return None
            folder = resolve(self.cfg, self.cfg["games"]["folder"])
            return Game(gid, name, kind, path=p if os.path.isabs(p) else os.path.join(folder, p))
        if kind == "url_route":
            exe = effective_gfn_exe(self.cfg)
            cms = str(e.get("cms_id", ""))
            if not exe or not cms:
                log.warning("%s : url_route demande gfn.exe (config) et cms_id", name)
                return None
            argv = [exe, build_url_route(cms, str(e.get("short_name", "")),
                                         str(e.get("parent_game_id", "")))]
            return Game(gid, name, kind, argv=argv)
        argv = e.get("command")
        if not (isinstance(argv, list) and argv and all(isinstance(a, str) for a in argv)):
            log.warning("%s : 'command' doit etre une liste de chaines", name)
            return None
        return Game(gid, name, kind, argv=list(argv))

    # -------------------------------------------------------------- acces
    def games(self) -> List[Game]:
        return list(self._games)

    def get(self, game_id: str) -> Optional[Game]:
        for g in self._games:
            if g.id == game_id:
                return g
        return None
