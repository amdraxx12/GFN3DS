// Tests unitaires du client, sur PC : le VRAI code (video_decoder, settings, input, protocol.hpp)
// compile avec g++ sans libctru. Lancement : tests/host/run_unit_tests.sh
//
// Ce qui est couvert : reassemblage des fragments video (ordre quelconque, doublons, pertes, bouclage
// du numero d'image, paquets invalides), tampon de gigue, reglages (valeurs par defaut, nettoyage,
// aller-retour fichier), correspondance boutons 3DS -> manette Xbox, zones tactiles, paquet d'entrees
// et fonctions du protocole.
//
// Les vecteurs "dores" (octets exacts d'un paquet d'entrees et d'un datagramme video) sont les memes
// dans relay/tests/test_protocol_sync.py : si l'un des deux cotes change de format, un test casse.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "input.hpp"
#include "protocol.hpp"
#include "settings.hpp"
#include "video_decoder.hpp"

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_failed++;                                                                \
            printf("    ECHEC %s:%d : %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                 \
    do {                                                                               \
        long long va_ = (long long)(a), vb_ = (long long)(b);                          \
        g_checks++;                                                                    \
        if (va_ != vb_) {                                                              \
            g_failed++;                                                                \
            printf("    ECHEC %s:%d : %s == %s (%lld != %lld)\n", __FILE__, __LINE__,  \
                   #a, #b, va_, vb_);                                                  \
        }                                                                              \
    } while (0)

#define CHECK_STR(a, b)                                                                \
    do {                                                                               \
        g_checks++;                                                                    \
        if (strcmp((a), (b)) != 0) {                                                   \
            g_failed++;                                                                \
            printf("    ECHEC %s:%d : \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
        }                                                                              \
    } while (0)

typedef std::vector<uint8_t> Bytes;

// Octets pseudo-aleatoires reproductibles.
Bytes pattern(size_t n, unsigned seed) {
    Bytes v(n);
    uint32_t x = seed * 2654435761u + 12345u;
    for (size_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        v[i] = (uint8_t)(x >> 24);
    }
    return v;
}

// Un datagramme video construit "a la main" (sans passer par du code teste).
Bytes datagram(uint16_t fid, uint16_t idx, uint16_t cnt, uint32_t ts, const uint8_t* payload, size_t n) {
    Bytes v(12 + n);
    v[0] = 0x47;
    v[1] = 1;
    v[2] = (uint8_t)(fid & 0xFF);
    v[3] = (uint8_t)(fid >> 8);
    v[4] = (uint8_t)(idx & 0xFF);
    v[5] = (uint8_t)(idx >> 8);
    v[6] = (uint8_t)(cnt & 0xFF);
    v[7] = (uint8_t)(cnt >> 8);
    v[8] = (uint8_t)(ts & 0xFF);
    v[9] = (uint8_t)((ts >> 8) & 0xFF);
    v[10] = (uint8_t)((ts >> 16) & 0xFF);
    v[11] = (uint8_t)(ts >> 24);
    if (n) memcpy(&v[12], payload, n);
    return v;
}

// Decoupe une image comme le relais (fragment_frame en Python).
std::vector<Bytes> fragment(uint16_t fid, const Bytes& jpeg, uint32_t ts) {
    std::vector<Bytes> out;
    const size_t P = 1200;
    size_t n = (jpeg.size() + P - 1) / P;
    for (size_t i = 0; i < n; i++) {
        size_t off = i * P;
        size_t len = jpeg.size() - off < P ? jpeg.size() - off : P;
        out.push_back(datagram(fid, (uint16_t)i, (uint16_t)n, ts, jpeg.data() + off, len));
    }
    return out;
}

bool feed(FrameReassembler& r, const Bytes& d) { return r.feed(d.data(), (int)d.size()); }

// ============================================================================ FrameReassembler
void test_reassembler_single_fragment() {
    FrameReassembler* r = new FrameReassembler();
    Bytes data = pattern(500, 1);
    std::vector<Bytes> f = fragment(7, data, 1234);
    CHECK_EQ(f.size(), 1);
    CHECK(feed(*r, f[0]));
    CHECK_EQ(r->size(), 500);
    CHECK(memcmp(r->data(), data.data(), 500) == 0);
    CHECK_EQ(r->frame_id(), 7);
    CHECK_EQ(r->ts_ms(), 1234);
    delete r;
}

void test_reassembler_out_of_order() {
    FrameReassembler* r = new FrameReassembler();
    Bytes data = pattern(5000, 2);    // 4 fragments de 1200 + 1 de 200
    std::vector<Bytes> f = fragment(100, data, 999);
    CHECK_EQ(f.size(), 5);
    const int order[5] = {3, 0, 4, 2, 1};
    for (int i = 0; i < 5; i++) {
        bool done = feed(*r, f[(size_t)order[i]]);
        CHECK(done == (i == 4));      // seul le dernier fragment recu termine l'image
    }
    CHECK_EQ(r->size(), 5000);
    CHECK(memcmp(r->data(), data.data(), 5000) == 0);
    CHECK_EQ(r->take_incomplete(), 0);
    delete r;
}

void test_reassembler_exact_multiple_of_payload() {
    FrameReassembler* r = new FrameReassembler();
    Bytes data = pattern(2400, 3);    // 2 fragments pleins : le dernier fait exactement 1200 octets
    std::vector<Bytes> f = fragment(1, data, 0);
    CHECK_EQ(f.size(), 2);
    CHECK(!feed(*r, f[0]));
    CHECK(feed(*r, f[1]));
    CHECK_EQ(r->size(), 2400);
    CHECK(memcmp(r->data(), data.data(), 2400) == 0);
    delete r;
}

void test_reassembler_duplicates() {
    FrameReassembler* r = new FrameReassembler();
    Bytes data = pattern(3000, 4);
    std::vector<Bytes> f = fragment(5, data, 0);
    CHECK(!feed(*r, f[1]));
    CHECK(!feed(*r, f[1]));           // doublon : ignore
    CHECK(!feed(*r, f[0]));
    CHECK(feed(*r, f[2]));
    CHECK(!feed(*r, f[2]));           // image deja complete : un fragment en retard ne la "re-termine" pas
    CHECK(!feed(*r, f[0]));
    CHECK_EQ(r->size(), 3000);
    CHECK(memcmp(r->data(), data.data(), 3000) == 0);

    // Un doublon aux donnees differentes (reseau fantaisiste) n'ecrase pas le premier exemplaire recu.
    Bytes other = pattern(3000, 99);
    std::vector<Bytes> g = fragment(6, data, 0), h = fragment(6, other, 0);
    CHECK(!feed(*r, g[1]));
    CHECK(!feed(*r, h[1]));
    CHECK(!feed(*r, g[0]));
    CHECK(feed(*r, g[2]));
    CHECK(memcmp(r->data(), data.data(), 3000) == 0);
    delete r;
}

void test_reassembler_loss_then_newer_frame() {
    FrameReassembler* r = new FrameReassembler();
    Bytes a = pattern(3000, 5), b = pattern(2500, 6);
    std::vector<Bytes> fa = fragment(10, a, 100), fb = fragment(11, b, 133);
    CHECK(!feed(*r, fa[0]));
    CHECK(!feed(*r, fa[2]));          // fa[1] est perdu
    CHECK_EQ(r->take_incomplete(), 0);    // pas encore sur : le fragment manquant peut arriver
    CHECK(!feed(*r, fb[0]));          // l'image suivante abandonne la 10
    CHECK_EQ(r->take_incomplete(), 1);
    CHECK_EQ(r->take_incomplete(), 0);    // le compteur est remis a zero a chaque lecture
    CHECK(!feed(*r, fa[1]));          // fragment tardif d'une image plus ancienne : ignore
    CHECK(!feed(*r, fb[1]));
    CHECK(feed(*r, fb[2]));
    CHECK_EQ(r->frame_id(), 11);
    CHECK_EQ(r->size(), 2500);
    CHECK(memcmp(r->data(), b.data(), 2500) == 0);
    delete r;
}

void test_reassembler_frame_id_wraparound() {
    FrameReassembler* r = new FrameReassembler();
    CHECK(feed(*r, fragment(65535, pattern(100, 7), 1)[0]));
    CHECK(feed(*r, fragment(0, pattern(100, 8), 34)[0]));     // 0 vient apres 65535
    CHECK_EQ(r->frame_id(), 0);
    CHECK(!feed(*r, fragment(65535, pattern(100, 9), 1)[0])); // et 65535 est desormais "ancien"
    CHECK_EQ(r->frame_id(), 0);
    CHECK(feed(*r, fragment(1, pattern(100, 10), 67)[0]));
    delete r;
}

void test_reassembler_rejects_garbage() {
    FrameReassembler* r = new FrameReassembler();
    Bytes pay = pattern(10, 11);
    Bytes ok = datagram(1, 0, 1, 0, pay.data(), 10);
    Bytes bad = ok;
    bad[0] = 0x48;                                             // mauvais "magic"
    CHECK(!feed(*r, bad));
    bad = ok;
    bad[1] = 2;                                                // type inconnu
    CHECK(!feed(*r, bad));
    CHECK(!feed(*r, datagram(1, 0, 0, 0, pay.data(), 10)));    // 0 fragment
    CHECK(!feed(*r, datagram(1, 0, 65, 0, pay.data(), 10)));   // plus que MAX_FRAGS
    CHECK(!feed(*r, datagram(1, 2, 2, 0, pay.data(), 10)));    // indice >= nombre
    Bytes only_header(ok.begin(), ok.begin() + 12);            // en-tete seul, sans donnees
    CHECK(!feed(*r, only_header));
    Bytes big = pattern(1201, 12);
    CHECK(!feed(*r, datagram(1, 0, 1, 0, big.data(), 1201)));  // fragment trop gros
    CHECK(!feed(*r, datagram(1, 0, 2, 0, pay.data(), 10)));    // fragment non final trop court
    CHECK(!r->feed(ok.data(), 5));                             // datagramme tronque
    CHECK(feed(*r, ok));                                       // et une image valide passe toujours ensuite
    CHECK_EQ(r->size(), 10);
    delete r;
}

void test_reassembler_64_fragments() {
    FrameReassembler* r = new FrameReassembler();
    Bytes data = pattern(63 * 1200 + 10, 13);    // 64 fragments : masque de 64 bits entierement rempli
    std::vector<Bytes> f = fragment(3, data, 5);
    CHECK_EQ(f.size(), 64);
    for (size_t i = 0; i < 64; i++) {
        size_t k = 63 - i;                       // a l'envers
        CHECK(feed(*r, f[k]) == (i == 63));
    }
    CHECK_EQ(r->size(), (int)data.size());
    CHECK(memcmp(r->data(), data.data(), data.size()) == 0);
    delete r;
}

void test_reassembler_mismatched_count() {
    FrameReassembler* r = new FrameReassembler();
    Bytes p = pattern(1200, 14);
    CHECK(!feed(*r, datagram(5, 0, 3, 0, p.data(), 1200)));
    CHECK(!feed(*r, datagram(5, 1, 4, 0, p.data(), 1200)));    // meme image, autre nombre de fragments
    CHECK(!feed(*r, datagram(5, 1, 3, 0, p.data(), 1200)));    // l'image d'origine peut toujours aboutir
    CHECK(feed(*r, datagram(5, 2, 3, 0, p.data(), 100)));
    CHECK_EQ(r->size(), 2500);
    delete r;
}

void test_reassembler_reset() {
    FrameReassembler* r = new FrameReassembler();
    Bytes d = pattern(3000, 15);
    std::vector<Bytes> f = fragment(9, d, 0);
    CHECK(!feed(*r, f[0]));
    r->reset();
    // apres reset, une image "plus ancienne" est acceptee (nouvelle session : la numerotation repart de zero)
    std::vector<Bytes> g = fragment(2, d, 0);
    CHECK(!feed(*r, g[0]));
    CHECK(!feed(*r, g[1]));
    CHECK(feed(*r, g[2]));
    CHECK_EQ(r->take_incomplete(), 0);
    delete r;
}

// ================================================================================ JitterBuffer
void test_jitter_constant_delay() {
    JitterBuffer jb;                              // mode adaptatif
    const uint64_t t0 = 100000;
    for (int i = 0; i < 300; i++) {
        uint32_t ts = (uint32_t)(i * 33);
        uint64_t arrival = t0 + ts + 20;          // reseau stable : 20 ms de transit
        uint64_t due = jb.on_frame(ts, arrival);
        CHECK(due >= arrival);
        CHECK_EQ(due - arrival, 5);               // seule la marge de base est ajoutee
    }
    CHECK_EQ(jb.jitter_ms(), 0);
    CHECK_EQ(jb.delay_ms(), 5);
}

void test_jitter_absorbs_network_jitter() {
    JitterBuffer jb;
    const uint64_t t0 = 50000;
    int64_t first_off = -1;
    for (int i = 0; i < 400; i++) {
        uint32_t ts = (uint32_t)(i * 33);
        uint64_t arrival = t0 + ts + 20 + ((i & 1) ? 30 : 0);   // une image sur deux arrive 30 ms plus tard
        uint64_t due = jb.on_frame(ts, arrival);
        CHECK(due >= arrival);
        if (i >= 100) {                           // une fois le tampon adapte, l'affichage est regulier
            int64_t off = (int64_t)due - (int64_t)ts;
            if (first_off < 0) first_off = off;
            CHECK_EQ(off, first_off);
        }
    }
    CHECK(jb.jitter_ms() >= 25 && jb.jitter_ms() <= 31);
    CHECK_EQ(jb.delay_ms(), 50);                  // plafonne a 50 ms en mode adaptatif
}

void test_jitter_low_latency_never_waits() {
    JitterBuffer jb;
    jb.set_mode(BUFFER_LOW_LATENCY);
    for (int i = 0; i < 200; i++) {
        uint32_t ts = (uint32_t)(i * 33);
        uint64_t arrival = 1000 + ts + (uint64_t)((i * 7) % 40);
        CHECK_EQ(jb.on_frame(ts, arrival), arrival);
    }
    CHECK_EQ(jb.delay_ms(), 0);
}

void test_jitter_smooth_mode_adds_margin() {
    JitterBuffer jb;
    jb.set_mode(BUFFER_SMOOTH);
    uint64_t due = 0, arrival = 0;
    for (int i = 0; i < 50; i++) {
        uint32_t ts = (uint32_t)(i * 33);
        arrival = 1000 + ts;
        due = jb.on_frame(ts, arrival);
    }
    CHECK_EQ(due - arrival, 25);                  // marge fixe de 25 ms sans gigue
}

void test_jitter_late_frame_does_not_wait() {
    JitterBuffer jb;
    for (int i = 0; i < 100; i++) jb.on_frame((uint32_t)(i * 33), 1000 + (uint64_t)i * 33);
    // une image qui a 100 ms de retard sur toutes les autres : on l'affiche tout de suite
    uint32_t ts = 100 * 33;
    uint64_t arrival = 1000 + ts + 100;
    CHECK_EQ(jb.on_frame(ts, arrival), arrival);
}

void test_jitter_timestamp_wraparound() {
    JitterBuffer jb;
    const uint32_t base = 0xFFFFFF00u;            // l'horloge du relais boucle (32 bits) pendant le test
    const uint64_t t0 = 777000;
    for (int i = 0; i < 100; i++) {
        uint32_t ts = base + (uint32_t)(i * 33);
        uint64_t arrival = t0 + (uint64_t)i * 33 + 15;
        uint64_t due = jb.on_frame(ts, arrival);
        CHECK_EQ(due - arrival, 5);
    }
    CHECK_EQ(jb.jitter_ms(), 0);
}

void test_jitter_long_pause_and_reset() {
    JitterBuffer jb;
    for (int i = 0; i < 50; i++) jb.on_frame((uint32_t)(i * 33), 1000 + (uint64_t)i * 33);
    // 30 s sans image, puis le flux reprend : pas de plantage, jamais d'heure d'affichage dans le passe
    for (int i = 0; i < 50; i++) {
        uint32_t ts = (uint32_t)(30000 + i * 33);
        uint64_t arrival = 1000 + ts;
        CHECK(jb.on_frame(ts, arrival) >= arrival);
    }
    jb.set_mode(BUFFER_LOW_LATENCY);
    jb.reset();                                   // reset() conserve le mode choisi
    CHECK_EQ(jb.delay_ms(), 0);
    CHECK_EQ(jb.on_frame(5, 9000), 9000);
    CHECK_EQ(jb.on_frame(38, 9033), 9033);
}

// ================================================================================== Settings
std::string make_tmp_dir_name() {
    char buf[128];
    snprintf(buf, sizeof(buf), "gfn3ds_ut_%d", (int)getpid());
    return buf;
}

bool write_text(const std::string& path, const char* text) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fputs(text, f);
    return fclose(f) == 0;
}

void test_settings_defaults() {
    Settings s;
    CHECK_EQ(s.port, proto::PORT_CONTROL);
    CHECK_STR(s.host, "");
    CHECK_STR(s.pin, "");
    CHECK_STR(s.profile, "balanced");
    CHECK(s.auto_connect);
    CHECK(s.auto_reconnect);
    CHECK_EQ(s.buffer_mode, BUFFER_ADAPTIVE);
    CHECK_EQ(s.overlay, 1);
    uint8_t ref[input::NUM_SRC];
    input::default_map(ref);
    CHECK(memcmp(ref, s.keymap, input::NUM_SRC) == 0);
}

void test_settings_sanitize() {
    Settings s;
    s.port = 0;
    s.sanitize();
    CHECK_EQ(s.port, proto::PORT_CONTROL);
    s.port = 70000;
    s.sanitize();
    CHECK_EQ(s.port, proto::PORT_CONTROL);
    s.port = 65535;
    s.sanitize();
    CHECK_EQ(s.port, 65535);
    s.buffer_mode = 3;
    s.overlay = 5;
    s.sanitize();
    CHECK_EQ(s.buffer_mode, BUFFER_ADAPTIVE);
    CHECK_EQ(s.overlay, 1);
    snprintf(s.host, sizeof(s.host), "h\xC3\xA9te");          // pas de caractere hors ASCII (clavier de la 3DS)
    snprintf(s.pin, sizeof(s.pin), "12\t3");                  // ni de tabulation (separateur du protocole)
    s.sanitize();
    CHECK_STR(s.host, "");
    CHECK_STR(s.pin, "");
    snprintf(s.profile, sizeof(s.profile), "a b");
    s.sanitize();
    CHECK_STR(s.profile, "balanced");
    s.profile[0] = '\0';
    s.sanitize();
    CHECK_STR(s.profile, "balanced");
    snprintf(s.profile, sizeof(s.profile), "latency");
    s.sanitize();
    CHECK_STR(s.profile, "latency");
    s.keymap[3] = 200;
    s.sanitize();
    CHECK_EQ(s.keymap[3], input::TGT_NONE);
}

void test_settings_roundtrip() {
    const std::string dir = make_tmp_dir_name();
    const std::string path = dir + "/sous_dossier/settings.ini";     // le dossier n'existe pas : il est cree
    Settings a;
    snprintf(a.host, sizeof(a.host), "192.168.1.50");
    a.port = 50000;
    snprintf(a.pin, sizeof(a.pin), "4821");
    a.auto_connect = false;
    a.auto_reconnect = false;
    snprintf(a.profile, sizeof(a.profile), "latency");
    a.buffer_mode = BUFFER_SMOOTH;
    a.overlay = 2;
    a.touch_stick = false;
    a.speedup = false;
    a.keymap[input::SRC_A] = input::TGT_START;
    a.keymap[input::SRC_ZL] = input::TGT_NONE;
    a.keymap[input::SRC_DUP] = input::TGT_GUIDE;
    CHECK(settings_save(a, path.c_str()));

    Settings b;
    CHECK(settings_load(b, path.c_str()));
    CHECK_STR(b.host, "192.168.1.50");
    CHECK_EQ(b.port, 50000);
    CHECK_STR(b.pin, "4821");
    CHECK(!b.auto_connect);
    CHECK(!b.auto_reconnect);
    CHECK_STR(b.profile, "latency");
    CHECK_EQ(b.buffer_mode, BUFFER_SMOOTH);
    CHECK_EQ(b.overlay, 2);
    CHECK(!b.touch_stick);
    CHECK(!b.speedup);
    CHECK(memcmp(a.keymap, b.keymap, input::NUM_SRC) == 0);

    remove(path.c_str());
    rmdir((dir + "/sous_dossier").c_str());
    rmdir(dir.c_str());
}

void test_settings_load_is_tolerant() {
    const std::string dir = make_tmp_dir_name() + "_t";
    CHECK(settings_save(Settings(), (dir + "/x.ini").c_str()));      // cree le dossier
    const std::string path = dir + "/settings.ini";
    CHECK(write_text(path,
                     "# commentaire\r\n\r\n; autre commentaire\r\n"
                     "host = 10.0.0.2 \r\n"
                     "port=99999\r\n"
                     "unknown=1\r\n"
                     "ligne_sans_egal\r\n"
                     "buffer_mode=7\r\n"
                     "overlay=-1\r\n"
                     "profile=bad profile\r\n"
                     "map.A=BOGUS\r\n"
                     "map.B=START\r\n"
                     "map.INCONNU=A\r\n"
                     "auto_connect=yes\r\n"
                     "speedup=0\r\n"));
    Settings s;
    CHECK(settings_load(s, path.c_str()));
    CHECK_STR(s.host, "10.0.0.2");                  // espaces et fins de ligne Windows retires
    CHECK_EQ(s.port, proto::PORT_CONTROL);          // valeurs invalides ramenees aux valeurs sures
    CHECK_EQ(s.buffer_mode, BUFFER_ADAPTIVE);
    CHECK_EQ(s.overlay, 1);
    CHECK_STR(s.profile, "balanced");
    CHECK_EQ(s.keymap[input::SRC_A], input::TGT_A); // "BOGUS" ignore : l'ancienne valeur reste
    CHECK_EQ(s.keymap[input::SRC_B], input::TGT_START);
    CHECK(s.auto_connect);
    CHECK(!s.speedup);

    remove(path.c_str());
    remove((dir + "/x.ini").c_str());
    rmdir(dir.c_str());
}

void test_settings_missing_file() {
    Settings s;
    snprintf(s.host, sizeof(s.host), "garde");
    CHECK(!settings_load(s, "ce_fichier_n_existe_pas/settings.ini"));
    CHECK_STR(s.host, "garde");                      // rien n'est modifie
}

// ====================================================================================== Input
input::Mapping default_mapping(bool touch_stick, bool new3ds) {
    input::Mapping m;
    input::default_map(m.map);
    m.touch_stick = touch_stick;
    m.new3ds = new3ds;
    return m;
}

void test_input_default_mapping() {
    input::Mapping m = default_mapping(false, false);
    input::Raw r;
    r.held = (1u << input::SRC_A) | (1u << input::SRC_L) | (1u << input::SRC_DUP) | (1u << input::SRC_ZR);
    proto::InputPacket p = input::build_packet(r, m);
    CHECK_EQ(p.buttons, proto::B_A | proto::B_L | proto::B_DUP | proto::B_VRT);
    CHECK_EQ(p.flags, 0);
    m.new3ds = true;
    CHECK_EQ(input::build_packet(r, m).flags, proto::INPUT_FLAG_NEW3DS);
}

void test_input_start_and_select_are_reserved() {
    input::Mapping m = default_mapping(true, true);
    input::Raw r;
    r.held = input::K_START | input::K_SELECT;
    proto::InputPacket p = input::build_packet(r, m);
    CHECK_EQ(p.buttons, 0);                         // jamais envoyes au jeu : ils pilotent l'interface
    r.held = input::K_START | (1u << input::SRC_B);
    CHECK_EQ(input::build_packet(r, m).buttons, proto::B_B);
}

void test_input_presets() {
    CHECK_EQ(input::num_presets(), 3);
    uint8_t m[input::NUM_SRC];
    for (int i = 0; i < input::num_presets(); i++) {
        input::preset_map(i, m);
        CHECK_EQ(input::detect_preset(m), i);
        m[input::SRC_X] = input::TGT_NONE;          // une touche changee : configuration personnalisee
        CHECK_EQ(input::detect_preset(m), -1);
    }
    input::Mapping mp = default_mapping(false, false);
    input::preset_map(1, mp.map);                   // "Position" : B <-> A, Y <-> X
    input::Raw r;
    r.held = 1u << input::SRC_A;
    CHECK_EQ(input::build_packet(r, mp).buttons, proto::B_B);
    r.held = 1u << input::SRC_Y;
    CHECK_EQ(input::build_packet(r, mp).buttons, proto::B_X);
    input::preset_map(2, mp.map);                   // gachettes sur L/R (Old 3DS)
    r.held = 1u << input::SRC_L;
    CHECK_EQ(input::build_packet(r, mp).buttons, proto::B_VLT);
    r.held = 1u << input::SRC_ZL;
    CHECK_EQ(input::build_packet(r, mp).buttons, proto::B_L);
}

void test_input_target_names_and_bits() {
    uint32_t seen = 0;
    for (int t = 0; t < input::NUM_TGT; t++) {
        CHECK_EQ(input::tgt_from_key(input::tgt_key(t)), t);
        uint32_t b = input::tgt_bit(t);
        if (t == input::TGT_NONE) {
            CHECK_EQ(b, 0);
        } else {
            CHECK(b != 0 && (b & (b - 1)) == 0);    // un seul bit
            CHECK((seen & b) == 0);                 // deux commandes n'ont jamais le meme bit
            seen |= b;
        }
    }
    CHECK_EQ(input::tgt_from_key("BOGUS"), -1);
    CHECK_EQ(input::tgt_from_key(nullptr), -1);
    CHECK_EQ(input::tgt_from_key(""), -1);
    CHECK_STR(input::tgt_key(999), "NONE");
    CHECK_STR(input::src_label(-1), "?");
}

void test_input_touch_buttons() {
    CHECK(input::vbutton_count() >= 9);
    uint32_t seen = 0;
    for (int i = 0; i < input::vbutton_count(); i++) {
        const input::VButton& b = input::vbutton(i);
        CHECK(b.x >= 0 && b.y >= 0 && b.x + b.w <= 320 && b.y + b.h <= 240);   // reste dans l'ecran du bas
        CHECK((seen & b.bit) == 0);
        seen |= b.bit;
        input::Raw r;
        r.touch = true;
        r.tx = b.x + b.w / 2;
        r.ty = b.y + b.h / 2;
        CHECK_EQ(input::touch_buttons(r), b.bit);     // le centre d'un bouton n'active que lui
        r.touch = false;
        CHECK_EQ(input::touch_buttons(r), 0);         // doigt leve : plus rien (meme si tx/ty sont conserves)
    }
    input::Raw r;
    r.touch = true;
    r.tx = 0;
    r.ty = 239;
    CHECK_EQ(input::touch_buttons(r), 0);
    // aucun bouton tactile ne recouvre la zone du stick
    input::StickPad sp = input::stick_pad();
    for (int i = 0; i < input::vbutton_count(); i++) {
        const input::VButton& b = input::vbutton(i);
        CHECK(b.y + b.h <= sp.cy - sp.r);
    }
}

void test_input_touch_stick() {
    input::StickPad sp = input::stick_pad();
    input::Raw r;
    r.touch = true;
    int x = 99, y = 99;
    r.tx = sp.cx;
    r.ty = sp.cy;
    CHECK(input::touch_stick_vec(r, x, y));
    CHECK_EQ(x, 0);
    CHECK_EQ(y, 0);
    r.tx = sp.cx + sp.r;
    r.ty = sp.cy;
    CHECK(input::touch_stick_vec(r, x, y));
    CHECK_EQ(x, input::STICK_RANGE);
    CHECK_EQ(y, 0);
    r.tx = sp.cx;
    r.ty = sp.cy - sp.r;                              // vers le haut de l'ecran : y positif
    CHECK(input::touch_stick_vec(r, x, y));
    CHECK_EQ(x, 0);
    CHECK_EQ(y, input::STICK_RANGE);
    r.tx = sp.cx - sp.r / 2;
    r.ty = sp.cy;
    CHECK(input::touch_stick_vec(r, x, y));
    CHECK_EQ(x, -input::STICK_RANGE / 2);
    r.tx = sp.cx + sp.r + 5;                          // un peu au-dela du cercle : tenu a fond
    r.ty = sp.cy;
    CHECK(input::touch_stick_vec(r, x, y));
    CHECK_EQ(x, input::STICK_RANGE);
    r.tx = sp.cx + sp.r + 30;                         // loin du cercle : pas un geste de stick
    CHECK(!input::touch_stick_vec(r, x, y));
    CHECK_EQ(x, 0);
    CHECK_EQ(y, 0);
    r.touch = false;
    r.tx = sp.cx;
    r.ty = sp.cy;
    CHECK(!input::touch_stick_vec(r, x, y));
}

void test_input_packet_sticks() {
    input::StickPad sp = input::stick_pad();
    input::Mapping m = default_mapping(true, true);
    input::Raw r;
    r.cpad_x = 40000;                                 // hors plage : borne au lieu de deborder
    r.cpad_y = -40000;
    proto::InputPacket p = input::build_packet(r, m);
    CHECK_EQ(p.cpad_x, 32767);
    CHECK_EQ(p.cpad_y, -32768);
    r.cpad_x = 100;
    r.cpad_y = -55;
    p = input::build_packet(r, m);
    CHECK_EQ(p.cpad_x, 100);
    CHECK_EQ(p.cpad_y, -55);

    // le doigt sur la zone du stick = stick droit
    input::Raw t;
    t.touch = true;
    t.tx = sp.cx + sp.r;
    t.ty = sp.cy;
    p = input::build_packet(t, m);
    CHECK_EQ(p.cstick_x, input::STICK_RANGE);
    CHECK_EQ(p.cstick_y, 0);
    // un vrai C-stick plus fort que le doigt garde la priorite
    t.cstick_x = 150;
    p = input::build_packet(t, m);
    CHECK_EQ(p.cstick_x, 150);
    // option desactivee : le doigt ne bouge pas le stick
    t.cstick_x = 0;
    m.touch_stick = false;
    p = input::build_packet(t, m);
    CHECK_EQ(p.cstick_x, 0);
    CHECK_EQ(p.cstick_y, 0);
    // un bouton tactile n'est pas un geste de stick
    m.touch_stick = true;
    input::Raw b;
    b.touch = true;
    b.tx = input::vbutton(3).x + 5;
    b.ty = input::vbutton(3).y + 5;
    p = input::build_packet(b, m);
    CHECK_EQ(p.buttons, input::vbutton(3).bit);
    CHECK_EQ(p.cstick_x, 0);
    CHECK_EQ(p.cstick_y, 0);
}

// ==================================================================================== Protocole
void test_protocol_golden_input_packet() {
    // Memes octets dans relay/tests/test_protocol_sync.py
    static const uint8_t GOLDEN[16] = {0x49, 0x01, 0x34, 0x12, 0x03, 0x00, 0x41, 0x00,
                                       0xFF, 0xFF, 0x78, 0x00, 0x88, 0xFF, 0xFF, 0x7F};
    proto::InputPacket p;
    p.flags = 1;
    p.seq = 0x1234;
    p.buttons = 0x00410003;
    p.cpad_x = -1;
    p.cpad_y = 120;
    p.cstick_x = -120;
    p.cstick_y = 32767;
    uint8_t out[proto::INPUT_PKT_SIZE];
    proto::pack_input(p, out);
    CHECK(memcmp(out, GOLDEN, 16) == 0);
}

void test_protocol_golden_video_header() {
    // Memes octets dans relay/tests/test_protocol_sync.py
    static const uint8_t GOLDEN[14] = {0x47, 0x01, 0x02, 0x01, 0x03, 0x00, 0x05, 0x00,
                                       0x0D, 0x0C, 0x0B, 0x0A, 0x61, 0x62};
    proto::VideoHeader h;
    CHECK(proto::parse_video_header(GOLDEN, 14, h));
    CHECK_EQ(h.frame_id, 0x0102);
    CHECK_EQ(h.frag_idx, 3);
    CHECK_EQ(h.frag_cnt, 5);
    CHECK_EQ(h.ts_ms, 0x0A0B0C0Du);
    CHECK(!proto::parse_video_header(GOLDEN, 12, h));    // sans donnees
}

void test_protocol_seq_newer() {
    CHECK(proto::seq_newer(1, 0));
    CHECK(!proto::seq_newer(0, 1));
    CHECK(!proto::seq_newer(5, 5));
    CHECK(proto::seq_newer(0, 65535));
    CHECK(!proto::seq_newer(65535, 0));
    CHECK(proto::seq_newer(0x7FFF, 0));
    CHECK(!proto::seq_newer(0x8000, 0));
}

void test_protocol_parse_state() {
    using proto::GfnState;
    CHECK(proto::parse_state("IDLE") == GfnState::Idle);
    CHECK(proto::parse_state("LAUNCHING") == GfnState::Launching);
    CHECK(proto::parse_state("QUEUE") == GfnState::Queue);
    CHECK(proto::parse_state("LOADING") == GfnState::Loading);
    CHECK(proto::parse_state("STREAMING") == GfnState::Streaming);
    CHECK(proto::parse_state("ERROR") == GfnState::Error);
    CHECK(proto::parse_state("UNTRACKED") == GfnState::Untracked);
    CHECK(proto::parse_state("queue") == GfnState::Unknown);   // la casse compte
    CHECK(proto::parse_state("") == GfnState::Unknown);
    CHECK(proto::parse_state("PAUSED") == GfnState::Unknown);
}

void test_protocol_split_fields() {
    char line[128];
    char* f[8];
    snprintf(line, sizeof(line), "OK\t1\tMon PC\t47971\r\n");
    int n = proto::split_fields(line, f, 8);
    CHECK_EQ(n, 4);
    CHECK_STR(f[0], "OK");
    CHECK_STR(f[2], "Mon PC");
    CHECK_STR(f[3], "47971");

    snprintf(line, sizeof(line), "STATE\tQUEUE\t12\t\t\tFile\n");      // champs vides conserves
    n = proto::split_fields(line, f, 8);
    CHECK_EQ(n, 6);
    CHECK_STR(f[3], "");
    CHECK_STR(f[4], "");
    CHECK_STR(f[5], "File");

    snprintf(line, sizeof(line), "PONG");
    n = proto::split_fields(line, f, 8);
    CHECK_EQ(n, 1);

    snprintf(line, sizeof(line), "a\tb\tc\td");                        // max_fields borne le nombre de champs
    n = proto::split_fields(line, f, 2);
    CHECK_EQ(n, 2);
    CHECK_STR(f[0], "a");
    CHECK_STR(f[1], "b");

    snprintf(line, sizeof(line), "\r\n");
    n = proto::split_fields(line, f, 8);
    CHECK_EQ(n, 1);
    CHECK_STR(f[0], "");
}

struct TestCase {
    const char* name;
    void (*fn)();
};

#define T(name) {#name, name}

const TestCase TESTS[] = {
    T(test_reassembler_single_fragment),
    T(test_reassembler_out_of_order),
    T(test_reassembler_exact_multiple_of_payload),
    T(test_reassembler_duplicates),
    T(test_reassembler_loss_then_newer_frame),
    T(test_reassembler_frame_id_wraparound),
    T(test_reassembler_rejects_garbage),
    T(test_reassembler_64_fragments),
    T(test_reassembler_mismatched_count),
    T(test_reassembler_reset),
    T(test_jitter_constant_delay),
    T(test_jitter_absorbs_network_jitter),
    T(test_jitter_low_latency_never_waits),
    T(test_jitter_smooth_mode_adds_margin),
    T(test_jitter_late_frame_does_not_wait),
    T(test_jitter_timestamp_wraparound),
    T(test_jitter_long_pause_and_reset),
    T(test_settings_defaults),
    T(test_settings_sanitize),
    T(test_settings_roundtrip),
    T(test_settings_load_is_tolerant),
    T(test_settings_missing_file),
    T(test_input_default_mapping),
    T(test_input_start_and_select_are_reserved),
    T(test_input_presets),
    T(test_input_target_names_and_bits),
    T(test_input_touch_buttons),
    T(test_input_touch_stick),
    T(test_input_packet_sticks),
    T(test_protocol_golden_input_packet),
    T(test_protocol_golden_video_header),
    T(test_protocol_seq_newer),
    T(test_protocol_parse_state),
    T(test_protocol_split_fields),
};

}  // namespace

int main() {
    int failed_tests = 0;
    const int n = (int)(sizeof(TESTS) / sizeof(TESTS[0]));
    for (int i = 0; i < n; i++) {
        const int before = g_failed;
        TESTS[i].fn();
        const bool ok = g_failed == before;
        if (!ok) failed_tests++;
        printf("%-52s %s\n", TESTS[i].name, ok ? "ok" : "ECHEC");
    }
    printf("\n%d tests, %d verifications, %d echec(s)\n", n, g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
