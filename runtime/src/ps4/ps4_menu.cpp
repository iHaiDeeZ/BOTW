// PS4 quick menu: see ps4_menu.h. Drawn by the CPU (the boot screen's font, /app0/boot/font.bin) into the boot
// screen's write-combined VideoOut buffers: each gets the dimmed frame once, then only the panel is composed (in a host
// buffer, since write-combined memory is slow to read) and copied out.
#include "ps4_menu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <orbis/Pad.h>
#include <orbis/SystemService.h>
#include <orbis/VideoOut.h>

#include <nx/runtime.h>

namespace nx::plat::ps4 {

// platform_ps4.cpp: player 1's DualShock 4 as it is (buttons, left stick y), and a storyboard request.
bool menu_pad(uint32_t& buttons, uint8_t& left_y);
void request_diag();

namespace {

constexpr uint32_t kW = 1920, kH = 1080;
constexpr const char* kConfig = "/data/SMW/config.txt";

// ---- settings -----------------------------------------------------------------------------------------------------

struct Res {
    const char* label;
    const char* value;  // NX_RENDER_RES, or null for handheld mode
};
constexpr Res kResolutions[] = {
    {"1280 x 720 (fastest)", "1280x720"}, {"1600 x 900", "1600x900"}, {"1920 x 1080 (sharpest)", "1920x1080"},
    {"Handheld mode (720p)", nullptr},
};
constexpr int kResCount = int(sizeof kResolutions / sizeof kResolutions[0]);
constexpr const char* kCores[] = {"balanced", "pinned", "spread"};

struct Ds4 {
    const char* name;  // in config.txt and on screen
    uint32_t bit;
};
constexpr Ds4 kDs4[] = {
    {"Cross", ORBIS_PAD_BUTTON_CROSS},       {"Circle", ORBIS_PAD_BUTTON_CIRCLE}, {"Square", ORBIS_PAD_BUTTON_SQUARE},
    {"Triangle", ORBIS_PAD_BUTTON_TRIANGLE}, {"L1", ORBIS_PAD_BUTTON_L1},         {"R1", ORBIS_PAD_BUTTON_R1},
    {"L2", ORBIS_PAD_BUTTON_L2},             {"R2", ORBIS_PAD_BUTTON_R2},         {"L3", ORBIS_PAD_BUTTON_L3},
    {"R3", ORBIS_PAD_BUTTON_R3},             {"Options", ORBIS_PAD_BUTTON_OPTIONS},
    {"Touchpad", ORBIS_PAD_BUTTON_TOUCH_PAD},
};
constexpr int kDs4Count = int(sizeof kDs4 / sizeof kDs4[0]);
constexpr const char* kNxNames[kNxCount] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Plus", "Minus",
                                            "Left stick click", "Right stick click"};
constexpr const char* kNxKeys[kNxCount] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LS", "RS"};

struct Settings {
    int res = 0;        // kResolutions
    bool fsr = true;
    float sharp = 0.2f;  // FSR stops: 0 sharpest, 2 none
    int cores = 0;      // kCores
    int map[kNxCount];  // kDs4 index per Switch button, -1 none
};
Settings g_set, g_started;  // now, and what this run started with (the rest-of-run settings)
bool g_cross = true;
std::atomic<bool> g_fsr{true};
std::atomic<float> g_sharp{0.2f};
std::atomic<uint32_t> g_map_bits[kNxCount];
std::atomic<bool> g_request{false}, g_open{false}, g_hold_after{false};

void default_map(int map[kNxCount]) {
    auto at = [](const char* n) {
        for (int k = 0; k < kDs4Count; k++)
            if (std::strcmp(kDs4[k].name, n) == 0) return k;
        return -1;
    };
    map[kNxA] = at(g_cross ? "Cross" : "Circle");
    map[kNxB] = at(g_cross ? "Circle" : "Cross");
    map[kNxX] = at("Triangle"), map[kNxY] = at("Square");
    map[kNxL] = at("L1"), map[kNxR] = at("R1"), map[kNxZL] = at("L2"), map[kNxZR] = at("R2");
    map[kNxPlus] = at("Options"), map[kNxMinus] = at("Touchpad"), map[kNxLS] = at("L3"), map[kNxRS] = at("R3");
}

void publish() {
    g_fsr = g_set.fsr;
    g_sharp = g_set.sharp;
    for (int k = 0; k < kNxCount; k++) g_map_bits[k] = g_set.map[k] >= 0 ? kDs4[g_set.map[k]].bit : 0;
}

std::string map_string(const int map[kNxCount]) {
    std::string s;
    for (int k = 0; k < kNxCount; k++) {
        if (!s.empty()) s += ',';
        s += kNxKeys[k];
        s += ':';
        s += map[k] >= 0 ? kDs4[map[k]].name : "none";
    }
    return s;
}

void parse_map(const char* text, int map[kNxCount]) {
    std::string s = text;
    size_t at = 0;
    while (at < s.size()) {
        size_t end = s.find(',', at);
        if (end == std::string::npos) end = s.size();
        std::string item = s.substr(at, end - at);
        at = end + 1;
        size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        std::string key = item.substr(0, colon), value = item.substr(colon + 1);
        for (int k = 0; k < kNxCount; k++)
            if (key == kNxKeys[k]) {
                map[k] = -1;
                for (int d = 0; d < kDs4Count; d++)
                    if (value == kDs4[d].name) map[k] = d;
            }
    }
}

// config.txt with the menu's keys replaced (other lines, comments included, stay as they are).
void save() {
    static const char* const kOwned[] = {"NX_RENDER_RES", "NX_HANDHELD", "NX_FSR", "NX_FSR_SHARP", "NX_PS4_CORES",
                                         "NX_PS4_MAP"};
    std::vector<std::string> lines;
    if (FILE* f = std::fopen(kConfig, "r")) {
        char buf[512];
        while (std::fgets(buf, sizeof buf, f)) {
            std::string line = buf;
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            bool owned = false;
            for (const char* k : kOwned)
                owned |= line.compare(0, std::strlen(k) + 1, std::string(k) + "=") == 0;
            if (!owned) lines.push_back(line);
        }
        std::fclose(f);
    }
    const Res& r = kResolutions[g_set.res];
    char sharp[16];
    std::snprintf(sharp, sizeof sharp, "%.1f", double(g_set.sharp));
    if (r.value)
        lines.push_back(std::string("NX_RENDER_RES=") + r.value), lines.push_back("NX_HANDHELD=0");
    else
        lines.push_back("NX_HANDHELD=1");
    lines.push_back(std::string("NX_FSR=") + (g_set.fsr ? "1" : "0"));
    lines.push_back(std::string("NX_FSR_SHARP=") + sharp);
    lines.push_back(std::string("NX_PS4_CORES=") + kCores[g_set.cores]);
    lines.push_back("NX_PS4_MAP=" + map_string(g_set.map));
    std::string tmp = std::string(kConfig) + ".tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "w")) {
        for (const std::string& l : lines) std::fprintf(f, "%s\n", l.c_str());
        bool ok = std::fclose(f) == 0;
        if (ok) std::rename(tmp.c_str(), kConfig);
    }
    log("menu: settings saved (%s, FSR %s %.1f, cores %s)", r.label, g_set.fsr ? "on" : "off", double(g_set.sharp),
        kCores[g_set.cores]);
}

