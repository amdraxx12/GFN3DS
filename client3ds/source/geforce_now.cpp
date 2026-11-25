// Client du relais GeForce NOW : voir geforce_now.hpp.
#include "geforce_now.hpp"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

const int CONNECT_TIMEOUT_MS = 5000;
const int HANDSHAKE_TIMEOUT_MS = 5000;
const int DEAD_LINK_MS = 6000;       // aucune ligne recue (meme pas un PONG) depuis ce delai : liaison morte
const int INPUT_PERIOD_MS = 16;      // ~60 Hz
const int RELAY_EXPIRY_MS = 9000;    // une balise toutes les 2 s : 9 s sans balise = relais disparu
const size_t MAX_GAMES = 512;

void copy_str(char* dst, size_t cap, const char* src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

}  // namespace

// ============================================================================ cycle de vie
GfnClient::GfnClient(VideoDecoder& video)
    : video_(video), running_(false), link_(Link::Offline), user_disconnect_(true),
      auto_reconnect_(true), want_stream_(false), video_on_(false), attempt_(0), deadline_(0), next_attempt_(0),
      last_rx_(0), next_ping_(0), next_input_(0), next_report_(0), next_prune_(0), server_ip_be_(0),
      input_port_(proto::PORT_INPUT), input_seq_(0), rtt_avg_(-1.0f), games_expected_(0),
      profiles_expected_(0), building_games_(false), building_profiles_(false), target_port_(proto::PORT_CONTROL),
      target_auto_(true), video_wanted_(false), cur_port_(proto::PORT_CONTROL) {
    target_host_[0] = '\0';
    target_pin_[0] = '\0';
    cur_host_[0] = '\0';
    cur_pin_[0] = '\0';
}

GfnClient::~GfnClient() { stop(); }

bool GfnClient::start() {
    if (running_) return true;
    if (!plat::net_init()) {
        set_message("Réseau indisponible : le Wi-Fi est-il activé ?");
        return false;
    }
    if (!video_sock_.open(proto::PORT_VIDEO, 256 * 1024, false) || !input_sock_.open_sender()) {
        set_message("Impossible d'ouvrir les sockets UDP");
        return false;
    }
    beacon_sock_.open(proto::PORT_BEACON, 0, true);   // la decouverte est facultative : l'echec est ignore
    running_ = true;
    // Travail leger (poll + copies) : coeur 1 si disponible, un peu plus prioritaire que l'interface.
    if (!thread_.start(&GfnClient::thread_main, this, 0x8000, plat::CoreHint::Syscore, -1)) {
        running_ = false;
        set_message("Impossible de démarrer le thread réseau");
        return false;
    }
    return true;
}

void GfnClient::stop() {
    if (!running_) return;
    running_ = false;
    thread_.join();
    video_sock_.close();
    input_sock_.close();
    beacon_sock_.close();
}

void GfnClient::thread_main(void* self) { ((GfnClient*)self)->run(); }

void GfnClient::run() {
    while (running_) {
        uint64_t now = plat::now_ms();
        process_commands(now);
        now = plat::now_ms();
        switch (link_) {
            case Link::Offline: step_idle(now, 30); break;
            case Link::Reconnecting:
                step_idle(now, 30);
                {
                    now = plat::now_ms();
                    int s = next_attempt_ > now ? (int)((next_attempt_ - now + 999) / 1000) : 0;
                    plat::Lock l(mu_);
                    snap_.retry_in_s = s;
                }
                if (plat::now_ms() >= next_attempt_) start_attempt(plat::now_ms());
                break;
            case Link::Connecting: step_connecting(now); break;
            case Link::Handshake: step_handshake(now); break;
            case Link::Online: step_online(now); break;
        }
    }
    if (tcp_.is_connected()) {
        tcp_.send_line("BYE");
        tcp_.flush();
    }
    tcp_.close();
}

