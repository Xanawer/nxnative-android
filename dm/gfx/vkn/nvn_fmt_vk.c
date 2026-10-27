// NVNformat (nvn.h) → Vulkan (nvn_fmt_vk.h). Kept to the formats the Vulkan backend handles so far: sampled from guest memory, the RGBA8 / BGRA8 families with their sRGB and no-alpha
// variants (Stardew Valley registers RGBA8 only, dm-work stardew-surface.md); rendered to, slice 1's colour formats and the depth formats. Anything else is not in the table: the caller says
// it in red once and the texture has no host image. The bytes of every NVK_UP format are the VkFormat's, in order (NVN's RGBA8 is R, G, B, A in memory, as R8G8B8A8; BGRA8 as B8G8R8A8)
#include "nvn_fmt_vk.h"
#include <stddef.h>

#define C(n, fl, f, lin) {n, fl, {f, 0, 0}, lin}
// The float, 16-bit and RGB10A2 formats are uploaded as they are too: their bytes are the VkFormat's (components in R, G, B, A order). TTYD's lighting samples a CPU-uploaded
// 256x256 RG16F lookup table, which read as zeros when only rendering into these formats was allowed (Mario lit black on the title screen)
static const nvk_fmt T[] = {
    C(0x25, NVK_RT | NVK_UP, VK_FORMAT_R8G8B8A8_UNORM, 0),                                       // RGBA8
    C(0x38, NVK_RT | NVK_UP, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM),                 // RGBA8_SRGB
    C(0x6D, NVK_RT | NVK_UP | NVK_NOA, VK_FORMAT_R8G8B8A8_UNORM, 0),                             // RGBX8
    C(0x37, NVK_RT | NVK_UP | NVK_NOA, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM),       // RGBX8_SRGB
    C(0x9A, NVK_RT | NVK_UP, VK_FORMAT_B8G8R8A8_UNORM, 0),                                       // BGRA8
    C(0x9C, NVK_RT | NVK_UP, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM),                 // BGRA8_SRGB
    C(0x99, NVK_RT | NVK_UP | NVK_NOA, VK_FORMAT_B8G8R8A8_UNORM, 0),                             // BGRX8
    C(0x9B, NVK_RT | NVK_UP | NVK_NOA, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM),       // BGRX8_SRGB
    // Render targets only (slice 1's): sampled once the GPU has rendered them, never uploaded
    C(0x3D, NVK_RT | NVK_UP, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 0),   // RGB10A2
    C(0x29, NVK_RT | NVK_UP, VK_FORMAT_R16G16B16A16_SFLOAT, 0),        // RGBA16F
    C(0x2A, NVK_RT | NVK_UP, VK_FORMAT_R16G16B16A16_UNORM, 0),         // RGBA16
    C(0x3F, NVK_RT | NVK_UP, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 0),    // R11G11B10F (the same bits: R in 0-10, G in 11-21, B in 22-31; TTYD uploads its lighting cube in it)
    C(0x2E, NVK_RT | NVK_UP, VK_FORMAT_R32G32B32A32_SFLOAT, 0),        // RGBA32F
    C(0x01, NVK_RT | NVK_UP, VK_FORMAT_R8_UNORM, 0),                   // R8
    C(0x0D, NVK_RT | NVK_UP, VK_FORMAT_R8G8_UNORM, 0),                 // RG8
    C(0x05, NVK_RT | NVK_UP, VK_FORMAT_R16_SFLOAT, 0),                 // R16F
    C(0x11, NVK_RT | NVK_UP, VK_FORMAT_R16G16_SFLOAT, 0),              // RG16F
    C(0x0A, NVK_RT | NVK_UP, VK_FORMAT_R32_SFLOAT, 0),                 // R32F
    C(0x16, NVK_RT | NVK_UP, VK_FORMAT_R32G32_SFLOAT, 0),    // RG32F
    // Depth and stencil (rendered to; D24S8 falls back to D32_SFLOAT_S8_UINT where the device lacks it)
    {0x35, NVK_RT, {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, 0}, 0},                      // DEPTH24_STENCIL8
    // ASTC (NVN 0x79-0x86 UNORM, 0x87-0x94 sRGB, block sizes in Vulkan's order: VK_FORMAT_ASTC_4x4_UNORM_BLOCK 157, each sRGB right after), uploaded block for block where the device
    // has textureCompressionASTC_LDR (vk_dev.c; Mali does): Paper Mario: The Thousand-Year Door's in-game textures (ASTC 8x8 sRGB among them) had no host image and read zeros
    {0x79, NVK_UP, {(VkFormat)(157), 0, 0}, 0},   // RGBA_ASTC_4x4
    {0x7A, NVK_UP, {(VkFormat)(159), 0, 0}, 0},   // RGBA_ASTC_5x4
    {0x7B, NVK_UP, {(VkFormat)(161), 0, 0}, 0},   // RGBA_ASTC_5x5
    {0x7C, NVK_UP, {(VkFormat)(163), 0, 0}, 0},   // RGBA_ASTC_6x5
    {0x7D, NVK_UP, {(VkFormat)(165), 0, 0}, 0},   // RGBA_ASTC_6x6
    {0x7E, NVK_UP, {(VkFormat)(167), 0, 0}, 0},   // RGBA_ASTC_8x5
    {0x7F, NVK_UP, {(VkFormat)(169), 0, 0}, 0},   // RGBA_ASTC_8x6
    {0x80, NVK_UP, {(VkFormat)(171), 0, 0}, 0},   // RGBA_ASTC_8x8
    {0x81, NVK_UP, {(VkFormat)(173), 0, 0}, 0},   // RGBA_ASTC_10x5
    {0x82, NVK_UP, {(VkFormat)(175), 0, 0}, 0},   // RGBA_ASTC_10x6
    {0x83, NVK_UP, {(VkFormat)(177), 0, 0}, 0},   // RGBA_ASTC_10x8
    {0x84, NVK_UP, {(VkFormat)(179), 0, 0}, 0},   // RGBA_ASTC_10x10
    {0x85, NVK_UP, {(VkFormat)(181), 0, 0}, 0},   // RGBA_ASTC_12x10
    {0x86, NVK_UP, {(VkFormat)(183), 0, 0}, 0},   // RGBA_ASTC_12x12
    {0x87, NVK_UP, {(VkFormat)(158), 0, 0}, 0},   // RGBA_ASTC_4x4_SRGB
    {0x88, NVK_UP, {(VkFormat)(160), 0, 0}, 0},   // RGBA_ASTC_5x4_SRGB
    {0x89, NVK_UP, {(VkFormat)(162), 0, 0}, 0},   // RGBA_ASTC_5x5_SRGB
    {0x8A, NVK_UP, {(VkFormat)(164), 0, 0}, 0},   // RGBA_ASTC_6x5_SRGB
    {0x8B, NVK_UP, {(VkFormat)(166), 0, 0}, 0},   // RGBA_ASTC_6x6_SRGB
    {0x8C, NVK_UP, {(VkFormat)(168), 0, 0}, 0},   // RGBA_ASTC_8x5_SRGB
    {0x8D, NVK_UP, {(VkFormat)(170), 0, 0}, 0},   // RGBA_ASTC_8x6_SRGB
    {0x8E, NVK_UP, {(VkFormat)(172), 0, 0}, 0},   // RGBA_ASTC_8x8_SRGB
    {0x8F, NVK_UP, {(VkFormat)(174), 0, 0}, 0},   // RGBA_ASTC_10x5_SRGB
    {0x90, NVK_UP, {(VkFormat)(176), 0, 0}, 0},   // RGBA_ASTC_10x6_SRGB
    {0x91, NVK_UP, {(VkFormat)(178), 0, 0}, 0},   // RGBA_ASTC_10x8_SRGB
    {0x92, NVK_UP, {(VkFormat)(180), 0, 0}, 0},   // RGBA_ASTC_10x10_SRGB
    {0x93, NVK_UP, {(VkFormat)(182), 0, 0}, 0},   // RGBA_ASTC_12x10_SRGB
    {0x94, NVK_UP, {(VkFormat)(184), 0, 0}, 0},   // RGBA_ASTC_12x12_SRGB
    {0x36, NVK_RT, {VK_FORMAT_D32_SFLOAT_S8_UINT, 0, 0}, 0},                                                // DEPTH32F_STENCIL8
    {0x32, NVK_RT, {VK_FORMAT_D16_UNORM, 0, 0}, 0},                                                         // DEPTH16
    {0x34, NVK_RT, {VK_FORMAT_D32_SFLOAT, 0, 0}, 0},                                                        // DEPTH32F
    {0x33, NVK_RT, {VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT, 0}, 0},                            // DEPTH24
    {0x31, NVK_RT, {VK_FORMAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}, 0},      // STENCIL8
};
#undef C
const nvk_fmt *nvn_fmt_vk(int nf) {
    for (size_t i = 0; i < sizeof T / sizeof *T; i++) if (T[i].nvn == nf) return &T[i];
    return vkn_bc_nvk(nf);   // BC formats, or NULL
}
