// PS4 host (jailbroken, homebrew; SMW): guest memory on direct memory mapped on first touch, pthreads placed on the
// console's cores, VideoOut + RADV for Vulkan (ps4/ps4_vk_wsi.cpp), ScePad, sceAudioOut, and main.
//
// Same shape as platform_linux.cpp: main() starts the game (nx::boot) on its own thread and then polls the pad.
//
// Memory. The guest's ranges (runtime.h: modules at 0x40_0000_0000, heap, alias, stack and kernel regions up to
// 0x70_0000_0000) are address space reservations (sceKernelReserveVirtualRange). The PS4 has no demand-zero memory
// to speak of: flexible memory is small and committed when mapped, direct memory is physical. The runtime commits
// gigabytes it may never touch (svcSetHeapSize: the whole heap at once), so commit() only marks a range usable, and
// the fault handler maps 64 KiB chunks of direct memory there on the first access (zeroed). Kernel calls that write
// into guest memory (pread) cannot fault on our behalf, so read_at() touches its buffer first. decommit() gives the
// chunks back to a free list. Guest threads' 64 MiB host stacks come from the same lazy space (0x78_0000_0000),
// with their top 1 MiB mapped up front, so a thread costs what it uses.
//
// Cores: the app has cores 0-5 and the 7th (core 6) when the system grants it, checked at startup. The thread that
// replays NVN into Vulkan and presents gets core 1 to itself (frame_stats.cpp, role 0); host threads that are not the
// game's (audio output, vsync, watchdogs, ASTC decoding, RADV's own) start on cores 4, 5 and 6.
// NX_PS4_CORES=balanced (the default): guest threads on every core but 1. NX_PS4_CORES=pinned: guest threads where
// their Switch core says (svcCreateThread, svcSetThreadCoreMask): guest core 0 (the game's main thread) on core 0,
// guest cores 1 and 2 (the job workers) on cores 2 and 3, guest core 3 with the host threads. NX_PS4_CORES=spread:
// every thread on every core.
//
// Data: /data/SMW (analysis/{main,sdk}.bin, extract/romfs, spirv/, fonts/, user/), nx.log there. Settings:
// /data/SMW/config.txt, one NX_<KEY>=<value> per line, put in the environment at startup: every NX_* switch of the
// runtime (docs/nvn/SWITCHES.md) works there.
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <orbis/AudioOut.h>
#include <orbis/Pad.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>

#include <nx/game.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"
#include "ps4/ps4_app.h"
#include "ps4/ps4_boot_screen.h"
#include "ps4/ps4_menu.h"
#include "ps4/ps4_vk_wsi.h"

// libkernel, declared by hand: several OpenOrbis prototypes are wrong or take no parameters.
extern "C" {
int32_t ps4_reserve_range(void** addr, size_t len, int32_t flags, size_t align) __asm__("sceKernelReserveVirtualRange");
int32_t ps4_map_direct(void** addr, size_t len, int32_t prot, int32_t flags, off_t phys, size_t align) __asm__(
    "sceKernelMapDirectMemory");
int32_t ps4_allocate_direct(off_t start, off_t end, size_t len, size_t align, int32_t type, off_t* phys) __asm__(
    "sceKernelAllocateDirectMemory");
size_t ps4_direct_memory_size() __asm__("sceKernelGetDirectMemorySize");
int32_t ps4_munmap(void* addr, size_t len) __asm__("sceKernelMunmap");
void* ps4_pthread_self() __asm__("scePthreadSelf");
int32_t ps4_set_affinity(void* thread, uint64_t mask) __asm__("scePthreadSetaffinity");
int32_t ps4_get_affinity(void* thread, uint64_t* mask) __asm__("scePthreadGetaffinity");
int32_t ps4_set_prio(void* thread, int prio) __asm__("scePthreadSetprio");
int32_t ps4_rename(void* thread, const char* name) __asm__("scePthreadRename");
int32_t ps4_thread_id() __asm__("scePthreadGetthreadid");
int32_t ps4_available_flexible_main(size_t* size) __asm__("sceKernelAvailableFlexibleMemorySize");

void ps4_boot_trace(const char* line);       // port/ps4_crashlog.cpp
void ps4_watch_thread(const char* name);     // port/ps4_sampler.cpp
void ps4_sampler_start();                    // port/ps4_sampler.cpp
void ps4_sampler_register_thread(const char* name);  // port/ps4_sampler.cpp
void ps4_boost_thread();                     // port/ps4_runtime.cpp: priority 650 (default 700)
extern int ac_ps4_flush_all_draws;           // RADV (Crema's Mesa): after every draw write back and invalidate every
                                             // GPU cache (a diagnostic: NX_PS4_GPU_FLUSH=1)
extern uint64_t ps4_new_thread_cores;        // port/ps4_runtime.cpp: the cores pthread_create gives new threads
}

namespace nx {
void boot();
}

