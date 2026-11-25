// Protocole relais PC <-> client 3DS, version 1.
//
// Miroir de relay/gfn3ds_relay/protocol.py (la reference). Les constantes portent le meme nom
// et la meme valeur : tests/test_protocol_sync.py le verifie a chaque lancement des tests.
//
// Quatre canaux :
//   controle : TCP, lignes de texte ASCII, champs separes par des tabulations
//   video    : UDP, relais -> 3DS, images JPEG fragmentees (en-tete de 12 octets)
//   entrees  : UDP, 3DS -> relais, paquets de 16 octets
//   balises  : UDP, relais -> broadcast, ecoutees par la 3DS (decouverte)
// Tout est en little-endian ; les lectures/ecritures ci-dessous sont explicites (octet par octet),
// donc ce fichier se compile et se teste aussi sur PC.
#pragma once

#include <stdint.h>
#include <string.h>

namespace proto {

constexpr int PROTO_VERSION = 1;

// Ports par defaut
constexpr int PORT_CONTROL = 47970;  // TCP, le relais ecoute
constexpr int PORT_INPUT = 47971;    // UDP, le relais ecoute (entrees 3DS -> PC)
constexpr int PORT_VIDEO = 47972;    // UDP, la 3DS ecoute (video PC -> 3DS)
constexpr int PORT_BEACON = 47973;   // UDP, la 3DS ecoute (balises du relais)

// ---------------------------------------------------------------- video
constexpr int SCREEN_W = 400;  // ecran du haut de la 3DS
constexpr int SCREEN_H = 240;

constexpr int VIDEO_MAGIC = 0x47;
constexpr int KIND_JPEG = 1;
constexpr int FRAG_PAYLOAD = 1200;           // octets de JPEG par datagramme
constexpr int MAX_FRAGS = 64;                // 64 * 1200 > MAX_FRAME_BYTES
constexpr int MAX_FRAME_BYTES = 64 * 1024;   // taille max d'une image JPEG
constexpr int VIDEO_HDR_SIZE = 12;           // magic, kind, frame_id, frag_idx, frag_cnt, ts_ms

// --------------------------------------------------------------- entrees
// Bits du champ "buttons". Numerotation propre au protocole : input.cpp traduit explicitement les
// KEY_* de libctru, on ne suppose donc rien sur leurs valeurs.
constexpr uint32_t B_A = 1u << 0;
constexpr uint32_t B_B = 1u << 1;
constexpr uint32_t B_X = 1u << 2;
constexpr uint32_t B_Y = 1u << 3;
constexpr uint32_t B_L = 1u << 4;
constexpr uint32_t B_R = 1u << 5;
constexpr uint32_t B_ZL = 1u << 6;
constexpr uint32_t B_ZR = 1u << 7;
constexpr uint32_t B_START = 1u << 8;
constexpr uint32_t B_SELECT = 1u << 9;
constexpr uint32_t B_DUP = 1u << 10;
constexpr uint32_t B_DDOWN = 1u << 11;
constexpr uint32_t B_DLEFT = 1u << 12;
constexpr uint32_t B_DRIGHT = 1u << 13;
// Boutons virtuels (zones tactiles de l'ecran du bas) : le relais les associe toujours a la meme
// commande Xbox, quelle que soit sa configuration.
constexpr uint32_t B_VLT = 1u << 16;     // gachette gauche (Old 3DS sans ZL)
constexpr uint32_t B_VRT = 1u << 17;     // gachette droite (Old 3DS sans ZR)
constexpr uint32_t B_L3 = 1u << 18;      // clic du stick gauche
constexpr uint32_t B_R3 = 1u << 19;      // clic du stick droit
constexpr uint32_t B_VSTART = 1u << 20;
constexpr uint32_t B_VBACK = 1u << 21;
constexpr uint32_t B_GUIDE = 1u << 22;

constexpr int INPUT_MAGIC = 0x49;
constexpr int INPUT_FLAG_NEW3DS = 0x01;
constexpr int INPUT_PKT_SIZE = 16;  // magic, flags, seq, buttons, cpad x/y, cstick x/y

constexpr int MAX_LINE = 512;  // longueur maximale d'une ligne de controle

// ------------------------------------------------------ lecture / ecriture
inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline void wr16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}
inline void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)(v >> 24);
}

