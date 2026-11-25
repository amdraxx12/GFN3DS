// Client du relais GeForce NOW : connexion, liste des jeux, lancement, file d'attente, streaming.
//
// GfnClient possede le thread reseau. Tout ce qui est public est utilisable depuis le thread
// principal sans jamais bloquer : les commandes sont mises en file, l'etat se lit par copie
// (snapshot()). Le thread reseau se charge de :
//   - la connexion TCP de controle (connexion non bloquante, reconnexion automatique avec delai croissant)
//   - la reception de la video UDP (reassemblage puis VideoDecoder::submit)
//   - l'envoi des entrees (60 Hz pendant le streaming) et des rapports de reception (1 Hz)
//   - la mesure de latence (PING / PONG chaque seconde) et la detection d'une liaison morte
//   - l'ecoute des balises de decouverte des relais du reseau local
#pragma once

#include <stdint.h>
#include <string.h>

#include <vector>

#include "input.hpp"
#include "network.hpp"
#include "platform.hpp"
#include "protocol.hpp"
#include "video_decoder.hpp"

enum class Link : uint8_t {
    Offline,        // pas de connexion (et pas de tentative en cours)
    Connecting,     // connexion TCP en cours
    Handshake,      // HELLO envoye, en attente de la reponse du relais
    Online,
    Reconnecting,   // liaison perdue : nouvelle tentative programmee
};

struct GameEntry {
    char id[48];
    char name[72];
};

struct ProfileEntry {
    char id[20];
    char label[32];
};

struct RelayEntry {
    char name[28];
    uint32_t ip_be;
    int port;
    uint64_t seen_ms;
};

// Photo de l'etat, copiee sous verrou : lisible a tout moment par l'interface.
struct Snapshot {
    // --- liaison
    Link link = Link::Offline;
    char relay_name[28] = "";
    char server[28] = "";             // "192.168.1.20:47970"
    char message[110] = "";           // dernier message de connexion (erreur, tentative en cours...)
    bool fatal = false;               // erreur sans reconnexion automatique (PIN refuse...)
    int attempt = 0;                  // numero de la tentative de reconnexion
    int retry_in_s = 0;               // secondes avant la prochaine tentative
    int rtt_ms = -1;                  // dernier aller-retour mesure (-1 : aucun)
    int rtt_avg_ms = -1;

    // --- GeForce NOW (vu par le relais)
    proto::GfnState state = proto::GfnState::Unknown;
    int queue_pos = -1;               // position dans la file d'attente (-1 : inconnue)
    int eta_s = -1;                   // attente estimee en secondes (-1 : inconnue)
    char game[48] = "";
    char state_text[80] = "";
    uint64_t state_since_ms = 0;      // heure locale du dernier changement d'etat

    // --- flux
    bool stream_on = false;           // cette console a demande le flux (LAUNCH / WATCH) : la video lui est envoyee
    bool pad_available = true;        // le relais dispose d'une manette virtuelle
    int relay_fps = 0;                // chiffres annonces par le relais (ligne STATS)
    int relay_kbps = 0;
    int relay_quality = 0;
    int relay_signal = 0;
    char profile[20] = "";            // profil de streaming actif

    // --- notification ponctuelle (reponse du relais a une commande)
    char toast[110] = "";
    uint32_t toast_seq = 0;

    // --- numeros de version des listes (changent quand la liste change)
    uint32_t games_version = 0;
    uint32_t profiles_version = 0;
    uint32_t relays_version = 0;
};

class GfnClient {
public:
    explicit GfnClient(VideoDecoder& video);
    ~GfnClient();

    bool start();      // ouvre les sockets et lance le thread reseau
    void stop();

    // --- commandes (non bloquantes ; executees par le thread reseau)
    void connect(const char* host, int port, const char* pin, bool auto_reconnect);
    void disconnect();                    // envoie BYE, pas de reconnexion automatique
    void refresh_games();                 // LIST
    void launch(const char* game_id);     // LAUNCH
    void watch();                         // WATCH : rejoindre la session en cours sans lancer de jeu
    void stop_game();                     // STOP
    void set_profile(const char* id);     // PROFILE

