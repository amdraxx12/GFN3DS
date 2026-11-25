"""Tests du relais avec un faux client 3DS (aucune dependance Windows).

    cd relay
    python -m unittest discover -s tests -t . -v
"""
from __future__ import annotations

import json
import logging
import os
import socket
import sys
import tempfile
import time
import types
import unittest
from unittest import mock

import cv2
import numpy as np

from gfn3ds_relay import protocol as P
from gfn3ds_relay.capture import FrameSource, TestPatternSource
from gfn3ds_relay.config import load_config
from gfn3ds_relay.encoder import FrameEncoder, QualityController
from gfn3ds_relay.gamepad import map_input, stick
from gfn3ds_relay.games import Game
from gfn3ds_relay.gfn import GfnMonitor, LogTail
from gfn3ds_relay.protocol import State
from gfn3ds_relay.server import RateControl, RelayServer
from tests.fake3ds import Fake3DS, Reassembler

logging.disable(logging.CRITICAL)


def free_udp_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class StaticSource(FrameSource):
    name = "image fixe"

    def __init__(self):
        self.frame = np.full((240, 400, 3), (30, 90, 160), np.uint8)

    def read(self):
        return self.frame


class FakeXusbButton:
    """Memes noms et valeurs que vgamepad.XUSB_BUTTON (lus dans vgamepad/api.py)."""
    XUSB_GAMEPAD_DPAD_UP = 0x0001
    XUSB_GAMEPAD_DPAD_DOWN = 0x0002
    XUSB_GAMEPAD_DPAD_LEFT = 0x0004
    XUSB_GAMEPAD_DPAD_RIGHT = 0x0008
    XUSB_GAMEPAD_START = 0x0010
    XUSB_GAMEPAD_BACK = 0x0020
    XUSB_GAMEPAD_LEFT_THUMB = 0x0040
    XUSB_GAMEPAD_RIGHT_THUMB = 0x0080
    XUSB_GAMEPAD_LEFT_SHOULDER = 0x0100
    XUSB_GAMEPAD_RIGHT_SHOULDER = 0x0200
    XUSB_GAMEPAD_GUIDE = 0x0400
    XUSB_GAMEPAD_A = 0x1000
    XUSB_GAMEPAD_B = 0x2000
    XUSB_GAMEPAD_X = 0x4000
    XUSB_GAMEPAD_Y = 0x8000


class FakeX360:
    """Reproduit la signature de vgamepad.VX360Gamepad : un mauvais nom d'argument leve TypeError."""
    events: list = []

    def __init__(self):
        self.reset()
        FakeX360.events.append("plug")

    def reset(self):
        self.pressed, self.lt, self.rt, self.left, self.right = set(), 0, 0, (0, 0), (0, 0)

    def press_button(self, button):
        self.pressed.add(button)

    def release_button(self, button):
        self.pressed.discard(button)

    def left_trigger(self, value):
        self.lt = value

    def right_trigger(self, value):
        self.rt = value

    def left_joystick(self, x_value, y_value):
        self.left = (x_value, y_value)

    def right_joystick(self, x_value, y_value):
        self.right = (x_value, y_value)

    def update(self):
        pass

    @property
    def neutral(self):
        return not (self.pressed or self.lt or self.rt or self.left != (0, 0) or self.right != (0, 0))

    def __del__(self):          # vgamepad debranche la manette a la destruction de l'objet
        FakeX360.events.append("unplug")


def fake_vgamepad_module():
    mod = types.ModuleType("vgamepad")
    mod.XUSB_BUTTON = FakeXusbButton
    mod.VX360Gamepad = FakeX360
    return mod


