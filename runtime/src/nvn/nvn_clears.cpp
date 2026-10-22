// Clears (split from nvn_draw.cpp). A clear of targets other than the open pass's used to end it and begin a pass on
// the new targets with every attachment loaded, then clear with a vkCmdClearAttachments quad: ~36 passes per course
// frame, each a full-resolution load on a tile GPU for data the clear then overwrote. Such a clear is now kept
// pending and becomes the loadOp CLEAR of the pass the next draw begins on the same targets (same extent, so the
// same area as the quad). Anything else first (a draw on other targets or sampling a cleared image, a dispatch, a
// copy, the submit's end) clears it in a pass of its own with no draw (docs/nvn/DEBUG.md #47).
// NX_EAGER_CLEARS=1: the old way (bisecting).
// Clears are scissored and channel-masked, as on Maxwell (G6): a clear inside a scissor smaller than its target, or
// with some channels masked off, is never a loadOp; it runs in the pass, with vkCmdClearAttachments on the scissor
// rectangle, or, masked, a fullscreen draw with that write mask (nvn_mask_clear.cpp). NX_CLEARS_SCISSOR=1 (off: it broke the course) and
// NX_CLEARS_NO_MASK=1: whole-target and all-channel clears, the old way (bisecting).
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

#include "nvn_draw_state.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

struct Pending {
    bool on = false;
    Targets t;
    uint32_t colors = 0;  // bit c: colour target c cleared
    VkClearValue color[8]{};
    VkImageAspectFlags ds = 0;
    VkClearValue depth{};
};
Pending g_pending;
uint64_t g_deferred = 0, g_as_load = 0, g_alone = 0, g_transfer = 0;
uint64_t g_scissored = 0, g_masked = 0, g_mask_fallback = 0, g_scissor_lost = 0;  // G6

// begin_rendering's render area for these targets: the smallest attachment at its level.
VkExtent2D targets_extent(const Targets& t) {
    VkExtent2D e{UINT32_MAX, UINT32_MAX};
    auto fold = [&](uint64_t guest, uint32_t level) {
        GpuImage* img = guest ? image_of(guest) : nullptr;
        if (!img) return;
        level = std::min<uint32_t>(level, img->levels - 1);
        e = {std::min(e.width, std::max(1u, img->width >> level)), std::min(e.height, std::max(1u, img->height >> level))};
    };
    for (uint32_t k = 0; k < t.color_count; k++) fold(t.colors[k], t.views.level[k]);
    if (GpuImage* d = t.depth ? image_of(t.depth) : nullptr; d && d->aspect != VK_IMAGE_ASPECT_COLOR_BIT)
        fold(t.depth, t.views.level[8]);
    return e;
}

}  // namespace

bool clears_pending_image(const GpuImage* img) {
    if (!g_pending.on || !img) return false;
    for (uint32_t k = 0; k < g_pending.t.color_count; k++)
        if ((g_pending.colors >> k & 1) && g_pending.t.colors[k] && image_of(g_pending.t.colors[k]) == img) return true;
    return g_pending.ds && g_pending.t.depth && image_of(g_pending.t.depth) == img;
}

namespace {

// Where each pending cleared attachment is in `t` (slot, or -1): the same texture and view at the same render area.
bool pending_slots(const Targets& t, int slot[9]) {
    const Targets& p = g_pending.t;
    for (uint32_t k = 0; k < 9; k++) slot[k] = -1;
    for (uint32_t k = 0; k < p.color_count; k++) {
        if (!(g_pending.colors >> k & 1)) continue;
        for (uint32_t j = 0; j < t.color_count && slot[k] < 0; j++)
            if (t.colors[j] == p.colors[k] && t.views.level[j] == p.views.level[k] &&
                t.views.layer[j] == p.views.layer[k] && t.views.format[j] == p.views.format[k])
                slot[k] = int(j);
        if (slot[k] < 0) return false;
    }
    if (g_pending.ds) {
        if (t.depth != p.depth || t.views.level[8] != p.views.level[8] || t.views.layer[8] != p.views.layer[8])
            return false;
        slot[8] = 8;
    }
    if (targets_same(t, p)) return true;
    // NX_CLEARS_SUBSET=1: also a pass on more targets with the same area. It measured no faster (GPU 13.7 against
    // 13.0 ms) and one run showed a black band across the course, so it is off.
    static const bool subset = std::getenv("NX_CLEARS_SUBSET") != nullptr;
    if (!subset) return false;
    VkExtent2D a = targets_extent(t), b = targets_extent(p);
    return a.width == b.width && a.height == b.height;
}

}  // namespace

