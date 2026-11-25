"""Le protocole est ecrit deux fois : relay/gfn3ds_relay/protocol.py (la reference, cote PC) et
client3ds/include/protocol.hpp (cote 3DS). Ces tests verifient que les deux decrivent la meme chose :
memes constantes, memes etats, memes octets sur le fil.

Si client3ds/ est absent (relais distribue seul), les tests sont ignores. Pour tester un autre
fichier d'en-tete : GFN3DS_PROTOCOL_HPP=/chemin/protocol.hpp
"""
import os
import re
import unittest

from gfn3ds_relay import protocol as P

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.environ.get("GFN3DS_PROTOCOL_HPP") or os.path.normpath(
    os.path.join(HERE, "..", "..", "client3ds", "include", "protocol.hpp"))

# Memes octets que test_protocol_golden_input_packet / test_protocol_golden_video_header
# dans client3ds/tests/host/unit_tests.cpp
GOLDEN_INPUT = bytes([0x49, 0x01, 0x34, 0x12, 0x03, 0x00, 0x41, 0x00,
                      0xFF, 0xFF, 0x78, 0x00, 0x88, 0xFF, 0xFF, 0x7F])
GOLDEN_VIDEO = bytes([0x47, 0x01, 0x02, 0x01, 0x03, 0x00, 0x05, 0x00,
                      0x0D, 0x0C, 0x0B, 0x0A, 0x61, 0x62])

# Constantes du .hpp dont le nom Python est different
ALIASES = {
    "VIDEO_HDR_SIZE": lambda: P.VIDEO_HDR.size,
    "INPUT_PKT_SIZE": lambda: P.INPUT_PKT.size,
}


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _eval_int(expr, known):
    """Evalue une expression entiere simple du .hpp (litteraux, << * + - et constantes deja lues)."""
    expr = re.sub(r"\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]*\b", r"\1", expr)
    expr = re.sub(r"\b[A-Za-z_]\w*\b", lambda m: str(known[m.group(0)]) if m.group(0) in known else m.group(0), expr)
    if not re.fullmatch(r"[0-9a-fA-FxX\s<*+\-()]+", expr):
        raise ValueError("expression non geree : %r" % expr)
    return int(eval(expr, {"__builtins__": {}}, {}))   # noqa: S307 - expression filtree ci-dessus


def read_header(path):
    with open(path, encoding="utf-8") as f:
        text = _strip_comments(f.read())
    consts = {}
    for m in re.finditer(r"constexpr\s+(?:int|uint32_t|uint16_t|uint8_t)\s+(\w+)\s*=\s*([^;]+);", text):
        consts[m.group(1)] = _eval_int(m.group(2).strip(), consts)
    states = re.findall(r'!strcmp\(s,\s*"(\w+)"\)', text)
    return consts, states


