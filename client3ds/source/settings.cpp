#include "settings.hpp"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "protocol.hpp"

static const char* const SRC_KEYS[input::NUM_SRC] = {
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "DUP", "DDOWN", "DLEFT", "DRIGHT"};

void Settings::set_defaults() {
    host[0] = '\0';
    port = proto::PORT_CONTROL;
    pin[0] = '\0';
    auto_connect = true;
    auto_reconnect = true;
    strcpy(profile, "balanced");
    buffer_mode = BUFFER_ADAPTIVE;
    overlay = 1;
    touch_stick = true;
    speedup = true;
    input::default_map(keymap);
}

static void copy_str(char* dst, size_t cap, const char* src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool printable_ascii(const char* s) {
    for (; *s; s++)
        if ((unsigned char)*s < 32 || (unsigned char)*s > 126) return false;
    return true;
}

void Settings::sanitize() {
    host[sizeof(host) - 1] = '\0';
    pin[sizeof(pin) - 1] = '\0';
    profile[sizeof(profile) - 1] = '\0';
    if (!printable_ascii(host)) host[0] = '\0';
    if (!printable_ascii(pin)) pin[0] = '\0';
    // Un profil ne contient ni espace ni tabulation (il est envoye tel quel dans une ligne de controle).
    bool ok = profile[0] != '\0';
    for (const char* p = profile; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') ok = false;
    if (!ok) strcpy(profile, "balanced");
    if (port < 1 || port > 65535) port = proto::PORT_CONTROL;
    if (buffer_mode < 0 || buffer_mode > 2) buffer_mode = BUFFER_ADAPTIVE;
    if (overlay < 0 || overlay > 2) overlay = 1;
    for (int i = 0; i < input::NUM_SRC; i++)
        if (keymap[i] >= input::NUM_TGT) keymap[i] = (uint8_t)input::TGT_NONE;
}

const char* settings_default_path() {
#ifdef __3DS__
    return "sdmc:/3ds/gfn3ds/settings.ini";
#else
    return "gfn3ds_settings.ini";
#endif
}

static bool parse_bool(const char* v) { return !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes"); }

static char* trim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = '\0';
    return s;
}

bool settings_load(Settings& s, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* l = trim(line);
        if (*l == '\0' || *l == '#' || *l == ';') continue;
        char* eq = strchr(l, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = trim(l);
        char* val = trim(eq + 1);
        if (!strcmp(key, "host")) copy_str(s.host, sizeof(s.host), val);
        else if (!strcmp(key, "port")) s.port = atoi(val);
        else if (!strcmp(key, "pin")) copy_str(s.pin, sizeof(s.pin), val);
        else if (!strcmp(key, "auto_connect")) s.auto_connect = parse_bool(val);
        else if (!strcmp(key, "auto_reconnect")) s.auto_reconnect = parse_bool(val);
        else if (!strcmp(key, "profile")) copy_str(s.profile, sizeof(s.profile), val);
        else if (!strcmp(key, "buffer_mode")) s.buffer_mode = atoi(val);
        else if (!strcmp(key, "overlay")) s.overlay = atoi(val);
        else if (!strcmp(key, "touch_stick")) s.touch_stick = parse_bool(val);
        else if (!strcmp(key, "speedup")) s.speedup = parse_bool(val);
        else if (!strncmp(key, "map.", 4)) {
            int tgt = input::tgt_from_key(val);
            if (tgt < 0) continue;
            for (int i = 0; i < input::NUM_SRC; i++)
                if (!strcmp(key + 4, SRC_KEYS[i])) s.keymap[i] = (uint8_t)tgt;
        }
    }
    fclose(f);
    s.sanitize();
    return true;
}

// Cree les dossiers intermediaires (un par un, les erreurs "existe deja" sont sans importance).
static void make_parent_dirs(const char* path) {
    char tmp[256];
    copy_str(tmp, sizeof(tmp), path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(tmp, 0777);
        *p = '/';
    }
}

bool settings_save(const Settings& s, const char* path) {
    make_parent_dirs(path);
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "# Reglages de GeForce NOW 3DS (fichier genere par l'application)\n");
    fprintf(f, "host=%s\n", s.host);
    fprintf(f, "port=%d\n", s.port);
    fprintf(f, "pin=%s\n", s.pin);
    fprintf(f, "auto_connect=%d\n", s.auto_connect ? 1 : 0);
    fprintf(f, "auto_reconnect=%d\n", s.auto_reconnect ? 1 : 0);
    fprintf(f, "profile=%s\n", s.profile);
    fprintf(f, "buffer_mode=%d\n", s.buffer_mode);
    fprintf(f, "overlay=%d\n", s.overlay);
    fprintf(f, "touch_stick=%d\n", s.touch_stick ? 1 : 0);
    fprintf(f, "speedup=%d\n", s.speedup ? 1 : 0);
    for (int i = 0; i < input::NUM_SRC; i++) fprintf(f, "map.%s=%s\n", SRC_KEYS[i], input::tgt_key(s.keymap[i]));
    bool ok = ferror(f) == 0;
    if (fclose(f) != 0) ok = false;
    return ok;
}