// Vrai si seq est plus recent que last (compteurs de 16 bits qui bouclent).
inline bool seq_newer(uint16_t seq, uint16_t last) {
    uint16_t d = (uint16_t)(seq - last);
    return d > 0 && d < 0x8000;
}

// ---------------------------------------------------------------- video
struct VideoHeader {
    uint8_t magic = 0;
    uint8_t kind = 0;
    uint16_t frame_id = 0;
    uint16_t frag_idx = 0;
    uint16_t frag_cnt = 0;
    uint32_t ts_ms = 0;
};

// Valide l'en-tete d'un datagramme video (memes regles que parse_video_packet en Python).
inline bool parse_video_header(const uint8_t* d, int len, VideoHeader& h) {
    if (len <= VIDEO_HDR_SIZE) return false;
    h.magic = d[0];
    h.kind = d[1];
    h.frame_id = rd16(d + 2);
    h.frag_idx = rd16(d + 4);
    h.frag_cnt = rd16(d + 6);
    h.ts_ms = rd32(d + 8);
    return h.magic == VIDEO_MAGIC && h.kind == KIND_JPEG && h.frag_cnt != 0 &&
           h.frag_cnt <= MAX_FRAGS && h.frag_idx < h.frag_cnt;
}

// --------------------------------------------------------------- entrees
struct InputPacket {
    uint8_t flags = 0;
    uint16_t seq = 0;
    uint32_t buttons = 0;
    int16_t cpad_x = 0;
    int16_t cpad_y = 0;     // positif = haut (comme libctru et XInput)
    int16_t cstick_x = 0;
    int16_t cstick_y = 0;
};

inline void pack_input(const InputPacket& p, uint8_t out[INPUT_PKT_SIZE]) {
    out[0] = (uint8_t)INPUT_MAGIC;
    out[1] = p.flags;
    wr16(out + 2, p.seq);
    wr32(out + 4, p.buttons);
    wr16(out + 8, (uint16_t)p.cpad_x);
    wr16(out + 10, (uint16_t)p.cpad_y);
    wr16(out + 12, (uint16_t)p.cstick_x);
    wr16(out + 14, (uint16_t)p.cstick_y);
}

// -------------------------------------------------------------- controle
// Etat annonce par le relais (ligne STATE).
// Untracked : jeu lance mais avancement non suivi par le relais (journaux non calibres) : la console affiche
// l'ecran de GeForce NOW du PC tel quel.
enum class GfnState : uint8_t { Unknown = 0, Idle, Launching, Queue, Loading, Streaming, Error, Untracked };

inline GfnState parse_state(const char* s) {
    if (!strcmp(s, "IDLE")) return GfnState::Idle;
    if (!strcmp(s, "LAUNCHING")) return GfnState::Launching;
    if (!strcmp(s, "QUEUE")) return GfnState::Queue;
    if (!strcmp(s, "LOADING")) return GfnState::Loading;
    if (!strcmp(s, "STREAMING")) return GfnState::Streaming;
    if (!strcmp(s, "ERROR")) return GfnState::Error;
    if (!strcmp(s, "UNTRACKED")) return GfnState::Untracked;
    return GfnState::Unknown;
}

// Decoupe une ligne de controle sur place : remplace les tabulations par des NUL et range les
// pointeurs des champs dans f[]. Retire d'abord les \r\n finaux. Retourne le nombre de champs.
inline int split_fields(char* line, char** f, int max_fields) {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    int count = 0;
    char* p = line;
    while (count < max_fields) {
        f[count++] = p;
        char* tab = strchr(p, '\t');
        if (!tab) break;
        *tab = '\0';
        p = tab + 1;
    }
    return count;
}

}  // namespace proto
