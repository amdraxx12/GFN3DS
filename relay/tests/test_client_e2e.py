"""Test de bout en bout : le VRAI code du client 3DS (compile sur PC) contre le VRAI relais.

Le client est ici le programme client3ds/tests/host/headless.cpp : il contient GfnClient, VideoDecoder, UI
et toute la logique reseau de la console ; seule la couche plate-forme (libctru) est remplacee par des
threads standard. Le relais est le vrai relais Python, avec une source video de test et une fausse manette.

    client3ds/tests/host/build.sh                 # compile le client sans ecran
    cd relay && python3 -I -m unittest tests.test_client_e2e -v

Le test est ignore si le programme n'a pas ete compile (variable GFN3DS_HEADLESS pour changer son chemin).
Les captures d'ecran (PPM) de chaque etape vont dans GFN3DS_SHOTS (par defaut : un dossier temporaire).
"""
from __future__ import annotations

import os
import select
import subprocess
import tempfile
import time
import sys
import unittest
from unittest import mock

from gfn3ds_relay import protocol as P
from tests.test_relay import RelayCase, FakeX360, fake_vgamepad_module

HERE = os.path.dirname(os.path.abspath(__file__))
BINARY = os.environ.get("GFN3DS_HEADLESS") or os.path.join(
    HERE, "..", "..", "client3ds", "tests", "host", "out", "headless")

# Boutons de la console (bits de input::Raw::held)
A, B, X, Y, L, R, ZL, ZR = (1 << i for i in range(8))
DUP, DDOWN, DLEFT, DRIGHT = 1 << 8, 1 << 9, 1 << 10, 1 << 11
START, SELECT = 1 << 12, 1 << 13


class Headless:
    """Pilote le client sans ecran par son entree standard."""

    def __init__(self, cwd: str):
        self.p = subprocess.Popen([BINARY], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, cwd=cwd, bufsize=0)
        self.buf = b""
        first = self.readline(15)
        if first != "READY":
            raise RuntimeError("client sans ecran : %r" % first)

    def readline(self, timeout: float = 20.0) -> str:
        end = time.time() + timeout
        while b"\n" not in self.buf:
            left = end - time.time()
            if left <= 0:
                raise TimeoutError("le client sans ecran ne repond plus")
            ready, _, _ = select.select([self.p.stdout], [], [], left)
            if ready:
                chunk = os.read(self.p.stdout.fileno(), 4096)
                if not chunk:
                    raise EOFError("le client sans ecran s'est arrete : %r" % self.buf)
                self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode("utf-8", "replace").strip()

    def cmd(self, text: str, timeout: float = 20.0) -> str:
        self.p.stdin.write((text + "\n").encode())
        self.p.stdin.flush()
        return self.readline(timeout)

    def ok(self, text: str) -> str:
        reply = self.cmd(text)
        assert reply.startswith("OK"), "%s -> %s" % (text, reply)
        return reply

    def status(self) -> dict:
        d = {}
        for tok in self.ok("status").split()[1:]:
            k, _, v = tok.partition("=")
            d[k] = v
        return d

    def run(self, ms: int) -> None:
        self.ok("run %d" % ms)

    def key(self, bits: int, ms: int = 100) -> None:
        """Appui bref sur des boutons de la console."""
        self.ok("press %x" % bits)
        self.run(ms)
        self.ok("press 0")
        self.run(ms)

    def wait(self, pred, what: str, timeout: float = 10.0, step: int = 100) -> dict:
        end = time.time() + timeout
        st = {}
        while time.time() < end:
            self.run(step)
            st = self.status()
            if pred(st):
                return st
        raise AssertionError("delai depasse : %s\ndernier etat du client : %s" % (what, st))

    def close(self) -> None:
        try:
            self.p.stdin.write(b"quit\n")
            self.p.stdin.flush()
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()


def rgb565(hexstr: str):
    v = int(hexstr, 16)
    return (v >> 11) & 31, (v >> 5) & 63, v & 31


@unittest.skipUnless(os.path.exists(BINARY), "client sans ecran non compile (client3ds/tests/host/build.sh)")
class ClientCase(RelayCase):
    def setUp(self):
        FakeX360.events.clear()
        self._patch = mock.patch.dict(sys.modules, {"vgamepad": fake_vgamepad_module()})
        self._patch.start()
        super().setUp()
        self.cwd = tempfile.mkdtemp(prefix="gfn3ds-client-")
        self.shots = os.environ.get("GFN3DS_SHOTS") or os.path.join(self.cwd, "shots")
        os.makedirs(self.shots, exist_ok=True)
        self.h = Headless(self.cwd)

    def tearDown(self):
        self.h.close()
        super().tearDown()
        self._patch.stop()

    def connect(self, pin="-"):
        self.h.ok("connect 127.0.0.1 %d %s 1" % (self.server.control_port, pin))
        return self.h.wait(lambda s: s["link"] == "online" and int(s["games"]) >= 1, "connexion au relais")

    def shot(self, name: str) -> None:
        self.h.ok("shot %s" % os.path.join(self.shots, name))


