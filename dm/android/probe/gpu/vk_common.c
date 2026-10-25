// Vulkan bring-up shared by the Phase 0 GPU probes; see vk_common.h.
#include "vk_common.h"
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define ERR(...) do { if (err) snprintf(err, errn, __VA_ARGS__); return -1; } while (0)

const char *vkc_result_str(VkResult r) {
    switch (r) {
#define C(x) case x: return #x;
    C(VK_SUCCESS) C(VK_NOT_READY) C(VK_TIMEOUT) C(VK_INCOMPLETE) C(VK_ERROR_OUT_OF_HOST_MEMORY) C(VK_ERROR_OUT_OF_DEVICE_MEMORY)
    C(VK_ERROR_INITIALIZATION_FAILED) C(VK_ERROR_DEVICE_LOST) C(VK_ERROR_MEMORY_MAP_FAILED) C(VK_ERROR_LAYER_NOT_PRESENT)
    C(VK_ERROR_EXTENSION_NOT_PRESENT) C(VK_ERROR_FEATURE_NOT_PRESENT) C(VK_ERROR_INCOMPATIBLE_DRIVER) C(VK_ERROR_TOO_MANY_OBJECTS)
    C(VK_ERROR_FORMAT_NOT_SUPPORTED) C(VK_ERROR_FRAGMENTED_POOL) C(VK_ERROR_UNKNOWN) C(VK_ERROR_OUT_OF_POOL_MEMORY)
    C(VK_ERROR_INVALID_EXTERNAL_HANDLE) C(VK_ERROR_FRAGMENTATION) C(VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS)
#undef C
    default: return "VK_?";
    }
}

int vkc_self_dir(char *out, size_t n) {
    Dl_info di;
    if (!dladdr((void *)&vkc_self_dir, &di) || !di.dli_fname) return -1;
    snprintf(out, n, "%s", di.dli_fname);
    char *s = strrchr(out, '/'); if (!s) return -1;
    *s = 0;
    return 0;
}

// Distinct file paths of mapped shared objects (".so" in the path, plus memfd mappings), one per line
static void maps_libs(char *out, size_t n) {
    out[0] = 0; size_t used = 0;
    FILE *f = fopen("/proc/self/maps", "r"); if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *p = strchr(line, '/'); if (!p) continue;
        p[strcspn(p, "\n")] = 0;
        if (!strstr(p, ".so") && !strstr(p, "memfd")) continue;
        // dedupe
        size_t pl = strlen(p); int dup = 0;
        for (char *q = out; q && *q; ) { char *e = strchr(q, '\n'); size_t ql = e ? (size_t)(e - q) : strlen(q); if (ql == pl && !memcmp(q, p, pl)) { dup = 1; break; } q = e ? e + 1 : NULL; }
        if (dup || used + pl + 2 >= n) continue;
        memcpy(out + used, p, pl); used += pl; out[used++] = '\n'; out[used] = 0;
    }
    fclose(f);
}

void vkc_update_new_libs(vkc_t *v) {
    static char now[32768];
    maps_libs(now, sizeof now);
    v->new_libs[0] = 0; size_t used = 0;
    for (char *q = now; *q; ) {
        char *e = strchr(q, '\n'); size_t ql = e ? (size_t)(e - q) : strlen(q);
        int found = 0;
        for (char *b = v->maps_before; *b; ) { char *be = strchr(b, '\n'); size_t bl = be ? (size_t)(be - b) : strlen(b); if (bl == ql && !memcmp(b, q, ql)) { found = 1; break; } if (!be) break; b = be + 1; }
        if (!found && used + ql + 2 < sizeof v->new_libs) { memcpy(v->new_libs + used, q, ql); used += ql; v->new_libs[used++] = '\n'; v->new_libs[used] = 0; }
        if (!e) break; q = e + 1;
    }
}

