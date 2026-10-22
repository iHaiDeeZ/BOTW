// Vertex data kept from frame to frame. Every draw's vertex buffers were copied from guest memory into the upload ring
// at replay time, once per submit (nvn_dedup.cpp): Tears of the Kingdom copied ~29 MB of vertex data a frame, 99% of it
// from CPU_CACHED memory pools, mostly meshes that never change, and on the PS4 that copying was a quarter of the
// replay (16 ms of ~62 at 1,550 draws a frame). Here a range of a CPU_CACHED pool is copied once into an arena of its
// own and bound from there until something can have changed it:
//   - the game flushed it (nvnMemoryPoolFlushMappedRange / nvnBufferFlushMappedRange: CPU writes to a cached pool
//     reach the GPU only through a flush, so a range the game rewrites is flushed),
//   - a GPU result was written back over it (apply_write_back), or the runtime wrote it (written_cpu_write),
//   - its pool was finalized.
// Those come from any thread (vcache_dirty): they are queued and applied before the next lookup. A range a dispatch of
// the current submit writes is not cached (read_upload binds the written copy). The arena is only appended to; when it
// is full the rest of the submit copies as before, and the next submit waits for the GPU once and starts it over.
// NX_VERTEX_CACHE=0: off. NX_VERTEX_CACHE_MB: its size (PS4 192, else 512). NX_VERTEX_CACHE_CHECK=1: every hit is
// compared with guest memory and a stale one counted and copied again (checking the invalidation, slow).
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "nvn_draw_state.h"

