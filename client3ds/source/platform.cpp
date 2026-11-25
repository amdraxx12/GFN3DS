// Couche plate-forme : implementation libctru (3DS) et implementation de test (PC).
// Voir include/platform.hpp.
#include "platform.hpp"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef __3DS__
// =============================================================================== 3DS
#include <malloc.h>

namespace plat {

uint64_t now_ms() {
    // svcGetSystemTick() tourne a SYSCLOCK_ARM11 (268 MHz) meme quand la New 3DS est en mode 804 MHz.
    return (uint64_t)svcGetSystemTick() * 1000ULL / (uint64_t)SYSCLOCK_ARM11;
}

void sleep_ms(uint32_t ms) { svcSleepThread((s64)ms * 1000000LL); }

// ---------------------------------------------------------------- systeme
bool is_new3ds() {
    static int cached = -1;
    if (cached < 0) {
        bool b = false;
        cached = (R_SUCCEEDED(APT_CheckNew3DS(&b)) && b) ? 1 : 0;
    }
    return cached == 1;
}

void set_cpu_speedup(bool on) {
    if (is_new3ds()) osSetSpeedupEnable(on);
}

void allow_syscore(int percent) { APT_SetAppCpuTimeLimit((u32)percent); }

bool app_running() { return aptMainLoop(); }
void app_request_quit() {}

// ---------------------------------------------------------------- threads
Thread::Thread() : impl_(nullptr), core_(-1) {}
Thread::~Thread() { join(); }

bool Thread::start(Fn fn, void* arg, size_t stack_bytes, CoreHint hint, int prio_delta) {
    if (impl_) return false;
    s32 main_prio = 0x30;
    svcGetThreadPriority(&main_prio, CUR_THREAD_HANDLE);
    int prio = (int)main_prio + prio_delta;
    if (prio < 0x18) prio = 0x18;
    if (prio > 0x3F) prio = 0x3F;

    int cores[3];
    int n = 0;
    if (hint == CoreHint::Syscore) cores[n++] = 1;
    if (hint == CoreHint::Compute && is_new3ds()) cores[n++] = 2;
    cores[n++] = 0;
    for (int i = 0; i < n; i++) {
        ::Thread t = threadCreate(fn, arg, stack_bytes, prio, cores[i], false);
        if (t) {
            impl_ = (void*)t;
            core_ = cores[i];
            return true;
        }
    }
    return false;
}

void Thread::join() {
    if (!impl_) return;
    ::Thread t = (::Thread)impl_;
    threadJoin(t, U64_MAX);
    threadFree(t);
    impl_ = nullptr;
}

// ----------------------------------------------------------------- reseau
static u32* g_soc_buffer = nullptr;

bool net_init() {
    if (g_soc_buffer) return true;
    const u32 size = 0x100000;  // 1 Mio : tampon du service de sockets (reception UDP comprise)
    g_soc_buffer = (u32*)memalign(0x1000, size);
    if (!g_soc_buffer) return false;
    if (R_FAILED(socInit(g_soc_buffer, size))) {
        free(g_soc_buffer);
        g_soc_buffer = nullptr;
        return false;
    }
    return true;
}

void net_exit() {
    if (!g_soc_buffer) return;
    socExit();
    free(g_soc_buffer);
    g_soc_buffer = nullptr;
}

int wifi_strength() { return (int)osGetWifiStrength(); }

static bool ptmu_ready() {
    static int state = 0;  // 0 : pas encore essaye, 1 : ok, -1 : indisponible
    if (state == 0) state = R_SUCCEEDED(ptmuInit()) ? 1 : -1;
    return state == 1;
}

int battery_level() {
    u8 v = 0;
    if (!ptmu_ready() || R_FAILED(PTMU_GetBatteryLevel(&v))) return -1;
    return (int)v;
}

bool battery_charging() {
    u8 v = 0;
    if (!ptmu_ready() || R_FAILED(PTMU_GetBatteryChargeState(&v))) return false;
    return v != 0;
}

bool net_would_block(int err) { return err == EAGAIN || err == EWOULDBLOCK || err == EINPROGRESS; }
int net_errno() { return errno; }

// -------------------------------------------------------------- affichage
bool display_init() {
    gfxInit(GSP_RGB565_OES, GSP_RGB565_OES, false);
    gfxSet3D(false);
    return true;
}

void display_exit() { gfxExit(); }

Framebuffer display_top() {
    u16 w = 0, h = 0;
    u8* p = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
    Framebuffer f = {(uint16_t*)p, 400, 240};
    return f;
}

Framebuffer display_bottom() {
    u16 w = 0, h = 0;
    u8* p = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
    Framebuffer f = {(uint16_t*)p, 320, 240};
    return f;
}

void display_present(bool top, bool bottom) {
    if (!top && !bottom) return;
    gfxFlushBuffers();
    if (top) gfxScreenSwapBuffers(GFX_TOP, false);
    if (bottom) gfxScreenSwapBuffers(GFX_BOTTOM, false);
}

void display_vsync() { gspWaitForVBlank(); }

bool text_input(const char* hint, const char* initial, char* out, int out_size, bool numeric) {
    if (out_size < 2) return false;
    SwkbdState kb;
    swkbdInit(&kb, numeric ? SWKBD_TYPE_NUMPAD : SWKBD_TYPE_QWERTY, 2, out_size - 1);
    swkbdSetValidation(&kb, SWKBD_ANYTHING, 0, 0);
    if (numeric) swkbdSetNumpadKeys(&kb, '.', 0);
    if (hint) swkbdSetHintText(&kb, hint);
    if (initial && initial[0]) swkbdSetInitialText(&kb, initial);
    out[0] = '\0';
    SwkbdButton b = swkbdInputText(&kb, out, (size_t)out_size);
    return b == SWKBD_BUTTON_CONFIRM;
}

}  // namespace plat

