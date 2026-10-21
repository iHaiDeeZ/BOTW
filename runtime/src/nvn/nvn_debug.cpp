// Debug readback of any NVN texture (docs/NVN.md): NX_DUMP_TEXTURES=<hex NVNtexture*>[,...] writes, at every frame
// NX_DUMP_FRAMES dumps, each listed texture's level 0 / layer 0 to <data root>/frames/NNNNN_<texture>.ppm. The image
// is blitted to RGBA8 first, so float and packed formats come out as the GPU would display them (clamped to 0..1).
// Runs under the GPU lock, after the frame's work completed, on its own command buffer.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nvn_vk.h"
#include "platform.h"

namespace nx::nvn {

using namespace vk;

VkQueue vk_queue();
uint32_t vk_queue_family();

namespace {

std::vector<uint64_t> dump_list() {
    std::vector<uint64_t> v;
    const char* e = std::getenv("NX_DUMP_TEXTURES");
    while (e && *e) {
        char* end = nullptr;
        uint64_t t = std::strtoull(e, &end, 16);
        if (end == e) break;
        v.push_back(t);
        e = *end == ',' ? end + 1 : end;
    }
    return v;
}

struct Scratch {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
    void destroy(VkDevice d) {
        if (image) p_vkDestroyImage(d, image, nullptr);
        if (image_memory) p_vkFreeMemory(d, image_memory, nullptr);
        if (buffer) p_vkDestroyBuffer(d, buffer, nullptr);
        if (buffer_memory) p_vkFreeMemory(d, buffer_memory, nullptr);
    }
};

bool make_scratch(Scratch& s, uint32_t w, uint32_t h, uint32_t texel_bytes = 4) {
    VkDevice d = vk_device();
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {w, h, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (p_vkCreateImage(d, &ii, nullptr, &s.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(d, s.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (p_vkAllocateMemory(d, &ai, nullptr, &s.image_memory) != VK_SUCCESS) return false;
    p_vkBindImageMemory(d, s.image, s.image_memory, 0);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = uint64_t(w) * h * texel_bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (p_vkCreateBuffer(d, &bi, nullptr, &s.buffer) != VK_SUCCESS) return false;
    p_vkGetBufferMemoryRequirements(d, s.buffer, &req);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (p_vkAllocateMemory(d, &ai, nullptr, &s.buffer_memory) != VK_SUCCESS) return false;
    p_vkBindBufferMemory(d, s.buffer, s.buffer_memory, 0);
    return true;
}

void write_ppm(const Scratch& s, uint32_t w, uint32_t h, uint64_t frame, uint64_t texture) {
    void* p = nullptr;
    if (p_vkMapMemory(vk_device(), s.buffer_memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return;
    std::string dir = plat::data_root() + "/frames";
    plat::make_dirs(dir);
    char name[64];
    std::snprintf(name, sizeof name, "/%05llu_%llx.ppm", (unsigned long long)frame, (unsigned long long)texture);
    if (FILE* f = plat::fopen_utf8(dir + name, "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        const uint8_t* px = static_cast<const uint8_t*>(p);
        for (size_t k = 0; k < size_t(w) * h; k++) std::fwrite(px + k * 4, 1, 3, f);
        std::fclose(f);
    }
    p_vkUnmapMemory(vk_device(), s.buffer_memory);
}

}  // namespace

std::vector<uint64_t> g_dump_extra;  // this submit's (NX_DUMP_ON_FS)
void debug_dump_add(uint64_t texture) {
    if (texture && std::find(g_dump_extra.begin(), g_dump_extra.end(), texture) == g_dump_extra.end())
        g_dump_extra.push_back(texture);
}

void debug_dump_textures(uint64_t frame) {
    static const std::vector<uint64_t> fixed = dump_list();
    std::vector<uint64_t> list = fixed;
    list.insert(list.end(), g_dump_extra.begin(), g_dump_extra.end());
    g_dump_extra.clear();
    if (list.empty()) return;
    VkDevice d = vk_device();
    static VkCommandPool pool = VK_NULL_HANDLE;
    static VkCommandBuffer cb = VK_NULL_HANDLE;
    static VkFence fence = VK_NULL_HANDLE;
    if (!pool) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = vk_queue_family();
        p_vkCreateCommandPool(d, &pi, nullptr, &pool);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        p_vkAllocateCommandBuffers(d, &ai, &cb);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        p_vkCreateFence(d, &fi, nullptr, &fence);
    }
    for (uint64_t texture : list) {
        GpuImage* src = image_of(texture);
        log("nvn: dump %llx at %llu: %s", (unsigned long long)texture, (unsigned long long)frame,
            !src ? "no image" : src->layout == VK_IMAGE_LAYOUT_UNDEFINED ? "undefined" : "ok");
        if (!src || !src->image || src->layout == VK_IMAGE_LAYOUT_UNDEFINED || !(src->aspect & VK_IMAGE_ASPECT_COLOR_BIT))
            continue;
        uint32_t w = src->width, h = src->height;
        Scratch s;
        if (!make_scratch(s, w, h)) {
            s.destroy(d);
            continue;
        }
        p_vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        p_vkBeginCommandBuffer(cb, &bi);
        VkImageLayout back = src->layout;
        transition(cb, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        GpuImage dst;
        dst.image = s.image;
        dst.format = VK_FORMAT_R8G8B8A8_UNORM;
        transition(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (std::getenv("NX_DUMP_RAW")) {  // 4-byte texels as stored, to <frame>_<texture>.raw (w, h, vk format, data)
            transition(cb, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy r{};
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.imageExtent = {w, h, 1};
            p_vkCmdCopyImageToBuffer(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buffer, 1, &r);
            transition(cb, *src, back);
            p_vkEndCommandBuffer(cb);
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cb;
            p_vkResetFences(d, 1, &fence);
            void* p = nullptr;
            if (p_vkQueueSubmit(vk_queue(), 1, &si, fence) == VK_SUCCESS &&
                p_vkWaitForFences(d, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS &&
                p_vkMapMemory(d, s.buffer_memory, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) {
                char name[64];
                std::snprintf(name, sizeof name, "/frames/%05llu_%llx.raw", (unsigned long long)frame,
                              (unsigned long long)texture);
                if (FILE* f = plat::fopen_utf8(plat::data_root() + name, "wb")) {
                    uint32_t head[3] = {w, h, uint32_t(src->format)};
                    std::fwrite(head, 4, 3, f);
                    std::fwrite(p, 4, size_t(w) * h, f);
                    std::fclose(f);
                }
                p_vkUnmapMemory(d, s.buffer_memory);
            }
            s.destroy(d);
            continue;
        }
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(w), int32_t(h), 1};
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[1] = blit.srcOffsets[1];
        p_vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        transition(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {w, h, 1};
        p_vkCmdCopyImageToBuffer(cb, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buffer, 1, &r);
        transition(cb, *src, back);
        p_vkEndCommandBuffer(cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        p_vkResetFences(d, 1, &fence);
        if (p_vkQueueSubmit(vk_queue(), 1, &si, fence) == VK_SUCCESS) {
            p_vkWaitForFences(d, 1, &fence, VK_TRUE, UINT64_MAX);
            write_ppm(s, w, h, frame, texture);
        }
        s.destroy(d);
    }
}

// NX_DUMP_ON_TARGET / NX_DUMP_ON_FS (nvn_draw.cpp): the n-th requesting draw of a submit, every NX_DUMP_TARGET_EVERY-th
// (default NX_DUMP_FRAMES) is read back when the submit completes.
void debug_dump_requested(uint64_t n) {
    static const uint64_t every = [] {
        const char* e = std::getenv("NX_DUMP_TARGET_EVERY");
        return e ? std::strtoull(e, nullptr, 10) : uint64_t(frame_dump_every());
    }();
    if (n && every && n % every == 0) debug_dump_textures(n);
}

// ---- presented frames (NX_DUMP_FRAMES=N) ----------------------------------------------------------------------------
// Every Nth presented texture is read back and written to <data root>/frames/NNNNN.ppm (what the game drew,
// independent of the window). The copy is recorded into the present's command buffer, written after its fence.

uint32_t frame_dump_every() {
    static uint32_t n = [] {
        const char* e = std::getenv("NX_DUMP_FRAMES");
        return e ? uint32_t(std::strtoul(e, nullptr, 10)) : 0u;
    }();
    return n;
}

// NX_DUMP_FRAMES, or on demand while playing: a file named dump_frames in the data folder (optionally holding
// "<count> [<every>]", default 40 frames, every 2nd) dumps the next presented frames, then is deleted. The game runs
// at full speed until then, for glitches that only show in a long session (DEBUG-2 #77). Checked once a second.
bool frame_dump_due(uint64_t presents) {
    static uint64_t left = 0, every = 2, next_check = 0;
    if (presents >= next_check) {
        next_check = presents + 60;
        std::string path = plat::data_root() + "/dump_frames";
        if (FILE* f = plat::fopen_utf8(path, "rb")) {
            unsigned long long n = 40, e = 2;
            int got = std::fscanf(f, "%llu %llu", &n, &e);
            std::fclose(f);
            std::remove(path.c_str());
            left = got >= 1 && n ? n : 40, every = got >= 2 && e ? e : 2;
            log("dump_frames: the next %llu frames, every %llu (from present %llu)", (unsigned long long)left,
                (unsigned long long)every, (unsigned long long)presents);
        }
    }
    if (left && presents % every == 0) return left--, true;
    return frame_dump_every() && presents % frame_dump_every() == 0;
}

namespace {

struct Readback {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint64_t size = 0;
};

Readback g_readback;

}  // namespace

bool frame_dump_record(VkCommandBuffer cb, GpuImage& img) {
    VkDevice d = vk_device();
    uint64_t size = uint64_t(img.width) * img.height * 4;
    if (img.format != VK_FORMAT_R8G8B8A8_UNORM && img.format != VK_FORMAT_R8G8B8A8_SRGB &&
        img.format != VK_FORMAT_B8G8R8A8_UNORM)
        return false;
    if (g_readback.size < size) {
        if (g_readback.buffer) p_vkDestroyBuffer(d, g_readback.buffer, nullptr);
        if (g_readback.memory) p_vkFreeMemory(d, g_readback.memory, nullptr);
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        p_vkCreateBuffer(d, &bi, nullptr, &g_readback.buffer);
        VkMemoryRequirements req{};
        p_vkGetBufferMemoryRequirements(d, g_readback.buffer, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex =
            memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        p_vkAllocateMemory(d, &ai, nullptr, &g_readback.memory);
        p_vkBindBufferMemory(d, g_readback.buffer, g_readback.memory, 0);
        g_readback.size = size;
    }
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {img.width, img.height, 1};
    p_vkCmdCopyImageToBuffer(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_readback.buffer, 1, &r);
    return true;
}

void frame_dump_write(const GpuImage& img, uint64_t frame) {
    void* p = nullptr;
    if (p_vkMapMemory(vk_device(), g_readback.memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return;
    std::string dir = plat::data_root() + "/frames";
    plat::make_dirs(dir);
    char name[64];
    std::snprintf(name, sizeof name, "/%05llu.ppm", (unsigned long long)frame);
    if (FILE* f = plat::fopen_utf8(dir + name, "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", img.width, img.height);
        const uint8_t* px = static_cast<const uint8_t*>(p);
        bool bgra = img.format == VK_FORMAT_B8G8R8A8_UNORM;
        std::vector<uint8_t> row(size_t(img.width) * 3);
        for (uint32_t y = 0; y < img.height; y++) {
            for (uint32_t x = 0; x < img.width; x++) {
                const uint8_t* s = px + (size_t(y) * img.width + x) * 4;
                row[x * 3 + 0] = bgra ? s[2] : s[0];
                row[x * 3 + 1] = s[1];
                row[x * 3 + 2] = bgra ? s[0] : s[2];
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
    }
    p_vkUnmapMemory(vk_device(), g_readback.memory);
}

// ---- GPU profile (NX_GPU_PROFILE=1) --------------------------------------------------------------------------------
// A timestamp at the start of every render pass and at the end of each submit; the time between two marks goes to
// the first one's label ("<target WxH> fs <hash>"). Totals per label are logged every 60 presents, largest first.
// Apple GPUs sample timestamps at encoder boundaries, which is what render passes are here.

namespace {

constexpr uint32_t kMarks = 1024;
VkQueryPool g_queries = VK_NULL_HANDLE;
std::vector<std::string> g_labels;
std::map<std::string, std::pair<double, uint64_t>> g_totals;  // label -> (ms, passes)
double g_period_ns = 1.0;

}  // namespace

bool gpu_profile_on() {
    static const bool on = std::getenv("NX_GPU_PROFILE") && std::atoi(std::getenv("NX_GPU_PROFILE")) == 1;
    return on;
}

void gpu_profile_begin(VkCommandBuffer cb) {
    if (!gpu_profile_on()) return;
    if (!g_queries) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kMarks;
        p_vkCreateQueryPool(vk_device(), &qi, nullptr, &g_queries);
        VkPhysicalDeviceProperties props{};
        p_vkGetPhysicalDeviceProperties(vk_physical(), &props);
        g_period_ns = props.limits.timestampPeriod;
    }
    p_vkCmdResetQueryPool(cb, g_queries, 0, kMarks);
    g_labels.clear();
}

void gpu_profile_mark(VkCommandBuffer cb, const std::string& label) {
    if (!gpu_profile_on() || !g_queries || g_labels.size() >= kMarks) return;
    p_vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_queries, uint32_t(g_labels.size()));
    g_labels.push_back(label);
}

void gpu_profile_collect() {  // after the submit's fence
    if (!gpu_profile_on() || g_labels.size() < 2) return;
    std::vector<uint64_t> t(g_labels.size());
    if (p_vkGetQueryPoolResults(vk_device(), g_queries, 0, uint32_t(t.size()), t.size() * 8, t.data(), 8,
                                VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    for (size_t k = 0; k + 1 < t.size(); k++) {
        auto& e = g_totals[g_labels[k]];
        e.first += double(t[k + 1] - t[k]) * g_period_ns / 1e6;
        e.second++;
    }
}

void gpu_profile_log(uint64_t frames) {
    if (!gpu_profile_on()) return;
    std::vector<std::pair<double, std::string>> v;
    for (auto& [label, e] : g_totals) v.push_back({e.first, label});
    std::sort(v.rbegin(), v.rend());
    double total = 0;
    for (auto& e : v) total += e.first;
    log("gpu profile: %8.2f ms/frame in all, %zu labels", total / double(frames), v.size());
    for (size_t k = 0; k < v.size() && k < 10; k++)
        log("gpu profile: %8.2f ms/frame  %s (%llu passes)", v[k].first / double(frames), v[k].second.c_str(),
            (unsigned long long)g_totals[v[k].second].second);
    g_totals.clear();
}

// ---- mid-frame snapshots (NX_SNAP_DRAWS=<draw>,<draw>,...) -------------------------------------------------------
// Right after each listed draw (numbers as NX_TRACE_DRAWS prints them), colour target 0 is blitted to RGBA8 in the
// same command buffer; after the submit each copy is written to frames/snap_<draw>.ppm. Shows a frame being built.

namespace {

struct Snap {
    Scratch s;
    uint32_t w = 0, h = 0;
    uint64_t draw = 0;
    uint32_t raw_format = 0;  // NX_SNAP_RAW: the texels as stored (4-byte formats), written as .raw
    std::string name;         // a storyboard picture (diag_pass): diag/<name>.ppm
    uint32_t depth_bytes = 0;  // a storyboard depth picture: texels of this size in the buffer, written as .pgm
    VkFormat float_format = VK_FORMAT_UNDEFINED;  // a storyboard float target (NX_DIAG_FLOAT): logged as statistics
};
std::vector<Snap> g_snaps;
std::set<uint64_t> g_diag_wanted;  // textures the storyboard frame sampled: pictured at the next frame's first pass end
std::string g_diag_tex_prefix;     // the folder under diag/ of the storyboard those textures belong to ("" or "<frame>/")
bool g_diag_textures_due = false;

}  // namespace

bool debug_snap_wanted(uint64_t draw) {
    static const std::vector<uint64_t> list = [] {
        std::vector<uint64_t> v;
        const char* e = std::getenv("NX_SNAP_DRAWS");
        while (e && *e) {
            char* end = nullptr;
            uint64_t n = std::strtoull(e, &end, 10);
            if (end == e) break;
            v.push_back(n);
            e = *end == ',' ? end + 1 : end;
        }
        return v;
    }();
    return std::find(list.begin(), list.end(), draw) != list.end();
}

void debug_snapshot(VkCommandBuffer cb, GpuImage& img, uint64_t draw) {
    if (!img.image || !(img.aspect & VK_IMAGE_ASPECT_COLOR_BIT)) return;
    Snap snap;
    snap.w = img.width, snap.h = img.height, snap.draw = draw;
    // NX_SNAP_CROP=x,y,w,h (fractions of the image): only that part (8 MB per whole 1080p snapshot otherwise).
    static float crop[4] = {0, 0, 1, 1};
    static const bool cropped = [] {
        const char* e = std::getenv("NX_SNAP_CROP");
        return e && std::sscanf(e, "%f,%f,%f,%f", &crop[0], &crop[1], &crop[2], &crop[3]) == 4;
    }();
    int32_t x0 = 0, y0 = 0;
    if (cropped) {
        x0 = int32_t(crop[0] * img.width), y0 = int32_t(crop[1] * img.height);
        snap.w = std::max(1u, std::min(uint32_t(crop[2] * img.width), img.width - uint32_t(x0)));
        snap.h = std::max(1u, std::min(uint32_t(crop[3] * img.height), img.height - uint32_t(y0)));
    }
    if (!make_scratch(snap.s, snap.w, snap.h)) {
        snap.s.destroy(vk_device());
        return;
    }
    VkImageLayout back = img.layout;
    transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    // NX_SNAP_RAW=1: 4-byte formats (R32F depth copies, RG16F, ...) as stored, to frames/00000_<draw>.raw (u32 width,
    // height, VkFormat, then the texels): an RGBA8 blit rounds small floats to 0..2.
    static const bool raw = std::getenv("NX_SNAP_RAW") != nullptr;
    if (raw && (img.format == VK_FORMAT_R32_SFLOAT || img.format == VK_FORMAT_R16G16_SFLOAT ||
                img.format == VK_FORMAT_R32_UINT || img.format == VK_FORMAT_R8G8B8A8_UNORM)) {
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageOffset = {x0, y0, 0};
        r.imageExtent = {snap.w, snap.h, 1};
        p_vkCmdCopyImageToBuffer(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snap.s.buffer, 1, &r);
        if (back != VK_IMAGE_LAYOUT_UNDEFINED) transition(cb, img, back);
        snap.raw_format = uint32_t(img.format);
        g_snaps.push_back(snap);
        return;
    }
    GpuImage dst;
    dst.image = snap.s.image;
    dst.format = VK_FORMAT_R8G8B8A8_UNORM;
    transition(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {x0, y0, 0};
    blit.srcOffsets[1] = {x0 + int32_t(snap.w), y0 + int32_t(snap.h), 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[1] = {int32_t(snap.w), int32_t(snap.h), 1};
    p_vkCmdBlitImage(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snap.s.image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    transition(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {snap.w, snap.h, 1};
    p_vkCmdCopyImageToBuffer(cb, snap.s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snap.s.buffer, 1, &r);
    if (back != VK_IMAGE_LAYOUT_UNDEFINED) transition(cb, img, back);
    g_snaps.push_back(snap);
}

bool debug_snapshots_pending() { return !g_snaps.empty(); }

namespace {

void diag_texture_level(VkCommandBuffer cb, GpuImage& img, uint64_t texture, uint32_t level) {
    uint32_t lw = std::max(1u, img.width >> level), lh = std::max(1u, img.height >> level);
    Snap snap;
    bool depth = img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT;
    char name[160];
    std::snprintf(name, sizeof name, "tex_%llx_%ux%u_f%d_l%u_L%u%s", (unsigned long long)texture, img.width, img.height,
                  int(img.format), level, img.layers, img.rendered ? "_rt" : "");
    snap.name = g_diag_tex_prefix + name;
    VkImageLayout back = img.layout;
    if (depth) {
        snap.depth_bytes = img.format == VK_FORMAT_D16_UNORM || img.format == VK_FORMAT_D16_UNORM_S8_UINT ? 2 : 4;
        snap.w = lw, snap.h = lh;
        if (!make_scratch(snap.s, lw, lh)) return snap.s.destroy(vk_device());
        transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, level, 0, 1};
        r.imageExtent = {lw, lh, 1};
        p_vkCmdCopyImageToBuffer(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snap.s.buffer, 1, &r);
    } else {
        snap.w = std::min(lw, 480u);
        snap.h = std::max(1u, uint32_t(uint64_t(lh) * snap.w / lw));
        if (!make_scratch(snap.s, snap.w, snap.h)) return snap.s.destroy(vk_device());
        transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        GpuImage dst;
        dst.image = snap.s.image;
        dst.format = VK_FORMAT_R8G8B8A8_UNORM;
        transition(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        blit.srcOffsets[1] = {int32_t(lw), int32_t(lh), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {int32_t(snap.w), int32_t(snap.h), 1};
        p_vkCmdBlitImage(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snap.s.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        transition(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {snap.w, snap.h, 1};
        p_vkCmdCopyImageToBuffer(cb, snap.s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snap.s.buffer, 1, &r);
    }
    if (back != VK_IMAGE_LAYOUT_UNDEFINED) transition(cb, img, back);
    g_snaps.push_back(std::move(snap));
}

}  // namespace

// Outside a render pass (nvn_draw.cpp pass end): the storyboard frame's sampled textures, once.
void diag_textures(VkCommandBuffer cb) {
    if (!g_diag_textures_due) return;
    g_diag_textures_due = false;
    int n = 0;
    for (uint64_t texture : g_diag_wanted)
        if (GpuImage* img = image_of(texture); img && img->image) {
            diag_texture_level(cb, *img, texture, 0);
            if (img->levels > 1 && !(img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) diag_texture_level(cb, *img, texture, 1);
            n++;
        }
    log("diag: %d sampled textures pictured (diag/tex_*)", n);
    g_diag_wanted.clear();
}

// ---- storyboard of one frame (diag_request; PS4: L3 + R3 held, or a file named diag_now in the data folder) -----
// Every colour target of every pass of the next frame, scaled to 480 pixels wide, as diag/<pass>_c<slot>_<size>_f
// <format>_d<draws>_<last fragment shader>.ppm, listed in diag/passes.txt: the first pass where a picture goes wrong.

namespace {
std::atomic<bool> g_diag_armed{false};
bool g_diag_on = false;
int g_diag_seq = 0;
FILE* g_diag_index = nullptr;
constexpr int kDiagMax = 400;
std::map<const GpuImage*, int> g_diag_pictures;  // pictures of each image in this storyboard
// NX_DIAG_KEEP=1: each storyboard in a folder of its own, diag/<frame>/, instead of replacing the last one: with
// NX_DIAG_EVERY one run keeps the title menu, a loading screen and gameplay (Breath of the Wild's menu text, missing
// on the PS4 only, was never on screen when L3 + R3 was pressed). Its sampled textures (~110 MB a storyboard) only
// with NX_DIAG_TEXTURES=1. g_diag_prefix: "<frame>/" or "" (the picture names' folder under diag/).
std::string g_diag_prefix;
bool diag_keep() {
    static const bool on = std::getenv("NX_DIAG_KEEP") != nullptr;
    return on;
}
}  // namespace

void diag_line(const std::string& line) {
    if (g_diag_on && g_diag_index) std::fprintf(g_diag_index, "%s\n", line.c_str());
}

void diag_want_texture(uint64_t texture) {
    static const bool textures = !diag_keep() || std::getenv("NX_DIAG_TEXTURES") != nullptr;
    if (textures && g_diag_on && texture && g_diag_wanted.size() < 300) g_diag_wanted.insert(texture);
}

void diag_request() { g_diag_armed = true; }
bool diag_active() { return g_diag_on; }

void diag_frame_boundary(uint64_t presents) {
    if (g_diag_on) {
        g_diag_on = false;
        g_diag_textures_due = !g_diag_wanted.empty();
        g_diag_tex_prefix = g_diag_prefix;
        if (g_diag_index) std::fclose(g_diag_index), g_diag_index = nullptr;
        log("diag: storyboard of frame %llu: %d pictures in %s/diag/%s", (unsigned long long)presents, g_diag_seq,
            plat::data_root().c_str(), g_diag_prefix.c_str());
    }
    // NX_DIAG_EVERY=<presents>: a storyboard every that many presents (each replaces the last), for a scene that is
    // hard to reach with the menu open (Tears of the Kingdom's black world).
    static const uint64_t every = [] {
        const char* e = std::getenv("NX_DIAG_EVERY");
        return e ? std::strtoull(e, nullptr, 10) : 0ull;
    }();
    if (every && presents && presents % every == 0) g_diag_armed = true;
    if (presents % 60 == 0) {  // the file request
        std::string req = plat::data_root() + "/diag_now";
        if (FILE* f = plat::fopen_utf8(req, "rb")) {
            std::fclose(f);
            std::remove(req.c_str());
            g_diag_armed = true;
        }
    }
    if (g_diag_armed.exchange(false)) {
        std::string dir = plat::data_root() + "/diag";
        g_diag_prefix.clear();
        if (diag_keep()) {
            dir += "/" + std::to_string(presents + 1);
            g_diag_prefix = std::to_string(presents + 1) + "/";
        }
        plat::make_dirs(dir);
        g_diag_index = plat::fopen_utf8(dir + "/passes.txt", "w");
        g_diag_seq = 0;
        g_diag_pictures.clear();
        g_diag_on = true;
        log("diag: storyboard of the next frame (after present %llu)", (unsigned long long)presents);
    }
}

void diag_pass(VkCommandBuffer cb, GpuImage& img, uint32_t slot, uint32_t draws, const std::string& fs,
               uint32_t level, uint32_t layer) {
    if (!g_diag_on || g_diag_seq >= kDiagMax || !img.image || !(img.aspect & VK_IMAGE_ASPECT_COLOR_BIT)) return;
    // The level and layer the pass rendered to, not always level 0 layer 0: Breath of the Wild renders its lighting
    // into the two layers of one RGBA16F array, each bound as its own target. The name gets L<layer> (an array) and
    // m<level> (not level 0) after the slot.
    level = std::min(level, img.levels ? img.levels - 1 : 0u);
    layer = std::min(layer, img.layers ? img.layers - 1 : 0u);
    const uint32_t lw = std::max(1u, img.width >> level), lh = std::max(1u, img.height >> level);
    char sub[24] = "";
    if (img.layers > 1) std::snprintf(sub, sizeof sub, "L%u", layer);
    if (level) std::snprintf(sub + std::strlen(sub), sizeof sub - std::strlen(sub), "m%u", level);
    // A pass of one or two draws on an image pictured that many times already is left out: Breath of the Wild draws
    // its decals one per 8-target pass (62 in the shrine), and the 400 pictures ran out before the lighting, after a
    // 20 s capture. NX_DIAG_PER_IMAGE=<n> (default 4, 0: every pass).
    static const int per_image = [] {
        const char* e = std::getenv("NX_DIAG_PER_IMAGE");
        return e ? std::atoi(e) : 4;
    }();
    int& pictured = g_diag_pictures[&img];
    if (per_image > 0 && draws <= 2 && pictured >= per_image) return;
    pictured++;
    Snap snap;
    snap.w = std::min(lw, 480u);
    snap.h = std::max(1u, uint32_t(uint64_t(lh) * snap.w / lw));
    char name[160];
    std::snprintf(name, sizeof name, "%03d_c%u%s_%ux%u_f%d_d%u_%s", g_diag_seq, slot, sub, img.width, img.height,
                  int(img.format), draws, fs.empty() ? "-" : fs.c_str());
    snap.name = g_diag_prefix + name;
    if (g_diag_index) std::fprintf(g_diag_index, "%s\n", name);
    g_diag_seq++;
    // NX_DIAG_FLOAT=1: float targets read back as stored, whole level 0, and logged per channel (min, max, mean, share
    // not zero): the RGBA8 pictures round HDR lighting and linear depth to 0..2.
    static const bool floats = std::getenv("NX_DIAG_FLOAT") != nullptr;
    if (floats && (img.format == VK_FORMAT_R16G16B16A16_SFLOAT || img.format == VK_FORMAT_R32_SFLOAT ||
                   img.format == VK_FORMAT_R16G16_SFLOAT || img.format == VK_FORMAT_R8G8B8A8_UNORM)) {
        snap.w = lw, snap.h = lh, snap.float_format = img.format;
        uint32_t bytes = img.format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
        if (!make_scratch(snap.s, snap.w, snap.h, bytes)) return snap.s.destroy(vk_device());
        VkImageLayout back = img.layout;
        transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1};
        r.imageExtent = {snap.w, snap.h, 1};
        p_vkCmdCopyImageToBuffer(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snap.s.buffer, 1, &r);
        if (back != VK_IMAGE_LAYOUT_UNDEFINED) transition(cb, img, back);
        g_snaps.push_back(std::move(snap));
        return;
    }
    if (!make_scratch(snap.s, snap.w, snap.h)) {
        snap.s.destroy(vk_device());
        return;
    }
    VkImageLayout back = img.layout;
    transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    GpuImage dst;
    dst.image = snap.s.image;
    dst.format = VK_FORMAT_R8G8B8A8_UNORM;
    transition(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1};
    blit.srcOffsets[1] = {int32_t(lw), int32_t(lh), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {int32_t(snap.w), int32_t(snap.h), 1};
    p_vkCmdBlitImage(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snap.s.image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    transition(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {snap.w, snap.h, 1};
    p_vkCmdCopyImageToBuffer(cb, snap.s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snap.s.buffer, 1, &r);
    if (back != VK_IMAGE_LAYOUT_UNDEFINED) transition(cb, img, back);
    g_snaps.push_back(std::move(snap));
}

// NX_DIAG_DEPTH=1: the storyboard also reads back each pass's depth target at its end (logged as "diag: depth
// <seq>_d_..."): what depth EQUAL draws were tested against (Tears of the Kingdom's material-ID lighting).
void diag_pass_depth(VkCommandBuffer cb, GpuImage& img, uint64_t texture, uint32_t draws, const std::string& fs) {
    static const bool on = std::getenv("NX_DIAG_DEPTH") != nullptr;
    if (!on || !g_diag_on || g_diag_seq >= kDiagMax || !img.image || !(img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) return;
    char name[160];
    std::snprintf(name, sizeof name, "%03d_d_%llx_d%u_%s", g_diag_seq++, (unsigned long long)texture, draws,
                  fs.empty() ? "-" : fs.c_str());
    if (g_diag_index) std::fprintf(g_diag_index, "%s\n", name);
    size_t before = g_snaps.size();
    diag_texture_level(cb, img, texture, 0);
    if (g_snaps.size() > before) g_snaps.back().name = g_diag_prefix + name;
}

void debug_snapshots_write() {  // after the submit's fence
    for (Snap& snap : g_snaps) {
        void* p = nullptr;
        if (snap.raw_format && p_vkMapMemory(vk_device(), snap.s.buffer_memory, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) {
            char name[64];
            std::snprintf(name, sizeof name, "/frames/00000_%llx.raw", (unsigned long long)snap.draw);
            if (FILE* f = plat::fopen_utf8(plat::data_root() + name, "wb")) {
                uint32_t head[3] = {snap.w, snap.h, snap.raw_format};
                std::fwrite(head, 4, 3, f);
                std::fwrite(p, 4, size_t(snap.w) * snap.h, f);
                std::fclose(f);
            }
            p_vkUnmapMemory(vk_device(), snap.s.buffer_memory);
        } else if (snap.float_format) {  // a storyboard float target: statistics per channel
            if (p_vkMapMemory(vk_device(), snap.s.buffer_memory, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) {
                auto half = [](uint16_t h) {
                    uint32_t e = h >> 10 & 0x1F, m = h & 0x3FF;
                    float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? (m ? NAN : INFINITY)
                                                                       : std::ldexp(float(m | 0x400), int(e) - 25);
                    return h & 0x8000 ? -v : v;
                };
                const bool unorm8 = snap.float_format == VK_FORMAT_R8G8B8A8_UNORM;  // flags in the bytes too
                const uint32_t channels = snap.float_format == VK_FORMAT_R16G16B16A16_SFLOAT || unorm8 ? 4
                                          : snap.float_format == VK_FORMAT_R16G16_SFLOAT              ? 2
                                                                                                      : 1;
                const size_t n = size_t(snap.w) * snap.h;
                std::string line;
                for (uint32_t c = 0; c < channels; c++) {
                    double lo = 1e30, hi = -1e30, sum = 0;
                    size_t nonzero = 0, nan = 0;
                    for (size_t k = 0; k < n; k++) {
                        float v;
                        if (unorm8) {
                            v = static_cast<const uint8_t*>(p)[k * 4 + c] / 255.0f;
                        } else if (snap.float_format == VK_FORMAT_R32_SFLOAT) {
                            std::memcpy(&v, static_cast<const uint8_t*>(p) + k * 4, 4);
                        } else {
                            uint16_t h;
                            std::memcpy(&h, static_cast<const uint8_t*>(p) + (k * channels + c) * 2, 2);
                            v = half(h);
                        }
                        if (v != v) {
                            nan++;
                            continue;
                        }
                        lo = std::min(lo, double(v)), hi = std::max(hi, double(v)), sum += v, nonzero += v != 0;
                    }
                    char b[120];
                    std::snprintf(b, sizeof b, " %c %.3g..%.3g mean %.3g nz %.0f%%%s", "RGBA"[c], lo, hi, sum / n,
                                  100.0 * nonzero / n, nan ? " NaN" : "");
                    line += b;
                }
                if (unorm8) {  // the share of texels with each low bit of the alpha byte set (G-buffer flags)
                    size_t bits[4] = {};
                    for (size_t k = 0; k < n; k++)
                        for (int b = 0; b < 4; b++) bits[b] += static_cast<const uint8_t*>(p)[k * 4 + 3] >> b & 1;
                    char b[96];
                    std::snprintf(b, sizeof b, " | A bits 1 %.0f%% 2 %.0f%% 4 %.0f%% 8 %.0f%%", 100.0 * bits[0] / n,
                                  100.0 * bits[1] / n, 100.0 * bits[2] / n, 100.0 * bits[3] / n);
                    line += b;
                }
                log("diag: float %s:%s", snap.name.c_str(), line.c_str());
                p_vkUnmapMemory(vk_device(), snap.s.buffer_memory);
            }
        } else if (snap.depth_bytes) {  // a storyboard depth picture
            if (p_vkMapMemory(vk_device(), snap.s.buffer_memory, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) {
                size_t n = size_t(snap.w) * snap.h;
                std::vector<float> v(n);
                for (size_t k = 0; k < n; k++) {
                    if (snap.depth_bytes == 2) {
                        uint16_t x;
                        std::memcpy(&x, static_cast<const uint8_t*>(p) + k * 2, 2);
                        v[k] = x / 65535.0f;
                    } else {
                        std::memcpy(&v[k], static_cast<const uint8_t*>(p) + k * 4, 4);
                    }
                }
                float lo = 1e30f, hi = -1e30f;
                for (float x : v)
                    if (x == x) lo = std::min(lo, x), hi = std::max(hi, x);
                if (FILE* f = plat::fopen_utf8(plat::data_root() + "/diag/" + snap.name + ".pgm", "wb")) {
                    std::fprintf(f, "P5\n%u %u\n255\n", snap.w, snap.h);
                    std::vector<uint8_t> row(snap.w);
                    for (uint32_t y = 0; y < snap.h; y++) {
                        for (uint32_t x = 0; x < snap.w; x++) {
                            float d = v[size_t(y) * snap.w + x];
                            row[x] = d == d && hi > lo ? uint8_t(std::clamp((d - lo) / (hi - lo), 0.0f, 1.0f) * 255) : 0;
                        }
                        std::fwrite(row.data(), 1, row.size(), f);
                    }
                    std::fclose(f);
                }
                log("diag: depth %s: %g .. %g", snap.name.c_str(), double(lo), double(hi));
                p_vkUnmapMemory(vk_device(), snap.s.buffer_memory);
            }
        } else if (!snap.name.empty()) {  // a storyboard picture
            if (p_vkMapMemory(vk_device(), snap.s.buffer_memory, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) {
                if (FILE* f = plat::fopen_utf8(plat::data_root() + "/diag/" + snap.name + ".ppm", "wb")) {
                    std::fprintf(f, "P6\n%u %u\n255\n", snap.w, snap.h);
                    const uint8_t* px = static_cast<const uint8_t*>(p);
                    std::vector<uint8_t> row(size_t(snap.w) * 3);
                    for (uint32_t y = 0; y < snap.h; y++) {
                        for (uint32_t x = 0; x < snap.w; x++)
                            std::memcpy(&row[x * 3], px + (size_t(y) * snap.w + x) * 4, 3);
                        std::fwrite(row.data(), 1, row.size(), f);
                    }
                    std::fclose(f);
                }
                p_vkUnmapMemory(vk_device(), snap.s.buffer_memory);
            }
        } else if (!snap.raw_format) {
            write_ppm(snap.s, snap.w, snap.h, 0, snap.draw);  // frames/00000_<draw in hex>.ppm
        }
        snap.s.destroy(vk_device());
    }
    g_snaps.clear();
}

// ---- draws that produce no pixels (NX_ZERO_DRAWS=1) ------------------------------------------------------------------
// Every draw runs inside an occlusion query; after each submit the samples passed are added up per (vertex, fragment)
// shader pair. Every 300 presents the pairs whose draws never produced a pixel are logged, most drawn first: what the
// scene tries to show and we lose (culled, depth-failed, off screen, or broken vertex data).

namespace {

constexpr uint32_t kZeroQueries = 8192;
VkQueryPool g_zero_pool = VK_NULL_HANDLE;
std::vector<std::string> g_zero_labels;  // this submit's queries
struct ZeroStat {
    uint64_t draws = 0, samples = 0;
};
std::map<std::string, ZeroStat> g_zero_stats;

}  // namespace

bool zero_draws_on() {
    static const bool on = std::getenv("NX_ZERO_DRAWS") != nullptr;
    return on;
}

void zero_draws_begin_submit(VkCommandBuffer cb) {
    if (!zero_draws_on()) return;
    if (!g_zero_pool) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_OCCLUSION;
        qi.queryCount = kZeroQueries;
        p_vkCreateQueryPool(vk_device(), &qi, nullptr, &g_zero_pool);
    }
    p_vkCmdResetQueryPool(cb, g_zero_pool, 0, kZeroQueries);
    g_zero_labels.clear();
}

// Around one draw call (inside the render pass). Returns false when this submit's queries are used up.
bool zero_draws_begin(VkCommandBuffer cb, const std::string& label) {
    if (!zero_draws_on() || !g_zero_pool || g_zero_labels.size() >= kZeroQueries) return false;
    p_vkCmdBeginQuery(cb, g_zero_pool, uint32_t(g_zero_labels.size()), 0);
    g_zero_labels.push_back(label);
    return true;
}

void zero_draws_end(VkCommandBuffer cb) { p_vkCmdEndQuery(cb, g_zero_pool, uint32_t(g_zero_labels.size() - 1)); }

void zero_draws_collect() {  // after the submit's fence
    if (!zero_draws_on() || g_zero_labels.empty()) return;
    std::vector<uint64_t> r(g_zero_labels.size());
    if (p_vkGetQueryPoolResults(vk_device(), g_zero_pool, 0, uint32_t(r.size()), r.size() * 8, r.data(), 8,
                                VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    for (size_t k = 0; k < r.size(); k++) {
        ZeroStat& z = g_zero_stats[g_zero_labels[k]];
        z.draws++, z.samples += r[k];
    }
}

void zero_draws_log() {
    if (!zero_draws_on()) return;
    std::vector<std::pair<uint64_t, std::string>> zero;
    for (auto& [label, z] : g_zero_stats)
        if (z.samples == 0) zero.push_back({z.draws, label});
    std::sort(zero.rbegin(), zero.rend());
    log("zero draws: %zu of %zu shader pairs produced no pixel", zero.size(), g_zero_stats.size());
    for (size_t k = 0; k < zero.size() && k < 25; k++)
        log("zero draws: %6llu x %s", (unsigned long long)zero[k].first, zero[k].second.c_str());
    g_zero_stats.clear();
}

}  // namespace nx::nvn
