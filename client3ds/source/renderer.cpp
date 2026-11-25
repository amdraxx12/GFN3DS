// Rendu logiciel RGB565 : voir renderer.hpp pour la disposition des tampons.
#include "renderer.hpp"

#include <math.h>
#include <string.h>

namespace gfx {

// Police 8x8 de libctru (CP437), generee par tools/gen_font.py.
static const uint8_t FONT[256][8] =
#include "font_data.inc"
    ;

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

Color mix(Color a, Color b, int t) {
    if (t <= 0) return a;
    if (t >= 256) return b;
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    int r = (ar * (256 - t) + br * t) >> 8;
    int g = (ag * (256 - t) + bg * t) >> 8;
    int bl = (ab * (256 - t) + bb * t) >> 8;
    return (Color)((r << 11) | (g << 5) | bl);
}

// ------------------------------------------------------------------ formes
void clear(const Surface& s, Color c) {
    if (!s.valid()) return;
    size_t n = (size_t)s.w * (size_t)s.h;
    uint16_t* p = s.px;
    for (size_t i = 0; i < n; i++) p[i] = c;
}

void fill_rect(const Surface& s, int x, int y, int w, int h, Color c) {
    if (!s.valid() || w <= 0 || h <= 0) return;
    int x0 = imax(x, 0), y0 = imax(y, 0);
    int x1 = imin(x + w, s.w), y1 = imin(y + h, s.h);
    if (x0 >= x1 || y0 >= y1) return;
    int n = y1 - y0;
    for (int cx = x0; cx < x1; cx++) {
        uint16_t* p = s.at(cx, y1 - 1);  // indice le plus bas = y le plus grand
        for (int i = 0; i < n; i++) p[i] = c;
    }
}

void frame_rect(const Surface& s, int x, int y, int w, int h, Color c, int t) {
    if (t < 1 || w <= 0 || h <= 0) return;
    fill_rect(s, x, y, w, t, c);
    fill_rect(s, x, y + h - t, w, t, c);
    fill_rect(s, x, y + t, t, h - 2 * t, c);
    fill_rect(s, x + w - t, y + t, t, h - 2 * t, c);
}

void fill_rrect(const Surface& s, int x, int y, int w, int h, int r, Color c) {
    if (r <= 0 || w < 2 * r || h < 2 * r) {
        fill_rect(s, x, y, w, h, c);
        return;
    }
    for (int i = 0; i < w; i++) {
        int inset = 0;
        int dist = -1;  // distance horizontale au centre du coin, en demi-pixels
        if (i < r) dist = 2 * (r - i) - 1;
        else if (i >= w - r) dist = 2 * (i - (w - r)) + 1;
        if (dist >= 0) {
            float dx = dist * 0.5f;
            int ext = (int)(sqrtf((float)(r * r) - dx * dx) + 0.5f);
            inset = r - ext;
        }
        fill_rect(s, x + i, y + inset, 1, h - 2 * inset, c);
    }
}

void vgradient(const Surface& s, int x, int y, int w, int h, Color top, Color bottom) {
    if (!s.valid() || w <= 0 || h <= 0) return;
    int hh = imin(h, 256);
    Color rows[256];
    for (int i = 0; i < hh; i++) rows[i] = mix(top, bottom, hh > 1 ? (i * 256) / (hh - 1) : 0);
    int x0 = imax(x, 0), x1 = imin(x + w, s.w);
    for (int cx = x0; cx < x1; cx++) {
        for (int i = 0; i < hh; i++) {
            int yy = y + i;
            if (yy < 0 || yy >= s.h) continue;
            *s.at(cx, yy) = rows[i];
        }
    }
}

void shade_rect(const Surface& s, int x, int y, int w, int h) {
    if (!s.valid() || w <= 0 || h <= 0) return;
    int x0 = imax(x, 0), y0 = imax(y, 0);
    int x1 = imin(x + w, s.w), y1 = imin(y + h, s.h);
    if (x0 >= x1 || y0 >= y1) return;
    int n = y1 - y0;
    for (int cx = x0; cx < x1; cx++) {
        uint16_t* p = s.at(cx, y1 - 1);
        for (int i = 0; i < n; i++) p[i] = (uint16_t)((p[i] & 0xF7DE) >> 1);  // 50 % plus sombre
    }
}

void fill_disc(const Surface& s, int cx, int cy, int r, Color c) {
    if (r <= 0) return;
    for (int dx = -r; dx <= r; dx++) {
        float e = sqrtf((float)(r * r - dx * dx));
        int ext = (int)(e + 0.5f);
        fill_rect(s, cx + dx, cy - ext, 1, 2 * ext + 1, c);
    }
}

void ring(const Surface& s, int cx, int cy, int r, Color c, int t) {
    if (r <= 0 || t < 1) return;
    int ro2 = r * r;
    int ri = r - t;
    int ri2 = ri > 0 ? ri * ri : 0;
    for (int dx = -r; dx <= r; dx++) {
        for (int dy = -r; dy <= r; dy++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= ro2 && d2 >= ri2) s.put(cx + dx, cy + dy, c);
        }
    }
}