bool clears_pending_other(const Targets& t) {
    int slot[9];
    return g_pending.on && !pending_slots(t, slot);
}

uint32_t clears_apply(const Targets& t, VkRenderingAttachmentInfo* colors, VkRenderingAttachmentInfo& depth,
                      VkRenderingAttachmentInfo& stencil) {
    int slot[9];
    if (!g_pending.on || !pending_slots(t, slot)) return 0;
    uint32_t cleared = 0;
    for (uint32_t k = 0; k < 8; k++)
        if (slot[k] >= 0 && colors[slot[k]].imageView) {
            colors[slot[k]].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colors[slot[k]].clearValue = g_pending.color[k];
            cleared |= 1u << slot[k];
        }
    if (g_pending.ds && depth.imageView) {
        depth.clearValue = stencil.clearValue = g_pending.depth;
        if (g_pending.ds & VK_IMAGE_ASPECT_DEPTH_BIT) depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        if (g_pending.ds & VK_IMAGE_ASPECT_STENCIL_BIT) stencil.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        cleared |= 1u << 8;
    }
    g_pending.on = false;
    g_as_load++;
    return cleared;
}

// A pending clear that no pass takes, as transfer clears instead of an empty pass (docs/nvn/android-perf/PHASE4.md
// G5A): on Adreno each pass makes the driver's per-pass objects (~40-57 us), ~11 such passes per course frame. A pending
// clear covers its whole target (draw_clear defers only those), so clearing the view's level and layer is the same.
// Not for a view in another format than its image (a loadOp clears through the view, an sRGB view encodes), nor for
// 3D images (a transfer clear takes every slice): those keep the pass. Android by default (MoltenVK encodes a transfer
// clear as a Metal encoder of its own, DEBUG.md #31); NX_CLEARS_PASS=1: the pass; NX_CLEARS_TRANSFER=1: elsewhere too.
bool transfer_clears() {
    static const bool on = [] {
#if defined(__ANDROID__)
        return std::getenv("NX_CLEARS_PASS") == nullptr;
#else
        return std::getenv("NX_CLEARS_TRANSFER") != nullptr;
#endif
    }();
    return on;
}

bool clears_transfer(VkCommandBuffer cb, const Targets& t) {
    GpuImage* imgs[9] = {};
    for (uint32_t k = 0; k < t.color_count; k++) {
        if (!(g_pending.colors >> k & 1)) continue;
        GpuImage* img = t.colors[k] ? image_of(t.colors[k]) : nullptr;
        if (!img) continue;
        if (img->view_type == VK_IMAGE_VIEW_TYPE_3D || view_format(*img, t.views.format[k]) != img->format) return false;
        imgs[k] = img;
    }
    if (g_pending.ds) {
        GpuImage* d = t.depth ? image_of(t.depth) : nullptr;
        if (d && (d->aspect & g_pending.ds) != g_pending.ds) return false;
        imgs[8] = d;
    }
    for (uint32_t k = 0; k < 9; k++) {
        GpuImage* img = imgs[k];
        if (!img) continue;
        transition(cb, *img, VK_IMAGE_LAYOUT_GENERAL);  // as begin_rendering: rendered images stay GENERAL
        img->rendered = true;
        image_written(*img);
        uint32_t level = std::min<uint32_t>(t.views.level[k], img->levels - 1);
        uint32_t layer = std::min<uint32_t>(t.views.layer[k], img->layers - 1);
        if (k < 8) {
            VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, level, 1, layer, 1};
            p_vkCmdClearColorImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, &g_pending.color[k].color, 1, &r);
        } else {
            VkImageSubresourceRange r{g_pending.ds, level, 1, layer, 1};
            p_vkCmdClearDepthStencilImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, &g_pending.depth.depthStencil, 1, &r);
        }
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr,
                           0, nullptr);
    g_pending.on = false;
    g_transfer++;
    return true;
}

void clears_flush(VkCommandBuffer cb) {
    if (!g_pending.on) return;
    Targets t = g_pending.t;
    g_alone++;
    if (transfer_clears() && clears_transfer(cb, t)) return;
    g_as_load--;           // counted by clears_apply
    pass_begin(cb, t);     // takes the clears as its loadOps
    pass_end_only(cb, 3);
}

