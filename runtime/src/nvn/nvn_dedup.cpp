// Per-submit upload reuse (docs/nvn/DEBUG.md #30). A draw's buffers are copied from guest memory into the upload
// ring when it is replayed; the same camera block, scene constants and vertex buffers were copied again for every
// draw that bound them (~130 KB per draw, 50-70 MB per frame). Within one submit guest memory only changes through
// inline uniform updates (UpdateUniformBuffer, which invalidates what it overlaps) and compute results (ranges a
// dispatch writes are read from their written copy, nvn_written.cpp, and invalidated here): a copy made once can
// serve every later draw of the submit that binds the same bytes.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
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

// g_copies' nodes come from chunks that dedup_reset frees whole: thousands of nodes a submit each took a malloc, and a
// free at the reset (the allocator was ~2% of Breath of the Wild's replay thread). Erased nodes wait for the reset.
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
Arena g_arena;

template <class T>
struct ArenaAlloc {
    using value_type = T;
    ArenaAlloc() = default;
    template <class U>
    ArenaAlloc(const ArenaAlloc<U>&) {}
    T* allocate(size_t n) { return static_cast<T*>(g_arena.take(n * sizeof(T))); }
    void deallocate(T*, size_t) {}
    template <class U>
    bool operator==(const ArenaAlloc<U>&) const { return true; }
    template <class U>
    bool operator!=(const ArenaAlloc<U>&) const { return false; }
};

// By host address; several sizes of one address are rare, so the latest one is kept.
std::map<const uint8_t*, Entry, std::less<const uint8_t*>, ArenaAlloc<std::pair<const uint8_t* const, Entry>>> g_copies;
uint64_t g_copies_max = 0;  // the largest copy in g_copies (how far back an overlap can start)
uint64_t g_saved = 0, g_copied = 0;
uint64_t g_kind_copied[5] = {}, g_kind_saved[5] = {}, g_kind_calls[5] = {};

// In front of the map (a tree walk per bound buffer of every draw, and most are hits): a slot per address hash with a
// copy of that address's entry in g_copies. A slot of the current generation always equals the map's entry: both are
// written together, dedup_invalidate clears the slot of each entry it erases, dedup_reset starts a new generation.
struct Front {
    const uint8_t* p = nullptr;
    uint64_t generation = 0;
    Entry entry;
};
// 4096 slots: Breath of the Wild binds ~16,000 buffers a frame outdoors, and 1024 sent many hits to the tree walk.
Front g_front[4096];
uint64_t g_generation = 1;
Front& front_of(const uint8_t* p) { return g_front[(uintptr_t(p) >> 4 ^ uintptr_t(p) >> 14) & 4095]; }

}  // namespace

Upload dedup_upload(const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align) {
    if (!p || !size) {
        Upload u = upload_alloc(padded, align);
        std::memset(u.ptr, 0, padded);
        return u;
    }
    static const bool off = std::getenv("NX_NO_DEDUP") != nullptr;  // bisecting: a fresh copy every time
    Front& f = front_of(p);
    const Entry* e = nullptr;
    auto at = g_copies.end();  // where p is or would go, when the tree was walked: the insert below starts there
    if (!off && f.generation == g_generation && f.p == p) {
        e = &f.entry;  // the map's entry for p
    } else if (!off) {
        at = g_copies.lower_bound(p);
        if (at != g_copies.end() && at->first == p) {
            f = {p, g_generation, at->second};
            e = &f.entry;
        }
    }
    const int kind = g_upload_kind >= 0 && g_upload_kind < 5 ? g_upload_kind : 0;
    g_kind_calls[kind]++;
    if (e && (e->size == size || (g_upload_prefix_ok && e->size > size)) &&
        (e->padded == padded || (g_upload_prefix_ok && e->padded > padded)) && e->upload.offset % align == 0) {
        g_saved += padded, g_kind_saved[kind] += padded;
        return e->upload;
    }
    Upload u = upload_alloc(padded, align);
    std::memcpy(u.ptr, p, size);
    if (padded > size) std::memset(u.ptr + size, 0, padded - size);
    g_copies.insert_or_assign(at, p, Entry{size, padded, u});
    g_copies_max = std::max(g_copies_max, size);
    f = {p, g_generation, {size, padded, u}};
    g_copied += padded, g_kind_copied[kind] += padded;
    return u;
}

void dedup_reset() { g_copies.clear(), g_arena.reset(), g_generation++, g_copies_max = 0; }

void dedup_invalidate(const uint8_t* p, uint64_t size) {
    // Entries starting before p may still reach into it: look back over the few that can.
    auto it = g_copies.lower_bound(p + size);
    while (it != g_copies.begin()) {
        --it;
        if (it->first + it->second.size <= p) {
            // Nothing further back reaches p: no copy this submit is larger than g_copies_max. (A fixed 64 MB walked
            // back over every uniform block copied below p at each replayed UpdateUniformBuffer.)
            if (uint64_t(p - it->first) > g_copies_max) break;
            continue;
        }
        if (Front& f = front_of(it->first); f.p == it->first) f.generation = 0;
        it = g_copies.erase(it);
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

int g_upload_kind = 0;
bool g_upload_prefix_ok = false;
uint64_t dedup_copied_bytes() { return g_copied; }
uint64_t g_vertex_by_pool[8] = {}, g_uniform_by_pool[8] = {};  // vertex bytes copied, by pool flags & 7 (draw_inputs)

void dedup_log() {
    if (g_copied + g_saved)
        log("nvn: uploads copied %.1f MB, reused %.1f MB", double(g_copied) / 1e6, double(g_saved) / 1e6);
    static const char* names[5] = {"other", "vertex", "index", "uniform", "storage"};
    std::string line;
    for (int k = 0; k < 5; k++) {
        char b[96];
        std::snprintf(b, sizeof b, " %s %.1f/%.1f MB %llu calls,", names[k], double(g_kind_copied[k]) / 1e6,
                      double(g_kind_saved[k]) / 1e6, (unsigned long long)g_kind_calls[k]);
        line += b;
        g_kind_copied[k] = g_kind_saved[k] = g_kind_calls[k] = 0;
    }
    log("nvn: uploads by kind (copied/reused):%s", line.c_str());
    log("nvn: vertex copied by pool CPU access: none %.1f MB, uncached %.1f MB, cached %.1f MB, other %.1f MB",
        double(g_vertex_by_pool[1]) / 1e6, double(g_vertex_by_pool[2]) / 1e6, double(g_vertex_by_pool[4]) / 1e6,
        double(g_vertex_by_pool[0] + g_vertex_by_pool[3] + g_vertex_by_pool[5] + g_vertex_by_pool[6] + g_vertex_by_pool[7]) / 1e6);
    for (uint64_t& v : g_vertex_by_pool) v = 0;
    log("nvn: uniform copied by pool CPU access: none %.1f MB, uncached %.1f MB, cached %.1f MB, other %.1f MB",
        double(g_uniform_by_pool[1]) / 1e6, double(g_uniform_by_pool[2]) / 1e6, double(g_uniform_by_pool[4]) / 1e6,
        double(g_uniform_by_pool[0] + g_uniform_by_pool[3] + g_uniform_by_pool[5] + g_uniform_by_pool[6] + g_uniform_by_pool[7]) / 1e6);
    for (uint64_t& v : g_uniform_by_pool) v = 0;
    g_copied = g_saved = 0;
}

}  // namespace nx::nvn::replay
