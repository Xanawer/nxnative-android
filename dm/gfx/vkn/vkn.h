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

#define VLOG(...) do { fprintf(stderr, "[dm] [vkn] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define VRED_ONCE(...) do { static _Atomic int said_; if (!atomic_exchange(&said_, 1)) VLOG("[RED] " __VA_ARGS__); } while (0)

// The platform surface structures are not in the vendored headers (vulkan_core.h only: dm/third_party/README.md); their layouts as the registry has them
typedef struct { VkStructureType sType; const void *pNext; VkFlags flags; void *window; } vkn_android_surface_ci;   // VkAndroidSurfaceCreateInfoKHR (ANativeWindow *window)
typedef struct { VkStructureType sType; const void *pNext; VkFlags flags; const void *pLayer; } vkn_metal_surface_ci;   // VkMetalSurfaceCreateInfoEXT (const CAMetalLayer *pLayer)
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
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline)
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
} vkn_dev_t;
extern vkn_dev_t vd;
int vkn_dev_init(void);                  // 1 = a device; logs the driver's identity
uint64_t vkn_validation_count(void);     // warnings and errors the messenger has had
int vkn_mem_type(uint32_t bits, VkMemoryPropertyFlags want);   // a memory type index in bits with want, -1 = none

// Host images of NVN textures (nvn_vk.c), by the texture's serial: render targets for now, plain device memory (PLAN.md 2.2: render-target pools have no shadow)
typedef struct vkn_img {
    uint64_t serial; VkImage img; VkDeviceMemory mem; VkFormat fmt, sfmt; VkImageAspectFlags aspect;   // sfmt: the format the present samples it as (UNORM for an sRGB target: bytes pass through)
    uint32_t w, h, layers, levels, nvn_fmt;
    VkImageLayout layout;                // as of the last submission handed to the queue (under g_qmu); the present restores it after sampling
    VkImageLayout rec;                   // as the submission being recorded leaves it (submit thread); published to layout at the commit
    uint64_t rec_sub;                    // the submission rec belongs to
    uint64_t use_nvn, use_pres;          // the NVN timeline / present timeline value of its last use: destroyed once both have completed
    struct { uint32_t key; VkImageView v; } views[16]; int nviews;   // attachment views by level << 16 | layer, and the present's (key ~0)
    float clear[4]; int cleared;         // the last colour clear landed on it (the present logs it: what the screen should show)
    int dead;                            // released by the game, destruction pending
    struct vkn_img *next;                // the table's bucket chain, then the pending-destruction list
} vkn_img;
vkn_img *vkn_img_get(uint64_t serial);  // under vkn_tmu
extern pthread_mutex_t vkn_tmu;          // the image table
extern pthread_mutex_t vkn_qmu;          // the queue (vkQueueSubmit2, vkQueuePresentKHR, vkQueueWaitIdle) and the published layouts
VkImageView vkn_img_view(vkn_img *e, uint32_t level, uint32_t layer, int present);   // under vkn_tmu
void vkn_sweep(void);                    // destroys released images whose last uses have completed
// The GPU lost to this run (VK_ERROR_DEVICE_LOST, or a submission the queue refused): said once in red; from then on nothing reaches the device (a lost device's queue would keep failing,
// and its command buffers stay pending for ever): NVN submissions complete in order without it (the null backend's behaviour) and presents are retired unshown. Any thread
extern _Atomic int vkn_lost;
void vkn_gpu_lost(const char *where, VkResult r);

// The NVN timeline (nvn_vk.c) and the present's own (vk_present.c): never mixed, an NVN fence value is only ever an NVN submission's
uint64_t vkn_pres_done(void);
int vkn_origin_ll(void);                 // NVN's window origin is lower left (mtl_set_origin_lower_left)
// The present thread (vk_present.c): a ticket per present, done once its frame is handed to the queue; log: say what this present shows
uint64_t vkn_present_enqueue(uint64_t serial, uint32_t w, uint32_t h, const int crop[4], int log);
void vkn_present_wait(uint64_t ticket);
void vkn_present_flush(void);            // every ticket so far done, and the GPU through their present work
int vkn_present_drain(uint64_t ns);      // the same within ns: 1 = done
