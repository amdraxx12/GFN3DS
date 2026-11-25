"""Point d'entree : python -m gfn3ds_relay [--check] [--list-windows] [--source ...]"""
from __future__ import annotations

import argparse
import logging
import os
import platform
import sys
import time

from . import __version__, winutil
from .config import load_config, resolve
from .encoder import FrameEncoder
from .games import HOME_ID, GameCatalog, effective_gfn_exe
from .gfn import GfnMonitor, default_log_dir
from .server import RelayServer, local_ipv4s


def banner(cfg, server: RelayServer) -> None:
    ips = local_ipv4s() or ["(adresse introuvable)"]
    print("=" * 60)
    print(" GFN 3DS - relais PC  v%s" % __version__)
    print("=" * 60)
    print(" Nom affiche sur la 3DS : %s" % cfg["name"])
    print(" Adresse(s) de ce PC    : %s" % ", ".join(ips))
    print(" Ports                  : TCP %d (controle), UDP %d (commandes)"
          % (server.control_port, server.input_port))
    print(" Code de connexion      : %s" % ("oui" if cfg["pin"] else "non (reseau local uniquement)"))
    print(" Source video           : %s" % (server.source_override or cfg["capture"]["source"]))
    print(" Jeux dans le catalogue : %d" % len(server.catalog.games()))
    print(" File d'attente         : %s" % (
        "suivie via les journaux GeForce NOW" if server.monitor.calibrated
        else "non calibree (la 3DS affichera l'ecran de GeForce NOW) - voir README"))
    print("-" * 60)
    if not effective_gfn_exe(cfg) and winutil.IS_WINDOWS:
        print(" Client GeForce NOW introuvable : installe-le (%s)" % winutil.GFN_DOWNLOAD_URL)
        print(" ou renseigne gfn.exe dans config.json s'il est installe ailleurs.")
    if not [g for g in server.catalog.games() if g.id != HOME_ID]:
        print(" Aucun jeu trouve. Dans le client GeForce NOW, ouvre un jeu puis")
        print(" 'Creer un raccourci' (Bureau) : il apparaitra sur la 3DS (bouton Actualiser).")
        print(" Autres moyens : dossier 'games' ou games.json (voir README).")
    print(" Ctrl+C pour arreter.\n")


def list_windows() -> int:
    if not winutil.IS_WINDOWS:
        print("Disponible sous Windows uniquement.")
        return 1
    winutil.set_dpi_aware()
    for hwnd, title, rect, exe in sorted(winutil.list_windows(), key=lambda w: w[1].lower()):
        print("%-24s %-28s %s  %s" % (title[:24], exe[:28], rect, "x".join(
            str(v) for v in (rect[2] - rect[0], rect[3] - rect[1]))))
    return 0