// ---- drawing --------------------------------------------------------------------------------------------------------

struct Font {
    uint16_t cell_w = 0, cell_h = 0;
    uint8_t first = 0, count = 0;
    std::vector<uint8_t> advance, cells;  // RGBA cells
} g_font;

bool load_font() {
    if (!g_font.cells.empty()) return true;
    FILE* f = std::fopen("/app0/boot/font.bin", "rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    if (d.size() < 10 || std::memcmp(d.data(), "SMWF", 4) != 0) return false;
    g_font.cell_w = uint16_t(d[4] | d[5] << 8), g_font.cell_h = uint16_t(d[6] | d[7] << 8);
    g_font.first = d[8], g_font.count = d[9];
    size_t cells = size_t(g_font.count) * g_font.cell_w * g_font.cell_h * 4;
    if (d.size() != 10 + g_font.count + cells) return false;
    g_font.advance.assign(d.begin() + 10, d.begin() + 10 + g_font.count);
    g_font.cells.assign(d.begin() + 10 + g_font.count, d.end());
    return true;
}

// The panel, composed in a host buffer (x kPx..kPx+kPw, y kPy..kPy+kPh of the screen).
constexpr int kPw = 1180, kPh = 900, kPx = (int(kW) - kPw) / 2, kPy = (int(kH) - kPh) / 2;
std::vector<uint8_t> g_panel(size_t(kPw) * kPh * 4);
std::vector<uint8_t> g_dim;  // the frame, dimmed, kW x kH RGBA

struct Rgb {
    uint8_t r, g, b;
};

void put(int x, int y, Rgb c, int a) {
    if (x < 0 || y < 0 || x >= kPw || y >= kPh || a <= 0) return;
    uint8_t* p = &g_panel[(size_t(y) * kPw + x) * 4];
    p[0] = uint8_t((c.r * a + p[0] * (255 - a)) / 255);
    p[1] = uint8_t((c.g * a + p[1] * (255 - a)) / 255);
    p[2] = uint8_t((c.b * a + p[2] * (255 - a)) / 255);
}

void rect(int x0, int y0, int x1, int y1, Rgb c, int a) {
    for (int y = std::max(0, y0); y < std::min(kPh, y1); y++)
        for (int x = std::max(0, x0); x < std::min(kPw, x1); x++) put(x, y, c, a);
}

int text_width(const std::string& s, float scale) {
    int w = 0;
    for (unsigned char ch : s)
        if (ch >= g_font.first && ch < g_font.first + g_font.count) w += g_font.advance[ch - g_font.first];
    return int(w * scale);
}

// Glyphs scaled by `scale` (nearest cell texel), coloured `c` with the cell's alpha.
void text(const std::string& s, int x, int y, Rgb c, float scale = 0.8f) {
    float fx = float(x);
    const int ch_h = int(g_font.cell_h * scale), ch_w = int(g_font.cell_w * scale);
    for (unsigned char ch : s) {
        if (ch < g_font.first || ch >= g_font.first + g_font.count) continue;
        int k = ch - g_font.first;
        const uint8_t* cell = &g_font.cells[size_t(k) * g_font.cell_w * g_font.cell_h * 4];
        for (int cy = 0; cy < ch_h; cy++)
            for (int cx = 0; cx < ch_w; cx++) {
                int sx = std::min(int(cx / scale), g_font.cell_w - 1), sy = std::min(int(cy / scale), g_font.cell_h - 1);
                const uint8_t* p = cell + (size_t(sy) * g_font.cell_w + sx) * 4;
                if (p[3]) put(int(fx) + cx, y + cy, c, p[3]);
            }
        fx += g_font.advance[k] * scale;
    }
}

struct Row {
    std::string label, value;
    bool restart = false;  // applies at the next start
};

void draw_panel(const std::string& title, const std::vector<Row>& rows, int selected, const std::string& footer,
                const std::string& note) {
    for (int y = 0; y < kPh; y++)  // the dimmed frame under the panel
        std::memcpy(&g_panel[size_t(y) * kPw * 4], &g_dim[(size_t(kPy + y) * kW + kPx) * 4], size_t(kPw) * 4);
    rect(0, 0, kPw, kPh, Rgb{14, 20, 60}, 215);
    rect(0, 0, kPw, 6, Rgb{255, 194, 31}, 255);
    text(title, 50, 34, Rgb{255, 255, 255}, 1.0f);
    const int row_h = 58, top = 130;
    for (size_t k = 0; k < rows.size(); k++) {
        int y = top + int(k) * row_h;
        bool sel = int(k) == selected;
        if (sel) rect(30, y - 6, kPw - 30, y + row_h - 10, Rgb{255, 194, 31}, 235);
        Rgb c = sel ? Rgb{20, 24, 70} : Rgb{235, 238, 255};
        text(rows[k].label + (rows[k].restart ? " *" : ""), 60, y, c);
        if (!rows[k].value.empty()) {
            std::string v = rows[k].value;
            if (sel && !rows[k].restart && v.find('<') == std::string::npos) v = "< " + v + " >";
            text(v, kPw - 60 - text_width(v, 0.8f), y, c);
        }
    }
    text(footer, 50, kPh - 120, Rgb{180, 190, 230}, 0.6f);
    text(note, 50, kPh - 70, Rgb{255, 214, 120}, 0.6f);
}

// ---- the menu -------------------------------------------------------------------------------------------------------

enum class Page { Main, Controls, Bind };

struct Input {
    uint32_t held = 0, pressed = 0;
    int repeat_dir = 0, repeat_ticks = 0;
    bool up = false, down = false;  // with the stick and auto-repeat
};

void poll(Input& in) {
    uint32_t b = 0;
    uint8_t ly = 128;
    menu_pad(b, ly);
    if (ly < 50) b |= ORBIS_PAD_BUTTON_UP;
    if (ly > 206) b |= ORBIS_PAD_BUTTON_DOWN;
    in.pressed = b & ~in.held;
    in.held = b;
    int dir = (b & ORBIS_PAD_BUTTON_UP) ? -1 : (b & ORBIS_PAD_BUTTON_DOWN) ? 1 : 0;
    in.up = in.down = false;
    if (dir != in.repeat_dir) in.repeat_dir = dir, in.repeat_ticks = 0;
    if (dir && (in.repeat_ticks == 0 || (in.repeat_ticks > 12 && in.repeat_ticks % 4 == 0))) (dir < 0 ? in.up : in.down) = true;
    if (dir) in.repeat_ticks++;
}

}  // namespace