class RelayCase(unittest.TestCase):
    """Un relais neuf par test, sur des ports ephemeres, avec une source de test."""

    pin = ""
    source = "testpattern"
    calibrated = True        # False : aucune expression de journal (etat par defaut d'une installation neuve)

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="gfn3ds-test-")
        self.logs = os.path.join(self.tmp, "logs")
        os.makedirs(self.logs)
        os.makedirs(os.path.join(self.tmp, "games"))
        self.launched = os.path.join(self.tmp, "launched.txt")
        with open(os.path.join(self.tmp, "games.json"), "w", encoding="utf-8") as f:
            json.dump([
                {"name": "Jeu Test", "type": "command",
                 "command": [sys.executable, "-c", "open(r'%s','w').write('ok')" % self.launched]},
                {"name": "Raccourci Casse", "type": "shortcut", "path": "inexistant.lnk"},
                {"name": "Desactive", "type": "command", "command": ["x"], "enabled": False},
                {"name": "Type invalide", "type": "magie"},
            ], f)
        open(os.path.join(self.tmp, "games", "Alpha Zero.lnk"), "w").close()
        cfg = load_config(os.path.join(self.tmp, "config.json"))
        cfg["ports"] = {"control": 0, "input": 0, "beacon": free_udp_port()}
        cfg["bind"] = "127.0.0.1"
        cfg["name"] = "Relais Test"
        cfg["pin"] = self.pin
        cfg["beacon_targets"] = ["127.0.0.1"]
        cfg["capture"]["source"] = "testpattern"
        cfg["gfn"].update(log_dir=self.logs, auto_detect=False)
        cfg["games"]["auto_scan"] = False     # ne jamais lire le vrai Bureau / menu Demarrer de la machine
        if self.calibrated:
            cfg["gfn"].update(
                queue_regex=r"queue position (?P<pos>\d+).*?(?P<eta>\d+) min",
                loading_regex=r"loading game",
                streaming_regex=r"stream started",
                ended_regex=r"session ended",
                error_regex=r"fatal error",
            )
        self.cfg = cfg
        src = self.source() if callable(self.source) else self.source
        self.server = RelayServer(cfg, source_override=src)
        self.server.start()
        self.clients = []

    def tearDown(self):
        for c in self.clients:
            c.close()
        self.server.stop()

    def client(self, device="new", hello=True, pin=""):
        c = Fake3DS(self.server.control_port, device)
        self.clients.append(c)
        if hello:
            reply = c.hello(pin)
            self.assertEqual(reply[0], "OK", reply)
        return c

    def append_log(self, line: str, name="debug.log"):
        with open(os.path.join(self.logs, name), "a", encoding="utf-8") as f:
            f.write(line + "\n")