#else
// =============================================================================== PC
#include <atomic>
#include <chrono>

namespace plat {

static std::atomic<bool> g_quit(false);
static std::atomic<bool> g_new3ds(false);
static std::atomic<uint64_t> g_presents(0);
static std::mutex g_answer_lock;
static char g_answer[128] = "";
static bool g_answer_set = false;

uint64_t now_ms() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void sleep_ms(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

bool is_new3ds() { return g_new3ds.load(); }
void set_cpu_speedup(bool) {}
void allow_syscore(int) {}
bool app_running() { return !g_quit.load(); }
void app_request_quit() { g_quit.store(true); }

struct HostThread {
    std::thread th;
};

Thread::Thread() : impl_(nullptr), core_(-1) {}
Thread::~Thread() { join(); }

bool Thread::start(Fn fn, void* arg, size_t, CoreHint, int) {
    if (impl_) return false;
    HostThread* t = new HostThread();
    t->th = std::thread(fn, arg);
    impl_ = t;
    core_ = 0;
    return true;
}

void Thread::join() {
    if (!impl_) return;
    HostThread* t = (HostThread*)impl_;
    if (t->th.joinable()) t->th.join();
    delete t;
    impl_ = nullptr;
}

bool net_init() { return true; }
void net_exit() {}
int wifi_strength() { return 3; }
int battery_level() { return 4; }
bool battery_charging() { return false; }
bool net_would_block(int err) { return err == EAGAIN || err == EWOULDBLOCK || err == EINPROGRESS; }
int net_errno() { return errno; }

static uint16_t g_top[400 * 240];
static uint16_t g_bottom[320 * 240];

bool display_init() {
    memset(g_top, 0, sizeof(g_top));
    memset(g_bottom, 0, sizeof(g_bottom));
    return true;
}
void display_exit() {}
Framebuffer display_top() {
    Framebuffer f = {g_top, 400, 240};
    return f;
}
Framebuffer display_bottom() {
    Framebuffer f = {g_bottom, 320, 240};
    return f;
}
void display_present(bool top, bool bottom) {
    if (top || bottom) g_presents.fetch_add(1);
}
void display_vsync() { sleep_ms(16); }

bool text_input(const char*, const char*, char* out, int out_size, bool) {
    std::lock_guard<std::mutex> g(g_answer_lock);
    if (!g_answer_set || out_size < 1) return false;
    strncpy(out, g_answer, (size_t)out_size - 1);
    out[out_size - 1] = '\0';
    g_answer_set = false;
    return true;
}

void host_set_text_input(const char* answer) {
    std::lock_guard<std::mutex> g(g_answer_lock);
    if (answer) {
        strncpy(g_answer, answer, sizeof(g_answer) - 1);
        g_answer[sizeof(g_answer) - 1] = '\0';
        g_answer_set = true;
    } else {
        g_answer_set = false;
    }
}
void host_set_new3ds(bool v) { g_new3ds.store(v); }
uint64_t host_present_count() { return g_presents.load(); }

}  // namespace plat
#endif