void clears_begin_submit() { g_pending.on = false; }

void clears_log() {
    log("nvn clears: %llu deferred to the next pass on their targets, %llu of those passes with draws, %llu alone "
        "(%llu of those as transfer clears)", (unsigned long long)g_deferred, (unsigned long long)g_as_load,
        (unsigned long long)g_alone, (unsigned long long)g_transfer);
    log("nvn clears: %llu scissored (%llu of those outside the pass: whole target), %llu channel-masked (%llu of those "
        "all channels: integer target or no pipeline)", (unsigned long long)g_scissored,
        (unsigned long long)g_scissor_lost, (unsigned long long)g_masked, (unsigned long long)g_mask_fallback);
}

// Clears run inside the render pass of the current targets (vkCmdClearAttachments), or become the loadOps of the
// next one (above). A transfer clear between passes was a Metal encoder of its own and made the next pass reload its
// targets: most of the ~25 ms per frame of GPU time "outside passes" (docs/nvn/DEBUG.md #31). Depth and stencil
// clears honour the call's masks.
void draw_clear(VkCommandBuffer cb, const Targets& t, const Cmd& c) {
    bool color = c.op == Cmd::ClearColor;
    // NX_SKIP_CLEARS_AFTER_FS=<hash>[,<hash>...]: the clears that follow a draw with that fragment shader (before the
    // next draw) are skipped (bisecting what a clear wipes).
    // Rendering off (SetRenderEnable(false) or a false condition): clears are skipped too, as on the GPU.
    if (!g_state.render_enable) return;
    static const char* skip_after = std::getenv("NX_SKIP_CLEARS_AFTER_FS");
    if (skip_after && replay::g_last_draw_fs && std::strstr(skip_after, replay::g_last_draw_fs->hash.c_str())) return;
    uint64_t guest = color ? (c.n < 8 ? t.colors[c.n] : 0) : t.depth;
    GpuImage* img = guest ? image_of(guest) : nullptr;
    if (!img || color != (img->aspect == VK_IMAGE_ASPECT_COLOR_BIT)) return;
    if (color && !(c.mask & 0xF)) return;  // no channel enabled
    VkClearAttachment a{};
    if (color) {
        a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, a.colorAttachment = c.n;
        std::memcpy(a.clearValue.color.float32, c.f, 16);
        if (diag_active()) {
            char b[200];
            std::snprintf(b, sizeof b, "  clear colour %u %llx (%ux%u vk%d) to %g %g %g %g (bits %08x) mask %x", c.n,
                          (unsigned long long)guest, img->width, img->height, int(img->format), double(c.f[0]),
                          double(c.f[1]), double(c.f[2]), double(c.f[3]), a.clearValue.color.uint32[0], c.mask);
            diag_line(b);
        }
    } else {
        if ((c.mask & 1) && (img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) a.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if ((c.mask >> 8 & 0xFF) && (img->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) a.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (!a.aspectMask) return;
        a.clearValue.depthStencil = {c.f[0], uint32_t(c.i[0])};
        if (diag_active()) {
            char b[160];
            std::snprintf(b, sizeof b, "  clear depth %llx (%ux%u vk%d) to %g stencil %d mask %x layer %u",
                          (unsigned long long)guest, img->width, img->height, int(img->format), double(c.f[0]), c.i[0],
                          c.mask, t.views.layer[8]);
            diag_line(b);
        }
    }
    uint32_t level = color ? t.views.level[c.n] : t.views.level[8];
    VkExtent2D full{std::max(1u, img->width >> level), std::max(1u, img->height >> level)};
    static const bool transfer = std::getenv("NX_TRANSFER_CLEARS") != nullptr;  // bisecting: whole-image clears
    static const bool eager = std::getenv("NX_EAGER_CLEARS") != nullptr;
    // Off unless NX_CLEARS_SCISSOR=1: clipping clears to the bound scissor broke the course (whole regions left
    // uncleared: huge black/white shapes, DEBUG.md #52), so NVN's clears are not scissored the way this reads it.
    static const bool no_scissor = std::getenv("NX_CLEARS_SCISSOR") == nullptr;
    static const bool no_mask = std::getenv("NX_CLEARS_NO_MASK") != nullptr;
    // The part of the target the clear covers: the scissor (pass coordinates, as the draws') within the target.
    VkRect2D rect{{0, 0}, full};
    if (!no_scissor && g_state.has_scissor) {
        const int32_t* s = g_state.scissor;
        int64_t x0 = std::max(0, s[0]), y0 = std::max(0, s[1]);
        int64_t x1 = std::min<int64_t>(int64_t(s[0]) + std::max(0, s[2]), full.width);
        int64_t y1 = std::min<int64_t>(int64_t(s[1]) + std::max(0, s[3]), full.height);
        if (x1 <= x0 || y1 <= y0) return;  // nothing of the target inside the scissor
        rect = {{int32_t(x0), int32_t(y0)}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    }
    bool scissored =
        rect.offset.x || rect.offset.y || rect.extent.width < full.width || rect.extent.height < full.height;
    bool masked = color && !no_mask && (c.mask & 0xF) != 0xF;
    g_scissored += scissored, g_masked += masked;
    if (!transfer && !eager && !scissored && !masked && !pass_open_on(t)) {
        VkExtent2D e = targets_extent(t);
        if (full.width <= e.width && full.height <= e.height) {  // the pass covers it: a loadOp can clear it
            pass_end_only(cb, 3);  // other targets' pass, and their pending clears
            if (g_pending.on && !targets_same(g_pending.t, t)) clears_flush(cb);  // its slots index its own targets
            if (!g_pending.on) g_pending = {true, t};
            if (color) {
                g_pending.colors |= 1u << c.n;
                g_pending.color[c.n] = a.clearValue;
            } else {
                if (a.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) g_pending.depth.depthStencil.depth = c.f[0];
                if (a.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) g_pending.depth.depthStencil.stencil = uint32_t(c.i[0]);
                g_pending.ds |= a.aspectMask;
            }
            g_deferred++;
            replay::trace_pass("clear deferred: slot, mask", t, color ? c.n : 8, c.mask);
            return;
        }
    }
    if (!pass_open_on(t)) {
        draw_end_rendering(cb, 3);
        pass_begin(cb, t);
    }
    VkExtent2D pe = pass_extent();
    if (transfer && !color) a.aspectMask = img->aspect;  // the old clears ignored the masks
    bool in_pass = rect.offset.x + rect.extent.width <= pe.width && rect.offset.y + rect.extent.height <= pe.height;
    if (transfer || !in_pass) {
        // The pass is smaller than this target (a larger shared depth buffer): clear all of it, outside the pass.
        g_scissor_lost += scissored, g_mask_fallback += masked;
        draw_end_rendering(cb, 4);
        transition(cb, *img, VK_IMAGE_LAYOUT_GENERAL);
        VkImageSubresourceRange r{a.aspectMask, std::min<uint32_t>(level, img->levels - 1), 1,
                                  std::min<uint32_t>(color ? t.views.layer[c.n] : t.views.layer[8], img->layers - 1), 1};
        if (color)
            p_vkCmdClearColorImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, &a.clearValue.color, 1, &r);
        else
            p_vkCmdClearDepthStencilImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, &a.clearValue.depthStencil, 1, &r);
        return;
    }
    if (masked) pass_inline(cb);  // a fullscreen draw of its own: not into the workers' command buffers
    if (masked && mask_clear(cb, t, c.n, c.mask & 0xF, c.f, rect)) {
        pass_mark_written(1u << c.n);
        return;
    }
    g_mask_fallback += masked;
    VkClearRect r{rect, 0, 1};  // unscissored: all of the target, which is the pass's area
    replay::trace_pass("clear in pass: slot, mask | bound channel mask << 8", t, color ? c.n : 8,
                       c.mask | (color ? uint32_t(g_state.mask.mask[c.n]) << 8 : 0));
    pass_clear_attachment(cb, a, r);  // the workers' stream in a parallel pass
    pass_mark_written(color ? 1u << c.n : 1u << 8);  // a later draw sampling it needs a fresh pass
}

// nvnCommandBufferClearTexture(cmd, const NVNtexture*, const NVNtextureView*, const NVNcopyRegion*, const float* rgba,
// int mask): a colour texture cleared without being bound as a target. Breath of the Wild's agl clears its targets
// this way (main+1283a4c), not with ClearColor; it was a trace stub, so nothing of the game's was ever cleared.
// NVNcopyRegion {x, y, z, width, height, depth}: z/depth are layers of an array, slices of a 3D texture.
namespace {

struct ClearTextureInfo {
    ViewInfo view;
    int32_t region[6] = {};
    float rgba[4] = {};
};

void ClearTexture(CPU& c) {
    adopt_texture(c, c.x[1]);
    ClearTextureInfo info;
    info.view = read_view(c, c.x[2]);
    for (int k = 0; k < 6; k++) info.region[k] = c.x[3] ? ld<int32_t>(c.x[3] + 4ull * k) : 0;
    if (c.x[4]) std::memcpy(info.rgba, reinterpret_cast<const void*>(c.x[4]), sizeof info.rgba);
    Cmd cmd{Cmd::ClearTexture};
    cmd.a[0] = stable_texture_key(c.x[1]);  // the incarnation as recorded (agl clears through holder structs)
    cmd.mask = uint32_t(c.x[5]);
    record(c.x[0], cmd, &info, sizeof info);
}

}  // namespace

// Each layer (or slice) of the region, at the view's level, as a clear of a pass on that texture alone: a whole layer
// goes through draw_clear (deferred into the next pass on it, as the game's own clears), a rectangle is cleared in
// such a pass.
void replay_clear_texture(VkCommandBuffer cb, const Cmd& c, const CmdList& list) {
    if (c.blob >= list.blobs.size() || list.blobs[c.blob].size() != sizeof(ClearTextureInfo)) return;
    ClearTextureInfo info;
    std::memcpy(&info, list.blobs[c.blob].data(), sizeof info);
    GpuImage* img = c.a[0] ? image_of(c.a[0]) : nullptr;
    if (!img || img->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
        static int logged = 0;
        if (logged++ < 10)
            log("nvn: ClearTexture of %llx skipped (%s)", (unsigned long long)c.a[0],
                img ? "not a colour texture" : "unknown texture");
        return;
    }
    if (!g_state.render_enable || !(c.mask & 0xF)) return;
    const int32_t* r = info.region;
    uint32_t level = std::min(info.view.base_level, img->levels - 1);
    VkExtent2D full{std::max(1u, img->width >> level), std::max(1u, img->height >> level)};
    bool whole = r[3] <= 0 || r[4] <= 0 ||
                 (r[0] <= 0 && r[1] <= 0 && int64_t(r[0]) + r[3] >= int64_t(full.width) &&
                  int64_t(r[1]) + r[4] >= int64_t(full.height));
    bool is3d = img->view_type == VK_IMAGE_VIEW_TYPE_3D;
    uint32_t slices = is3d ? std::max(1u, img->depth >> level) : img->layers;
    uint32_t first = std::min<uint32_t>(uint32_t(std::max(r[2], 0)) + (is3d ? 0 : info.view.min_layer), slices - 1);
    uint32_t count = std::min<uint32_t>(uint32_t(std::max(r[5], 1)), slices - first);
    Cmd clear{Cmd::ClearColor};
    clear.n = 0, clear.mask = c.mask & 0xF;
    std::memcpy(clear.f, info.rgba, sizeof clear.f);
    for (uint32_t layer = first; layer < first + count; layer++) {
        Targets t;
        t.color_count = 1;
        t.colors[0] = c.a[0];
        t.views.level[0] = uint8_t(level);
        t.views.layer[0] = uint16_t(layer);
        t.views.format[0] = uint16_t(info.view.format);
        if (whole) {
            draw_clear(cb, t, clear);
            continue;
        }
        if (!pass_open_on(t)) {
            draw_end_rendering(cb, 3);
            pass_begin(cb, t);
        }
        VkExtent2D pe = pass_extent();
        int64_t x0 = std::max(0, r[0]), y0 = std::max(0, r[1]);
        int64_t x1 = std::min<int64_t>(int64_t(r[0]) + r[3], pe.width), y1 = std::min<int64_t>(int64_t(r[1]) + r[4], pe.height);
        if (x1 <= x0 || y1 <= y0) continue;
        VkRect2D rect{{int32_t(x0), int32_t(y0)}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
        if (clear.mask != 0xF) pass_inline(cb);
        if (clear.mask != 0xF && mask_clear(cb, t, 0, clear.mask, clear.f, rect)) {
            pass_mark_written(1);
            continue;
        }
        VkClearAttachment a{};
        a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, a.colorAttachment = 0;
        std::memcpy(a.clearValue.color.float32, clear.f, 16);
        VkClearRect cr{rect, 0, 1};
        pass_clear_attachment(cb, a, cr);
        pass_mark_written(1);
    }
}

NX_HLE("nvnCommandBufferClearTexture", ClearTexture);

}  // namespace nx::nvn
