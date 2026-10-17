// Memory guard (PS4): what the PS4 counts is direct memory, the 4.5 GiB pool the guest's lazily mapped chunks
// (platform_ps4.cpp) and RADV's buffers and images both come from. Every 30 s the log gets the guest's share, the
// pool's use and what is left; below NX_MEM_LOW_MB (default 96) left it says so every 5 s, since running out ends
// in a crash (a chunk that cannot be mapped) rather than in the system closing the app.
#include <sys/types.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

#include <nx/runtime.h>

#include "platform.h"

extern "C" {
size_t ps4_direct_memory_size_g() __asm__("sceKernelGetDirectMemorySize");
int32_t ps4_available_direct(off_t start, off_t end, size_t align, off_t* found, size_t* size) __asm__(
    "sceKernelAvailableDirectMemorySize");
int32_t ps4_available_flexible(size_t* size) __asm__("sceKernelAvailableFlexibleMemorySize");
void ps4_boot_trace(const char* line);
}

namespace nx::plat {

uint64_t ps4_lazy_mapped_bytes();
uint64_t ps4_lazy_faults();
uint64_t ps4_lazy_failed();
uint64_t ps4_lazy_blocks();

namespace {

constexpr double kMiB = 1024.0 * 1024.0;

// The largest free piece of direct memory (what the next allocation can get) and the pool's size.
void direct_memory(uint64_t& total, uint64_t& largest_free) {
    total = ps4_direct_memory_size_g();
    off_t found = 0;
    size_t size = 0;
    largest_free = ps4_available_direct(0, off_t(total), 0x10000, &found, &size) == 0 ? size : 0;
}

}  // namespace

void start_memory_guard() {
    const char* e = std::getenv("NX_MEM_LOW_MB");
    const double low_mb = e ? std::atof(e) : 96.0;
    uint64_t total = 0, free_now = 0;
    direct_memory(total, free_now);
    size_t flexible = 0;
    ps4_available_flexible(&flexible);
    char line[200];
    std::snprintf(line, sizeof line, "memory: direct memory %.0f MiB (%.0f MiB free), flexible %.0f MiB free",
                  double(total) / kMiB, double(free_now) / kMiB, double(flexible) / kMiB);
    ps4_boot_trace(line);
    log("%s", line);
    std::thread([low_mb] {
        uint64_t ticks = 0, peak = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            uint64_t t = 0, f = 0;
            direct_memory(t, f);
            uint64_t guest = ps4_lazy_mapped_bytes();
            if (guest > peak) peak = guest;
            bool low = double(f) / kMiB < low_mb;
            if (++ticks % 6 == 0 || low) {
                size_t flex = 0;
                ps4_available_flexible(&flex);  // thread stacks come from it: they ran out twice
                log("memory: guest %.0f MiB mapped (peak %.0f, %llu first touches, %llu direct memory blocks), "
                    "direct memory free %.0f of %.0f MiB, flexible %.0f MiB free%s",
                    double(guest) / kMiB, double(peak) / kMiB, (unsigned long long)ps4_lazy_faults(),
                    (unsigned long long)ps4_lazy_blocks(), double(f) / kMiB, double(t) / kMiB,
                    double(flex) / kMiB, low ? " - LOW" : "");
            }
            if (uint64_t failed = ps4_lazy_failed())
                log("memory: %llu chunks could not be mapped (out of direct memory)", (unsigned long long)failed);
        }
    }).detach();
}

uint64_t memguard_overlap_ns(uint64_t, uint64_t) { return 0; }

}  // namespace nx::plat
