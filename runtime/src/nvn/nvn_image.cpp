// The VkImage behind an NVN texture, and its layout transitions (split from nvn_vk.cpp). Under the GPU lock.
#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "nvn_vk.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

bool check(VkResult r, const char* what) {  // as nvn_vk.cpp's
    if (r >= 0) return true;
    log("nvn/vk: %s failed (%d)", what, int(r));
    if (r == VK_ERROR_DEVICE_LOST) gpu_hung(what);
    return false;
}

// Whether the device can make this image. An unsupported format or usage is undefined behaviour in vkCreateImage:
// NVIDIA's driver divided by zero on one (WINDOWS-PLAN.md). Usages the device refuses (storage, attachment) are
// dropped first; a format it cannot make at all is logged once and the texture gets no image (the dummy).
bool image_supported(VkImageCreateInfo& ci, uint32_t nvn_format) {
    auto ok = [&] {
        VkImageFormatProperties p{};
        return p_vkGetPhysicalDeviceImageFormatProperties(vk_physical(), ci.format, ci.imageType, ci.tiling, ci.usage,
                                                          ci.flags, &p) == VK_SUCCESS &&
               ci.mipLevels <= p.maxMipLevels && ci.arrayLayers <= p.maxArrayLayers &&
               ci.extent.width <= p.maxExtent.width && ci.extent.height <= p.maxExtent.height &&
               ci.extent.depth <= p.maxExtent.depth;
    };
    if (ok()) return true;
    static std::map<std::pair<uint32_t, uint32_t>, bool> logged;
    const VkImageUsageFlags wanted = ci.usage;
    for (VkImageUsageFlags drop : {VkImageUsageFlags(VK_IMAGE_USAGE_STORAGE_BIT),
                                   VkImageUsageFlags(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)}) {
        ci.usage = wanted & ~drop;
        if (ok()) {
            if (!logged[{nvn_format, ci.usage}]) {
                logged[{nvn_format, ci.usage}] = true;
                log("nvn/vk: format %#x (Vulkan %d): usage %#x not supported, made with %#x", nvn_format,
                    int(ci.format), wanted, ci.usage);
            }
            return true;
        }
    }
    ci.usage = wanted;
    if (!logged[{nvn_format, 0}]) {
        logged[{nvn_format, 0}] = true;
        log("nvn/vk: format %#x (Vulkan %d) %ux%ux%u, %u levels, %u layers, type %d, flags %#x: not supported by this "
            "GPU, texture left without an image",
            nvn_format, int(ci.format), ci.extent.width, ci.extent.height, ci.extent.depth, ci.mipLevels,
            ci.arrayLayers, int(ci.imageType), ci.flags);
    }
    return false;
}

}  // namespace

// The VkImage behind an NVN texture, made on first use. NVN texture targets (GL order): 0 1D, 1 2D, 2 3D, 3 1D array,
// 4 2D array, 5 2D multisample, 6 2D multisample array, 7 rectangle, 8 cube map, 9 cube map array, 10 buffer.
// Arrays and cube maps keep their layer count in the builder's depth.
// Live textures over the same storage share one image: the game renders through one NVNtexture and samples the same
// memory through another (UI capture and blur chains, agl filters). With an image per object the sampled one was a
// stale upload of guest memory: no blur behind the menus, missing UI masks (docs/nvn/DEBUG.md #36).
struct StorageKey {
    uint64_t memory = 0;
    uint32_t format = 0, width = 0, height = 0, depth = 0, levels = 0, target = 0, samples = 0, swizzle = 0;
    bool operator<(const StorageKey& o) const {
        return std::tie(memory, format, width, height, depth, levels, target, samples, swizzle) <
               std::tie(o.memory, o.format, o.width, o.height, o.depth, o.levels, o.target, o.samples, o.swizzle);
    }
};
std::map<StorageKey, std::weak_ptr<GpuImage>> g_live_images;