namespace nx::plat {

namespace {

const char* const kDataRoot = NX_PS4_DATA;

constexpr int32_t kMapFixed = 0x10;  // SCE_KERNEL_MAP_FIXED
constexpr int32_t kProtCpuRw = 0x3;
constexpr int32_t kWbOnion = 0;      // SCE_KERNEL_WB_ONION

void trace(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void trace(const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    ps4_boot_trace(line);
}

// ---- lazy guest memory --------------------------------------------------------------------------------------

namespace lazy {

constexpr uint64_t kBase = 0x4000000000ull;      // the guest's ranges (runtime.h) and the host stacks below
constexpr uint64_t kEnd = 0x7C00000000ull;
constexpr uint64_t kChunk = 0x10000;             // 64 KiB, four PS4 pages
constexpr uint64_t kSlice = 1ull << 30;          // the state tables come per GiB that has commits in it
constexpr uint64_t kChunksPerSlice = kSlice / kChunk;
constexpr uint64_t kSlices = (kEnd - kBase) / kSlice;
constexpr uint64_t kBlock = 32ull << 20;         // direct memory is allocated in blocks of this, mapped by chunk
constexpr size_t kMaxFree = 1u << 17;            // freed chunks kept for reuse (8 GiB worth)

enum : uint8_t { kUnused = 0, kCommitted = 1, kMapped = 2 };

struct Slice {
    std::atomic<uint8_t> state[kChunksPerSlice];
    uint32_t phys[kChunksPerSlice];  // direct memory offset / kChunk, while mapped
};

std::atomic<Slice*> g_slices[kSlices];
std::mutex g_slice_alloc;
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;  // the fault handler takes it: no mutex
uint64_t g_block = 0, g_block_size = 0, g_block_used = 0;  // the current block (offset, size), how much is handed out
uint32_t g_free[kMaxFree];
size_t g_free_count = 0;
std::atomic<uint64_t> g_mapped{0}, g_faults{0}, g_failed{0};
std::atomic<uint64_t> g_blocks{0};

struct Lock {
    Lock() {
        while (g_lock.test_and_set(std::memory_order_acquire)) sched_yield();
    }
    ~Lock() { g_lock.clear(std::memory_order_release); }
};

bool ours(uint64_t addr) { return addr >= kBase && addr < kEnd; }

Slice* slice_of(uint64_t addr) { return g_slices[(addr - kBase) / kSlice].load(std::memory_order_acquire); }

Slice* make_slice(uint64_t addr) {
    auto& slot = g_slices[(addr - kBase) / kSlice];
    if (Slice* s = slot.load(std::memory_order_acquire)) return s;
    std::lock_guard lk(g_slice_alloc);
    if (Slice* s = slot.load(std::memory_order_acquire)) return s;
    auto* s = static_cast<Slice*>(std::calloc(1, sizeof(Slice)));
    if (!s) fatal("memory: no host memory for the state of %llx", (unsigned long long)addr);
    slot.store(s, std::memory_order_release);
    return s;
}

constexpr uint32_t kNoPhys = UINT32_MAX;

// A free chunk of direct memory (offset / kChunk), kNoPhys when there is none. With g_lock held.
uint32_t take_phys() {
    if (g_free_count) return g_free[--g_free_count];
    if (g_block_used == g_block_size) {
        // 32 MiB at a time; when direct memory is that fragmented (late in a course, with the GPU's allocations
        // between ours), smaller pieces down to one chunk, not a crash with megabytes still free.
        off_t phys = 0;
        uint64_t size = kBlock;
        while (ps4_allocate_direct(0, off_t(ps4_direct_memory_size()), size, kChunk, kWbOnion, &phys) != 0)
            if ((size /= 2) < kChunk) return kNoPhys;
        g_block = uint64_t(phys);
        g_block_size = size;
        g_block_used = 0;
        g_blocks++;
    }
    uint64_t at = g_block + g_block_used;
    g_block_used += kChunk;
    return uint32_t(at / kChunk);
}

// Zeroes direct memory chunk `phys` before it shows at a guest address: mapped where the kernel likes, cleared there,
// unmapped. Cleared in place, after mapping, the chunk was already every thread's while the memset ran, and a write
// another thread made there meanwhile was lost: random guest corruption (found in SSBU v00.16; TOTK 640b099 took the
// same fix). False when it could not be mapped (the caller clears in place, as before).
bool zero_phys(uint32_t phys) {
    void* at = nullptr;
    if (ps4_map_direct(&at, kChunk, kProtCpuRw, 0, off_t(phys) * off_t(kChunk), kChunk) != 0 || !at) return false;
    std::memset(at, 0, kChunk);  // direct memory comes with whatever was there before
    ps4_munmap(at, kChunk);
    return true;
}

// Maps the chunk at `addr` (committed, unmapped) with zeroes. With g_lock held. False: out of direct memory.
bool map_chunk(Slice* s, uint64_t index, uint64_t addr) {
    uint32_t phys = take_phys();
    if (phys == kNoPhys) return false;
    const bool zeroed = zero_phys(phys);
    ps4_munmap(reinterpret_cast<void*>(addr), kChunk);  // our reservation there: mapping over it fails
    void* at = reinterpret_cast<void*>(addr);
    int32_t r = ps4_map_direct(&at, kChunk, kProtCpuRw, kMapFixed, off_t(phys) * off_t(kChunk), kChunk);
    if (r != 0 || at != reinterpret_cast<void*>(addr)) {
        void* back = reinterpret_cast<void*>(addr);
        ps4_reserve_range(&back, kChunk, kMapFixed, 0);
        if (g_free_count < kMaxFree) g_free[g_free_count++] = phys;
        return false;
    }
    if (!zeroed) std::memset(at, 0, kChunk);  // no window to clear it in (no log: this runs in the fault handler)
    s->phys[index] = phys;
    s->state[index].store(kMapped, std::memory_order_release);
    g_mapped += kChunk;
    return true;
}

thread_local uint64_t t_last_fault = 0;
thread_local uint32_t t_repeats = 0;

// The fault handler's part: true when `addr` was committed memory that is mapped now (the access is retried).
bool fault(uint64_t addr) {
    if (!ours(addr)) return false;
    Slice* s = slice_of(addr);
    if (!s) return false;
    uint64_t chunk = addr & ~(kChunk - 1), index = (chunk - kBase) % kSlice / kChunk;
    uint8_t st = s->state[index].load(std::memory_order_acquire);
    if (st == kMapped) {
        // Another thread mapped it between the access and now: retry. The same address faulting again and again on
        // a mapped chunk is a real protection fault: the crash handler's.
        t_repeats = t_last_fault == addr ? t_repeats + 1 : 0;
        t_last_fault = addr;
        return t_repeats < 4;
    }
    if (st != kCommitted) return false;
    Lock lk;
    st = s->state[index].load(std::memory_order_acquire);
    if (st == kMapped) return true;
    if (st != kCommitted) return false;
    g_faults++;
    if (!map_chunk(s, index, chunk)) {
        g_failed++;
        return false;  // out of direct memory: the crash handler reports it
    }
    return true;
}

void commit(uint64_t addr, uint64_t size) {
    uint64_t a = addr & ~(kChunk - 1), e = (addr + size + kChunk - 1) & ~(kChunk - 1);
    for (uint64_t c = a; c < e; c += kChunk) {
        Slice* s = make_slice(c);
        uint64_t index = (c - kBase) % kSlice / kChunk;
        uint8_t expected = kUnused;
        s->state[index].compare_exchange_strong(expected, kCommitted, std::memory_order_acq_rel);
    }
}

void decommit(uint64_t addr, uint64_t size) {
    uint64_t lo = (addr + kChunk - 1) & ~(kChunk - 1), hi = (addr + size) & ~(kChunk - 1);
    auto zero = [](uint64_t from, uint64_t to) {  // part of a chunk that stays: zeroes, if it is mapped at all
        if (from >= to) return;
        Slice* s = slice_of(from);
        if (s && s->state[(from - kBase) % kSlice / kChunk].load() == kMapped)
            std::memset(reinterpret_cast<void*>(from), 0, to - from);
    };
    if (lo >= hi) {
        zero(addr, addr + size);
        return;
    }
    zero(addr, lo);
    zero(hi, addr + size);
    Lock lk;
    for (uint64_t c = lo; c < hi; c += kChunk) {
        Slice* s = slice_of(c);
        if (!s) continue;
        uint64_t index = (c - kBase) % kSlice / kChunk;
        uint8_t st = s->state[index].load(std::memory_order_acquire);
        if (st == kMapped) {
            ps4_munmap(reinterpret_cast<void*>(c), kChunk);
            void* back = reinterpret_cast<void*>(c);
            ps4_reserve_range(&back, kChunk, kMapFixed, 0);
            if (g_free_count < kMaxFree) g_free[g_free_count++] = s->phys[index];
            g_mapped -= kChunk;
        }
        s->state[index].store(kUnused, std::memory_order_release);
    }
}

// Touches every chunk of [addr, addr + size) that is ours, so the kernel can write there.
void prefault(uint64_t addr, uint64_t size) {
    if (!size || addr + size <= kBase || addr >= kEnd) return;
    uint64_t a = std::max(addr, kBase) & ~(kChunk - 1), e = std::min(addr + size, kEnd);
    for (uint64_t c = a; c < e; c += kChunk) {
        Slice* s = slice_of(c);
        if (s && s->state[(c - kBase) % kSlice / kChunk].load(std::memory_order_acquire) == kCommitted)
            (void)*reinterpret_cast<volatile uint8_t*>(std::max(c, addr));
    }
}

}  // namespace lazy

// ---- host stacks for start_thread -----------------------------------------------------------------------------

constexpr uint64_t kStacksBase = 0x7800000000ull;  // inside the lazy space, above the guest's ranges
constexpr uint64_t kStackSlot = 64ull << 20;
constexpr int kStackSlots = int((lazy::kEnd - kStacksBase) / kStackSlot);  // 256
constexpr uint64_t kStackGuard = lazy::kChunk;     // never committed: an overflow faults
constexpr uint64_t kStackEager = 1ull << 20;       // mapped at once (kernel calls with buffers on the stack)

struct StackSlot {
    std::atomic<int> state{0};  // 0 free, 1 in use, 2 its thread is ending (join it, then reuse)
    pthread_t thread{};
};
StackSlot g_stack_slots[kStackSlots];
std::mutex g_stack_lock;
thread_local int t_stack_slot = -1;

// ---- cores ----------------------------------------------------------------------------------------------------

uint64_t g_app_cores = 0x3F;     // the cores this process may use (0x7F with the 7th)
uint64_t g_helper_cores = 0x30;  // threads that are not the game's: cores 4, 5 (and 6)
enum class CoreMode { balanced, pinned, spread };
CoreMode g_mode = CoreMode::balanced;  // NX_PS4_CORES

constexpr int kCoreGuest0 = 0, kCoreReplay = 1, kCoreGuest1 = 2, kCoreGuest2 = 3;

uint64_t guest_core_mask(int core) {
    switch (g_mode) {
        case CoreMode::spread: return g_app_cores;
        // Every core but the replay thread's: several guest threads share a Switch core, and pinned to one PS4 core
        // a thread spinning on a lock could keep its holder off it.
        case CoreMode::balanced: return g_app_cores & ~(1ull << kCoreReplay);
        case CoreMode::pinned: break;
    }
    switch (core) {
        case 0: return 1ull << kCoreGuest0;
        case 1: return 1ull << kCoreGuest1;
        case 2: return 1ull << kCoreGuest2;
        default: return g_helper_cores;  // guest core 3 (the Switch's system core)
    }
}

void detect_cores() {
    void* self = ps4_pthread_self();
    uint64_t before = 0;
    ps4_get_affinity(self, &before);
    uint64_t got = 0;
    bool seventh = ps4_set_affinity(self, 0x40) == 0 && ps4_get_affinity(self, &got) == 0 && got == 0x40;
    g_app_cores = seventh ? 0x7F : 0x3F;
    g_helper_cores = seventh ? 0x70 : 0x30;
    const char* mode = std::getenv("NX_PS4_CORES");
    g_mode = mode && std::strcmp(mode, "spread") == 0   ? CoreMode::spread
             : mode && std::strcmp(mode, "pinned") == 0 ? CoreMode::pinned
                                                        : CoreMode::balanced;
    if (g_mode == CoreMode::spread) g_helper_cores = g_app_cores;
    ps4_new_thread_cores = g_helper_cores;
    ps4_set_affinity(self, g_helper_cores);
    const char* names[] = {"balanced", "pinned", "spread"};
    trace("cores: %s (main thread had %#llx), %s: guest threads on %#llx (pinned: guest cores 0/1/2 on %d/%d/%d), "
          "replay+present on %d, the rest on %#llx",
          seventh ? "7, the 7th granted" : "6, no 7th", (unsigned long long)before, names[int(g_mode)],
          (unsigned long long)guest_core_mask(0), kCoreGuest0, kCoreGuest1, kCoreGuest2, kCoreReplay,
          (unsigned long long)g_helper_cores);
}

// Host threads by id, for placing guest threads from another thread (svcSetThreadCoreMask).
struct Known {
    std::atomic<int64_t> tid{0};
    void* thread = nullptr;
    std::atomic<bool> guest{false};
};
constexpr int kKnown = 512;
Known g_known[kKnown];

void register_thread() {
    int64_t tid = ps4_thread_id();
    for (Known& k : g_known) {
        int64_t expected = 0;
        if (k.tid.compare_exchange_strong(expected, tid)) {
            k.thread = ps4_pthread_self();
            k.guest = false;
            return;
        }
    }
}

void forget_thread() {
    int64_t tid = ps4_thread_id();
    for (Known& k : g_known)
        if (k.tid == tid) {
            k.thread = nullptr;
            k.tid = 0;
        }
}

Known* known(int64_t tid) {
    for (Known& k : g_known)
        if (k.tid == tid) return &k;
    return nullptr;
}

// ---- pad --------------------------------------------------------------------------------------------------------

// Up to four DualShock 4s, one per signed-in user: player 1 is the user who started the app, players 2-4 the others
// in the order they appear (each controller on a PS4 belongs to a user; a second player picks theirs or a guest).
constexpr int kPlayers = 4;
struct Player {
    int32_t user = -1;
    int handle = -1;
    bool connected = false;
    PadState state;
};
std::mutex g_pad_lock;
Player g_players[kPlayers];
bool g_cross_confirms = true;
std::atomic<bool> g_diag_requested{false};  // NX_BUTTONS=nintendo: by position (Circle = A) instead

// ---- audio ------------------------------------------------------------------------------------------------------

struct Pending {
    const uint8_t* data;
    size_t size, pos;
    void* ctx;
};
AudioDone g_done = nullptr;
int32_t g_audio_port = -1;
std::mutex g_audio;
std::deque<Pending> g_pending;
std::atomic<bool> g_audio_paused{false};
constexpr int kGrainFrames = 256;  // 5.3 ms at 48 kHz

}  // namespace

uint64_t ps4_lazy_mapped_bytes() { return lazy::g_mapped.load(); }
uint64_t ps4_lazy_faults() { return lazy::g_faults.load(); }
uint64_t ps4_lazy_failed() { return lazy::g_failed.load(); }
uint64_t ps4_lazy_blocks() { return lazy::g_blocks.load(); }

// ---- memory ---------------------------------------------------------------------------------------------------

bool reserve(uint64_t addr, uint64_t size) {
    void* a = reinterpret_cast<void*>(addr);
    int32_t r = ps4_reserve_range(&a, size, kMapFixed, 0);
    if (r != 0 || a != reinterpret_cast<void*>(addr)) {
        trace("memory: reserving %#llx+%#llx failed (%#x, got %p)", (unsigned long long)addr,
              (unsigned long long)size, unsigned(r), a);
        return false;
    }
    return true;
}

bool commit(uint64_t addr, uint64_t size) {
    if (!lazy::ours(addr) || !lazy::ours(addr + size - 1)) {
        log("memory: commit %llx+%llx is outside the lazy space", (unsigned long long)addr, (unsigned long long)size);
        return false;
    }
    lazy::commit(addr, size);
    return true;
}

void decommit(uint64_t addr, uint64_t size) {
    if (lazy::ours(addr)) lazy::decommit(addr, size);
}

// ---- threads and time -----------------------------------------------------------------------------------------

struct ThreadStart {
    ThreadFn fn;
    void* arg;
    int slot;
};

bool profile_requested();  // /data/<app>/profile.txt (below)

static void* thread_entry(void* p) {
    ThreadStart s = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    t_stack_slot = s.slot;
    register_thread();
    if (profile_requested()) {  // guest threads sampled too, named by start order (port/ps4_sampler.cpp)
        // The first 60: the sampler has 64 slots, and the replay thread and the game's main thread register later
        // (v00.30's 48 guests left them out).
        static std::atomic<int> started{0};
        if (int n = started++; n < 60) {
            char* name = new char[16];
            std::snprintf(name, 16, "guest %d", n);
            ps4_sampler_register_thread(name);
        }
    }
    s.fn(s.arg);
    forget_thread();
    if (s.slot >= 0) g_stack_slots[s.slot].state = 2;
    return nullptr;
}

void start_thread(ThreadFn fn, void* arg, size_t stack_size) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    // The slot lock is held until the new thread's handle is in its slot: a later start_thread may join it.
    std::unique_lock lk(g_stack_lock);
    int slot = -1;
    // Guest threads ask for exactly a slot (64 MiB): they get it less the guard chunk.
    if (stack_size <= kStackSlot) {
        for (int k = 0; k < kStackSlots && slot < 0; k++) {
            int st = g_stack_slots[k].state.load();
            if (st == 2) {  // its thread has returned from (or is leaving) its function: wait until it is gone
                pthread_join(g_stack_slots[k].thread, nullptr);
                st = 0;
            }
            if (st == 0) slot = k;
        }
        if (slot >= 0) g_stack_slots[slot].state = 1;
    }
    if (slot >= 0) {
        uint64_t base = kStacksBase + uint64_t(slot) * kStackSlot;
        commit(base + kStackGuard, kStackSlot - kStackGuard);
        lazy::prefault(base + kStackSlot - kStackEager, kStackEager);
        pthread_attr_setstack(&a, reinterpret_cast<void*>(base + kStackGuard), size_t(kStackSlot - kStackGuard));
    } else {
        log("threads: no lazy stack slot left: an 8 MiB system stack");
        pthread_attr_setstacksize(&a, 8u << 20);
    }
    pthread_t t;
    int r = pthread_create(&t, &a, thread_entry, new ThreadStart{fn, arg, slot});
    pthread_attr_destroy(&a);
    if (r != 0) fatal("threads: pthread_create failed (%d)", r);
    if (slot >= 0)
        g_stack_slots[slot].thread = t;
    else
        pthread_detach(t);
}

void exit_thread() {
    forget_thread();
    if (t_stack_slot >= 0) g_stack_slots[t_stack_slot].state = 2;
    pthread_exit(nullptr);
}

void exit_process(int code) {
    std::fflush(nullptr);
    _exit(code);
}

void sleep_ns(int64_t ns) {
    if (ns <= 0) {
        sched_yield();
        return;
    }
    timespec ts{time_t(ns / 1000000000), long(ns % 1000000000)};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

void yield() { sched_yield(); }

void set_thread_name(const char* name) {
    char n[32];
    std::snprintf(n, sizeof n, "%s", name);
    ps4_rename(ps4_pthread_self(), n);
    ps4_watch_thread(n);
}

int64_t current_thread_id() { return int64_t(ps4_thread_id()); }

bool profile(const int64_t*, const uint64_t* const*, size_t, int, const std::string&) {
    return false;  // port/ps4_sampler.cpp (/data/SMW/profile.txt) samples every thread
}

uint64_t monotonic_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);  // the time stamp counter (port/ps4_runtime.cpp)
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

void set_guest_core(int64_t host_tid, int core) {
    Known* k = known(host_tid);
    if (!k || !k->thread) return;
    k->guest = true;
    ps4_set_affinity(k->thread, guest_core_mask(core));
}

// Whether /data/<app>/profile.txt exists (the sampler, port/ps4_sampler.cpp). Through open: access() answered "no"
// on the console for a file that was there (TOTK v00.27 never started the sampler).
bool profile_requested() {
    static const bool on = [] {
        int fd = open(NX_PS4_DATA "/profile.txt", O_RDONLY);
        if (fd < 0) return false;
        close(fd);
        return true;
    }();
    return on;
}

void place_frame_thread(int role) {
    if (role == 2) {  // a replay worker (nvn_parallel.cpp): every core but the replay thread's, as high as it
        ps4_set_affinity(ps4_pthread_self(), g_app_cores & ~(1ull << kCoreReplay));
        ps4_boost_thread();
        return;
    }
    // /data/SMW/profile.txt: these two are the threads worth sampling (port/ps4_sampler.cpp, samples.log)
    if (profile_requested()) ps4_sampler_register_thread(role == 0 ? "replay" : "game main");
    Known* k = known(ps4_thread_id());
    if (k && k->guest) return;  // a guest thread presents (NX_REPLAY_THREAD=0): it keeps its guest core
    if (role == 0) {
        ps4_set_affinity(ps4_pthread_self(), g_mode == CoreMode::spread ? g_app_cores : 1ull << kCoreReplay);
        ps4_boost_thread();  // above loading and shader compiles (port/ps4_runtime.cpp)
    }
}

// ---- files and output -----------------------------------------------------------------------------------------

FILE* fopen_utf8(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }
int64_t ftell64(FILE* f) { return ftello(f); }
int fseek64(FILE* f, int64_t off, int origin) { return fseeko(f, off, origin); }

uint64_t read_at(FILE* f, int64_t off, void* buf, uint64_t size) {
    if (size > uint64_t(SSIZE_MAX)) return 0;  // the game passes -1 as "everything": fread read nothing
    lazy::prefault(reinterpret_cast<uint64_t>(buf), size);  // the kernel writes there: no faults to map it
    int fd = fileno(f);
    uint64_t done = 0;
    bool touched = false;
    while (done < size) {
        ssize_t n = pread(fd, static_cast<uint8_t*>(buf) + done, size_t(size - done), off + int64_t(done));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EFAULT && !touched) {
            // A page the kernel cannot write: one a write watch keeps read-only (write_watch.cpp, 16 KiB pages here,
            // so a watched texture's neighbours too). Storing what is there makes the watch's fault handler give the
            // page back; the read overwrites it anyway.
            touched = true;
            auto* p = static_cast<volatile uint8_t*>(buf) + done;
            for (uint64_t k = 0; k < size - done; k += 0x4000 - (reinterpret_cast<uintptr_t>(p + k) & 0x3FFF)) p[k] = p[k];
            p[size - done - 1] = p[size - done - 1];
            continue;
        }
        if (n < 0) {
            static std::atomic<int> logged{0};
            if (logged++ < 20)
                log("fs: read of %llu bytes at %lld into %p failed after %llu bytes (errno %d)", (unsigned long long)size,
                    (long long)off, buf, (unsigned long long)done, errno);
        }
        if (n <= 0) break;
        done += uint64_t(n);
    }
    ps4::boot_note_bytes(done);  // the boot screen's progress
    return done;
}