void blit_video(const Surface& dst, const uint16_t* src) {
    if (!dst.valid() || !src) return;
    memcpy(dst.px, src, (size_t)dst.w * (size_t)dst.h * sizeof(uint16_t));
}

// ------------------------------------------------------------------- texte
// Lit un point de code UTF-8 ; avance p. Retourne 0 en fin de chaine.
static uint32_t next_cp(const char*& p) {
    const uint8_t* s = (const uint8_t*)p;
    uint32_t c = *s;
    if (c == 0) return 0;
    if (c < 0x80) {
        p++;
        return c;
    }
    int n;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) {
        n = 1;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        n = 2;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        n = 3;
        cp = c & 0x07;
    } else {
        p++;
        return '?';
    }
    for (int i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            p++;
            return '?';
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    p += n + 1;
    return cp;
}

// Point de code Unicode -> glyphe de la police CP437 (les majuscules accentuees absentes de la
// police perdent leur accent).
static uint8_t glyph_for(uint32_t cp) {
    if (cp >= 0x20 && cp < 0x7F) return (uint8_t)cp;
    switch (cp) {
        case 0xC7: return 0x80;                  // C cedille
        case 0xFC: return 0x81;                  // u trema
        case 0xE9: return 0x82;                  // e aigu
        case 0xE2: return 0x83;                  // a circonflexe
        case 0xE4: return 0x84;
        case 0xE0: return 0x85;                  // a grave
        case 0xE5: return 0x86;
        case 0xE7: return 0x87;                  // c cedille
        case 0xEA: return 0x88;                  // e circonflexe
        case 0xEB: return 0x89;
        case 0xE8: return 0x8A;                  // e grave
        case 0xEF: return 0x8B;                  // i trema
        case 0xEE: return 0x8C;                  // i circonflexe
        case 0xEC: return 0x8D;
        case 0xC4: return 0x8E;
        case 0xC5: return 0x8F;
        case 0xC9: return 0x90;                  // E aigu
        case 0xE6: return 0x91;
        case 0xC6: return 0x92;
        case 0xF4: return 0x93;                  // o circonflexe
        case 0xF6: return 0x94;
        case 0xF2: return 0x95;
        case 0xFB: return 0x96;                  // u circonflexe
        case 0xF9: return 0x97;                  // u grave
        case 0xFF: return 0x98;
        case 0xD6: return 0x99;
        case 0xDC: return 0x9A;
        case 0xA2: return 0x9B;
        case 0xA3: return 0x9C;
        case 0xA5: return 0x9D;
        case 0xE1: return 0xA0;
        case 0xED: return 0xA1;
        case 0xF3: return 0xA2;
        case 0xFA: return 0xA3;
        case 0xF1: return 0xA4;
        case 0xD1: return 0xA5;
        case 0xAA: return 0xA6;
        case 0xBA: return 0xA7;
        case 0xBF: return 0xA8;
        case 0xAC: return 0xAA;
        case 0xBD: return 0xAB;
        case 0xBC: return 0xAC;
        case 0xA1: return 0xAD;
        case 0xAB: return 0xAE;                  // «
        case 0xBB: return 0xAF;                  // »
        case 0xB0: return 0xF8;                  // degre
        case 0xB7: return 0xFA;                  // point median
        case 0xB1: return 0xF1;
        case 0xD7: return 'x';
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: return 'A';
        case 0xC8: case 0xCA: case 0xCB: return 'E';
        case 0xCC: case 0xCD: case 0xCE: case 0xCF: return 'I';
        case 0xD2: case 0xD3: case 0xD4: case 0xD5: return 'O';
        case 0xD9: case 0xDA: case 0xDB: return 'U';
        case 0x153: return 'o';                  // oe
        case 0x152: return 'O';
        case 0xA0: return ' ';
        case 0x2018: case 0x2019: return '\'';
        case 0x201C: case 0x201D: return '"';
        case 0x2022: return 0x07;                // puce
        case 0x2190: return 0x1B;                // fleches
        case 0x2191: return 0x18;
        case 0x2192: return 0x1A;
        case 0x2193: return 0x19;
        case 0x25B2: return 0x1E;                // triangles
        case 0x25BC: return 0x1F;
        case 0x25BA: return 0x10;
        case 0x25C4: return 0x11;
        case 0x25A0: return 0xFE;                // carre plein
        default: return '?';
    }
}