GpuImage* image_of(uint64_t guest) {
    Texture* t = get<Texture>(guest);
    if (!t) return nullptr;
    if (t->image) return t->image.get();
    uint32_t swizzle = uint32_t(t->swizzle[0]) | uint32_t(t->swizzle[1]) << 8 | uint32_t(t->swizzle[2]) << 16 |
                  uint32_t(t->swizzle[3]) << 24;
    // Swizzles apply when sampling (sampled_view, #44): neither the key's nor a pool view's keeps textures from sharing.
    // With them, the shadow map and the UI's text strips were sampled as stale uploads (#46). Env: bisecting.
    static const bool key_swizzle = std::getenv("NX_KEY_SWIZZLE") != nullptr;
    if (!key_swizzle) swizzle = 0;
    StorageKey key{t->memory, t->format, t->width, t->height, t->depth, t->levels, t->target, t->samples, swizzle};
    static const bool share = std::getenv("NX_NO_IMAGE_SHARE") == nullptr;  // bisecting
    // An R8 image widened for alpha-from-red keeps its own.
    static const bool old_rule = std::getenv("NX_SHARE_NO_POOL_SWIZZLE") != nullptr;  // bisecting
    bool widened = t->format == 0x1 && t->pool_swizzle == 0x02010101u;
    if (share && t->memory && (old_rule ? t->pool_swizzle == 0 : !widened))
        if (auto it = g_live_images.find(key); it != g_live_images.end())
            if (auto shared = it->second.lock(); shared && shared->image && !shared->alpha_from_r) {
                static uint64_t shares = 0;
                if (++shares % 1000 == 1) log("nvn/vk: %llu textures shared an image with another", (unsigned long long)shares);
                t->image = shared;
                return shared.get();
            }
    // A live image over the same memory that the key kept apart (Tears of the Kingdom: the lighting read empty shadow
    // and depth-derived buffers; MacNativeGames shares across formats of one texel size). Logged once per pair of
    // keys: what differs.
    if (share && t->memory) {
        static std::set<std::pair<StorageKey, StorageKey>> seen;
        static int logged = 0;
        for (auto it = g_live_images.lower_bound(StorageKey{t->memory}); it != g_live_images.end() && it->first.memory == t->memory; ++it) {
            auto other = it->second.lock();
            if (!other || !other->image || logged >= 200 || !seen.insert({key, it->first}).second) continue;
            const StorageKey& o = it->first;
            logged++;
            log("nvn/vk: no image share at %llx: this %ux%ux%u fmt %#x levels %u target %u samples %u%s; live %ux%ux%u fmt %#x "
                "levels %u target %u samples %u%s",
                (unsigned long long)t->memory, key.width, key.height, key.depth, key.format, key.levels, key.target,
                key.samples, widened ? " (widened R8)" : "", o.width, o.height, o.depth, o.format, o.levels, o.target,
                o.samples, other->rendered ? " rendered" : "");
        }
    }
    if ((t->image = reuse_image(*t))) return t->image->guest_memory = t->memory, t->image.get();
    auto img = std::make_shared<GpuImage>();
    img->format = bc_host_decode(t->format) ? bc_decoded_format(t->format) : vk_format(t->format);  // nvn_bc.cpp
    img->nvn_format = t->format;
    img->guest_memory = t->memory;
    if (t->format == 0x1 && t->pool_swizzle == 0x02010101u) {
        img->format = VK_FORMAT_R8G8B8A8_UNORM;
        img->alpha_from_r = true;
    }
    uint32_t aspect = format_aspect(t->format);
    img->aspect = aspect == 0 ? VK_IMAGE_ASPECT_COLOR_BIT
                  : aspect == 1 ? VK_IMAGE_ASPECT_DEPTH_BIT
                  : aspect == 2 ? VK_IMAGE_ASPECT_STENCIL_BIT
                                : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    bool one_d = vk_1d_images() && (t->target == 0 || t->target == 3);  // else 2D images of height 1 (G8)
    img->width = std::max(1u, t->width);
    img->height = (t->target == 0 || t->target == 3) && !one_d ? 1u : std::max(1u, t->height);
    img->levels = std::max(1u, t->levels);
    uint32_t depth = std::max(1u, t->target == 3 && !one_d && t->depth <= 1 ? t->height : t->depth);  // GL-style layers
    bool is3d = t->target == 2, cube = t->target == 8 || t->target == 9;
    bool array = t->target == 3 || t->target == 4 || t->target == 6 || t->target == 9;
    img->layers = is3d ? 1 : (array || cube) ? depth : 1;
    img->depth = is3d ? depth : 1;
    if (cube && img->layers < 6) img->layers = 6;
    // NVN takes more levels than the size allows (a 1x1 texture with 5), Metal aborts on it (DEBUG.md #27).
    uint32_t largest = std::max({img->width, img->height, img->depth}), fit = 1;
    while (largest >>= 1) fit++;
    img->levels = std::min(img->levels, fit);
    img->view_type = is3d             ? VK_IMAGE_VIEW_TYPE_3D
                     : t->target == 9 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                     : cube           ? VK_IMAGE_VIEW_TYPE_CUBE
                     : one_d          ? (t->target == 0 ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_1D_ARRAY)
                     : array          ? VK_IMAGE_VIEW_TYPE_2D_ARRAY  // 1D array (G8), 2D array, 2D MS array
                                      : VK_IMAGE_VIEW_TYPE_2D;
    if (img->format == VK_FORMAT_UNDEFINED) {
        log("nvn/vk: texture %llx has format %#x with no Vulkan equivalent", (unsigned long long)guest, t->format);
        img->format = VK_FORMAT_R8G8B8A8_UNORM;
        img->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        aspect = 0;
    }
    bool compressed = false;
    {
        uint32_t bw = 1, bh = 1, bytes = 4;
        format_block(t->format, bw, bh, bytes);
        compressed = bw > 1 || bh > 1;
    }
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    // 3D textures are rendered one slice at a time (a colour-grading LUT): 2D views of a slice need this flag
    // (Metal otherwise refuses a 2D view of a 3D texture).
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : is3d ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0;
    // Colour images may be viewed in another format of the same size (an sRGB view of RGBA8, docs/nvn/DEBUG.md #29).
    // With ubwc_flags (Android): only where a view can differ, and with the list of those formats, as Adreno keeps
    // its framebuffer compression (UBWC) on an image whose views are listed; MUTABLE_FORMAT alone turns it off
    // (docs/nvn/android-perf/PHASE4.md G1a). In every run the only reinterpretation was RGBA8 viewed as sRGB.
    VkFormat listed[2] = {};
    VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    img->mutable_format = aspect == 0 && !img->alpha_from_r;
    if (img->mutable_format && ubwc_flags()) {
        auto pair = [&](VkFormat a, VkFormat b) {
            if (img->format != a && img->format != b) return false;
            listed[0] = a, listed[1] = b;
            return true;
        };
        if (pair(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB) ||
            pair(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB)) {
            list.viewFormatCount = 2, list.pViewFormats = listed;
            ci.pNext = &list;
        } else {
            img->mutable_format = compressed;  // block formats: no UBWC to keep, views of the sRGB variant as before
        }
    }
    if (img->mutable_format) ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    ci.imageType = is3d ? VK_IMAGE_TYPE_3D : one_d ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
    ci.format = img->format;
    ci.extent = {img->width, ci.imageType == VK_IMAGE_TYPE_1D ? 1 : img->height, is3d ? depth : 1};
    ci.mipLevels = img->levels;
    ci.arrayLayers = img->layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!compressed) ci.usage |= aspect ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    {
        VkFormatProperties fp{};
        p_vkGetPhysicalDeviceFormatProperties(vk_physical(), ci.format, &fp);
        img->storage = !compressed && !aspect && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
        if (img->storage) ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!image_supported(ci, t->format)) return nullptr;
    if (!check(p_vkCreateImage(vk_device(), &ci, nullptr, &img->image), "vkCreateImage")) return nullptr;
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(vk_device(), img->image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!check(p_vkAllocateMemory(vk_device(), &ai, nullptr, &img->memory), "vkAllocateMemory")) return nullptr;
    p_vkBindImageMemory(vk_device(), img->image, img->memory, 0);
    constexpr VkComponentSwizzle kSwizzle[] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R,
                                               VK_COMPONENT_SWIZZLE_G,    VK_COMPONENT_SWIZZLE_B,   VK_COMPONENT_SWIZZLE_A};
    img->swizzle = {kSwizzle[t->swizzle[0]], kSwizzle[t->swizzle[1]], kSwizzle[t->swizzle[2]], kSwizzle[t->swizzle[3]]};
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img->image;
    vi.viewType = img->view_type;
    vi.format = img->format;
    // A sampled depth/stencil view reads depth.
    vi.subresourceRange = {aspect == 3 ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : img->aspect, 0, img->levels, 0,
                           img->layers};
    check(p_vkCreateImageView(vk_device(), &vi, nullptr, &img->view), "vkCreateImageView");
    static uint64_t created = 0;
    if (g_trace || ++created % 1000 == 0)  // a sample of what keeps being created
        log("nvn/vk: image %llu for texture %llx (memory %llx): %ux%u x%u layers, %u levels, target %u format %#x -> %d",
            (unsigned long long)created, (unsigned long long)guest, (unsigned long long)t->memory,
            img->width, img->height, img->layers, img->levels, t->target, t->format, int(img->format));
    t->image = img;
    if (t->memory) g_live_images[key] = img;
    return img.get();
}

