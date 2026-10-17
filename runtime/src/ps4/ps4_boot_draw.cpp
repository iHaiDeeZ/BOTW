// PS4 boot screen pictures: see ps4_boot_draw.h. The band that changes is composed in a host buffer (a copy of the
// background under it, then the bar and the glyphs blended in) and copied out row by row.
#include "ps4_boot_draw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace nx::plat::ps4 {

namespace {

// Layout (1920x1080), as design A: the logo sits in the art at a third of the height; below it the bar, the stage
// on the left and the percentage on the right under the bar, the credit line below them.
constexpr int kBarX0 = 345, kBarX1 = 1575, kBarY = 618, kBarH = 36, kBorder = 5;
constexpr int kTextY = 676;     // top of the stage / percentage cells
constexpr int kCreditY = 760;   // top of the credit line's cells
constexpr int kBandX0 = 300, kBandX1 = 1620, kBandY0 = 600, kBandY1 = 830;  // everything that changes

struct Rgb {
    uint8_t r, g, b;
};
constexpr Rgb kTrack{30, 44, 114}, kFill{255, 194, 31}, kWhite{255, 255, 255};

std::vector<uint8_t> g_bg;  // kBootWidth x kBootHeight RGBA
struct Font {
    uint16_t cell_w = 0, cell_h = 0;
    uint8_t first = 0, count = 0;
    std::vector<uint8_t> advance, cells;  // RGBA cells
} g_font;

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? size_t(n) : 0);
    bool ok = n > 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

// The band: x kBandX0..kBandX1, y kBandY0..kBandY1, RGBA.
constexpr int kBandW = kBandX1 - kBandX0, kBandH = kBandY1 - kBandY0;
std::vector<uint8_t> g_band(size_t(kBandW) * kBandH * 4);

void band_put(int x, int y, Rgb c, int alpha = 255) {
    x -= kBandX0, y -= kBandY0;
    if (x < 0 || y < 0 || x >= kBandW || y >= kBandH) return;
    uint8_t* p = &g_band[(size_t(y) * kBandW + x) * 4];
    p[0] = uint8_t((c.r * alpha + p[0] * (255 - alpha)) / 255);
    p[1] = uint8_t((c.g * alpha + p[1] * (255 - alpha)) / 255);
    p[2] = uint8_t((c.b * alpha + p[2] * (255 - alpha)) / 255);
    p[3] = 255;
}

// A pill (rounded ends of radius h/2) from x0 to x1 at y, h high; edges anti-aliased by coverage.
void pill(int x0, int x1, int y, int h, Rgb c) {
    if (x1 <= x0) return;
    const float r = h / 2.0f;
    for (int py = y; py < y + h; py++)
        for (int px = x0; px < x1; px++) {
            float cx = px + 0.5f, cy = py + 0.5f - (y + r);
            float dx = cx < x0 + r ? x0 + r - cx : cx > x1 - r ? cx - (x1 - r) : 0.0f;
            float d = std::sqrt(dx * dx + cy * cy) - r;  // < 0 inside
            float cover = std::clamp(0.5f - d, 0.0f, 1.0f);
            if (cover > 0) band_put(px, py, c, int(cover * 255));
        }
}

int text_width(const std::string& s) {
    int w = 0;
    for (unsigned char ch : s)
        if (ch >= g_font.first && ch < g_font.first + g_font.count) w += g_font.advance[ch - g_font.first];
    return w;
}

void text(const std::string& s, int x, int y) {
    for (unsigned char ch : s) {
        if (ch < g_font.first || ch >= g_font.first + g_font.count) continue;
        int k = ch - g_font.first;
        const uint8_t* cell = &g_font.cells[size_t(k) * g_font.cell_w * g_font.cell_h * 4];
        for (int cy = 0; cy < g_font.cell_h; cy++)
            for (int cx = 0; cx < g_font.cell_w; cx++) {
                const uint8_t* p = cell + (size_t(cy) * g_font.cell_w + cx) * 4;
                if (p[3]) band_put(x + cx, y + cy, Rgb{p[0], p[1], p[2]}, p[3]);
            }
        x += g_font.advance[k];
    }
}

}  // namespace

bool boot_draw_load(const std::string& dir) {
    std::vector<uint8_t> font;
    if (!read_file(dir + "/background.rgba", g_bg) || g_bg.size() != size_t(kBootWidth) * kBootHeight * 4) return false;
    if (!read_file(dir + "/font.bin", font) || font.size() < 10 || std::memcmp(font.data(), "SMWF", 4) != 0) return false;
    g_font.cell_w = uint16_t(font[4] | font[5] << 8);
    g_font.cell_h = uint16_t(font[6] | font[7] << 8);
    g_font.first = font[8];
    g_font.count = font[9];
    size_t cells = size_t(g_font.count) * g_font.cell_w * g_font.cell_h * 4;
    if (font.size() != 10 + g_font.count + cells) return false;
    g_font.advance.assign(font.begin() + 10, font.begin() + 10 + g_font.count);
    g_font.cells.assign(font.begin() + 10 + g_font.count, font.end());
    return true;
}

void boot_draw(uint8_t* fb, uint32_t pitch, const BootView& view, bool full) {
    if (full)
        for (uint32_t y = 0; y < kBootHeight; y++)
            std::memcpy(fb + size_t(y) * pitch * 4, &g_bg[size_t(y) * kBootWidth * 4], size_t(kBootWidth) * 4);
    // the band: background, then the bar and the texts
    for (int y = 0; y < kBandH; y++)
        std::memcpy(&g_band[size_t(y) * kBandW * 4], &g_bg[(size_t(kBandY0 + y) * kBootWidth + kBandX0) * 4],
                    size_t(kBandW) * 4);
    pill(kBarX0 - kBorder, kBarX1 + kBorder, kBarY - kBorder, kBarH + 2 * kBorder, kWhite);
    pill(kBarX0, kBarX1, kBarY, kBarH, kTrack);
    int pct = std::clamp(view.percent, 0, 100);
    int fill = (kBarX1 - kBarX0) * pct / 100;
    if (fill >= kBarH) pill(kBarX0, kBarX0 + fill, kBarY, kBarH, kFill);
    text(view.stage, kBarX0, kTextY);
    std::string p = std::to_string(pct) + "%";
    text(p, kBarX1 - text_width(p), kTextY);
    text(view.credit, (int(kBootWidth) - text_width(view.credit)) / 2, kCreditY);
    for (int y = 0; y < kBandH; y++)
        std::memcpy(fb + (size_t(kBandY0 + y) * pitch + kBandX0) * 4, &g_band[size_t(y) * kBandW * 4],
                    size_t(kBandW) * 4);
}

}  // namespace nx::plat::ps4
