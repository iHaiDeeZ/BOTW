// PS4 boot screen (ps4_boot_screen.cpp): from the start of the app until the game's first frame, a picture of our own
// on VideoOut instead of the system's still splash: the art, a progress bar, what is being done and the credit.
#pragma once

#include <cstdint>

namespace nx::plat::ps4 {

// main(): shows the boot screen (from /app0/boot) on its own thread. Nothing happens if the assets are missing.
void boot_screen_start();
// The game's first present (ps4_vk_wsi.cpp), before its flip: the boot screen stops drawing (returns once it has)
// and keeps this boot's numbers for the next one's progress bar. Later calls do nothing.
void boot_screen_stop();
// Progress: bytes of game data read so far (platform_ps4.cpp read_at), the Vulkan device made (ps4_vk_wsi.cpp).
void boot_note_bytes(uint64_t bytes);
void boot_note_graphics();

// The app's one VideoOut handle (opened on first use, with its flip event queue): the boot screen and the swapchain
// (ps4_vk_wsi.cpp) share it. Negative when VideoOut could not be opened.
int video_out();
void* video_out_flip_queue();  // the OrbisKernelEqueue its flip events arrive on

}  // namespace nx::plat::ps4