// ---- alias sync ----------------------------------------------------------------------------------------------
// Tears of the Kingdom renders a target in one format and samples the same memory through a texture of another
// format of the same texel size (R32F or RG16F read as RGBA8, layer 0 of an R11G11B10F array read as a 2D texture):
// on the Switch that is one memory; here they are two images, and the reader saw a cleared one (the lighting
// composite read zero shadow, AO and depth-derived inputs: a black world). The reader gets a copy of the newest
// contents (through a buffer: the bits, as the Switch's one memory holds them). NX_ALIAS_SYNC=0: off (bisecting).
namespace {
uint64_t g_content_serial = 0;
std::unordered_map<uint64_t, uint64_t> g_memory_newest;  // guest memory -> newest content_at of an image over it
bool alias_sync_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_ALIAS_SYNC");
        return !e || e[0] != 0;
    }();
    return on;
}
uint32_t texel_bytes(const GpuImage& img) {  // 0: not a plain colour format
    if (img.aspect != VK_IMAGE_ASPECT_COLOR_BIT || img.alpha_from_r || bc_host_decode(img.nvn_format)) return 0;
    if (vk_format(img.nvn_format) != img.format) return 0;  // substituted: another texel size on the host
    uint32_t bw = 1, bh = 1, bytes = 0;
    format_block(img.nvn_format, bw, bh, bytes);
    return bw == 1 && bh == 1 ? bytes : 0;
}
bool alias_compatible(const GpuImage& a, const GpuImage& b) {
    if (a.width != b.width || a.height != b.height || a.depth != 1 || b.depth != 1) return false;
    if (a.view_type == VK_IMAGE_VIEW_TYPE_3D || b.view_type == VK_IMAGE_VIEW_TYPE_3D) return false;
    uint32_t n = texel_bytes(a);
    return n && n == texel_bytes(b);
}
}  // namespace