class TestClientBasics(ClientCase):
    def test_connect_games_profile_and_latency(self):
        st = self.connect()
        self.assertEqual(st["relay"], "Relais_Test")
        self.assertEqual(st["state"], "idle")
        self.assertEqual(st["profile"], "balanced")
        self.assertEqual(self.h.ok("games"),
                         "OK 3 lnk-alpha-zero=Alpha_Zero jeu-test=Jeu_Test raccourci-casse=Raccourci_Casse")
        st = self.h.wait(lambda s: int(s["rtt"]) >= 0, "mesure de latence (PING)", timeout=5)
        self.assertLess(int(st["rtt"]), 500)
        self.shot("01_games_online")

    def test_profile_change_is_confirmed_by_the_relay(self):
        self.connect()
        other = [p for p in self.cfg["profiles"] if p != "balanced"][0]
        self.h.ok("profile %s" % other)
        self.h.wait(lambda s: s["profile"] == other, "profil %s applique" % other, timeout=5)

    def test_unknown_game_gives_a_toast_not_a_crash(self):
        self.connect()
        self.h.ok("launch n-existe-pas")
        st = self.h.wait(lambda s: s["toast"] != "-", "message d'erreur du relais", timeout=5)
        self.assertEqual(st["link"], "online")
        self.assertEqual(st["stream_on"], "0")


class TestClientPin(ClientCase):
    pin = "1234"

    def test_wrong_pin_is_fatal_then_right_pin_connects(self):
        self.h.ok("connect 127.0.0.1 %d 9999 1" % self.server.control_port)
        st = self.h.wait(lambda s: s["fatal"] == "1", "refus du code PIN", timeout=6)
        self.assertEqual(st["link"], "offline")          # pas de boucle de reconnexion avec un mauvais code
        self.shot("02_wrong_pin")
        self.connect(pin="1234")


class TestClientDiscovery(ClientCase):
    def setUp(self):
        self._beacon = mock.patch("tests.test_relay.free_udp_port", return_value=P.PORT_BEACON)
        self._beacon.start()
        try:
            super().setUp()
        except OSError as e:                              # port de balise deja pris sur cette machine
            self._beacon.stop()
            self.skipTest("port de balise indisponible : %s" % e)

    def tearDown(self):
        super().tearDown()
        self._beacon.stop()

    def test_relay_found_by_beacon_and_chosen_with_the_buttons(self):
        st = self.h.wait(lambda s: int(s["relays"]) >= 1, "balise du relais", timeout=8)
        self.assertEqual(st["link"], "offline")
        self.shot("03_relay_discovered")
        # Selon la machine, la balise peut aussi arriver par une autre adresse (diffusion) : on choisit
        # l'entree de 127.0.0.1, la seule a laquelle ce relais de test est joignable.
        self.h.wait(lambda s: "127.0.0.1:" in self.h.ok("relays"), "balise de 127.0.0.1", timeout=6)
        self.h.run(2500)                                   # une periode de balise de plus : liste stable
        entries = self.h.ok("relays").split()[2:]
        idx = next(i for i, e in enumerate(entries) if e.startswith("127.0.0.1:"))
        # Liste : [Se connecter a 127.0.0.1] puis les relais detectes. Bas (idx + 1 fois), puis A.
        for _ in range(idx + 1):
            self.h.key(DDOWN)
        self.h.key(A)
        st = self.h.wait(lambda s: s["link"] == "online", "connexion depuis la liste des relais", timeout=8)
        self.assertEqual(st["relay"], "Relais_Test")


