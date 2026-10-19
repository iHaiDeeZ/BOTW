// NVN copies between textures (docs/NVN.md, N6). nvnCommandBufferCopyTextureToTexture(cmd, const NVNtexture* src,
// const NVNtextureView* srcView, const NVNcopyRegion* srcRegion, const NVNtexture* dst, const NVNtextureView* dstView,
// const NVNcopyRegion* dstRegion, int copyFlags) (Wonder's call sites). NVNcopyRegion {int x, y, z, width, height,
// depth}; for array textures z/depth are layers. Flags: 1 linear filter, 4/8/16 mirror x/y/z (2 selects the 2D
// engine: no difference here). Replayed as a vkCmdCopyImage when formats and sizes match, else a vkCmdBlitImage.
// Wonder copies its frame into a texture for screen transitions (the opening's cloud wipe).
// Copies within one image (another level or layer, or another rectangle of the same one: blur chains, mips, atlases;
// textures over one storage share an image since #36) are replayed with the image in GENERAL, as Vulkan allows when
// the regions do not overlap; overlapping ones are dropped (G10). NX_COPY_NO_SAME_IMAGE=1: all dropped, the old way.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "nvn_draw_state.h"
#include "nvn_pipeline.h"
#include "nvn_vk.h"

namespace nx::nvn {

using namespace vk;

std::recursive_mutex& gpu_lock();  // nvn_vk.cpp
VkQueue vk_queue();
uint32_t vk_queue_family();

namespace {

struct CopyInfo {
    ViewInfo src_view, dst_view;
    int32_t src[6] = {}, dst[6] = {};
    uint32_t flags = 0;
};

void CopyTextureToTexture(CPU& c) {
    CopyInfo info;
    info.src_view = read_view(c, c.x[2]);
    info.dst_view = read_view(c, c.x[5]);
    for (int k = 0; k < 6; k++) {
        info.src[k] = c.x[3] ? ld<int32_t>(c.x[3] + 4ull * k) : 0;
        info.dst[k] = c.x[6] ? ld<int32_t>(c.x[6] + 4ull * k) : 0;
    }
    info.flags = uint32_t(c.x[7]);
    Cmd cmd{Cmd::CopyTextureToTexture};
    cmd.a[0] = c.x[1], cmd.a[1] = c.x[4];
    record(c.x[0], cmd, &info, sizeof info);
}

bool compressed(VkFormat f) {
    return (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
}

// A copy within one image keeps it GENERAL, where transition() records nothing: the writes before it (a pass,
// another copy) and the reads after it need a barrier of their own.
void general_barrier(VkCommandBuffer cb, const GpuImage& img) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {img.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
}

// Half-open ranges [a0, a0 + an) and [b0, b0 + bn) intersect.
bool ranges_meet(int64_t a0, int64_t an, int64_t b0, int64_t bn) { return a0 < b0 + bn && b0 < a0 + an; }

uint64_t g_same_image = 0, g_same_overlap = 0;

}  // namespace

void replay_copy(VkCommandBuffer cb, const Cmd& c, const CmdList& list) {
    static const bool off = std::getenv("NX_NO_COPY") != nullptr;  // debug: skip texture copies
    if (off) return;
    if (c.blob >= list.blobs.size() || list.blobs[c.blob].size() != sizeof(CopyInfo)) return;
    CopyInfo info;
    std::memcpy(&info, list.blobs[c.blob].data(), sizeof info);
    GpuImage* src = ensure_contents(cb, c.a[0]);
    GpuImage* dst = ensure_contents(cb, c.a[1]);
    if (!src || !dst) return;
    bool same = src == dst;
    static const bool no_same = std::getenv("NX_COPY_NO_SAME_IMAGE") != nullptr;  // bisecting: the old way
    if (same && no_same) return;
    uint32_t sl = std::min(info.src_view.base_level, src->levels - 1), dl = std::min(info.dst_view.base_level, dst->levels - 1);
    bool s3d = src->view_type == VK_IMAGE_VIEW_TYPE_3D, d3d = dst->view_type == VK_IMAGE_VIEW_TYPE_3D;
    auto sub = [](const GpuImage& img, uint32_t level, int32_t z, int32_t depth, uint32_t view_layer, bool is3d) {
        VkImageAspectFlags aspect = img.aspect;
        uint32_t layer = is3d ? 0 : std::min<uint32_t>(uint32_t(std::max(z, 0)) + view_layer, img.layers - 1);
        uint32_t count = is3d ? 1 : std::clamp<uint32_t>(uint32_t(std::max(depth, 1)), 1, img.layers - layer);
        return VkImageSubresourceLayers{aspect, level, layer, count};
    };
    VkImageSubresourceLayers ss = sub(*src, sl, info.src[2], info.src[5], info.src_view.min_layer, s3d);
    VkImageSubresourceLayers ds = sub(*dst, dl, info.dst[2], info.dst[5], info.dst_view.min_layer, d3d);
    int32_t sw = std::max(info.src[3], 1), sh = std::max(info.src[4], 1);
    int32_t dw = std::max(info.dst[3], 1), dh = std::max(info.dst[4], 1);
    if (same) {
        // One subresource and layer range, and the rectangles (and 3D slices) meet: Vulkan forbids it, dropped.
        bool overlap = ss.mipLevel == ds.mipLevel &&
                       ranges_meet(ss.baseArrayLayer, ss.layerCount, ds.baseArrayLayer, ds.layerCount) &&
                       ranges_meet(info.src[0], sw, info.dst[0], dw) && ranges_meet(info.src[1], sh, info.dst[1], dh) &&
                       (!s3d || ranges_meet(info.src[2], std::max(info.src[5], 1), info.dst[2],
                                            std::max(info.dst[5], 1)));
        g_same_image++, g_same_overlap += overlap;
        if (g_same_image == 1 || g_same_image % 1000 == 0 || (overlap && g_same_overlap <= 5))
            log("nvn: %llu copies within one image (%llu overlapping, dropped); this one %llx level %u -> %u, "
                "layer %u -> %u%s",
                (unsigned long long)g_same_image, (unsigned long long)g_same_overlap, (unsigned long long)c.a[0],
                ss.mipLevel, ds.mipLevel, ss.baseArrayLayer, ds.baseArrayLayer, overlap ? " (overlapping)" : "");
        if (overlap) return;
    }
    static const bool trace_cubes = std::getenv("NX_TRACE_CUBES") != nullptr;
    static int cube_logs = 0;
    if (trace_cubes && (dst->view_type == VK_IMAGE_VIEW_TYPE_CUBE || dst->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) &&
        cube_logs++ < 400)
        log("cubes: copy %llx -> %llx level %u layer %u+%u (z %d depth %d view layer %u) %dx%d", (unsigned long long)c.a[0],
            (unsigned long long)c.a[1], ds.mipLevel, ds.baseArrayLayer, ds.layerCount, info.dst[2], info.dst[5],
            info.dst_view.min_layer, info.dst[3], info.dst[4]);
    VkImageLayout sly = same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkImageLayout dly = same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    transition(cb, *src, sly);
    transition(cb, *dst, dly);
    if (same) general_barrier(cb, *src);
    bool mirror = info.flags & (4 | 8 | 16);
    if (src->format == dst->format && sw == dw && sh == dh && !mirror && ss.layerCount == ds.layerCount) {
        VkImageCopy r{};
        r.srcSubresource = ss, r.dstSubresource = ds;
        r.srcOffset = {info.src[0], info.src[1], s3d ? info.src[2] : 0};
        r.dstOffset = {info.dst[0], info.dst[1], d3d ? info.dst[2] : 0};
        r.extent = {uint32_t(sw), uint32_t(sh), s3d ? uint32_t(std::max(info.src[5], 1)) : 1u};
        p_vkCmdCopyImage(cb, src->image, sly, dst->image, dly, 1, &r);
    } else if (!compressed(src->format) && !compressed(dst->format) && src->aspect == dst->aspect) {
        VkImageBlit b{};
        b.srcSubresource = ss, b.dstSubresource = ds;
        ds.layerCount = ss.layerCount = std::min(ss.layerCount, ds.layerCount);
        b.srcSubresource.layerCount = b.dstSubresource.layerCount = ss.layerCount;
        b.srcOffsets[0] = {info.src[0], info.src[1], 0};
        b.srcOffsets[1] = {info.src[0] + sw, info.src[1] + sh, 1};
        b.dstOffsets[0] = {info.dst[0], info.dst[1], 0};
        b.dstOffsets[1] = {info.dst[0] + dw, info.dst[1] + dh, 1};
        if (info.flags & 4) std::swap(b.dstOffsets[0].x, b.dstOffsets[1].x);
        if (info.flags & 8) std::swap(b.dstOffsets[0].y, b.dstOffsets[1].y);
        bool linear = (info.flags & 1) && src->aspect == VK_IMAGE_ASPECT_COLOR_BIT;
        p_vkCmdBlitImage(cb, src->image, sly, dst->image, dly, 1, &b, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    } else {
        static int logged = 0;
        if (logged++ < 5)
            log("nvn: copy %llx -> %llx not replayed (formats %d -> %d, %dx%d -> %dx%d)", (unsigned long long)c.a[0],
                (unsigned long long)c.a[1], int(src->format), int(dst->format), sw, sh, dw, dh);
        return;
    }
    if (same) general_barrier(cb, *dst);
    dst->rendered = true;  // GPU-written from now on
    image_written(*dst);
}

// ---- GPU texels to the CPU ---------------------------------------------------------------------------------------
// nvnCommandBufferCopyTextureToBuffer(cmd, texture, view, region, NVNbufferAddress dst, flags) and
// nvnTextureReadTexels(texture, view, region, void* dst) / ...Strided(..., rowStride, imageStride): Tears of the
// Kingdom reads GPU results back every frame (main+11c9d44, main+103bbe0); both were stubs that left the game's
// memory untouched. Texels come out as tight rows of the NVN format's blocks, which the host image keeps as long as
// its texel size is the NVN format's (a widened or re-packed host format is not copied: logged).

namespace {

struct ToBufferInfo {
    ViewInfo view;
    int32_t region[6] = {};
    uint32_t flags = 0;
};

void CopyTextureToBuffer(CPU& c) {
    ToBufferInfo info;
    info.view = read_view(c, c.x[2]);
    for (int k = 0; k < 6; k++) info.region[k] = c.x[3] ? ld<int32_t>(c.x[3] + 4ull * k) : 0;
    info.flags = uint32_t(c.x[5]);
    Cmd cmd{Cmd::CopyTextureToBuffer};
    cmd.a[0] = c.x[1], cmd.a[1] = c.x[4];
    record(c.x[0], cmd, &info, sizeof info);
}

// The copy of the view's region of `img` as tight rows of blocks; false when the host texels are not the NVN ones.
bool readback_region(const Texture& t, const GpuImage& img, const ViewInfo& v, const int32_t* r, VkBufferImageCopy& out,
                     uint64_t& bytes) {
    uint32_t bw = 1, bh = 1, nb = 4;
    format_block(t.format, bw, bh, nb);
    if (!(img.aspect & VK_IMAGE_ASPECT_COLOR_BIT) || vk_block_bytes(img.format) != nb) return false;
    bool is3d = img.view_type == VK_IMAGE_VIEW_TYPE_3D;
    uint32_t level = std::min(v.base_level, img.levels - 1);
    uint32_t w = uint32_t(std::max(r[3], 1)), h = uint32_t(std::max(r[4], 1)), d = uint32_t(std::max(r[5], 1));
    uint32_t layer = is3d ? 0 : std::min<uint32_t>(uint32_t(std::max(r[2], 0)) + v.min_layer, img.layers - 1);
    uint32_t layers = is3d ? 1 : std::clamp<uint32_t>(d, 1, img.layers - layer);
    out = {};
    out.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, layers};
    out.imageOffset = {r[0], r[1], is3d ? r[2] : 0};
    out.imageExtent = {w, h, is3d ? d : 1u};
    bytes = uint64_t((w + bw - 1) / bw) * ((h + bh - 1) / bh) * nb * (is3d ? d : layers);
    return true;
}

void log_skipped(const char* what, uint64_t texture, const Texture* t, const GpuImage* img) {
    static int logged = 0;
    if (logged++ < 10)
        log("nvn: %s of %llx skipped (%s, NVN format %#x, host format %d)", what, (unsigned long long)texture,
            !t ? "unknown texture" : !img ? "no image" : "texel size or aspect differs", t ? t->format : 0,
            img ? int(img->format) : -1);
}

void ReadTexelsStrided(CPU& c) {  // (texture, view, region, void* dst, ptrdiff_t row, ptrdiff_t image)
    uint64_t texture = c.x[0], dst = c.x[3];
    int64_t row = int64_t(c.x[4]), image = int64_t(c.x[5]);
    ViewInfo v = read_view(c, c.x[1]);
    int32_t r[6];
    for (int k = 0; k < 6; k++) r[k] = c.x[2] ? ld<int32_t>(c.x[2] + 4ull * k) : 0;
    std::lock_guard lk(gpu_lock());
    Texture* t = get<Texture>(texture);
    GpuImage* img = t ? image_of(texture) : nullptr;
    VkBufferImageCopy copy;
    uint64_t bytes = 0;
    if (!t || !img || !img->image || img->layout == VK_IMAGE_LAYOUT_UNDEFINED || !dst ||
        !readback_region(*t, *img, v, r, copy, bytes)) {
        log_skipped("ReadTexels", texture, t, img);
        return;
    }
    // Everything queued so far may write the texture: wait for it, then copy on a command buffer of our own.
    frame_wait_all();
    VkDevice d = vk_device();
    static VkCommandPool pool = VK_NULL_HANDLE;
    static VkCommandBuffer cb = VK_NULL_HANDLE;
    static VkFence fence = VK_NULL_HANDLE;
    static VkBuffer buffer = VK_NULL_HANDLE;
    static VkDeviceMemory memory = VK_NULL_HANDLE;
    static uint64_t capacity = 0;
    static void* mapped = nullptr;
    if (!pool) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = vk_queue_family();
        p_vkCreateCommandPool(d, &pi, nullptr, &pool);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool, ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, ai.commandBufferCount = 1;
        p_vkAllocateCommandBuffers(d, &ai, &cb);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        p_vkCreateFence(d, &fi, nullptr, &fence);
    }
    if (bytes > capacity) {  // a host-visible buffer, grown as needed and kept
        if (buffer) p_vkDestroyBuffer(d, buffer, nullptr), p_vkFreeMemory(d, memory, nullptr);
        buffer = VK_NULL_HANDLE, memory = VK_NULL_HANDLE, mapped = nullptr, capacity = 0;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = std::max<uint64_t>(bytes, 1 << 20);
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (p_vkCreateBuffer(d, &bi, nullptr, &buffer) != VK_SUCCESS) return;
        VkMemoryRequirements req{};
        p_vkGetBufferMemoryRequirements(d, buffer, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex =
            memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (p_vkAllocateMemory(d, &ai, nullptr, &memory) != VK_SUCCESS || p_vkBindBufferMemory(d, buffer, memory, 0) ||
            p_vkMapMemory(d, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
            p_vkDestroyBuffer(d, buffer, nullptr);
            if (memory) p_vkFreeMemory(d, memory, nullptr);
            buffer = VK_NULL_HANDLE, memory = VK_NULL_HANDLE, mapped = nullptr;
            return;
        }
        capacity = bi.size;
    }
    p_vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    p_vkBeginCommandBuffer(cb, &bi);
    VkImageLayout back = img->layout;
    transition(cb, *img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    p_vkCmdCopyImageToBuffer(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    transition(cb, *img, back);
    p_vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1, si.pCommandBuffers = &cb;
    p_vkResetFences(d, 1, &fence);
    if (p_vkQueueSubmit(vk_queue(), 1, &si, fence) != VK_SUCCESS ||
        p_vkWaitForFences(d, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        return;
    // Tight rows on our side; the game's strides (0: tight) on its side.
    uint32_t bw = 1, bh = 1, nb = 4;
    format_block(t->format, bw, bh, nb);
    uint64_t tight_row = uint64_t((copy.imageExtent.width + bw - 1) / bw) * nb;
    uint64_t rows = (copy.imageExtent.height + bh - 1) / bh;
    uint64_t slices = std::max(copy.imageExtent.depth, copy.imageSubresource.layerCount);
    uint64_t drow = row ? uint64_t(row) : tight_row, dimage = image ? uint64_t(image) : drow * rows;
    const uint8_t* from = static_cast<const uint8_t*>(mapped);
    for (uint64_t z = 0; z < slices; z++)
        for (uint64_t y = 0; y < rows; y++)
            std::memcpy(reinterpret_cast<void*>(dst + z * dimage + y * drow), from + (z * rows + y) * tight_row,
                        tight_row);
}

void ReadTexels(CPU& c) {  // (texture, view, region, void* dst): tight rows
    c.x[4] = 0, c.x[5] = 0;
    ReadTexelsStrided(c);
}

}  // namespace

void replay_copy_to_buffer(VkCommandBuffer cb, const Cmd& c, const CmdList& list) {
    if (c.blob >= list.blobs.size() || list.blobs[c.blob].size() != sizeof(ToBufferInfo)) return;
    ToBufferInfo info;
    std::memcpy(&info, list.blobs[c.blob].data(), sizeof info);
    Texture* t = get<Texture>(c.a[0]);
    GpuImage* src = t ? ensure_contents(cb, c.a[0]) : nullptr;
    uint64_t off = 0;
    MemoryPool* pool = pool_at_gpu(c.a[1], &off);
    uint8_t* to = pool ? reinterpret_cast<uint8_t*>(pool_host(*pool, off)) : nullptr;
    VkBufferImageCopy r;
    uint64_t bytes = 0;
    if (!src || !to || !readback_region(*t, *src, info.view, info.region, r, bytes)) {
        log_skipped("CopyTextureToBuffer", c.a[0], t, src);
        return;
    }
    Upload u = upload_alloc(bytes, 16);
    r.bufferOffset = u.offset;
    transition(cb, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    p_vkCmdCopyImageToBuffer(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, u.buffer, 1, &r);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                           nullptr);
    replay::separate_write_backs().push_back({u.ptr, to, bytes, nullptr});  // to the game's memory after the fence
}

NX_HLE("nvnCommandBufferCopyTextureToTexture", CopyTextureToTexture);
NX_HLE("nvnCommandBufferCopyTextureToBuffer", CopyTextureToBuffer);
NX_HLE("nvnTextureReadTexels", ReadTexels);
NX_HLE("nvnTextureReadTexelsStrided", ReadTexelsStrided);

}  // namespace nx::nvn
