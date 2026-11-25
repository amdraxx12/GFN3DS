"""Serveur relais : controle TCP, video UDP, entrees UDP et balises de decouverte.

Un seul client (une console) a la fois. Tout ce que le client peut declencher passe par une
liste fermee de commandes ; il ne peut jamais envoyer une commande a executer sur le PC.
"""
from __future__ import annotations

import hmac
import ipaddress
import logging
import socket
import threading
import time
import zlib
from typing import Any, Dict, List, Optional

import cv2
import numpy as np

from . import protocol as P
from . import winutil
from .capture import FrameSource, make_source
from .encoder import FrameEncoder, QualityController
from .gamepad import VirtualPad, map_input
from .games import GameCatalog
from .gfn import GfnMonitor, Status
from .protocol import ascii_clean, make_line, split_line

log = logging.getLogger("server")

SESSION_TIMEOUT = 15.0      # silence TCP au bout duquel la console est consideree perdue
BUSY_TIMEOUT = 10.0         # une session silencieuse depuis ce delai peut etre remplacee
INPUT_WATCHDOG = 0.3        # sans paquet d'entrees pendant ce delai, la manette est remise a zero
KEEPALIVE_FRAME_S = 1.0     # image identique renvoyee au moins toutes les secondes
SOURCE_RETRY_S = 2.0


# ------------------------------------------------------------------ reseau
def is_private(ip: str) -> bool:
    try:
        a = ipaddress.ip_address(ip)
    except ValueError:
        return False
    return a.is_private or a.is_loopback or a.is_link_local


def local_ipv4s() -> List[str]:
    ips = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except OSError:
        pass
    try:  # adresse de l'interface de la route par defaut (aucun paquet n'est envoye)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        ips.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    return sorted(ip for ip in ips if not ip.startswith(("127.", "169.254.", "0.")))


def beacon_targets(extra: List[str]) -> List[str]:
    t = ["255.255.255.255"]
    for ip in local_ipv4s():
        a, b, c, _d = ip.split(".")
        t.append("%s.%s.%s.255" % (a, b, c))   # hypothese /24 : cas de la plupart des box
    t.extend(extra)
    return list(dict.fromkeys(t))


class Session:
    """Une console connectee."""

    def __init__(self, sock: socket.socket, ip: str, device: str, video_port: int):
        self.sock = sock
        self.ip = ip
        self.device = device
        self.video_port = video_port
        self.streaming = False
        self.alive = True
        self.last_rx = time.monotonic()
        self.last_seq: Optional[int] = None
        self._send_lock = threading.Lock()

    def send(self, *fields) -> bool:
        if not self.alive:
            return False
        try:
            with self._send_lock:
                self.sock.sendall(make_line(*fields))
            return True
        except OSError:
            self.alive = False
            return False


class RateControl:
    """Reduit le debit quand la 3DS signale des images perdues, le remonte quand tout va bien."""

    def __init__(self) -> None:
        self.scale = 1.0
        self.good = 0

    def report(self, ok: int, lost: int) -> None:
        total = ok + lost
        if total <= 0:
            return
        if lost / total > 0.08:
            self.scale = max(0.4, self.scale * 0.85)
            self.good = 0
        else:
            self.good += 1
            if self.good >= 3:
                self.scale = min(1.0, self.scale + 0.05)
                self.good = 0


def nosignal_frame() -> np.ndarray:
    f = np.zeros((P.SCREEN_H, P.SCREEN_W, 3), np.uint8)
    f[:] = (40, 30, 20)
    cv2.putText(f, "PAS DE SIGNAL", (70, 115), cv2.FONT_HERSHEY_SIMPLEX, 1.0, (255, 255, 255), 2,
                cv2.LINE_AA)
    cv2.putText(f, "Fenetre GeForce NOW introuvable", (45, 155), cv2.FONT_HERSHEY_SIMPLEX, 0.6,
                (200, 200, 200), 1, cv2.LINE_AA)
    return f


