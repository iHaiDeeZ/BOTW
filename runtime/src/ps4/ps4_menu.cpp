// PS4 quick menu: see ps4_menu.h. Drawn by the CPU into the boot screen's write-combined VideoOut buffers: each gets
// the dimmed frame once, then only the panel is composed (in a host buffer, since write-combined memory is slow to
// read) and copied out. Text: /app0/boot/menu_font.bin (plain glyphs, coloured here, with a drop shadow); the title
// uses the boot screen's outlined font.bin as it is. SMW_MENU_PREVIEW builds the drawing alone on a PC
// (scripts/menu-preview.sh): no PS4 calls then.
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

#if defined(SMW_MENU_PREVIEW)
enum : uint32_t {
    ORBIS_PAD_BUTTON_L3 = 0x0002, ORBIS_PAD_BUTTON_R3 = 0x0004, ORBIS_PAD_BUTTON_OPTIONS = 0x0008,
    ORBIS_PAD_BUTTON_UP = 0x0010, ORBIS_PAD_BUTTON_RIGHT = 0x0020, ORBIS_PAD_BUTTON_DOWN = 0x0040,
    ORBIS_PAD_BUTTON_LEFT = 0x0080, ORBIS_PAD_BUTTON_L2 = 0x0100, ORBIS_PAD_BUTTON_R2 = 0x0200,
    ORBIS_PAD_BUTTON_L1 = 0x0400, ORBIS_PAD_BUTTON_R1 = 0x0800, ORBIS_PAD_BUTTON_TRIANGLE = 0x1000,
    ORBIS_PAD_BUTTON_CIRCLE = 0x2000, ORBIS_PAD_BUTTON_CROSS = 0x4000, ORBIS_PAD_BUTTON_SQUARE = 0x8000,
    ORBIS_PAD_BUTTON_TOUCH_PAD = 0x100000,
};
#define log(...) std::printf(__VA_ARGS__), std::printf("\n")
#else
#include <orbis/Pad.h>
#include <orbis/SystemService.h>
#include <orbis/VideoOut.h>

#include <nx/runtime.h>
#endif

namespace nx::plat::ps4 {

#if !defined(SMW_MENU_PREVIEW)
// platform_ps4.cpp: player 1's DualShock 4 as it is (buttons, left stick y), and a storyboard request.
bool menu_pad(uint32_t& buttons, uint8_t& left_y);
void request_diag();
#endif

namespace {

constexpr uint32_t kW = 1920, kH = 1080;
constexpr const char* kConfig = "/data/SMW/config.txt";

// ---- settings -----------------------------------------------------------------------------------------------------

struct Res {
    const char* label;
    const char* value;  // NX_RENDER_RES, or null for handheld mode
};
// FSR 1's quality modes (render scale per axis: 1.3, 1.5, 1.7, 2.0) against the 1080p screen, and native.
constexpr Res kResolutions[] = {
    {"Native 1920x1080", "1920x1080"},       {"FSR Ultra Quality 1476x830", "1476x830"},
    {"FSR Quality 1280x720", "1280x720"},    {"FSR Balanced 1128x634", "1128x634"},
    {"FSR Performance 960x540", "960x540"},  {"Handheld mode 1280x720", nullptr},
};
constexpr int kResCount = int(sizeof kResolutions / sizeof kResolutions[0]);
constexpr int kResDefault = 2;  // FSR Quality (platform_ps4.cpp's default NX_RENDER_RES)
constexpr const char* kCores[] = {"balanced", "pinned", "spread"};
constexpr const char* kCounter[] = {"Off", "FPS", "FPS and frame time"};

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
    int res = kResDefault;  // kResolutions
    bool fsr = true;
    float sharp = 0.2f;     // FSR stops: 0 sharpest, 2 none
    int cores = 0;          // kCores
    int counter = 0;        // kCounter
    int map[kNxCount];      // kDs4 index per Switch button, -1 none
};
Settings g_set;
bool g_cross = true;
std::atomic<bool> g_fsr{true};
std::atomic<float> g_sharp{0.2f};
std::atomic<int> g_counter{0};
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
    g_counter = g_set.counter;
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
                                         "NX_PS4_MAP", "NX_PS4_FPS"};
    std::vector<std::string> lines;
    if (FILE* f = std::fopen(kConfig, "r")) {
        char buf[512];
        while (std::fgets(buf, sizeof buf, f)) {
            std::string line = buf;
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            bool owned = false;
            for (const char* k : kOwned) owned |= line.compare(0, std::strlen(k) + 1, std::string(k) + "=") == 0;
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
    lines.push_back("NX_PS4_FPS=" + std::to_string(g_set.counter));
    lines.push_back("NX_PS4_MAP=" + map_string(g_set.map));
    std::string tmp = std::string(kConfig) + ".tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "w")) {
        for (const std::string& l : lines) std::fprintf(f, "%s\n", l.c_str());
        if (std::fclose(f) == 0) std::rename(tmp.c_str(), kConfig);
    }
    log("menu: settings saved (%s, FSR %s %.1f, cores %s, counter %d)", r.label, g_set.fsr ? "on" : "off",
        double(g_set.sharp), kCores[g_set.cores], g_set.counter);
}

