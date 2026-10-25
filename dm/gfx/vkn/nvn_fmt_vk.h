// NVNformat → Vulkan formats for the Vulkan backend (nvn_vk.c: host images of render targets and sampled textures), the twin of dm/gfx/tex/nvn_fmt_mtl.c (docs/android/PLAN.md Phase 2).
// It lives beside the backend, not in dm/gfx/tex: every host build takes that folder's sources by wildcard (the Mac and iOS Metal hosts, the Android null build), and this table is the Vulkan
// backend's alone (built only where dm/gfx/vkn is: make GFX=vkn, the Android vkn build, t_vkn)
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>
#include <stdint.h>

// flags: NVK_RT rendered to (an attachment); NVK_UP sampled from guest memory (a CPU deswizzle into a staging buffer, copied in as is: the bytes are the VkFormat's);
// NVK_NOA no alpha (RGBX, BGRX: a reads as 1)
enum { NVK_RT = 1, NVK_UP = 2, NVK_NOA = 4 };
// f: the VkFormats to try, in order (a device may lack the first: KosmicKrisp has no D24_UNORM_S8_UINT), 0-terminated; lin: the UNORM twin of an sRGB format (the present samples the bytes through it), 0 = f itself
typedef struct { uint16_t nvn; uint8_t flags; VkFormat f[3], lin; } nvk_fmt;
const nvk_fmt *nvn_fmt_vk(int nvn_format);   // NULL = a format this backend does not handle yet