    // Derniere entree de la console ; le thread reseau en fait un paquet a chaque envoi.
    void set_input(const input::Raw& raw, const input::Mapping& map);

    // La vue video est affichee (true) ou non (false). Tant qu'elle ne l'est pas, les images recues sont
    // jetees sans etre decodees (une longue file d'attente ne doit pas user la batterie pour rien), et les
    // entrees / rapports de reception ne sont pas envoyes. Passe a true : le decodeur repart de zero.
    void set_video_wanted(bool on);

    // --- lecture
    Snapshot snapshot();
    void copy_games(std::vector<GameEntry>& out);
    void copy_profiles(std::vector<ProfileEntry>& out);
    void copy_relays(std::vector<RelayEntry>& out);
    int net_core() const { return thread_.core(); }

private:
    struct Cmd {
        enum Type { None, Connect, Disconnect, List, Launch, Watch, Stop, Profile };
        Type type = None;
        char arg[64] = "";
    };

    static void thread_main(void* self);
    void run();
    void process_commands(uint64_t now);
    void step_idle(uint64_t now, int wait_ms);
    void step_connecting(uint64_t now);
    void step_handshake(uint64_t now);
    void step_online(uint64_t now);

    void start_attempt(uint64_t now);
    void attempt_failed(uint64_t now, const char* msg, bool fatal);
    void link_lost(uint64_t now, const char* msg);
    void set_link(Link l);
    void set_message(const char* msg);
    void set_toast(const char* msg);
    void set_want_stream(bool v);
    void send_cmd_line(const char* fmt, const char* arg);
    void send_input(uint64_t now);
    void handle_line(char* line, uint64_t now);
    void handle_video(const uint8_t* pkt, int len, uint64_t now);
    void apply_state(char** f, uint64_t now);
    void apply_profile(const char* id);
    void poll_beacons(uint64_t now);
    void update_relay(uint32_t ip_be, int port, const char* name, uint64_t now);

    VideoDecoder& video_;
    plat::Thread thread_;
    volatile bool running_;

    // --- sockets (thread reseau uniquement)
    net::TcpLine tcp_;
    net::UdpSocket video_sock_;
    net::UdpSocket input_sock_;
    net::UdpSocket beacon_sock_;
    FrameReassembler reasm_;

    // --- etat prive du thread reseau
    Link link_;
    bool user_disconnect_;
    bool auto_reconnect_;
    bool want_stream_;     // LAUNCH / WATCH envoye : apres une coupure, on redemande le flux (WATCH) des la reconnexion
    bool video_on_;        // copie, cote thread reseau, de video_wanted_ (la vue video est affichee)
    int attempt_;
    uint64_t deadline_;
    uint64_t next_attempt_;
    uint64_t last_rx_;
    uint64_t next_ping_;
    uint64_t next_input_;
    uint64_t next_report_;
    uint64_t next_prune_;
    uint32_t server_ip_be_;
    int input_port_;
    uint16_t input_seq_;
    float rtt_avg_;
    std::vector<GameEntry> games_building_;
    std::vector<ProfileEntry> profiles_building_;
    int games_expected_;
    int profiles_expected_;
    bool building_games_;
    bool building_profiles_;

    // --- partage avec le thread principal (protege par mu_)
    plat::Mutex mu_;
    Snapshot snap_;
    std::vector<GameEntry> games_;
    std::vector<ProfileEntry> profiles_;
    std::vector<RelayEntry> relays_;
    std::vector<Cmd> cmds_;
    char target_host_[64];
    int target_port_;
    char target_pin_[16];
    bool target_auto_;
    bool video_wanted_;          // voir set_video_wanted()

    // Copie de travail du thread reseau (prise au moment de la commande Connect).
    char cur_host_[64];
    int cur_port_;
    char cur_pin_[16];

    plat::Mutex in_mu_;
    input::Raw in_raw_;
    input::Mapping in_map_;
};
