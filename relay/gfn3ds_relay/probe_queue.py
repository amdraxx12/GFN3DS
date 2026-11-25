"""Outil de calibration de la file d'attente.

NVIDIA ne documente pas le format des lignes de journal qui annoncent la position dans la file.
Cet outil enregistre les nouvelles lignes des journaux du client GeForce NOW pendant que tu lances
un jeu, pour qu'on en tire les expressions regulieres de config.json (section "gfn").

    python -m gfn3ds_relay.probe_queue
    -> lance un jeu dans GeForce NOW (idealement a une heure ou il y a une file d'attente)
    -> Ctrl+C une fois en jeu ; le fichier probe_<date>.txt contient TOUTES les nouvelles lignes
"""
from __future__ import annotations

import argparse
import re
import sys
import time
from datetime import datetime

from .gfn import LogTail, default_log_dir

# Mots-cles affiches a l'ecran (le fichier, lui, garde toutes les lignes)
DEFAULT_WORDS = ("queue", "wait", "position", "seat", "rig", "eta", "minute", "ready", "session",
                 "stream", "launch", "error", "fail", "start", "load")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="probe_queue", description=__doc__.split("\n\n")[0])
    ap.add_argument("--dir", default="", help="dossier des journaux (defaut : %%LOCALAPPDATA%%\\NVIDIA Corporation\\GeForceNOW)")
    ap.add_argument("--grep", nargs="*", default=list(DEFAULT_WORDS),
                    help="mots affiches a l'ecran (le fichier garde tout)")
    ap.add_argument("--out", default="", help="fichier de sortie (defaut : probe_<date>.txt)")
    args = ap.parse_args(argv)

    log_dir = args.dir or default_log_dir()
    tail = LogTail(log_dir)
    if not tail._files():
        print("Aucun fichier .log dans %r. Installe/lance GeForce NOW, ou donne --dir." % log_dir)
        return 1
    out = args.out or "probe_%s.txt" % datetime.now().strftime("%Y%m%d_%H%M%S")
    words = re.compile("|".join(re.escape(w) for w in args.grep), re.IGNORECASE) if args.grep else None
    tail.prime()
    print("Surveillance de %s" % log_dir)
    print("Enregistrement dans %s - lance maintenant un jeu dans GeForce NOW. Ctrl+C pour finir.\n" % out)
    n = 0
    try:
        with open(out, "w", encoding="utf-8") as f:
            while True:
                for name, line in tail.poll():
                    stamp = datetime.now().strftime("%H:%M:%S")
                    f.write("[%s] %s | %s\n" % (stamp, name, line))
                    n += 1
                    if words is None or words.search(line):
                        print("[%s] %s | %s" % (stamp, name, line[:200]))
                f.flush()
                time.sleep(0.3)
    except KeyboardInterrupt:
        print("\n%d ligne(s) enregistree(s) dans %s - envoie-moi ce fichier." % (n, out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
