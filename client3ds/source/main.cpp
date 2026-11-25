// GFN 3DS : client Nintendo 3DS d'un relais GeForce NOW installe sur un PC Windows.
//
// Ce programme ne parle PAS a GeForce NOW : il parle au relais (voir ../../relay) qui, lui, pilote le
// client officiel GeForce NOW sur le PC (connexion, captcha, file d'attente), capture sa fenetre,
// l'envoie en JPEG sur le reseau local et rejoue les touches de la 3DS sur une manette virtuelle.
//
// Boucle principale (60 Hz, calee sur la synchronisation verticale) :
//   lecture des entrees -> interface -> envoi des entrees au thread reseau -> dessin si necessaire.
// Le reseau (thread systeme) et le decodage JPEG (autre coeur sur New 3DS) ne bloquent jamais cette
// boucle ; elle ne redessine un ecran que lorsque quelque chose a change.
#include <stdio.h>
#include <string.h>

#include "audio.hpp"
#include "geforce_now.hpp"
#include "input.hpp"
#include "platform.hpp"
#include "renderer.hpp"
#include "settings.hpp"
#include "ui.hpp"
#include "video_decoder.hpp"

namespace {

gfx::Surface surface_of(const plat::Framebuffer& f) {
    gfx::Surface s;
    s.px = f.px;
    s.w = f.w;
    s.h = f.h;
    return s;
}

// Erreur de demarrage : on l'affiche et on attend que l'utilisateur quitte (Start) plutot que de
// disparaitre sans explication.
void fatal(const char* line1, const char* line2) {
    while (plat::app_running()) {
        input::Raw raw;
        input::poll(raw);
        if (raw.pressed & input::K_START) break;
        gfx::Surface top = surface_of(plat::display_top());
        gfx::clear(top, gfx::col::BG);
        gfx::text_center(top, 200, 90, "GFN 3DS", gfx::col::GREEN, 3);
        gfx::text_center(top, 200, 130, line1, gfx::col::RED);
        gfx::text_center(top, 200, 146, line2, gfx::col::DIM);
        gfx::text_center(top, 200, 190, "Start : quitter", gfx::col::DIM);
        plat::display_present(true, false);
        plat::display_vsync();
    }
}

}  // namespace

int main(int, char**) {
    Settings settings;
    settings_load(settings, settings_default_path());   // absent au premier lancement : valeurs par defaut
    settings.sanitize();

    plat::set_cpu_speedup(settings.speedup);   // New 3DS : 804 MHz et cache L2 (sans effet sur Old 3DS)
    plat::display_init();
    plat::allow_syscore(30);                   // doit preceder la creation de threads sur le coeur systeme

    // Gros objets (tampons de reassemblage et d'images) : statiques, la pile de la 3DS est petite.
    static VideoDecoder video;
    static GfnClient gfn(video);

    if (!video.start()) {
        fatal("Memoire insuffisante pour le decodeur video.", "Fermez les autres applications et relancez.");
        plat::display_exit();
        return 1;
    }
    gfn.start();       // un echec (Wi-Fi coupe...) est signale dans l'interface, sans empecher de la parcourir
    audio::init();

    static UI ui(settings, gfn, video);
    input::Raw raw;

    while (plat::app_running() && !ui.quit_requested()) {
        const uint64_t now = plat::now_ms();
        input::poll(raw);
        ui.update(raw, now);
        gfn.set_input(ui.game_input(raw), ui.mapping());
        gfn.set_video_wanted(ui.video_view());

        const bool new_frame = ui.video_view() && video.acquire(now);
        const bool draw_top = new_frame || ui.top_dirty();
        const bool draw_bottom = ui.bottom_dirty();
        if (draw_top) {
            ui.draw_top(surface_of(plat::display_top()), now);
            ui.clear_top_dirty();
        }
        if (draw_bottom) {
            ui.draw_bottom(surface_of(plat::display_bottom()), now);
            ui.clear_bottom_dirty();
        }
        plat::display_present(draw_top, draw_bottom);
        plat::display_vsync();
    }

    // Arret propre : BYE au relais (il libere la manette virtuelle), puis arret des threads.
    gfn.stop();
    video.stop();
    audio::shutdown();
    plat::net_exit();
    plat::display_exit();
    return 0;
}
