// Vulkan backend internals (docs/android/PLAN.md Phase 2), shared by nvn_vk.c (the mtl_* set: render targets, passes, submission, completion), vk_dev.c (loading, driver choice, instance,
// device, validation) and vk_present.c (surface, swapchain, present thread). Vulkan is reached through the function table below, filled from vkGetInstanceProcAddr / vkGetDeviceProcAddr
// of a library opened at run time (the system loader, or libadrenotools' for a driver package): nothing links libvulkan, so one host binary runs on any of them
#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include "../nvn/nvn_tex.h"   // nvn_texdesc

#define VLOG(...) do { fprintf(stderr, "[dm] [vkn] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define VRED_ONCE(...) do { static _Atomic int said_; if (!atomic_exchange(&said_, 1)) VLOG("[RED] " __VA_ARGS__); } while (0)

// The platform surface structures are not in the vendored headers (vulkan_core.h only: dm/third_party/README.md); their layouts as the registry has them
typedef struct { VkStructureType sType; const void *pNext; VkFlags flags; void *window; } vkn_android_surface_ci;   // VkAndroidSurfaceCreateInfoKHR (ANativeWindow *window)
typedef struct { VkStructureType sType; const void *pNext; VkFlags flags; const void *pLayer; } vkn_metal_surface_ci;   // VkMetalSurfaceCreateInfoEXT (const CAMetalLayer *pLayer)
// VkPhysicalDevicePortabilitySubsetFeaturesKHR (a beta extension: vulkan_beta.h, not vendored), as the registry lays it out; MoltenVK has it, KosmicKrisp, Adreno and Turnip do not
#define VKN_STYPE_PORTABILITY_SUBSET_FEATURES ((VkStructureType)1000163000)
typedef struct { VkStructureType sType; void *pNext; VkBool32 constantAlphaColorBlendFactors, events, imageViewFormatReinterpretation, imageViewFormatSwizzle, imageView2DOn3DImage,
                 multisampleArrayImage, mutableComparisonSamplers, pointPolygons, samplerMipLodBias, separateStencilMaskRef, shaderSampleRateInterpolationFunctions, tessellationIsolines,
                 tessellationPointMode, triangleFans, vertexAttributeAccessBeyondStride; } vkn_portability_features;
typedef VkResult (VKAPI_PTR *vkn_create_surface_fn)(VkInstance, const void *, const VkAllocationCallbacks *, VkSurfaceKHR *);

#define VKN_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceVersion) X(vkEnumerateInstanceLayerProperties) X(vkEnumerateInstanceExtensionProperties)
#define VKN_INSTANCE(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkCreateDebugUtilsMessengerEXT) X(vkDestroyDebugUtilsMessengerEXT)
#define VKN_DEVICE(X) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueWaitIdle) X(vkQueueSubmit2) X(vkQueuePresentKHR) X(vkCreateSwapchainKHR) \
    X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier2) \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdClearAttachments) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdDraw) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateSampler) X(vkDestroySampler) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkInvalidateMappedMemoryRanges) X(vkFlushMappedMemoryRanges) X(vkCmdCopyBuffer) X(vkCmdCopyImageToBuffer) X(vkGetBufferDeviceAddress) X(vkCmdCopyBufferToImage) \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage)
typedef struct {
#define VKN_PFN(n) PFN_##n n;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    VKN_GLOBAL(VKN_PFN) VKN_INSTANCE(VKN_PFN) VKN_DEVICE(VKN_PFN)
#undef VKN_PFN
    vkn_create_surface_fn create_surface;   // vkCreateAndroidSurfaceKHR or vkCreateMetalSurfaceEXT, whichever this platform's instance enabled
} vkn_fn;
extern vkn_fn vk;

// The device (vk_dev.c, made once by vkn_dev_init): NULL handles when it could not be made, and the backend runs as the null one does (said in red)
typedef struct {
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qfam;
    VkPhysicalDeviceProperties props; VkPhysicalDeviceDriverProperties drv; VkPhysicalDeviceMemoryProperties mem;
    int validation;                      // DM_VK_VALIDATION=1: VK_LAYER_KHRONOS_validation and the messenger are on
    const char *surface_ext;             // VK_KHR_android_surface / VK_EXT_metal_surface; NULL = none, nothing is shown
    int absent;                          // vkn_dev_init failed for want of Vulkan itself (no loader library, or a loader with no driver), not a broken one (t_vkn's only skip)
    int bda;                             // bufferDeviceAddress is on (Vulkan 1.3 requires it): shadow buffers have device addresses (nvn_vk.c mtl_gpu_addr)
    int aniso, mirror, minmax;           // samplerAnisotropy, samplerMirrorClampToEdge, samplerFilterMinmax are on (where the device has them: nvn_vk.c samp_make says a sampler that needs a missing one)
    int port, port_bias, port_swz;       // VK_KHR_portability_subset is on (required where the driver has it), with its samplerMipLodBias / imageViewFormatSwizzle features; every feature it
                                         // offers is enabled, and nvn_vk.c avoids a use whose feature is absent (a LOD bias of 0, an identity view swizzle: said once)
} vkn_dev_t;
extern vkn_dev_t vd;
int vkn_dev_init(void);                  // 1 = a device; logs the driver's identity
uint64_t vkn_validation_count(void);     // warnings and errors the messenger has had
int vkn_mem_type(uint32_t bits, VkMemoryPropertyFlags want);   // a memory type index in bits with want, -1 = none