// ============================================================================ API (thread principal)
void GfnClient::connect(const char* host, int port, const char* pin, bool auto_reconnect) {
    plat::Lock l(mu_);
    copy_str(target_host_, sizeof(target_host_), host ? host : "");
    target_port_ = port;
    copy_str(target_pin_, sizeof(target_pin_), pin ? pin : "");
    target_auto_ = auto_reconnect;
    Cmd c;
    c.type = Cmd::Connect;
    cmds_.push_back(c);
}

void GfnClient::disconnect() {
    plat::Lock l(mu_);
    Cmd c;
    c.type = Cmd::Disconnect;
    cmds_.push_back(c);
}

void GfnClient::refresh_games() {
    plat::Lock l(mu_);
    Cmd c;
    c.type = Cmd::List;
    cmds_.push_back(c);
}

void GfnClient::launch(const char* game_id) {
    plat::Lock l(mu_);
    Cmd c;
    c.type = Cmd::Launch;
    copy_str(c.arg, sizeof(c.arg), game_id ? game_id : "");
    cmds_.push_back(c);
}

void GfnClient::watch() {
    plat::Lock l(mu_);
    Cmd c;
    c.type = Cmd::Watch;
    cmds_.push_back(c);
}

void GfnClient::stop_game() {
    plat::Lock l(mu_);
    Cmd c;
    c.type = Cmd::Stop;
    cmds_.push_back(c);
}

void GfnClient::set_profile(const char* id) {
    plat::Lock l(mu_);
    Cmd c;
    c.type = Cmd::Profile;
    copy_str(c.arg, sizeof(c.arg), id ? id : "");
    cmds_.push_back(c);
}

void GfnClient::set_input(const input::Raw& raw, const input::Mapping& map) {
    plat::Lock l(in_mu_);
    in_raw_ = raw;
    in_map_ = map;
}

void GfnClient::set_video_wanted(bool on) {
    plat::Lock l(mu_);
    video_wanted_ = on;
}

Snapshot GfnClient::snapshot() {
    plat::Lock l(mu_);
    return snap_;
}

void GfnClient::copy_games(std::vector<GameEntry>& out) {
    plat::Lock l(mu_);
    out = games_;
}

void GfnClient::copy_profiles(std::vector<ProfileEntry>& out) {
    plat::Lock l(mu_);
    out = profiles_;
}

void GfnClient::copy_relays(std::vector<RelayEntry>& out) {
    plat::Lock l(mu_);
    out = relays_;
}

// ============================================================================ etat partage
void GfnClient::set_link(Link l) {
    link_ = l;
    plat::Lock lk(mu_);
    snap_.link = l;
}

void GfnClient::set_message(const char* msg) {
    plat::Lock l(mu_);
    copy_str(snap_.message, sizeof(snap_.message), msg);
}

void GfnClient::set_toast(const char* msg) {
    plat::Lock l(mu_);
    copy_str(snap_.toast, sizeof(snap_.toast), msg);
    snap_.toast_seq++;
}

void GfnClient::set_want_stream(bool v) {
    want_stream_ = v;
    plat::Lock l(mu_);
    snap_.stream_on = v;
}

