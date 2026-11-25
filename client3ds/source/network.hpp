// Reseau bas niveau : sockets non bloquants (UDP video / entrees / balises, TCP de controle).
//
// Rien ici ne bloque : tous les sockets sont en mode non bloquant et le thread reseau les surveille
// avec poll() par tranches de quelques millisecondes (comme ftpd sur 3DS). Le thread principal
// (interface, rendu) ne touche jamais a un socket.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace net {

// Adresses IPv4 en ordre reseau (big-endian), comme in_addr.s_addr.
bool parse_ipv4(const char* text, uint32_t* ip_be);            // "192.168.1.20" uniquement
bool resolve_ipv4(const char* host, uint32_t* ip_be);          // IP ou nom d'hote (DNS : bloquant)
void ip_to_str(uint32_t ip_be, char* out, int cap);

void close_socket(int fd);
bool set_nonblocking(int fd);

// ------------------------------------------------------------------- UDP
class UdpSocket {
public:
    UdpSocket() : fd_(-1), port_(0) {}
    ~UdpSocket() { close(); }
    // Ouvre le socket et le lie a INADDR_ANY:port (0 : port libre). Si le port demande est occupe,
    // un port libre est pris : local_port() donne la valeur obtenue. rcvbuf <= 0 : valeur par defaut.
    // reuse : autorise plusieurs sockets sur le meme port (balises de decouverte).
    bool open(int port, int rcvbuf, bool reuse);
    // Socket d'emission seul (pas de bind).
    bool open_sender();
    void close();
    int fd() const { return fd_; }
    int local_port() const { return port_; }
    // > 0 : octets recus ; 0 : rien a lire ; -1 : erreur.
    int recv(uint8_t* buf, int cap, uint32_t* from_ip_be, int* from_port);
    int send_to(const uint8_t* data, int len, uint32_t ip_be, int port);

private:
    UdpSocket(const UdpSocket&);
    UdpSocket& operator=(const UdpSocket&);
    int fd_;
    int port_;
};

// ------------------------------------------------------------------- TCP
// Connexion de controle : lignes de texte terminees par '\n'.
class TcpLine {
public:
    enum Status { Pending, Connected, Failed };

    TcpLine();
    ~TcpLine() { close(); }
    bool begin_connect(uint32_t ip_be, int port);   // lance une connexion non bloquante
    Status poll_connect(int timeout_ms);            // attend au plus timeout_ms
    void close();
    bool is_open() const { return fd_ >= 0; }
    bool is_connected() const { return connected_; }
    int fd() const { return fd_; }
    bool wants_write() const { return out_len_ > 0; }

    // Ajoute une ligne (sans '\n') a la file d'envoi puis essaie de l'envoyer. Faux si la file est pleine.
    bool send_line(const char* line);
    bool flush();                                   // faux en cas d'erreur de connexion
    // 1 : une ligne complete est dans out (sans '\n') ; 0 : pas encore ; -1 : connexion fermee ou en erreur.
    int read_line(char* out, int cap);

    uint32_t local_ip_be() const;

private:
    TcpLine(const TcpLine&);
    TcpLine& operator=(const TcpLine&);
    int fd_;
    bool connecting_;
    bool connected_;
    char in_[1024];
    int in_len_;
    char out_[2048];
    int out_len_;
};

// Balise du relais : "GFN3DS<TAB>version<TAB>nom<TAB>port". Retourne vrai si le datagramme en est une.
bool parse_beacon(const uint8_t* data, int len, int* version, char* name, int name_cap, int* port);

}  // namespace net