void settings_load(bool cross_confirms) {
    g_cross = cross_confirms;
    default_map(g_set.map);
    const char* h = std::getenv("NX_HANDHELD");
    const char* r = std::getenv("NX_RENDER_RES");
    g_set.res = 0;
    if (h && h[0] == '1') {
        g_set.res = kResCount - 1;
    } else if (r) {
        for (int k = 0; k < kResCount - 1; k++)
            if (std::strcmp(kResolutions[k].value, r) == 0) g_set.res = k;
    }
    const char* f = std::getenv("NX_FSR");
    g_set.fsr = !f || f[0] == '1';
    if (const char* s = std::getenv("NX_FSR_SHARP")) g_set.sharp = std::clamp(float(std::atof(s)), 0.0f, 2.0f);
    if (const char* c = std::getenv("NX_PS4_CORES"))
        for (int k = 0; k < 3; k++)
            if (std::strcmp(kCores[k], c) == 0) g_set.cores = k;
    if (const char* m = std::getenv("NX_PS4_MAP")) parse_map(m, g_set.map);
    g_started = g_set;
    publish();
}

uint32_t settings_ds4_for(int b) { return b >= 0 && b < kNxCount ? g_map_bits[b].load(std::memory_order_relaxed) : 0; }
bool settings_fsr() { return g_fsr.load(std::memory_order_relaxed); }
float settings_fsr_sharpness() { return g_sharp.load(std::memory_order_relaxed); }