// ---- drawing --------------------------------------------------------------------------------------------------------

struct Font {
    uint16_t cell_w = 0, cell_h = 0;
    uint8_t first = 0, count = 0;
    std::vector<uint8_t> advance, cells;  // RGBA cells
};
Font g_text, g_title;

bool load_font(Font& font, const char* path) {
    if (!font.cells.empty()) return true;
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    if (d.size() < 10 || std::memcmp(d.data(), "SMWF", 4) != 0) return false;
    font.cell_w = uint16_t(d[4] | d[5] << 8), font.cell_h = uint16_t(d[6] | d[7] << 8);
    font.first = d[8], font.count = d[9];
    size_t cells = size_t(font.count) * font.cell_w * font.cell_h * 4;
    if (d.size() != 10 + font.count + cells) return false;
    font.advance.assign(d.begin() + 10, d.begin() + 10 + font.count);
    font.cells.assign(d.begin() + 10 + font.count, d.end());
    return true;
}

struct Rgb {
    uint8_t r, g, b;
};
constexpr Rgb kPanel{16, 22, 64}, kGold{255, 194, 31}, kWhite{245, 247, 255}, kNavy{16, 20, 60}, kGrey{170, 180, 215},
    kAmber{255, 214, 120}, kShadow{0, 0, 0};

// A canvas: the panel's host buffer or the frame counter's label.
struct Canvas {
    uint8_t* px;
    int w, h;
    void put(int x, int y, Rgb c, int a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        uint8_t* p = px + (size_t(y) * w + x) * 4;
        p[0] = uint8_t((c.r * a + p[0] * (255 - a)) / 255);
        p[1] = uint8_t((c.g * a + p[1] * (255 - a)) / 255);
        p[2] = uint8_t((c.b * a + p[2] * (255 - a)) / 255);
        p[3] = 255;
    }
    void rect(int x0, int y0, int x1, int y1, Rgb c, int a) {
        for (int y = std::max(0, y0); y < std::min(h, y1); y++)
            for (int x = std::max(0, x0); x < std::min(w, x1); x++) put(x, y, c, a);
    }
};

int text_width(const Font& f, const std::string& s) {
    int w = 0;
    for (unsigned char ch : s)
        if (ch >= f.first && ch < f.first + f.count) w += f.advance[ch - f.first];
    return w;
}

// `tint`: the colour of the plain menu font's glyphs; the title font keeps its own colours (no tint).
void glyphs(Canvas& cv, const Font& f, const std::string& s, int x, int y, const Rgb* tint, int alpha = 255) {
    for (unsigned char ch : s) {
        if (ch < f.first || ch >= f.first + f.count) continue;
        int k = ch - f.first;
        const uint8_t* cell = &f.cells[size_t(k) * f.cell_w * f.cell_h * 4];
        for (int cy = 0; cy < f.cell_h; cy++)
            for (int cx = 0; cx < f.cell_w; cx++) {
                const uint8_t* p = cell + (size_t(cy) * f.cell_w + cx) * 4;
                if (p[3]) cv.put(x + cx, y + cy, tint ? *tint : Rgb{p[0], p[1], p[2]}, p[3] * alpha / 255);
            }
        x += f.advance[k];
    }
}

// Menu text: a soft shadow under it keeps it readable over anything.
void text(Canvas& cv, const std::string& s, int x, int y, Rgb c, bool shadow = true) {
    if (shadow) glyphs(cv, g_text, s, x + 2, y + 2, &kShadow, 150);
    glyphs(cv, g_text, s, x, y, &c);
}

// The panel, composed in a host buffer (x kPx..kPx+kPw, y kPy..kPy+kPh of the screen).
constexpr int kPw = 1200, kPh = 920, kPx = (int(kW) - kPw) / 2, kPy = (int(kH) - kPh) / 2;
constexpr int kRowH = 52, kTop = 136, kVisible = 12;
std::vector<uint8_t> g_panel(size_t(kPw) * kPh * 4);
std::vector<uint8_t> g_dim;  // the frame, dimmed, kW x kH RGBA

