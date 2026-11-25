// Client sans ecran : fait tourner le VRAI code du client (GfnClient, VideoDecoder, UI) sur PC, pilote
// par des commandes sur stdin, pour le tester contre le VRAI relais Python
// (voir relay/tests/test_client_e2e.py). Ce n'est pas du code de la console : la couche plate-forme
// (platform.cpp, branche PC) remplace libctru par des threads standard et des tampons d'ecran en memoire.
//
// Une ligne de commande -> une ligne de reponse ("OK ..." ou "ERR ..."). Commandes :
//   run <ms>                         fait tourner la boucle principale (60 Hz, comme main.cpp) pendant <ms> ms
//   ui_settings <host> <port> <pin>  reglages utilises par l'interface ("-" : code vide)
//   connect <host> <port> <pin> <auto>   commande directe au client reseau
//   disconnect | launch <id> | watch | stop | profile <id>
//   press <hex>                      boutons maintenus (bits input::Src, K_START = 0x1000, K_SELECT = 0x2000)
//   cpad <x> <y>                     Circle Pad
//   touch <x> <y> | release | tap <x> <y>
//   status | games | relays | frame | page
//   shot <prefixe>                   ecrit <prefixe>_top.ppm et <prefixe>_bottom.ppm
//   quit
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "geforce_now.hpp"
#include "input.hpp"
#include "platform.hpp"
#include "renderer.hpp"
#include "settings.hpp"
#include "ui.hpp"
#include "video_decoder.hpp"

