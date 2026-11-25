// Reseau bas niveau : voir network.hpp.
#include "network.hpp"

#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#ifndef __3DS__
#include <unistd.h>
#endif

#include "platform.hpp"
#include "protocol.hpp"

namespace net {

// "Reessayer plus tard" : rien a lire / buffer plein / connexion en cours.
static bool again(int e) { return plat::net_would_block(e) || e == EINTR; }

bool parse_ipv4(const char* text, uint32_t* ip_be) {
    if (!text || !*text) return false;
    struct in_addr a;
    if (inet_pton(AF_INET, text, &a) != 1) return false;
    if (ip_be) *ip_be = (uint32_t)a.s_addr;
    return true;
}

bool resolve_ipv4(const char* host, uint32_t* ip_be) {
    if (parse_ipv4(host, ip_be)) return true;
    if (!host || !*host) return false;
    struct hostent* he = gethostbyname(host);   // non reentrant : appele uniquement par le thread reseau
    if (!he || he->h_addrtype != AF_INET || he->h_length != 4 || !he->h_addr_list || !he->h_addr_list[0])
        return false;
    uint32_t v;
    memcpy(&v, he->h_addr_list[0], 4);
    if (ip_be) *ip_be = v;
    return true;
}

void ip_to_str(uint32_t ip_be, char* out, int cap) {
    snprintf(out, (size_t)cap, "%u.%u.%u.%u", (unsigned)(ip_be & 0xFF), (unsigned)((ip_be >> 8) & 0xFF),
             (unsigned)((ip_be >> 16) & 0xFF), (unsigned)((ip_be >> 24) & 0xFF));
}

void close_socket(int fd) {
    if (fd < 0) return;
#ifdef __3DS__
    closesocket(fd);
#else
    ::close(fd);
#endif
}

bool set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return false;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

static void fill_addr(struct sockaddr_in* a, uint32_t ip_be, int port) {
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons((uint16_t)port);
    a->sin_addr.s_addr = ip_be;
}

// ========================================================================= UDP
void UdpSocket::close() {
    close_socket(fd_);
    fd_ = -1;
    port_ = 0;
}

bool UdpSocket::open_sender() {
    close();
    fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ < 0) {
        fd_ = -1;
        return false;
    }
    set_nonblocking(fd_);
    return true;
}

bool UdpSocket::open(int port, int rcvbuf, bool reuse) {
    close();
    fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ < 0) {
        fd_ = -1;
        return false;
    }
    set_nonblocking(fd_);
    int one = 1;
    if (reuse) {
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
#ifdef SO_REUSEPORT
        setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, (const char*)&one, sizeof(one));
#endif
    }
    if (rcvbuf > 0) setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));   // best effort

    struct sockaddr_in a;
    fill_addr(&a, htonl(INADDR_ANY), port);
    if (bind(fd_, (struct sockaddr*)&a, sizeof(a)) != 0) {
        fill_addr(&a, htonl(INADDR_ANY), 0);             // port occupe : on prend un port libre
        if (port == 0 || bind(fd_, (struct sockaddr*)&a, sizeof(a)) != 0) {
            close();
            return false;
        }
    }
    struct sockaddr_in b;
    socklen_t bl = sizeof(b);
    memset(&b, 0, sizeof(b));
    port_ = (getsockname(fd_, (struct sockaddr*)&b, &bl) == 0) ? (int)ntohs(b.sin_port) : port;
    return true;
}

int UdpSocket::recv(uint8_t* buf, int cap, uint32_t* from_ip_be, int* from_port) {
    if (fd_ < 0) return -1;
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    memset(&from, 0, sizeof(from));
    int n = (int)recvfrom(fd_, buf, (size_t)cap, 0, (struct sockaddr*)&from, &fl);
    if (n < 0) return again(plat::net_errno()) ? 0 : -1;
    if (from_ip_be) *from_ip_be = (uint32_t)from.sin_addr.s_addr;
    if (from_port) *from_port = (int)ntohs(from.sin_port);
    return n;
}

int UdpSocket::send_to(const uint8_t* data, int len, uint32_t ip_be, int port) {
    if (fd_ < 0) return -1;
    struct sockaddr_in a;
    fill_addr(&a, ip_be, port);
    int n = (int)sendto(fd_, data, (size_t)len, 0, (struct sockaddr*)&a, sizeof(a));
    if (n < 0) return again(plat::net_errno()) ? 0 : -1;
    return n;
}

// ========================================================================= TCP
TcpLine::TcpLine() : fd_(-1), connecting_(false), connected_(false), in_len_(0), out_len_(0) {}