// ============================================================================ commandes
void GfnClient::process_commands(uint64_t now) {
    std::vector<Cmd> local;
    {
        plat::Lock l(mu_);
        if (cmds_.empty()) return;
        local.swap(cmds_);
    }
    for (size_t i = 0; i < local.size(); i++) {
        const Cmd& c = local[i];
        switch (c.type) {
            case Cmd::Connect: {
                {
                    plat::Lock l(mu_);
                    copy_str(cur_host_, sizeof(cur_host_), target_host_);
                    cur_port_ = target_port_;
                    copy_str(cur_pin_, sizeof(cur_pin_), target_pin_);
                    auto_reconnect_ = target_auto_;
                    snap_.fatal = false;
                    snap_.attempt = 0;
                    snap_.retry_in_s = 0;
                }
                if (tcp_.is_open()) tcp_.close();
                user_disconnect_ = false;
                attempt_ = 0;
                set_want_stream(false);      // nouvelle connexion voulue par l'utilisateur : le flux se redemande explicitement
                start_attempt(now);
                break;
            }
            case Cmd::Disconnect:
                user_disconnect_ = true;
                set_want_stream(false);
                if (tcp_.is_connected()) {
                    tcp_.send_line("BYE");
                    tcp_.flush();
                }
                tcp_.close();
                set_link(Link::Offline);
                {
                    plat::Lock l(mu_);
                    snap_.state = proto::GfnState::Unknown;
                    snap_.queue_pos = snap_.eta_s = -1;
                    snap_.game[0] = '\0';
                    snap_.state_text[0] = '\0';
                    snap_.rtt_ms = snap_.rtt_avg_ms = -1;
                    snap_.attempt = 0;
                    snap_.fatal = false;
                    copy_str(snap_.message, sizeof(snap_.message), "Déconnecté");
                }
                break;
            case Cmd::List: send_cmd_line("LIST", nullptr); break;
            case Cmd::Launch:
                if (link_ == Link::Online) set_want_stream(true);
                send_cmd_line("LAUNCH\t%s", c.arg);
                break;
            case Cmd::Watch:
                if (link_ == Link::Online) set_want_stream(true);
                send_cmd_line("WATCH", nullptr);
                break;
            case Cmd::Stop:
                if (link_ == Link::Online) set_want_stream(false);
                send_cmd_line("STOP", nullptr);
                break;
            case Cmd::Profile: send_cmd_line("PROFILE\t%s", c.arg); break;
            default: break;
        }
    }
}

// Envoie une commande de controle ; ignoree (avec un message) si la liaison n'est pas etablie.
void GfnClient::send_cmd_line(const char* fmt, const char* arg) {
    if (link_ != Link::Online) {
        set_toast("Pas connecté au relais");
        return;
    }
    char line[proto::MAX_LINE];
    if (arg) {
        // Les arguments viennent de l'interface (identifiants du relais) : jamais de tabulation ni de saut de ligne.
        char clean[64];
        size_t j = 0;
        for (const char* p = arg; *p && j < sizeof(clean) - 1; p++)
            if ((unsigned char)*p >= 32) clean[j++] = *p;
        clean[j] = '\0';
        snprintf(line, sizeof(line), fmt, clean);
    } else {
        copy_str(line, sizeof(line), fmt);
    }
    if (!tcp_.send_line(line)) link_lost(plat::now_ms(), "Connexion perdue");
}

// ============================================================================ etats de liaison
void GfnClient::start_attempt(uint64_t now) {
    uint32_t ip = 0;
    if (!net::resolve_ipv4(cur_host_, &ip)) {
        attempt_failed(now, cur_host_[0] ? "Adresse du relais invalide" : "Aucun relais choisi", true);
        return;
    }
    server_ip_be_ = ip;
    char text[28];
    char ips[20];
    net::ip_to_str(ip, ips, sizeof(ips));
    snprintf(text, sizeof(text), "%s:%d", ips, cur_port_);
    {
        plat::Lock l(mu_);
        copy_str(snap_.server, sizeof(snap_.server), text);
    }
    if (!tcp_.begin_connect(ip, cur_port_)) {
        attempt_failed(now, "Connexion impossible (Wi-Fi ?)", false);
        return;
    }
    deadline_ = now + CONNECT_TIMEOUT_MS;
    set_link(Link::Connecting);
    set_message("Connexion au relais…");
}

void GfnClient::attempt_failed(uint64_t now, const char* msg, bool fatal) {
    tcp_.close();
    bool retry = !fatal && auto_reconnect_ && !user_disconnect_;
    if (!retry) {
        set_link(Link::Offline);
        plat::Lock l(mu_);
        snap_.fatal = fatal;
        snap_.attempt = 0;
        copy_str(snap_.message, sizeof(snap_.message), msg);
        return;
    }
    attempt_++;
    uint32_t delay = 500u << (attempt_ > 4 ? 3 : attempt_ - 1);   // 0,5 s, 1 s, 2 s, puis 4 s
    next_attempt_ = now + delay;
    set_link(Link::Reconnecting);
    plat::Lock l(mu_);
    snap_.fatal = false;
    snap_.attempt = attempt_;
    snap_.retry_in_s = (int)((delay + 999) / 1000);
    copy_str(snap_.message, sizeof(snap_.message), msg);
}

