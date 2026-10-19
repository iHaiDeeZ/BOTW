// Texture uploads (nvn_pipeline.h): guest pool memory into the host image, untiled, widened where the host stores a
// format wider than NVN does, and checked against what the copies read (docs/nvn/DEBUG.md #50).
#include <algorithm>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <memory>
#include <deque>
#include <condition_variable>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include "nvn_astc.h"
#define STB_DXT_STATIC
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"
#include "nvn_pipeline.h"
#include "nvn_tiling.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

// R11G11B10F (NVN 0x3f) is stored as RGBA16F (nvn_format.cpp). Its channels are unsigned floats with half's 5-bit
// exponent and bias 15, and 6 (R, G) or 5 (B) mantissa bits: each is a half with the low mantissa bits zero and no
// sign, so shifting it into place is exact (Inf and NaN included). Alpha is 1. Copying the 4-byte texels as they are
// read twice the data: rows side by side, bits reinterpreted, the bottom half from other uploads (the garbage band).
void widen_r11g11b10(const uint8_t* src, uint8_t* dst, uint64_t texels) {
    for (uint64_t k = 0; k < texels; k++) {
        uint32_t v;
        std::memcpy(&v, src + k * 4, 4);
        uint16_t h[4] = {uint16_t((v & 0x7FF) << 4), uint16_t(((v >> 11) & 0x7FF) << 4),
                         uint16_t(((v >> 22) & 0x3FF) << 5), 0x3C00};
        std::memcpy(dst + k * 8, h, 8);
    }
}

// NX_FULL_UPLOAD=1: a pitch-linear texture the CPU wrote part of is uploaded whole again (bisecting).
bool full_upload() {
    static const bool on = std::getenv("NX_FULL_UPLOAD") != nullptr;
    return on;
}

// Bytes from `base` to the end of what vkCmdCopyBufferToImage reads for `r`, at `bytes` per bw x bh block.
uint64_t copy_end(const VkBufferImageCopy& r, uint64_t base, uint32_t bw, uint32_t bh, uint32_t bytes) {
    uint64_t row_blocks = ((r.bufferRowLength ? r.bufferRowLength : r.imageExtent.width) + bw - 1) / bw;
    uint64_t image_rows = ((r.bufferImageHeight ? r.bufferImageHeight : r.imageExtent.height) + bh - 1) / bh;
    uint64_t w = (r.imageExtent.width + bw - 1) / bw, h = (r.imageExtent.height + bh - 1) / bh;
    uint64_t slices = uint64_t(r.imageExtent.depth) * r.imageSubresource.layerCount;
    if (!w || !h || !slices) return r.bufferOffset - base;
    return r.bufferOffset - base + ((slices - 1) * image_rows + (h - 1)) * row_blocks * bytes + w * bytes;
}

// The copies read at most `staged` bytes from `base`, counted at the host format's size. A copy past it reads other
// uploads of the submit (or past the buffer): garbage, so the image is cleared instead. NX_NO_UPLOAD_GUARD=1: the old
// way (bisecting).
bool copies_fit(const Texture& t, const VkBufferImageCopy* r, uint32_t n, uint64_t base, uint64_t staged, uint32_t bw,
                uint32_t bh, uint32_t bytes) {
    static const bool off = std::getenv("NX_NO_UPLOAD_GUARD") != nullptr;
    uint64_t need = 0;
    for (uint32_t k = 0; k < n; k++) need = std::max(need, copy_end(r[k], base, bw, bh, bytes));
    if (off || need <= staged) return true;
    static std::set<uint32_t> logged;  // once per format
    if (logged.insert(t.format).second)
        log("nvn/vk: upload short: texture %ux%u format %#x reads %llu bytes, %llu staged: cleared instead", t.width,
            t.height, t.format, (unsigned long long)need, (unsigned long long)staged);
    return false;
}

