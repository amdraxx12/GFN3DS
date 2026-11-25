// Video : voir video_decoder.hpp.
#include "video_decoder.hpp"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <jpeglib.h>
}

#include "settings.hpp"  // BUFFER_*

// ============================================================== FrameReassembler
FrameReassembler::FrameReassembler() { reset(); }

void FrameReassembler::reset() {
    got_mask_ = 0;
    cur_id_ = 0;
    cnt_ = 0;
    ts_ = 0;
    size_ = 0;
    last_len_ = 0;
    have_cur_ = false;
    done_ = false;
    incomplete_ = 0;
}

int FrameReassembler::take_incomplete() {
    int n = incomplete_;
    incomplete_ = 0;
    return n;
}

bool FrameReassembler::feed(const uint8_t* pkt, int len) {
    proto::VideoHeader h;
    if (!proto::parse_video_header(pkt, len, h)) return false;
    int payload = len - proto::VIDEO_HDR_SIZE;
    if (payload > proto::FRAG_PAYLOAD) return false;
    // Tous les fragments sauf le dernier font exactement FRAG_PAYLOAD octets.
    bool last = (h.frag_idx == h.frag_cnt - 1);
    if (!last && payload != proto::FRAG_PAYLOAD) return false;

    if (!have_cur_ || h.frame_id != cur_id_) {
        if (have_cur_ && !proto::seq_newer(h.frame_id, cur_id_)) return false;  // fragment d'une image plus ancienne
        if (have_cur_ && !done_) incomplete_++;     // l'image en cours n'a jamais ete completee
        have_cur_ = true;
        cur_id_ = h.frame_id;
        cnt_ = h.frag_cnt;
        ts_ = h.ts_ms;
        got_mask_ = 0;
        done_ = false;
        size_ = 0;
        last_len_ = 0;
    }
    if (done_ || h.frag_cnt != cnt_) return false;

    uint64_t bit = 1ULL << h.frag_idx;
    if (got_mask_ & bit) return false;              // doublon
    memcpy(buf_ + (size_t)h.frag_idx * proto::FRAG_PAYLOAD, pkt + proto::VIDEO_HDR_SIZE, (size_t)payload);
    got_mask_ |= bit;
    if (last) last_len_ = payload;

    uint64_t full = (cnt_ == 64) ? ~0ULL : ((1ULL << cnt_) - 1);
    if (got_mask_ != full) return false;
    done_ = true;
    size_ = (int)(cnt_ - 1) * proto::FRAG_PAYLOAD + last_len_;
    return true;
}

// ================================================================ JitterBuffer
void JitterBuffer::reset() {              // le mode choisi par l'utilisateur est conserve
    have_base_ = false;
    base_ts_ = 0;
    base_arrival_ = 0;
    for (int i = 0; i < BUCKETS; i++) bucket_min_[i] = INT64_MAX;
    bucket_idx_ = 0;
    bucket_start_ = 0;
    last_qd_ = 0;
    jitter_ = 0.0f;
    target_ = 0.0f;
}

uint64_t JitterBuffer::on_frame(uint32_t ts_ms, uint64_t arrival_ms) {
    if (!have_base_) {
        have_base_ = true;
        base_ts_ = ts_ms;
        base_arrival_ = arrival_ms;
        bucket_start_ = arrival_ms;
    }
    // Retard de cette image par rapport a la premiere : temps ecoule en local moins temps ecoule
    // chez le relais. Le plus petit retard recent correspond au "meilleur cas" du reseau.
    int64_t rel = (int64_t)(arrival_ms - base_arrival_) - (int64_t)(int32_t)(ts_ms - base_ts_);

    if (arrival_ms - bucket_start_ >= 1000ULL * BUCKETS) {   // longue pause : on repart de zero
        for (int i = 0; i < BUCKETS; i++) bucket_min_[i] = INT64_MAX;
        bucket_idx_ = 0;
        bucket_start_ = arrival_ms;
    }
    while (arrival_ms - bucket_start_ >= 1000) {    // une case par seconde, 8 secondes d'historique
        bucket_start_ += 1000;
        bucket_idx_ = (bucket_idx_ + 1) % BUCKETS;
        bucket_min_[bucket_idx_] = INT64_MAX;
    }
    if (rel < bucket_min_[bucket_idx_]) bucket_min_[bucket_idx_] = rel;
    int64_t best = INT64_MAX;
    for (int i = 0; i < BUCKETS; i++)
        if (bucket_min_[i] < best) best = bucket_min_[i];

    int64_t qd = rel - best;                         // retard supplementaire subi par cette image (>= 0)
    int64_t diff = qd > last_qd_ ? qd - last_qd_ : last_qd_ - qd;
    last_qd_ = qd;
    jitter_ += ((float)diff - jitter_) * (1.0f / 16.0f);

    float want = 0.0f;
    if (mode_ == BUFFER_ADAPTIVE) {
        want = 3.0f * jitter_ + 5.0f;
        if (want > 50.0f) want = 50.0f;
    } else if (mode_ == BUFFER_SMOOTH) {
        want = 4.0f * jitter_ + 25.0f;
        if (want > 120.0f) want = 120.0f;
    }
    // Monte tout de suite (eviter de manquer une image), redescend doucement.
    if (want > target_) target_ = want;
    else target_ += (want - target_) * 0.02f;

    int64_t wait = (int64_t)(target_ + 0.5f) - qd;   // une image deja en retard n'attend pas
    if (wait < 0) wait = 0;
    return arrival_ms + (uint64_t)wait;
}

