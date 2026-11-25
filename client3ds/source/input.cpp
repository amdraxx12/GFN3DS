// Entrees : voir input.hpp.
#include "input.hpp"

#include <math.h>
#include <string.h>

#ifdef __3DS__
#include <3ds.h>
#else
#include "platform.hpp"
#endif

namespace input {

// ------------------------------------------------------------------ noms
static const char* const SRC_LABELS[NUM_SRC] = {
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR",
    "Croix haut", "Croix bas", "Croix gauche", "Croix droite"};

static const char* const TGT_LABELS[NUM_TGT] = {
    "Aucun", "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "L3", "R3",
    "Start", "Back", "Guide", "Croix haut", "Croix bas", "Croix gauche", "Croix droite"};

static const char* const TGT_KEYS[NUM_TGT] = {
    "NONE", "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "L3", "R3",
    "START", "BACK", "GUIDE", "DUP", "DDOWN", "DLEFT", "DRIGHT"};

const char* src_label(int s) { return (s >= 0 && s < NUM_SRC) ? SRC_LABELS[s] : "?"; }
const char* tgt_label(int t) { return (t >= 0 && t < NUM_TGT) ? TGT_LABELS[t] : "?"; }
const char* tgt_key(int t) { return (t >= 0 && t < NUM_TGT) ? TGT_KEYS[t] : "NONE"; }

int tgt_from_key(const char* key) {
    if (!key) return -1;
    for (int i = 0; i < NUM_TGT; i++)
        if (!strcmp(key, TGT_KEYS[i])) return i;
    return -1;
}

uint32_t tgt_bit(int t) {
    switch (t) {
        case TGT_A: return proto::B_A;
        case TGT_B: return proto::B_B;
        case TGT_X: return proto::B_X;
        case TGT_Y: return proto::B_Y;
        case TGT_LB: return proto::B_L;
        case TGT_RB: return proto::B_R;
        case TGT_LT: return proto::B_VLT;
        case TGT_RT: return proto::B_VRT;
        case TGT_L3: return proto::B_L3;
        case TGT_R3: return proto::B_R3;
        case TGT_START: return proto::B_VSTART;
        case TGT_BACK: return proto::B_VBACK;
        case TGT_GUIDE: return proto::B_GUIDE;
        case TGT_DUP: return proto::B_DUP;
        case TGT_DDOWN: return proto::B_DDOWN;
        case TGT_DLEFT: return proto::B_DLEFT;
        case TGT_DRIGHT: return proto::B_DRIGHT;
        default: return 0;
    }
}

// ------------------------------------------------------------ prereglages
void default_map(uint8_t map[NUM_SRC]) {
    static const uint8_t m[NUM_SRC] = {TGT_A,  TGT_B,  TGT_X,  TGT_Y,     TGT_LB,    TGT_RB,
                                       TGT_LT, TGT_RT, TGT_DUP, TGT_DDOWN, TGT_DLEFT, TGT_DRIGHT};
    memcpy(map, m, NUM_SRC);
}

int num_presets() { return 3; }

const char* preset_name(int i) {
    switch (i) {
        case 0: return "Standard";
        case 1: return "Position (B=A, Y=X)";
        case 2: return "G\xC3\xA2" "chettes sur L/R";
        default: return "Personnalis\xC3\xA9";
    }
}

void preset_map(int i, uint8_t map[NUM_SRC]) {
    default_map(map);
    if (i == 1) {  // le bouton du bas de la 3DS (B) devient le A de la Xbox, etc.
        map[SRC_A] = TGT_B;
        map[SRC_B] = TGT_A;
        map[SRC_X] = TGT_Y;
        map[SRC_Y] = TGT_X;
    } else if (i == 2) {  // utile sur Old 3DS : L/R font gachettes, LB/RB passent par ZL/ZR ou le tactile
        map[SRC_L] = TGT_LT;
        map[SRC_R] = TGT_RT;
        map[SRC_ZL] = TGT_LB;
        map[SRC_ZR] = TGT_RB;
    }
}

int detect_preset(const uint8_t map[NUM_SRC]) {
    uint8_t ref[NUM_SRC];
    for (int i = 0; i < num_presets(); i++) {
        preset_map(i, ref);
        if (!memcmp(ref, map, NUM_SRC)) return i;
    }
    return -1;
}

// ---------------------------------------------------------- zones tactiles
static const VButton VBUTTONS[] = {
    // Rangee du haut : comme les epaulettes d'une manette
    {7, 8, 72, 44, proto::B_L, "LB"},
    {85, 8, 72, 44, proto::B_VLT, "LT"},
    {163, 8, 72, 44, proto::B_VRT, "RT"},
    {241, 8, 72, 44, proto::B_R, "RB"},
    // Deuxieme rangee
    {7, 58, 58, 36, proto::B_L3, "L3"},
    {69, 58, 58, 36, proto::B_VBACK, "Back"},
    {131, 58, 58, 36, proto::B_GUIDE, "Guide"},
    {193, 58, 58, 36, proto::B_VSTART, "Start"},
    {255, 58, 58, 36, proto::B_R3, "R3"},
};

int vbutton_count() { return (int)(sizeof(VBUTTONS) / sizeof(VBUTTONS[0])); }
const VButton& vbutton(int i) { return VBUTTONS[i]; }
StickPad stick_pad() {
    StickPad p = {160, 172, 60};
    return p;
}

uint32_t touch_buttons(const Raw& raw) {
    if (!raw.touch) return 0;
    uint32_t bits = 0;
    for (int i = 0; i < vbutton_count(); i++) {
        const VButton& b = VBUTTONS[i];
        if (raw.tx >= b.x && raw.tx < b.x + b.w && raw.ty >= b.y && raw.ty < b.y + b.h) bits |= b.bit;
    }
    return bits;
}

bool touch_stick_vec(const Raw& raw, int& x, int& y) {
    x = y = 0;
    if (!raw.touch) return false;
    StickPad p = stick_pad();
    int dx = raw.tx - p.cx;
    int dy = p.cy - raw.ty;  // y positif = haut
    float len = sqrtf((float)(dx * dx + dy * dy));
    if (len > (float)(p.r + 10)) return false;
    float k = len > (float)p.r ? 1.0f : len / (float)p.r;
    if (len < 0.5f) return true;
    x = (int)lroundf((float)dx / len * k * STICK_RANGE);
    y = (int)lroundf((float)dy / len * k * STICK_RANGE);
    return true;
}

static int16_t clamp16(int v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

proto::InputPacket build_packet(const Raw& raw, const Mapping& m) {
    proto::InputPacket p;
    p.flags = m.new3ds ? (uint8_t)proto::INPUT_FLAG_NEW3DS : (uint8_t)0;
    uint32_t b = 0;
    for (int s = 0; s < NUM_SRC; s++)
        if (raw.held & (1u << s)) b |= tgt_bit(m.map[s]);
    b |= touch_buttons(raw);
    p.buttons = b;
    p.cpad_x = clamp16(raw.cpad_x);
    p.cpad_y = clamp16(raw.cpad_y);
    int sx = raw.cstick_x, sy = raw.cstick_y;
    int tx = 0, ty = 0;
    if (m.touch_stick && touch_stick_vec(raw, tx, ty) && tx * tx + ty * ty > sx * sx + sy * sy) {
        sx = tx;
        sy = ty;
    }
    p.cstick_x = clamp16(sx);
    p.cstick_y = clamp16(sy);
    return p;
}

// ---------------------------------------------------------------- materiel
#ifdef __3DS__
static uint32_t translate(u32 k) {
    uint32_t o = 0;
    if (k & KEY_A) o |= 1u << SRC_A;
    if (k & KEY_B) o |= 1u << SRC_B;
    if (k & KEY_X) o |= 1u << SRC_X;
    if (k & KEY_Y) o |= 1u << SRC_Y;
    if (k & KEY_L) o |= 1u << SRC_L;
    if (k & KEY_R) o |= 1u << SRC_R;
    if (k & KEY_ZL) o |= 1u << SRC_ZL;
    if (k & KEY_ZR) o |= 1u << SRC_ZR;
    if (k & KEY_DUP) o |= 1u << SRC_DUP;
    if (k & KEY_DDOWN) o |= 1u << SRC_DDOWN;
    if (k & KEY_DLEFT) o |= 1u << SRC_DLEFT;
    if (k & KEY_DRIGHT) o |= 1u << SRC_DRIGHT;
    if (k & KEY_START) o |= K_START;
    if (k & KEY_SELECT) o |= K_SELECT;
    return o;
}

void poll(Raw& r) {
    // hidInit() (appele par libctru au demarrage) initialise aussi irrst : le C-stick et ZL/ZR de la
    // New 3DS (ou du Circle Pad Pro) sont donc lus par hidScanInput() / hidKeysHeld().
    hidScanInput();
    u32 held = hidKeysHeld();
    u32 down = hidKeysDown();
    u32 up = hidKeysUp();
    r.held = translate(held);
    r.pressed = translate(down);
    r.released = translate(up);

    circlePosition cp, cs;
    hidCircleRead(&cp);
    hidCstickRead(&cs);
    r.cpad_x = cp.dx;
    r.cpad_y = cp.dy;
    r.cstick_x = cs.dx;
    r.cstick_y = cs.dy;

    r.touch = (held & KEY_TOUCH) != 0;
    r.touch_pressed = (down & KEY_TOUCH) != 0;
    r.touch_released = (up & KEY_TOUCH) != 0;
    if (r.touch) {
        touchPosition tp;
        hidTouchRead(&tp);
        r.tx = tp.px;
        r.ty = tp.py;
    }
}
#else
static plat::Mutex g_lock;
static Raw g_injected;
static uint32_t g_prev_held = 0;
static bool g_prev_touch = false;

void host_inject(const Raw& r) {
    plat::Lock l(g_lock);
    g_injected = r;
}

void poll(Raw& r) {
    plat::Lock l(g_lock);
    r = g_injected;
    r.pressed = r.held & ~g_prev_held;
    r.released = g_prev_held & ~r.held;
    r.touch_pressed = r.touch && !g_prev_touch;
    r.touch_released = !r.touch && g_prev_touch;
    g_prev_held = r.held;
    g_prev_touch = r.touch;
}
#endif

}  // namespace input