// One level layer of RGBA8 texels (w x h, rows back to back) as BC3 blocks (DXT5: 16 bytes per 4x4), the edge blocks
// padded with their last row and column. stb_dxt (public domain), normal quality: ~4 ms a megatexel on a Jaguar core.
void encode_bc3(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* out) {
    uint8_t block[64];
    for (uint32_t by = 0; by < h; by += 4)
        for (uint32_t bx = 0; bx < w; bx += 4) {
            for (uint32_t y = 0; y < 4; y++)
                for (uint32_t x = 0; x < 4; x++)
                    std::memcpy(block + (y * 4 + x) * 4,
                                rgba + (uint64_t(std::min(by + y, h - 1)) * w + std::min(bx + x, w - 1)) * 4, 4);
            stb_compress_dxt_block(out, block, 1, STB_DXT_NORMAL);
            out += 16;
        }
}

// ASTC on a GPU without it (nvn_format.cpp): the untiled blocks of every level become RGBA8 texels (and on the PS4
// then BC3 blocks, astc_bc3), and the levels
// are re-laid out for them (each level's layers back to back, 16-byte aligned). Logged every 200 textures: count,
// bytes and CPU time, the cost of the decoder at load.
void decode_astc(uint32_t format, std::vector<uint8_t>& linear, std::vector<LinearLevel>& levels, uint32_t layers,
                 uint32_t bw, uint32_t bh) {
    static uint64_t textures = 0, bytes = 0, ns = 0, invalid = 0;
    const uint64_t t0 = plat::monotonic_ns();
    const bool srgb = format >= 0x87;
    std::vector<LinearLevel> out = levels;
    uint64_t at = 0;
    for (LinearLevel& l : out) {
        l.offset = at;
        l.layer_size = uint64_t(l.width) * l.height * 4;
        l.width_blocks = l.width, l.height_blocks = l.height;
        at += (l.layer_size * layers + 15) & ~15ull;
    }
    std::vector<uint8_t> rgba(at);
    for (size_t k = 0; k < levels.size(); k++)
        if (levels[k].offset + levels[k].layer_size * layers <= linear.size())
            invalid += astc_decode_image(linear.data() + levels[k].offset, levels[k].width, levels[k].height, layers,
                                         bw, bh, srgb, rgba.data() + out[k].offset);
    if (astc_bc3()) {  // the texels -> BC3 blocks, every level and layer (nvn_format.cpp astc_bc3)
        std::vector<LinearLevel> bc = out;
        uint64_t bat = 0;
        for (LinearLevel& l : bc) {
            l.offset = bat;
            l.width_blocks = (l.width + 3) / 4, l.height_blocks = (l.height + 3) / 4;
            l.layer_size = uint64_t(l.width_blocks) * l.height_blocks * 16;
            bat += (l.layer_size * layers + 15) & ~15ull;
        }
        std::vector<uint8_t> blocks(bat);
        for (size_t k = 0; k < out.size(); k++)
            for (uint32_t z = 0; z < layers; z++)
                encode_bc3(rgba.data() + out[k].offset + out[k].layer_size * z, out[k].width, out[k].height,
                           blocks.data() + bc[k].offset + bc[k].layer_size * z);
        rgba.swap(blocks);
        out.swap(bc);
        at = bat;
    }
    linear.swap(rgba);
    levels.swap(out);
    const uint64_t spent = plat::monotonic_ns() - t0;
    note_astc_decoded(spent);  // per timing window (nvn_pipeline_cache.cpp)
    bytes += at, ns += spent;
    if (++textures % 200 == 1)
        log("nvn/vk: ASTC decoded on the CPU: %llu textures, %.1f MB, %.0f ms, %llu invalid blocks",
            (unsigned long long)textures, double(bytes) / 1e6, double(ns) / 1e6, (unsigned long long)invalid);
}

// Whether a 1 MB+ upload gets a staging buffer of its own (PHASE4.md U1). In steady state (a course frame stages ~30
// such textures, ~40 MB per second) the ring takes them: a fresh buffer each was an allocation (a kgsl ioctl that
// zero-fills), a page fault per 4 KB of the copy and a free at retire, on the replay thread. A scene load, which
// stages hundreds of MB in one submit, still gets its own buffers: past a quarter of the ring in this submit, or one
// upload over an eighth of it. NX_STAGING_MIN=0: a staging buffer for every 1 MB+ upload, as before.
bool staging_wanted(uint64_t size) {
    static const bool always = [] {
        const char* e = std::getenv("NX_STAGING_MIN");
        return e && e[0] == '0';
    }();
    uint64_t ring = frame_ring_size();
    return always || !ring || frame_upload_used() + size > ring / 4 || size > ring / 8;
}

