// Video : reassemblage des fragments UDP, tampon de gigue adaptatif et decodage JPEG.
//
// Chemin d'une image :
//   thread reseau   FrameReassembler  -> VideoDecoder::submit()      (copie ~10 Kio, jamais de decodage ici)
//   thread decodage libjpeg-turbo     -> tampon RGB565 240x400, deja a la disposition de l'ecran
//   thread principal VideoDecoder::acquire() -> renderer::blit_video()
//
// Le relais pivote chaque image de 90 degres avant de l'encoder : chaque ligne du JPEG est une
// colonne de l'ecran de la 3DS, et le resultat decode se copie tel quel dans le framebuffer.
#pragma once

#include <stdint.h>

#include "platform.hpp"
#include "protocol.hpp"

// ------------------------------------------------------------- reassemblage
// Une seule image en cours : une image plus recente abandonne l'ancienne (pas de retransmission,
// un JPEG partiel est inutilisable et l'image suivante arrive 33 ms plus tard). Meme politique que
// Reassembler dans relay/tests/fake3ds.py.
class FrameReassembler {
public:
    FrameReassembler();
    // Donne un datagramme. Vrai quand une image complete est disponible (data()/size()/ts_ms()).
    bool feed(const uint8_t* pkt, int len);
    const uint8_t* data() const { return buf_; }
    int size() const { return size_; }
    uint16_t frame_id() const { return cur_id_; }
    uint32_t ts_ms() const { return ts_; }
    // Images abandonnees (fragments manquants) depuis le dernier appel.
    int take_incomplete();
    void reset();

private:
    uint8_t buf_[proto::MAX_FRAGS * proto::FRAG_PAYLOAD];
    uint64_t got_mask_;
    uint16_t cur_id_;
    uint16_t cnt_;
    uint32_t ts_;
    int size_;
    int last_len_;
    bool have_cur_;
    bool done_;
    int incomplete_;
};

// ---------------------------------------------------------- tampon de gigue
// Mesure la gigue du reseau a partir des horodatages du relais (aucune synchronisation d'horloge
// necessaire : seules les differences comptent) et retarde chaque image juste assez pour que
// l'affichage reste regulier. Mode 1 (latence minimale) n'ajoute aucun delai.
class JitterBuffer {
public:
    JitterBuffer() : mode_(0) { reset(); }   // 0 = BUFFER_ADAPTIVE
    void reset();
    void set_mode(int mode) { mode_ = mode; }   // BUFFER_ADAPTIVE / BUFFER_LOW_LATENCY / BUFFER_SMOOTH
    // Retourne l'heure locale (ms) a partir de laquelle l'image peut etre affichee (>= arrival_ms).
    uint64_t on_frame(uint32_t ts_ms, uint64_t arrival_ms);
    int delay_ms() const { return (int)(target_ + 0.5f); }    // delai ajoute en ce moment
    int jitter_ms() const { return (int)(jitter_ + 0.5f); }   // gigue estimee

private:
    static const int BUCKETS = 8;
    int mode_;
    bool have_base_;
    uint32_t base_ts_;
    uint64_t base_arrival_;
    int64_t bucket_min_[BUCKETS];
    int bucket_idx_;
    uint64_t bucket_start_;
    int64_t last_qd_;
    float jitter_;
    float target_;
};

// --------------------------------------------------------------- decodage
struct VideoStats {
    int fps_decoded = 0;       // images decodees par seconde (derniere seconde complete)
    int fps_shown = 0;         // images affichees par seconde
    int kbps = 0;              // debit video recu (kilobits/s)
    float decode_ms = 0.0f;    // duree moyenne de decodage d'une image
    int delay_ms = 0;          // delai du tampon de gigue
    int jitter_ms = 0;         // gigue estimee
    uint32_t decoded_total = 0;
    uint32_t lost_total = 0;   // images perdues (fragments manquants, decodeur occupe, JPEG invalide)
    uint64_t last_frame_ms = 0;  // heure locale de la derniere image decodee (0 : aucune)
    int decode_core = -1;      // coeur du thread de decodage
};

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();
    bool start();
    void stop();
    void reset();                          // oublie images et statistiques (nouvelle session)
    void set_buffer_mode(int mode);
    void set_fast_dct(bool fast);          // IDCT rapide (defaut) ou precise

    // --- thread reseau
    void submit(const uint8_t* jpeg, int size, uint32_t ts_ms, uint64_t arrival_ms);
    void note_incomplete(int n);
    void take_report(int& ok, int& lost);  // pour la ligne RX envoyee chaque seconde

    // --- thread principal
    bool acquire(uint64_t now_ms);         // vrai si une nouvelle image est affichable
    const uint16_t* frame() const;         // derniere image acquise (nullptr : aucune), 400x240 RGB565
    VideoStats stats();

    static const int W = 240;              // largeur du JPEG pivote = hauteur de l'ecran
    static const int H = 400;              // hauteur du JPEG pivote = largeur de l'ecran
    static const size_t FRAME_PIXELS = (size_t)W * H;

private:
    static void thread_main(void* self);
    void run();
    bool decode(uint8_t* jpeg, int size, uint16_t* out);

    plat::Mutex mu_;
    plat::Event wake_;
    plat::Thread thread_;
    bool running_;

    uint16_t* buf_[3];                     // triple tampon : travail / pret / affiche
    int work_, ready_, show_;
    bool ready_valid_;
    bool have_shown_;
    uint64_t ready_due_;

    uint8_t* mail_;                        // dernier JPEG recu, en attente de decodage
    uint8_t* dec_;                         // JPEG en cours de decodage
    int mail_size_;
    uint32_t mail_ts_;
    uint64_t mail_due_;
    bool mail_pending_;
    uint32_t gen_;                         // change a chaque reset() : ecarte les images perimees
    uint32_t mail_gen_;

    JitterBuffer jb_;
    int fast_dct_;

    // statistiques et compteurs du rapport RX
    int ok_interval_;
    int lost_interval_;
    uint32_t decoded_total_;
    uint32_t lost_total_;
    float decode_ms_;
    uint64_t last_frame_ms_;
    uint64_t win_start_;
    int win_decoded_;
    int fps_decoded_;
    uint64_t show_win_start_;
    int win_shown_;
    int fps_shown_;
    uint64_t kb_win_start_;
    uint32_t kb_win_bytes_;
    int kbps_;
};