struct Row {
    std::string label, value;
    bool restart = false;  // applies at the next start
};

void draw_panel(const std::string& title, const std::vector<Row>& rows, int selected, const std::string& footer,
                const std::string& note) {
    Canvas cv{g_panel.data(), kPw, kPh};
    for (int y = 0; y < kPh; y++)  // the dimmed frame under the panel
        std::memcpy(&g_panel[size_t(y) * kPw * 4], &g_dim[(size_t(kPy + y) * kW + kPx) * 4], size_t(kPw) * 4);
    cv.rect(0, 0, kPw, kPh, kPanel, 232);
    cv.rect(0, 0, kPw, 6, kGold, 255);
    glyphs(cv, g_title, title, 44, 30, nullptr);
    // a window of kVisible rows that keeps the selection in view
    int n = int(rows.size()), first = 0;
    if (n > kVisible) first = std::clamp(selected - kVisible / 2, 0, n - kVisible);
    for (int k = first; k < std::min(n, first + kVisible); k++) {
        int y = kTop + (k - first) * kRowH;
        bool sel = k == selected;
        if (sel) cv.rect(28, y - 4, kPw - 28, y + kRowH - 6, kGold, 245);
        Rgb c = sel ? kNavy : kWhite;
        text(cv, rows[k].label + (rows[k].restart ? " *" : ""), 56, y, c, !sel);
        if (!rows[k].value.empty()) {
            std::string v = sel && rows[k].value != ">" ? "<  " + rows[k].value + "  >" : rows[k].value;
            text(cv, v, kPw - 56 - text_width(g_text, v), y, c, !sel);
        }
    }
    if (first > 0) text(cv, "...", kPw / 2 - 20, kTop - 40, kGrey);
    if (first + kVisible < n) text(cv, "...", kPw / 2 - 20, kTop + kVisible * kRowH - 14, kGrey);
    cv.rect(28, kPh - 128, kPw - 28, kPh - 126, kGrey, 120);
    text(cv, footer, 56, kPh - 112, kGrey);
    text(cv, note, 56, kPh - 64, kAmber);
}

// ---- the menu's pages -------------------------------------------------------------------------------------------------

enum class Page { Main, Controls, Bind };

struct Input {
    uint32_t held = 0, pressed = 0;
    int repeat_dir = 0, repeat_ticks = 0;
    bool up = false, down = false;  // with the stick and auto-repeat
};