int vkc_load(vkc_t *v, const vkc_load_opts_t *o, char *err, size_t errn) {
    memset(v, 0, sizeof *v);
    maps_libs(v->maps_before, sizeof v->maps_before);
    if (!strcmp(o->mode, "system")) {
        v->lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!v->lib) ERR("dlopen libvulkan.so: %s", dlerror());
        snprintf(v->load_desc, sizeof v->load_desc, "dlopen(\"libvulkan.so\", RTLD_NOW|RTLD_LOCAL)");
    } else if (!strcmp(o->mode, "adrenotools")) {
        char lib[600], ddir[600];
        snprintf(lib, sizeof lib, "%s/libadrenotools.so", o->hook_dir);
        size_t dl = strlen(o->driver_dir);
        snprintf(ddir, sizeof ddir, "%s%s", o->driver_dir, dl && o->driver_dir[dl - 1] == '/' ? "" : "/");
        void *at = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
        if (!at) ERR("dlopen %s: %s", lib, dlerror());
        typedef void *(*open_fn)(int, int, const char *, const char *, const char *, const char *, const char *, void **);
        open_fn fn = (open_fn)dlsym(at, "adrenotools_open_libvulkan");
        if (!fn) ERR("dlsym adrenotools_open_libvulkan: %s", dlerror());
        snprintf(v->load_desc, sizeof v->load_desc, "adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, NULL, \"%s\", \"%s\", \"%s\", NULL, NULL)",
                 o->hook_dir, ddir, o->driver_name);
        v->lib = fn(RTLD_NOW, 1 /* ADRENOTOOLS_DRIVER_CUSTOM */, NULL, o->hook_dir, ddir, o->driver_name, NULL, NULL);
        if (!v->lib) ERR("adrenotools_open_libvulkan returned NULL (dlerror: %s)", dlerror() ? dlerror() : "-");
    } else ERR("unknown load mode %s", o->mode);
    v->vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(v->lib, "vkGetInstanceProcAddr");
    if (!v->vkGetInstanceProcAddr) ERR("dlsym vkGetInstanceProcAddr: %s", dlerror());
#define VKC_X(n) v->n = (PFN_##n)v->vkGetInstanceProcAddr(NULL, #n);
    VKC_GLOBAL_FUNCS(VKC_X)
#undef VKC_X
    if (!v->vkCreateInstance) ERR("no vkCreateInstance");
    return 0;
}

int vkc_has_ext(const vkc_t *v, const char *name) {
    for (uint32_t i = 0; i < v->next; i++) if (!strcmp(v->ext[i].extensionName, name)) return 1;
    return 0;
}

int vkc_instance(vkc_t *v, uint32_t want_api, char *err, size_t errn) {
    v->inst_version = VK_API_VERSION_1_0;
    if (v->vkEnumerateInstanceVersion) v->vkEnumerateInstanceVersion(&v->inst_version);
    v->inst_api = want_api < v->inst_version ? want_api : v->inst_version;
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "nxnative-probe", 1, "nxnative", 1, v->inst_api };
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai, 0, NULL, 0, NULL };
    VkResult r = v->vkCreateInstance(&ci, NULL, &v->inst);
    if (r) ERR("vkCreateInstance(api %u.%u): %s", VK_API_VERSION_MAJOR(v->inst_api), VK_API_VERSION_MINOR(v->inst_api), vkc_result_str(r));
#define VKC_X(n) v->n = (PFN_##n)v->vkGetInstanceProcAddr(v->inst, #n);
    VKC_INSTANCE_FUNCS(VKC_X)
#undef VKC_X
    uint32_t n = 0; v->vkEnumeratePhysicalDevices(v->inst, &n, NULL);
    if (!n) ERR("no physical devices");
    VkPhysicalDevice pds[8]; if (n > 8) n = 8;
    v->vkEnumeratePhysicalDevices(v->inst, &n, pds);
    v->pd = pds[0];
    v->vkGetPhysicalDeviceProperties(v->pd, &v->props);
    memset(&v->drv, 0, sizeof v->drv);
    v->drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    v->vkEnumerateDeviceExtensionProperties(v->pd, NULL, &v->next, NULL);
    v->ext = calloc(v->next ? v->next : 1, sizeof *v->ext);
    v->vkEnumerateDeviceExtensionProperties(v->pd, NULL, &v->next, v->ext);
    if (v->vkGetPhysicalDeviceProperties2 && (v->props.apiVersion >= VK_API_VERSION_1_2 || vkc_has_ext(v, "VK_KHR_driver_properties"))) {
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &v->drv, {0} };
        v->vkGetPhysicalDeviceProperties2(v->pd, &p2);
    }
    v->vkGetPhysicalDeviceMemoryProperties(v->pd, &v->mem);
    vkc_update_new_libs(v);
    return 0;
}

