// PS4: Vulkan window system integration on VideoOut (ps4_vk_wsi.cpp), from Resonance's ps4_wsi.cpp by way of
// M33's PS4VulkanWSI.cpp.
#pragma once

#include <cstdint>

namespace nx::plat::ps4 {

// VideoOut's 1080p output (the system scales it for a 720p TV).
constexpr uint32_t kScreenWidth = 1920;
constexpr uint32_t kScreenHeight = 1080;

// vkGetInstanceProcAddr of the statically linked RADV with the PS4 surface and swapchain (VK_EXT_headless_surface +
// VK_KHR_swapchain on VideoOut) added. A PFN_vkGetInstanceProcAddr.
void* vk_get_instance_proc_addr();

// Flips submitted so far (the first one hides the system's splash screen).
uint64_t vk_flips();

}  // namespace nx::plat::ps4