namespace nx::nvn {

namespace {

std::mutex g_dirty_lock;
std::vector<std::pair<uintptr_t, uint64_t>> g_dirty;  // ranges to drop (address, size); size 0: everything
std::atomic<bool> g_dirty_pending{false};

}  // namespace

void vcache_dirty(const void* p, uint64_t size) {
    if (!p && size) return;
    std::lock_guard lk(g_dirty_lock);
    g_dirty.push_back({reinterpret_cast<uintptr_t>(p), size});
    g_dirty_pending = true;
}

namespace replay {

using namespace nx::vk;

namespace {

struct Entry {
    uint64_t size = 0, padded = 0, offset = 0;
};
std::map<std::pair<const uint8_t*, uint64_t>, Entry> g_entries;  // by host address and size
uint64_t g_max_size = 0;  // the largest entry: how far back an overlap can start
VkBuffer g_arena = VK_NULL_HANDLE;
VkDeviceMemory g_arena_memory = VK_NULL_HANDLE;
uint8_t* g_arena_ptr = nullptr;
uint64_t g_arena_size = 0, g_arena_used = 0;
bool g_reset_pending = false, g_failed = false;
uint64_t g_hits = 0, g_hit_bytes = 0, g_misses = 0, g_miss_bytes = 0, g_dropped = 0, g_resets = 0, g_stale = 0;

bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("NX_VERTEX_CACHE");
        return !(e && e[0] == '0');
    }();
    return on;
}

bool make_arena() {
    if (g_arena || g_failed) return g_arena != VK_NULL_HANDLE;
    const char* e = std::getenv("NX_VERTEX_CACHE_MB");
#if defined(__ORBIS__)
    uint64_t mb = e ? std::strtoull(e, nullptr, 10) : 192;
#else
    uint64_t mb = e ? std::strtoull(e, nullptr, 10) : 512;
#endif
    VkDevice d = vk_device();
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = mb << 20;
    bi.usage = kUploadUsage;
    VkMemoryRequirements req{};
    if (!mb || p_vkCreateBuffer(d, &bi, nullptr, &g_arena) != VK_SUCCESS) return g_failed = true, false;
    p_vkGetBufferMemoryRequirements(d, g_arena, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    // Plain host-visible coherent memory: write-combined where that is the fast path for the GPU (PS4 Garlic); the CPU
    // only writes the arena.
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* p = nullptr;
    if (p_vkAllocateMemory(d, &ai, nullptr, &g_arena_memory) != VK_SUCCESS ||
        p_vkBindBufferMemory(d, g_arena, g_arena_memory, 0) != VK_SUCCESS ||
        p_vkMapMemory(d, g_arena_memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
        log("nvn/vk: vertex cache: no %llu MB arena: copying as before", (unsigned long long)mb);
        p_vkDestroyBuffer(d, g_arena, nullptr);
        if (g_arena_memory) p_vkFreeMemory(d, g_arena_memory, nullptr);
        g_arena = VK_NULL_HANDLE, g_arena_memory = VK_NULL_HANDLE;
        return g_failed = true, false;
    }
    g_arena_ptr = static_cast<uint8_t*>(p);
    g_arena_size = bi.size;
    log("nvn/vk: vertex cache: %llu MB arena", (unsigned long long)mb);
    return true;
}

void drop(const uint8_t* p, uint64_t size) {
    auto it = g_entries.lower_bound({p + size, 0});
    while (it != g_entries.begin()) {
        --it;
        const uint8_t* at = it->first.first;
        if (at + it->second.size <= p) {
            if (uint64_t(p - at) > g_max_size) break;  // nothing further back reaches p
            continue;
        }
        it = g_entries.erase(it), g_dropped++;
    }
}

void drain() {
    if (!g_dirty_pending.load(std::memory_order_acquire)) return;
    std::vector<std::pair<uintptr_t, uint64_t>> v;
    {
        std::lock_guard lk(g_dirty_lock);
        v.swap(g_dirty);
        g_dirty_pending = false;
    }
    for (auto [p, size] : v) {
        if (!size) {
            g_dropped += g_entries.size(), g_entries.clear();
            continue;
        }
        drop(reinterpret_cast<const uint8_t*>(p), size);
    }
}

// The replay workers recording parallel passes (nvn_parallel.cpp) bind vertex data too.
std::mutex g_bind_lock;

}  // namespace

bool vcache_bind(const uint8_t* p, uint64_t size, uint64_t padded, Upload& out) {
    std::lock_guard lk(g_bind_lock);
    if (!enabled() || !p || !size || g_reset_pending || !make_arena()) return false;
    drain();
    static const bool check = std::getenv("NX_VERTEX_CACHE_CHECK") != nullptr;
    if (auto it = g_entries.find({p, size}); it != g_entries.end()) {
        Entry& e = it->second;
        if (e.padded >= padded) {
            if (check && std::memcmp(g_arena_ptr + e.offset, p, size) != 0) {
                if (g_stale++ < 20)
                    log("nvn/vk: vertex cache: stale copy of %p (%llu bytes) found", static_cast<const void*>(p),
                        (unsigned long long)size);
                g_entries.erase(it);
            } else {
                g_hits++, g_hit_bytes += size;
                out = {g_arena, e.offset, g_arena_ptr + e.offset};
                return true;
            }
        } else {
            g_entries.erase(it);
        }
    }
    uint64_t at = (g_arena_used + 255) & ~255ull;
    if (at + padded > g_arena_size) {  // the next submit starts the arena over, after the GPU is done with it
        g_reset_pending = true;
        return false;
    }
    std::memcpy(g_arena_ptr + at, p, size);
    if (padded > size) std::memset(g_arena_ptr + at + size, 0, padded - size);
    g_arena_used = at + padded;
    g_entries[{p, size}] = {size, padded, at};
    g_max_size = std::max(g_max_size, size);
    g_misses++, g_miss_bytes += size;
    out = {g_arena, at, g_arena_ptr + at};
    return true;
}

void vcache_begin_submit() {
    if (!g_reset_pending) return;
    frame_wait_all();  // nothing in flight reads the arena any more
    g_entries.clear();
    g_arena_used = 0, g_max_size = 0;
    g_reset_pending = false;
    g_resets++;
}

void vcache_log() {
    if (!g_hits && !g_misses) return;
    log("nvn/vk: vertex cache: %llu hits (%.1f MB not copied), %llu copies (%.1f MB), %llu dropped, %llu resets, "
        "arena %.0f of %.0f MB, %zu entries%s",
        (unsigned long long)g_hits, double(g_hit_bytes) / 1e6, (unsigned long long)g_misses, double(g_miss_bytes) / 1e6,
        (unsigned long long)g_dropped, (unsigned long long)g_resets, double(g_arena_used) / 1048576.0,
        double(g_arena_size) / 1048576.0, g_entries.size(), g_stale ? " (stale copies found!)" : "");
    g_hits = g_hit_bytes = g_misses = g_miss_bytes = g_dropped = 0;
}

}  // namespace replay

}  // namespace nx::nvn