// Room for a texture's texels on their way to the image. PS4: 1 MB and up get a staging buffer of their own, whatever
// the path: the ring is 64 MB there, and the glyph cache's first upload alone (16 MB of R8 widened to RGBA8) is 64 MiB.
// It wrapped the ring over the uploads before it in the same submit at course loads.
Upload texel_alloc(uint64_t size, uint64_t align) {
#if defined(__ORBIS__)
    if (size >= (1u << 20)) return staging_alloc(size);
#endif
    return upload_alloc(size, align);
}

}  // namespace

// NX_1D_IMAGES=1: NVN 1D textures as Vulkan 1D images again (they are 2D of height 1: Ryujinx samples 1D as 2D, G8).
bool vk_1d_images() {
    static const bool on = std::getenv("NX_1D_IMAGES") != nullptr;
    return on;
}

void clear_image(VkCommandBuffer cb, GpuImage& img, bool magenta) {
    transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceRange r{img.aspect, 0, img.levels, 0, img.layers};
    if (img.aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
        VkClearColorValue v{};
        if (magenta) v = {{1.0f, 0.0f, 1.0f, 1.0f}};
        p_vkCmdClearColorImage(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
    } else {
        VkClearDepthStencilValue v{1.0f, 0};  // far: the course passes test LESS
        p_vkCmdClearDepthStencilImage(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
    }
}

// ---- ASTC decoded in the background (NX_ASYNC_ASTC, on by default on the PS4) ----------------------------------------
// Decoding a texture's ASTC blocks to RGBA8 on the replay thread cost the PS4 1-2 s in a window of 60 frames when new
// objects came in (the frame dips of a course). A draw that may wait (into the frame's own targets) skips its frame
// instead while a worker untiles and decodes a copy of the texture's bytes (the decoder's own helpers join in);
// once done, the next upload takes the texels as they are. Other draws decode at once, as before.
namespace {
struct AstcJob {
    std::shared_ptr<GpuImage> image;  // kept alive while the job runs
    std::vector<uint8_t> raw, linear;
    std::vector<LinearLevel> levels;
    uint32_t format = 0, width = 0, height = 0, layers = 1, level_count = 1, bw = 1, bh = 1, bytes = 4, gobs = 0;
    std::atomic<bool> done{false};
};
std::unordered_map<const GpuImage*, std::shared_ptr<AstcJob>> g_astc_jobs;  // the replay thread's
std::mutex g_astc_lock;
std::condition_variable g_astc_cv, g_astc_done;
std::deque<std::shared_ptr<AstcJob>> g_astc_queue;
uint64_t g_astc_deferred = 0;

bool async_astc() {
    static const bool on = [] {
        const char* e = std::getenv("NX_ASYNC_ASTC");
#if defined(__ORBIS__)
        bool v = !e || e[0] != '0';
#else
        bool v = e && e[0] == '1';
#endif
        if (v)
            std::thread([] {
                for (;;) {
                    std::shared_ptr<AstcJob> j;
                    {
                        std::unique_lock lk(g_astc_lock);
                        g_astc_cv.wait(lk, [] { return !g_astc_queue.empty(); });
                        j = g_astc_queue.front();
                        g_astc_queue.pop_front();
                    }
                    j->levels = deswizzle_texture(j->raw.data(), j->raw.size(), j->width, j->height, j->layers,
                                                  j->level_count, j->bw, j->bh, j->bytes, j->linear, j->gobs);
                    if (!j->levels.empty()) decode_astc(j->format, j->linear, j->levels, j->layers, j->bw, j->bh);
                    std::vector<uint8_t>().swap(j->raw);
                    std::lock_guard lk(g_astc_lock);
                    j->done = true;
                    g_astc_done.notify_all();
                }
            }).detach();
        return v;
    }();
    return on;
}

// The finished decode of `img`, taken (false: none).
bool take_astc(const GpuImage* img, std::vector<uint8_t>& linear, std::vector<LinearLevel>& levels) {
    if (g_astc_jobs.empty()) return false;
    auto it = g_astc_jobs.find(img);
    if (it == g_astc_jobs.end()) return false;
    std::shared_ptr<AstcJob> j = it->second;
    if (!j->done.load(std::memory_order_acquire)) {  // asked for now: wait for it
        std::unique_lock lk(g_astc_lock);
        g_astc_done.wait(lk, [&] { return j->done.load(); });
    }
    g_astc_jobs.erase(it);
    if (j->levels.empty()) return false;
    linear.swap(j->linear);
    levels.swap(j->levels);
    return true;
}
}  // namespace

namespace replay {  // declared with the draw state (nvn_draw_state.h)
bool astc_pending(uint64_t texture, bool may_defer) {
    if (!async_astc()) return false;
    Texture* t = get<Texture>(texture);
    if (!t || !t->memory || !t->storage_size || t->stride || !astc_decoded(t->format)) return false;
    GpuImage* img = image_of(texture);
    if (!img || img->uploaded || img->rendered || img->alpha_from_r) return false;
    auto it = g_astc_jobs.find(img);
    if (it == g_astc_jobs.end()) {
        if (!may_defer) return false;  // decoded at once by the upload, as before
        auto j = std::make_shared<AstcJob>();
        j->image = t->image;
        const auto* m = reinterpret_cast<const uint8_t*>(t->memory);
        j->raw.assign(m, m + t->storage_size);  // the game may reuse the memory while the worker reads
        j->format = t->format, j->width = img->width, j->height = img->height;
        j->layers = img->view_type == VK_IMAGE_VIEW_TYPE_3D ? std::max(1u, t->depth) : img->layers;
        j->level_count = img->levels, j->gobs = t->block_height_gobs;
        format_block(t->format, j->bw, j->bh, j->bytes);
        g_astc_jobs[img] = j;
        {
            std::lock_guard lk(g_astc_lock);
            g_astc_queue.push_back(j);
        }
        g_astc_cv.notify_one();
        g_astc_deferred++;
        return true;
    }
    if (it->second->done.load(std::memory_order_acquire)) return false;
    if (!may_defer) return false;  // the upload waits for it
    g_astc_deferred++;
    return true;
}

uint64_t astc_deferred_take() {
    uint64_t n = g_astc_deferred;
    g_astc_deferred = 0;
    return n;
}
}  // namespace replay

void upload_texture(VkCommandBuffer cb, const Texture& t, GpuImage& img, uint64_t lo, uint64_t hi) {
    const bool was_uploaded = img.uploaded;
    img.uploaded = true;
    if (gpu_profile_on()) gpu_profile_mark(cb, "texture uploads");
    if (!t.memory || !t.storage_size) return;
    if (std::getenv("NX_TRACE_UPLOADS") && t.storage_size >= (1u << 20)) {
        const uint8_t* m = reinterpret_cast<const uint8_t*>(t.memory);
        size_t nz = 0;
        for (uint64_t k = 0; k < t.storage_size; k += 64) nz += m[k] != 0;
        log("nvn: upload %ux%u fmt %#x stride %u: %zu of %llu sampled bytes non-zero", t.width, t.height, t.format,
            t.stride, nz, (unsigned long long)(t.storage_size / 64));
    }
    uint32_t bw = 1, bh = 1, bytes = 4;
    format_block(t.format, bw, bh, bytes);
    // What the copies read per block: the host format's size (RGBA16F for R11G11B10F), else the NVN one.
    uint32_t host_bytes = vk_block_bytes(img.format) ? vk_block_bytes(img.format) : bytes;
    // NX_R11G11B10_NARROW=1: R11G11B10F copied at 4 bytes per texel, as before (the guard then clears it unless
    // NX_NO_UPLOAD_GUARD=1 too; bisecting).
    static const bool narrow = std::getenv("NX_R11G11B10_NARROW") != nullptr;
    bool widen = !narrow && t.format == 0x3f && img.format == VK_FORMAT_R16G16B16A16_SFLOAT && !img.alpha_from_r;
    std::vector<uint8_t> linear;
    std::vector<VkBufferImageCopy> copies;
    uint32_t layers = img.view_type == VK_IMAGE_VIEW_TYPE_3D ? std::max(1u, t.depth) : img.layers;
    if (img.alpha_from_r && t.stride) {  // R8 pitch-linear -> RGBA8 (255, 255, 255, R), level 0
        const uint8_t* src = reinterpret_cast<const uint8_t*>(t.memory);
        uint64_t rows = uint64_t(img.height) * layers;
        if (uint64_t(t.stride) * rows > t.storage_size) rows = t.storage_size / t.stride;
        // Only the rows the CPU wrote (a single layer), when the texture is already there (NX_FULL_UPLOAD=1: all).
        uint64_t y0 = 0, y1 = rows;
        if (was_uploaded && layers == 1 && !full_upload() && hi != UINT64_MAX)
            y0 = std::min(rows, lo / t.stride), y1 = std::min(rows, (hi + t.stride - 1) / t.stride);
        if (y1 <= y0) return;
        Upload u = texel_alloc((y1 - y0) * img.width * 4, 16);
        for (uint64_t y = y0; y < y1; y++) {
            const uint8_t* s = src + y * t.stride;
            uint32_t* d = reinterpret_cast<uint32_t*>(u.ptr + (y - y0) * img.width * 4);
            for (uint32_t x = 0; x < img.width; x++) d[x] = 0x00FFFFFFu | (uint32_t(s[x]) << 24);
        }
        VkBufferImageCopy r{};
        r.bufferOffset = u.offset;
        r.imageSubresource = {img.aspect, 0, 0, img.layers};
        r.imageOffset = {0, int32_t(y0), 0};
        r.imageExtent = {img.width, uint32_t(std::min<uint64_t>(y1, img.height) - y0), 1};
        transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        return;
    }
    bool decode = bc_host_decode(t.format);  // nvn_bc.cpp: no BC on this GPU
    if (decode && t.stride) {
        static bool logged = false;
        if (!logged) log("nvn/vk: pitch-linear BC texture (format %#x): not decoded, cleared", t.format), logged = true;
        return clear_image(cb, img);
    }
    if (t.stride && astc_decoded(t.format)) {  // never seen: pitch-linear ASTC would need the decoder too
        log("nvn/vk: pitch-linear ASTC texture %ux%u format %#x: cleared", img.width, img.height, t.format);
        return clear_image(cb, img);
    }
    if (t.stride) {  // pitch-linear: rows as they are
        uint64_t rows = (img.height + bh - 1) / bh;
        VkBufferImageCopy r{};
        r.imageSubresource = {img.aspect, 0, 0, img.layers};
        r.imageExtent = {img.width, img.height, 1};
        Upload u;
        uint64_t staged = 0;
        if (widen) {  // tightly packed RGBA16F rows; rows past the storage stay zero
            const uint8_t* src = reinterpret_cast<const uint8_t*>(t.memory);
            uint64_t n = rows * layers, have = std::min<uint64_t>(n, t.storage_size / t.stride);
            uint64_t texels = std::min<uint64_t>(img.width, t.stride / 4);
            staged = n * img.width * 8;
            u = texel_alloc(staged, 16);
            std::memset(u.ptr, 0, staged);
            for (uint64_t y = 0; y < have; y++) widen_r11g11b10(src + y * t.stride, u.ptr + y * img.width * 8, texels);
        } else {
            uint64_t y0 = 0, y1 = rows;  // only the rows the CPU wrote, for a texture already there
            if (was_uploaded && bw == 1 && bh == 1 && layers == 1 && !full_upload() && hi != UINT64_MAX) {
                y0 = std::min(rows, lo / t.stride), y1 = std::min(rows, (hi + t.stride - 1) / t.stride);
                if (y1 <= y0) return;
                r.imageOffset = {0, int32_t(y0), 0};
                r.imageExtent = {img.width, uint32_t(y1 - y0), 1};
            }
            staged = uint64_t(t.stride) * (y1 - y0) * layers;
            u = texel_alloc(staged, 16);
            std::memcpy(u.ptr, reinterpret_cast<const uint8_t*>(t.memory) + y0 * t.stride, staged);
            r.bufferRowLength = t.stride / bytes * bw;
        }
        r.bufferOffset = u.offset;
        if (!copies_fit(t, &r, 1, u.offset, staged, bw, bh, host_bytes)) return clear_image(cb, img);
        transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        return;
    }
    std::vector<LinearLevel> levels;
    const bool predecoded = astc_decoded(t.format) && take_astc(&img, linear, levels);  // decoded in the background
    if (!predecoded)
        levels = deswizzle_texture(reinterpret_cast<const uint8_t*>(t.memory), t.storage_size, img.width, img.height,
                                   layers, img.levels, bw, bh, bytes, linear, t.block_height_gobs);
    if (levels.empty()) {
        log("nvn/vk: texture %ux%u format %#x: could not untile (%llu bytes)", img.width, img.height, t.format,
            (unsigned long long)t.storage_size);
        return;
    }
    if (decode) {  // every level's blocks -> texels, tightly packed, then copied as an uncompressed image
        uint32_t tb = bc_decoded_bytes(t.format), slices = img.view_type == VK_IMAGE_VIEW_TYPE_3D ? layers : img.layers;
        uint64_t total = 0;
        for (const auto& l : levels) total += uint64_t(l.width) * l.height * slices * tb;
        Upload u = texel_alloc(total, 16);
        uint64_t at = 0;
        for (uint32_t k = 0; k < levels.size(); k++) {
            bc_decode(t.format, linear.data() + levels[k].offset, levels[k].width, levels[k].height, slices, u.ptr + at);
            VkBufferImageCopy r{};
            r.bufferOffset = u.offset + at;
            r.imageSubresource = {img.aspect, k, 0, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? 1u : img.layers};
            r.imageExtent = {levels[k].width, levels[k].height, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? layers : 1u};
            copies.push_back(r);
            at += uint64_t(levels[k].width) * levels[k].height * slices * tb;
        }
        transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()),
                                 copies.data());
        return;
    }
    if (astc_decoded(t.format)) {  // RGBA8 texels from here on, or BC3 blocks (astc_bc3)
        if (!predecoded) decode_astc(t.format, linear, levels, layers, bw, bh);
        if (astc_bc3())
            bw = bh = 4, bytes = host_bytes = 16;
        else
            bw = bh = 1, bytes = host_bytes = 4;
    }
    // Widened, every offset in the linear buffer doubles (level offsets are multiples of the 4-byte texel).
    uint64_t scale = widen ? 2 : 1;
    Upload u;
    if (widen) {
        u = texel_alloc(linear.size() * 2, 16);
        widen_r11g11b10(linear.data(), u.ptr, linear.size() / 4);
    } else if (linear.size() >= (1u << 20) && !std::getenv("NX_STAGING_RING") && staging_wanted(linear.size())) {
        // 1 MB and up: a staging buffer of its own, so a scene load cannot wrap the ring over itself (DEBUG-2 #72).
        // NX_STAGING_RING=1: through the ring, as before (bisecting).
        u = staging_alloc(linear.size());
        std::memcpy(u.ptr, linear.data(), linear.size());
    } else {
        u = upload(linear.data(), linear.size(), 16);
    }
    for (uint32_t k = 0; k < levels.size(); k++) {
        VkBufferImageCopy r{};
        r.bufferOffset = u.offset + levels[k].offset * scale;
        r.imageSubresource = {img.aspect, k, 0, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? 1u : img.layers};
        r.imageExtent = {levels[k].width, levels[k].height, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? layers : 1u};
        copies.push_back(r);
    }
    if (!copies_fit(t, copies.data(), uint32_t(copies.size()), u.offset, linear.size() * scale, bw, bh, host_bytes))
        return clear_image(cb, img);
    transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()),
                             copies.data());
}

}  // namespace nx::nvn