static int decode(const char* utf8, uint8_t* out, int cap) {
    int n = 0;
    if (!utf8) return 0;
    const char* p = utf8;
    for (;;) {
        uint32_t cp = next_cp(p);
        if (cp == 0 || n >= cap) break;
        if (cp == 0x2026) {                      // points de suspension : la police n'en a pas, on en met trois
            for (int i = 0; i < 3 && n < cap; i++) out[n++] = '.';
            continue;
        }
        out[n++] = glyph_for(cp);
    }
    return n;
}

static void draw_glyph(const Surface& s, int x, int y, uint8_t g, Color c, int scale) {
    const uint8_t* rows = FONT[g];
    if (scale == 1 && x >= 0 && y >= 0 && x + GLYPH <= s.w && y + GLYPH <= s.h) {
        for (int cx = 0; cx < GLYPH; cx++) {
            uint8_t mask = (uint8_t)(0x80 >> cx);
            uint16_t* col = s.at(x + cx, y);  // indice du pixel (x+cx, y) ; y+r = indice - r
            for (int r = 0; r < GLYPH; r++) {
                if (rows[r] & mask) col[-r] = c;
            }
        }
        return;
    }
    for (int cx = 0; cx < GLYPH; cx++) {
        uint8_t mask = (uint8_t)(0x80 >> cx);
        for (int r = 0; r < GLYPH; r++) {
            if (!(rows[r] & mask)) continue;
            if (scale == 1) s.put(x + cx, y + r, c);
            else fill_rect(s, x + cx * scale, y + r * scale, scale, scale, c);
        }
    }
}

int text_width(const char* utf8, int scale) {
    uint8_t g[160];
    return decode(utf8, g, (int)sizeof(g)) * GLYPH * (scale < 1 ? 1 : scale);
}

int text(const Surface& s, int x, int y, const char* utf8, Color c, int scale) {
    if (scale < 1) scale = 1;
    uint8_t g[160];
    int n = decode(utf8, g, (int)sizeof(g));
    for (int i = 0; i < n; i++) {
        int gx = x + i * GLYPH * scale;
        if (gx + GLYPH * scale <= 0 || gx >= s.w) continue;
        draw_glyph(s, gx, y, g[i], c, scale);
    }
    return n * GLYPH * scale;
}

int text_fit(const Surface& s, int x, int y, const char* utf8, Color c, int max_w, int scale) {
    if (scale < 1) scale = 1;
    uint8_t g[160];
    int n = decode(utf8, g, (int)sizeof(g));
    int cell = GLYPH * scale;
    int fit = max_w / cell;
    if (fit < 1) return 0;
    if (n > fit) {
        int keep = fit > 3 ? fit - 3 : 0;  // les `keep` premiers glyphes restent, puis "..."
        for (int i = keep; i < fit; i++) g[i] = '.';
        n = fit;
    }
    for (int i = 0; i < n; i++) draw_glyph(s, x + i * cell, y, g[i], c, scale);
    return n * cell;
}

int text_center(const Surface& s, int cx, int y, const char* utf8, Color c, int scale) {
    int w = text_width(utf8, scale);
    return text(s, cx - w / 2, y, utf8, c, scale);
}

int text_right(const Surface& s, int right, int y, const char* utf8, Color c, int scale) {
    int w = text_width(utf8, scale);
    return text(s, right - w, y, utf8, c, scale);
}

}  // namespace gfx