def run_check(cfg) -> int:
    """Teste chaque composant separement et dit clairement ce qui manque."""
    problems = 0
    print("== Diagnostic du relais GFN 3DS ==")
    print("Python %s sur %s" % (sys.version.split()[0], platform.platform()))
    needed_win = ("dxcam", "vgamepad")
    for mod in ("numpy", "cv2", "dxcam", "vgamepad"):
        try:
            m = __import__(mod)
            print("[ok] module %-9s %s" % (mod, getattr(m, "__version__", "")))
        except Exception as e:  # noqa: BLE001
            bad = mod not in needed_win or winutil.IS_WINDOWS
            problems += bad
            print("[%s] module %-9s %s" % ("!!" if bad else "--", mod, e))

    log_dir = cfg["gfn"]["log_dir"] or default_log_dir()
    if log_dir and os.path.isdir(log_dir):
        logs = sorted(f for f in os.listdir(log_dir) if f.lower().endswith(".log"))
        print("[ok] journaux GeForce NOW : %s (%d fichier(s) .log)" % (log_dir, len(logs)))
    else:
        problems += 1
        print("[!!] dossier des journaux GeForce NOW introuvable : %r "
              "(installe le client GeForce NOW ou renseigne gfn.log_dir)" % log_dir)

    exe = effective_gfn_exe(cfg)
    if exe:
        print("[ok] client GeForce NOW : %s" % exe)
    else:
        problems += winutil.IS_WINDOWS
        print("[%s] client GeForce NOW introuvable aux emplacements usuels : installe-le (%s) "
              "ou renseigne gfn.exe" % ("!!" if winutil.IS_WINDOWS else "--", winutil.GFN_DOWNLOAD_URL))
    cat = GameCatalog(cfg)
    real = [g for g in cat.games() if g.id != HOME_ID]
    print("[%s] jeux trouves : %d%s" % (
        "ok" if real else "--", len(real),
        "" if real else " (cree un raccourci de jeu dans le client GeForce NOW, voir README)"))
    for g in real[:15]:
        print("       - %s" % g.name)
    if len(real) > 15:
        print("       ... et %d autre(s)" % (len(real) - 15))

    if winutil.IS_WINDOWS:
        winutil.set_dpi_aware()
        w = winutil.find_window(cfg["capture"]["window_title"], cfg["capture"]["window_process"])
        if w:
            print("[ok] fenetre GeForce NOW trouvee : zone %s" % (w[1],))
        else:
            print("[--] fenetre '%s' introuvable (normal si le client n'est pas lance). "
                  "Voir --list-windows." % cfg["capture"]["window_title"])
        try:
            from .capture import make_source
            src = make_source(cfg, cfg["capture"]["source"])
            src.start()
            frame = None
            for _ in range(20):
                frame = src.read()
                if frame is not None:
                    break
                time.sleep(0.05)
            src.stop()
            if frame is None:
                problems += 1
                print("[!!] capture : aucune image recue")
            else:
                import cv2
                enc = FrameEncoder(cfg["capture"]["fit"])
                fitted = enc.fit_frame(frame)
                out = os.path.abspath("check_capture.jpg")
                cv2.imwrite(out, fitted)
                size = len(enc.encode_fitted(fitted, 50))
                print("[ok] capture : image %dx%d -> %s (%d octets en JPEG q50). "
                      "Ouvre ce fichier : c'est ce que verra la 3DS." % (
                          frame.shape[1], frame.shape[0], out, size))
        except Exception as e:  # noqa: BLE001
            problems += 1
            print("[!!] capture : %s" % e)
        from .gamepad import VirtualPad
        pad = VirtualPad(True)
        if pad.open():
            print("[ok] manette virtuelle Xbox 360 (ViGEmBus)")
            pad.close()
        else:
            problems += 1
            print("[!!] manette virtuelle indisponible : installe vgamepad (il installe ViGEmBus)")
    else:
        print("[--] tests de capture / manette : Windows uniquement")

    print("Adresses IP de ce PC : %s" % ", ".join(local_ipv4s() or ["?"]))
    print("Pare-feu Windows : autorise Python sur les reseaux PRIVES quand la fenetre s'affiche "
          "(TCP %d, UDP %d)." % (cfg["ports"]["control"], cfg["ports"]["input"]))
    print("\n%s" % ("Tout est pret." if not problems else "%d probleme(s) a corriger." % problems))
    return 1 if problems else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="gfn3ds_relay",
                                 description="Relais PC pour GeForce NOW sur Nintendo 3DS")
    ap.add_argument("--config", default="config.json", help="fichier de configuration (cree s'il manque)")
    ap.add_argument("--source", help="gfn | testpattern | video:<fichier>  (remplace la config)")
    ap.add_argument("--no-pad", action="store_true", help="n'emule pas de manette")
    ap.add_argument("--check", action="store_true", help="diagnostic des composants puis quitte")
    ap.add_argument("--list-windows", action="store_true", help="liste les fenetres visibles puis quitte")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s", datefmt="%H:%M:%S")
    if args.list_windows:
        return list_windows()
    cfg = load_config(args.config)
    if args.source:
        cfg["capture"]["source"] = args.source
    if args.check:
        return run_check(cfg)

    winutil.set_dpi_aware()
    winutil.set_timer_resolution(1)     # sans cela, Windows dort par pas de ~15 ms
    games_json = resolve(cfg, cfg["games"]["json"])
    if not os.path.exists(games_json):
        with open(games_json, "w", encoding="utf-8") as f:
            f.write("[]\n")
    os.makedirs(resolve(cfg, cfg["games"]["folder"]), exist_ok=True)

    server = RelayServer(cfg, source_override=args.source, use_pad=not args.no_pad,
                         monitor=GfnMonitor(cfg["gfn"]))
    try:
        server.start()
    except OSError as e:
        print("Impossible de demarrer le relais : %s\n(un autre relais tourne-t-il deja ? port deja pris ?)" % e)
        return 1
    banner(cfg, server)
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\nArret...")
    finally:
        server.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