void prefault(uint64_t addr, uint64_t size) { lazy::prefault(addr, size); }

bool make_dirs(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}

void log_line(const char*) {}  // nx.log only: the PS4 has no console to write to
std::string data_root() { return kDataRoot; }

// The package's own copy of the game data (scripts/package-smw.sh, SMW_BUNDLE_DATA) when it has one, else /data/SMW
// (scripts/upload-data.py).
std::string content_root() {
    static const std::string root = access("/app0/data/extract/romfs", F_OK) == 0 || access("/app0/data/romfs.idx", F_OK) == 0 ? "/app0/data" : kDataRoot;
    return root;
}

std::string setting(const std::string& key) {
    std::string env = "NX_";
    for (char ch : key) env += char(std::toupper(static_cast<unsigned char>(ch)));
    const char* v = std::getenv(env.c_str());
    return v ? v : "";
}

// ---- display ----------------------------------------------------------------------------------------------------

namespace {
int g_display_token;
}

bool headless() { return false; }
void* create_window() { return &g_display_token; }  // the VideoOut display; the surface is the headless one
void release_window(void*) {}
uint64_t window_generation() { return 1; }
bool window_minimized() { return false; }

void window_size(uint32_t& w, uint32_t& h) {
    w = ps4::kScreenWidth;
    h = ps4::kScreenHeight;
}