class TestClientSession(ClientCase):
    def test_launch_queue_stream_input_and_reconnect(self):
        self.connect()

        # --- choisir et lancer "Jeu Test" avec les boutons : [Voir l'ecran] puis Alpha Zero, Jeu Test
        self.h.key(DDOWN)
        self.h.key(DDOWN)
        self.shot("04_game_selected")
        self.h.key(A)
        st = self.h.wait(lambda s: s["state"] == "launching", "lancement du jeu", timeout=6)
        self.assertEqual(st["game"], "Jeu_Test")
        self.assertEqual(st["stream_on"], "1")
        deadline = time.time() + 3
        while not os.path.exists(self.launched) and time.time() < deadline:
            time.sleep(0.05)
        self.assertTrue(os.path.exists(self.launched), "la commande du jeu n'a pas ete lancee par le relais")

        # --- file d'attente, suivie dans les journaux de GeForce NOW
        self.append_log("[12:00:01] session: queue position 12, estimated wait 7 min")
        st = self.h.wait(lambda s: s["state"] == "queue", "file d'attente", timeout=6)
        self.assertEqual((st["queue"], st["eta"]), ("12", "420"))
        self.assertEqual(st["page"], "games")
        self.shot("05_queue")
        # Tant que l'ecran de file d'attente est affiche, la video n'est pas decodee (economie de batterie).
        d0 = int(self.h.status()["decoded"])
        self.h.run(800)
        self.assertLessEqual(int(self.h.status()["decoded"]) - d0, 1,
                             "la video est decodee alors qu'elle n'est pas affichee")
        self.append_log("[12:00:31] session: queue position 11, estimated wait 6 min")
        st = self.h.wait(lambda s: s["queue"] == "11", "position 11", timeout=6)
        self.assertEqual(st["eta"], "360")
        self.append_log("loading game assets", name="geronimo.log")
        self.h.wait(lambda s: s["state"] == "loading", "chargement", timeout=6)
        self.shot("06_loading")

        # --- le flux demarre : la manette tactile s'ouvre toute seule, la video arrive
        self.append_log("stream started")
        st = self.h.wait(lambda s: s["state"] == "streaming" and s["page"] == "pad" and int(s["decoded"]) >= 5,
                         "flux video", timeout=8)
        self.assertEqual(st["view"], "1")
        frame = self.h.ok("frame").split()
        corners = {k: rgb565(v) for k, v in (t.split("=") for t in frame[1:])}
        tl, tr, bl, br = corners["TL"], corners["TR"], corners["BL"], corners["BR"]
        # motif de test du relais : rouge, vert, bleu, jaune (orientation exacte de l'image affichee)
        self.assertTrue(tl[0] >= 24 and tl[1] <= 16 and tl[2] <= 6, "haut-gauche devrait etre rouge : %s" % (tl,))
        self.assertTrue(tr[1] >= 48 and tr[0] <= 6 and tr[2] <= 6, "haut-droite devrait etre vert : %s" % (tr,))
        self.assertTrue(bl[2] >= 24 and bl[0] <= 6 and bl[1] <= 16, "bas-gauche devrait etre bleu : %s" % (bl,))
        self.assertTrue(br[0] >= 24 and br[1] >= 48 and br[2] <= 8, "bas-droite devrait etre jaune : %s" % (br,))
        self.shot("07_streaming")

        # --- les boutons de la console atteignent la manette virtuelle (preset Standard)
        self.h.ok("press %x" % (A | DUP))
        self.h.run(300)
        self.assertEqual(self.server.pad.history[-1].buttons, frozenset({"A", "DPAD_UP"}))
        self.h.ok("cpad 150 0")
        self.h.run(300)
        self.assertEqual(self.server.pad.history[-1].lx, 32767)
        self.h.ok("cpad 0 0")
        self.h.ok("press 0")
        self.h.run(200)
        self.assertTrue(self.server.pad.history[-1].buttons == frozenset())

        # --- Start ouvre le menu du streaming et ne part JAMAIS vers le jeu
        n = len(self.server.pad.history)
        self.h.key(START)
        st = self.h.status()
        self.assertEqual(st["page"], "streammenu")
        self.shot("08_stream_menu")
        self.assertNotIn("START", set().union(*(h.buttons for h in self.server.pad.history[n:])))
        # menu ouvert : les boutons ne sont plus envoyes au jeu
        self.h.ok("press %x" % A)           # A = "Reprendre"
        self.h.run(100)
        self.h.ok("press 0")
        self.h.run(200)
        self.assertEqual(self.h.status()["page"], "pad")
        # le bouton A qui a valide "Reprendre" ne doit pas etre rejoue dans le jeu
        self.assertNotIn("A", set().union(*(h.buttons for h in self.server.pad.history[n:])))

        # --- stick droit tactile
        self.h.ok("touch 200 172")          # 40 px a droite du centre de la zone
        self.h.run(300)
        self.assertGreater(self.server.pad.history[-1].rx, 10000)
        self.shot("09_touch_stick")
        self.h.ok("release")
        self.h.run(200)
        self.assertEqual(self.server.pad.history[-1].rx, 0)

        # --- coupure du Wi-Fi : le relais perd la console, elle se reconnecte et redemande le flux
        before = int(self.h.status()["decoded"])
        self.server._end_session(self.server.session)
        st = self.h.wait(lambda s: s["link"] in ("reconnecting", "offline", "connecting"), "detection de la coupure",
                         timeout=6)
        self.shot("10_link_lost")
        st = self.h.wait(lambda s: s["link"] == "online", "reconnexion automatique", timeout=10)
        self.assertEqual(st["state"], "streaming")
        self.assertEqual(st["stream_on"], "1")
        self.h.wait(lambda s: int(s["decoded"]) >= before + 5, "reprise de la video apres reconnexion", timeout=8)
        self.assertEqual(self.h.status()["page"], "pad")

        # --- fin de session
        self.append_log("session ended")
        st = self.h.wait(lambda s: s["state"] == "idle", "fin de session", timeout=6)
        self.assertEqual(st["page"], "games")
        self.shot("11_after_session")

    def test_stop_stream_keeps_the_game_running_and_can_resume(self):
        self.connect()
        self.h.ok("launch jeu-test")
        self.h.wait(lambda s: s["state"] == "launching", "lancement", timeout=6)
        self.append_log("stream started")
        self.h.wait(lambda s: s["state"] == "streaming" and s["page"] == "pad", "flux", timeout=8)
        self.h.ok("stop")
        st = self.h.wait(lambda s: s["stream_on"] == "0", "arret du flux", timeout=5)
        self.assertEqual(st["state"], "streaming")        # le jeu continue sur le PC
        self.assertEqual(st["page"], "games")             # la vue video se ferme : retour a la liste (bouton "Reprendre")
        self.h.ok("watch")
        self.h.wait(lambda s: s["stream_on"] == "1" and int(s["decoded"]) >= 3, "reprise du flux", timeout=8)


