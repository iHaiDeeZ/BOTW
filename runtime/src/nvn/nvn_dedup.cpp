// Per-submit upload reuse (docs/nvn/DEBUG.md #30). A draw's buffers are copied from guest memory into the upload
// ring when it is replayed; the same camera block, scene constants and vertex buffers were copied again for every
// draw that bound them (~130 KB per draw, 50-70 MB per frame). Within one submit guest memory only changes through
// inline uniform updates (UpdateUniformBuffer, which invalidates what it overlaps) and compute results (ranges a
// dispatch writes are read from their written copy, nvn_written.cpp, and invalidated here): a copy made once can
// serve every later draw of the submit that binds the same bytes.
//
// Each thread that copies keeps copies of its own: the replay thread, and each worker recording parallel passes
// (nvn_parallel.cpp), which shares its copies between its own draws only. A thread drops its copies the next time it
// copies after the epoch moved on (dedup_reset: every submit, and a ring that wrapped).
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <cstdio>
#include <string>
#include <vector>

#include "nvn_draw_state.h"

namespace nx::nvn::replay {

namespace {

struct Entry {
    uint64_t size = 0, padded = 0;
    Upload upload;
};

// The copies' map nodes come from chunks that the reset frees whole: thousands of nodes a submit each took a malloc,
// and a free at the reset (the allocator was ~2% of Breath of the Wild's replay thread). Erased nodes wait for the
// reset.
struct Arena {
    std::vector<std::unique_ptr<char[]>> chunks;
    size_t used = 0, size = 0;
    void* take(size_t n) {
        n = (n + 15) & ~size_t(15);
        if (chunks.empty() || used + n > size) {
            size = std::max<size_t>(n, 256 << 10);
            chunks.emplace_back(new char[size]);
            used = 0;
        }
        void* p = chunks.back().get() + used;
        used += n;
        return p;
    }
    void reset() {  // the last chunk stays for the next submit
        if (chunks.size() > 1) chunks.erase(chunks.begin(), chunks.end() - 1);
        used = 0;
    }
};

template <class T>
struct ArenaAlloc {
    using value_type = T;
    Arena* arena = nullptr;
    explicit ArenaAlloc(Arena* a) : arena(a) {}
    template <class U>
    ArenaAlloc(const ArenaAlloc<U>& o) : arena(o.arena) {}
    T* allocate(size_t n) { return static_cast<T*>(arena->take(n * sizeof(T))); }
    void deallocate(T*, size_t) {}
    template <class U>
    bool operator==(const ArenaAlloc<U>& o) const { return arena == o.arena; }
    template <class U>
    bool operator!=(const ArenaAlloc<U>& o) const { return arena != o.arena; }
};

// In front of the map (a tree walk per bound buffer of every draw, and most are hits): a slot per address hash with a
// copy of that address's entry in the map. A slot of the current generation always equals the map's entry: both are
// written together, dedup_invalidate clears the slot of each entry it erases, a reset starts a new generation.
struct Front {
    const uint8_t* p = nullptr;
    uint64_t generation = 0;
    Entry entry;
};
// 4096 slots: Breath of the Wild binds ~16,000 buffers a frame outdoors, and 1024 sent many hits to the tree walk.
constexpr size_t kFront = 4096;

using Pair = std::pair<const uint8_t* const, Entry>;

struct Dedup {
    Arena arena;
    // By host address; several sizes of one address are rare, so the latest one is kept.
    std::map<const uint8_t*, Entry, std::less<const uint8_t*>, ArenaAlloc<Pair>> copies{ArenaAlloc<Pair>(&arena)};
    uint64_t copies_max = 0;  // the largest copy in the map (how far back an overlap can start)
    std::unique_ptr<Front[]> front{new Front[kFront]()};
    uint64_t generation = 1, epoch = 0;
    // Per log period.
    uint64_t saved = 0, copied = 0;
    uint64_t kind_copied[5] = {}, kind_saved[5] = {}, kind_calls[5] = {};
    UploadStats stats;
    Front& front_of(const uint8_t* p) { return front[(uintptr_t(p) >> 4 ^ uintptr_t(p) >> 14) & (kFront - 1)]; }
};

std::atomic<uint64_t> g_epoch{1};
std::mutex g_all_lock;
std::vector<Dedup*> g_all;  // every thread's, for dedup_invalidate and the log

Dedup& walk_dedup() {  // the replay thread's (and any other thread's but the workers')
    static Dedup* d = [] {
        auto* n = new Dedup;
        std::lock_guard lk(g_all_lock);
        g_all.push_back(n);
        return n;
    }();
    return *d;
}

thread_local Dedup* t_dedup = nullptr;  // a worker's own

void forget(Dedup& d) {
    d.copies.clear(), d.arena.reset(), d.generation++, d.copies_max = 0;
}

Dedup& dd() {
    Dedup& d = t_dedup ? *t_dedup : walk_dedup();
    uint64_t epoch = g_epoch.load(std::memory_order_relaxed);
    if (d.epoch != epoch) forget(d), d.epoch = epoch;
    return d;
}

}  // namespace

void dedup_thread_init() {
    if (t_dedup) return;
    t_dedup = new Dedup;
    std::lock_guard lk(g_all_lock);
    g_all.push_back(t_dedup);
}

Upload dedup_upload(const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align) {
    if (!p || !size) {
        Upload u = upload_alloc(padded, align);
        std::memset(u.ptr, 0, padded);
        return u;
    }
    static const bool off = std::getenv("NX_NO_DEDUP") != nullptr;  // bisecting: a fresh copy every time
    Dedup& d = dd();
    Front& f = d.front_of(p);
    const Entry* e = nullptr;
    auto at = d.copies.end();  // where p is or would go, when the tree was walked: the insert below starts there
    if (!off && f.generation == d.generation && f.p == p) {
        e = &f.entry;  // the map's entry for p
    } else if (!off) {
        at = d.copies.lower_bound(p);
        if (at != d.copies.end() && at->first == p) {
            f = {p, d.generation, at->second};
            e = &f.entry;
        }
    }
    const int kind = g_upload_kind >= 0 && g_upload_kind < 5 ? g_upload_kind : 0;
    d.kind_calls[kind]++;
    if (e && (e->size == size || (g_upload_prefix_ok && e->size > size)) &&
        (e->padded == padded || (g_upload_prefix_ok && e->padded > padded)) && e->upload.offset % align == 0) {
        d.saved += padded, d.kind_saved[kind] += padded;
        return e->upload;
    }
    Upload u = upload_alloc(padded, align);
    std::memcpy(u.ptr, p, size);
    if (padded > size) std::memset(u.ptr + size, 0, padded - size);
    d.copies.insert_or_assign(at, p, Entry{size, padded, u});
    d.copies_max = std::max(d.copies_max, size);
    f = {p, d.generation, {size, padded, u}};
    d.copied += padded, d.kind_copied[kind] += padded;
    return u;
}

void dedup_reset() { g_epoch.fetch_add(1, std::memory_order_relaxed); }

// Guest bytes the replay thread wrote: every thread's copies of them go (the workers are idle then: a parallel pass is
// flushed before such a write, nvn_draw_state.cpp).
void dedup_invalidate(const uint8_t* p, uint64_t size) {
    std::lock_guard lk(g_all_lock);
    const uint64_t epoch = g_epoch.load(std::memory_order_relaxed);
    for (Dedup* dp : g_all) {
        Dedup& d = *dp;
        if (d.epoch != epoch) continue;  // forgets everything at its next copy anyway
        // Entries starting before p may still reach into it: look back over the few that can.
        auto it = d.copies.lower_bound(p + size);
        while (it != d.copies.begin()) {
            --it;
            if (it->first + it->second.size <= p) {
                // Nothing further back reaches p: no copy this submit is larger than copies_max. (A fixed 64 MB
                // walked back over every uniform block copied below p at each replayed UpdateUniformBuffer.)
                if (uint64_t(p - it->first) > d.copies_max) break;
                continue;
            }
            if (Front& f = d.front_of(it->first); f.p == it->first) f.generation = 0;
            it = d.copies.erase(it);
        }
    }
}

// c[0], the NVN driver's constants, as far as shaders read them (docs/nvn/TRANSLATOR.md): buffer descriptors
// {u64 GPU address; u32 size} of what is bound, which compute shaders search to find the buffer an address falls in
// (they wrote garbage with a zero table), and the maximum point size.
std::array<uint8_t, 0x1000> driver_constants(ShaderStage stage, int st) {
    std::array<uint8_t, 0x1000> c0{};
    auto put = [&](uint32_t at, const Range& r) {
        if (at + 12 > c0.size()) return;
        std::memcpy(c0.data() + at, &r.address, 8);
        uint32_t size = uint32_t(std::min<uint64_t>(r.size, 0xFFFFFFFFu));
        std::memcpy(c0.data() + at + 8, &size, 4);
    };
    if (stage == ShaderStage::Compute) {
        for (int n = 0; n < 16; n++) put(0x210 + 0x10 * n, g_state.uniforms[st][n]);
        for (int n = 0; n < 16; n++) put(0x310 + 0x10 * n, g_state.storage[st][n]);
    } else {
        int hw = stage == ShaderStage::Fragment ? 4 : 0;  // Maxwell stage numbers: vertex 0, fragment 4
        for (int n = 0; n < 16; n++) put(0x110 + 0x100 * hw + 0x10 * n, g_state.storage[st][n]);
        float max_point = 64.0f;
        std::memcpy(c0.data() + 0x610, &max_point, 4);
    }
    return c0;
}

uint64_t dedup_copied_bytes() { return dd().copied; }
UploadStats& upload_stats() { return dd().stats; }

// Every thread's counts (the workers are idle between submits, when this runs).
void dedup_log() {
    std::lock_guard lk(g_all_lock);
    uint64_t copied = 0, saved = 0, kind_copied[5] = {}, kind_saved[5] = {}, kind_calls[5] = {};
    UploadStats s;
    for (Dedup* d : g_all) {
        copied += d->copied, saved += d->saved;
        for (int k = 0; k < 5; k++)
            kind_copied[k] += d->kind_copied[k], kind_saved[k] += d->kind_saved[k], kind_calls[k] += d->kind_calls[k];
        for (int k = 0; k < 8; k++)
            s.vertex_by_pool[k] += d->stats.vertex_by_pool[k], s.uniform_by_pool[k] += d->stats.uniform_by_pool[k];
        d->copied = d->saved = 0;
        for (int k = 0; k < 5; k++) d->kind_copied[k] = d->kind_saved[k] = d->kind_calls[k] = 0;
        d->stats = {};
    }
    if (copied + saved) log("nvn: uploads copied %.1f MB, reused %.1f MB", double(copied) / 1e6, double(saved) / 1e6);
    static const char* names[5] = {"other", "vertex", "index", "uniform", "storage"};
    std::string line;
    for (int k = 0; k < 5; k++) {
        char b[96];
        std::snprintf(b, sizeof b, " %s %.1f/%.1f MB %llu calls,", names[k], double(kind_copied[k]) / 1e6,
                      double(kind_saved[k]) / 1e6, (unsigned long long)kind_calls[k]);
        line += b;
    }
    log("nvn: uploads by kind (copied/reused):%s", line.c_str());
    const uint64_t* v = s.vertex_by_pool;
    log("nvn: vertex copied by pool CPU access: none %.1f MB, uncached %.1f MB, cached %.1f MB, other %.1f MB",
        double(v[1]) / 1e6, double(v[2]) / 1e6, double(v[4]) / 1e6, double(v[0] + v[3] + v[5] + v[6] + v[7]) / 1e6);
    const uint64_t* u = s.uniform_by_pool;
    log("nvn: uniform copied by pool CPU access: none %.1f MB, uncached %.1f MB, cached %.1f MB, other %.1f MB",
        double(u[1]) / 1e6, double(u[2]) / 1e6, double(u[4]) / 1e6, double(u[0] + u[3] + u[5] + u[6] + u[7]) / 1e6);
}

}  // namespace nx::nvn::replay