void TcpLine::close() {
    if (fd_ >= 0) {
        shutdown(fd_, 2);
        close_socket(fd_);
    }
    fd_ = -1;
    connecting_ = false;
    connected_ = false;
    in_len_ = 0;
    out_len_ = 0;
}

bool TcpLine::begin_connect(uint32_t ip_be, int port) {
    close();
    fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd_ < 0) {
        fd_ = -1;
        return false;
    }
    set_nonblocking(fd_);
    int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));   // best effort : commandes courtes

    struct sockaddr_in a;
    fill_addr(&a, ip_be, port);
    int r = connect(fd_, (struct sockaddr*)&a, sizeof(a));
    if (r == 0) {
        connected_ = true;
        return true;
    }
    if (again(plat::net_errno())) {
        connecting_ = true;
        return true;
    }
    close();
    return false;
}

TcpLine::Status TcpLine::poll_connect(int timeout_ms) {
    if (connected_) return Connected;
    if (!connecting_ || fd_ < 0) return Failed;
    struct pollfd p;
    p.fd = fd_;
    p.events = POLLOUT;
    p.revents = 0;
    int r = poll(&p, 1, timeout_ms);
    if (r == 0) return Pending;
    if (r < 0) return again(plat::net_errno()) ? Pending : Failed;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return Failed;
    int err = 0;
    socklen_t el = sizeof(err);
    if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, (char*)&err, &el) == 0 && err != 0) return Failed;
    connecting_ = false;
    connected_ = true;
    return Connected;
}

bool TcpLine::flush() {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;       // PC : pas de SIGPIPE si le relais a ferme la connexion
#endif
    while (out_len_ > 0 && fd_ >= 0) {
        int n = (int)send(fd_, out_, (size_t)out_len_, flags);
        if (n < 0) return again(plat::net_errno());
        if (n == 0) return true;
        memmove(out_, out_ + n, (size_t)(out_len_ - n));
        out_len_ -= n;
    }
    return fd_ >= 0;
}

bool TcpLine::send_line(const char* line) {
    size_t n = strlen(line);
    if (n + 1 > sizeof(out_) - (size_t)out_len_) return false;   // file pleine : le relais ne lit plus
    memcpy(out_ + out_len_, line, n);
    out_[out_len_ + (int)n] = '\n';
    out_len_ += (int)n + 1;
    return flush();
}

int TcpLine::read_line(char* out, int cap) {
    for (;;) {
        char* nl = (char*)memchr(in_, '\n', (size_t)in_len_);
        if (nl) {
            int len = (int)(nl - in_);
            int copy = len < cap - 1 ? len : cap - 1;
            memcpy(out, in_, (size_t)copy);
            out[copy] = '\0';
            if (copy > 0 && out[copy - 1] == '\r') out[copy - 1] = '\0';
            int rest = in_len_ - (len + 1);
            memmove(in_, nl + 1, (size_t)rest);
            in_len_ = rest;
            return 1;
        }
        if (in_len_ >= (int)sizeof(in_)) in_len_ = 0;            // ligne anormalement longue : on la jette
        if (fd_ < 0) return -1;
        int n = (int)recv(fd_, in_ + in_len_, sizeof(in_) - (size_t)in_len_, 0);
        if (n > 0) {
            in_len_ += n;
            continue;
        }
        if (n == 0) return -1;                                   // le relais a ferme la connexion
        return again(plat::net_errno()) ? 0 : -1;
    }
}

uint32_t TcpLine::local_ip_be() const {
    if (fd_ < 0) return 0;
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    memset(&a, 0, sizeof(a));
    if (getsockname(fd_, (struct sockaddr*)&a, &al) != 0) return 0;
    return (uint32_t)a.sin_addr.s_addr;
}

// ===================================================================== balises
bool parse_beacon(const uint8_t* data, int len, int* version, char* name, int name_cap, int* port) {
    if (len < 8 || len > 200 || name_cap < 2) return false;
    char line[256];
    memcpy(line, data, (size_t)len);
    line[len] = '\0';
    char* f[6];
    int n = proto::split_fields(line, f, 6);
    if (n < 4 || strcmp(f[0], "GFN3DS") != 0) return false;
    int p = atoi(f[3]);
    if (p < 1 || p > 65535) return false;
    int j = 0;
    for (const char* c = f[2]; *c && j < name_cap - 1; c++)
        if ((unsigned char)*c >= 32 && (unsigned char)*c < 127) name[j++] = *c;
    name[j] = '\0';
    if (version) *version = atoi(f[1]);
    if (port) *port = p;
    return true;
}

}  // namespace net