void step(Input& in, uint32_t b, uint8_t ly) {
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

struct MenuState {
    Page page = Page::Main;
    int sel = 0, ctl_sel = 0, bind_target = 0;
    bool released = false;  // Bind: every button let go since the page opened
    bool done = false, restart = false, diag = false;
    std::string note = "* applies when the game starts again";
};

// One frame of the menu: input in, rows out.
void menu_frame(MenuState& m, const Input& in, std::string& title, std::vector<Row>& rows, std::string& footer, int& sel) {
    rows.clear();
    if (m.page == Page::Main) {
        char sharp[32];
        std::snprintf(sharp, sizeof sharp, "%.1f", double(g_set.sharp));
        title = "SMW quick menu";
        rows = {{"Resume", ""},
                {"Render resolution", kResolutions[g_set.res].label, true},
                {"Upscaling", g_set.fsr ? "FSR 1" : "Bilinear"},
                {"FSR sharpness (0 = sharpest)", sharp},
                {"Frame counter", kCounter[g_set.counter]},
                {"CPU cores", kCores[g_set.cores], true},
                {"Controls", ">"},
                {"Capture a diagnostics frame", ""},
                {"Restart the game now", ""}};
        footer = "Up/Down choose  Left/Right change  Cross OK  Circle close";
        int n = int(rows.size());
        if (in.up) m.sel = (m.sel + n - 1) % n;
        if (in.down) m.sel = (m.sel + 1) % n;
        int dx = (in.pressed & ORBIS_PAD_BUTTON_RIGHT) ? 1 : (in.pressed & ORBIS_PAD_BUTTON_LEFT) ? -1 : 0;
        bool ok = in.pressed & ORBIS_PAD_BUTTON_CROSS;
        if (ok && dx == 0) dx = 1;
        switch (m.sel) {
            case 0: if (ok) m.done = true; break;
            case 1: if (dx) g_set.res = (g_set.res + kResCount + dx) % kResCount; break;
            case 2: if (dx) g_set.fsr = !g_set.fsr; break;
            case 3:
                if (dx) g_set.sharp = std::clamp(std::round((g_set.sharp + 0.1f * float(dx)) * 10.0f) / 10.0f, 0.0f, 2.0f);
                break;
            case 4: if (dx) g_set.counter = (g_set.counter + 3 + dx) % 3; break;
            case 5: if (dx) g_set.cores = (g_set.cores + 3 + dx) % 3; break;
            case 6: if (ok || dx > 0) m.page = Page::Controls, m.ctl_sel = 0; break;
            case 7: if (ok) m.diag = m.done = true; break;
            case 8: if (ok) m.restart = m.done = true; break;
        }
        if (in.pressed & ORBIS_PAD_BUTTON_CIRCLE) m.done = true;
        const uint32_t both = ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3;
        if ((in.held & both) == both && (in.pressed & both)) m.done = true;
        sel = m.sel;
    } else if (m.page == Page::Controls) {
        title = "Controls";
        for (int b = 0; b < kNxCount; b++)
            rows.push_back({std::string("Switch ") + kNxNames[b], g_set.map[b] >= 0 ? kDs4[g_set.map[b]].name : "none"});
        rows.push_back({"Reset to defaults", ""});
        rows.push_back({"Back", ""});
        footer = "Cross: then press the new button  Circle back";
        int n = int(rows.size());
        if (in.up) m.ctl_sel = (m.ctl_sel + n - 1) % n;
        if (in.down) m.ctl_sel = (m.ctl_sel + 1) % n;
        if (in.pressed & ORBIS_PAD_BUTTON_CROSS) {
            if (m.ctl_sel < kNxCount) m.page = Page::Bind, m.bind_target = m.ctl_sel, m.released = false;
            else if (m.ctl_sel == kNxCount) default_map(g_set.map);
            else m.page = Page::Main;
        }
        if (in.pressed & ORBIS_PAD_BUTTON_CIRCLE) m.page = Page::Main;
        sel = m.ctl_sel;
    } else {  // Bind: the next button pressed (after everything was let go)
        title = "Controls";
        rows = {{std::string("Press the button for Switch ") + kNxNames[m.bind_target], ""},
                {"The D-pad cancels", ""}};
        footer = "";
        if (in.held == 0) m.released = true;
        if (m.released && in.pressed) {
            uint32_t p = in.pressed;
            if (!(p & (ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN | ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT)))
                for (int d = 0; d < kDs4Count; d++)
                    if (p & kDs4[d].bit) {
                        // the Switch button that had the new one gets this one's old button (a swap)
                        for (int b = 0; b < kNxCount; b++)
                            if (b != m.bind_target && g_set.map[b] == d) g_set.map[b] = g_set.map[m.bind_target];
                        g_set.map[m.bind_target] = d;
                        break;
                    }
            m.page = Page::Controls;
        }
        sel = -1;
    }
}

void dim_frame(const uint8_t* frame, uint32_t pitch) {
    g_dim.resize(size_t(kW) * kH * 4);
    std::vector<uint8_t> row(size_t(kW) * 4);
    for (uint32_t y = 0; y < kH; y++) {
        std::memcpy(row.data(), frame + size_t(y) * pitch * 4, row.size());
        uint8_t* d = &g_dim[size_t(y) * kW * 4];
        for (uint32_t x = 0; x < kW * 4; x += 4)
            d[x] = uint8_t(row[x] * 2 / 5), d[x + 1] = uint8_t(row[x + 1] * 2 / 5), d[x + 2] = uint8_t(row[x + 2] * 2 / 5),
            d[x + 3] = 255;
    }
}

bool load_fonts() {
    return load_font(g_text, "/app0/boot/menu_font.bin") && load_font(g_title, "/app0/boot/font.bin");
}

}  // namespace

void settings_load(bool cross_confirms) {
    g_cross = cross_confirms;
    default_map(g_set.map);
    const char* h = std::getenv("NX_HANDHELD");
    const char* r = std::getenv("NX_RENDER_RES");
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
    if (const char* c = std::getenv("NX_PS4_FPS")) g_set.counter = std::clamp(std::atoi(c), 0, 2);
    if (const char* m = std::getenv("NX_PS4_MAP")) parse_map(m, g_set.map);
    publish();
}