void screen_size(uint32_t& w, uint32_t& h) { window_size(w, h); }
float default_render_scale() { return 1.0f; }

// ---- input: a DualShock 4 per signed-in user, up to four players (polled on the main thread) --------------------

static void open_player(int k, int32_t user) {
    Player& p = g_players[k];
    int handle = scePadOpen(user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
    if (handle < 0) return;  // no controller for that user yet: the next scan tries again
    p.user = user;
    p.handle = handle;
    trace("pad: player %d is user %08x (handle %d)", k + 1, unsigned(user), handle);
}

// Users who signed out lose their slot; signed-in users with a controller get the first free one. Player 1 is kept
// for the user who started the app.
static void scan_players() {
    OrbisUserServiceLoginUserIdList list{};
    if (sceUserServiceGetLoginUserIdList(&list) != 0) return;
    auto signed_in = [&](int32_t user) {
        for (int i = 0; i < ORBIS_USER_SERVICE_MAX_LOGIN_USERS; i++)
            if (list.userId[i] == user) return true;
        return false;
    };
    for (int k = 0; k < kPlayers; k++) {
        Player& p = g_players[k];
        if (p.handle >= 0 && !signed_in(p.user)) {
            scePadClose(p.handle);
            trace("pad: player %d (user %08x) signed out", k + 1, unsigned(p.user));
            std::lock_guard lk(g_pad_lock);
            p = Player();
        }
    }
    static int32_t initial = [] {
        int32_t u = -1;
        return sceUserServiceGetInitialUser(&u) == 0 ? u : -1;
    }();
    if (g_players[0].handle < 0 && initial >= 0 && signed_in(initial)) open_player(0, initial);
    for (int i = 0; i < ORBIS_USER_SERVICE_MAX_LOGIN_USERS; i++) {
        int32_t user = list.userId[i];
        if (user == -1 || user == 0) continue;
        bool known = false;
        for (const Player& p : g_players) known |= p.handle >= 0 && p.user == user;
        if (known) continue;
        // Player 1's slot waits for the user who started the app, unless that user has left.
        int first = g_players[0].handle < 0 && !(initial >= 0 && signed_in(initial)) ? 0 : 1;
        for (int k = first; k < kPlayers; k++)
            if (g_players[k].handle < 0) {
                open_player(k, user);
                break;
            }
    }
}

static int32_t stick(uint8_t v, bool invert) {
    int32_t s = (int32_t(v) - 128) * 258;  // -33024..32766
    if (s > -3277 && s < 3277) return 0;   // 10% dead zone
    s = std::clamp(s, -kStickMax, kStickMax);
    return invert ? -s : s;
}

static bool read_player(const Player& pl, PadState& p) {
    OrbisPadData d{};
    if (pl.handle < 0 || scePadReadState(pl.handle, &d) != 0 || !d.connected) return false;
    {
        // The Switch buttons from the quick menu's mapping (ps4_menu.cpp). By default Cross is A (confirm, as on every
        // PS4 menu) and Circle B; NX_BUTTONS=nintendo keeps the positions (Circle A, Cross B). L2/R2 count from a
        // light pull of the trigger.
        uint32_t b = d.buttons;
        if (d.analogButtons.l2 > 48) b |= ORBIS_PAD_BUTTON_L2;
        if (d.analogButtons.r2 > 48) b |= ORBIS_PAD_BUTTON_R2;
        static const uint64_t npad[ps4::kNxCount] = {kA, kB, kX, kY, kL, kR, kZL, kZR, kPlus, kMinus, kStickL, kStickR};
        for (int k = 0; k < ps4::kNxCount; k++)
            if (b & ps4::settings_ds4_for(k)) p.buttons |= npad[k];
        struct Map {
            uint32_t bit;
            uint64_t npad;
        };
        const Map dpad[] = {{ORBIS_PAD_BUTTON_LEFT, kLeft}, {ORBIS_PAD_BUTTON_UP, kUp},
                            {ORBIS_PAD_BUTTON_RIGHT, kRight}, {ORBIS_PAD_BUTTON_DOWN, kDown}};
        for (const Map& m : dpad)
            if (b & m.bit) p.buttons |= m.npad;
        p.lx = stick(d.leftStick.x, false);
        p.ly = stick(d.leftStick.y, true);  // DS4: down is positive; Horizon: up is positive
        p.rx = stick(d.rightStick.x, false);
        p.ry = stick(d.rightStick.y, true);
    }
    return true;
}

static void sample_input() {
    static int ticks = 0;
    if (ticks++ % 500 == 0) scan_players();  // every 2 s: sign-ins, sign-outs, controllers turned on
    uint32_t raw = 0;  // player 1's buttons as they are (the menu's own mapping does not apply to L3 + R3)
    for (int k = 0; k < kPlayers; k++) {
        PadState p;
        bool on = read_player(g_players[k], p);
        if (k == 0 && on) {
            OrbisPadData d{};
            if (scePadReadState(g_players[0].handle, &d) == 0) raw = d.buttons;
        }
        std::lock_guard lk(g_pad_lock);
        if (on != g_players[k].connected) trace("pad: player %d %s", k + 1, on ? "connected" : "disconnected");
        g_players[k].connected = on;
        g_players[k].state = p;
    }
    // L3 + R3 held 0.4 s on player 1's controller: the quick menu (ps4_menu.cpp; its own item takes a storyboard).
    // While it is open, and until every button is let go after it, the game reads no buttons.
    static int held = 0;
    bool both = (raw & (ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3)) == (ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3);
    held = both ? held + 1 : 0;
    if (held == 100) {
        ps4::menu_request();
        trace("menu: asked for (L3 + R3)");
    }
    if (ps4::menu_blocks_input(raw)) {
        std::lock_guard lk(g_pad_lock);
        for (Player& pl : g_players) pl.state = PadState();
    }
}

namespace ps4 {
bool menu_pad(uint32_t& buttons, uint8_t& left_y) {
    OrbisPadData d{};
    if (g_players[0].handle < 0 || scePadReadState(g_players[0].handle, &d) != 0 || !d.connected) return false;
    buttons = d.buttons;
    if (d.analogButtons.l2 > 48) buttons |= ORBIS_PAD_BUTTON_L2;
    if (d.analogButtons.r2 > 48) buttons |= ORBIS_PAD_BUTTON_R2;
    left_y = d.leftStick.y;
    return true;
}
void request_diag() { g_diag_requested = true; }
}  // namespace ps4

bool live_fsr(bool& on, float& sharpness) {
    on = ps4::settings_fsr();
    sharpness = ps4::settings_fsr_sharpness();
    return true;
}

bool take_diag_request() { return g_diag_requested.exchange(false); }

void read_pad(PadState& p) {
    {
        std::lock_guard lk(g_pad_lock);
        const PadState& s = g_players[0].state;
        p.buttons |= s.buttons;
        if (s.lx) p.lx = s.lx;
        if (s.ly) p.ly = s.ly;
        if (s.rx) p.rx = s.rx;
        if (s.ry) p.ry = s.ry;
    }
    input_script(p);
}

bool read_extra_pad(int player, PadState& p) {
    if (player < 1 || player >= kPlayers) return false;
    std::lock_guard lk(g_pad_lock);
    if (!g_players[player].connected) return false;
    p = g_players[player].state;
    return true;
}

// ---- audio: sceAudioOut, s16 stereo 48 kHz, fed from the queue of submitted buffers ----------------------------

static void* audio_thread(void*) {
    set_thread_name(NX_PS4_APP " AudioOut");
    // above the game's threads, so a slow frame does not make the sound crackle (default priority 700)
    ps4_set_prio(ps4_pthread_self(), 400);
    alignas(64) static int16_t grains[2][kGrainFrames * 2];
    int current = 0;
    std::vector<void*> finished;
    for (;;) {
        uint8_t* buf = reinterpret_cast<uint8_t*>(grains[current]);
        const size_t n = sizeof grains[0];
        size_t filled = 0;
        finished.clear();
        if (!g_audio_paused) {
            std::lock_guard lk(g_audio);
            while (filled < n && !g_pending.empty()) {
                Pending& p = g_pending.front();
                size_t take = std::min(n - filled, p.size - p.pos);
                std::memcpy(buf + filled, p.data + p.pos, take);
                filled += take;
                p.pos += take;
                if (p.pos == p.size) {
                    finished.push_back(p.ctx);
                    g_pending.pop_front();
                }
            }
        }
        if (filled < n) std::memset(buf + filled, 0, n - filled);  // underrun or paused: silence
        for (void* ctx : finished) g_done(ctx);
        sceAudioOutOutput(g_audio_port, buf);  // blocks until the previous grain plays
        current ^= 1;
    }
    return nullptr;
}

bool audio_open(AudioDone done) {
    g_done = done;
    if (std::getenv("NX_MUTE")) return false;
    int32_t r = sceAudioOutInit();
    if (r != 0 && r != int32_t(0x8026000E)) {  // already initialized is fine
        log("audio: sceAudioOutInit failed: %08x", unsigned(r));
        return false;
    }
    g_audio_port = sceAudioOutOpen(0xFF, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, kGrainFrames, 48000,
                                   ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (g_audio_port < 0) {
        log("audio: sceAudioOutOpen failed: %08x", unsigned(g_audio_port));
        return false;
    }
    int32_t volume[2] = {32768, 32768};  // 0 dB
    sceAudioOutSetVolume(g_audio_port, 0x3, volume);
    pthread_t t;
    pthread_create(&t, nullptr, audio_thread, nullptr);
    pthread_detach(t);
    log("audio: sceAudioOut port %08x, %d frames at 48 kHz", unsigned(g_audio_port), kGrainFrames);
    return true;
}

bool audio_submit(const void* data, size_t bytes, void* ctx) {
    if (g_audio_port < 0) return false;
    std::lock_guard lk(g_audio);
    g_pending.push_back({static_cast<const uint8_t*>(data), bytes, 0, ctx});
    return true;
}

uint32_t audio_queued() {
    std::lock_guard lk(g_audio);
    return uint32_t(g_pending.size());
}

void audio_pause(bool paused) { g_audio_paused = paused; }

// ---- Vulkan: RADV linked in, with the VideoOut surface (ps4/ps4_vk_wsi.cpp) ------------------------------------

void* vulkan_get_instance_proc_addr() { return ps4::vk_get_instance_proc_addr(); }
const char* vulkan_surface_extension() { return VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME; }

int vulkan_create_surface(void* instance, void*, uint64_t* surface, void* gipa) {
    auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(gipa ? gipa : ps4::vk_get_instance_proc_addr());
    auto create = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
        get(static_cast<VkInstance>(instance), "vkCreateHeadlessSurfaceEXT"));
    VkHeadlessSurfaceCreateInfoEXT ci{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    VkSurfaceKHR s = VK_NULL_HANDLE;
    VkResult r = create ? create(static_cast<VkInstance>(instance), &ci, nullptr, &s) : VK_ERROR_EXTENSION_NOT_PRESENT;
    *surface = reinterpret_cast<uint64_t>(s);
    return r;
}

}  // namespace nx::plat

// ---- fault handler, crash report and entry --------------------------------------------------------------------

namespace {

struct sigaction g_old_segv, g_old_bus;

void chain(int sig, siginfo_t* info, void* uc) {
    struct sigaction& old = sig == SIGBUS ? g_old_bus : g_old_segv;
    if (old.sa_flags & SA_SIGINFO) {
        old.sa_sigaction(sig, info, uc);
        return;
    }
    if (old.sa_handler == SIG_IGN) return;
    if (old.sa_handler != SIG_DFL) {
        old.sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void fault_handler(int sig, siginfo_t* info, void* uc) {
    if (nx::plat::write_watch_fault(info->si_addr)) return;               // write_watch.cpp
    if (nx::plat::lazy::fault(uint64_t(uintptr_t(info->si_addr)))) return;  // first touch of guest memory
    chain(sig, info, uc);  // port/ps4_crashlog.cpp writes crash.log (and calls ps4_crash_guest_info)
}

void install_fault_handler() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_old_segv);
    sigaction(SIGBUS, &sa, &g_old_bus);
}

void load_system_modules() {
    // Calling into a system library whose module isn't loaded ends the app without a word.
    const std::pair<const char*, uint32_t> modules[] = {
        {"SystemService", 0x80000010}, {"UserService", 0x80000011}, {"VideoOut", 0x80000022},
        {"AudioOut", 0x80000001},      {"Pad", 0x80000024},
    };
    for (const auto& [name, id] : modules) {
        int r = int(sceSysmoduleLoadModuleInternal(OrbisSysModuleInternal(id)));
        if (r != 0) nx::plat::trace("loading system module %s failed (%x)", name, unsigned(r));
    }
}

std::string app_version() {
    char buf[32]{};
    if (FILE* f = std::fopen("/app0/version.txt", "r")) {
        if (!std::fgets(buf, sizeof buf, f)) buf[0] = 0;
        std::fclose(f);
    }
    std::string v(buf);
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
    return v.empty() ? "?" : v;
}

// /data/SMW/config.txt: NX_<KEY>=<value> per line ('#' starts a comment) into the environment.
void load_config() {
    FILE* f = std::fopen(NX_PS4_DATA "/config.txt", "r");
    if (!f) return;
    char line[512];
    int n = 0;
    while (std::fgets(line, sizeof line, f)) {
        std::string s(line);
        if (auto hash = s.find('#'); hash != std::string::npos) s.resize(hash);
        auto eq = s.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string t) {
            size_t a = t.find_first_not_of(" \t\r\n"), b = t.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
        };
        std::string key = trim(s.substr(0, eq)), value = trim(s.substr(eq + 1));
        if (key.empty()) continue;
        setenv(key.c_str(), value.c_str(), 1);
        nx::plat::trace("config: %s=%s", key.c_str(), value.c_str());
        n++;
    }
    std::fclose(f);
    // svc_am.cpp only asks whether NX_HANDHELD exists: NX_HANDHELD=0 means docked, so it must go (as on Android).
    if (const char* h = std::getenv("NX_HANDHELD"); h && h[0] == '0') unsetenv("NX_HANDHELD");
    nx::plat::trace("config: %d settings from " NX_PS4_DATA "/config.txt", n);
}

void game_thread(void*) {
    try {
        nx::boot();
    } catch (const nx::LongJmp& j) {
        nx::fatal("longjmp to buffer %llx with no setjmp frame", (unsigned long long)j.buf);
    } catch (const std::exception& e) {
        // Where the game was when a host call threw: the guest registers still hold the call that failed.
        nx::CPU& c = nx::current_cpu();
        nx::log("host exception in a host call from the guest; guest lr %s", nx::describe(c.x[30]).c_str());
        nx::backtrace(c);
        size_t flex = 0;
        ps4_available_flexible_main(&flex);
        nx::log("flexible memory free: %zu MiB", flex >> 20);
        nx::fatal("host exception: %s", e.what());
    }
    nx::plat::exit_process(0);
}

}  // namespace

