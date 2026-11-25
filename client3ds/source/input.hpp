// Entrees : lecture des boutons de la 3DS, configuration des touches, zones tactiles et
// construction du paquet envoye au relais.
//
// Les boutons de la 3DS ne sont jamais envoyes "bruts" : chacun est associe (reglage utilisateur) a
// une commande de manette Xbox, puis traduit en bit du protocole (proto::B_*). Start et Select de
// la 3DS sont reserves a l'interface (menu / options de streaming) et ne partent jamais vers le jeu ;
// Start, Back et Guide du jeu passent par les boutons tactiles de l'ecran du bas.
#pragma once

#include <stdint.h>

#include "protocol.hpp"

namespace input {

// Boutons physiques configurables de la 3DS. L'ordre fixe aussi les bits de Raw::held.
enum Src {
    SRC_A, SRC_B, SRC_X, SRC_Y, SRC_L, SRC_R, SRC_ZL, SRC_ZR,
    SRC_DUP, SRC_DDOWN, SRC_DLEFT, SRC_DRIGHT,
    NUM_SRC
};
constexpr uint32_t K_START = 1u << 12;   // reserve a l'interface
constexpr uint32_t K_SELECT = 1u << 13;  // reserve a l'interface

// Commandes de la manette Xbox que l'on peut associer a un bouton.
enum Tgt {
    TGT_NONE, TGT_A, TGT_B, TGT_X, TGT_Y, TGT_LB, TGT_RB, TGT_LT, TGT_RT, TGT_L3, TGT_R3,
    TGT_START, TGT_BACK, TGT_GUIDE, TGT_DUP, TGT_DDOWN, TGT_DLEFT, TGT_DRIGHT,
    NUM_TGT
};

const char* src_label(int s);              // nom affiche : "A", "ZL", "Croix haut"...
const char* tgt_label(int t);              // nom affiche : "Aucun", "LB", "Croix bas"...
const char* tgt_key(int t);                // nom dans le fichier de reglages : "DUP", "LB"...
int tgt_from_key(const char* key);         // -1 si inconnu
uint32_t tgt_bit(int t);                   // bit du protocole correspondant (0 pour TGT_NONE)

// Etat brut lu sur la console, une fois par image. Les champs non touches sont conserves d'un appel
// a l'autre (derniere position tactile, par exemple).
struct Raw {
    uint32_t held = 0;       // bits (1 << SRC_*) | K_START | K_SELECT
    uint32_t pressed = 0;    // front montant depuis l'image precedente
    uint32_t released = 0;
    int cpad_x = 0;          // Circle Pad ; y positif = haut
    int cpad_y = 0;
    int cstick_x = 0;        // C-stick (New 3DS ou Circle Pad Pro)
    int cstick_y = 0;
    bool touch = false;
    bool touch_pressed = false;
    bool touch_released = false;
    int tx = 0;              // position tactile (ecran du bas, 320 x 240)
    int ty = 0;
};

void poll(Raw& r);                          // lit le materiel (ou l'etat injecte par les tests)
#ifndef __3DS__
void host_inject(const Raw& r);             // tests sur PC : etat que poll() renverra
#endif

// ---- configuration des touches
constexpr int STICK_RANGE = 120;            // deviation du C-stick tactile a fond (comme cstick_max cote relais)

struct Mapping {
    uint8_t map[NUM_SRC];                   // commande Xbox (Tgt) associee a chaque bouton
    bool touch_stick;                       // zone tactile = stick droit
    bool new3ds;                            // annonce au relais (flag du paquet)
};

void default_map(uint8_t map[NUM_SRC]);
int num_presets();
const char* preset_name(int i);
void preset_map(int i, uint8_t map[NUM_SRC]);
int detect_preset(const uint8_t map[NUM_SRC]);   // -1 si la configuration est personnalisee

// ---- zones tactiles (mode streaming, ecran du bas 320 x 240)
struct VButton {
    int16_t x, y, w, h;
    uint32_t bit;                           // bit du protocole
    const char* label;
};
int vbutton_count();
const VButton& vbutton(int i);
struct StickPad {
    int cx, cy, r;
};
StickPad stick_pad();

uint32_t touch_buttons(const Raw& raw);                     // bits des boutons tactiles enfonces
bool touch_stick_vec(const Raw& raw, int& x, int& y);       // vrai si le doigt est dans la zone du stick

// Construit le paquet d'entrees (sans numero de sequence : il est donne a l'envoi).
proto::InputPacket build_packet(const Raw& raw, const Mapping& m);

}  // namespace input