void GfnClient::link_lost(uint64_t now, const char* msg) {
    attempt_ = 0;
    attempt_failed(now, msg, false);
}

void GfnClient::step_idle(uint64_t now, int wait_ms) {
    if (beacon_sock_.fd() >= 0) {
        struct pollfd p;
        p.fd = beacon_sock_.fd();
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, wait_ms) > 0 && (p.revents & POLLIN)) poll_beacons(now);
    } else {
        plat::sleep_ms((uint32_t)wait_ms);
    }
    if (now >= next_prune_) {
        next_prune_ = now + 1000;
        plat::Lock l(mu_);
        for (size_t i = 0; i < relays_.size();) {
            if (now - relays_[i].seen_ms > (uint64_t)RELAY_EXPIRY_MS) {
                relays_.erase(relays_.begin() + (long)i);
                snap_.relays_version++;
            } else {
                i++;
            }
        }
    }
}

void GfnClient::step_connecting(uint64_t now) {
    net::TcpLine::Status st = tcp_.poll_connect(20);
    if (st == net::TcpLine::Connected) {
        char line[proto::MAX_LINE];
        snprintf(line, sizeof(line), "HELLO\t%d\t%s\t%s\t%d", proto::PROTO_VERSION,
                 plat::is_new3ds() ? "new" : "old", cur_pin_, video_sock_.local_port());
        if (!tcp_.send_line(line)) {
            attempt_failed(now, "Connexion perdue pendant la négociation", false);
            return;
        }
        deadline_ = now + HANDSHAKE_TIMEOUT_MS;
        set_link(Link::Handshake);
        set_message("Négociation avec le relais…");
    } else if (st == net::TcpLine::Failed) {
        attempt_failed(now, "Relais injoignable (adresse, port ou pare-feu ?)", false);
    } else if (now >= deadline_) {
        attempt_failed(now, "Le relais ne répond pas (adresse, port ou pare-feu ?)", false);
    }
}

void GfnClient::step_handshake(uint64_t now) {
    struct pollfd p;
    p.fd = tcp_.fd();
    p.events = POLLIN;
    p.revents = 0;
    if (p.fd >= 0) poll(&p, 1, 20);

    char line[proto::MAX_LINE];
    int r = (p.revents & (POLLIN | POLLHUP | POLLERR)) ? tcp_.read_line(line, sizeof(line)) : 0;
    if (r < 0) {
        attempt_failed(now, "Le relais a fermé la connexion", false);
        return;
    }
    if (r == 1) {
        char* f[8];
        int n = proto::split_fields(line, f, 8);
        if (n >= 4 && !strcmp(f[0], "OK")) {
            input_port_ = atoi(f[3]);
            if (input_port_ < 1 || input_port_ > 65535) input_port_ = proto::PORT_INPUT;
            // Datagrammes video d'une session precedente : on les jette.
            uint8_t junk[1500];
            for (int i = 0; i < 256 && video_sock_.recv(junk, sizeof(junk), nullptr, nullptr) > 0; i++) {}
            video_.reset();
            reasm_.reset();
            attempt_ = 0;
            last_rx_ = now;
            next_ping_ = now;
            next_input_ = now;
            next_report_ = now + 1000;
            {
                plat::Lock l(mu_);
                copy_str(snap_.relay_name, sizeof(snap_.relay_name), f[2]);
                snap_.attempt = 0;
                snap_.retry_in_s = 0;
                snap_.fatal = false;
                copy_str(snap_.message, sizeof(snap_.message), "Connecté");
            }
            set_link(Link::Online);
            tcp_.send_line("LIST");
            // Reconnexion apres une coupure pendant une session : le relais a une nouvelle session
            // sans flux, on le redemande (sans quoi l'ecran resterait fige).
            if (want_stream_) tcp_.send_line("WATCH");
        } else if (n >= 3 && !strcmp(f[0], "ERR")) {
            // Un code PIN refuse ou un protocole incompatible ne s'arrangent pas en reessayant.
            bool fatal = !strcmp(f[1], "auth") || !strcmp(f[1], "proto");
            attempt_failed(now, f[2], fatal);
        } else {
            attempt_failed(now, "Réponse inattendue du relais", true);
        }
        return;
    }
    if (now >= deadline_) attempt_failed(now, "Le relais n'a pas répondu à la négociation", false);
}

