// Interface : ecran du haut (etat, file d'attente, video + indicateurs) et ecran du bas (onglets
// Jeux / Reseau / Touches / Options, manette tactile pendant le streaming).
//
// Tout est dessine par le CPU dans les surfaces fournies par le programme principal (voir
// renderer.hpp). L'interface ne bloque jamais : le clavier systeme (swkbd) est le seul appel
// bloquant, et il n'est lance que par une action explicite de l'utilisateur.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "geforce_now.hpp"
#include "input.hpp"
#include "renderer.hpp"
#include "settings.hpp"
#include "video_decoder.hpp"

class UI {
public:
    // Les quatre premieres pages sont les onglets, dans l'ordre d'affichage.
    enum class Page { Games, Network, Keys, Options, Pad, KeyTest, StreamMenu, StreamOptions, QuitConfirm };

    UI(Settings& settings, GfnClient& gfn, VideoDecoder& video);

    // Une fois par image : lit les entrees, met a jour la navigation, envoie les commandes au relais.
    void update(const input::Raw& raw, uint64_t now_ms);
    void draw_top(const gfx::Surface& top, uint64_t now_ms);
    void draw_bottom(const gfx::Surface& bottom, uint64_t now_ms);

    bool top_dirty() const { return top_dirty_; }
    bool bottom_dirty() const { return bottom_dirty_; }
    void clear_top_dirty() { top_dirty_ = false; }
    void clear_bottom_dirty() { bottom_dirty_ = false; }
    void mark_top_dirty() { top_dirty_ = true; }
    void mark_all_dirty() { top_dirty_ = bottom_dirty_ = true; }

    // L'ecran du haut affiche la video du PC : session en cours de streaming (flux demande par cette
    // console), ou "Voir l'ecran du PC" choisi par l'utilisateur.
    bool video_view() const { return view_active_; }
    bool quit_requested() const { return quit_; }

    // Entrees a transmettre au jeu : vide tant qu'un menu est ouvert, et les boutons deja
    // enfonces au moment de revenir sur la manette tactile sont ignores jusqu'a leur relachement.
    input::Raw game_input(const input::Raw& raw);
    input::Mapping mapping() const;

    Page page() const { return page_; }
    const Snapshot& snap() const { return snap_; }
    bool toast_active() const { return toast_until_ != 0; }
    const char* toast_text() const { return toast_; }

private:
    struct Row {
        enum Kind : uint8_t { Header, Item, Value, Toggle, Button, Info };
        Kind kind = Info;
        int id = 0;
        char label[128] = "";
        char value[48] = "";   // Value : valeur affichee ; Item : texte secondaire (adresse...)
        char pill[12] = "";    // Item : action affichee quand la ligne est selectionnee ("Lancer")
        bool on = false;       // Toggle : active ; Button : bouton principal (vert)
        bool enabled = true;
        gfx::Color color = gfx::col::DIM;   // Info : texte ; Button : fond (sauf principal)
    };

    // Liste verticale defilante (tactile + croix directionnelle) : sert a toutes les pages du bas.
    class RowList {
    public:
        void set_area(int x, int y, int w, int h) { ax_ = x; ay_ = y; aw_ = w; ah_ = h; }
        void begin() { rows_.clear(); }
        Row& add(Row::Kind kind, int id, const char* label, const char* value = "");
        void end();                                    // calcule la mise en page, restaure la selection
        void move(int dir);                            // +1 : ligne suivante, -1 : precedente (avec bouclage)
        int selected_id() const { return (sel_ >= 0 && sel_ < (int)rows_.size()) ? rows_[(size_t)sel_].id : -1; }
        const Row* selected_row() const { return (sel_ >= 0 && sel_ < (int)rows_.size()) ? &rows_[(size_t)sel_] : nullptr; }
        void select_id(int id) { sel_id_ = id; }       // appliquee au prochain end()
        void select_index(int i);
        int hit(int x, int y) const;                   // ligne sous le point, -1 sinon
        const Row* row_at(int i) const { return (i >= 0 && i < (int)rows_.size()) ? &rows_[(size_t)i] : nullptr; }
        void scroll_by(int dy);
        void reset_scroll() { scroll_ = 0; }
        void draw(const gfx::Surface& s) const;
        size_t size() const { return rows_.size(); }
        static bool selectable(const Row& r) {
            return r.enabled && (r.kind == Row::Item || r.kind == Row::Value || r.kind == Row::Toggle || r.kind == Row::Button);
        }

