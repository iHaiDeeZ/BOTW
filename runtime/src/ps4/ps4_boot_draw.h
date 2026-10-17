// PS4 boot screen pictures (ps4_boot_draw.cpp): the home screen art with a progress bar, the stage, the percentage
// and the credit line, drawn by the CPU into a linear 1920x1080 RGBA framebuffer. No PS4 calls: tools/ can build it
// on the PC to preview a frame.
#pragma once

#include <cstdint>
#include <string>

namespace nx::plat::ps4 {

constexpr uint32_t kBootWidth = 1920, kBootHeight = 1080;

struct BootView {
    std::string stage;  // "Loading game files: 412 MB"
    int percent = 0;    // 0..100
    std::string credit;  // under the bar
};

// Reads <dir>/background.rgba and <dir>/font.bin (scripts/make-art.py --boot). False if either is missing or bad.
bool boot_draw_load(const std::string& dir);

// The whole picture (`full`) or only the band that changes (the bar, the texts), into `fb` (rows of `pitch` pixels).
// Writes only, never reads `fb` (the PS4's framebuffers are write-combined memory).
void boot_draw(uint8_t* fb, uint32_t pitch, const BootView& view, bool full);

}  // namespace nx::plat::ps4