uint32_t settings_ds4_for(int b) { return b >= 0 && b < kNxCount ? g_map_bits[b].load(std::memory_order_relaxed) : 0; }
bool settings_fsr() { return g_fsr.load(std::memory_order_relaxed); }
float settings_fsr_sharpness() { return g_sharp.load(std::memory_order_relaxed); }
int settings_frame_counter() { return g_counter.load(std::memory_order_relaxed); }

void counter_label(uint8_t* rgba, uint32_t w, uint32_t h, double fps, double ms) {
    Canvas cv{rgba, int(w), int(h)};
    cv.rect(0, 0, int(w), int(h), kPanel, 255);
    cv.rect(0, 0, 4, int(h), kGold, 255);
    if (!load_fonts()) return;
    char line[48];
    if (g_counter == 2)
        std::snprintf(line, sizeof line, "%.0f FPS  %.1f ms", fps, ms);
    else
        std::snprintf(line, sizeof line, "%.0f FPS", fps);
    text(cv, line, 16, (int(h) - int(g_text.cell_h)) / 2, kWhite, false);
}

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

#if !defined(SMW_MENU_PREVIEW)
void menu_run(const uint8_t* frame, uint32_t pitch, int video, int game_buffer, int64_t flip_arg) {
    g_request = false;
    uint8_t* bufs[2] = {boot_buffer(0), boot_buffer(1)};
    if (!bufs[0] || !bufs[1] || !load_fonts()) {
        log("menu: no boot screen buffers or fonts: no menu");
        return;
    }
    g_open = true;
    log("menu: open");
    dim_frame(frame, pitch);  // read once: the swapchain image is write-combined too
    bool full[2] = {false, false};
    MenuState m;
    Input in;
    uint32_t b = 0;
    uint8_t ly = 128;
    menu_pad(b, ly);
    step(in, b, ly);  // the L3 + R3 that opened the menu: held, not pressed
    int k = 0;
    while (!m.done) {
        b = 0, ly = 128;
        menu_pad(b, ly);
        step(in, b, ly);
        std::string title, footer;
        std::vector<Row> rows;
        int sel = 0;
        menu_frame(m, in, title, rows, footer, sel);
        publish();  // FSR, the frame counter and the mapping take effect at once
        draw_panel(title, rows, sel, footer, m.note);
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
    }
    publish();
    save();
    if (m.diag) request_diag();
    if (m.restart) {
        log("menu: restarting the game");
        const char* args[] = {nullptr};
        sceSystemServiceLoadExec("/app0/eboot.bin", args);
    }
    sceVideoOutSubmitFlip(video, game_buffer, ORBIS_VIDEO_OUT_FLIP_VSYNC, flip_arg);
    g_hold_after = true;
    g_open = false;
    log("menu: closed");
}
#else
// The PC preview: page `page` with `sel` selected over `frame` (or grey), written to `out` as a PPM.
void menu_preview(const char* fonts, const uint8_t* frame, int page, int sel, const char* out) {
    std::string dir = fonts;
    load_font(g_text, (dir + "/menu_font.bin").c_str());
    load_font(g_title, (dir + "/font.bin").c_str());
    default_map(g_set.map);
    std::vector<uint8_t> grey(size_t(kW) * kH * 4, 90);
    dim_frame(frame ? frame : grey.data(), kW);
    MenuState m;
    m.page = page == 0 ? Page::Main : page == 1 ? Page::Controls : Page::Bind;
    m.sel = m.ctl_sel = sel;
    Input in;
    std::string title, footer;
    std::vector<Row> rows;
    int s = 0;
    menu_frame(m, in, title, rows, footer, s);
    draw_panel(title, rows, s, footer, m.note);
    std::vector<uint8_t> img = g_dim;
    for (int y = 0; y < kPh; y++)
        std::memcpy(&img[(size_t(kPy + y) * kW + kPx) * 4], &g_panel[size_t(y) * kPw * 4], size_t(kPw) * 4);
    std::vector<uint8_t> label(340 * 56 * 4);
    g_counter = 2;
    counter_label(label.data(), 340, 56, 29.97, 33.4);
    for (int y = 0; y < 56; y++) std::memcpy(&img[(size_t(24 + y) * kW + kW - 364) * 4], &label[size_t(y) * 340 * 4], 340 * 4);
    FILE* f = std::fopen(out, "wb");
    std::fprintf(f, "P6\n%u %u\n255\n", kW, kH);
    for (size_t i = 0; i < img.size(); i += 4) std::fwrite(&img[i], 1, 3, f);
    std::fclose(f);
}
#endif

}  // namespace nx::plat::ps4
