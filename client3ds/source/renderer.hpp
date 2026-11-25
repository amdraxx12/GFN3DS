// Rendu logiciel dans les tampons d'ecran de la 3DS (RGB565).
//
// Pourquoi pas citro2d/citro3d : decoder le JPEG puis l'envoyer au GPU imposerait de convertir
// l'image en texture "swizzlee" (de la copie en plus pour le CPU, le maillon faible de l'Old 3DS).
// Ici le decodeur ecrit directement dans la disposition de l'ecran et l'interface est dessinee par
// le CPU par-dessus : aucune conversion, aucune dependance.
//
// Disposition des tampons : l'ecran de la 3DS est monte "couche" ; la memoire est rangee par
// colonnes, de bas en haut. Le pixel (x, y) (origine en haut a gauche) est a l'indice
//     x * h + (h - 1 - y)
// ou h est la hauteur de l'ecran (240). Un tampon video 240 x 400 decode par ligne (JPEG pivote de
// 90 degres cote PC) a donc exactement cette disposition.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace gfx {

typedef uint16_t Color;  // RGB565

constexpr Color rgb(int r, int g, int b) {
    return (Color)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xFF) >> 3));
}

// Melange lineaire de deux couleurs : t = 0 donne a, t = 256 donne b.
Color mix(Color a, Color b, int t);

// Palette inspiree de l'interface GeForce NOW (noir charbon + vert NVIDIA).
namespace col {
constexpr Color BG = rgb(18, 18, 18);
constexpr Color PANEL = rgb(32, 32, 32);
constexpr Color PANEL_HI = rgb(46, 46, 46);
constexpr Color LINE = rgb(64, 64, 64);
constexpr Color TEXT = rgb(235, 235, 235);
constexpr Color DIM = rgb(150, 150, 150);
constexpr Color GREEN = rgb(118, 185, 0);
constexpr Color GREEN_DK = rgb(66, 104, 0);
constexpr Color AMBER = rgb(245, 166, 35);
constexpr Color RED = rgb(229, 72, 77);
constexpr Color BLUE = rgb(70, 160, 240);
constexpr Color BLACK = rgb(0, 0, 0);
constexpr Color WHITE = rgb(255, 255, 255);
}  // namespace col

struct Surface {
    uint16_t* px = nullptr;
    int w = 0;
    int h = 0;

    bool valid() const { return px != nullptr && w > 0 && h > 0; }
    uint16_t* at(int x, int y) const { return px + (size_t)x * (size_t)h + (size_t)(h - 1 - y); }
    // Vrai si (x, y) est dans la surface.
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    void put(int x, int y, Color c) const {
        if (inside(x, y)) *at(x, y) = c;
    }
};

void clear(const Surface& s, Color c);
void fill_rect(const Surface& s, int x, int y, int w, int h, Color c);
void frame_rect(const Surface& s, int x, int y, int w, int h, Color c, int thickness = 1);
// Rectangle aux coins arrondis (rayon <= 6 environ).
void fill_rrect(const Surface& s, int x, int y, int w, int h, int r, Color c);
void vgradient(const Surface& s, int x, int y, int w, int h, Color top, Color bottom);
// Assombrit une zone de moitie (texte lisible par-dessus la video).
void shade_rect(const Surface& s, int x, int y, int w, int h);
void fill_disc(const Surface& s, int cx, int cy, int r, Color c);
void ring(const Surface& s, int cx, int cy, int r, Color c, int thickness = 1);

// Copie une image video deja a la disposition de l'ecran (400 x 240) dans la surface.
void blit_video(const Surface& dst, const uint16_t* src);

// ---- texte : police 8x8 de libctru (CP437), chaines en UTF-8 (les accents francais sont geres)
constexpr int GLYPH = 8;
int text_width(const char* utf8, int scale = 1);
int text(const Surface& s, int x, int y, const char* utf8, Color c, int scale = 1);  // retourne la largeur
// Tronque avec "..." pour tenir dans max_w pixels.
int text_fit(const Surface& s, int x, int y, const char* utf8, Color c, int max_w, int scale = 1);
int text_center(const Surface& s, int cx, int y, const char* utf8, Color c, int scale = 1);
int text_right(const Surface& s, int right, int y, const char* utf8, Color c, int scale = 1);

}  // namespace gfx