int vkc_device(vkc_t *v, const char *const *exts, uint32_t next, const void *pnext, char *err, size_t errn) {
    uint32_t nq = 0; v->vkGetPhysicalDeviceQueueFamilyProperties(v->pd, &nq, NULL);
    VkQueueFamilyProperties qf[16]; if (nq > 16) nq = 16;
    v->vkGetPhysicalDeviceQueueFamilyProperties(v->pd, &nq, qf);
    int fam = -1;
    for (uint32_t i = 0; i < nq && fam < 0; i++) if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) fam = (int)i;
    for (uint32_t i = 0; i < nq && fam < 0; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) fam = (int)i;
    if (fam < 0) ERR("no compute queue family");
    v->qfam = (uint32_t)fam;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, v->qfam, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, pnext, 0, 1, &qci, 0, NULL, next, exts, NULL };
    VkResult r = v->vkCreateDevice(v->pd, &dci, NULL, &v->dev);
    if (r) ERR("vkCreateDevice: %s", vkc_result_str(r));
#define VKC_X(n) v->n = (PFN_##n)v->vkGetDeviceProcAddr(v->dev, #n);
    VKC_DEVICE_FUNCS(VKC_X)
#undef VKC_X
    v->MapMemory2 = (PFN_vkMapMemory2KHR)v->vkGetDeviceProcAddr(v->dev, "vkMapMemory2KHR");
    if (!v->MapMemory2) v->MapMemory2 = (PFN_vkMapMemory2KHR)v->vkGetDeviceProcAddr(v->dev, "vkMapMemory2");
    v->UnmapMemory2 = (PFN_vkUnmapMemory2KHR)v->vkGetDeviceProcAddr(v->dev, "vkUnmapMemory2KHR");
    if (!v->UnmapMemory2) v->UnmapMemory2 = (PFN_vkUnmapMemory2KHR)v->vkGetDeviceProcAddr(v->dev, "vkUnmapMemory2");
    v->GetBufferDeviceAddress = (PFN_vkGetBufferDeviceAddress)v->vkGetDeviceProcAddr(v->dev, "vkGetBufferDeviceAddress");
    if (!v->GetBufferDeviceAddress) v->GetBufferDeviceAddress = (PFN_vkGetBufferDeviceAddress)v->vkGetDeviceProcAddr(v->dev, "vkGetBufferDeviceAddressKHR");
    v->vkGetDeviceQueue(v->dev, v->qfam, 0, &v->queue);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, v->qfam };
    if ((r = v->vkCreateCommandPool(v->dev, &pci, NULL, &v->cpool))) ERR("vkCreateCommandPool: %s", vkc_result_str(r));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, v->cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    if ((r = v->vkAllocateCommandBuffers(v->dev, &cai, &v->cmd))) ERR("vkAllocateCommandBuffers: %s", vkc_result_str(r));
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
    if ((r = v->vkCreateFence(v->dev, &fci, NULL, &v->fence))) ERR("vkCreateFence: %s", vkc_result_str(r));
    vkc_update_new_libs(v);
    return 0;
}

void vkc_destroy(vkc_t *v) {
    if (v->dev) {
        v->vkDeviceWaitIdle(v->dev);
        if (v->fence) v->vkDestroyFence(v->dev, v->fence, NULL);
        if (v->cpool) v->vkDestroyCommandPool(v->dev, v->cpool, NULL);
        v->vkDestroyDevice(v->dev, NULL);
    }
    if (v->inst) v->vkDestroyInstance(v->inst, NULL);
    free(v->ext);
    v->dev = NULL; v->inst = NULL; v->ext = NULL;
}

int vkc_memtype(const vkc_t *v, uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags avoid) {
    for (uint32_t i = 0; i < v->mem.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = v->mem.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (f & need) == need && !(f & avoid)) return (int)i;
    }
    return -1;
}

void vkc_memflags_str(VkMemoryPropertyFlags f, char *out, size_t n) {
    snprintf(out, n, "%s%s%s%s%s%s%s",
             f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "DEVICE_LOCAL|" : "", f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? "HOST_VISIBLE|" : "",
             f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? "HOST_COHERENT|" : "", f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "HOST_CACHED|" : "",
             f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT ? "LAZILY_ALLOCATED|" : "", f & VK_MEMORY_PROPERTY_PROTECTED_BIT ? "PROTECTED|" : "",
             f & ~(VkMemoryPropertyFlags)0x3f ? "OTHER|" : "");
    size_t l = strlen(out); if (l) out[l - 1] = 0; else snprintf(out, n, "0");
}