// Host images of NVN textures (nvn_vk.c), by the texture's serial, in plain device memory (PLAN.md 2.2: render-target pools have no shadow): one image per serial, whether it was first made as a
// render target or as a sampled texture (PLAN.md 2.3, slice 2b), with every usage its format offers
typedef struct vkn_img {
    uint64_t serial; VkImage img; VkDeviceMemory mem; VkFormat fmt, sfmt; VkImageAspectFlags aspect;   // sfmt: the format the present samples it as (UNORM for an sRGB target: bytes pass through)
    uint32_t w, h, layers, levels, nvn_fmt;
    VkImageLayout layout;                // as of the last submission handed to the queue (under g_qmu); the present restores it after sampling
    VkImageLayout rec;                   // as the submission being recorded leaves it (submit thread); published to layout at the commit
    uint64_t rec_sub;                    // the submission rec belongs to
    uint64_t use_nvn, use_pres;          // the NVN timeline / present timeline value of its last use: destroyed once both have completed
    struct { uint32_t key; VkImageView v; } views[16]; int nviews;   // attachment views by level << 16 | layer, and the present's (key ~0)
    float clear[4]; int cleared;         // the last colour clear landed on it (the present logs it: what the screen should show)
    VkImageUsageFlags usage;             // what it was made with
    int gpu;                             // made for a render target (TextureInitialize in a CPU-no-access pool, or SET_RT), not for sampling
    // Sampled textures (nvn_vk.c, submit thread): the storage in guest memory and its description, the upload's state, and the views
    const uint8_t *gmem; uint64_t size; nvn_texdesc d;
    int up, alw;                         // up: its content was uploaded from guest memory (stamp is that upload's); alw: it lies in a page another pool shares, never protected: uploaded at every use
    uint64_t stamp;                      // nvn_dirty generation of the last upload (every use checks its pages against it: nvn_vk.c tex_ready)
    const void *span;                    // its nvn_dirty range (the pool's)
    uint8_t *edge[2]; uint32_t edge_n[2];   // its bytes in the pages it shares with other data at each end, as last uploaded: a write there that leaves them as they were uploads nothing (nvn_mtl.m pkg_edges_*)
    struct { uint64_t key; VkImageView v; } sviews[8]; int nsviews;   // sampled views by format, levels, layers, swizzle and aspect (nvn_vk.c img_sview)
    int dead;                            // released by the game, destruction pending
    struct vkn_img *next;                // the table's bucket chain, then the pending-destruction list
} vkn_img;
vkn_img *vkn_img_get(uint64_t serial);  // under vkn_tmu
extern pthread_mutex_t vkn_tmu;          // the image table
extern pthread_mutex_t vkn_qmu;          // the queue (vkQueueSubmit2, vkQueuePresentKHR, vkQueueWaitIdle) and the published layouts
VkImageView vkn_img_view(vkn_img *e, uint32_t level, uint32_t layer, int present);   // under vkn_tmu
void vkn_sweep(void);                    // destroys released images, and freed pools' shadow buffers, whose last uses have completed
// Strategy B (nvn_vk.c, PLAN.md 2.2 "As built, slice 2a"): a game address → its pool's shadow VkBuffer (as void *), the offset in it and its device address (0 without bufferDeviceAddress).
// 0 = no pool with a shadow holds [addr, addr+len): a pool gets one when a replayed record first reads it as a buffer
int mtl_gpu_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off, uint64_t *gpu);
// The GPU lost to this run (VK_ERROR_DEVICE_LOST, or a submission the queue refused): said once in red; from then on nothing reaches the device (a lost device's queue would keep failing,
// and its command buffers stay pending for ever): NVN submissions complete in order without it (the null backend's behaviour) and presents are retired unshown. Any thread
extern _Atomic int vkn_lost;
void vkn_gpu_lost(const char *where, VkResult r);

// The NVN timeline (nvn_vk.c) and the present's own (vk_present.c): never mixed, an NVN fence value is only ever an NVN submission's
uint64_t vkn_pres_done(void);
int vkn_origin_ll(void);                 // NVN's window origin is lower left (mtl_set_origin_lower_left)
// The present thread (vk_present.c): a ticket per present, done once its frame is handed to the queue; log: say what this present shows; shot: a PNG path (DM_NVN_SHOT picked this present,
// nvn_mtl.m's numbering: the present count, which is not the ticket's) or NULL: the window texture is read back and written there before the ticket is done
uint64_t vkn_present_enqueue(uint64_t serial, uint32_t w, uint32_t h, const int crop[4], int log, const char *shot);
void vkn_present_wait(uint64_t ticket);
void vkn_present_flush(void);            // every ticket so far done, and the GPU through their present work
int vkn_present_drain(uint64_t ns);      // the same within ns: 1 = done