void GfnClient::step_online(uint64_t now) {
    struct pollfd fds[3];
    int n = 0;
    fds[n].fd = tcp_.fd();
    fds[n].events = (short)(POLLIN | (tcp_.wants_write() ? POLLOUT : 0));
    fds[n].revents = 0;
    n++;
    fds[n].fd = video_sock_.fd();
    fds[n].events = POLLIN;
    fds[n].revents = 0;
    n++;
    if (beacon_sock_.fd() >= 0) {
        fds[n].fd = beacon_sock_.fd();
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;
    }
    // Pendant que la video est affichee, on reveille le thread souvent (entrees a 60 Hz) ; sinon 20 ms suffisent.
    int timeout = (want_stream_ && video_on_) ? 4 : 20;
    poll(fds, (nfds_t)n, timeout);
    now = plat::now_ms();

    // La vue video s'ouvre ou se ferme : on ne decode les images que pendant qu'elles sont affichees.
    bool wanted;
    {
        plat::Lock l(mu_);
        wanted = video_wanted_;
    }
    if (wanted != video_on_) {
        video_on_ = wanted;
        if (wanted) {                  // pas d'image perimee d'une vue precedente, statistiques a zero
            video_.reset();
            reasm_.reset();
            next_input_ = now;
            next_report_ = now + 1000;
        }
    }
    const bool streaming = want_stream_ && video_on_;

    // --- controle TCP
    if (fds[0].revents & POLLOUT) {
        if (!tcp_.flush()) {
            link_lost(now, "Connexion perdue");
            return;
        }
    }
    if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
        char line[proto::MAX_LINE];
        for (int guard = 0; guard < 32; guard++) {
            int r = tcp_.read_line(line, sizeof(line));
            if (r == 1) {
                last_rx_ = now;
                handle_line(line, now);
                if (link_ != Link::Online) return;
            } else if (r == 0) {
                break;
            } else {
                link_lost(now, "Connexion perdue");
                return;
            }
        }
    }

    // --- video UDP
    if (fds[1].revents & POLLIN) {
        uint8_t pkt[1500];
        uint32_t ip = 0;
        for (int i = 0; i < 96; i++) {
            int len = video_sock_.recv(pkt, sizeof(pkt), &ip, nullptr);
            if (len <= 0) break;
            if (ip != server_ip_be_) continue;           // pas de notre relais : on ignore
            handle_video(pkt, len, now);
        }
        video_.note_incomplete(reasm_.take_incomplete());
    }

    // --- balises
    if (n > 2 && (fds[2].revents & POLLIN)) poll_beacons(now);

    // --- taches periodiques
    if (streaming && now >= next_input_) {
        send_input(now);
        next_input_ += INPUT_PERIOD_MS;
        if (next_input_ + 50 < now) next_input_ = now + INPUT_PERIOD_MS;   // en retard : on ne rattrape pas
    }
    if (now >= next_ping_) {
        char line[40];
        snprintf(line, sizeof(line), "PING\t%u", (unsigned)(uint32_t)now);
        if (!tcp_.send_line(line)) {
            link_lost(now, "Connexion perdue");
            return;
        }
        next_ping_ = now + 1000;
    }
    if (streaming && now >= next_report_) {
        video_.note_incomplete(reasm_.take_incomplete());
        int ok = 0, lost = 0;
        video_.take_report(ok, lost);
        char line[40];
        snprintf(line, sizeof(line), "RX\t%d\t%d", ok, lost);
        if (!tcp_.send_line(line)) {
            link_lost(now, "Connexion perdue");
            return;
        }
        next_report_ = now + 1000;
    } else if (!streaming) {
        next_report_ = now + 1000;
    }
    if (now - last_rx_ > (uint64_t)DEAD_LINK_MS) {
        link_lost(now, "Le relais ne répond plus");
        return;
    }
    if (tcp_.wants_write() && !tcp_.flush()) link_lost(now, "Connexion perdue");
}