namespace {

Settings g_settings;
VideoDecoder g_video;
GfnClient g_gfn(g_video);
input::Raw g_cur, g_prev;
UI* g_ui = nullptr;
uint32_t g_frames_drawn_top = 0, g_frames_drawn_bottom = 0;

gfx::Surface surface_of(const plat::Framebuffer& f) {
    gfx::Surface s;
    s.px = f.px;
    s.w = f.w;
    s.h = f.h;
    return s;
}

void step_once() {
    const uint64_t now = plat::now_ms();
    input::Raw r = g_cur;
    r.pressed = g_cur.held & ~g_prev.held;
    r.released = g_prev.held & ~g_cur.held;
    r.touch_pressed = g_cur.touch && !g_prev.touch;
    r.touch_released = !g_cur.touch && g_prev.touch;
    g_prev = g_cur;
    g_ui->update(r, now);
    g_gfn.set_input(g_ui->game_input(r), g_ui->mapping());
    g_gfn.set_video_wanted(g_ui->video_view());
    if (g_ui->video_view()) g_video.acquire(now);
    const bool top = g_ui->top_dirty();
    const bool bottom = g_ui->bottom_dirty();
    if (top) {
        g_ui->draw_top(surface_of(plat::display_top()), now);
        g_ui->clear_top_dirty();
        g_frames_drawn_top++;
    }
    if (bottom) {
        g_ui->draw_bottom(surface_of(plat::display_bottom()), now);
        g_ui->clear_bottom_dirty();
        g_frames_drawn_bottom++;
    }
    plat::display_present(top, bottom);
}

void run_for(uint64_t ms) {
    const uint64_t end = plat::now_ms() + ms;
    while (plat::now_ms() < end) {
        step_once();
        plat::sleep_ms(16);
    }
}

void rgb565_to_rgb(uint16_t c, uint8_t* out) {
    unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    out[0] = (uint8_t)((r * 255 + 15) / 31);
    out[1] = (uint8_t)((g * 255 + 31) / 63);
    out[2] = (uint8_t)((b * 255 + 15) / 31);
}

bool write_ppm(const char* path, const uint16_t* fb, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t rgb[3];
            rgb565_to_rgb(fb[(size_t)x * (size_t)h + (size_t)(h - 1 - y)], rgb);
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return true;
}

const char* page_name(UI::Page p) {
    switch (p) {
        case UI::Page::Games: return "games";
        case UI::Page::Network: return "network";
        case UI::Page::Keys: return "keys";
        case UI::Page::Options: return "options";
        case UI::Page::Pad: return "pad";
        case UI::Page::KeyTest: return "keytest";
        case UI::Page::StreamMenu: return "streammenu";
        case UI::Page::StreamOptions: return "streamoptions";
        case UI::Page::QuitConfirm: return "quit";
    }
    return "?";
}

std::string clean(const char* s) {
    std::string o;
    for (; *s; s++) o += ((unsigned char)*s < 32 || *s == ' ') ? '_' : *s;
    return o.empty() ? "-" : o;
}

const char* link_name(Link l) {
    switch (l) {
        case Link::Offline: return "offline";
        case Link::Connecting: return "connecting";
        case Link::Handshake: return "handshake";
        case Link::Online: return "online";
        case Link::Reconnecting: return "reconnecting";
    }
    return "?";
}

const char* state_name(proto::GfnState st) {
    switch (st) {
        case proto::GfnState::Unknown: return "unknown";
        case proto::GfnState::Idle: return "idle";
        case proto::GfnState::Launching: return "launching";
        case proto::GfnState::Queue: return "queue";
        case proto::GfnState::Loading: return "loading";
        case proto::GfnState::Streaming: return "streaming";
        case proto::GfnState::Error: return "error";
        case proto::GfnState::Untracked: return "untracked";
    }
    return "?";
}

void print_status() {
    Snapshot s = g_gfn.snapshot();
    VideoStats v = g_video.stats();
    std::vector<RelayEntry> relays;
    g_gfn.copy_relays(relays);
    printf("OK link=%s state=%s queue=%d eta=%d game=%s stream_on=%d pad=%d profile=%s rtt=%d relay=%s "
           "fatal=%d attempt=%d games=%u relays=%d decoded=%u lost=%u kbps=%d view=%d page=%s toast=%s msg=%s\n",
           link_name(s.link), state_name(s.state), s.queue_pos, s.eta_s, clean(s.game).c_str(), s.stream_on ? 1 : 0,
           s.pad_available ? 1 : 0, clean(s.profile).c_str(), s.rtt_ms, clean(s.relay_name).c_str(), s.fatal ? 1 : 0,
           s.attempt, (unsigned)s.games_version, (int)relays.size(), (unsigned)v.decoded_total, (unsigned)v.lost_total,
           v.kbps, g_ui->video_view() ? 1 : 0, page_name(g_ui->page()), clean(g_ui->toast_text()).c_str(),
           clean(s.message).c_str());
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    g_settings.set_defaults();
    snprintf(g_settings.host, sizeof(g_settings.host), "127.0.0.1");
    g_settings.auto_connect = false;
    plat::display_init();
    if (!g_video.start()) {
        printf("ERR video\n");
        return 1;
    }
    g_gfn.start();
    UI ui(g_settings, g_gfn, g_video);
    g_ui = &ui;
    printf("READY\n");

    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        char* argv[8];
        int argc = 0;
        for (char* p = strtok(line, " \r\n"); p && argc < 8; p = strtok(nullptr, " \r\n")) argv[argc++] = p;
        if (argc == 0) continue;
        const char* c = argv[0];
        if (!strcmp(c, "run") && argc >= 2) {
            run_for((uint64_t)atoi(argv[1]));
            printf("OK\n");
        } else if (!strcmp(c, "ui_settings") && argc >= 4) {
            snprintf(g_settings.host, sizeof(g_settings.host), "%s", argv[1]);
            g_settings.port = atoi(argv[2]);
            snprintf(g_settings.pin, sizeof(g_settings.pin), "%s", strcmp(argv[3], "-") ? argv[3] : "");
            printf("OK\n");
        } else if (!strcmp(c, "connect") && argc >= 5) {
            g_gfn.connect(argv[1], atoi(argv[2]), strcmp(argv[3], "-") ? argv[3] : "", atoi(argv[4]) != 0);
            printf("OK\n");
        } else if (!strcmp(c, "disconnect")) {
            g_gfn.disconnect();
            printf("OK\n");
        } else if (!strcmp(c, "launch") && argc >= 2) {
            g_gfn.launch(argv[1]);
            printf("OK\n");
        } else if (!strcmp(c, "watch")) {
            g_gfn.watch();
            printf("OK\n");
        } else if (!strcmp(c, "stop")) {
            g_gfn.stop_game();
            printf("OK\n");
        } else if (!strcmp(c, "profile") && argc >= 2) {
            g_gfn.set_profile(argv[1]);
            printf("OK\n");
        } else if (!strcmp(c, "press") && argc >= 2) {
            g_cur.held = (uint32_t)strtoul(argv[1], nullptr, 16);
            printf("OK\n");
        } else if (!strcmp(c, "cpad") && argc >= 3) {
            g_cur.cpad_x = atoi(argv[1]);
            g_cur.cpad_y = atoi(argv[2]);
            printf("OK\n");
        } else if (!strcmp(c, "touch") && argc >= 3) {
            g_cur.touch = true;
            g_cur.tx = atoi(argv[1]);
            g_cur.ty = atoi(argv[2]);
            printf("OK\n");
        } else if (!strcmp(c, "release")) {
            g_cur.touch = false;
            printf("OK\n");
        } else if (!strcmp(c, "tap") && argc >= 3) {
            g_cur.touch = true;
            g_cur.tx = atoi(argv[1]);
            g_cur.ty = atoi(argv[2]);
            run_for(80);
            g_cur.touch = false;
            run_for(80);
            printf("OK\n");
        } else if (!strcmp(c, "status")) {
            print_status();
        } else if (!strcmp(c, "page")) {
            printf("OK %s\n", page_name(ui.page()));
        } else if (!strcmp(c, "games")) {
            std::vector<GameEntry> g;
            g_gfn.copy_games(g);
            printf("OK %d", (int)g.size());
            for (const GameEntry& e : g) printf(" %s=%s", e.id, clean(e.name).c_str());
            printf("\n");
        } else if (!strcmp(c, "relays")) {
            std::vector<RelayEntry> r;
            g_gfn.copy_relays(r);
            printf("OK %d", (int)r.size());
            for (const RelayEntry& e : r) {
                char ip[20];
                net::ip_to_str(e.ip_be, ip, sizeof(ip));
                printf(" %s:%d=%s", ip, e.port, clean(e.name).c_str());
            }
            printf("\n");
        } else if (!strcmp(c, "frame")) {
            const uint16_t* f = g_video.frame();
            if (!f) {
                printf("ERR noframe\n");
            } else {
                // coins de l'image affichee : (0,0) (399,0) (0,239) (399,239), au format RGB565 du tampon
                auto px = [&](int x, int y) { return f[(size_t)x * 240 + (size_t)(239 - y)]; };
                printf("OK TL=%04x TR=%04x BL=%04x BR=%04x\n", px(2, 2), px(397, 2), px(2, 237), px(397, 237));
            }
        } else if (!strcmp(c, "shot") && argc >= 2) {
            const uint64_t now = plat::now_ms();
            char path[300];
            ui.draw_top(surface_of(plat::display_top()), now);
            snprintf(path, sizeof(path), "%s_top.ppm", argv[1]);
            bool ok = write_ppm(path, plat::display_top().px, 400, 240);
            ui.draw_bottom(surface_of(plat::display_bottom()), now);
            snprintf(path, sizeof(path), "%s_bottom.ppm", argv[1]);
            ok = write_ppm(path, plat::display_bottom().px, 320, 240) && ok;
            ui.mark_all_dirty();
            printf(ok ? "OK\n" : "ERR write\n");
        } else if (!strcmp(c, "quit")) {
            break;
        } else {
            printf("ERR unknown\n");
        }
    }
    g_gfn.stop();
    g_video.stop();
    return 0;
}