void image_written(GpuImage& img) {
    img.content_at = ++g_content_serial;
    if (img.guest_memory) g_memory_newest[img.guest_memory] = img.content_at;
}

bool alias_pending(const GpuImage& img) {
    if (!img.guest_memory || !alias_sync_on()) return false;
    auto it = g_memory_newest.find(img.guest_memory);
    return it != g_memory_newest.end() && it->second > img.content_at && it->second > img.alias_checked;
}

bool sync_alias(VkCommandBuffer cb, GpuImage& img) {
    if (!alias_pending(img)) return false;
    if (pass_open()) {  // no transfer inside a render pass: the caller ends it first (sampled_ready said not ready)
        static uint64_t inside = 0;
        if (inside++ < 20) log("nvn/vk: alias sync at %llx skipped: a render pass is open", (unsigned long long)img.guest_memory);
        return false;
    }
    img.alias_checked = g_memory_newest[img.guest_memory];
    std::shared_ptr<GpuImage> best;
    for (auto it = g_live_images.lower_bound(StorageKey{img.guest_memory});
         it != g_live_images.end() && it->first.memory == img.guest_memory; ++it) {
        auto other = it->second.lock();
        if (!other || other.get() == &img || !other->image || !other->rendered || other->content_at <= img.content_at)
            continue;
        if (best && other->content_at <= best->content_at) continue;
        if (alias_compatible(img, *other)) best = other;
    }
    if (!best) return false;
    GpuImage& src = *best;
    // Through a buffer (image -> buffer -> image, the paths uploads and readbacks take): v00.19 copied image to image
    // across formats (vkCmdCopyImage) and the console killed the game two frames into gameplay.
    const uint32_t bytes = texel_bytes(img);
    uint32_t levels = std::min(src.levels, img.levels), layers = std::min(src.layers, img.layers);
    std::vector<VkBufferImageCopy> regions(levels);
    uint64_t size = 0;
    for (uint32_t l = 0; l < levels; l++) {
        VkBufferImageCopy& r = regions[l];
        r.bufferOffset = size;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, layers};
        r.imageExtent = {std::max(1u, img.width >> l), std::max(1u, img.height >> l), 1};
        size += (uint64_t(r.imageExtent.width) * r.imageExtent.height * layers * bytes + 255) & ~uint64_t(255);
    }
    Upload u = size < (1u << 20) ? upload_alloc(size, 256) : staging_alloc(size);
    if (!u.buffer) return false;
    for (auto& r : regions) r.bufferOffset += u.offset;
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr,
                           0, nullptr);
    transition(cb, src, VK_IMAGE_LAYOUT_GENERAL);
    transition(cb, img, VK_IMAGE_LAYOUT_GENERAL);
    p_vkCmdCopyImageToBuffer(cb, src.image, VK_IMAGE_LAYOUT_GENERAL, u.buffer, levels, regions.data());
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0,
                           nullptr);
    p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_GENERAL, levels, regions.data());
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr,
                           0, nullptr);
    img.rendered = img.uploaded = true;
    img.content_at = src.content_at;  // the same contents: the source does not copy them back
    static uint64_t copies = 0;
    if (++copies <= 60 || copies % 5000 == 0)
        log("nvn/vk: alias sync #%llu at %llx: %ux%u fmt %#x (%u levels, %u layers) <- fmt %#x (%u levels, %u layers)",
            (unsigned long long)copies, (unsigned long long)img.guest_memory, img.width, img.height, img.nvn_format,
            img.levels, img.layers, src.nvn_format, src.levels, src.layers);
    return true;
}

void transition(VkCommandBuffer cb, GpuImage& img, VkImageLayout to) {
    if (img.layout == to) return;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = img.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {img.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
    img.layout = to;
}

}  // namespace nx::nvn