    private:
        int height_of(const Row& r) const;
        void ensure_visible();
        std::vector<Row> rows_;
        std::vector<int> off_;
        int total_ = 0;
        int scroll_ = 0;
        int sel_ = -1;
        int sel_id_ = -1;
        int ax_ = 0, ay_ = 24, aw_ = 320, ah_ = 192;
    };

    bool compute_view() const {
        if (snap_.link == Link::Offline) return false;
        // Untracked : le relais ne suit pas la session (journaux non calibres) -> on montre l'ecran du PC tel quel.
        return force_view_ || (snap_.stream_on && (snap_.state == proto::GfnState::Streaming ||
                                                   snap_.state == proto::GfnState::Untracked));
    }
    bool tabs_visible() const { return (int)page_ <= (int)Page::Options && back_page_ != Page::StreamMenu; }
    // Pages qui n'ont de sens que pendant le streaming : on les quitte quand la vue video se ferme.
    bool streaming_page() const {
        return page_ == Page::Pad || page_ == Page::StreamMenu || page_ == Page::StreamOptions ||
               ((page_ == Page::Keys || page_ == Page::KeyTest) && back_page_ == Page::StreamMenu);
    }

    // --- logique
    void build_rows();
    void build_games(RowList& l);
    void build_network(RowList& l);
    void build_keys(RowList& l);
    void build_options(RowList& l, bool stream_only);
    void build_stream_menu(RowList& l);
    void build_quit(RowList& l);
    void activate(int id, int dir);
    void set_page(Page p, Page back = Page::Games);
    void on_snapshot_changed();
    void update_menu();
    void update_pad();
    void update_keytest();
    void handle_touch_list();
    void edit_text(const char* hint, char* target, size_t cap, bool numeric);
    void save_settings();
    void connect_now();
    void cycle_profile(int dir);
    void apply_buffer_mode();
    void maybe_auto_connect();
    void show_toast(const char* msg);
    const char* current_profile_id() const;
    const char* profile_label(const char* id) const;
    bool animating() const;

    // --- dessin
    void draw_tabs(const gfx::Surface& s) const;
    void draw_title_bar(const gfx::Surface& s, const char* title) const;
    void draw_status_bar(const gfx::Surface& s) const;
    void draw_pad(const gfx::Surface& s) const;
    void draw_keytest(const gfx::Surface& s) const;
    void draw_top_status(const gfx::Surface& s) const;
    void draw_top_stream(const gfx::Surface& s);
    void draw_brand(const gfx::Surface& s) const;
    void draw_toast(const gfx::Surface& s, int bottom) const;
    const char* hint_text() const;
    const char* page_title() const;

    Settings& settings_;
    GfnClient& gfn_;
    VideoDecoder& video_;

    Snapshot snap_;
    std::vector<GameEntry> games_;
    std::vector<ProfileEntry> profiles_;
    std::vector<RelayEntry> relays_;
    uint32_t games_ver_ = 0, profiles_ver_ = 0, relays_ver_ = 0, toast_seq_ = 0;
    uint32_t applied_profiles_ver_ = 0;
    uint32_t sig_ = 0;

    Page page_ = Page::Games;
    Page back_page_ = Page::Games;
    RowList list_;
    input::Raw raw_;
    uint64_t now_ = 0;
    uint64_t last_tick_ = 0;
    uint64_t last_anim_ = 0;
    uint64_t last_sys_ = 0;
    uint64_t toast_until_ = 0;
    char toast_[110] = "";
    int wifi_ = 0;
    int batt_ = -1;
    bool charging_ = false;

    // navigation a la croix directionnelle avec repetition
    uint64_t next_repeat_ = 0;
    int hold_dir_ = 0;

    // geste tactile sur une liste (defilement ou appui)
    bool touch_down_ = false;
    bool touch_dragging_ = false;
    int touch_start_y_ = 0;
    int touch_last_y_ = 0;
    int touch_row_ = -1;

    // retour sur la manette : ignore ce qui etait deja enfonce
    uint32_t swallow_mask_ = 0;
    bool swallow_touch_ = false;

    uint32_t kt_sig_ = 0;
    bool force_view_ = false;    // "Voir l'ecran du PC" : video affichee meme hors streaming (connexion, captcha...)
    bool view_active_ = false;
    bool top_dirty_ = true;
    bool bottom_dirty_ = true;
    bool quit_ = false;
    bool auto_tried_ = false;
    proto::GfnState last_state_ = proto::GfnState::Unknown;   // pour reperer un changement d'etat
};