// The game's view of a crash, appended to crash.log by port/ps4_crashlog.cpp after the host's stacks: the guest
// registers of the crashed thread.
extern "C" void ps4_crash_guest_info() {
    const int fd = open(NX_PS4_DATA "/crash.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;
    char line[200];
    nx::CPU& c = nx::current_cpu();
    int n = std::snprintf(line, sizeof line, "\n== guest CPU at the crash: lr %016llx sp %016llx, lazy memory %llu MiB "
                                             "mapped, %llu chunk maps failed\n",
                          (unsigned long long)c.x[30], (unsigned long long)c.x[31],
                          (unsigned long long)(nx::plat::ps4_lazy_mapped_bytes() >> 20),
                          (unsigned long long)nx::plat::ps4_lazy_failed());
    write(fd, line, size_t(n));
    for (int k = 0; k < 32; k += 4) {
        n = std::snprintf(line, sizeof line, "x%-2d %016llx %016llx %016llx %016llx\n", k,
                          (unsigned long long)c.x[k], (unsigned long long)c.x[k + 1],
                          (unsigned long long)c.x[k + 2], (unsigned long long)c.x[k + 3]);
        write(fd, line, size_t(n));
    }
    std::string lr = nx::describe(c.x[30]);  // module+offset and the nearest symbol
    n = std::snprintf(line, sizeof line, "guest lr: %s\n", lr.c_str());
    write(fd, line, size_t(n));
    close(fd);
    // The guest call stack from the frame records (x29 chain), into nx.log: the lr alone does not say which caller
    // led there (Breath of the Wild's first crash was three virtual calls deep).
    nx::log("guest call stack at the crash:");
    nx::backtrace(c);
}

int main() {
    using namespace nx::plat;
    load_system_modules();
    mkdir(kDataRoot, 0777);
    trace(NX_PS4_APP " %s starting (%s)", app_version().c_str(), nx::game::kInfo.title);
    load_config();
    // PS4 defaults (config.txt overrides them): the game renders at 1280x720 and FidelityFX Super Resolution 1 (EASU,
    // then RCAS sharpening) brings it to the TV's 1080p in the present pass (nvn_present.cpp).
    setenv("NX_FSR", "1", 0);
    setenv("NX_RENDER_RES", "1280x720", 0);
    // Pipelined submits (the CPU does not wait for the GPU at the end of each frame, nvn_frames.cpp) and uniform
    // buffers bound with dynamic offsets (descriptor sets reused, nvn_pipeline.cpp): course 1-1 from 25-27 fps to 30.
    setenv("NX_PIPELINED", "1", 0);
    setenv("NX_DYNAMIC_UBO", "1", 0);
    // NX_PS4_GPU_FLUSH=1: every GPU cache written back and invalidated after every draw (slow). Tells a cache
    // coherency problem (data the CPU or a later pass reads before the GPU's L2 wrote it out) from the rest.
    if (const char* f = std::getenv("NX_PS4_GPU_FLUSH"); f && f[0] == '1') {
        ac_ps4_flush_all_draws = 1;
        trace("gpu: every cache flushed after every draw (NX_PS4_GPU_FLUSH)");
    }
    ps4::boot_screen_start();  // our own picture with a progress bar until the game's first frame
    if (profile_requested()) ps4_sampler_start();
    detect_cores();
    g_cross_confirms = !(std::getenv("NX_BUTTONS") && std::strcmp(std::getenv("NX_BUTTONS"), "nintendo") == 0);
    ps4::settings_load(g_cross_confirms);  // the quick menu's settings (ps4_menu.cpp): button mapping, FSR
    install_fault_handler();
    register_thread();
    // The host stacks' range (the guest's are reserved by nx::memory_init, on the game thread started below).
    if (!reserve(kStacksBase, lazy::kEnd - kStacksBase)) trace("memory: no range for the thread stacks");

    sceUserServiceInitialize(nullptr);
    scePadInit();
    scan_players();

    start_memory_guard();  // memguard_ps4.cpp
    trace("game thread");
    // The game on its own thread, with a big stack (recompiled calls nest deeply); the main thread polls the pad.
    start_thread(game_thread, nullptr, 64ull << 20);
    for (;;) {
        sample_input();
        sleep_ns(4'000'000);
    }
}