@unittest.skipUnless(os.path.exists(HEADER), "client3ds/include/protocol.hpp introuvable (relais distribue seul)")
class TestProtocolSync(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.consts, cls.states = read_header(HEADER)

    def py_value(self, name):
        if name in ALIASES:
            return ALIASES[name]()
        self.assertTrue(hasattr(P, name), "%s existe dans protocol.hpp mais pas dans protocol.py" % name)
        return getattr(P, name)

    def test_every_constant_of_the_header_matches_python(self):
        self.assertGreater(len(self.consts), 30, "l'analyse du .hpp n'a presque rien trouve")
        for name, value in self.consts.items():
            self.assertEqual(self.py_value(name), value, "constante %s differente" % name)

    def test_no_python_constant_is_missing_from_the_header(self):
        py_names = set()
        for name in dir(P):
            if name.startswith("_"):
                continue
            obj = getattr(P, name)
            if name.isupper() and isinstance(obj, int):
                py_names.add(name)
        # SCREEN_W / SCREEN_H sont declares en tuple en Python mais restent des entiers
        missing = sorted(n for n in py_names if n not in self.consts)
        self.assertEqual(missing, [], "constantes Python absentes de protocol.hpp : %s" % missing)

    def test_button_bits_are_unique(self):
        bits = {n: v for n, v in self.consts.items() if n.startswith("B_")}
        self.assertGreaterEqual(len(bits), 20)
        for n, v in bits.items():
            self.assertTrue(v and v & (v - 1) == 0, "%s n'est pas un bit unique" % n)
        self.assertEqual(len(set(bits.values())), len(bits))

    def test_states_match(self):
        py_states = {v for k, v in vars(P.State).items() if k.isupper()}
        self.assertEqual(set(self.states), py_states)

    def test_frame_geometry_is_consistent(self):
        self.assertGreaterEqual(self.consts["MAX_FRAGS"] * self.consts["FRAG_PAYLOAD"], self.consts["MAX_FRAME_BYTES"])
        self.assertEqual(self.consts["VIDEO_HDR_SIZE"], 12)
        self.assertEqual(self.consts["INPUT_PKT_SIZE"], 16)


class TestWireFormat(unittest.TestCase):
    """Octets exacts sur le fil : ces vecteurs sont aussi verifies par les tests C++."""

    def test_golden_input_packet(self):
        pkt = P.InputPacket(flags=1, seq=0x1234, buttons=0x00410003,
                            cpad_x=-1, cpad_y=120, cstick_x=-120, cstick_y=32767)
        self.assertEqual(pkt.pack(), GOLDEN_INPUT)
        back = P.InputPacket.unpack(GOLDEN_INPUT)
        self.assertEqual(back, pkt)
        self.assertTrue(back.new3ds)

    def test_golden_video_datagram(self):
        self.assertEqual(P.VIDEO_HDR.pack(P.VIDEO_MAGIC, P.KIND_JPEG, 0x0102, 3, 5, 0x0A0B0C0D) + b"ab", GOLDEN_VIDEO)
        self.assertEqual(P.parse_video_packet(GOLDEN_VIDEO), (0x0102, 3, 5, 0x0A0B0C0D, b"ab"))

    def test_fragment_frame_matches_the_documented_layout(self):
        jpeg = bytes(range(256)) * 10             # 2560 octets -> 3 fragments (1200, 1200, 160)
        frags = P.fragment_frame(0x1FFFF, jpeg, 0x1_0000_0005)     # les numeros bouclent sur 16 / 32 bits
        self.assertEqual(len(frags), 3)
        self.assertEqual([len(f) for f in frags], [12 + 1200, 12 + 1200, 12 + 160])
        for i, f in enumerate(frags):
            fid, idx, cnt, ts, payload = P.parse_video_packet(f)
            self.assertEqual((fid, idx, cnt, ts), (0xFFFF, i, 3, 5))
            self.assertEqual(payload, jpeg[i * 1200:(i + 1) * 1200])

    def test_seq_newer_matches_the_cpp_cases(self):
        # memes cas que test_protocol_seq_newer cote C++
        self.assertTrue(P.seq_newer(1, 0))
        self.assertFalse(P.seq_newer(0, 1))
        self.assertFalse(P.seq_newer(5, 5))
        self.assertTrue(P.seq_newer(0, 65535))
        self.assertFalse(P.seq_newer(65535, 0))
        self.assertTrue(P.seq_newer(0x7FFF, 0))
        self.assertFalse(P.seq_newer(0x8000, 0))

    def test_control_lines_are_tab_separated_ascii(self):
        line = P.make_line("STATE", "QUEUE", 12, -1, "Jeu\tavec tab", "Texte")
        self.assertTrue(line.endswith(b"\n"))
        self.assertEqual(P.split_line(line), ["STATE", "QUEUE", "12", "-1", "Jeu avec tab", "Texte"])
        self.assertLessEqual(len(P.make_beacon("Mon PC", 47970)), P.MAX_LINE)
        self.assertEqual(P.split_line(P.make_beacon("Mon PC", 47970)), ["GFN3DS", "1", "Mon PC", "47970"])


if __name__ == "__main__":
    unittest.main()
