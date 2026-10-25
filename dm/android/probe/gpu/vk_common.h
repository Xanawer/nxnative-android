// Vulkan bring-up shared by the Phase 0 GPU probes (docs/android/PLAN.md P0.7, P0.8): driver loading (the system loader, or a custom
// driver through libadrenotools), instance and device creation, function pointers, and small helpers for buffers, compute pipelines
// and one-shot command buffers. No libvulkan link: every entry point comes from the loader handle chosen at run time.
#pragma once
#define VK_NO_PROTOTYPES 1
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "vulkan/vulkan_core.h"

#define VKC_GLOBAL_FUNCS(X) X(vkCreateInstance) X(vkEnumerateInstanceVersion) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties)
#define VKC_INSTANCE_FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceFeatures) X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceMemoryProperties2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceImageFormatProperties) \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define VKC_DEVICE_FUNCS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueWaitIdle) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkFlushMappedMemoryRanges) X(vkInvalidateMappedMemoryRanges) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineCache) X(vkDestroyPipelineCache) X(vkGetPipelineCacheData) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateComputePipelines) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) X(vkCmdFillBuffer) X(vkCmdUpdateBuffer) X(vkCmdCopyBuffer) \
    X(vkCmdPipelineBarrier) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdDraw) X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) \
    X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkWaitForFences)

typedef struct {
    const char *mode;          // "system" (dlopen libvulkan.so) or "adrenotools"
    const char *hook_dir;      // adrenotools: the directory holding libadrenotools.so and its hook libraries (the APK's nativeLibraryDir)
    const char *driver_dir;    // adrenotools: directory with the custom driver (a trailing '/' is added if missing)
    const char *driver_name;   // adrenotools: the driver's file name, e.g. vulkan.ad07xx.so
} vkc_load_opts_t;

typedef struct vkc {
    void *lib;
    char load_desc[1024];      // what was loaded and with which arguments
    char new_libs[4096];       // shared objects mapped by loading + instance + device creation (from /proc/self/maps), newline separated
    char maps_before[16384];
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#define VKC_X(n) PFN_##n n;
    VKC_GLOBAL_FUNCS(VKC_X) VKC_INSTANCE_FUNCS(VKC_X) VKC_DEVICE_FUNCS(VKC_X)
#undef VKC_X
    // optional device entry points (NULL when absent)
    PFN_vkMapMemory2KHR MapMemory2;          // vkMapMemory2KHR, else core vkMapMemory2
    PFN_vkUnmapMemory2KHR UnmapMemory2;
    PFN_vkGetBufferDeviceAddress GetBufferDeviceAddress;
    uint32_t inst_version, inst_api;
    VkInstance inst;
    VkPhysicalDevice pd;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceDriverProperties drv;
    VkPhysicalDeviceMemoryProperties mem;
    VkExtensionProperties *ext; uint32_t next;
    VkDevice dev;
    VkQueue queue; uint32_t qfam;
    VkCommandPool cpool; VkCommandBuffer cmd; VkFence fence;
    int lost;                  // set once vkc_cmd_submit_wait has seen VK_ERROR_DEVICE_LOST or VK_TIMEOUT
} vkc_t;

const char *vkc_result_str(VkResult r);
int vkc_self_dir(char *out, size_t n);                       // directory of the module holding this code (exe dir, or the APK's nativeLibraryDir)
int vkc_load(vkc_t *v, const vkc_load_opts_t *o, char *err, size_t errn);
int vkc_instance(vkc_t *v, uint32_t want_api, char *err, size_t errn);
int vkc_has_ext(const vkc_t *v, const char *name);
// Creates the device with the given extensions and pNext chain (e.g. a VkPhysicalDeviceFeatures2 heading the chain), one queue of a
// compute-capable family (graphics + compute preferred), a command pool, one command buffer and one fence.
int vkc_device(vkc_t *v, const char *const *exts, uint32_t next, const void *pnext, char *err, size_t errn);
void vkc_destroy(vkc_t *v);
void vkc_update_new_libs(vkc_t *v);                          // recompute new_libs against the snapshot taken in vkc_load
int vkc_memtype(const vkc_t *v, uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags avoid);
void vkc_memflags_str(VkMemoryPropertyFlags f, char *out, size_t n);
VkResult vkc_alloc(vkc_t *v, VkDeviceSize size, uint32_t type, int device_address, VkDeviceMemory *out);
VkResult vkc_buffer(vkc_t *v, VkDeviceMemory mem, VkDeviceSize off, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *out, uint32_t *type_bits);
// A compute pipeline from SPIR-V with nbind storage-buffer bindings (set 0) and push constants of push_size bytes
typedef struct { VkShaderModule sm; VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline pipe; VkDescriptorPool dp; VkDescriptorSet ds; } vkc_compute_t;
VkResult vkc_compute(vkc_t *v, const uint32_t *code, size_t bytes, uint32_t nbind, uint32_t push_size, vkc_compute_t *c);
void vkc_compute_destroy(vkc_t *v, vkc_compute_t *c);
void vkc_compute_bind(vkc_t *v, vkc_compute_t *c, uint32_t binding, VkBuffer buf, VkDeviceSize off, VkDeviceSize range);
VkResult vkc_cmd_begin(vkc_t *v);
VkResult vkc_cmd_submit_wait(vkc_t *v, uint64_t timeout_ns);