// ================================================================== decodage JPEG
namespace {
struct ErrMgr {
    jpeg_error_mgr pub;
    jmp_buf jb;
};
void on_error(j_common_ptr c) { longjmp(((ErrMgr*)c->err)->jb, 1); }
void on_message(j_common_ptr) {}
void on_emit(j_common_ptr, int) {}
}  // namespace

bool VideoDecoder::decode(uint8_t* jpeg, int size, uint16_t* out) {
    jpeg_decompress_struct cinfo;
    ErrMgr err;
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = on_error;                   // par defaut libjpeg appelle exit() !
    err.pub.output_message = on_message;
    err.pub.emit_message = on_emit;
    jpeg_create_decompress(&cinfo);
    if (setjmp(err.jb)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_mem_src(&cinfo, jpeg, (unsigned long)size);
    jpeg_read_header(&cinfo, TRUE);
    if ((int)cinfo.image_width != W || (int)cinfo.image_height != H) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    cinfo.out_color_space = JCS_RGB565;              // 16 bits, comme le framebuffer
    cinfo.dct_method = fast_dct_ ? JDCT_IFAST : JDCT_ISLOW;
    cinfo.do_fancy_upsampling = FALSE;               // active le chemin "fusionne" (plus rapide)
    cinfo.do_block_smoothing = FALSE;
    cinfo.dither_mode = JDITHER_NONE;
    jpeg_start_decompress(&cinfo);
    if ((int)cinfo.output_width != W) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    JSAMPROW rows[16];
    while ((int)cinfo.output_scanline < H) {
        int first = (int)cinfo.output_scanline;
        int n = H - first < 16 ? H - first : 16;
        for (int i = 0; i < n; i++) rows[i] = (JSAMPROW)(out + (size_t)(first + i) * W);
        JDIMENSION got = jpeg_read_scanlines(&cinfo, rows, (JDIMENSION)n);
        if (got == 0) break;                         // donnees tronquees
    }
    bool complete = (int)cinfo.output_scanline >= H;
    if (complete) jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return complete;
}

// ============================================================== VideoDecoder
VideoDecoder::VideoDecoder()
    : running_(false), work_(0), ready_(1), show_(2), ready_valid_(false), have_shown_(false),
      ready_due_(0), mail_(nullptr), dec_(nullptr), mail_size_(0), mail_ts_(0), mail_due_(0),
      mail_pending_(false), gen_(1), mail_gen_(0), fast_dct_(1), ok_interval_(0), lost_interval_(0),
      decoded_total_(0), lost_total_(0), decode_ms_(0.0f), last_frame_ms_(0), win_start_(0),
      win_decoded_(0), fps_decoded_(0), show_win_start_(0), win_shown_(0), fps_shown_(0),
      kb_win_start_(0), kb_win_bytes_(0), kbps_(0) {
    buf_[0] = buf_[1] = buf_[2] = nullptr;
}

VideoDecoder::~VideoDecoder() { stop(); }

bool VideoDecoder::start() {
    if (running_) return true;
    for (int i = 0; i < 3; i++) {
        buf_[i] = (uint16_t*)calloc(FRAME_PIXELS, sizeof(uint16_t));
        if (!buf_[i]) return false;
    }
    mail_ = (uint8_t*)malloc(proto::MAX_FRAME_BYTES + 1024);
    dec_ = (uint8_t*)malloc(proto::MAX_FRAME_BYTES + 1024);
    if (!mail_ || !dec_) return false;
    running_ = true;
    // Le decodage est le travail le plus lourd : coeur 2 sur New 3DS, sinon coeur 0 (un peu moins
    // prioritaire que l'interface, qui dort la plupart du temps en attendant la synchro verticale).
    if (!thread_.start(&VideoDecoder::thread_main, this, 0x10000, plat::CoreHint::Compute, 1)) {
        running_ = false;
        return false;
    }
    return true;
}

void VideoDecoder::stop() {
    if (running_) {
        running_ = false;
        wake_.signal();
        thread_.join();
    }
    for (int i = 0; i < 3; i++) {
        free(buf_[i]);
        buf_[i] = nullptr;
    }
    free(mail_);
    free(dec_);
    mail_ = dec_ = nullptr;
}

void VideoDecoder::reset() {
    plat::Lock l(mu_);
    gen_++;
    mail_pending_ = false;
    ready_valid_ = false;
    have_shown_ = false;
    ok_interval_ = lost_interval_ = 0;
    decoded_total_ = lost_total_ = 0;
    decode_ms_ = 0.0f;
    last_frame_ms_ = 0;
    win_decoded_ = fps_decoded_ = win_shown_ = fps_shown_ = 0;
    win_start_ = show_win_start_ = kb_win_start_ = 0;
    kb_win_bytes_ = 0;
    kbps_ = 0;
    jb_.reset();
}

void VideoDecoder::set_buffer_mode(int mode) {
    plat::Lock l(mu_);
    jb_.set_mode(mode);
}

void VideoDecoder::set_fast_dct(bool fast) { fast_dct_ = fast ? 1 : 0; }

void VideoDecoder::thread_main(void* self) { ((VideoDecoder*)self)->run(); }

void VideoDecoder::submit(const uint8_t* jpeg, int size, uint32_t ts_ms, uint64_t arrival_ms) {
    if (size <= 0 || size > proto::MAX_FRAME_BYTES || !mail_) return;
    {
        plat::Lock l(mu_);
        if (mail_pending_) {                         // le decodeur n'a pas fini la precedente : on la jette
            lost_interval_++;
            lost_total_++;
        }
        memcpy(mail_, jpeg, (size_t)size);
        mail_size_ = size;
        mail_ts_ = ts_ms;
        mail_due_ = jb_.on_frame(ts_ms, arrival_ms);
        mail_gen_ = gen_;
        mail_pending_ = true;
        if (kb_win_start_ == 0) kb_win_start_ = arrival_ms;
        kb_win_bytes_ += (uint32_t)size;
        if (arrival_ms - kb_win_start_ >= 1000) {
            kbps_ = (int)((uint64_t)kb_win_bytes_ * 8 / (arrival_ms - kb_win_start_));   // octets*8/ms = kbit/s
            kb_win_bytes_ = 0;
            kb_win_start_ = arrival_ms;
        }
    }
    wake_.signal();
}

void VideoDecoder::note_incomplete(int n) {
    if (n <= 0) return;
    plat::Lock l(mu_);
    lost_interval_ += n;
    lost_total_ += (uint32_t)n;
}

void VideoDecoder::take_report(int& ok, int& lost) {
    plat::Lock l(mu_);
    ok = ok_interval_;
    lost = lost_interval_;
    ok_interval_ = lost_interval_ = 0;
}

void VideoDecoder::run() {
    while (running_) {
        if (!wake_.wait(100)) continue;
        if (!running_) break;
        uint32_t gen;
        uint64_t due;
        int size;
        {
            plat::Lock l(mu_);
            if (!mail_pending_) continue;
            uint8_t* t = mail_;                      // on echange les tampons : pas de copie
            mail_ = dec_;
            dec_ = t;
            size = mail_size_;
            due = mail_due_;
            gen = mail_gen_;
            mail_pending_ = false;
        }
        uint64_t t0 = plat::now_ms();
        bool ok = decode(dec_, size, buf_[work_]);
        uint64_t t1 = plat::now_ms();

        plat::Lock l(mu_);
        if (gen != gen_) continue;                   // session terminee entre-temps
        if (!ok) {
            lost_interval_++;
            lost_total_++;
            continue;
        }
        float ms = (float)(t1 - t0);
        decode_ms_ = decode_ms_ == 0.0f ? ms : decode_ms_ + (ms - decode_ms_) * 0.1f;
        int t = work_;                               // l'image prete passe au rang "pret"
        work_ = ready_;
        ready_ = t;
        ready_valid_ = true;
        ready_due_ = due;
        ok_interval_++;
        decoded_total_++;
        last_frame_ms_ = t1;
        win_decoded_++;
        if (win_start_ == 0) win_start_ = t1;
        if (t1 - win_start_ >= 1000) {
            fps_decoded_ = (int)((uint64_t)win_decoded_ * 1000 / (t1 - win_start_));
            win_decoded_ = 0;
            win_start_ = t1;
        }
    }
}

bool VideoDecoder::acquire(uint64_t now) {
    plat::Lock l(mu_);
    if (show_win_start_ == 0) show_win_start_ = now;
    if (now - show_win_start_ >= 1000) {
        fps_shown_ = (int)((uint64_t)win_shown_ * 1000 / (now - show_win_start_));
        win_shown_ = 0;
        show_win_start_ = now;
    }
    if (!ready_valid_ || now < ready_due_) return false;
    int t = show_;
    show_ = ready_;
    ready_ = t;
    ready_valid_ = false;
    have_shown_ = true;
    win_shown_++;
    return true;
}

const uint16_t* VideoDecoder::frame() const { return have_shown_ ? buf_[show_] : nullptr; }

VideoStats VideoDecoder::stats() {
    plat::Lock l(mu_);
    VideoStats s;
    s.fps_decoded = fps_decoded_;
    s.fps_shown = fps_shown_;
    s.kbps = kbps_;
    s.decode_ms = decode_ms_;
    s.delay_ms = jb_.delay_ms();
    s.jitter_ms = jb_.jitter_ms();
    s.decoded_total = decoded_total_;
    s.lost_total = lost_total_;
    s.last_frame_ms = last_frame_ms_;
    s.decode_core = thread_.core();
    return s;
}
