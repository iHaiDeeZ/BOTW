// Texture storage layout (docs/NVN.md, N4): how many bytes of a memory pool an NVN texture occupies, and how it is
// aligned. The SDK driver computes this through its device's surface-layout object, which only its own
// nvnDeviceInitialize (talking to the GPU kernel driver) sets up, so we compute it here: Maxwell block-linear.
//
// Format facts come from the driver's own format table (found in the sdk, find_format_table; 0x30 bytes per NVNformat):
//   word 2: block width (byte 0), block height (byte 1); word 4: bytes per block (byte 0).
// Block linear: the image is cut into GOBs of 64 bytes x 8 rows; GOBs stack vertically into blocks of
// 1..16 GOBs (the block height, picked per mip level from the level's height); a level's rows of blocks are
// 64-byte aligned in width and block-aligned in height. Pre-tiled data (BNTX) is laid out the same way, so the
// sizes must not come out smaller than the real driver's.
#include <algorithm>

#include "nvn.h"

namespace nx::nvn {

namespace {

constexpr uint32_t kFormatCount = 0xd0;
constexpr uint64_t kEntry = 0x30;

struct FormatInfo {
    uint32_t block_w = 1, block_h = 1, bytes = 4;
};

// Where the driver's format table is: searched for in the sdk, since its address moves with the SDK version
// (sdk+0xd34198 in Wonder's SDK 16.2, sdk+0xd015f8 in Tears of the Kingdom's). Recognized by a few entries' block
// width, height and bytes: R8 (0x01) 1x1x1, RGBA8 (0x25) 1x1x4, BC1 (0x42) 4x4x8, ASTC 8x8 (0x80, 0x8e) 8x8x16.
uint64_t find_format_table() {
    uint64_t loader = find_symbol("nvnBootstrapLoader");
    const LoadedModule* m = module_at(loader);
    if (!m) return 0;
    struct Probe {
        uint32_t format, bw, bh, bytes;
    };
    static constexpr Probe kProbes[] = {{0x01, 1, 1, 1}, {0x25, 1, 1, 4}, {0x42, 4, 4, 8}, {0x80, 8, 8, 16},
                                        {0x8e, 8, 8, 16}};
    auto matches = [](uint64_t table) {
        for (const Probe& p : kProbes) {
            const uint32_t* e = reinterpret_cast<const uint32_t*>(table + p.format * kEntry);
            if ((e[2] & 0xFF) != p.bw || ((e[2] >> 8) & 0xFF) != p.bh || (e[4] & 0xFF) != p.bytes) return false;
        }
        return true;
    };
    for (uint64_t a = m->base; a + kFormatCount * kEntry <= m->base + m->size; a += 8)
        if (matches(a)) {
            log("nvn: format table at sdk+%llx", (unsigned long long)(a - m->base));
            return a;
        }
    log("nvn: format table not found in the sdk: every format is taken as 1x1 blocks of 4 bytes");
    return 0;
}

FormatInfo format_info(uint32_t format) {
    static const uint64_t table = find_format_table();
    FormatInfo f;
    if (!table || format >= kFormatCount) return f;
    const uint32_t* e = reinterpret_cast<const uint32_t*>(table + uint64_t(format) * kEntry);
    uint32_t bw = e[2] & 0xFF, bh = (e[2] >> 8) & 0xFF, bytes = e[4] & 0xFF;
    if (bw && bh && bytes) f = {bw, bh, bytes};
    return f;
}

uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

// Block height in GOBs for a level `rows` block rows tall: the smallest power of two >= rows / 8, at most 16.
uint32_t block_height_gobs(uint32_t rows) {
    uint32_t gobs = div_up(rows, 8), h = 1;
    while (h < gobs && h < 16) h <<= 1;
    return h;
}

struct Layout {
    uint64_t size = 0, alignment = 512;
};

Layout compute(CPU& c, uint64_t builder) {
    uint32_t format = uint32_t(driver(c, "nvnTextureBuilderGetFormat", {builder}));
    uint32_t width = std::max<uint32_t>(1, uint32_t(driver(c, "nvnTextureBuilderGetWidth", {builder})));
    uint32_t height = std::max<uint32_t>(1, uint32_t(driver(c, "nvnTextureBuilderGetHeight", {builder})));
    uint32_t depth = std::max<uint32_t>(1, uint32_t(driver(c, "nvnTextureBuilderGetDepth", {builder})));
    uint32_t levels = std::max<uint32_t>(1, uint32_t(driver(c, "nvnTextureBuilderGetLevels", {builder})));
    uint32_t samples = std::max<uint32_t>(1, uint32_t(driver(c, "nvnTextureBuilderGetSamples", {builder})));
    uint32_t stride = uint32_t(driver(c, "nvnTextureBuilderGetStride", {builder}));
    FormatInfo f = format_info(format);
    Layout l;
    if (stride) {  // linear (pitch) texture
        l.size = uint64_t(stride) * div_up(height, f.block_h) * depth;
        l.alignment = 0x100;
        return l;
    }
    uint64_t layer = 0;
    uint32_t bh0 = block_height_gobs(div_up(height, f.block_h));
    for (uint32_t level = 0; level < levels; level++) {
        uint32_t w = std::max(1u, width >> level), h = std::max(1u, height >> level);
        uint32_t cols = div_up(w, f.block_w), rows = div_up(h, f.block_h);
        uint32_t bh = std::min(bh0, block_height_gobs(rows));
        uint64_t pitch = (uint64_t(cols) * f.bytes + 63) & ~63ull;
        uint64_t tall = (uint64_t(rows) + 8 * bh - 1) / (8 * bh) * (8 * bh);
        layer += pitch * tall;
    }
    uint64_t block = 512ull * bh0;  // one block: 64 bytes x 8 rows x block height
    layer = (layer + block - 1) / block * block;
    l.size = layer * depth * samples;
    l.alignment = block;
    if (l.size >= (32ull << 20)) {  // the big ones (Tears of the Kingdom's MaterialAlb array): checked against the game's
        static std::atomic<int> logged{0};
        if (logged++ < 40)
            log("nvn: storage size %llu (layer %llu, block height %u GOBs) for %ux%ux%u, %u levels, format %#x, %u samples",
                (unsigned long long)l.size, (unsigned long long)layer, bh0, width, height, depth, levels, format, samples);
    }
    return l;
}

// nvnTextureGetViewOffset(texture, view): where the view's first level and layer start in the texture's storage,
// in the layout compute() sizes (each layer holds every level, block-aligned). Tears of the Kingdom streams texture
// mips (TexToGo) into pool memory at these offsets; the SDK driver cannot answer (its device is never initialized).
void GetViewOffset(CPU& c) {
    Texture* t = c.x[0] ? get<Texture>(c.x[0]) : nullptr;
    ViewInfo v = read_view(c, c.x[1]);
    if (!t || (!v.base_level && !v.min_layer)) {
        c.x[0] = 0;
        return;
    }
    FormatInfo f = format_info(t->format);
    uint32_t width = std::max(1u, t->width), height = std::max(1u, t->height);
    uint32_t levels = std::max(1u, t->levels);
    if (t->stride) {  // linear: one level, layers one after another
        c.x[0] = uint64_t(v.min_layer) * t->stride * div_up(height, f.block_h);
        return;
    }
    bool is3d = t->target == 2;  // NVN_TEXTURE_TARGET_3D: levels hold all their slices
    uint32_t depth = is3d ? std::max(1u, t->depth) : 1;
    uint32_t bh0 = t->block_height_gobs ? t->block_height_gobs : block_height_gobs(div_up(height, f.block_h));
    uint64_t at = 0, level_offset = 0, layer = 0;
    for (uint32_t level = 0; level < levels; level++) {
        if (level == v.base_level) level_offset = at;
        uint32_t w = std::max(1u, width >> level), h = std::max(1u, height >> level);
        uint32_t d = std::max(1u, depth >> level);
        uint32_t cols = div_up(w, f.block_w), rows = div_up(h, f.block_h);
        uint32_t bh = std::min(bh0, block_height_gobs(rows));
        uint64_t pitch = (uint64_t(cols) * f.bytes + 63) & ~63ull;
        uint64_t tall = (uint64_t(rows) + 8 * bh - 1) / (8 * bh) * (8 * bh);
        at += pitch * tall * d;
    }
    uint64_t block = 512ull * bh0;
    layer = (at + block - 1) / block * block;
    c.x[0] = level_offset + (is3d ? 0 : uint64_t(v.min_layer) * layer);
}

void GetStorageSize(CPU& c) { c.x[0] = compute(c, c.x[0]).size; }            // size_t (const NVNtextureBuilder*)
void GetStorageAlignment(CPU& c) { c.x[0] = compute(c, c.x[0]).alignment; }  // size_t (const NVNtextureBuilder*)
void GetStorageClass(CPU& c) { c.x[0] = 0; }  // NVNstorageClass: only matters for compression / sparse tiles

}  // namespace

uint64_t texture_storage_size(CPU& c, uint64_t builder) { return compute(c, builder).size; }

void format_block(uint32_t format, uint32_t& block_w, uint32_t& block_h, uint32_t& bytes) {
    FormatInfo f = format_info(format);
    block_w = f.block_w, block_h = f.block_h, bytes = f.bytes;
}

NX_HLE("nvnTextureBuilderGetStorageSize", GetStorageSize);
NX_HLE("nvnTextureGetViewOffset", GetViewOffset);
NX_HLE("nvnTextureBuilderGetStorageAlignment", GetStorageAlignment);
NX_HLE("nvnTextureBuilderGetStorageClass", GetStorageClass);
NX_HLE("nvnTextureBuilderGetRawStorageClass", GetStorageClass);

}  // namespace nx::nvn