// ============================================================================ messages du relais
void GfnClient::handle_video(const uint8_t* pkt, int len, uint64_t now) {
    if (!video_on_) return;               // la vue video n'est pas affichee : rien a decoder
    if (reasm_.feed(pkt, len)) video_.submit(reasm_.data(), reasm_.size(), reasm_.ts_ms(), now);
}

void GfnClient::send_input(uint64_t) {
    input::Raw raw;
    input::Mapping map;
    {
        plat::Lock l(in_mu_);
        raw = in_raw_;
        map = in_map_;
    }
    proto::InputPacket p = input::build_packet(raw, map);
    p.seq = ++input_seq_;
    uint8_t buf[proto::INPUT_PKT_SIZE];
    proto::pack_input(p, buf);
    input_sock_.send_to(buf, (int)sizeof(buf), server_ip_be_, input_port_);
}

void GfnClient::apply_profile(const char* id) {
    {
        plat::Lock l(mu_);
        copy_str(snap_.profile, sizeof(snap_.profile), id);
    }
    // Le profil "compat" garde l'IDCT precise ; les autres privilegient la vitesse de decodage.
    video_.set_fast_dct(strcmp(id, "compat") != 0);
}

void GfnClient::apply_state(char** f, uint64_t now) {
    proto::GfnState st = proto::parse_state(f[1]);
    int pos = atoi(f[2]);
    int eta = atoi(f[3]);
    bool changed;
    {
        plat::Lock l(mu_);
        changed = snap_.state != st;
        snap_.state = st;
        snap_.queue_pos = pos;
        snap_.eta_s = eta;
        copy_str(snap_.game, sizeof(snap_.game), f[4]);
        copy_str(snap_.state_text, sizeof(snap_.state_text), f[5]);
        if (changed) snap_.state_since_ms = now;
    }
}