VkResult vkc_alloc(vkc_t *v, VkDeviceSize size, uint32_t type, int device_address, VkDeviceMemory *out) {
    VkMemoryAllocateFlagsInfo fi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0 };
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, device_address ? &fi : NULL, size, type };
    *out = VK_NULL_HANDLE;
    return v->vkAllocateMemory(v->dev, &ai, NULL, out);
}

VkResult vkc_buffer(vkc_t *v, VkDeviceMemory mem, VkDeviceSize off, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *out, uint32_t *type_bits) {
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkResult r = v->vkCreateBuffer(v->dev, &bci, NULL, out);
    if (r) return r;
    VkMemoryRequirements req; v->vkGetBufferMemoryRequirements(v->dev, *out, &req);
    if (type_bits) *type_bits = req.memoryTypeBits;
    if (mem == VK_NULL_HANDLE) return VK_SUCCESS;
    return v->vkBindBufferMemory(v->dev, *out, mem, off);
}

VkResult vkc_compute(vkc_t *v, const uint32_t *code, size_t bytes, uint32_t nbind, uint32_t push_size, vkc_compute_t *c) {
    memset(c, 0, sizeof *c);
    VkResult r;
    VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, bytes, code };
    if ((r = v->vkCreateShaderModule(v->dev, &smci, NULL, &c->sm))) return r;
    VkDescriptorSetLayoutBinding b[8];
    for (uint32_t i = 0; i < nbind && i < 8; i++) b[i] = (VkDescriptorSetLayoutBinding){ i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dlci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, nbind, b };
    if ((r = v->vkCreateDescriptorSetLayout(v->dev, &dlci, NULL, &c->dsl))) return r;
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size };
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &c->dsl, push_size ? 1 : 0, &pcr };
    if ((r = v->vkCreatePipelineLayout(v->dev, &plci, NULL, &c->pl))) return r;
    VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, c->sm, "main", NULL }, c->pl, VK_NULL_HANDLE, -1 };
    if ((r = v->vkCreateComputePipelines(v->dev, VK_NULL_HANDLE, 1, &cpci, NULL, &c->pipe))) return r;
    if (nbind) {
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nbind };
        VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps };
        if ((r = v->vkCreateDescriptorPool(v->dev, &dpci, NULL, &c->dp))) return r;
        VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, c->dp, 1, &c->dsl };
        if ((r = v->vkAllocateDescriptorSets(v->dev, &dsai, &c->ds))) return r;
    }
    return VK_SUCCESS;
}

void vkc_compute_bind(vkc_t *v, vkc_compute_t *c, uint32_t binding, VkBuffer buf, VkDeviceSize off, VkDeviceSize range) {
    VkDescriptorBufferInfo bi = { buf, off, range };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, c->ds, binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL };
    v->vkUpdateDescriptorSets(v->dev, 1, &w, 0, NULL);
}

void vkc_compute_destroy(vkc_t *v, vkc_compute_t *c) {
    if (c->dp) v->vkDestroyDescriptorPool(v->dev, c->dp, NULL);
    if (c->pipe) v->vkDestroyPipeline(v->dev, c->pipe, NULL);
    if (c->pl) v->vkDestroyPipelineLayout(v->dev, c->pl, NULL);
    if (c->dsl) v->vkDestroyDescriptorSetLayout(v->dev, c->dsl, NULL);
    if (c->sm) v->vkDestroyShaderModule(v->dev, c->sm, NULL);
    memset(c, 0, sizeof *c);
}

VkResult vkc_cmd_begin(vkc_t *v) {
    VkResult r = v->vkResetCommandBuffer(v->cmd, 0);
    if (r) return r;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    if ((r = v->vkBeginCommandBuffer(v->cmd, &bi))) return r;
    // GPU writes of earlier submissions -> any GPU access in this one (a fence wait alone orders them only for the host)
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    v->vkCmdPipelineBarrier(v->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    return VK_SUCCESS;
}

VkResult vkc_cmd_submit_wait(vkc_t *v, uint64_t timeout_ns) {
    VkResult r = v->vkEndCommandBuffer(v->cmd);
    if (r) return r;
    if ((r = v->vkResetFences(v->dev, 1, &v->fence))) return r;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &v->cmd, 0, NULL };
    if (!(r = v->vkQueueSubmit(v->queue, 1, &si, v->fence))) r = v->vkWaitForFences(v->dev, 1, &v->fence, VK_TRUE, timeout_ns);
    if (r == VK_ERROR_DEVICE_LOST || r == VK_TIMEOUT) v->lost = 1;   // a timed-out submission may still run: reusing its command buffer is undefined
    return r;
}