void menu_request() {
    if (!g_open) g_request = true;
}
bool menu_requested() { return g_request.load(); }

bool menu_blocks_input(uint32_t ds4_buttons) {
    if (g_open) return true;
    if (g_hold_after) {  // after the menu: nothing until every button is up (no stray A press into the game)
        if (ds4_buttons) return true;
        g_hold_after = false;
    }
    return false;
}

void menu_run(const uint8_t* frame, uint32_t pitch, int video, int game_buffer, int64_t flip_arg) {
    g_request = false;
    uint8_t* bufs[2] = {boot_buffer(0), boot_buffer(1)};
    if (!bufs[0] || !bufs[1] || !load_font()) {
        log("menu: no boot screen buffers or font: no menu");
        return;
    }
    g_open = true;
    log("menu: open");
    // The frame, dimmed (read once: the swapchain image is write-combined too).
    g_dim.resize(size_t(kW) * kH * 4);
    std::vector<uint8_t> row(size_t(kW) * 4);
    for (uint32_t y = 0; y < kH; y++) {
        std::memcpy(row.data(), frame + size_t(y) * pitch * 4, row.size());
        uint8_t* d = &g_dim[size_t(y) * kW * 4];
        for (uint32_t x = 0; x < kW * 4; x += 4)
            d[x] = uint8_t(row[x] * 2 / 5), d[x + 1] = uint8_t(row[x + 1] * 2 / 5), d[x + 2] = uint8_t(row[x + 2] * 2 / 5),
            d[x + 3] = 255;
    }
    bool full[2] = {false, false};
    Page page = Page::Main;
    int sel = 0, ctl_sel = 0, bind_target = 0;
    std::string note = "* applies after a restart of the game";
    Input in;
    poll(in);  // the L3 + R3 that opened the menu: held, not pressed
    bool done = false, restart = false;
    int k = 0;
    for (uint64_t tick = 0; !done; tick++) {
        poll(in);
        std::vector<Row> rows;
        std::string title, footer;
        if (page == Page::Main) {
            char sharp[32];
            std::snprintf(sharp, sizeof sharp, "%.1f", double(g_set.sharp));
            title = "SMW quick menu";
            rows = {{"Resume", ""},
                    {"Render resolution", kResolutions[g_set.res].label, true},
                    {"Upscaling", g_set.fsr ? "FSR 1" : "Bilinear"},
                    {"FSR sharpness (0 = sharpest)", sharp},
                    {"CPU cores", kCores[g_set.cores], true},
                    {"Controls", ">"},
                    {"Capture a diagnostics frame", ""},
                    {"Restart the game now", ""}};
            footer = "Up/Down select   Left/Right change   Cross OK   Circle or L3+R3 close";
            int n = int(rows.size());
            if (in.up) sel = (sel + n - 1) % n;
            if (in.down) sel = (sel + 1) % n;
            int dx = (in.pressed & ORBIS_PAD_BUTTON_RIGHT) ? 1 : (in.pressed & ORBIS_PAD_BUTTON_LEFT) ? -1 : 0;
            bool ok = in.pressed & ORBIS_PAD_BUTTON_CROSS;
            if (ok && dx == 0) dx = 1;
            switch (sel) {
                case 0: if (ok) done = true; break;
                case 1: if (dx) g_set.res = (g_set.res + kResCount + dx) % kResCount; break;
                case 2: if (dx) g_set.fsr = !g_set.fsr; break;
                case 3:
                    if (dx) g_set.sharp = std::clamp(std::round((g_set.sharp + 0.1f * float(dx)) * 10.0f) / 10.0f, 0.0f, 2.0f);
                    break;
                case 4: if (dx) g_set.cores = (g_set.cores + 3 + dx) % 3; break;
                case 5: if (ok || dx > 0) page = Page::Controls, ctl_sel = 0; break;
                case 6:
                    if (ok) request_diag(), note = "A storyboard of the next frame goes to /data/SMW/diag", done = true;
                    break;
                case 7: if (ok) restart = done = true; break;
            }
            if (in.pressed & ORBIS_PAD_BUTTON_CIRCLE) done = true;
            if ((in.held & (ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3)) == (ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3) &&
                (in.pressed & (ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3)))
                done = true;
        } else if (page == Page::Controls) {
            title = "Controls";
            for (int b = 0; b < kNxCount; b++)
                rows.push_back({std::string("Switch ") + kNxNames[b], g_set.map[b] >= 0 ? kDs4[g_set.map[b]].name : "none"});
            rows.push_back({"Reset to defaults", ""});
            rows.push_back({"Back", ""});
            footer = "Cross: press the new button for it   Circle back";
            int n = int(rows.size());
            if (in.up) ctl_sel = (ctl_sel + n - 1) % n;
            if (in.down) ctl_sel = (ctl_sel + 1) % n;
            if (in.pressed & ORBIS_PAD_BUTTON_CROSS) {
                if (ctl_sel < kNxCount) page = Page::Bind, bind_target = ctl_sel;
                else if (ctl_sel == kNxCount) default_map(g_set.map), publish();
                else page = Page::Main;
            }
            if (in.pressed & ORBIS_PAD_BUTTON_CIRCLE) page = Page::Main;
        } else {  // Bind: the next button pressed (after everything was let go)
            title = "Controls";
            rows = {{std::string("Press the button for Switch ") + kNxNames[bind_target], ""},
                    {"(Up/Down/Left/Right on the D-pad cancel)", ""}};
            footer = "";
            static bool released = false;
            if (in.held == 0) released = true;
            if (released && in.pressed) {
                uint32_t p = in.pressed;
                if (p & (ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN | ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT)) {
                    page = Page::Controls, released = false;
                } else {
                    for (int d = 0; d < kDs4Count; d++)
                        if (p & kDs4[d].bit) {
                            // the button this one had goes to the one that had the new button (a swap)
                            for (int b = 0; b < kNxCount; b++)
                                if (b != bind_target && g_set.map[b] == d) g_set.map[b] = g_set.map[bind_target];
                            g_set.map[bind_target] = d;
                            break;
                        }
                    publish();
                    page = Page::Controls, released = false;
                }
            }
        }
        draw_panel(title, rows, page == Page::Main ? sel : page == Page::Controls ? ctl_sel : -1, footer, note);
        if (!full[k]) {  // this buffer's first frame: the whole dimmed picture
            for (uint32_t y = 0; y < kH; y++)
                std::memcpy(bufs[k] + size_t(y) * kW * 4, &g_dim[size_t(y) * kW * 4], size_t(kW) * 4);
            full[k] = true;
        }
        for (int y = 0; y < kPh; y++)
            std::memcpy(bufs[k] + (size_t(kPy + y) * kW + kPx) * 4, &g_panel[size_t(y) * kPw * 4], size_t(kPw) * 4);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);  // write-combined stores out before the flip
        sceVideoOutSubmitFlip(video, boot_buffer_index(k), ORBIS_VIDEO_OUT_FLIP_VSYNC, flip_arg);
        k ^= 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(33));  // the other buffer is off screen by then
        if (!done) publish();  // FSR and the mapping take effect at once
    }
    publish();
    save();
    if (restart) {
        log("menu: restarting the game");
        const char* args[] = {nullptr};
        sceSystemServiceLoadExec("/app0/eboot.bin", args);
    }
    sceVideoOutSubmitFlip(video, game_buffer, ORBIS_VIDEO_OUT_FLIP_VSYNC, flip_arg);
    g_hold_after = true;
    g_open = false;
    log("menu: closed");
}

}  // namespace nx::plat::ps4
