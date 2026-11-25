// Reglages utilisateur, enregistres dans sdmc:/3ds/gfn3ds/settings.ini (fichier texte "cle=valeur").
#pragma once

#include <stdint.h>

#include "input.hpp"

struct Settings {
    char host[64];          // adresse du relais ("" : a choisir parmi les relais detectes)
    int port;               // port de controle du relais
    char pin[16];           // code PIN du relais ("" : aucun)
    bool auto_connect;      // se connecter des le demarrage
    bool auto_reconnect;    // se reconnecter tout seul apres une coupure
    char profile[16];       // dernier profil de streaming (perf / balanced / latency / compat)
    int buffer_mode;        // 0 adaptatif, 1 latence minimale, 2 fluide
    int overlay;            // 0 aucun, 1 compact (FPS, latence), 2 detaille
    bool touch_stick;       // la zone tactile du bas fait office de stick droit
    bool speedup;           // New 3DS : 804 MHz + cache L2
    uint8_t keymap[input::NUM_SRC];  // commande Xbox associee a chaque bouton (input::Tgt)

    Settings() { set_defaults(); }
    void set_defaults();
    void sanitize();        // ramene toute valeur invalide a une valeur sure
};

constexpr int BUFFER_ADAPTIVE = 0;
constexpr int BUFFER_LOW_LATENCY = 1;
constexpr int BUFFER_SMOOTH = 2;

// Retourne faux si le fichier n'existe pas ou est illisible (s est alors laisse aux valeurs par defaut).
bool settings_load(Settings& s, const char* path);
bool settings_save(const Settings& s, const char* path);
const char* settings_default_path();
