"""Configuration du relais : valeurs par defaut + fichier config.json de l'utilisateur."""
from __future__ import annotations

import copy
import json
import os
import socket
from typing import Any, Dict

from .protocol import PORT_BEACON, PORT_CONTROL, PORT_INPUT

# Profils de streaming. Valeurs de depart a regler sur ta console (voir README).
#   fps       : cadence cible
#   q_min/max : bornes de qualite JPEG (le relais ajuste dans cet intervalle)
#   target_kb : taille cible d'une image ; debit ~= fps * target_kb
DEFAULT_PROFILES: Dict[str, Dict[str, Any]] = {
    "perf":     {"label": "Performance maximale", "fps": 30, "q_min": 30, "q_max": 60, "target_kb": 8},
    "balanced": {"label": "Qualite equilibree",   "fps": 30, "q_min": 40, "q_max": 75, "target_kb": 11},
    "latency":  {"label": "Faible latence",       "fps": 40, "q_min": 30, "q_max": 55, "target_kb": 7},
    "compat":   {"label": "Compatibilite max",    "fps": 20, "q_min": 30, "q_max": 55, "target_kb": 6},
}

DEFAULTS: Dict[str, Any] = {
    # Nom affiche sur la 3DS ("" = nom de la machine)
    "name": "",
    # Code optionnel que la 3DS doit envoyer ("" = pas de code)
    "pin": "",
    "bind": "0.0.0.0",
    # Refuse les clients hors reseau local (adresses privees uniquement)
    "lan_only": True,
    "ports": {"control": PORT_CONTROL, "input": PORT_INPUT, "beacon": PORT_BEACON},
    "capture": {
        # gfn = capture de la fenetre GeForce NOW (dxcam, Windows)
        # testpattern = image de test ; video:<fichier> = lit une video (pour tester la 3DS)
        "source": "gfn",
        "window_title": "GeForce NOW",
        # Si plusieurs fenetres portent ce titre, on prefere celle dont l'exe contient ce texte
        "window_process": "geforcenow",
        # Ecran DXGI ou tourne GeForce NOW. La detection automatique de la fenetre ne marche que
        # sur l'ecran principal (0) ; ailleurs, l'ecran entier est capture (ou "region" ci-dessous).
        "monitor": 0,
        # [gauche, haut, droite, bas] pour forcer une zone, relative a l'ecran capture
        "region": None,
        # stretch = remplit 400x240 ; letterbox = bandes noires ; crop = recadre
        "fit": "stretch",
    },
    "games": {
        "folder": "games",
        "json": "games.json",
        # Retrouver tout seul les raccourcis crees par le client GeForce NOW : dossier du menu Demarrer
        # "NVIDIA Corporation\\Games" (tout est pris) et Bureau (seuls les .gfnpc sont pris).
        "auto_scan": True,
        # Dossiers supplementaires dont tous les raccourcis (.lnk .url .gfnpc) sont proposes
        "extra_dirs": [],
    },
    "gfn": {
        # Chemin de GeForceNOW.exe. Vide = recherche automatique dans les emplacements usuels
        # (utile pour l'entree "[ Ouvrir GeForce NOW ]" et pour les jeux de type "url_route")
        "exe": "",
        "auto_detect": True,
        # Dossier des journaux du client ("" = %LOCALAPPDATA%\\NVIDIA Corporation\\GeForceNOW)
        "log_dir": "",
        # Expressions regulieres (groupes nommes) appliquees aux nouvelles lignes des journaux.
        # Vides par defaut : le format reel est a calibrer avec probe_queue (voir README).
        "queue_regex": "",       # groupe (?P<pos>\d+)
        "eta_regex": "",         # groupe (?P<eta>\d+)
        "eta_unit": "min",       # min ou s
        "loading_regex": "",
        "streaming_regex": "",
        "ended_regex": "",       # session terminee -> retour a l'etat IDLE
        "error_regex": "",
    },
    "input": {
        "virtual_pad": True,
        # Zones mortes et amplitudes du Circle Pad / C-stick, en unites brutes de la 3DS.
        # Si le stick n'atteint jamais le maximum dans le jeu, baisse cpad_max / cstick_max.
        "deadzone_cpad": 12,
        "deadzone_cstick": 12,
        "cpad_max": 150,
        "cstick_max": 120,
        # label : A->A, B->B, X->X, Y->Y (la lettre sur la console = la lettre a l'ecran)
        # position : position physique (B, en bas sur la 3DS, devient le A de la Xbox, etc.)
        "face_layout": "label",
        # Touche 3DS -> commande Xbox (A B X Y LB RB LT RT START BACK GUIDE L3 R3 DPAD_UP/DOWN/
        # LEFT/RIGHT) ou "NONE". Seules les touches modifiees ont besoin d'etre listees.
        # La console envoie deja ses touches remappees (ecran "Touches" de l'appli) : laisse ce
        # tableau tel quel sauf besoin particulier cote PC.
        "mapping": {
            "A": "A", "B": "B", "X": "X", "Y": "Y",
            "L": "LB", "R": "RB", "ZL": "LT", "ZR": "RT",
            "START": "START", "SELECT": "BACK",
            "DUP": "DPAD_UP", "DDOWN": "DPAD_DOWN", "DLEFT": "DPAD_LEFT", "DRIGHT": "DPAD_RIGHT",
        },
    },
    # Adresses supplementaires pour les balises de decouverte (tests / reseaux particuliers)
    "beacon_targets": [],
    "default_profile": "balanced",
    "profiles": DEFAULT_PROFILES,
}


def deep_merge(base: Dict[str, Any], over: Dict[str, Any]) -> Dict[str, Any]:
    out = copy.deepcopy(base)
    for k, v in over.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict) and k != "profiles":
            out[k] = deep_merge(out[k], v)
        else:
            out[k] = copy.deepcopy(v)
    return out


def load_config(path: str) -> Dict[str, Any]:
    """Charge config.json (le cree avec les valeurs par defaut s'il n'existe pas)."""
    path = os.path.abspath(path)
    base_dir = os.path.dirname(path)
    if not os.path.exists(path):
        os.makedirs(base_dir, exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(DEFAULTS, f, indent=2, ensure_ascii=False)
        user: Dict[str, Any] = {}
    else:
        with open(path, "r", encoding="utf-8") as f:
            user = json.load(f)
    cfg = deep_merge(DEFAULTS, user)
    if not cfg["name"]:
        cfg["name"] = socket.gethostname()
    cfg["_base_dir"] = base_dir
    cfg["_path"] = path
    for pid, prof in cfg["profiles"].items():   # valeurs manquantes -> valeurs sures
        prof.setdefault("label", pid)
        prof.setdefault("fps", 30)
        prof.setdefault("q_min", 30)
        prof.setdefault("q_max", 70)
        prof.setdefault("target_kb", 10)
    if cfg["default_profile"] not in cfg["profiles"]:
        cfg["default_profile"] = next(iter(cfg["profiles"]))
    return cfg


def resolve(cfg: Dict[str, Any], rel: str) -> str:
    """Chemin relatif au dossier du config.json."""
    return rel if os.path.isabs(rel) else os.path.join(cfg["_base_dir"], rel)