class TestHandshake(RelayCase):
    def test_hello_ok_profiles_and_state(self):
        c = Fake3DS(self.server.control_port, "old")
        self.clients.append(c)
        reply = c.hello()
        self.assertEqual(reply[:3], ["OK", "1", "Relais Test"])
        self.assertEqual(int(reply[3]), self.server.input_port)
        head = c.wait("PROFILES")
        self.assertEqual(head[1], str(len(self.cfg["profiles"])))
        self.assertEqual(head[2], "balanced")
        ids = [c.wait("P")[1] for _ in range(int(head[1]))]
        self.assertEqual(ids, list(self.cfg["profiles"]))
        st = c.wait("STATE")
        self.assertEqual(st[1], State.IDLE)

    def test_bad_protocol_version(self):
        c = Fake3DS(self.server.control_port)
        self.clients.append(c)
        c.send("HELLO", 99, "new", "", c.video_port)
        self.assertEqual(c.wait("ERR")[1], "proto")

    def test_command_before_hello_is_refused(self):
        c = Fake3DS(self.server.control_port)
        self.clients.append(c)
        c.send("LIST")
        self.assertEqual(c.wait("ERR")[1], "proto")

    def test_ping_pong(self):
        c = self.client()
        c.send("PING", "42")
        self.assertEqual(c.wait("PONG"), ["PONG", "42"])

    def test_unknown_command(self):
        c = self.client()
        c.send("FORMAT_C")
        self.assertEqual(c.wait("ERR")[1], "unknown")

    def test_second_console_is_refused_then_accepted_after_first_leaves(self):
        a = self.client("new")
        b = Fake3DS(self.server.control_port, "old")      # une autre console (autre type)
        self.clients.append(b)
        self.assertEqual(b.hello()[:2], ["ERR", "busy"])
        a.send("BYE")
        time.sleep(0.5)
        c = Fake3DS(self.server.control_port, "old")
        self.clients.append(c)
        self.assertEqual(c.hello()[0], "OK")

    def test_same_console_takes_over_its_stale_session(self):
        # Coupure Wi-Fi : l'ancienne connexion TCP n'est pas fermee proprement cote relais.
        a = self.client("new")
        b = Fake3DS(self.server.control_port, "new")      # la meme console qui se reconnecte
        self.clients.append(b)
        self.assertEqual(b.hello()[0], "OK")
        self.assertIsNotNone(a.wait("__CLOSED__", timeout=3.0))   # l'ancienne session est fermee
        b.send("PING", "7")
        self.assertEqual(b.wait("PONG"), ["PONG", "7"])

    def test_beacon_announces_name_and_port(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", self.cfg["ports"]["beacon"]))
        s.settimeout(4.0)
        data, _ = s.recvfrom(256)
        s.close()
        f = P.split_line(data)
        self.assertEqual(f, ["GFN3DS", "1", "Relais Test", str(self.server.control_port)])


class TestPin(RelayCase):
    pin = "4242"

    def test_wrong_pin_refused(self):
        c = Fake3DS(self.server.control_port)
        self.clients.append(c)
        self.assertEqual(c.hello("0000")[:2], ["ERR", "auth"])

    def test_missing_pin_refused(self):
        c = Fake3DS(self.server.control_port)
        self.clients.append(c)
        self.assertEqual(c.hello("")[:2], ["ERR", "auth"])

    def test_right_pin_accepted(self):
        self.client(pin="4242")


class TestAutoGames(unittest.TestCase):
    """Detection automatique du client GeForce NOW et de ses raccourcis de jeux."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="gfn3ds-auto-")
        self.appdata = os.path.join(self.tmp, "AppData", "Roaming")
        self.local = os.path.join(self.tmp, "AppData", "Local")
        self.home = os.path.join(self.tmp, "User")
        self.start = os.path.join(self.appdata, "Microsoft", "Windows", "Start Menu", "Programs",
                                  "NVIDIA Corporation", "Games")
        self.desk = os.path.join(self.home, "Desktop")
        for d in (self.start, self.desk, os.path.join(self.tmp, "games")):
            os.makedirs(d)
        self.env = {"APPDATA": self.appdata, "LOCALAPPDATA": self.local, "USERPROFILE": self.home}

    def touch(self, *parts):
        p = os.path.join(*parts)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "w").close()
        return p

    def cfg(self, **games):
        cfg = load_config(os.path.join(self.tmp, "config.json"))
        cfg["games"].update(games)
        return cfg

    def names(self, cfg):
        from gfn3ds_relay.games import GameCatalog
        with mock.patch.dict(os.environ, self.env, clear=True):
            return [g.name for g in GameCatalog(cfg).games()]

    def test_finds_start_menu_shortcuts_and_desktop_gfnpc_only(self):
        self.touch(self.start, "Cyberpunk 2077 (GeForce NOW).lnk")
        self.touch(self.start, "Store", "Fortnite.url")          # sous-dossier : pris
        self.touch(self.desk, "Portal 2.gfnpc")
        self.touch(self.desk, "Chrome.lnk")                      # Bureau : pas un raccourci GFN
        self.touch(self.desk, "notes.txt")
        self.assertEqual(self.names(self.cfg()), ["Cyberpunk 2077", "Fortnite", "Portal 2"])

    def test_depth_limit_and_dedup_by_name(self):
        self.touch(self.start, "a", "b", "c", "Trop Profond.lnk")
        self.touch(self.start, "Portal 2.lnk")
        self.touch(self.desk, "Portal 2.gfnpc")                  # meme nom : un seul jeu
        self.touch(self.tmp, "games", "portal 2.lnk")            # le dossier games passe en premier
        cfg = self.cfg()
        self.assertEqual(self.names(cfg), ["portal 2"])

    def test_auto_scan_can_be_disabled_and_extra_dirs_work(self):
        self.touch(self.start, "Portal 2.lnk")
        extra = os.path.join(self.tmp, "mes jeux")
        self.touch(extra, "Halo.lnk")
        cfg = self.cfg(auto_scan=False, extra_dirs=[extra])
        self.assertEqual(self.names(cfg), ["Halo"])

    def test_gfn_exe_detected_and_home_entry_added(self):
        exe = self.touch(self.local, "NVIDIA Corporation", "GeForceNOW", "CEF", "GeForceNOW.exe")
        cfg = self.cfg()
        self.assertEqual(self.names(cfg), ["[ Ouvrir GeForce NOW ]"])
        from gfn3ds_relay.games import GameCatalog, effective_gfn_exe
        with mock.patch.dict(os.environ, self.env, clear=True):
            self.assertEqual(effective_gfn_exe(cfg), exe)
            self.assertEqual(GameCatalog(cfg).games()[0].argv, [exe])
            cfg["gfn"]["auto_detect"] = False
            self.assertEqual(effective_gfn_exe(cfg), "")
            cfg["gfn"]["exe"] = exe                              # chemin explicite : toujours respecte
            self.assertEqual(effective_gfn_exe(cfg), exe)

    def test_no_gfn_installed_means_no_home_entry(self):
        self.assertEqual(self.names(self.cfg()), [])

    def test_shortcut_name_cleanup(self):
        from gfn3ds_relay.games import shortcut_name
        self.assertEqual(shortcut_name("Fortnite - GeForce NOW"), "Fortnite")
        self.assertEqual(shortcut_name("Hades [GeForce NOW]"), "Hades")
        self.assertEqual(shortcut_name("GeForce NOW"), "GeForce NOW")   # jamais un nom vide
        self.assertEqual(shortcut_name("Pokémon Légendes"), "Pokemon Legendes")


class TestGames(RelayCase):
    def test_list_games(self):
        c = self.client()
        c.send("LIST")
        head = c.wait("GAMES")
        n = int(head[1])
        games = [c.wait("G") for _ in range(n)]
        names = [g[2] for g in games]
        # entrees invalides / desactivees absentes, raccourci du dossier present, tri alphabetique
        self.assertEqual(names, ["Alpha Zero", "Jeu Test", "Raccourci Casse"])
        self.assertEqual(games[0][1], "lnk-alpha-zero")

    def test_unknown_game(self):
        c = self.client()
        c.send("LAUNCH", "n-existe-pas")
        self.assertEqual(c.wait("ERR")[1], "launch")

    def test_launch_runs_command_and_follows_queue(self):
        c = self.client()
        # ancien contenu des journaux : doit etre ignore
        self.append_log("queue position 99, about 99 min")
        c.send("LAUNCH", "jeu-test")
        self.assertEqual(c.wait("OK", match=lambda f: f[1] == "LAUNCH")[1], "LAUNCH")
        st = c.wait("STATE", match=lambda f: f[1] == State.LAUNCHING)
        self.assertEqual(st[4], "Jeu Test")
        deadline = time.time() + 3
        while not os.path.exists(self.launched) and time.time() < deadline:
            time.sleep(0.05)
        self.assertTrue(os.path.exists(self.launched), "la commande du jeu n'a pas ete lancee")

        self.append_log("[12:00:01] session: queue position 12, estimated wait 7 min")
        st = c.wait("STATE", match=lambda f: f[1] == State.QUEUE, timeout=4)
        self.assertEqual((st[2], st[3]), ("12", "420"))
        self.append_log("[12:00:31] session: queue position 11, estimated wait 6 min")
        st = c.wait("STATE", match=lambda f: f[1] == State.QUEUE and f[2] == "11", timeout=4)
        self.assertEqual(st[3], "360")
        self.append_log("loading game assets", name="geronimo.log")
        self.assertIsNotNone(c.wait("STATE", match=lambda f: f[1] == State.LOADING, timeout=4))
        self.append_log("stream started")
        st = c.wait("STATE", match=lambda f: f[1] == State.STREAMING, timeout=4)
        self.assertEqual(st[2], "-1")
        self.append_log("session ended")
        self.assertIsNotNone(c.wait("STATE", match=lambda f: f[1] == State.IDLE, timeout=4))

    def test_error_line_reports_error_state(self):
        c = self.client()
        c.send("LAUNCH", "jeu-test")
        c.wait("OK", match=lambda f: f[1] == "LAUNCH")
        self.append_log("fatal error: code 12")
        st = c.wait("STATE", match=lambda f: f[1] == State.ERROR, timeout=4)
        self.assertIn("fatal error", st[5])

    def test_failed_launch_reports_error_state(self):
        c = self.client()
        c.send("LAUNCH", "raccourci-casse")
        err = c.wait("ERR")
        self.assertEqual(err[1], "launch")
        self.assertIsNotNone(c.wait("STATE", match=lambda f: f[1] == State.ERROR))

    def test_state_survives_a_reconnection(self):
        c = self.client()
        c.send("LAUNCH", "jeu-test")
        c.wait("OK", match=lambda f: f[1] == "LAUNCH")
        self.append_log("queue position 5, estimated wait 3 min")
        c.wait("STATE", match=lambda f: f[1] == State.QUEUE, timeout=4)
        c.close()                         # le Wi-Fi de la console coupe
        deadline = time.time() + 3
        while self.server.session is not None and time.time() < deadline:
            time.sleep(0.05)
        self.assertIsNone(self.server.session)
        d = Fake3DS(self.server.control_port)   # elle se reconnecte
        self.clients.append(d)
        self.assertEqual(d.hello()[0], "OK")
        st = d.wait("STATE")
        self.assertEqual((st[1], st[2], st[3]), (State.QUEUE, "5", "180"))


class TestUncalibrated(RelayCase):
    """Installation neuve : aucune expression de journal. La file n'est pas suivie, mais le flux part."""

    calibrated = False

    def test_launch_reports_untracked_and_streams_the_pc_screen(self):
        c = self.client()
        c.send("LAUNCH", "jeu-test")
        self.assertEqual(c.wait("OK", match=lambda f: f[1] == "LAUNCH")[1], "LAUNCH")
        st = c.wait("STATE", match=lambda f: f[1] == State.UNTRACKED)
        self.assertEqual(st[2:5], ["-1", "-1", "Jeu Test"])
        deadline = time.time() + 4
        while not c.frames and time.time() < deadline:
            time.sleep(0.05)
        self.assertTrue(c.frames, "la video doit partir des le lancement")
        # Sans expression, les journaux ne sont pas interpretes : l'etat ne change plus.
        self.append_log("queue position 3, estimated wait 2 min")
        self.append_log("stream started")
        time.sleep(1.0)
        later = [h[1] for h in c.all_lines("STATE")
                 if h[1] not in (State.IDLE, State.LAUNCHING, State.UNTRACKED)]
        self.assertEqual(later, [])

    def test_untracked_state_is_resent_after_a_reconnection(self):
        c = self.client()
        c.send("LAUNCH", "jeu-test")
        c.wait("STATE", match=lambda f: f[1] == State.UNTRACKED)
        c.close()
        time.sleep(0.3)
        c2 = self.client()
        self.assertIsNotNone(c2.wait("STATE", match=lambda f: f[1] == State.UNTRACKED))


class TestVideo(RelayCase):
    def watch(self, c):
        c.send("WATCH")
        self.assertIsNotNone(c.wait("OK", match=lambda f: f[1] == "WATCH"))

    def test_frames_arrive_at_a_decent_rate_with_correct_orientation(self):
        c = self.client()
        self.watch(c)
        time.sleep(1.5)
        n = len(c.frames)
        self.assertGreaterEqual(n, 15, "moins de 10 images/s recues sur la boucle locale")
        jpeg = c.frames[-1]
        self.assertLessEqual(len(jpeg), P.MAX_FRAME_BYTES)

        dec = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
        self.assertEqual(dec.shape, (400, 240, 3), "le JPEG doit faire 240 de large sur 400 de haut")

        # image tournee de 90 deg horaire : on la remet droite et on regarde les coins
        img = cv2.rotate(dec, cv2.ROTATE_90_COUNTERCLOCKWISE)
        self.assertEqual(img.shape, (240, 400, 3))

        def near(y, x, bgr):
            patch = img[y:y + 6, x:x + 6].reshape(-1, 3).mean(axis=0)
            return np.all(np.abs(patch - np.array(bgr)) < 45)

        self.assertTrue(near(4, 4, (0, 0, 255)), "haut-gauche doit etre rouge")
        self.assertTrue(near(4, 390, (0, 255, 0)), "haut-droit doit etre vert")
        self.assertTrue(near(230, 4, (255, 0, 0)), "bas-gauche doit etre bleu")
        self.assertTrue(near(230, 390, (0, 255, 255)), "bas-droit doit etre jaune")

    def test_jpeg_rows_are_framebuffer_columns(self):
        """Les 240 pixels d'une ligne du JPEG = une colonne du framebuffer 3DS (de bas en haut)."""
        c = self.client()
        self.watch(c)
        time.sleep(0.8)
        jpeg = c.frames[-1]
        dec = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
        fb = dec.reshape(-1, 3)                      # ce que la 3DS ecrit tel quel dans l'ecran
        # formule de libctru (console.c) : pixel (x, y) a l'index x*240 + (239 - y)
        screen = np.empty((240, 400, 3), np.uint8)
        for x in range(400):
            col = fb[x * 240:(x + 1) * 240]          # colonne x, indexee par m = 239 - y
            screen[:, x] = col[::-1]
        expected = TestPatternSource().read()
        expected = FrameEncoder("stretch").fit_frame(expected)
        diff = np.abs(screen.astype(int) - expected.astype(int)).mean()
        self.assertLess(diff, 14, "l'image affichee ne correspond pas a la source (diff=%.1f)" % diff)

    def test_stop_ends_video(self):
        c = self.client()
        self.watch(c)
        time.sleep(0.6)
        c.send("STOP")
        self.assertIsNotNone(c.wait("OK", match=lambda f: f[1] == "STOP"))
        time.sleep(0.3)
        n = len(c.frames)
        time.sleep(0.6)
        self.assertEqual(len(c.frames), n, "des images arrivent encore apres STOP")

    def test_stats_are_pushed(self):
        c = self.client()
        self.watch(c)
        st = c.wait("STATS", timeout=3)
        self.assertIsNotNone(st)
        self.assertEqual(len(st), 7)             # STATS fps kbps q profil signal manette
        self.assertEqual(st[4], "balanced")
        self.assertGreater(int(st[1]), 5)

    def test_profile_switch(self):
        c = self.client()
        self.watch(c)
        c.send("PROFILE", "latency")
        self.assertEqual(c.wait("OK", match=lambda f: f[1] == "PROFILE")[2], "latency")
        st = c.wait("STATS", match=lambda f: f[4] == "latency", timeout=4)
        self.assertIsNotNone(st)
        c.send("PROFILE", "nope")
        self.assertEqual(c.wait("ERR")[1], "profile")

    def test_survives_packet_loss(self):
        c = self.client()
        c.rx = Reassembler()
        c.drop_prob = 0.15
        self.watch(c)
        time.sleep(2.0)
        self.assertGreater(c.rx.ok, 5)
        self.assertGreater(c.rx.lost, 0, "avec 15 % de pertes, des images incompletes sont attendues")
        for j in c.frames:       # toute image livree est un JPEG complet et decodable
            self.assertIsNotNone(cv2.imdecode(np.frombuffer(j, np.uint8), cv2.IMREAD_COLOR))

    def test_loss_report_lowers_bitrate(self):
        c = self.client()
        self.watch(c)
        for _ in range(4):
            c.send("RX", 10, 10)
        time.sleep(0.4)
        self.assertLess(self.server.rate.scale, 0.7)


class TestStaticScreen(RelayCase):
    source = staticmethod(StaticSource)

    def test_identical_frames_are_not_resent_every_tick(self):
        c = self.client()
        c.send("WATCH")
        c.wait("OK", match=lambda f: f[1] == "WATCH")
        time.sleep(3.2)
        self.assertGreaterEqual(len(c.frames), 2)
        self.assertLessEqual(len(c.frames), 6, "image fixe : une image par seconde suffit")


class TestInput(RelayCase):
    def setUp(self):
        FakeX360.events.clear()
        self._patch = mock.patch.dict(sys.modules, {"vgamepad": fake_vgamepad_module()})
        self._patch.start()
        super().setUp()

    def tearDown(self):
        super().tearDown()
        self._patch.stop()

    def watch(self, c):
        c.send("WATCH")
        ok = c.wait("OK", match=lambda f: f[1] == "WATCH")
        self.assertIsNotNone(ok)
        self.assertEqual(ok[2], "1", "la manette virtuelle devrait etre branchee")
        return self.server.pad._pad          # le faux peripherique

    def last(self, timeout=1.0):
        end = time.time() + timeout
        while time.time() < end and not self.server.pad.history:
            time.sleep(0.01)
        return self.server.pad.history[-1] if self.server.pad.history else None

    def test_input_ignored_until_streaming(self):
        c = self.client()
        c.send_input(P.B_A)
        time.sleep(0.3)
        self.assertEqual(self.server.pad.history, [])

    def test_pad_is_plugged_on_watch_and_unplugged_on_stop(self):
        c = self.client()
        self.assertEqual(FakeX360.events, [])
        self.watch(c)
        self.assertEqual(FakeX360.events, ["plug"])
        c.send("STOP")
        c.wait("OK", match=lambda f: f[1] == "STOP")
        deadline = time.time() + 2
        while FakeX360.events != ["plug", "unplug"] and time.time() < deadline:
            time.sleep(0.02)
        self.assertEqual(FakeX360.events, ["plug", "unplug"])

    def test_buttons_and_sticks_reach_the_pad(self):
        c = self.client()
        self.watch(c)
        c.send_input(P.B_A | P.B_DUP | P.B_ZL | P.B_L3, cpad=(150, 0), cstick=(0, -120))
        st = self.last()
        self.assertEqual(st.buttons, frozenset({"A", "DPAD_UP", "L3"}))
        self.assertEqual(st.lt, 255)
        self.assertEqual((st.lx, st.ly), (32767, 0))
        self.assertEqual((st.rx, st.ry), (0, -32767))

    def test_real_vgamepad_calls_get_the_right_values(self):
        c = self.client()
        dev = self.watch(c)
        c.send_input(P.B_A | P.B_DUP | P.B_L | P.B_VBACK | P.B_ZR, cpad=(-150, 0), cstick=(0, 120))
        self.last()
        time.sleep(0.15)
        B = FakeXusbButton
        self.assertEqual(dev.pressed, {B.XUSB_GAMEPAD_A, B.XUSB_GAMEPAD_DPAD_UP,
                                       B.XUSB_GAMEPAD_LEFT_SHOULDER, B.XUSB_GAMEPAD_BACK})
        self.assertEqual((dev.lt, dev.rt), (0, 255))
        self.assertEqual(dev.left, (-32767, 0))
        self.assertEqual(dev.right, (0, 32767))
        c.send_input(P.B_B | P.B_R3)           # A, D-pad... relaches ; B et clic du stick droit
        time.sleep(0.15)
        self.assertEqual(dev.pressed, {B.XUSB_GAMEPAD_B, B.XUSB_GAMEPAD_RIGHT_THUMB})
        self.assertEqual((dev.lt, dev.rt, dev.left, dev.right), (0, 0, (0, 0), (0, 0)))

    def test_old_and_duplicate_sequence_numbers_are_ignored(self):
        c = self.client()
        self.watch(c)
        c.send_input(P.B_A, seq=10)
        self.assertEqual(self.last().buttons, frozenset({"A"}))
        n = len(self.server.pad.history)
        c.send_input(P.B_B, seq=9)       # plus ancien
        c.send_input(P.B_B, seq=10)      # doublon
        time.sleep(0.2)
        self.assertEqual(len(self.server.pad.history), n)
        c.send_input(P.B_B, seq=11)
        time.sleep(0.2)
        self.assertEqual(self.last().buttons, frozenset({"B"}))

    def test_sequence_wraparound(self):
        c = self.client()
        self.watch(c)
        c.send_input(P.B_A, seq=65535)
        time.sleep(0.15)
        c.send_input(P.B_B, seq=0)
        time.sleep(0.15)
        self.assertEqual(self.last().buttons, frozenset({"B"}))

    def test_watchdog_releases_everything_when_the_console_goes_silent(self):
        c = self.client()
        dev = self.watch(c)
        c.send_input(P.B_A, cpad=(150, 150))
        self.assertFalse(self.last().neutral)
        time.sleep(0.15)
        self.assertFalse(dev.neutral)
        time.sleep(0.7)
        self.assertTrue(dev.neutral, "une touche est restee appuyee")

    def test_stop_releases_the_pad_and_later_input_is_ignored(self):
        c = self.client()
        dev = self.watch(c)
        c.send_input(P.B_A)
        time.sleep(0.15)
        self.assertFalse(dev.neutral)
        c.send("STOP")
        c.wait("OK", match=lambda f: f[1] == "STOP")
        self.assertTrue(dev.neutral, "STOP doit relacher la manette")
        self.assertIsNone(self.server.pad._pad)
        n = len(self.server.pad.history)
        c.send_input(P.B_B)               # apres STOP : ignore
        time.sleep(0.5)
        self.assertEqual(len(self.server.pad.history), n)

    def test_disconnect_releases_the_pad_and_clears_the_session(self):
        c = self.client()
        dev = self.watch(c)
        c.send_input(P.B_A)
        time.sleep(0.15)
        self.assertFalse(dev.neutral)
        c.close()
        deadline = time.time() + 3
        while self.server.session is not None and time.time() < deadline:
            time.sleep(0.05)
        self.assertIsNone(self.server.session)
        self.assertTrue(dev.neutral)
        self.assertIsNone(self.server.pad._pad)


class TestUnits(unittest.TestCase):
    def test_protocol_roundtrip(self):
        pkt = P.InputPacket(1, 65535, P.B_A | P.B_GUIDE, -156, 156, 120, -120)
        self.assertEqual(P.InputPacket.unpack(pkt.pack()), pkt)
        self.assertEqual(len(pkt.pack()), 16)
        self.assertIsNone(P.InputPacket.unpack(b"x" * 16))
        self.assertIsNone(P.InputPacket.unpack(b"short"))

    def test_fragmentation_roundtrip_and_limits(self):
        data = bytes(range(256)) * 20
        frags = P.fragment_frame(7, data, 1234)
        self.assertTrue(all(len(f) <= 12 + P.FRAG_PAYLOAD for f in frags))
        rx = Reassembler()
        out = None
        for f in reversed(frags):                 # meme dans le desordre
            out = rx.feed(f) or out
        self.assertEqual(out, data)
        with self.assertRaises(ValueError):
            P.fragment_frame(0, b"", 0)
        with self.assertRaises(ValueError):
            P.fragment_frame(0, b"x" * (P.MAX_FRAME_BYTES + 1), 0)
        self.assertIsNone(P.parse_video_packet(b"\x00" * 20))

    def test_newer_frame_abandons_incomplete_one(self):
        rx = Reassembler()
        a = P.fragment_frame(1, b"a" * 3000, 0)
        b = P.fragment_frame(2, b"b" * 3000, 0)
        rx.feed(a[0])
        out = None
        for f in b:
            out = rx.feed(f) or out
        self.assertEqual(out, b"b" * 3000)
        self.assertEqual(rx.lost, 1)
        self.assertIsNone(rx.feed(a[1]))          # trainard : image ancienne ignoree

    def test_seq_newer(self):
        self.assertTrue(P.seq_newer(1, 0))
        self.assertTrue(P.seq_newer(0, 65535))
        self.assertFalse(P.seq_newer(0, 0))
        self.assertFalse(P.seq_newer(65535, 0))

    def test_ascii_clean_and_make_line(self):
        self.assertEqual(P.ascii_clean("Créature\tde\nl'été ❤"), "Creature de l'ete")
        self.assertEqual(P.make_line("A\tB", "C\nD"), b"A B\tC D\n")

    def test_stick_deadzone_and_scale(self):
        self.assertEqual(stick(5, 5, 150, 12), (0, 0))
        self.assertEqual(stick(150, 0, 150, 12), (32767, 0))
        self.assertEqual(stick(-300, 0, 150, 12), (-32767, 0))
        x, y = stick(100, 100, 150, 12)
        self.assertGreater(x, 0)
        self.assertEqual(x, y)

    def test_map_input_layout_and_custom_mapping(self):
        icfg = load_config(os.path.join(tempfile.mkdtemp(), "c.json"))["input"]
        pkt = P.InputPacket(0, 1, P.B_A | P.B_B | P.B_X | P.B_Y)
        self.assertEqual(map_input(pkt, icfg).buttons, frozenset({"A", "B", "X", "Y"}))
        pkt = P.InputPacket(0, 1, P.B_A)
        icfg["face_layout"] = "position"
        self.assertEqual(map_input(pkt, icfg).buttons, frozenset({"B"}))
        icfg["face_layout"] = "label"
        icfg["mapping"]["L"] = "NONE"
        icfg["mapping"]["R"] = "RT"
        st = map_input(P.InputPacket(0, 1, P.B_L | P.B_R | P.B_VLT | P.B_VSTART), icfg)
        self.assertEqual((st.lt, st.rt), (255, 255))
        self.assertEqual(st.buttons, frozenset({"START"}))

    def test_rate_control(self):
        r = RateControl()
        r.report(10, 0)
        self.assertEqual(r.scale, 1.0)
        for _ in range(30):
            r.report(5, 5)
        self.assertAlmostEqual(r.scale, 0.4)
        for _ in range(60):
            r.report(10, 0)
        self.assertEqual(r.scale, 1.0)
        r.report(0, 0)

    def test_quality_controller(self):
        q = QualityController(30, 70)
        start = q.q
        q.update(30000, 10000)
        self.assertLess(q.q, start)
        for _ in range(100):
            q.update(1000, 10000)
        self.assertEqual(q.q, 70)
        q.set_bounds(20, 40)
        self.assertEqual(q.q, 40)

    def test_encoder_fit_modes(self):
        wide = np.zeros((720, 1280, 3), np.uint8)
        for mode in ("stretch", "letterbox", "crop"):
            self.assertEqual(FrameEncoder(mode).fit_frame(wide).shape, (240, 400, 3))
        tall = np.zeros((600, 300, 4), np.uint8)
        self.assertEqual(FrameEncoder("letterbox").fit_frame(tall).shape, (240, 400, 3))
        with self.assertRaises(ValueError):
            FrameEncoder("zoom")


class TestLogTail(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.p = os.path.join(self.d, "debug.log")

    def write(self, data: bytes, mode="ab"):
        with open(self.p, mode) as f:
            f.write(data)

    def test_ignores_old_content_and_handles_partial_lines(self):
        self.write(b"old\n")
        t = LogTail(self.d)
        t.prime()
        self.write(b"new 1\nnew ")
        self.assertEqual(t.poll(), [("debug.log", "new 1")])
        self.write(b"2\n")
        self.assertEqual(t.poll(), [("debug.log", "new 2")])
        self.assertEqual(t.poll(), [])

    def test_truncation_and_new_file(self):
        self.write(b"a\nb\n")
        t = LogTail(self.d)
        t.prime()
        self.write(b"x\n", "wb")                     # tronque puis reecrit
        self.assertEqual(t.poll(), [("debug.log", "x")])
        with open(os.path.join(self.d, "other.log"), "wb") as f:
            f.write(b"fresh\n")
        self.assertEqual(t.poll(), [("other.log", "fresh")])

    def test_utf16_log_becomes_readable(self):
        t = LogTail(self.d)
        t.prime()
        self.write("queue position 5\r\n".encode("utf-16-le"))
        lines = [ln for _n, ln in t.poll()]
        self.assertEqual(lines, ["queue position 5"])

    def test_missing_dir(self):
        t = LogTail(os.path.join(self.d, "nope"))
        t.prime()
        self.assertEqual(t.poll(), [])


class TestMonitorRegex(unittest.TestCase):
    def make(self, **rx):
        cfg = {"log_dir": tempfile.mkdtemp(), "eta_unit": "min", **rx}
        m = GfnMonitor({"queue_regex": "", "eta_regex": "", "loading_regex": "",
                        "streaming_regex": "", "ended_regex": "", "error_regex": "", **cfg})
        m._set(state=State.LAUNCHING)
        return m

    def test_uncalibrated_monitor_stays_launching(self):
        m = self.make()
        self.assertFalse(m.calibrated)
        m.feed("queue position 3")
        self.assertEqual(m.status.state, State.LAUNCHING)

    def test_uncalibrated_launch_becomes_untracked(self):
        m = self.make()
        ok, _ = m.launch(Game("t", "Test", "command", argv=[sys.executable, "-c", "pass"]))
        self.assertTrue(ok)
        self.assertEqual((m.status.state, m.status.game), (State.UNTRACKED, "Test"))
        self.assertEqual((m.status.queue_pos, m.status.eta_s), (-1, -1))

    def test_calibrated_launch_stays_launching_until_a_line_matches(self):
        m = self.make(queue_regex=r"pos=(?P<pos>\d+)")
        ok, _ = m.launch(Game("t", "Test", "command", argv=[sys.executable, "-c", "pass"]))
        self.assertTrue(ok)
        self.assertEqual(m.status.state, State.LAUNCHING)
        m.feed("pos=9")
        self.assertEqual((m.status.state, m.status.queue_pos), (State.QUEUE, 9))

    def test_failed_launch_is_an_error_even_when_uncalibrated(self):
        m = self.make()
        ok, msg = m.launch(Game("t", "Test", "shortcut", path="/nulle/part/inexistant.lnk"))
        self.assertFalse(ok)
        self.assertEqual(m.status.state, State.ERROR)
        self.assertIn("Lancement impossible", msg)

    def test_separate_eta_regex_and_seconds_unit(self):
        m = self.make(queue_regex=r"pos=(?P<pos>\d+)", eta_regex=r"eta=(?P<eta>\d+)s", eta_unit="s")
        m.feed("pos=4")
        m.feed("eta=95s")
        s = m.status
        self.assertEqual((s.state, s.queue_pos, s.eta_s), (State.QUEUE, 4, 95))

    def test_invalid_regex_is_ignored(self):
        m = self.make(queue_regex="(unclosed")
        self.assertFalse(m.calibrated)

    def test_idle_monitor_ignores_lines(self):
        m = self.make(streaming_regex="go")
        m.reset()
        m.feed("go")
        self.assertEqual(m.status.state, State.IDLE)


if __name__ == "__main__":
    unittest.main()
