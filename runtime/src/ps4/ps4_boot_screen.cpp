// PS4 boot screen: see ps4_boot_screen.h. Two linear 1920x1080 buffers in write-combined direct memory, registered as
// VideoOut buffers 12 and 13 (the swapchain uses 0-3 and 8-11), drawn by the CPU (ps4_boot_draw.cpp) and flipped at
// 30 fps on a thread of their own until the game presents.
//
// Progress: the game data read so far against what the last boot had read by its first frame, or the time against
// the last boot's time, whichever is further (/data/SMW/boot-profile.txt, written at each first frame; the first boot
// guesses). The bar eases towards that and never goes back.
#include "ps4_boot_screen.h"
#include "ps4_app.h"

#include <sys/types.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>

#include <orbis/SystemService.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

#include <nx/runtime.h>

#include "ps4_boot_draw.h"
#include "ps4_menu.h"

extern "C" {
int32_t ps4bs_allocate_direct(off_t start, off_t end, size_t len, size_t align, int32_t type, off_t* phys) __asm__(
    "sceKernelAllocateDirectMemory");
int32_t ps4bs_map_direct(void** addr, size_t len, int32_t prot, int32_t flags, off_t phys, size_t align) __asm__(
    "sceKernelMapDirectMemory");
size_t ps4bs_direct_memory_size() __asm__("sceKernelGetDirectMemorySize");
void ps4_boot_trace(const char* line);
}

namespace nx::plat::ps4 {

namespace {

constexpr int kFirstBuffer = 12;
constexpr size_t kBufferSize = 8ull << 20;  // 1920x1080x4 rounded up to 2 MiB pages
constexpr int32_t kWcGarlic = 3;            // SCE_KERNEL_WC_GARLIC: write-combined, what the display reads
constexpr int32_t kProtCpuGpu = 0x33;       // CPU read/write, GPU read/write
const char* const kProfile = NX_PS4_DATA "/boot-profile.txt";

std::mutex g_video_lock;
int g_video = -1;
OrbisKernelEqueue g_flip_queue;

std::atomic<bool> g_stop{false}, g_running{false}, g_graphics{false};
std::atomic<uint64_t> g_bytes{0};
std::thread g_thread;
uint8_t* g_buffers[2] = {};  // kept after the game starts: the quick menu draws on them (ps4_menu.cpp)
bool g_registered = false;
std::once_flag g_stopped;
std::chrono::steady_clock::time_point g_start;

void trace(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void trace(const char* fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    ps4_boot_trace(line);
}

void draw_loop(uint8_t* buffers[2]) {
    // what the last boot needed by its first frame; the first boot's guess
    double want_bytes = 400.0 * (1 << 20), want_secs = 60.0;
    if (FILE* f = std::fopen(kProfile, "r")) {
        unsigned long long b = 0;
        double s = 0;
        if (std::fscanf(f, "%llu %lf", &b, &s) == 2 && b > (16u << 20) && s > 3) want_bytes = double(b), want_secs = s;
        std::fclose(f);
    }
    BootView view;
    view.credit = "by ShiroKlein";
    double shown = 0;
    int k = 0;
    for (uint64_t frame = 0; !g_stop; frame++) {
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
        uint64_t bytes = g_bytes.load();
        double target = std::max(double(bytes) / want_bytes, secs / want_secs);
        if (g_graphics) target = std::max(target, 0.9);
        target = std::min(target, 0.99);
        shown = std::max(shown, shown + (target - shown) * 0.15);
        char stage[96];
        if (g_graphics)
            std::snprintf(stage, sizeof stage, "Preparing graphics");
        else if (bytes)
            std::snprintf(stage, sizeof stage, "Loading game files: %llu MB", (unsigned long long)(bytes >> 20));
        else
            std::snprintf(stage, sizeof stage, "Starting the game");
        view.stage = stage;
        view.percent = int(shown * 100);
        // each buffer gets the whole picture once, then only the band that changes
        boot_draw(buffers[k], kBootWidth, view, frame < 2);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);  // write-combined stores out before the flip
        sceVideoOutSubmitFlip(g_video, kFirstBuffer + k, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0);
        if (frame == 0) sceSystemServiceHideSplashScreen();
        k ^= 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(33));  // the other buffer is off screen by then
    }
}

}  // namespace

int video_out() {
    std::lock_guard lk(g_video_lock);
    if (g_video >= 0) return g_video;
    g_video = sceVideoOutOpen(0xFF, 0, 0, nullptr);
    if (g_video < 0) {
        log("ps4: sceVideoOutOpen failed: %x", unsigned(g_video));
        return g_video;
    }
    sceKernelCreateEqueue(&g_flip_queue, "smw flip");
    sceVideoOutAddFlipEvent(g_flip_queue, g_video, nullptr);
    return g_video;
}

void* video_out_flip_queue() { return &g_flip_queue; }
uint8_t* boot_buffer(int k) { return g_registered && k >= 0 && k < 2 ? g_buffers[k] : nullptr; }
int boot_buffer_index(int k) { return kFirstBuffer + k; }

void boot_note_bytes(uint64_t bytes) { g_bytes.fetch_add(bytes, std::memory_order_relaxed); }
void boot_note_graphics() { g_graphics = true; }

void boot_screen_start() {
    g_start = std::chrono::steady_clock::now();
    if (!boot_draw_load("/app0/boot")) {
        trace("boot screen: no assets in /app0/boot: the system splash stays");
        return;
    }
    if (video_out() < 0) return;
    off_t phys = 0;
    void* base = nullptr;
    if (ps4bs_allocate_direct(0, off_t(ps4bs_direct_memory_size()), 2 * kBufferSize, 2 << 20, kWcGarlic, &phys) != 0 ||
        ps4bs_map_direct(&base, 2 * kBufferSize, kProtCpuGpu, 0, phys, 2 << 20) != 0) {
        trace("boot screen: no direct memory for its buffers");
        return;
    }
    static uint8_t* buffers[2] = {static_cast<uint8_t*>(base), static_cast<uint8_t*>(base) + kBufferSize};
    g_buffers[0] = buffers[0], g_buffers[1] = buffers[1];
    void* addresses[2] = {buffers[0], buffers[1]};
    OrbisVideoOutBufferAttribute attribute;
    sceVideoOutSetBufferAttribute(&attribute, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
                                  ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, 0, kBootWidth, kBootHeight, kBootWidth);
    int set = sceVideoOutRegisterBuffers(g_video, kFirstBuffer, addresses, 2, &attribute);
    if (set < 0) {
        trace("boot screen: sceVideoOutRegisterBuffers failed: %x", unsigned(set));
        return;
    }
    g_registered = true;
    g_running = true;
    g_thread = std::thread([] { draw_loop(buffers); });
    trace("boot screen: drawing");
}

void boot_screen_stop() {
    std::call_once(g_stopped, [] {
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
        if (g_running) {
            g_stop = true;
            g_thread.join();
        }
        if (FILE* f = std::fopen(kProfile, "w")) {
            std::fprintf(f, "%llu %.1f\n", (unsigned long long)g_bytes.load(), secs);
            std::fclose(f);
        }
        trace("boot screen: first game frame after %.1f s, %llu MB read", secs,
              (unsigned long long)(g_bytes.load() >> 20));
        log("ps4: first game frame after %.1f s, %llu MB of game data read", secs,
            (unsigned long long)(g_bytes.load() >> 20));
    });
}

}  // namespace nx::plat::ps4