class TestClientUncalibrated(ClientCase):
    """Installation neuve : le relais ne sait pas suivre la file. La console montre l'ecran du PC, manette comprise."""

    calibrated = False

    def test_launch_shows_the_pc_screen_and_the_pad_works(self):
        self.connect()
        self.h.ok("launch jeu-test")
        st = self.h.wait(lambda s: s["state"] == "untracked" and s["page"] == "pad" and int(s["decoded"]) >= 5,
                         "ecran du PC affiche sans calibration", timeout=8)
        self.assertEqual((st["view"], st["stream_on"]), ("1", "1"))
        self.assertEqual(st["state"], "untracked")
        self.shot("12_untracked_pc_screen")

        # La manette marche meme si le relais ne dit pas "streaming" : c'est la console qui a demande le flux.
        self.h.ok("press %x" % (A | DUP))
        self.h.run(300)
        self.assertEqual(self.server.pad.history[-1].buttons, frozenset({"A", "DPAD_UP"}))
        self.h.ok("press 0")
        self.h.run(200)

        # Arreter le flux : la partie continue sur le PC, la liste propose de reprendre.
        self.h.ok("stop")
        st = self.h.wait(lambda s: s["stream_on"] == "0" and s["page"] == "games", "arret du flux", timeout=5)
        self.assertEqual(st["state"], "untracked")
        self.shot("13_untracked_stopped")
        d0 = int(self.h.status()["decoded"])
        self.h.run(600)
        self.assertLessEqual(int(self.h.status()["decoded"]) - d0, 1)     # video coupee : rien n'est decode
        self.h.ok("watch")
        self.h.wait(lambda s: s["page"] == "pad" and int(s["decoded"]) >= 3, "reprise du flux", timeout=8)

    def test_pc_screen_survives_a_wifi_drop(self):
        self.connect()
        self.h.ok("launch jeu-test")
        self.h.wait(lambda s: s["state"] == "untracked" and s["page"] == "pad" and int(s["decoded"]) >= 3,
                    "ecran du PC", timeout=8)
        self.server._end_session(self.server.session)
        self.h.wait(lambda s: s["link"] != "online", "detection de la coupure", timeout=6)
        st = self.h.wait(lambda s: s["link"] == "online", "reconnexion", timeout=10)
        self.assertEqual((st["state"], st["stream_on"]), ("untracked", "1"))
        self.h.wait(lambda s: s["page"] == "pad" and int(s["decoded"]) >= 3, "video apres reconnexion", timeout=8)
        self.h.ok("press %x" % B)
        self.h.run(300)
        self.assertEqual(self.server.pad.history[-1].buttons, frozenset({"B"}))
        self.h.ok("press 0")


if __name__ == "__main__":
    unittest.main()
