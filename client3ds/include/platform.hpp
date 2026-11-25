// Couche plate-forme : tout ce qui touche directement a la 3DS (libctru) passe par ici.
//
// Le reste du client (reseau, decodage, interface, rendu logiciel) est du C++ portable qui ne
// connait que ces fonctions. Sur 3DS (-D__3DS__) elles s'appuient sur libctru ; sur PC elles sont
// remplacees par de simples equivalents, ce qui permet de tester le client contre le vrai relais
// sans console (voir tests/host/).
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __3DS__
#include <3ds.h>
#else
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

namespace plat {

// ------------------------------------------------------------------ temps
uint64_t now_ms();           // horloge monotone en millisecondes (origine arbitraire)
void sleep_ms(uint32_t ms);

// ---------------------------------------------------------- synchronisation
#ifdef __3DS__
class Mutex {
public:
    Mutex() { LightLock_Init(&l_); }
    void lock() { LightLock_Lock(&l_); }
    void unlock() { LightLock_Unlock(&l_); }

private:
    Mutex(const Mutex&);
    Mutex& operator=(const Mutex&);
    LightLock l_;
};

// Evenement a remise a zero automatique : signal() reveille un seul wait().
class Event {
public:
    Event() { LightEvent_Init(&e_, RESET_ONESHOT); }
    void signal() { LightEvent_Signal(&e_); }
    // Attend au plus timeout_ms (< 0 : sans limite). Vrai si l'evenement a ete signale.
    bool wait(int timeout_ms) {
        if (timeout_ms < 0) {
            LightEvent_Wait(&e_);
            return true;
        }
        return LightEvent_WaitTimeout(&e_, (s64)timeout_ms * 1000000LL) == 0;
    }

private:
    Event(const Event&);
    Event& operator=(const Event&);
    LightEvent e_;
};
#else
class Mutex {
public:
    Mutex() {}
    void lock() { m_.lock(); }
    void unlock() { m_.unlock(); }

private:
    Mutex(const Mutex&);
    Mutex& operator=(const Mutex&);
    std::mutex m_;
};

class Event {
public:
    Event() : set_(false) {}
    void signal() {
        {
            std::lock_guard<std::mutex> g(m_);
            set_ = true;
        }
        cv_.notify_one();
    }
    bool wait(int timeout_ms) {
        std::unique_lock<std::mutex> g(m_);
        if (timeout_ms < 0) {
            cv_.wait(g, [this] { return set_; });
        } else if (!cv_.wait_for(g, std::chrono::milliseconds(timeout_ms), [this] { return set_; })) {
            return false;
        }
        set_ = false;
        return true;
    }

private:
    Event(const Event&);
    Event& operator=(const Event&);
    std::mutex m_;
    std::condition_variable cv_;
    bool set_;
};
#endif

class Lock {
public:
    explicit Lock(Mutex& m) : m_(m) { m_.lock(); }
    ~Lock() { m_.unlock(); }

private:
    Lock(const Lock&);
    Lock& operator=(const Lock&);
    Mutex& m_;
};

// ----------------------------------------------------------------- threads
// Ou placer un thread. Sur 3DS le coeur 0 est celui de l'appli (thread principal) ; le coeur 1
// ("syscore") n'est accessible qu'a une fraction du temps (allow_syscore) ; le coeur 2 n'existe que
// sur New 3DS. Si le coeur voulu est refuse, on retombe sur le suivant (jusqu'au coeur 0).
enum class CoreHint {
    Main,     // coeur 0, comme le thread principal
    Syscore,  // coeur 1 (travail leger : reseau), sinon coeur 0
    Compute,  // New 3DS : coeur 2 ; Old 3DS : coeur 0 (le coeur 1 est trop limite pour decoder)
};

class Thread {
public:
    typedef void (*Fn)(void*);
    Thread();
    ~Thread();
    // prio_delta : negatif = plus prioritaire que le thread principal.
    bool start(Fn fn, void* arg, size_t stack_bytes, CoreHint hint, int prio_delta);
    void join();
    bool joinable() const { return impl_ != nullptr; }
    int core() const { return core_; }  // coeur obtenu (-1 : inconnu)

private:
    Thread(const Thread&);
    Thread& operator=(const Thread&);
    void* impl_;
    int core_;
};

// ------------------------------------------------------------------ systeme
bool is_new3ds();
void set_cpu_speedup(bool on);       // New 3DS : 804 MHz + cache L2 (sans effet sur Old 3DS)
void allow_syscore(int percent);     // part de temps CPU laissee a l'appli sur le coeur 1 (5..89)
bool app_running();                  // aptMainLoop() ; sur PC : vrai tant que personne ne quitte
void app_request_quit();

// ------------------------------------------------------------------- reseau
bool net_init();                     // initialise le service de sockets (idempotent)
void net_exit();
int wifi_strength();                 // 0 (aucun) .. 3
int battery_level();                 // 0..5, -1 si inconnu
bool battery_charging();

// Messages d'erreur des sockets : errno ne se compare pas de la meme facon partout.
bool net_would_block(int err);       // EAGAIN / EWOULDBLOCK / EINPROGRESS
int net_errno();

// --------------------------------------------------------------- affichage
// Les deux ecrans de la 3DS sont des tableaux RGB565 ranges par colonnes, de bas en haut :
// le pixel (x, y) est a l'indice x * h + (h - 1 - y). Voir renderer.hpp.
struct Framebuffer {
    uint16_t* px;
    int w;
    int h;
};

bool display_init();
void display_exit();
Framebuffer display_top();           // tampon arriere de l'ecran du haut (400x240)
Framebuffer display_bottom();        // tampon arriere de l'ecran du bas (320x240)
void display_present(bool top, bool bottom);   // purge le cache et echange les tampons
void display_vsync();                // attend la synchronisation verticale (60 Hz)

// Clavier du systeme (swkbd). Retourne faux si l'utilisateur annule.
bool text_input(const char* hint, const char* initial, char* out, int out_size, bool numeric);

#ifndef __3DS__
// Aides reservees aux tests sur PC.
void host_set_text_input(const char* answer);   // reponse imposee au prochain text_input()
void host_set_new3ds(bool v);
uint64_t host_present_count();
#endif

}  // namespace plat