class RelayServer:
    def __init__(self, cfg: Dict[str, Any], source_override: Any = None,
                 use_pad: bool = True, monitor: Optional[GfnMonitor] = None,
                 pad: Optional[VirtualPad] = None):
        self.cfg = cfg
        self.source_override = source_override
        self.catalog = GameCatalog(cfg)
        self.monitor = monitor or GfnMonitor(cfg["gfn"])
        self.pad = pad or VirtualPad(enabled=use_pad and cfg["input"].get("virtual_pad", True))
        self.profile_id: str = cfg["default_profile"]
        self.rate = RateControl()
        self.session: Optional[Session] = None
        self.stats = {"fps": 0, "kbps": 0, "q": 0, "signal": 1}
        self.control_port = 0
        self.input_port = 0
        self._lock = threading.RLock()
        self._stop = threading.Event()
        self._threads: List[threading.Thread] = []
        self._t0 = time.monotonic()
        self._idle_since = time.monotonic()
        self.monitor.add_listener(self._on_status)

    # ------------------------------------------------------------ demarrage
    def start(self) -> None:
        ports, bind = self.cfg["ports"], self.cfg["bind"]
        self.tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if not winutil.IS_WINDOWS:
            self.tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.tcp.bind((bind, ports["control"]))
        self.tcp.listen(4)
        self.tcp.settimeout(0.5)
        self.udp_in = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp_in.bind((bind, ports["input"]))
        self.udp_in.settimeout(0.1)
        self.udp_out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            self.udp_out.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
        except OSError:
            pass
        self.control_port = self.tcp.getsockname()[1]
        self.input_port = self.udp_in.getsockname()[1]
        self.monitor.start()
        for name, fn in (("accept", self._accept_loop), ("input", self._input_loop),
                         ("video", self._video_loop), ("beacon", self._beacon_loop)):
            t = threading.Thread(target=fn, name=name, daemon=True)
            t.start()
            self._threads.append(t)
        log.info("relais pret : controle TCP %d, entrees UDP %d", self.control_port, self.input_port)

    def stop(self) -> None:
        self._stop.set()
        self.monitor.stop()
        with self._lock:
            if self.session:
                self._end_session(self.session)
        for s in (self.tcp, self.udp_in, self.udp_out):
            try:
                s.close()
            except OSError:
                pass
        self.pad.close()
        for t in self._threads:
            t.join(timeout=2.0)

    def serve_forever(self) -> None:
        self.start()
        try:
            while not self._stop.wait(0.5):
                pass
        finally:
            self.stop()

    # ------------------------------------------------------------- balises
    def _beacon_loop(self) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        targets = beacon_targets(self.cfg.get("beacon_targets", []))
        payload = P.make_beacon(self.cfg["name"], self.control_port)
        beacon_port = self.cfg["ports"]["beacon"]
        while not self._stop.is_set():
            for t in targets:
                try:
                    sock.sendto(payload, (t, beacon_port))
                except OSError:
                    pass
            self._stop.wait(2.0)
        sock.close()

    # ------------------------------------------------------------- controle
    def _accept_loop(self) -> None:
        while not self._stop.is_set():
            try:
                conn, addr = self.tcp.accept()
            except socket.timeout:
                continue
            except OSError:
                if self._stop.is_set():
                    return
                continue
            threading.Thread(target=self._handle, args=(conn, addr[0]), name="ctl-" + addr[0],
                             daemon=True).start()

    def _handle(self, conn: socket.socket, ip: str) -> None:
        if self.cfg["lan_only"] and not is_private(ip):
            log.warning("connexion refusee (hors reseau local) : %s", ip)
            conn.close()
            return
        conn.settimeout(1.0)
        try:
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass
        sess: Optional[Session] = None
        buf = b""
        t_conn = time.monotonic()
        try:
            while not self._stop.is_set():
                try:
                    chunk = conn.recv(1024)
                except socket.timeout:
                    now = time.monotonic()
                    if sess is None and now - t_conn > 5.0:
                        return                        # pas de HELLO
                    if sess is not None and now - sess.last_rx > SESSION_TIMEOUT:
                        log.warning("console %s silencieuse, deconnexion", ip)
                        return
                    continue
                except OSError:
                    return
                if not chunk:
                    return
                buf += chunk
                if len(buf) > 2 * P.MAX_LINE and b"\n" not in buf:
                    return
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    if len(raw) > P.MAX_LINE:
                        return
                    fields = split_line(raw)
                    if sess is None:
                        sess = self._hello(conn, ip, fields)
                        if sess is None:
                            return
                    else:
                        sess.last_rx = time.monotonic()
                        if not self._dispatch(sess, fields):
                            return
        finally:
            if sess is not None:
                self._end_session(sess)
            try:
                conn.close()
            except OSError:
                pass

    def _reject(self, conn: socket.socket, code: str, msg: str, delay: float = 0.0) -> None:
        if delay:
            time.sleep(delay)
        try:
            conn.sendall(make_line("ERR", code, msg))
        except OSError:
            pass
        return None

    def _hello(self, conn: socket.socket, ip: str, f: List[str]) -> Optional[Session]:
        if len(f) < 5 or f[0] != "HELLO":
            return self._reject(conn, "proto", "HELLO attendu")
        try:
            proto, vport = int(f[1]), int(f[4])
        except ValueError:
            return self._reject(conn, "proto", "HELLO invalide")
        if proto != P.PROTO_VERSION:
            return self._reject(conn, "proto", "Version %d non geree (relais : %d)"
                                % (proto, P.PROTO_VERSION))
        if not 1 <= vport <= 65535:
            return self._reject(conn, "proto", "Port video invalide")
        pin = self.cfg["pin"]
        if pin and not hmac.compare_digest(f[3].encode("utf-8", "replace"), pin.encode("utf-8")):
            log.warning("code incorrect depuis %s", ip)
            return self._reject(conn, "auth", "Code incorrect", delay=1.0)
        with self._lock:
            old = self.session
            # La meme console (meme adresse, meme type) qui se reconnecte apres une coupure Wi-Fi
            # reprend tout de suite sa propre session, sans attendre que l'ancienne expire.
            same_console = old is not None and old.ip == ip and old.device == f[2]
            if (old is not None and old.alive and not same_console
                    and time.monotonic() - old.last_rx < BUSY_TIMEOUT):
                return self._reject(conn, "busy", "Une autre console est deja connectee")
            if old is not None:
                self._end_session(old)
            sess = Session(conn, ip, f[2], vport)
            self.session = sess
        log.info("console connectee : %s (%s), video vers le port %d", ip, f[2], vport)
        sess.send("OK", P.PROTO_VERSION, ascii_clean(self.cfg["name"], 24), self.input_port)
        self._send_profiles(sess)
        self._send_state(sess, self.monitor.status)
        return sess

    def _send_profiles(self, sess: Session) -> None:
        profs = self.cfg["profiles"]
        sess.send("PROFILES", len(profs), self.profile_id)
        for pid, p in profs.items():
            sess.send("P", ascii_clean(pid, 16), ascii_clean(p["label"], 28))

    def _send_state(self, sess: Session, st: Status) -> None:
        sess.send("STATE", st.state, st.queue_pos, st.eta_s, ascii_clean(st.game, 38),
                  ascii_clean(st.text, 70))

    def _on_status(self, st: Status) -> None:
        s = self.session
        if s is not None:
            self._send_state(s, st)

    def _dispatch(self, sess: Session, f: List[str]) -> bool:
        cmd = f[0]
        if cmd == "PING":
            sess.send("PONG", f[1] if len(f) > 1 else "0")
        elif cmd == "LIST":
            self.catalog.reload()
            games = self.catalog.games()
            sess.send("GAMES", len(games))
            for g in games:
                sess.send("G", g.id, g.name)
        elif cmd == "LAUNCH" and len(f) >= 2:
            g = self.catalog.get(f[1])
            if g is None:
                sess.send("ERR", "launch", "Jeu inconnu")
            else:
                ok, msg = self.monitor.launch(g)
                if ok:
                    self._start_streaming(sess)
                    sess.send("OK", "LAUNCH", int(self.pad.available))
                else:
                    sess.send("ERR", "launch", msg)
        elif cmd == "WATCH":
            self._start_streaming(sess)
            sess.send("OK", "WATCH", int(self.pad.available))
        elif cmd == "STOP":
            self._stop_streaming(sess)
            sess.send("OK", "STOP")
        elif cmd == "PROFILE" and len(f) >= 2:
            if f[1] in self.cfg["profiles"]:
                self.profile_id = f[1]
                sess.send("OK", "PROFILE", f[1])
            else:
                sess.send("ERR", "profile", "Profil inconnu")
        elif cmd == "RX" and len(f) >= 3:
            try:
                self.rate.report(int(f[1]), int(f[2]))
            except ValueError:
                pass
        elif cmd == "STATUS":
            self._send_state(sess, self.monitor.status)
        elif cmd == "BYE":
            return False
        else:
            sess.send("ERR", "unknown", ascii_clean(cmd, 20))
        return True

    # ------------------------------------------------------------- sessions
    def _start_streaming(self, sess: Session) -> None:
        with self._lock:
            if self.session is not sess:
                return
            sess.streaming = True
        self.pad.open()
        self._focus_gfn_later()

    def _stop_streaming(self, sess: Session) -> None:
        sess.streaming = False
        self.pad.close()

    def _end_session(self, sess: Session) -> None:
        # L'etat GeForce NOW (file d'attente, jeu en cours) n'est volontairement PAS remis a zero :
        # si la console se reconnecte (Wi-Fi qui coupe), elle retrouve l'etat et peut reprendre.
        with self._lock:
            sess.streaming = False
            sess.alive = False
            try:
                sess.sock.close()
            except OSError:
                pass
            if self.session is sess:
                self.session = None
                self.pad.close()
                log.info("console deconnectee : %s", sess.ip)

    def _focus_gfn_later(self) -> None:
        """Amene la fenetre GeForce NOW au premier plan (elle doit etre visible pour etre capturee)."""
        if not winutil.IS_WINDOWS or self.source_override:
            return
        c = self.cfg["capture"]

        def run() -> None:
            for delay in (3.0, 6.0, 10.0):
                if self._stop.wait(delay):
                    return
                w = winutil.find_window(c["window_title"], c.get("window_process", ""))
                if w:
                    winutil.bring_to_front(w[0])

        threading.Thread(target=run, name="focus", daemon=True).start()

    # --------------------------------------------------------------- entrees
    def _input_loop(self) -> None:
        last_rx = 0.0
        active = False
        while not self._stop.is_set():
            data, addr = None, None
            try:
                data, addr = self.udp_in.recvfrom(64)
            except socket.timeout:
                pass
            except OSError:
                if self._stop.is_set():
                    return
            now = time.monotonic()
            s = self.session
            if data is not None and s is not None and s.streaming and addr[0] == s.ip:
                pkt = P.InputPacket.unpack(data)
                if pkt is not None and (s.last_seq is None or P.seq_newer(pkt.seq, s.last_seq)):
                    s.last_seq = pkt.seq
                    last_rx = now
                    st = map_input(pkt, self.cfg["input"])
                    active = not st.neutral
                    self.pad.apply(st)
            if active and (s is None or not s.streaming):
                active = False                      # le relachement est fait par pad.close()
            elif active and now - last_rx > INPUT_WATCHDOG:   # console muette : rien ne reste appuye
                self.pad.neutral()
                active = False

    # ----------------------------------------------------------------- video
    def _close_source(self, src: Optional[FrameSource]) -> None:
        if src is not None:
            try:
                src.stop()
            except Exception:  # noqa: BLE001
                log.exception("arret de la source")

    def _video_loop(self) -> None:
        enc = FrameEncoder(self.cfg["capture"]["fit"])
        src: Optional[FrameSource] = None
        src_retry_at = 0.0
        qc: Optional[QualityController] = None
        qc_profile = ""
        nosignal: Optional[np.ndarray] = None
        next_t = time.monotonic()
        frame_id = 0
        last_crc: Optional[int] = None
        last_sent = 0.0
        win_t = time.monotonic()
        win_frames = win_bytes = 0
        had_signal = True

        while not self._stop.is_set():
            s = self.session
            now = time.monotonic()
            if s is None or not s.streaming or not s.alive:
                if src is not None and now - self._idle_since > 5.0:
                    self._close_source(src)       # libere la capture quand personne ne regarde
                    src = None
                self._stop.wait(0.05)
                next_t = time.monotonic()
                win_t, win_frames, win_bytes = next_t, 0, 0
                continue
            self._idle_since = now

            prof = self.cfg["profiles"][self.profile_id]
            if qc is None or qc_profile != self.profile_id:
                qc, qc_profile = QualityController(prof["q_min"], prof["q_max"]), self.profile_id
            scale = self.rate.scale
            qc.set_bounds(max(10, prof["q_min"] - int((1.0 - scale) * 40)), prof["q_max"])
            period = 1.0 / max(5, int(prof["fps"] * (0.5 + 0.5 * scale)))

            if now < next_t:
                self._stop.wait(next_t - now)
                now = time.monotonic()
            next_t += period
            if now - next_t > 2 * period:           # tres en retard : on se resynchronise
                next_t = now + period

            if src is None and now >= src_retry_at:
                try:
                    src = make_source(self.cfg, self.source_override)
                    src.start()
                    log.info("source video : %s", src.name)
                except Exception as e:  # noqa: BLE001
                    log.error("source video indisponible : %s", e)
                    src = None
                    src_retry_at = now + SOURCE_RETRY_S
            frame = None
            if src is not None:
                try:
                    frame = src.read()
                except Exception as e:  # noqa: BLE001
                    log.error("lecture de la source : %s", e)
                    self._close_source(src)
                    src = None
                    src_retry_at = now + SOURCE_RETRY_S
            signal = frame is not None
            if frame is None:
                if nosignal is None:
                    nosignal = nosignal_frame()
                frame = nosignal
            if signal != had_signal:
                log.info("signal video : %s", "ok" if signal else "perdu")
                had_signal = signal

            fitted = enc.fit_frame(frame)
            crc = zlib.crc32(np.ascontiguousarray(fitted))
            if crc != last_crc or now - last_sent >= KEEPALIVE_FRAME_S:
                target = int(prof["target_kb"] * 1024 * scale)
                jpeg = enc.encode_fitted(fitted, qc.q)
                if len(jpeg) > P.MAX_FRAME_BYTES:
                    jpeg = enc.encode_fitted(fitted, max(10, qc.q - 25))
                if len(jpeg) <= P.MAX_FRAME_BYTES:
                    qc.update(len(jpeg), target)
                    ts = int((time.monotonic() - self._t0) * 1000)
                    for pkt in P.fragment_frame(frame_id, jpeg, ts):
                        try:
                            self.udp_out.sendto(pkt, (s.ip, s.video_port))
                        except OSError:
                            pass
                        win_bytes += len(pkt)
                    frame_id = (frame_id + 1) & 0xFFFF
                    win_frames += 1
                    last_crc, last_sent = crc, now
            if now - win_t >= 1.0:
                dt = now - win_t
                self.stats = {"fps": round(win_frames / dt), "kbps": int(win_bytes * 8 / dt / 1000),
                              "q": qc.q, "signal": int(signal)}
                s.send("STATS", self.stats["fps"], self.stats["kbps"], qc.q, self.profile_id,
                       int(signal), int(self.pad.available))
                win_t, win_frames, win_bytes = now, 0, 0
        self._close_source(src)