void GfnClient::handle_line(char* line, uint64_t now) {
    char* f[8];
    int n = proto::split_fields(line, f, 8);
    const char* c = f[0];

    if (!strcmp(c, "PONG")) {
        if (n >= 2) {
            uint32_t sent = (uint32_t)strtoul(f[1], nullptr, 10);
            uint32_t rtt = (uint32_t)now - sent;
            if (rtt < 10000) {
                rtt_avg_ = rtt_avg_ < 0 ? (float)rtt : rtt_avg_ * 0.7f + (float)rtt * 0.3f;
                plat::Lock l(mu_);
                snap_.rtt_ms = (int)rtt;
                snap_.rtt_avg_ms = (int)(rtt_avg_ + 0.5f);
            }
        }
    } else if (!strcmp(c, "STATE")) {
        if (n >= 6) apply_state(f, now);
    } else if (!strcmp(c, "STATS")) {
        if (n >= 7) {
            plat::Lock l(mu_);
            snap_.relay_fps = atoi(f[1]);
            snap_.relay_kbps = atoi(f[2]);
            snap_.relay_quality = atoi(f[3]);
            copy_str(snap_.profile, sizeof(snap_.profile), f[4]);
            snap_.relay_signal = atoi(f[5]);
            snap_.pad_available = atoi(f[6]) != 0;
        }
    } else if (!strcmp(c, "GAMES")) {
        if (n >= 2) {
            games_building_.clear();
            games_expected_ = atoi(f[1]);
            building_games_ = true;
            if (games_expected_ <= 0) {
                plat::Lock l(mu_);
                games_.clear();
                snap_.games_version++;
                building_games_ = false;
            }
        }
    } else if (!strcmp(c, "G")) {
        if (building_games_ && n >= 3 && games_building_.size() < MAX_GAMES) {
            GameEntry g;
            copy_str(g.id, sizeof(g.id), f[1]);
            copy_str(g.name, sizeof(g.name), f[2]);
            games_building_.push_back(g);
            if ((int)games_building_.size() >= games_expected_) {
                plat::Lock l(mu_);
                games_.swap(games_building_);
                snap_.games_version++;
                building_games_ = false;
            }
        }
    } else if (!strcmp(c, "PROFILES")) {
        if (n >= 3) {
            profiles_building_.clear();
            profiles_expected_ = atoi(f[1]);
            building_profiles_ = true;
            apply_profile(f[2]);
            if (profiles_expected_ <= 0) {
                plat::Lock l(mu_);
                profiles_.clear();
                snap_.profiles_version++;
                building_profiles_ = false;
            }
        }
    } else if (!strcmp(c, "P")) {
        if (building_profiles_ && n >= 3 && profiles_building_.size() < 16) {
            ProfileEntry p;
            copy_str(p.id, sizeof(p.id), f[1]);
            copy_str(p.label, sizeof(p.label), f[2]);
            profiles_building_.push_back(p);
            if ((int)profiles_building_.size() >= profiles_expected_) {
                plat::Lock l(mu_);
                profiles_.swap(profiles_building_);
                snap_.profiles_version++;
                building_profiles_ = false;
            }
        }
    } else if (!strcmp(c, "OK")) {
        if (n >= 3 && (!strcmp(f[1], "LAUNCH") || !strcmp(f[1], "WATCH"))) {
            bool pad = atoi(f[2]) != 0;
            {
                plat::Lock l(mu_);
                snap_.pad_available = pad;
            }
            if (!pad) set_toast("Manette virtuelle absente sur le PC (ViGEmBus ?)");
        } else if (n >= 3 && !strcmp(f[1], "PROFILE")) {
            apply_profile(f[2]);
        }
    } else if (!strcmp(c, "ERR")) {
        if (n >= 3) {
            if (!strcmp(f[1], "launch")) set_want_stream(false);   // le jeu n'a pas pu etre lance : pas de flux a reprendre
            set_toast(f[2]);
        }
    }
}

// ============================================================================ decouverte
void GfnClient::poll_beacons(uint64_t now) {
    uint8_t buf[256];
    for (int i = 0; i < 8; i++) {
        uint32_t ip = 0;
        int len = beacon_sock_.recv(buf, sizeof(buf), &ip, nullptr);
        if (len <= 0) break;
        int version = 0, port = 0;
        char name[28];
        if (net::parse_beacon(buf, len, &version, name, sizeof(name), &port) && version == proto::PROTO_VERSION)
            update_relay(ip, port, name, now);
    }
}

void GfnClient::update_relay(uint32_t ip_be, int port, const char* name, uint64_t now) {
    plat::Lock l(mu_);
    for (size_t i = 0; i < relays_.size(); i++) {
        RelayEntry& r = relays_[i];
        if (r.ip_be == ip_be && r.port == port) {
            r.seen_ms = now;
            if (strcmp(r.name, name) != 0) {
                copy_str(r.name, sizeof(r.name), name);
                snap_.relays_version++;
            }
            return;
        }
    }
    if (relays_.size() >= 8) return;
    RelayEntry r;
    memset(&r, 0, sizeof(r));
    copy_str(r.name, sizeof(r.name), name);
    r.ip_be = ip_be;
    r.port = port;
    r.seen_ms = now;
    relays_.push_back(r);
    snap_.relays_version++;
}
