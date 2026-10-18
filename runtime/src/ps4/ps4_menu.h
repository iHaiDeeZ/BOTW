// PS4 quick menu (ps4_menu.cpp): hold L3 + R3 in game for the settings an emulator would offer. The game pauses while
// it is open; the menu draws over a dimmed copy of the last frame on the boot screen's two VideoOut buffers.
//   Live: upscaling (FSR 1 or a plain stretch), FSR sharpness, a frame counter, the button mapping, a storyboard
//   capture.
//   At the next start (the menu can restart the game): render resolution (FSR 1 quality modes), handheld mode, CPU
//   core layout.
// Settings are kept in /data/SMW/config.txt as NX_<KEY>=<value> lines, read at startup like the rest of that file.
#pragma once

#include <cstdint>

namespace nx::plat::ps4 {

// The Switch buttons the menu maps to DualShock 4 buttons (the D-pad and sticks stay where they are).
enum NxButton { kNxA, kNxB, kNxX, kNxY, kNxL, kNxR, kNxZL, kNxZR, kNxPlus, kNxMinus, kNxLS, kNxRS, kNxCount };

// After config.txt became the environment (main): the settings from NX_RENDER_RES, NX_HANDHELD, NX_FSR,
// NX_FSR_SHARP, NX_PS4_CORES and NX_PS4_MAP. `cross_confirms`: the default mapping puts A on Cross.
void settings_load(bool cross_confirms);

// The DualShock 4 button bits (ORBIS_PAD_BUTTON_*) mapped to Switch button `b`: 0 for none.
uint32_t settings_ds4_for(int b);
bool settings_fsr();
float settings_fsr_sharpness();
int settings_frame_counter();  // 0 off, 1 FPS, 2 FPS and frame time (NX_PS4_FPS)
// The frame counter's label (ps4_vk_wsi.cpp copies it into each frame's top right corner): w x h RGBA, opaque.
void counter_label(uint8_t* rgba, uint32_t w, uint32_t h, double fps, double ms);

// The input thread (platform_ps4.cpp): L3 + R3 held asks for the menu; while it is open (and until every button is
// released after it) the game reads no buttons.
void menu_request();
bool menu_requested();
bool menu_blocks_input(uint32_t ds4_buttons);

// The presenting thread (ps4_vk_wsi.cpp), after the frame's flip and with the GPU idle: runs the menu until it is
// closed. `frame`: the picture on screen (linear RGBA, `pitch` pixels a row). The game's buffer `game_buffer` is
// flipped back on screen (flip argument `flip_arg`) before it returns.
void menu_run(const uint8_t* frame, uint32_t pitch, int video, int game_buffer, int64_t flip_arg);

// The boot screen's buffers (ps4_boot_screen.cpp), which the menu draws on: null when they were not made.
uint8_t* boot_buffer(int k);
int boot_buffer_index(int k);

}  // namespace nx::plat::ps4
