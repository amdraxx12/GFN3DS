// Interface : voir ui.hpp.
#include "ui.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace gfx;

namespace {

// ------------------------------------------------------------------------------ identifiants
// Une ligne de menu = un identifiant ; activate() sait quoi en faire.
enum Id : int {
    ID_NONE = 0,
    ID_CONNECT = 1, ID_DISCONNECT, ID_REFRESH, ID_WATCH,
    ID_HOST = 10, ID_PORT, ID_PIN, ID_AUTO_RECONNECT, ID_AUTO_CONNECT,
    ID_PRESET = 20, ID_TOUCH_STICK, ID_KEYS_RESET, ID_KEYTEST,
    ID_PROFILE = 30, ID_BUFFER, ID_OVERLAY, ID_SPEEDUP, ID_QUIT,
    ID_RESUME = 40, ID_SM_OPTIONS, ID_SM_KEYS, ID_SM_STOP, ID_SM_QUIT,
    ID_QUIT_YES = 50, ID_QUIT_NO,
    ID_RELAY_BASE = 100,     // + indice dans la liste des relais detectes (8 au plus)
    ID_KEY_BASE = 200,       // + bouton de la 3DS (input::Src)
    ID_GAME_BASE = 1000,     // + indice dans la liste des jeux
};

// Lignes dont la valeur se change avec gauche / droite (les autres ouvrent le clavier).
bool cycles(int id) {
    return id == ID_PRESET || id == ID_PROFILE || id == ID_BUFFER || id == ID_OVERLAY ||
           (id >= ID_KEY_BASE && id < ID_KEY_BASE + input::NUM_SRC);
}

// ------------------------------------------------------------------------------ geometrie (ecran du bas)
const int SCREEN_W = 320;
const int TAB_H = 24;
const int BAR_Y = 216;
const int BAR_H = 24;
const int LIST_Y = TAB_H;
const int LIST_H = BAR_Y - TAB_H;
const int NUM_TABS = 4;
const int TOAST_MS = 4000;
const int KT_BACK_X = 110, KT_BACK_Y = 192, KT_BACK_W = 100, KT_BACK_H = 24;

// ------------------------------------------------------------------------------ utilitaires
void copy_str(char* dst, size_t cap, const char* src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int clamp_idx(int v, int n) { return v < 0 ? 0 : (v >= n ? n - 1 : v); }

// Longueur (octets) du caractere UTF-8 qui commence en p ; une sequence tronquee compte pour ce qu'elle contient.
int utf8_step(const char* p) {
    unsigned char c = (unsigned char)*p;
    int n = c < 0x80 ? 1 : ((c >> 5) == 6 ? 2 : ((c >> 4) == 14 ? 3 : ((c >> 3) == 30 ? 4 : 1)));
    for (int i = 1; i < n; i++)
        if (((unsigned char)p[i] & 0xC0) != 0x80) return i;
    return n;
}

int utf8_count(const char* s, int from, int to) {
    int n = 0;
    for (int i = from; i < to; i += utf8_step(s + i)) n++;
    return n;
}

// Coupe un texte en lignes de max_chars caracteres, a l'espace quand c'est possible.
// start[k] / len[k] : position et longueur (octets) de la ligne k. Retourne le nombre de lignes (>= 1).
int wrap_lines(const char* s, int max_chars, int* start, int* len, int max_lines) {
    if (max_chars < 1) max_chars = 1;
    int n = 0;
    int pos = 0, line_start = 0, chars = 0, space_pos = -1;
    for (;;) {
        char c = s[pos];
        if (c == '\0') break;
        if (n >= max_lines) return n;
        if (c == ' ' && chars == 0 && pos == line_start && n > 0) {   // pas d'espace en tete des lignes suivantes
            pos++;
            line_start = pos;
            continue;
        }
        if (c == ' ') space_pos = pos;
        if (chars + 1 > max_chars && c != ' ') {
            if (space_pos >= 0) {
                start[n] = line_start;
                len[n] = space_pos - line_start;
                n++;
                line_start = space_pos + 1;
                chars = utf8_count(s, line_start, pos);
                space_pos = -1;
            } else {
                start[n] = line_start;
                len[n] = pos - line_start;
                n++;
                line_start = pos;
                chars = 0;
            }
            continue;
        }
        chars++;
        pos += utf8_step(s + pos);
    }
    if (n < max_lines && (pos > line_start || n == 0)) {
        start[n] = line_start;
        len[n] = pos - line_start;
        n++;
    }
    return n;
}

// Copie la ligne k d'un texte coupe par wrap_lines.
void line_copy(char* out, size_t cap, const char* s, int start, int len) {
    int n = len < (int)cap - 1 ? len : (int)cap - 1;
    if (n < 0) n = 0;
    memcpy(out, s + start, (size_t)n);
    out[n] = '\0';
}

uint32_t fnv(uint32_t h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}
uint32_t fnv_str(uint32_t h, const char* s) { return fnv(h, s, strlen(s)); }

void fmt_duration(char* out, size_t cap, int sec) {
    if (sec < 0) snprintf(out, cap, "inconnue");
    else if (sec < 60) snprintf(out, cap, "%d s", sec);
    else if (sec < 3600) snprintf(out, cap, "%d min %02d s", sec / 60, sec % 60);
    else snprintf(out, cap, "%d h %02d min", sec / 3600, (sec % 3600) / 60);
}

void fmt_clock(char* out, size_t cap, uint64_t ms) {
    unsigned sec = (unsigned)(ms / 1000);
    snprintf(out, cap, "%02u:%02u", sec / 60, sec % 60);
}

// Compare deux libelles sans tenir compte de la casse (ASCII), des espaces et des points de suspension
// finaux ("..." ou U+2026) : sert a ne pas afficher deux fois la meme chose.
bool same_text(const char* a, const char* b) {
    auto norm = [](const char* s, char* out, size_t cap) {
        size_t n = 0;
        for (; *s && n + 1 < cap; s++) out[n++] = (*s >= 'A' && *s <= 'Z') ? (char)(*s + 32) : *s;
        for (;;) {
            if (n >= 3 && (uint8_t)out[n - 3] == 0xE2 && (uint8_t)out[n - 2] == 0x80 && (uint8_t)out[n - 1] == 0xA6) n -= 3;
            else if (n >= 1 && (out[n - 1] == '.' || out[n - 1] == ' ')) n--;
            else break;
        }
        out[n] = '\0';
    };
    char x[128], y[128];
    norm(a, x, sizeof(x));
    norm(b, y, sizeof(y));
    return strcmp(x, y) == 0;
}

// Le relais envoie un libelle « standard » avec chaque etat ("Chargement du jeu"...) : l'interface l'affiche
// deja a sa maniere, donc on ne le repete pas. Un texte different (ex. une erreur precise) reste affiche.
bool generic_state_text(proto::GfnState st, const char* t) {
    static const char* const queue[] = {"File d'attente", "En file d'attente"};
    static const char* const launching[] = {"Lancement", "Lancement de GeForce NOW", "Lancement du jeu"};
    static const char* const loading[] = {"Chargement", "Chargement du jeu"};
    static const char* const untracked[] = {"File d'attente non suivie"};
    const char* const* list = nullptr;
    int n = 0;
    switch (st) {
        case proto::GfnState::Queue: list = queue; n = 2; break;
        case proto::GfnState::Launching: list = launching; n = 3; break;
        case proto::GfnState::Loading: list = loading; n = 2; break;
        case proto::GfnState::Untracked: list = untracked; n = 1; break;
        default: break;
    }
    for (int i = 0; i < n; i++)
        if (same_text(t, list[i])) return true;
    return false;
}

// Roue de 12 points dont l'un est plus clair : tourne avec "phase".
const int8_t COS12[12] = {100, 87, 50, 0, -50, -87, -100, -87, -50, 0, 50, 87};
const int8_t SIN12[12] = {0, 50, 87, 100, 87, 50, 0, -50, -87, -100, -87, -50};

void spinner(const Surface& s, int cx, int cy, int r, int phase, Color c, Color bg) {
    for (int i = 0; i < 12; i++) {
        int age = ((phase - i) % 12 + 12) % 12;
        int t = 256 - age * 22;
        if (t < 40) t = 40;
        int x = cx + COS12[i] * r / 100;
        int y = cy - SIN12[i] * r / 100;
        fill_disc(s, x, y, r >= 14 ? 3 : 2, mix(bg, c, t));
    }
}

// Texte long centre sur plusieurs lignes ; retourne l'ordonnee suivante.
int center_wrapped(const Surface& s, int cx, int y, const char* txt, Color c, int max_chars, int max_lines) {
    int st[6], ln[6];
    if (max_lines > 6) max_lines = 6;
    int n = wrap_lines(txt, max_chars, st, ln, max_lines);
    for (int k = 0; k < n; k++) {
        char tmp[96];
        line_copy(tmp, sizeof(tmp), txt, st[k], ln[k]);
        text_center(s, cx, y, tmp, c);
        y += 11;
    }
    return y;
}

// Petit cadre sombre avec des lignes de texte (indicateurs par-dessus la video).
void overlay_box(const Surface& s, int x, int y, const char* const* lines, int n) {
    int w = 0;
    for (int i = 0; i < n; i++) {
        int tw = text_width(lines[i]);
        if (tw > w) w = tw;
    }
    shade_rect(s, x, y, w + 8, n * 10 + 6);
    for (int i = 0; i < n; i++) text(s, x + 4, y + 4 + i * 10, lines[i], col::WHITE);
}

}  // namespace

// =============================================================================================== RowList
UI::Row& UI::RowList::add(Row::Kind kind, int id, const char* label, const char* value) {
    rows_.push_back(Row());
    Row& r = rows_.back();
    r.kind = kind;
    r.id = id;
    copy_str(r.label, sizeof(r.label), label ? label : "");
    copy_str(r.value, sizeof(r.value), value ? value : "");
    return r;
}

int UI::RowList::height_of(const Row& r) const {
    switch (r.kind) {
        case Row::Header: return 18;
        case Row::Button: return 24;
        case Row::Info: {
            int st[4], ln[4];
            int n = wrap_lines(r.label, (aw_ - 20) / GLYPH, st, ln, 4);
            return 6 + 11 * (n < 1 ? 1 : n);
        }
        default: return 22;
    }
}

void UI::RowList::end() {
    off_.resize(rows_.size());
    int y = 0;
    for (size_t i = 0; i < rows_.size(); i++) {
        off_[i] = y;
        y += height_of(rows_[i]);
    }
    total_ = y + 4;
    sel_ = -1;
    for (size_t i = 0; i < rows_.size(); i++) {
        if (rows_[i].id == sel_id_ && selectable(rows_[i])) {
            sel_ = (int)i;
            break;
        }
    }
    if (sel_ < 0) {
        for (size_t i = 0; i < rows_.size(); i++) {
            if (selectable(rows_[i])) {
                sel_ = (int)i;
                break;
            }
        }
    }
    sel_id_ = sel_ >= 0 ? rows_[(size_t)sel_].id : -1;
    int max_scroll = total_ > ah_ ? total_ - ah_ : 0;
    if (scroll_ > max_scroll) scroll_ = max_scroll;
    if (scroll_ < 0) scroll_ = 0;
}

void UI::RowList::ensure_visible() {
    if (sel_ < 0 || sel_ >= (int)rows_.size()) return;
    int top = off_[(size_t)sel_];
    int bot = top + height_of(rows_[(size_t)sel_]);
    if (sel_ > 0 && rows_[(size_t)sel_ - 1].kind == Row::Header) top = off_[(size_t)sel_ - 1];   // garde l'en-tete a l'ecran
    if (top < scroll_) scroll_ = top;
    if (bot > scroll_ + ah_) scroll_ = bot - ah_;
    int max_scroll = total_ > ah_ ? total_ - ah_ : 0;
    if (scroll_ > max_scroll) scroll_ = max_scroll;
    if (scroll_ < 0) scroll_ = 0;
}

void UI::RowList::move(int dir) {
    int n = (int)rows_.size();
    if (n == 0 || sel_ < 0) return;
    int i = sel_;
    for (int k = 0; k < n; k++) {
        i = (i + dir + n) % n;
        if (selectable(rows_[(size_t)i])) {
            sel_ = i;
            sel_id_ = rows_[(size_t)i].id;
            ensure_visible();
            return;
        }
    }
}

void UI::RowList::select_index(int i) {
    if (i < 0 || i >= (int)rows_.size() || !selectable(rows_[(size_t)i])) return;
    sel_ = i;
    sel_id_ = rows_[(size_t)i].id;
}

int UI::RowList::hit(int x, int y) const {
    if (x < ax_ || x >= ax_ + aw_ || y < ay_ || y >= ay_ + ah_) return -1;
    int yy = y - ay_ + scroll_;
    for (size_t i = 0; i < rows_.size(); i++) {
        int h = height_of(rows_[i]);
        if (yy >= off_[i] && yy < off_[i] + h) return (int)i;
    }
    return -1;
}

void UI::RowList::scroll_by(int dy) {
    int max_scroll = total_ > ah_ ? total_ - ah_ : 0;
    scroll_ += dy;
    if (scroll_ > max_scroll) scroll_ = max_scroll;
    if (scroll_ < 0) scroll_ = 0;
}

void UI::RowList::draw(const Surface& s) const {
    const bool bar = total_ > ah_;
    const int right = ax_ + aw_ - (bar ? 8 : 4);
    for (size_t i = 0; i < rows_.size(); i++) {
        const Row& r = rows_[i];
        int h = height_of(r);
        int y = ay_ + off_[i] - scroll_;
        if (y + h <= ay_ || y >= ay_ + ah_) continue;
        const bool sel = (int)i == sel_;

        switch (r.kind) {
            case Row::Header:
                text(s, ax_ + 10, y + 7, r.label, col::GREEN);
                fill_rect(s, ax_ + 10, y + h - 2, right - ax_ - 14, 1, col::LINE);
                break;

            case Row::Info: {
                int st[4], ln[4];
                int n = wrap_lines(r.label, (aw_ - 20) / GLYPH, st, ln, 4);
                for (int k = 0; k < n; k++) {
                    char tmp[96];
                    line_copy(tmp, sizeof(tmp), r.label, st[k], ln[k]);
                    text(s, ax_ + 10, y + 3 + k * 11, tmp, r.color);
                }
                break;
            }

            case Row::Button: {
                Color bg;
                if (r.on) bg = sel ? col::GREEN : col::GREEN_DK;
                else if (r.color == col::RED) bg = sel ? col::RED : mix(col::RED, col::PANEL, 150);
                else bg = sel ? col::LINE : col::PANEL_HI;
                fill_rrect(s, ax_ + 6, y + 2, right - ax_ - 8, h - 4, 4, bg);
                Color fg = !r.enabled ? col::DIM : ((r.on && sel) ? col::BLACK : col::WHITE);
                text_center(s, (ax_ + 6 + right - 2) / 2, y + (h - GLYPH) / 2, r.label, fg);
                break;
            }

            default: {   // Item, Value, Toggle
                fill_rrect(s, ax_ + 6, y + 1, right - ax_ - 8, h - 2, 4, sel ? col::PANEL_HI : col::PANEL);
                if (sel) fill_rect(s, ax_ + 6, y + 4, 3, h - 8, col::GREEN);
                const Color fg = !r.enabled ? col::DIM : (sel ? col::WHITE : col::TEXT);
                const int ty = y + (h - GLYPH) / 2;
                const int lx = ax_ + 14;

                if (r.kind == Row::Value) {
                    int vw = text_width(r.value);
                    int vx = right - 18 - vw;
                    text_fit(s, lx, ty, r.label, fg, vx - 14 - lx);
                    text(s, vx, ty, r.value, sel ? col::GREEN : col::TEXT);
                    if (cycles(r.id)) {
                        text(s, vx - 10, ty, "<", sel ? col::GREEN : col::DIM);
                        text(s, right - 14, ty, ">", sel ? col::GREEN : col::DIM);
                    }
                } else if (r.kind == Row::Toggle) {
                    text_fit(s, lx, ty, r.label, fg, right - 60 - lx);
                    int px = right - 12 - 30, py = y + (h - 14) / 2;
                    Color pc = !r.enabled ? col::PANEL_HI : (r.on ? col::GREEN : col::LINE);
                    fill_rrect(s, px, py, 30, 14, 6, pc);
                    fill_disc(s, r.on ? px + 22 : px + 8, py + 7, 5, r.enabled ? col::WHITE : col::DIM);
                } else {     // Item
                    if (sel && r.pill[0]) {
                        int pw = text_width(r.pill) + 12;
                        int px = right - 6 - pw;
                        fill_rrect(s, px, y + 3, pw, h - 6, 4, col::GREEN);
                        text(s, px + 6, ty, r.pill, col::BLACK);
                        text_fit(s, lx, ty, r.label, fg, px - 6 - lx);
                    } else {
                        int vw = r.value[0] ? text_width(r.value) : 0;
                        text_fit(s, lx, ty, r.label, fg, right - 14 - (vw ? vw + 8 : 0) - lx);
                        if (vw) text(s, right - 10 - vw, ty, r.value, col::DIM);
                    }
                }
                break;
            }
        }
    }

    if (bar) {   // ascenseur
        int th = ah_ * ah_ / total_;
        if (th < 12) th = 12;
        int max_scroll = total_ - ah_;
        int ty = ay_ + (ah_ - th) * scroll_ / (max_scroll > 0 ? max_scroll : 1);
        fill_rect(s, ax_ + aw_ - 5, ay_, 3, ah_, col::PANEL);
        fill_rrect(s, ax_ + aw_ - 5, ty, 3, th, 1, col::DIM);
    }
}

// ===================================================================================== construction
UI::UI(Settings& settings, GfnClient& gfn, VideoDecoder& video) : settings_(settings), gfn_(gfn), video_(video) {
    list_.set_area(0, LIST_Y, SCREEN_W, LIST_H);
    video_.set_buffer_mode(settings_.buffer_mode);
}

input::Mapping UI::mapping() const {
    input::Mapping m;
    memcpy(m.map, settings_.keymap, sizeof(m.map));
    m.touch_stick = settings_.touch_stick;
    m.new3ds = plat::is_new3ds();
    return m;
}

input::Raw UI::game_input(const input::Raw& raw) {
    if (page_ != Page::Pad) return input::Raw();
    input::Raw r = raw;
    r.held &= ~(swallow_mask_ | input::K_START | input::K_SELECT);
    r.pressed = 0;
    r.released = 0;
    if (swallow_touch_) {
        r.touch = false;
        r.touch_pressed = false;
        r.touch_released = false;
    }
    return r;
}

void UI::show_toast(const char* msg) {
    if (!msg || !msg[0]) return;
    copy_str(toast_, sizeof(toast_), msg);
    toast_until_ = now_ + TOAST_MS;
    if (toast_until_ == 0) toast_until_ = 1;
    bottom_dirty_ = true;
}

void UI::save_settings() {
    settings_.sanitize();
    settings_save(settings_, settings_default_path());   // meilleur effort : une carte SD en lecture seule n'est pas fatale
}

void UI::set_page(Page p, Page back) {
    page_ = p;
    back_page_ = back;
    list_.select_id(p == Page::QuitConfirm ? ID_QUIT_NO : -1);
    list_.reset_scroll();
    touch_down_ = false;
    touch_dragging_ = false;
    hold_dir_ = 0;
    if (p == Page::Pad) {
        swallow_mask_ = raw_.held;
        swallow_touch_ = raw_.touch;
    }
    kt_sig_ = ~0u;
    mark_all_dirty();
}

const char* UI::current_profile_id() const { return snap_.profile[0] ? snap_.profile : settings_.profile; }

const char* UI::profile_label(const char* id) const {
    if (!id || !id[0]) return "-";
    for (size_t i = 0; i < profiles_.size(); i++)
        if (!strcmp(profiles_[i].id, id)) return profiles_[i].label;
    return id;
}

void UI::cycle_profile(int dir) {
    if (profiles_.empty() || snap_.link != Link::Online) {
        show_toast("Connectez-vous au relais pour changer de profil.");
        return;
    }
    const char* cur = current_profile_id();
    int idx = 0;
    for (size_t i = 0; i < profiles_.size(); i++)
        if (!strcmp(profiles_[i].id, cur)) idx = (int)i;
    int n = (int)profiles_.size();
    idx = (idx + dir + n) % n;
    copy_str(settings_.profile, sizeof(settings_.profile), profiles_[(size_t)idx].id);
    gfn_.set_profile(profiles_[(size_t)idx].id);
    save_settings();
}

void UI::apply_buffer_mode() { video_.set_buffer_mode(settings_.buffer_mode); }

void UI::connect_now() {
    if (!settings_.host[0]) {
        show_toast("Aucune adresse : choisissez un relais ou saisissez-la (onglet Réseau).");
        return;
    }
    gfn_.connect(settings_.host, settings_.port, settings_.pin, settings_.auto_reconnect);
}

void UI::maybe_auto_connect() {
    if (auto_tried_) return;
    auto_tried_ = true;
    if (settings_.auto_connect && settings_.host[0]) connect_now();
}

void UI::edit_text(const char* hint, char* target, size_t cap, bool numeric) {
    char out[96];
    out[0] = '\0';
    if (!plat::text_input(hint, target, out, (int)(cap < sizeof(out) ? cap : sizeof(out)), numeric)) {
        mark_all_dirty();
        return;
    }
    // Pas d'espace ni de caractere de controle autour d'une adresse ou d'un code.
    char clean[96];
    size_t j = 0;
    for (const char* p = out; *p && j < sizeof(clean) - 1; p++)
        if ((unsigned char)*p > ' ') clean[j++] = *p;
    clean[j] = '\0';
    copy_str(target, cap, clean);
    save_settings();
    mark_all_dirty();
}

// ============================================================================================ actions
void UI::activate(int id, int dir) {
    if (id == ID_NONE) return;
    mark_all_dirty();

    if (id >= ID_GAME_BASE) {
        size_t i = (size_t)(id - ID_GAME_BASE);
        if (i < games_.size()) gfn_.launch(games_[i].id);
        return;
    }
    if (id >= ID_KEY_BASE && id < ID_KEY_BASE + input::NUM_SRC) {
        uint8_t& t = settings_.keymap[id - ID_KEY_BASE];
        t = (uint8_t)(((int)t + dir + input::NUM_TGT) % input::NUM_TGT);
        save_settings();
        return;
    }
    if (id >= ID_RELAY_BASE && id < ID_KEY_BASE) {
        size_t i = (size_t)(id - ID_RELAY_BASE);
        if (i < relays_.size()) {
            net::ip_to_str(relays_[i].ip_be, settings_.host, sizeof(settings_.host));
            settings_.port = relays_[i].port;
            save_settings();
            connect_now();
        }
        return;
    }

    switch (id) {
        case ID_CONNECT: connect_now(); break;
        case ID_DISCONNECT: gfn_.disconnect(); break;
        case ID_REFRESH: gfn_.refresh_games(); break;
        case ID_WATCH:
            force_view_ = true;
            gfn_.watch();
            break;

        case ID_HOST: edit_text("Adresse IP du PC (ex. 192.168.1.20)", settings_.host, sizeof(settings_.host), false); break;
        case ID_PORT: {
            char tmp[16];
            snprintf(tmp, sizeof(tmp), "%d", settings_.port);
            edit_text("Port du relais", tmp, sizeof(tmp), true);
            int p = atoi(tmp);
            settings_.port = (p >= 1 && p <= 65535) ? p : proto::PORT_CONTROL;
            save_settings();
            break;
        }
        case ID_PIN: edit_text("Code PIN du relais (vide : aucun)", settings_.pin, sizeof(settings_.pin), false); break;
        case ID_AUTO_RECONNECT: settings_.auto_reconnect = !settings_.auto_reconnect; save_settings(); break;
        case ID_AUTO_CONNECT: settings_.auto_connect = !settings_.auto_connect; save_settings(); break;

        case ID_PRESET: {
            int n = input::num_presets();
            int cur = input::detect_preset(settings_.keymap);
            int next = cur < 0 ? (dir > 0 ? 0 : n - 1) : (cur + dir + n) % n;
            input::preset_map(next, settings_.keymap);
            save_settings();
            break;
        }
        case ID_TOUCH_STICK: settings_.touch_stick = !settings_.touch_stick; save_settings(); break;
        case ID_KEYS_RESET:
            input::default_map(settings_.keymap);
            save_settings();
            break;
        case ID_KEYTEST: set_page(Page::KeyTest, back_page_); break;

        case ID_PROFILE: cycle_profile(dir); break;
        case ID_BUFFER:
            settings_.buffer_mode = (settings_.buffer_mode + dir + 3) % 3;
            apply_buffer_mode();
            save_settings();
            break;
        case ID_OVERLAY:
            settings_.overlay = (settings_.overlay + dir + 3) % 3;
            save_settings();
            break;
        case ID_SPEEDUP:
            if (plat::is_new3ds()) {
                settings_.speedup = !settings_.speedup;
                plat::set_cpu_speedup(settings_.speedup);
                save_settings();
            }
            break;
        case ID_QUIT: set_page(Page::QuitConfirm, page_); break;

        case ID_RESUME: set_page(Page::Pad); break;
        case ID_SM_OPTIONS: set_page(Page::StreamOptions); break;
        case ID_SM_KEYS: set_page(Page::Keys, Page::StreamMenu); break;
        case ID_SM_STOP:
            force_view_ = false;
            gfn_.stop_game();          // coupe le flux ; la partie continue sur le PC
            set_page(Page::Games);
            break;
        case ID_SM_QUIT: set_page(Page::QuitConfirm, Page::StreamMenu); break;

        case ID_QUIT_YES: quit_ = true; break;
        case ID_QUIT_NO: set_page(back_page_); break;
        default: break;
    }
}

// ===================================================================================== construction des pages
void UI::build_rows() {
    list_.set_area(0, LIST_Y, SCREEN_W, LIST_H);
    list_.begin();
    switch (page_) {
        case Page::Games: build_games(list_); break;
        case Page::Network: build_network(list_); break;
        case Page::Keys: build_keys(list_); break;
        case Page::Options: build_options(list_, false); break;
        case Page::StreamOptions: build_options(list_, true); break;
        case Page::StreamMenu: build_stream_menu(list_); break;
        case Page::QuitConfirm: build_quit(list_); break;
        default: break;
    }
    list_.end();
}

void UI::build_games(RowList& l) {
    const Snapshot& s = snap_;
    char buf[128];

    if (s.link != Link::Online) {
        const char* title = "Non connecté au relais";
        Color c = s.fatal ? col::RED : col::TEXT;
        switch (s.link) {
            case Link::Connecting: title = "Connexion au relais..."; c = col::AMBER; break;
            case Link::Handshake: title = "Négociation avec le relais..."; c = col::AMBER; break;
            case Link::Reconnecting:
                snprintf(buf, sizeof(buf), "Reconnexion dans %d s (essai %d)", s.retry_in_s, s.attempt);
                title = buf;
                c = col::AMBER;
                break;
            default: break;
        }
        l.add(Row::Info, 0, title).color = c;
        if (s.message[0] && s.link != Link::Connecting && s.link != Link::Handshake)
            l.add(Row::Info, 0, s.message).color = s.fatal ? col::RED : col::DIM;
        if (s.fatal) l.add(Row::Info, 0, "Corrigez le réglage (onglet Réseau) puis reconnectez-vous.");
        if (s.link == Link::Offline) {
            if (settings_.host[0]) {
                snprintf(buf, sizeof(buf), "Se connecter à %s", settings_.host);
                l.add(Row::Button, ID_CONNECT, buf).on = true;
            } else {
                l.add(Row::Info, 0, "Choisissez un relais ci-dessous ou saisissez son adresse (onglet Réseau).");
            }
        } else {
            l.add(Row::Button, ID_DISCONNECT, s.link == Link::Reconnecting ? "Arrêter les tentatives" : "Annuler");
        }
        if (!relays_.empty()) {
            l.add(Row::Header, 0, "Relais détectés sur le Wi-Fi");
            for (size_t i = 0; i < relays_.size(); i++) {
                char ip[20];
                net::ip_to_str(relays_[i].ip_be, ip, sizeof(ip));
                Row& r = l.add(Row::Item, ID_RELAY_BASE + (int)i, relays_[i].name, ip);
                copy_str(r.pill, sizeof(r.pill), "Choisir");
            }
        } else if (s.link == Link::Offline) {
            l.add(Row::Info, 0, "Aucun relais détecté. Le PC et la console doivent être sur le même réseau.");
        }
        return;
    }

    // ---- connecte : etat de la session GeForce NOW
    using proto::GfnState;
    const bool busy = s.state == GfnState::Launching || s.state == GfnState::Queue || s.state == GfnState::Loading;
    const bool game_running = s.state == GfnState::Streaming || s.state == GfnState::Untracked;
    if (busy || s.state == GfnState::Error || (game_running && !s.stream_on)) {
        l.add(Row::Header, 0, "Session GeForce NOW");
        if (s.state == GfnState::Queue) {
            if (s.queue_pos >= 0) snprintf(buf, sizeof(buf), "File d'attente : position %d", s.queue_pos);
            else snprintf(buf, sizeof(buf), "File d'attente (position inconnue)");
            l.add(Row::Info, 0, buf).color = col::AMBER;
            if (s.eta_s >= 0) {
                char d[32];
                fmt_duration(d, sizeof(d), s.eta_s);
                snprintf(buf, sizeof(buf), "Attente estimée : %s", d);
                l.add(Row::Info, 0, buf).color = col::TEXT;
            }
        } else if (s.state == GfnState::Launching) {
            snprintf(buf, sizeof(buf), "Lancement de %s...", s.game[0] ? s.game : "du jeu");
            l.add(Row::Info, 0, buf).color = col::TEXT;
        } else if (s.state == GfnState::Loading) {
            snprintf(buf, sizeof(buf), "Chargement de %s...", s.game[0] ? s.game : "du jeu");
            l.add(Row::Info, 0, buf).color = col::TEXT;
        } else if (s.state == GfnState::Error) {
            l.add(Row::Info, 0, "Erreur signalée par GeForce NOW").color = col::RED;
        } else if (s.state == GfnState::Untracked) {
            snprintf(buf, sizeof(buf), "Partie lancée sur le PC : %s", s.game[0] ? s.game : "jeu");
            l.add(Row::Info, 0, buf).color = col::TEXT;
            l.add(Row::Info, 0, "File d'attente non suivie : l'écran du PC s'affiche tel quel (voir le README).");
        } else {
            snprintf(buf, sizeof(buf), "Partie en cours sur le PC : %s", s.game[0] ? s.game : "jeu");
            l.add(Row::Info, 0, buf).color = col::TEXT;
        }
        if (s.state_text[0] && !generic_state_text(s.state, s.state_text)) l.add(Row::Info, 0, s.state_text);
        if (s.state == GfnState::Queue)
            l.add(Row::Info, 0, "Pour quitter la file, fermez la partie dans GeForce NOW sur le PC.");
        Row& w = l.add(Row::Button, ID_WATCH, game_running ? "Reprendre le flux" : "Voir l'écran du PC");
        w.on = game_running;
    } else {
        Row& w = l.add(Row::Button, ID_WATCH, "Voir l'écran du PC");
        (void)w;
    }

    if (games_.empty()) snprintf(buf, sizeof(buf), "Jeux");
    else snprintf(buf, sizeof(buf), "Jeux (%d)", (int)games_.size());
    l.add(Row::Header, 0, buf);
    if (games_.empty()) {
        l.add(Row::Info, 0, "Aucun jeu. Ajoutez des raccourcis ou un fichier games.json côté PC (voir le README).");
    }
    for (size_t i = 0; i < games_.size(); i++) {
        Row& r = l.add(Row::Item, ID_GAME_BASE + (int)i, games_[i].name);
        copy_str(r.pill, sizeof(r.pill), "Lancer");
    }
    l.add(Row::Button, ID_REFRESH, "Actualiser la liste");
}

void UI::build_network(RowList& l) {
    const Snapshot& s = snap_;
    char buf[128];
    l.add(Row::Header, 0, "Relais (PC Windows)");
    l.add(Row::Value, ID_HOST, "Adresse", settings_.host[0] ? settings_.host : "à saisir");
    snprintf(buf, sizeof(buf), "%d", settings_.port);
    l.add(Row::Value, ID_PORT, "Port", buf);
    l.add(Row::Value, ID_PIN, "Code PIN", settings_.pin[0] ? "****" : "aucun");
    l.add(Row::Toggle, ID_AUTO_RECONNECT, "Reconnexion automatique").on = settings_.auto_reconnect;
    l.add(Row::Toggle, ID_AUTO_CONNECT, "Connexion au démarrage").on = settings_.auto_connect;
    if (s.link == Link::Offline) l.add(Row::Button, ID_CONNECT, "Se connecter").on = true;
    else l.add(Row::Button, ID_DISCONNECT, "Se déconnecter");

    if (!relays_.empty()) {
        l.add(Row::Header, 0, "Relais détectés");
        for (size_t i = 0; i < relays_.size(); i++) {
            char ip[20];
            net::ip_to_str(relays_[i].ip_be, ip, sizeof(ip));
            Row& r = l.add(Row::Item, ID_RELAY_BASE + (int)i, relays_[i].name, ip);
            copy_str(r.pill, sizeof(r.pill), "Choisir");
        }
    }

    l.add(Row::Header, 0, "État de la liaison");
    const char* st = "Hors ligne";
    switch (s.link) {
        case Link::Connecting: st = "Connexion..."; break;
        case Link::Handshake: st = "Négociation..."; break;
        case Link::Online: st = "Connecté"; break;
        case Link::Reconnecting: st = "Reconnexion..."; break;
        default: break;
    }
    if (s.link == Link::Online && s.relay_name[0]) snprintf(buf, sizeof(buf), "%s : %s", st, s.relay_name);
    else snprintf(buf, sizeof(buf), "%s", st);
    l.add(Row::Info, 0, buf).color = s.link == Link::Online ? col::GREEN : col::TEXT;
    if (s.server[0]) {
        snprintf(buf, sizeof(buf), "Serveur : %s", s.server);
        l.add(Row::Info, 0, buf);
    }
    if (s.link == Link::Online && s.rtt_ms >= 0) {
        snprintf(buf, sizeof(buf), "Latence : %d ms (moy. %d ms)", s.rtt_ms, s.rtt_avg_ms);
        l.add(Row::Info, 0, buf);
    }
    if (s.link != Link::Online && s.message[0]) l.add(Row::Info, 0, s.message).color = s.fatal ? col::RED : col::DIM;
    snprintf(buf, sizeof(buf), "Wi-Fi de la console : %d/3", wifi_);
    l.add(Row::Info, 0, buf);
}

void UI::build_keys(RowList& l) {
    int cur = input::detect_preset(settings_.keymap);
    l.add(Row::Header, 0, "Préréglage");
    l.add(Row::Value, ID_PRESET, "Disposition", cur >= 0 ? input::preset_name(cur) : "Personnalisée");
    l.add(Row::Header, 0, "Bouton 3DS  ->  commande Xbox");
    for (int i = 0; i < input::NUM_SRC; i++) {
        char lab[32];
        if (i == input::SRC_ZL || i == input::SRC_ZR) snprintf(lab, sizeof(lab), "%s (New 3DS)", input::src_label(i));
        else copy_str(lab, sizeof(lab), input::src_label(i));
        l.add(Row::Value, ID_KEY_BASE + i, lab, input::tgt_label(settings_.keymap[i]));
    }
    l.add(Row::Header, 0, "Écran tactile");
    l.add(Row::Toggle, ID_TOUCH_STICK, "Stick droit tactile").on = settings_.touch_stick;
    l.add(Row::Button, ID_KEYTEST, "Tester les touches");
    l.add(Row::Button, ID_KEYS_RESET, "Rétablir les valeurs par défaut");
    l.add(Row::Info, 0, "Start = menu, Select = options : réservés. LT, RT, L3, R3, Start, Back et Guide du jeu : boutons tactiles.");
    if (!plat::is_new3ds()) l.add(Row::Info, 0, "Old 3DS : pas de ZL/ZR. Le préréglage Gâchettes sur L/R met LT/RT sur L/R.");
}

void UI::build_options(RowList& l, bool stream_only) {
    static const char* const BUF[3] = {"Adaptatif", "Latence min.", "Fluide"};
    static const char* const OVL[3] = {"Aucune", "Compactes", "Détaillées"};
    l.add(Row::Header, 0, "Streaming");
    l.add(Row::Value, ID_PROFILE, "Profil", profile_label(current_profile_id()));
    l.add(Row::Value, ID_BUFFER, "Tampon vidéo", BUF[clamp_idx(settings_.buffer_mode, 3)]);
    l.add(Row::Value, ID_OVERLAY, "Infos à l'écran", OVL[clamp_idx(settings_.overlay, 3)]);
    if (stream_only) {
        l.add(Row::Toggle, ID_TOUCH_STICK, "Stick droit tactile").on = settings_.touch_stick;
        return;
    }
    l.add(Row::Header, 0, "Console");
    Row& sp = l.add(Row::Toggle, ID_SPEEDUP, "Mode rapide (804 MHz)");
    sp.on = settings_.speedup && plat::is_new3ds();
    sp.enabled = plat::is_new3ds();
    if (!plat::is_new3ds()) l.add(Row::Info, 0, "Old 3DS : le décodage est plus lent. Le profil Performance est conseillé.");
    l.add(Row::Header, 0, "À propos");
    l.add(Row::Info, 0, "GFN 3DS : client non officiel pour un relais installé sur votre PC. Pas d'audio pour l'instant.");
    l.add(Row::Button, ID_QUIT, "Quitter l'application").color = col::RED;
}

void UI::build_stream_menu(RowList& l) {
    char buf[128];
    if (snap_.game[0]) snprintf(buf, sizeof(buf), "En jeu : %s", snap_.game);
    else snprintf(buf, sizeof(buf), "Flux du PC en cours");
    l.add(Row::Info, 0, buf).color = col::TEXT;
    l.add(Row::Button, ID_RESUME, "Reprendre").on = true;
    l.add(Row::Button, ID_SM_OPTIONS, "Options de streaming");
    l.add(Row::Button, ID_SM_KEYS, "Configuration des touches");
    l.add(Row::Button, ID_SM_STOP, "Arrêter le flux (le jeu continue)");
    l.add(Row::Button, ID_SM_QUIT, "Quitter l'application").color = col::RED;
}

void UI::build_quit(RowList& l) {
    l.add(Row::Info, 0, "Quitter GFN 3DS ?").color = col::WHITE;
    using proto::GfnState;
    if (snap_.state == GfnState::Queue || snap_.state == GfnState::Loading || snap_.state == GfnState::Streaming ||
        snap_.state == GfnState::Launching || snap_.state == GfnState::Untracked)
        l.add(Row::Info, 0, "La session GeForce NOW continue sur le PC.");
    l.add(Row::Button, ID_QUIT_NO, "Annuler").on = true;
    l.add(Row::Button, ID_QUIT_YES, "Quitter").color = col::RED;
}

// ==================================================================================== mise a jour
bool UI::animating() const {
    switch (snap_.link) {
        case Link::Connecting:
        case Link::Handshake:
        case Link::Reconnecting: return true;
        case Link::Offline: return false;
        default: break;
    }
    using proto::GfnState;
    if (snap_.state == GfnState::Launching || snap_.state == GfnState::Queue || snap_.state == GfnState::Loading) return true;
    return view_active_ && video_.frame() == nullptr;
}

void UI::on_snapshot_changed() {
    if (snap_.games_version != games_ver_) {
        games_ver_ = snap_.games_version;
        gfn_.copy_games(games_);
        bottom_dirty_ = true;
    }
    if (snap_.profiles_version != profiles_ver_) {
        profiles_ver_ = snap_.profiles_version;
        gfn_.copy_profiles(profiles_);
        bottom_dirty_ = true;
    }
    if (snap_.relays_version != relays_ver_) {
        relays_ver_ = snap_.relays_version;
        gfn_.copy_relays(relays_);
        bottom_dirty_ = true;
    }
    if (snap_.toast_seq != toast_seq_) {
        toast_seq_ = snap_.toast_seq;
        show_toast(snap_.toast);
    }

    // Sans calibration des journaux, le relais ne peut pas suivre la file : on le dit une fois, franchement.
    if (snap_.state != last_state_) {
        if (snap_.state == proto::GfnState::Untracked && snap_.stream_on)
            show_toast("File d'attente non suivie : écran du PC affiché tel quel");
        last_state_ = snap_.state;
    }

    // La vue video (et la manette tactile) s'ouvre toute seule quand le flux demarre, et se ferme a sa fin.
    if (snap_.link == Link::Offline) force_view_ = false;
    const bool view = compute_view();
    if (view && !view_active_ && !streaming_page()) set_page(Page::Pad);
    else if (!view && view_active_ && streaming_page()) set_page(Page::Games);
    if (view != view_active_) top_dirty_ = true;
    view_active_ = view;

    // Le profil choisi sur la console est reapplique a chaque connexion, des que le relais a annonce ses profils.
    if (snap_.link == Link::Online && snap_.profiles_version != applied_profiles_ver_ &&
        snap_.profiles_version == profiles_ver_) {
        applied_profiles_ver_ = snap_.profiles_version;
        if (settings_.profile[0] && strcmp(settings_.profile, snap_.profile) != 0) {
            for (size_t i = 0; i < profiles_.size(); i++) {
                if (!strcmp(profiles_[i].id, settings_.profile)) {
                    gfn_.set_profile(settings_.profile);
                    break;
                }
            }
        }
    }

    // Tout changement d'etat visible redessine les deux ecrans.
    uint32_t h = 2166136261u;
    int iv[14] = {(int)snap_.link, (int)snap_.state, snap_.queue_pos, snap_.eta_s, snap_.attempt, snap_.retry_in_s,
                  (int)snap_.fatal, (int)snap_.stream_on, (int)snap_.pad_available, (int)snap_.profiles_version,
                  (int)snap_.games_version, (int)snap_.relays_version, (int)(snap_.rtt_ms >= 0), 0};
    h = fnv(h, iv, sizeof(iv));
    h = fnv_str(h, snap_.message);
    h = fnv_str(h, snap_.game);
    h = fnv_str(h, snap_.state_text);
    h = fnv_str(h, snap_.relay_name);
    h = fnv_str(h, snap_.profile);
    h = fnv_str(h, snap_.server);
    if (h != sig_) {
        sig_ = h;
        top_dirty_ = true;
        bottom_dirty_ = true;
    }
}

void UI::update(const input::Raw& raw, uint64_t now) {
    raw_ = raw;
    now_ = now;
    if (last_tick_ == 0) last_tick_ = now ? now : 1;
    snap_ = gfn_.snapshot();
    on_snapshot_changed();
    maybe_auto_connect();

    if (toast_until_ != 0 && now_ >= toast_until_) {
        toast_until_ = 0;
        toast_[0] = '\0';
        bottom_dirty_ = true;
    }

    // Wi-Fi et batterie : un appel systeme chacun, donc rarement.
    if (last_sys_ == 0 || now_ - last_sys_ >= 4000) {
        last_sys_ = now_ ? now_ : 1;
        int w = plat::wifi_strength();
        int b = plat::battery_level();
        bool c = plat::battery_charging();
        if (w != wifi_ || b != batt_ || c != charging_) {
            wifi_ = w;
            batt_ = b;
            charging_ = c;
            bottom_dirty_ = true;
        }
    }

    switch (page_) {
        case Page::Pad: update_pad(); break;
        case Page::KeyTest: update_keytest(); break;
        default: update_menu(); break;
    }

    if (now_ - last_tick_ >= 1000) {     // chronometres, latence, valeurs en direct
        last_tick_ = now_;
        top_dirty_ = true;
        bottom_dirty_ = true;
    }
    if (animating() && now_ - last_anim_ >= 100) {
        last_anim_ = now_;
        top_dirty_ = true;
    }
}

void UI::update_menu() {
    const uint32_t kA = 1u << input::SRC_A, kB = 1u << input::SRC_B, kX = 1u << input::SRC_X;
    const uint32_t kL = 1u << input::SRC_L, kR = 1u << input::SRC_R;
    const uint32_t kUp = 1u << input::SRC_DUP, kDown = 1u << input::SRC_DDOWN;
    const uint32_t kLeft = 1u << input::SRC_DLEFT, kRight = 1u << input::SRC_DRIGHT;
    const uint32_t p = raw_.pressed;

    switch (page_) {
        case Page::StreamMenu:
            if (p & (kB | input::K_START)) { set_page(Page::Pad); return; }
            if (p & input::K_SELECT) { set_page(Page::StreamOptions); return; }
            break;
        case Page::StreamOptions:
            if (p & (kB | input::K_START | input::K_SELECT)) { set_page(Page::Pad); return; }
            break;
        case Page::QuitConfirm:
            if (p & (kB | input::K_START)) { set_page(back_page_); return; }
            break;
        default:     // onglets, ou Touches ouvert depuis le menu du streaming
            if (back_page_ == Page::StreamMenu) {
                if (p & (kB | input::K_START)) { set_page(Page::StreamMenu); return; }
            } else {
                if (p & input::K_START) { set_page(Page::QuitConfirm, page_); return; }
                if (p & kL) { set_page((Page)(((int)page_ + NUM_TABS - 1) % NUM_TABS)); return; }
                if (p & kR) { set_page((Page)(((int)page_ + 1) % NUM_TABS)); return; }
                if ((p & kX) && page_ == Page::Games) gfn_.refresh_games();
            }
            break;
    }

    // Navigation verticale : croix directionnelle ou Circle Pad, avec repetition.
    int dir = 0;
    if (raw_.held & kUp) dir = -1;
    else if (raw_.held & kDown) dir = 1;
    else if (raw_.cpad_y > 80) dir = -1;
    else if (raw_.cpad_y < -80) dir = 1;
    if (dir == 0) {
        hold_dir_ = 0;
    } else if (dir != hold_dir_) {
        hold_dir_ = dir;
        next_repeat_ = now_ + 350;
        list_.move(dir);
        bottom_dirty_ = true;
    } else if (now_ >= next_repeat_) {
        next_repeat_ = now_ + 70;
        list_.move(dir);
        bottom_dirty_ = true;
    }

    const Row* sel = list_.selected_row();
    if (sel) {
        const int id = sel->id;
        const Row::Kind kind = sel->kind;
        if ((p & kLeft) && ((kind == Row::Value && cycles(id)) || kind == Row::Toggle)) activate(id, -1);
        else if ((p & kRight) && ((kind == Row::Value && cycles(id)) || kind == Row::Toggle)) activate(id, 1);
        else if (p & kA) activate(id, 1);
    }
    handle_touch_list();
}

void UI::handle_touch_list() {
    const input::Raw& r = raw_;
    if (r.touch_pressed) {
        if (r.ty < TAB_H) {
            if (tabs_visible()) {
                int t = clamp_idx(r.tx * NUM_TABS / SCREEN_W, NUM_TABS);
                if ((int)page_ != t) set_page((Page)t);
            }
            return;
        }
        if (r.ty >= LIST_Y && r.ty < LIST_Y + LIST_H) {
            touch_down_ = true;
            touch_dragging_ = false;
            touch_start_y_ = touch_last_y_ = r.ty;
            touch_row_ = list_.hit(r.tx, r.ty);
        }
    }
    if (touch_down_ && r.touch) {
        if (!touch_dragging_ && abs(r.ty - touch_start_y_) > 6) touch_dragging_ = true;
        if (touch_dragging_) {
            int dy = r.ty - touch_last_y_;
            if (dy != 0) {
                list_.scroll_by(-dy);
                touch_last_y_ = r.ty;
                bottom_dirty_ = true;
            }
        }
    }
    if (touch_down_ && r.touch_released) {
        touch_down_ = false;
        if (!touch_dragging_ && touch_row_ >= 0) {
            const Row* row = list_.row_at(touch_row_);
            if (row && RowList::selectable(*row)) {
                const int id = row->id;
                const Row::Kind kind = row->kind;
                const bool was_selected = list_.selected_id() == id;
                int dir = 1;
                if (kind == Row::Value && cycles(id)) {      // petite fleche gauche : valeur precedente
                    int vx = SCREEN_W - 12 - 18 - text_width(row->value);
                    if (r.tx >= vx - 14 && r.tx < vx) dir = -1;
                }
                list_.select_index(touch_row_);
                bottom_dirty_ = true;
                if (kind != Row::Item || was_selected) activate(id, dir);   // une ligne de liste : un appui choisit, le suivant valide
            }
        }
        touch_dragging_ = false;
    }
    if (!r.touch && !r.touch_released) touch_down_ = false;
}

void UI::update_pad() {
    const input::Raw& r = raw_;
    if (r.pressed & input::K_START) { set_page(Page::StreamMenu); return; }
    if (r.pressed & input::K_SELECT) { set_page(Page::StreamOptions); return; }
    swallow_mask_ &= r.held;
    if (!r.touch) swallow_touch_ = false;
    uint32_t sig = 2166136261u;
    int v[4] = {r.touch ? 1 : 0, r.touch ? r.tx : 0, r.touch ? r.ty : 0, 0};
    sig = fnv(sig, v, sizeof(v));
    if (sig != kt_sig_) {
        kt_sig_ = sig;
        bottom_dirty_ = true;
    }
}

void UI::update_keytest() {
    const input::Raw& r = raw_;
    if (r.pressed & input::K_START) { set_page(Page::Keys, back_page_); return; }
    if (r.touch_pressed && r.tx >= KT_BACK_X && r.tx < KT_BACK_X + KT_BACK_W && r.ty >= KT_BACK_Y &&
        r.ty < KT_BACK_Y + KT_BACK_H) {
        set_page(Page::Keys, back_page_);
        return;
    }
    uint32_t sig = 2166136261u;
    int v[8] = {(int)r.held, r.cpad_x, r.cpad_y, r.cstick_x, r.cstick_y, r.touch ? 1 : 0, r.tx, r.ty};
    sig = fnv(sig, v, sizeof(v));
    if (sig != kt_sig_) {
        kt_sig_ = sig;
        bottom_dirty_ = true;
    }
}

// ======================================================================================== dessin : bas
const char* UI::page_title() const {
    switch (page_) {
        case Page::StreamMenu: return "Menu du streaming";
        case Page::StreamOptions: return "Options de streaming";
        case Page::QuitConfirm: return "Quitter";
        case Page::KeyTest: return "Test des touches";
        case Page::Keys: return "Touches";
        default: return "GFN 3DS";
    }
}

const char* UI::hint_text() const {
    switch (page_) {
        case Page::Games:
            return snap_.link == Link::Online ? "A Valider  X Actualiser" : "A Valider  L/R Onglets";
        case Page::Network: return "A Modifier  Start Quitter";
        case Page::Keys: return back_page_ == Page::StreamMenu ? "\xE2\x86\x90\xE2\x86\x92 Changer  B Retour" : "\xE2\x86\x90\xE2\x86\x92 Changer  Start Quitter";
        case Page::Options: return "\xE2\x86\x90\xE2\x86\x92 Changer  Start Quitter";
        case Page::StreamMenu: return "A Choisir  B Reprendre";
        case Page::StreamOptions: return "\xE2\x86\x90\xE2\x86\x92 Changer  B Retour";
        case Page::QuitConfirm: return "A Valider  B Annuler";
        default: return "";
    }
}

void UI::draw_tabs(const Surface& s) const {
    static const char* const NAMES[NUM_TABS] = {"Jeux", "Réseau", "Touches", "Options"};
    fill_rect(s, 0, 0, SCREEN_W, TAB_H, col::PANEL);
    fill_rect(s, 0, TAB_H - 1, SCREEN_W, 1, col::LINE);
    const int tw = SCREEN_W / NUM_TABS;
    for (int i = 0; i < NUM_TABS; i++) {
        const bool act = (int)page_ == i;
        if (act) {
            fill_rect(s, i * tw, 0, tw, TAB_H, col::PANEL_HI);
            fill_rect(s, i * tw, TAB_H - 3, tw, 3, col::GREEN);
        }
        text_center(s, i * tw + tw / 2, 7, NAMES[i], act ? col::WHITE : col::DIM);
    }
}

void UI::draw_title_bar(const Surface& s, const char* title) const {
    vgradient(s, 0, 0, SCREEN_W, TAB_H, col::PANEL_HI, col::PANEL);
    fill_rect(s, 0, TAB_H - 3, SCREEN_W, 3, col::GREEN);
    text_center(s, SCREEN_W / 2, 7, title, col::WHITE);
}

void UI::draw_status_bar(const Surface& s) const {
    fill_rect(s, 0, BAR_Y, SCREEN_W, BAR_H, col::PANEL);
    fill_rect(s, 0, BAR_Y, SCREEN_W, 1, col::LINE);
    text_fit(s, 6, BAR_Y + 8, hint_text(), col::DIM, 236);
    for (int i = 0; i < 3; i++) {     // Wi-Fi
        int hh = 4 + i * 3;
        fill_rect(s, 252 + i * 5, BAR_Y + 17 - hh, 3, hh, i < wifi_ ? col::GREEN : col::LINE);
    }
    const int bx = 278, by = BAR_Y + 7;     // batterie
    frame_rect(s, bx, by, 26, 11, col::DIM);
    fill_rect(s, bx + 26, by + 3, 2, 5, col::DIM);
    if (batt_ >= 0) {
        int fill = (batt_ * 22 + 2) / 5;
        Color c = charging_ ? col::BLUE : (batt_ <= 1 ? col::RED : col::GREEN);
        if (fill > 0) fill_rect(s, bx + 2, by + 2, fill, 7, c);
    }
}

void UI::draw_toast(const Surface& s, int bottom) const {
    if (!toast_until_ || !toast_[0]) return;
    int st[2], ln[2];
    int n = wrap_lines(toast_, 35, st, ln, 2);
    int hh = 8 + 11 * n;
    int y = bottom - hh - 4;
    fill_rrect(s, 6, y, SCREEN_W - 12, hh, 5, col::AMBER);
    for (int k = 0; k < n; k++) {
        char tmp[64];
        line_copy(tmp, sizeof(tmp), toast_, st[k], ln[k]);
        text(s, 12, y + 4 + k * 11, tmp, col::BLACK);
    }
}

void UI::draw_pad(const Surface& s) const {
    clear(s, col::BG);
    const uint32_t down = (raw_.touch && !swallow_touch_) ? input::touch_buttons(raw_) : 0;
    for (int i = 0; i < input::vbutton_count(); i++) {
        const input::VButton& b = input::vbutton(i);
        const bool on = (down & b.bit) != 0;
        fill_rrect(s, b.x - 1, b.y - 1, b.w + 2, b.h + 2, 7, on ? col::GREEN : col::LINE);
        fill_rrect(s, b.x, b.y, b.w, b.h, 6, on ? col::GREEN_DK : col::PANEL);
        text_center(s, b.x + b.w / 2, b.y + (b.h - GLYPH) / 2, b.label, on ? col::WHITE : col::TEXT);
    }
    if (settings_.touch_stick) {
        const input::StickPad sp = input::stick_pad();
        fill_disc(s, sp.cx, sp.cy, sp.r, col::PANEL);
        ring(s, sp.cx, sp.cy, sp.r, col::LINE, 2);
        ring(s, sp.cx, sp.cy, sp.r / 3, col::LINE, 1);
        text_center(s, sp.cx, sp.cy - sp.r + 8, "stick droit", col::DIM);
        int x = 0, y = 0;
        const bool act = !swallow_touch_ && input::touch_stick_vec(raw_, x, y);
        int kx = sp.cx, ky = sp.cy;
        if (act) {
            kx += x * sp.r / input::STICK_RANGE;
            ky -= y * sp.r / input::STICK_RANGE;
        }
        fill_disc(s, kx, ky, 14, act ? col::GREEN : col::LINE);
    } else {
        text_center(s, SCREEN_W / 2, 168, "Stick tactile désactivé", col::DIM);
        text_center(s, SCREEN_W / 2, 180, "(Select > Options)", col::DIM);
    }
    if (!snap_.pad_available) text_center(s, SCREEN_W / 2, 99, "Pas de manette virtuelle sur le PC (ViGEmBus ?)", col::AMBER);
    else text_center(s, SCREEN_W / 2, 99, "Start : menu     Select : options", col::DIM);
    draw_toast(s, 232);
}

void UI::draw_keytest(const Surface& s) const {
    clear(s, col::BG);
    const input::Raw& r = raw_;
    for (int i = 0; i < input::NUM_SRC; i++) {
        const int cx = i % 4, cy = i / 4;
        const int x = 7 + cx * 78, y = 30 + cy * 40;
        const bool on = (r.held & (1u << i)) != 0;
        fill_rrect(s, x - 1, y - 1, 76, 36, 5, on ? col::GREEN : col::LINE);
        fill_rrect(s, x, y, 74, 34, 4, on ? col::GREEN_DK : col::PANEL);
        text_center(s, x + 37, y + 6, input::src_label(i), col::WHITE);
        char buf[40];
        snprintf(buf, sizeof(buf), "\xE2\x86\x92 %s", input::tgt_label(settings_.keymap[i]));
        text_fit(s, x + 4, y + 20, buf, on ? col::WHITE : col::DIM, 66);
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "Circle Pad %+4d %+4d", r.cpad_x, r.cpad_y);
    text(s, 10, 152, buf, col::TEXT);
    snprintf(buf, sizeof(buf), "C-stick    %+4d %+4d", r.cstick_x, r.cstick_y);
    text(s, 10, 164, buf, col::TEXT);
    if (r.touch) snprintf(buf, sizeof(buf), "Tactile    %3d,%3d", r.tx, r.ty);
    else snprintf(buf, sizeof(buf), "Tactile    -");
    text(s, 10, 176, buf, col::TEXT);
    const bool back_hit = r.touch && r.tx >= KT_BACK_X && r.tx < KT_BACK_X + KT_BACK_W && r.ty >= KT_BACK_Y && r.ty < KT_BACK_Y + KT_BACK_H;
    fill_rrect(s, KT_BACK_X, KT_BACK_Y, KT_BACK_W, KT_BACK_H, 5, back_hit ? col::GREEN : col::PANEL_HI);
    text_center(s, KT_BACK_X + KT_BACK_W / 2, KT_BACK_Y + 8, "Retour (Start)", col::WHITE);
    draw_title_bar(s, "Test des touches");
}

void UI::draw_bottom(const Surface& s, uint64_t now) {
    now_ = now;
    if (page_ == Page::Pad) {
        draw_pad(s);
        return;
    }
    if (page_ == Page::KeyTest) {
        draw_keytest(s);
        return;
    }
    build_rows();
    clear(s, col::BG);
    list_.draw(s);
    draw_toast(s, BAR_Y);
    if (tabs_visible()) draw_tabs(s);
    else draw_title_bar(s, page_title());
    draw_status_bar(s);
}

// ======================================================================================== dessin : haut
void UI::draw_brand(const Surface& s) const {
    vgradient(s, 0, 0, 400, 34, col::PANEL_HI, col::PANEL);
    fill_rect(s, 0, 34, 400, 2, col::GREEN);
    int x = 12;
    x += text(s, x, 9, "GFN", col::GREEN, 2) + 6;
    x += text(s, x, 9, "3DS", col::WHITE, 2) + 14;
    text(s, x, 13, "client non officiel", col::DIM);

    const char* label = "Hors ligne";
    Color c = col::RED;
    switch (snap_.link) {
        case Link::Online: label = "Connecté"; c = col::GREEN; break;
        case Link::Connecting:
        case Link::Handshake: label = "Connexion"; c = col::AMBER; break;
        case Link::Reconnecting: label = "Reconnexion"; c = col::AMBER; break;
        default: break;
    }
    const int w = text_width(label);
    text(s, 400 - 12 - w, 13, label, col::TEXT);
    fill_disc(s, 400 - 12 - w - 10, 17, 4, c);
}

void UI::draw_top_status(const Surface& s) const {
    using proto::GfnState;
    const Snapshot& sn = snap_;
    clear(s, col::BG);
    draw_brand(s);
    const int phase = (int)((now_ / 100) % 12);
    char buf[128];

    if (sn.link != Link::Online) {
        if (sn.link == Link::Offline) {
            text_center(s, 200, 74, "Non connecté", sn.fatal ? col::RED : col::TEXT, 2);
            int y = 108;
            if (sn.message[0]) y = center_wrapped(s, 200, y, sn.message, sn.fatal ? col::RED : col::DIM, 44, 3);
            center_wrapped(s, 200, y + 14, sn.fatal ? "Corrigez le réglage sur l'écran du bas (onglet Réseau)."
                                                    : "Choisissez un relais sur l'écran du bas.",
                           col::DIM, 44, 2);
        } else {
            spinner(s, 200, 90, 20, phase, col::GREEN, col::BG);
            if (sn.link == Link::Reconnecting) {
                text_center(s, 200, 128, "Reconnexion", col::AMBER, 2);
                snprintf(buf, sizeof(buf), "Nouvel essai dans %d s (essai %d)", sn.retry_in_s, sn.attempt);
                text_center(s, 200, 152, buf, col::TEXT);
            } else {
                text_center(s, 200, 128, sn.link == Link::Connecting ? "Connexion au relais" : "Négociation", col::TEXT, 2);
                if (sn.server[0]) text_center(s, 200, 152, sn.server, col::DIM);
            }
            if (sn.message[0] && sn.link == Link::Reconnecting) center_wrapped(s, 200, 168, sn.message, col::DIM, 44, 2);
        }
        return;
    }

    // ---- connecte
    uint64_t since = (sn.state_since_ms && now_ > sn.state_since_ms) ? now_ - sn.state_since_ms : 0;
    switch (sn.state) {
        case GfnState::Queue: {
            text_center(s, 200, 46, "FILE D'ATTENTE", col::DIM);
            if (sn.queue_pos >= 0) {
                snprintf(buf, sizeof(buf), "#%d", sn.queue_pos);
                int tw = text_width(buf, 6);
                text_center(s, 200, 62, buf, col::GREEN, 6);
                spinner(s, 200 - tw / 2 - 34, 86, 14, phase, col::GREEN, col::BG);
                spinner(s, 200 + tw / 2 + 34, 86, 14, phase, col::GREEN, col::BG);
                text_center(s, 200, 118, "position dans la file", col::DIM);
            } else {
                spinner(s, 200, 88, 20, phase, col::GREEN, col::BG);
                text_center(s, 200, 118, "Position inconnue", col::TEXT, 2);
            }
            if (sn.eta_s >= 0) {
                char d[32];
                fmt_duration(d, sizeof(d), sn.eta_s);
                snprintf(buf, sizeof(buf), "Attente estimée : %s", d);
                text_center(s, 200, 146, buf, col::WHITE, 1);
            }
            if (sn.game[0]) text_center(s, 200, 164, sn.game, col::TEXT);
            fmt_clock(buf, sizeof(buf), since);
            char line[160];
            snprintf(line, sizeof(line), "dans la file depuis %s", buf);
            text_center(s, 200, 182, line, col::DIM);
            break;
        }
        case GfnState::Launching:
        case GfnState::Loading:
            spinner(s, 200, 86, 20, phase, col::GREEN, col::BG);
            text_center(s, 200, 122, sn.state == GfnState::Launching ? "Lancement" : "Chargement du jeu", col::WHITE, 2);
            if (sn.game[0]) text_center(s, 200, 150, sn.game, col::TEXT);
            if (sn.state_text[0] && !generic_state_text(sn.state, sn.state_text))
                center_wrapped(s, 200, 166, sn.state_text, col::DIM, 44, 2);
            fmt_clock(buf, sizeof(buf), since);
            text_center(s, 200, 196, buf, col::DIM);
            break;
        case GfnState::Untracked:
        case GfnState::Streaming:     // flux non demande par cette console (reprise possible depuis l'ecran du bas)
            text_center(s, 200, 70, "Partie en cours", col::GREEN, 2);
            if (sn.game[0]) text_center(s, 200, 100, sn.game, col::WHITE);
            center_wrapped(s, 200, 126, "Reprenez le flux depuis l'écran du bas (Jeux > Reprendre le flux).", col::DIM, 44, 2);
            break;
        case GfnState::Error:
            text_center(s, 200, 70, "Erreur GeForce NOW", col::RED, 2);
            if (sn.state_text[0]) center_wrapped(s, 200, 104, sn.state_text, col::TEXT, 44, 3);
            break;
        default:     // Idle, inconnu
            text_center(s, 200, 70, "Choisissez un jeu", col::WHITE, 2);
            text_center(s, 200, 96, "sur l'écran du bas", col::DIM);
            if (!games_.empty()) {
                snprintf(buf, sizeof(buf), "%d jeu%s disponible%s", (int)games_.size(), games_.size() > 1 ? "x" : "",
                         games_.size() > 1 ? "s" : "");
                text_center(s, 200, 130, buf, col::TEXT);
            }
            break;
    }
    // pied de page : relais et latence
    fill_rect(s, 0, 220, 400, 1, col::LINE);
    snprintf(buf, sizeof(buf), "%s", sn.relay_name[0] ? sn.relay_name : sn.server);
    text_fit(s, 8, 227, buf, col::DIM, 250);
    if (sn.rtt_ms >= 0) {
        snprintf(buf, sizeof(buf), "Latence %d ms", sn.rtt_ms);
        text_right(s, 392, 227, buf, col::DIM);
    }
}

void UI::draw_top_stream(const Surface& s) {
    const uint16_t* fr = video_.frame();
    if (fr) blit_video(s, fr);
    else clear(s, col::BLACK);
    const VideoStats st = video_.stats();
    const bool online = snap_.link == Link::Online;
    const uint64_t age = (fr && st.last_frame_ms && now_ > st.last_frame_ms) ? now_ - st.last_frame_ms : 0;

    char l0[64], l1[64], l2[64], l3[64], l4[64];
    char rtt[16];
    if (snap_.rtt_ms >= 0) snprintf(rtt, sizeof(rtt), "%d", snap_.rtt_ms);
    else snprintf(rtt, sizeof(rtt), "--");
    if (settings_.overlay == 1) {
        snprintf(l0, sizeof(l0), "%d FPS | %s ms", st.fps_shown, rtt);
        const char* lines[1] = {l0};
        overlay_box(s, 4, 4, lines, 1);
    } else if (settings_.overlay >= 2) {
        int d10 = (int)(st.decode_ms * 10.0f + 0.5f);
        snprintf(l0, sizeof(l0), "FPS %d (décodées %d, relais %d)", st.fps_shown, st.fps_decoded, snap_.relay_fps);
        char avg[16];
        if (snap_.rtt_avg_ms >= 0) snprintf(avg, sizeof(avg), "%d", snap_.rtt_avg_ms);
        else snprintf(avg, sizeof(avg), "--");
        snprintf(l1, sizeof(l1), "Latence %s ms (moy. %s)  gigue %d ms", rtt, avg, st.jitter_ms);
        snprintf(l2, sizeof(l2), "Débit %d kb/s  images perdues %u", st.kbps, (unsigned)st.lost_total);
        snprintf(l3, sizeof(l3), "Décodage %d.%d ms  tampon %d ms", d10 / 10, d10 % 10, st.delay_ms);
        snprintf(l4, sizeof(l4), "Profil %s  qualité %d", snap_.profile[0] ? snap_.profile : "-", snap_.relay_quality);
        const char* lines[5] = {l0, l1, l2, l3, l4};
        overlay_box(s, 4, 4, lines, 5);
    }

    const int phase = (int)((now_ / 100) % 12);
    if (!online) {
        shade_rect(s, 0, 0, 400, 240);
        fill_rrect(s, 60, 80, 280, 80, 8, col::PANEL);
        spinner(s, 90, 120, 14, phase, col::AMBER, col::PANEL);
        text(s, 120, 98, "Connexion perdue", col::AMBER, 2);
        char buf[64];
        if (snap_.link == Link::Reconnecting) snprintf(buf, sizeof(buf), "Nouvel essai dans %d s (essai %d)", snap_.retry_in_s, snap_.attempt);
        else snprintf(buf, sizeof(buf), "Reconnexion...");
        text(s, 120, 124, buf, col::TEXT);
        if (!same_text(snap_.message, "Connexion perdue")) text_fit(s, 120, 138, snap_.message, col::DIM, 210);
    } else if (!fr) {
        fill_rrect(s, 80, 90, 240, 60, 8, col::PANEL);
        spinner(s, 112, 120, 14, phase, col::GREEN, col::PANEL);
        text(s, 140, 112, "En attente de", col::TEXT);
        text(s, 140, 124, "la première image...", col::TEXT);
    } else if (age > 2500) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Image figée depuis %d s", (int)(age / 1000));
        int w = text_width(buf) + 16;
        fill_rrect(s, 200 - w / 2, 6, w, 20, 6, col::AMBER);
        text_center(s, 200, 12, buf, col::BLACK);
    }
}

void UI::draw_top(const Surface& s, uint64_t now) {
    now_ = now;
    if (view_active_) draw_top_stream(s);
    else draw_top_status(s);
}
