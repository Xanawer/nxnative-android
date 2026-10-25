// P0.8 shader probe (docs/android/PLAN.md, Phase 0; results in docs/android/probes-shader.md).
// Reads a manifest written by gen_manifest.py and the SPIR-V modules it names (pushed at run time; they derive from the user's game and never enter the repository), then:
//   t1  vkCreateShaderModule for every module, unmodified
//   t2  a graphics pipeline for every pair: dynamic rendering, one RGBA8 attachment per fragment output, vertex attributes from the vertex shader's inputs,
//       one descriptor set per stage (vertex: set 0, pixel: set 1, the pixel module's DescriptorSet decorations patched from 0 to 1), dynamic viewport and scissor.
//       Phases, each on a fresh VkDevice except cold, which runs on the device T1 used (T1 only creates and destroys shader modules): cold (no cache),
//       cold2 (again without a cache: shows an implicit driver cache), fill (an empty VkPipelineCache), warm (the same cache), reload (a new cache from fill's
//       serialized data); --cache-in <file>: only the phase disk (a cache from another process's data). Times cover vkCreateGraphicsPipelines only, not vkCreatePipelineCache.
//   t3  Y direction: a hand-written pair draws a triangle covering clip-space y < 0 with a positive and a negative viewport height, without culling and with back- or front-face culling
//   t4  NaN min / max: a compute shader evaluates FMin / FMax / FClamp and NMin / NMax / NClamp with NaN and signed-zero operands (parameters in a buffer)
// Writes <out_dir>/res/: summary.txt (every step, RESULT lines), results.tsv, t1.tsv, t2.tsv, t3.txt, t4.tsv, validation.txt (with --validation), cache.bin.
// Arguments: --label <name> --load system|adrenotools [--drvdir <dir> --drvname <file>] [--expect-lib <s>] [--expect-id <n>] [--expect-info <s>]
//            --dir <dir with manifest.txt and <set>/<key>.spv> [--set stripped|orig] [--tests t1,t2,t3,t4] [--phases cold,cold2,fill,warm,reload] [--cache-in <file>]
//            [--layout split|merged] [--validation] [--controls] [--mesa-cache] [--env NAME=VALUE ...]
//   --layout merged: both stages in set 0 as translated (vertex shader's type wins on a collision); only meant for the validation controls
//   --controls: positive controls for the validation layer (a merged layout, an uncovered pixel shader input, a UINT format on a float input)
//   macOS (KosmicKrisp through the Homebrew loader, --load system): VKC_LIBVULKAN overrides /opt/homebrew/lib/libvulkan.1.dylib
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include "vk_common.h"
#include "ydir_vert_spv.h"
#include "ydir_frag_spv.h"
#include "nanmm_f_spv.h"
#include "nanmm_n_spv.h"
#include "nanmm_fp_spv.h"
#include "nanmm_np_spv.h"

// ---------------------------------------------------------------- output
static FILE *g_sum, *g_val;
static char g_resdir[700];
static int g_in_app;
static vkc_t V;
typedef struct { char item[48]; char verdict[8]; char detail[700]; } result_t;
static result_t g_results[200]; static int g_nres;
static char g_ctx[160] = "init";   // what runs now: validation messages are filed under it
static int g_val_n, g_val_ctx_n;

static void S(const char *fmt, ...) {
    char buf[4096]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (g_sum) { fputs(buf, g_sum); fputc('\n', g_sum); fflush(g_sum); }
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "nxprobe-shader", "%s", buf);
#endif
    if (!g_in_app) { puts(buf); fflush(stdout); }
}
static void R(const char *item, const char *verdict, const char *fmt, ...) {
    result_t *r = g_nres < 200 ? &g_results[g_nres++] : &g_results[199];
    snprintf(r->item, sizeof r->item, "%s", item); snprintf(r->verdict, sizeof r->verdict, "%s", verdict);
    va_list ap; va_start(ap, fmt); vsnprintf(r->detail, sizeof r->detail, fmt, ap); va_end(ap);
    if (V.lost) { size_t l = strlen(r->detail); snprintf(r->detail + l, sizeof r->detail - l, " [AFTER DEVICE LOST]"); }
    S("RESULT %-26s %-4s %s", r->item, r->verdict, r->detail);
}
static FILE *open_res(const char *name) { char p[800]; snprintf(p, sizeof p, "%s/%s", g_resdir, name); return fopen(p, "w"); }
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }
static void ctx(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vsnprintf(g_ctx, sizeof g_ctx, fmt, ap); va_end(ap); g_val_ctx_n = 0; }

static const char *arg(int argc, char **argv, const char *name, const char *def) {
    for (int i = 0; i + 1 < argc; i++) if (!strcmp(argv[i], name)) return argv[i + 1];
    return def;
}
static int flag(int argc, char **argv, const char *name) { for (int i = 0; i < argc; i++) if (!strcmp(argv[i], name)) return 1; return 0; }
static int in_list(const char *list, const char *t) { char l[300], k[64]; snprintf(l, sizeof l, ",%s,", list); snprintf(k, sizeof k, ",%s,", t); return strstr(l, k) != NULL; }

// ---------------------------------------------------------------- extra entry points (not in vk_common)
static PFN_vkCmdBeginRendering pBeginRendering;
static PFN_vkCmdEndRendering pEndRendering;
static PFN_vkCmdSetViewport pSetViewport;
static PFN_vkCmdSetScissor pSetScissor;
static PFN_vkCreateDebugUtilsMessengerEXT pCreateMessenger;
static PFN_vkDestroyDebugUtilsMessengerEXT pDestroyMessenger;
static VkDebugUtilsMessengerEXT g_messenger;

static const char *g_expect_vuid; static int g_expect_hits;   // controls: the VUID a control is meant to trigger, and how often it did
static VKAPI_ATTR VkBool32 VKAPI_CALL val_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                             const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type; (void)user;
    const char *s = sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "ERROR" : sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT ? "WARNING" :
                    sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT ? "INFO" : "VERBOSE";
    if (!(sev & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT))) return VK_FALSE;
    g_val_n++; g_val_ctx_n++;
    if (g_expect_vuid && d->pMessageIdName && strstr(d->pMessageIdName, g_expect_vuid)) g_expect_hits++;
    static char msg[8192]; snprintf(msg, sizeof msg, "%s", d->pMessage ? d->pMessage : "");
    for (char *q = msg; *q; q++) if (*q == '\n' || *q == '\r' || *q == '\t') *q = ' ';   // one message per line in validation.txt
    if (g_val) { fprintf(g_val, "%s\t%s\t%s\t%s\n", g_ctx, s, d->pMessageIdName ? d->pMessageIdName : "-", msg); fflush(g_val); }
    if (g_val_ctx_n <= 3) S("  [validation %s] %s: %.300s", s, d->pMessageIdName ? d->pMessageIdName : "-", msg);
    return VK_FALSE;
}

// ---------------------------------------------------------------- loading and instance (vk_common's, plus layers on macOS)
#ifdef __APPLE__
static int load_apple(char *err, size_t n) {
    memset(&V, 0, sizeof V);
    const char *lib = getenv("VKC_LIBVULKAN") ? getenv("VKC_LIBVULKAN") : "/opt/homebrew/lib/libvulkan.1.dylib";
    V.lib = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!V.lib) { snprintf(err, n, "dlopen %s: %s", lib, dlerror()); return -1; }
    snprintf(V.load_desc, sizeof V.load_desc, "dlopen(\"%s\")", lib);
    V.vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(V.lib, "vkGetInstanceProcAddr");
    if (!V.vkGetInstanceProcAddr) { snprintf(err, n, "no vkGetInstanceProcAddr"); return -1; }
#define VKC_X(f) V.f = (PFN_##f)V.vkGetInstanceProcAddr(NULL, #f);
    VKC_GLOBAL_FUNCS(VKC_X)
#undef VKC_X
    return 0;
}
#endif

// As vkc_instance, with the Khronos validation layer and VK_EXT_debug_utils when asked
static int make_instance(int validation, char *err, size_t errn) {
    V.inst_version = VK_API_VERSION_1_0;
    if (V.vkEnumerateInstanceVersion) V.vkEnumerateInstanceVersion(&V.inst_version);
    V.inst_api = VK_API_VERSION_1_3 < V.inst_version ? VK_API_VERSION_1_3 : V.inst_version;
    const char *layers[1], *exts[2]; uint32_t nl = 0, ne = 0;
    if (validation) {
        uint32_t n = 0; V.vkEnumerateInstanceLayerProperties(&n, NULL);
        VkLayerProperties *lp = calloc(n ? n : 1, sizeof *lp); V.vkEnumerateInstanceLayerProperties(&n, lp);
        int found = 0;
        for (uint32_t i = 0; i < n; i++) { S("instance layer: %s (spec %u.%u.%u, impl %u)", lp[i].layerName, VK_API_VERSION_MAJOR(lp[i].specVersion), VK_API_VERSION_MINOR(lp[i].specVersion), VK_API_VERSION_PATCH(lp[i].specVersion), lp[i].implementationVersion); if (!strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation")) found = 1; }
        free(lp);
        if (!found) { snprintf(err, errn, "VK_LAYER_KHRONOS_validation not among the instance layers (VK_LAYER_PATH?)"); return -1; }
        layers[nl++] = "VK_LAYER_KHRONOS_validation"; exts[ne++] = "VK_EXT_debug_utils";
    }
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "nxnative-shader-probe", 1, "nxnative", 1, V.inst_api };
    VkDebugUtilsMessengerCreateInfoEXT mci = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, NULL, 0,
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, val_cb, NULL };
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, validation ? &mci : NULL, 0, &ai, nl, layers, ne, exts };
    VkResult r = V.vkCreateInstance(&ci, NULL, &V.inst);
    if (r) { snprintf(err, errn, "vkCreateInstance: %s", vkc_result_str(r)); return -1; }
#define VKC_X(f) V.f = (PFN_##f)V.vkGetInstanceProcAddr(V.inst, #f);
    VKC_INSTANCE_FUNCS(VKC_X)
#undef VKC_X
    if (validation) {
        pCreateMessenger = (PFN_vkCreateDebugUtilsMessengerEXT)V.vkGetInstanceProcAddr(V.inst, "vkCreateDebugUtilsMessengerEXT");
        pDestroyMessenger = (PFN_vkDestroyDebugUtilsMessengerEXT)V.vkGetInstanceProcAddr(V.inst, "vkDestroyDebugUtilsMessengerEXT");
        if (!pCreateMessenger || pCreateMessenger(V.inst, &mci, NULL, &g_messenger)) { snprintf(err, errn, "vkCreateDebugUtilsMessengerEXT failed"); return -1; }
    }
    uint32_t n = 0; V.vkEnumeratePhysicalDevices(V.inst, &n, NULL);
    if (!n) { snprintf(err, errn, "no physical devices"); return -1; }
    VkPhysicalDevice pds[8]; if (n > 8) n = 8;
    V.vkEnumeratePhysicalDevices(V.inst, &n, pds);
    V.pd = pds[0];
    V.vkGetPhysicalDeviceProperties(V.pd, &V.props);
    V.drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    V.vkEnumerateDeviceExtensionProperties(V.pd, NULL, &V.next, NULL);
    V.ext = calloc(V.next ? V.next : 1, sizeof *V.ext);
    V.vkEnumerateDeviceExtensionProperties(V.pd, NULL, &V.next, V.ext);
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &V.drv, {0} };
    V.vkGetPhysicalDeviceProperties2(V.pd, &p2);
    V.vkGetPhysicalDeviceMemoryProperties(V.pd, &V.mem);
    for (char *q = V.drv.driverInfo; *q; q++) if (*q == '\n' || *q == '\r') *q = ';';
    vkc_update_new_libs(&V);
    return 0;
}

// ---------------------------------------------------------------- device (re-created per T2 phase: a fresh VkDevice drops any per-device driver cache)
static VkPhysicalDeviceFeatures g_have;
static VkPhysicalDeviceVulkan11Features g_have11;
static VkPhysicalDeviceVulkan13Features g_have13;
static int g_orig;   // the original modules: enable what the driver reports, nothing more
static char g_enabled[512];
static int make_device(char *err, size_t errn) {
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f13 };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11 };
    f13.dynamicRendering = g_have13.dynamicRendering;
    f11.shaderDrawParameters = g_have11.shaderDrawParameters;
    f2.features.shaderImageGatherExtended = g_have.shaderImageGatherExtended;
    f2.features.shaderClipDistance = g_have.shaderClipDistance;
    f2.features.shaderCullDistance = g_have.shaderCullDistance;
    f2.features.shaderFloat64 = g_have.shaderFloat64;   // 0 on every Adreno driver and on KosmicKrisp
    snprintf(g_enabled, sizeof g_enabled, "dynamicRendering %u shaderDrawParameters %u shaderImageGatherExtended %u shaderClipDistance %u shaderCullDistance %u shaderFloat64 %u",
             f13.dynamicRendering, f11.shaderDrawParameters, f2.features.shaderImageGatherExtended, f2.features.shaderClipDistance, f2.features.shaderCullDistance, f2.features.shaderFloat64);
    const char *exts[4]; uint32_t ne = 0;
    if (vkc_has_ext(&V, "VK_KHR_portability_subset")) exts[ne++] = "VK_KHR_portability_subset";
    if (g_orig && vkc_has_ext(&V, "VK_EXT_fragment_shader_interlock")) exts[ne++] = "VK_EXT_fragment_shader_interlock";
    if (vkc_device(&V, exts, ne, &f2, err, errn)) return -1;
    pBeginRendering = (PFN_vkCmdBeginRendering)V.vkGetDeviceProcAddr(V.dev, "vkCmdBeginRendering");
    pEndRendering = (PFN_vkCmdEndRendering)V.vkGetDeviceProcAddr(V.dev, "vkCmdEndRendering");
    pSetViewport = (PFN_vkCmdSetViewport)V.vkGetDeviceProcAddr(V.dev, "vkCmdSetViewport");
    pSetScissor = (PFN_vkCmdSetScissor)V.vkGetDeviceProcAddr(V.dev, "vkCmdSetScissor");
    return 0;
}
static void drop_device(void) {
    if (!V.dev) return;
    V.vkDeviceWaitIdle(V.dev);
    if (V.fence) V.vkDestroyFence(V.dev, V.fence, NULL);
    if (V.cpool) V.vkDestroyCommandPool(V.dev, V.cpool, NULL);
    V.vkDestroyDevice(V.dev, NULL);
    V.dev = NULL; V.fence = VK_NULL_HANDLE; V.cpool = VK_NULL_HANDLE; V.cmd = NULL;
}

// ---------------------------------------------------------------- manifest
typedef struct { int set, binding, type, count; char name[48]; } bind_t;
typedef struct { int loc, n; char ty; } io_t;
typedef struct {
    char key[24]; int frag; char entry[32];
    bind_t b[24]; int nb; io_t vin[32]; int nvin; io_t fout[8]; int nfout;
    uint32_t *code; size_t bytes;
    VkShaderModule sm; VkDescriptorSetLayout dsl;   // per device
} mod_t;
typedef struct { int id, vs, fs; char src[64], cover[96]; } pair_t;
static mod_t g_mod[160]; static int g_nmod;
static pair_t g_pair[400]; static int g_npair;

static int mod_find(const char *k) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].key, k)) return i; return -1; }
static uint32_t *read_file(const char *path, size_t *bytes) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *p = malloc(n > 0 ? (size_t)n : 4); if (!p) { fclose(f); return NULL; }
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f); *bytes = (size_t)n; return p;
}
static int load_manifest(const char *dir, const char *set) {
    char p[900]; snprintf(p, sizeof p, "%s/manifest.txt", dir);
    FILE *f = fopen(p, "r"); if (!f) { S("cannot open %s: %s", p, strerror(errno)); return -1; }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char w0[16], k[24], k2[32], a[64], c[96]; int i1, i2, i3, i4;
        if (line[0] == '#') continue;
        if (sscanf(line, "module %23s %15s %31s", k, w0, a) == 3) {
            if (g_nmod >= 160) continue;
            mod_t *m = &g_mod[g_nmod++]; memset(m, 0, sizeof *m);
            snprintf(m->key, sizeof m->key, "%s", k); m->frag = !strcmp(w0, "frag"); snprintf(m->entry, sizeof m->entry, "%s", a);
        } else if (sscanf(line, "bind %23s %d %d %d %d %63s", k, &i1, &i2, &i3, &i4, a) == 6) {
            int m = mod_find(k); if (m < 0 || g_mod[m].nb >= 24) continue;
            bind_t *b = &g_mod[m].b[g_mod[m].nb++]; b->set = i1; b->binding = i2; b->type = i3; b->count = i4; snprintf(b->name, sizeof b->name, "%s", a);
        } else if (sscanf(line, "vin %23s %d %1s %d", k, &i1, w0, &i2) == 4) {
            int m = mod_find(k); if (m < 0 || g_mod[m].nvin >= 32) continue;
            g_mod[m].vin[g_mod[m].nvin++] = (io_t){ i1, i2, w0[0] };
        } else if (sscanf(line, "fout %23s %d %1s %d", k, &i1, w0, &i2) == 4) {
            int m = mod_find(k); if (m < 0 || g_mod[m].nfout >= 8) continue;
            g_mod[m].fout[g_mod[m].nfout++] = (io_t){ i1, i2, w0[0] };
        } else if (sscanf(line, "pair %d %23s %31s %63s %95s", &i1, k, k2, a, c) == 5) {
            int v = mod_find(k), fs = mod_find(k2);
            if (v < 0 || fs < 0 || g_npair >= 400) { S("manifest: pair %d names unknown modules", i1); continue; }
            pair_t *q = &g_pair[g_npair++]; q->id = i1; q->vs = v; q->fs = fs; snprintf(q->src, sizeof q->src, "%s", a); snprintf(q->cover, sizeof q->cover, "%s", c);
        }
    }
    fclose(f);
    int bad = 0;
    for (int i = 0; i < g_nmod; i++) {
        snprintf(p, sizeof p, "%s/%s/%s.spv", dir, set, g_mod[i].key);
        g_mod[i].code = read_file(p, &g_mod[i].bytes);
        if (!g_mod[i].code || g_mod[i].bytes < 20 || g_mod[i].code[0] != 0x07230203) { S("cannot read %s", p); bad++; }
    }
    S("manifest %s/manifest.txt: %d modules, %d pairs; modules from %s/%s/ (%d unreadable)", dir, g_nmod, g_npair, dir, set, bad);
    return bad ? -1 : 0;
}

// The pixel shader's descriptor set 0 → set 1 (the per-stage layout); returns a patched copy
static uint32_t *patch_set(const uint32_t *code, size_t bytes, uint32_t to, int *n_patched) {
    uint32_t *w = malloc(bytes); memcpy(w, code, bytes);
    size_t nw = bytes / 4; *n_patched = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (!n) break;
        if (op == 71 && n >= 4 && w[i + 2] == 34) { w[i + 3] = to; (*n_patched)++; }   // OpDecorate <id> DescriptorSet <set>
        i += n;
    }
    return w;
}

// ---------------------------------------------------------------- T1
static void t1(const char *setname) {
    ctx("t1/%s", setname);
    FILE *o = open_res("t1.tsv");
    if (o) fprintf(o, "set\tkey\tstage\tbytes\tresult\tms\n");
    int ok = 0, bad = 0; double tot = 0, mx = 0;
    for (int i = 0; i < g_nmod; i++) {
        mod_t *m = &g_mod[i];
        ctx("t1/%s/%s", setname, m->key);
        VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, m->bytes, m->code };
        VkShaderModule sm = VK_NULL_HANDLE;
        double t0 = now_ms(); VkResult r = V.vkCreateShaderModule(V.dev, &ci, NULL, &sm); double dt = now_ms() - t0;
        if (r) { bad++; S("  t1 %s: %s", m->key, vkc_result_str(r)); } else ok++;
        tot += dt; if (dt > mx) mx = dt;
        if (o) { fprintf(o, "%s\t%s\t%s\t%zu\t%s\t%.3f\n", setname, m->key, m->frag ? "frag" : "vert", m->bytes, vkc_result_str(r), dt); fflush(o); }
        if (sm) V.vkDestroyShaderModule(V.dev, sm, NULL);
    }
    if (o) fclose(o);
    char it[48]; snprintf(it, sizeof it, "t1.%s", setname);
    R(it, bad ? "FAIL" : "PASS", "vkCreateShaderModule: %d ok, %d failed of %d; total %.2f ms, max %.3f ms", ok, bad, g_nmod, tot, mx);
}

// ---------------------------------------------------------------- T2
static VkFormat attr_format(char ty, int n) {
    static const VkFormat F[3][4] = { { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT },
                                      { VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT },
                                      { VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT } };
    int t = ty == 'i' ? 1 : ty == 'u' ? 2 : 0; if (n < 1) n = 1; if (n > 4) n = 4;
    return F[t][n - 1];
}
static VkFormat color_format(char ty) { return ty == 'u' ? VK_FORMAT_R8G8B8A8_UINT : ty == 'i' ? VK_FORMAT_R8G8B8A8_SINT : VK_FORMAT_R8G8B8A8_UNORM; }

static int g_layout_merged;
typedef struct { int ok; VkResult r; double ms; uint32_t fb_flags, fb_vs, fb_fs; double fb_ms; } pres_t;

// Modules and per-stage set layouts for the current device. Fragment modules get their set decorations patched to 1 (split layout).
static int stage_objects(void) {
    for (int i = 0; i < g_nmod; i++) {
        mod_t *m = &g_mod[i];
        int np = 0; uint32_t *code = (m->frag && !g_layout_merged) ? patch_set(m->code, m->bytes, 1, &np) : m->code;
        VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, m->bytes, code };
        ctx("t2/module/%s", m->key);
        VkResult r = V.vkCreateShaderModule(V.dev, &ci, NULL, &m->sm);
        if (code != m->code) free(code);
        if (r) { S("  module %s: %s", m->key, vkc_result_str(r)); m->sm = VK_NULL_HANDLE; }
        VkDescriptorSetLayoutBinding b[24]; uint32_t nb = 0;
        for (int j = 0; j < m->nb; j++)
            b[nb++] = (VkDescriptorSetLayoutBinding){ (uint32_t)m->b[j].binding, (VkDescriptorType)m->b[j].type, (uint32_t)(m->b[j].count ? m->b[j].count : 1),
                                                      m->frag ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo dl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, nb, b };
        if ((r = V.vkCreateDescriptorSetLayout(V.dev, &dl, NULL, &m->dsl))) { S("  set layout %s: %s", m->key, vkc_result_str(r)); return -1; }
    }
    return 0;
}
static void stage_objects_free(void) {
    for (int i = 0; i < g_nmod; i++) {
        if (g_mod[i].sm) V.vkDestroyShaderModule(V.dev, g_mod[i].sm, NULL);
        if (g_mod[i].dsl) V.vkDestroyDescriptorSetLayout(V.dev, g_mod[i].dsl, NULL);
        g_mod[i].sm = VK_NULL_HANDLE; g_mod[i].dsl = VK_NULL_HANDLE;
    }
}
// Merged layout: one set 0 holding both stages' bindings; on a binding both use, the vertex shader's descriptor type wins (the type collision the controls show)
static VkDescriptorSetLayout merged_dsl(const mod_t *v, const mod_t *f) {
    VkDescriptorSetLayoutBinding b[48]; uint32_t nb = 0;
    for (int j = 0; j < v->nb; j++) b[nb++] = (VkDescriptorSetLayoutBinding){ (uint32_t)v->b[j].binding, (VkDescriptorType)v->b[j].type, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
    for (int j = 0; j < f->nb; j++) {
        int dup = -1; for (uint32_t k = 0; k < nb; k++) if ((int)b[k].binding == f->b[j].binding) dup = (int)k;
        if (dup >= 0) b[dup].stageFlags |= VK_SHADER_STAGE_FRAGMENT_BIT;
        else b[nb++] = (VkDescriptorSetLayoutBinding){ (uint32_t)f->b[j].binding, (VkDescriptorType)f->b[j].type, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    }
    VkDescriptorSetLayoutCreateInfo dl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, nb, b };
    VkDescriptorSetLayout s = VK_NULL_HANDLE; V.vkCreateDescriptorSetLayout(V.dev, &dl, NULL, &s);
    return s;
}

// One graphics pipeline for a pair. bad_attr: control, a UINT format on the first float input. Returns the pipeline (or NULL) and fills *pr.
static VkPipeline make_pipeline(const pair_t *q, VkPipelineCache cache, int bad_attr, pres_t *pr) {
    const mod_t *v = &g_mod[q->vs], *f = &g_mod[q->fs];
    memset(pr, 0, sizeof *pr); pr->r = VK_ERROR_UNKNOWN;
    if (!v->sm || !f->sm) return VK_NULL_HANDLE;
    VkDescriptorSetLayout sets[2] = { v->dsl, f->dsl }, mdsl = VK_NULL_HANDLE;
    uint32_t nsets = 2;
    if (g_layout_merged) { mdsl = merged_dsl(v, f); sets[0] = mdsl; nsets = 1; }
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, nsets, sets, 0, NULL };
    VkPipelineLayout pl = VK_NULL_HANDLE;
    if ((pr->r = V.vkCreatePipelineLayout(V.dev, &plci, NULL, &pl))) { if (mdsl) V.vkDestroyDescriptorSetLayout(V.dev, mdsl, NULL); return VK_NULL_HANDLE; }
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, v->sm, v->entry, NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, f->sm, f->entry, NULL } };
    int maxloc = -1; for (int i = 0; i < v->nvin; i++) if (v->vin[i].loc > maxloc) maxloc = v->vin[i].loc;
    VkVertexInputBindingDescription vb = { 0, (uint32_t)(16 * (maxloc + 1)), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[32]; uint32_t nva = 0;
    for (int i = 0; i < v->nvin; i++) {
        VkFormat fm = attr_format(v->vin[i].ty, v->vin[i].n);
        if (bad_attr && v->vin[i].ty == 'f' && bad_attr-- == 1) fm = VK_FORMAT_R32G32B32A32_UINT;
        va[nva++] = (VkVertexInputAttributeDescription){ (uint32_t)v->vin[i].loc, 0, fm, (uint32_t)(16 * v->vin[i].loc) };
    }
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, maxloc >= 0, &vb, nva, va };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 0, NULL, VK_FALSE, VK_FALSE };
    int ncol = 0; for (int i = 0; i < f->nfout; i++) if (f->fout[i].loc + 1 > ncol) ncol = f->fout[i].loc + 1;
    if (ncol > 8) ncol = 8;
    VkFormat cf[8]; VkPipelineColorBlendAttachmentState cb[8];
    for (int i = 0; i < ncol; i++) {
        cf[i] = VK_FORMAT_UNDEFINED;
        for (int j = 0; j < f->nfout; j++) if (f->fout[j].loc == i) cf[i] = color_format(f->fout[j].ty);
        cb[i] = (VkPipelineColorBlendAttachmentState){ VK_FALSE, 0, 0, 0, 0, 0, 0, 0xf };
    }
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, (uint32_t)ncol, cb, { 0 } };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn };
    VkPipelineCreationFeedback fb = { 0 }, sfb[2] = { { 0 }, { 0 } };
    VkPipelineCreationFeedbackCreateInfo fbi = { VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, NULL, &fb, 2, sfb };
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, &fbi, 0, (uint32_t)ncol, cf, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rci, 0, 2, st, &vi, &ia, NULL, &vp, &rs, &ms, NULL, &cbs, &ds, pl,
                                        VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1 };
    VkPipeline pipe = VK_NULL_HANDLE;
    double t0 = now_ms();
    pr->r = V.vkCreateGraphicsPipelines(V.dev, cache, 1, &gp, NULL, &pipe);
    pr->ms = now_ms() - t0;
    pr->ok = pr->r == VK_SUCCESS;
    if (fb.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) { pr->fb_flags = fb.flags; pr->fb_ms = fb.duration * 1e-6; }
    pr->fb_vs = sfb[0].flags; pr->fb_fs = sfb[1].flags;
    V.vkDestroyPipelineLayout(V.dev, pl, NULL);   // a pipeline does not need its layout after creation
    if (mdsl) V.vkDestroyDescriptorSetLayout(V.dev, mdsl, NULL);
    return pipe;
}

static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static VkPipeline g_pipes[400];
static void t2_pass(FILE *o, const char *setname, const char *phase, VkPipelineCache cache) {
    double ms[400]; int n = 0, bad = 0, hits = 0, fbvalid = 0; double tot = 0;
    int vlog0 = g_val_n;
    for (int i = 0; i < g_npair; i++) {
        pair_t *q = &g_pair[i]; pres_t pr;
        ctx("t2/%s/%s/pair%d %s+%s", setname, phase, q->id, g_mod[q->vs].key, g_mod[q->fs].key);
        g_pipes[i] = make_pipeline(q, cache, 0, &pr);
        if (pr.ok) { ms[n++] = pr.ms; tot += pr.ms; } else { bad++; S("  t2 %s %s pair %d (%s + %s): %s", setname, phase, q->id, g_mod[q->vs].key, g_mod[q->fs].key, vkc_result_str(pr.r)); }
        if (pr.fb_flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) fbvalid++;
        if (pr.fb_flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) hits++;
        if (o) { fprintf(o, "%s\t%s\t%d\t%s\t%s\t%s\t%s\t%s\t%.3f\t%#x\t%.3f\t%#x\t%#x\t%d\n", setname, phase, q->id, g_mod[q->vs].key, g_mod[q->fs].key, q->src, q->cover,
                         vkc_result_str(pr.r), pr.ms, pr.fb_flags, pr.fb_ms, pr.fb_vs, pr.fb_fs, g_val_ctx_n); fflush(o); }
    }
    for (int i = 0; i < g_npair; i++) if (g_pipes[i]) { V.vkDestroyPipeline(V.dev, g_pipes[i], NULL); g_pipes[i] = VK_NULL_HANDLE; }
    qsort(ms, n, sizeof *ms, dcmp);
    char it[48]; snprintf(it, sizeof it, "t2.%s.%s", setname, phase);
    R(it, bad ? "FAIL" : "PASS", "pipelines %d ok, %d failed of %d; total %.1f ms, p50 %.2f ms, p90 %.2f ms, max %.2f ms; feedback valid %d, cache hits %d%s",
      n, bad, g_npair, tot, n ? ms[n / 2] : 0, n ? ms[(n * 9) / 10] : 0, n ? ms[n - 1] : 0, fbvalid, hits,
      g_val ? (g_val_n > vlog0 ? " [validation messages, see validation.txt]" : " [validation: none]") : "");
}

static void t2(const char *setname, const char *phases, const char *cache_in, int first_device_ready) {
    FILE *o = open_res("t2.tsv");
    if (o) fprintf(o, "set\tphase\tpair\tvs\tps\tsource\tcover\tresult\tms\tfeedback_flags\tfeedback_ms\tvs_flags\tps_flags\tvalidation_msgs\n");
    char err[512];
    const char *list[] = { "cold", "cold2", "fill", "reload", "disk" };
    VkPipelineCache cache = VK_NULL_HANDLE; void *data = NULL; size_t dsz = 0;
    int ready = first_device_ready;
    for (int li = 0; li < 5; li++) {
        const char *ph = list[li];
        if (!strcmp(ph, "disk") ? !cache_in : (cache_in || !in_list(phases, ph))) continue;
        if (!ready) { drop_device(); if (make_device(err, sizeof err)) { R("t2.device", "FAIL", "%s", err); return; } }
        ready = 0;
        if (stage_objects()) { R("t2.objects", "FAIL", "module / set layout creation failed"); stage_objects_free(); continue; }
        if (!strcmp(ph, "cold") || !strcmp(ph, "cold2")) t2_pass(o, setname, ph, VK_NULL_HANDLE);
        else {
            VkPipelineCacheCreateInfo cci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, 0, NULL };
            void *in = NULL; size_t insz = 0;
            if (!strcmp(ph, "reload")) { cci.initialDataSize = dsz; cci.pInitialData = data; }
            if (!strcmp(ph, "disk")) { in = read_file(cache_in, &insz); if (!in) { R("t2.disk", "FAIL", "cannot read %s", cache_in); stage_objects_free(); continue; } cci.initialDataSize = insz; cci.pInitialData = in; }
            VkResult r = V.vkCreatePipelineCache(V.dev, &cci, NULL, &cache);
            free(in);
            if (r) { R("t2.cache", "FAIL", "vkCreatePipelineCache (%s): %s", ph, vkc_result_str(r)); stage_objects_free(); continue; }
            if (!strcmp(ph, "fill")) {
                t2_pass(o, setname, "fill", cache);
                if (in_list(phases, "warm")) t2_pass(o, setname, "warm", cache);
                size_t n = 0; V.vkGetPipelineCacheData(V.dev, cache, &n, NULL);
                free(data); data = malloc(n ? n : 1); dsz = n;
                if (V.vkGetPipelineCacheData(V.dev, cache, &dsz, data)) dsz = 0;
                S("  pipeline cache data after fill/warm: %zu bytes", dsz);
                FILE *cf = open_res("cache.bin"); if (cf) { fwrite(data, 1, dsz, cf); fclose(cf); }
                R("t2.cache_size", "INFO", "%s: %zu bytes of VkPipelineCache data for %d pipelines (res/cache.bin)", setname, dsz, g_npair);
            } else t2_pass(o, setname, ph, cache);
            V.vkDestroyPipelineCache(V.dev, cache, NULL); cache = VK_NULL_HANDLE;
        }
        stage_objects_free();
    }
    free(data);
    if (o) fclose(o);
}

// ---------------------------------------------------------------- validation controls (macOS): each must produce at least one validation error
static int g_vout_n[160], g_fin_n[160];
static void count_io(const char *dir) {   // vertex shader output / pixel shader input counts per module (manifest vout / fin lines)
    char p[900]; snprintf(p, sizeof p, "%s/manifest.txt", dir);
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[512], k[24];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "vout %23s", k) == 1) { int m = mod_find(k); if (m >= 0) g_vout_n[m]++; }
        else if (sscanf(line, "fin %23s", k) == 1) { int m = mod_find(k); if (m >= 0) g_fin_n[m]++; }
    }
    fclose(f);
}
static void control_one(const char *item, const pair_t *q, int bad_attr, const char *vuid, const char *what) {
    pres_t pr; ctx("control/%s/%s+%s", item, g_mod[q->vs].key, g_mod[q->fs].key);
    int before = g_val_n; g_expect_vuid = vuid; g_expect_hits = 0;   // PASS only on the expected VUID: every pair also raises maxCombinedClipAndCullDistances-00710
    VkPipeline p = make_pipeline(q, VK_NULL_HANDLE, bad_attr, &pr); if (p) V.vkDestroyPipeline(V.dev, p, NULL);
    g_expect_vuid = NULL;
    char it[48]; snprintf(it, sizeof it, "control.%s", item);
    R(it, g_expect_hits ? "PASS" : "FAIL", "%s + %s, %s: %d x %s among %d validation messages; vkCreateGraphicsPipelines %s", g_mod[q->vs].key, g_mod[q->fs].key, what,
      g_expect_hits, vuid, g_val_n - before, vkc_result_str(pr.r));
}
static void controls(void) {
    S("== validation controls (each should report at least one error)");
    char err[256];
    drop_device(); if (make_device(err, sizeof err)) { S("controls: device: %s", err); return; }
    // 1. both stages in set 0, as translated: the first pair with a binding both stages use with different descriptor types
    int coll = -1;
    for (int i = 0; i < g_npair && coll < 0; i++) {
        const mod_t *v = &g_mod[g_pair[i].vs], *f = &g_mod[g_pair[i].fs];
        for (int a = 0; a < v->nb; a++) for (int b = 0; b < f->nb; b++) if (v->b[a].binding == f->b[b].binding && v->b[a].type != f->b[b].type) coll = i;
    }
    int save = g_layout_merged;
    if (coll >= 0) { g_layout_merged = 1; stage_objects(); control_one("merged_set0", &g_pair[coll], 0, "VkGraphicsPipelineCreateInfo-layout-07990", "both stages in set 0 (expected: descriptor type mismatch)"); stage_objects_free(); }
    else R("control.merged_set0", "SKIP", "no pair with a binding type collision");
    g_layout_merged = save;
    stage_objects();
    // 2. a pixel shader input the vertex shader does not write: the vertex shader with the fewest outputs and the pixel shader with the most inputs
    int bv = -1, bf_ = -1, vmin = 99, fmax = -1;
    for (int i = 0; i < g_nmod; i++) {
        if (!g_mod[i].frag && g_vout_n[i] < vmin) { vmin = g_vout_n[i]; bv = i; }
        if (g_mod[i].frag && g_fin_n[i] > fmax) { fmax = g_fin_n[i]; bf_ = i; }
    }
    if (bv >= 0 && bf_ >= 0 && vmin < fmax) { pair_t q = { 999, bv, bf_, "control", "missing" }; char w[96]; snprintf(w, sizeof w, "%d outputs for %d inputs (expected: input not written)", vmin, fmax); control_one("uncovered_input", &q, 0, "RuntimeSpirv-OpEntryPoint-08743", w); }
    else R("control.uncovered_input", "SKIP", "no combination leaves a pixel shader input uncovered");
    // 3. a UINT vertex format on a float input
    if (g_npair) control_one("uint_format", &g_pair[0], 1, "VkGraphicsPipelineCreateInfo-Input-08733", "R32G32B32A32_UINT on a float input (expected: format type mismatch)");
    stage_objects_free();
}

// ---------------------------------------------------------------- buffers and images for T3 / T4
typedef struct { VkBuffer b; VkDeviceMemory m; void *p; VkDeviceSize size; } hbuf_t;
static int hbuf(hbuf_t *h, VkDeviceSize size, VkBufferUsageFlags usage) {
    memset(h, 0, sizeof *h); h->size = size;
    uint32_t bits = 0;
    if (vkc_buffer(&V, VK_NULL_HANDLE, 0, size, usage, &h->b, &bits)) return -1;
    int t = vkc_memtype(&V, bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    VkMemoryRequirements req; V.vkGetBufferMemoryRequirements(V.dev, h->b, &req);
    if (t < 0 || vkc_alloc(&V, req.size, (uint32_t)t, 0, &h->m)) return -1;
    if (V.vkBindBufferMemory(V.dev, h->b, h->m, 0)) return -1;
    if (V.vkMapMemory(V.dev, h->m, 0, VK_WHOLE_SIZE, 0, &h->p)) return -1;
    return 0;
}
static void hbuf_free(hbuf_t *h) {
    if (h->b) V.vkDestroyBuffer(V.dev, h->b, NULL);
    if (h->m) V.vkFreeMemory(V.dev, h->m, NULL);
    memset(h, 0, sizeof *h);
}

// ---------------------------------------------------------------- T3
#define T3W 64
#define T3H 64
static VkPipeline t3_pipeline(VkPipelineLayout pl, VkShaderModule vs, VkShaderModule fs, VkCullModeFlags cull) {
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL } };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL,
        cull, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cb = { VK_FALSE, 0, 0, 0, 0, 0, 0, 0xf };
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &cb, { 0 } };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn };
    VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &cf, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rci, 0, 2, st, &vi, &ia, NULL, &vp, &rs, &ms, NULL, &cbs, &ds, pl, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1 };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = V.vkCreateGraphicsPipelines(V.dev, VK_NULL_HANDLE, 1, &gp, NULL, &p);
    if (r) S("  t3 pipeline: %s", vkc_result_str(r));
    return p;
}
static void img_barrier(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags dst) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, sa, da, from, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    V.vkCmdPipelineBarrier(V.cmd, ss, dst, 0, 0, NULL, 0, NULL, 1, &b);
}
static void t3(void) {
    ctx("t3");
    FILE *o = open_res("t3.txt");
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo c1 = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof ydir_vert_spv, ydir_vert_spv };
    VkShaderModuleCreateInfo c2 = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof ydir_frag_spv, ydir_frag_spv };
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl = VK_NULL_HANDLE;
    if (V.vkCreateShaderModule(V.dev, &c1, NULL, &vs) || V.vkCreateShaderModule(V.dev, &c2, NULL, &fs) || V.vkCreatePipelineLayout(V.dev, &plci, NULL, &pl)) { R("t3", "FAIL", "setup"); return; }
    VkPipeline p_none = t3_pipeline(pl, vs, fs, VK_CULL_MODE_NONE), p_back = t3_pipeline(pl, vs, fs, VK_CULL_MODE_BACK_BIT), p_front = t3_pipeline(pl, vs, fs, VK_CULL_MODE_FRONT_BIT);
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, { T3W, T3H, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img = VK_NULL_HANDLE; VkDeviceMemory im = VK_NULL_HANDLE; VkImageView iv = VK_NULL_HANDLE; hbuf_t rb;
    V.vkCreateImage(V.dev, &ici, NULL, &img);
    VkMemoryRequirements req; V.vkGetImageMemoryRequirements(V.dev, img, &req);
    int t = vkc_memtype(&V, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0); if (t < 0) t = vkc_memtype(&V, req.memoryTypeBits, 0, 0);
    vkc_alloc(&V, req.size, (uint32_t)t, 0, &im); V.vkBindImageMemory(V.dev, img, im, 0);
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    V.vkCreateImageView(V.dev, &vci, NULL, &iv);
    if (!p_none || !p_back || !p_front || !iv || hbuf(&rb, T3W * T3H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) { R("t3", "FAIL", "pipelines / image / buffer"); goto out; }
    struct { const char *name; int neg; VkPipeline p; } var[7] = { { "vp+ cull-none", 0, p_none }, { "vp- cull-none", 1, p_none }, { "vp+ cull-back(front=CCW)", 0, p_back }, { "vp- cull-back(front=CCW)", 1, p_back },
                                                                   { "vp+ cull-front(front=CCW)", 0, p_front }, { "vp- cull-front(front=CCW)", 1, p_front },
                                                                   { "vp+ cull-none (repeat)", 0, p_none } };   // the repeat checks that state from earlier draws does not leak
    int first_row[7], last_row[7], npx[7], gl_ok[7], nclear[7], nfill[7], ran[7];
    for (int k = 0; k < 7; k++) {
        ctx("t3/%s", var[k].name);
        memset(rb.p, 0xee, T3W * T3H * 4);
        vkc_cmd_begin(&V);
        img_barrier(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, iv, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_NULL_HANDLE, 0,
                                         VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, { .color = { .float32 = { 0, 0, (float)(10 * (k + 1)) / 255.0f, 0 } } } };   // a clear colour per draw: shows the pass ran
        VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, { { 0, 0 }, { T3W, T3H } }, 1, 0, 1, &ca, NULL, NULL };
        pBeginRendering(V.cmd, &ri);
        V.vkCmdBindPipeline(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, var[k].p);
        VkViewport vpt = var[k].neg ? (VkViewport){ 0, T3H, T3W, -(float)T3H, 0, 1 } : (VkViewport){ 0, 0, T3W, T3H, 0, 1 };
        VkRect2D sc = { { 0, 0 }, { T3W, T3H } };
        pSetViewport(V.cmd, 0, 1, &vpt); pSetScissor(V.cmd, 0, 1, &sc);
        V.vkCmdDraw(V.cmd, 3, 1, 0, 0);
        pEndRendering(V.cmd);
        img_barrier(img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy bic = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { T3W, T3H, 1 } };
        V.vkCmdCopyImageToBuffer(V.cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &bic);
        VkMemoryBarrier hb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
        V.vkCmdPipelineBarrier(V.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
        VkResult r = vkc_cmd_submit_wait(&V, 5000000000ull);
        const uint8_t *px = rb.p;
        first_row[k] = -1; last_row[k] = -1; npx[k] = 0; gl_ok[k] = 1; nclear[k] = 0; nfill[k] = 0;
        char rows[T3H + 1];
        for (int y = 0; y < T3H; y++) {
            int c = 0;
            for (int x = 0; x < T3W; x++) {
                const uint8_t *q = px + (y * T3W + x) * 4;
                if (q[0] == 255) { c++; if (q[1] != y) gl_ok[k] = 0; } else if (q[0] == 0 && q[2] == 10 * (k + 1)) nclear[k]++;
                else if (q[0] == 0xee && q[1] == 0xee && q[2] == 0xee && q[3] == 0xee) nfill[k]++;   // the copy did not write this pixel
            }
            npx[k] += c; rows[y] = c == T3W ? '#' : c ? '+' : '.';
            if (c) { if (first_row[k] < 0) first_row[k] = y; last_row[k] = y; }
        }
        rows[T3H] = 0;
        ran[k] = npx[k] + nclear[k] == T3W * T3H;   // every pixel is this draw's colour or this pass's clear: the pass ran; otherwise its rows say nothing
        if (!npx[k]) gl_ok[k] = 0;
        if (o) fprintf(o, "%-26s submit %s: pass ran: %s; covered memory rows %d..%d, %d pixels, %d pixels of this draw's clear colour, %d never written by the copy; FragCoord.y == row: %s\n  rows (0 = first in memory): %s\n",
                       var[k].name, vkc_result_str(r), ran[k] ? "yes" : "NO", first_row[k], last_row[k], npx[k], nclear[k], nfill[k], gl_ok[k] ? "yes" : npx[k] ? "no" : "-", rows);
        uint32_t p0, p1; memcpy(&p0, px, 4); memcpy(&p1, px + ((T3H - 1) * T3W) * 4, 4);   // RGBA bytes, little-endian: 0xAABBGGRR
        S("  t3 %-26s %s rows %d..%d, %d px, %d px clear, %d px unwritten; first/last row pixel %08x/%08x; rows %s", var[k].name, ran[k] ? "ran " : "LOST", first_row[k], last_row[k], npx[k], nclear[k], nfill[k], p0, p1, rows);
    }
    {
        int full = T3W * T3H / 2;
        int pos_top = first_row[0] == 0 && last_row[0] == T3H / 2 - 1 && npx[0] == full;
        int neg_bot = first_row[1] == T3H / 2 && last_row[1] == T3H - 1 && npx[1] == full;
        if (!ran[0] || !ran[1]) R("t3.ydir", "FAIL", "the render pass of the %s viewport draw did not run (its clear colour is missing)", !ran[0] ? "positive" : "negative"); else
        R("t3.ydir", pos_top && neg_bot ? "PASS" : "INFO",
          "clip y<0 lands on memory rows %d..%d with a positive viewport height and %d..%d with a negative one (%d / %d px of %d); FragCoord.y is the memory row: %s",
          first_row[0], last_row[0], first_row[1], last_row[1], npx[0], npx[1], full, gl_ok[0] && gl_ok[1] ? "yes" : "no");
        // facing per viewport sign from the BACK and FRONT culling draws: front = drawn with BACK culled, culled with FRONT culled
        const char *face[2];
        for (int n = 0; n < 2; n++) { int b = npx[2 + n] > 0, f = npx[4 + n] > 0;
            face[n] = !ran[2 + n] || !ran[4 + n] ? "not measurable (a render pass did not run)" : b && !f ? "front" : !b && f ? "back" : b && f ? "drawn with either cull mode (culling not applied)" : "culled with either cull mode"; }
        int all_clear = 1; for (int k = 0; k < 7; k++) if (!ran[k]) all_clear = 0;
        int rep_ok = first_row[6] == first_row[0] && last_row[6] == last_row[0] && npx[6] == npx[0];
        R("t3.state", all_clear && rep_ok ? "PASS" : "FAIL", "every draw's pass cleared the rest of the image: %s; a repeat of the first draw after the others matches it: %s (rows %d..%d)",
          all_clear ? "yes" : "no", rep_ok ? "yes" : "no", first_row[6], last_row[6]);
        R("t3.winding", face[0][0] == 'b' && face[1][0] == 'f' ? "PASS" : "INFO",
          "triangle counter-clockwise in NDC with y up, front face CCW: %s with a positive viewport height, %s with a negative one (expected: back, front; the spec computes facing in framebuffer coordinates)",
          face[0], face[1]);
    }
out:
    hbuf_free(&rb);
    if (p_none) V.vkDestroyPipeline(V.dev, p_none, NULL);
    if (p_back) V.vkDestroyPipeline(V.dev, p_back, NULL);
    if (p_front) V.vkDestroyPipeline(V.dev, p_front, NULL);
    if (iv) V.vkDestroyImageView(V.dev, iv, NULL);
    if (img) V.vkDestroyImage(V.dev, img, NULL);
    if (im) V.vkFreeMemory(V.dev, im, NULL);
    V.vkDestroyPipelineLayout(V.dev, pl, NULL);
    V.vkDestroyShaderModule(V.dev, vs, NULL); V.vkDestroyShaderModule(V.dev, fs, NULL);
    if (o) fclose(o);
}

// ---------------------------------------------------------------- T4
#define QNAN 0x7fc00000u
#define NQNAN 0xffc00000u
#define SNAN 0x7f800001u
#define POSZERO 0x00000000u
#define NEGZERO 0x80000000u
#define ONE 0x3f800000u
#define TWO 0x40000000u
#define THREE 0x40400000u
#define MTWO 0xc0000000u
#define QUARTER 0x3e800000u
#define PINF 0x7f800000u
#define NINF 0xff800000u
static const uint32_t T4IN[][2] = { { QNAN, ONE }, { ONE, QNAN }, { QNAN, QNAN }, { NQNAN, ONE }, { ONE, NQNAN }, { SNAN, ONE }, { ONE, SNAN }, { POSZERO, NEGZERO }, { NEGZERO, POSZERO },
                                    { TWO, THREE }, { NINF, QNAN }, { QNAN, PINF }, { MTWO, QNAN }, { QUARTER, QNAN }, { NQNAN, MTWO } };
#define T4N (int)(sizeof T4IN / sizeof T4IN[0])
#define T4OPS 8
static const char *T4OPNAME[T4OPS] = { "min(a,b)", "max(a,b)", "max(0,a)", "max(a,0)", "clamp(a,0,1)", "clamp(0.5,a,1)", "clamp(0.5,0,a)", "min(1,a)" };
static float fb(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t bf(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static int isnanu(uint32_t u) { return (u & 0x7f800000u) == 0x7f800000u && (u & 0x7fffffu); }
// Maxwell FMNMX (= GLSL.std.450 NMin / NMax): a NaN operand yields the other operand, two NaNs a NaN
static uint32_t nmin(uint32_t a, uint32_t b) { if (isnanu(a)) return isnanu(b) ? QNAN : b; if (isnanu(b)) return a; return fb(a) < fb(b) ? a : fb(b) < fb(a) ? b : (a | b); }
static uint32_t nmax(uint32_t a, uint32_t b) { if (isnanu(a)) return isnanu(b) ? QNAN : b; if (isnanu(b)) return a; return fb(a) > fb(b) ? a : fb(b) > fb(a) ? b : (a & b); }
static uint32_t expect(int op, uint32_t a, uint32_t b) {
    uint32_t z = POSZERO, one = ONE, half = bf(0.5f);
    switch (op) {
    case 0: return nmin(a, b); case 1: return nmax(a, b); case 2: return nmax(z, a); case 3: return nmax(a, z);
    case 4: return nmin(nmax(a, z), one); case 5: return nmin(nmax(half, a), one); case 6: return nmin(nmax(half, z), a); case 7: return nmin(one, a);
    }
    return 0;
}
static int nan_case(int op, uint32_t a, uint32_t b) { return op <= 1 ? (isnanu(a) || isnanu(b)) : isnanu(a); }   // the result depends on a NaN operand
// cases T4 does not grade: clamp with minVal > maxVal (undefined in GLSL.std.450), and a +0 / -0 tie in max(0,a), max(a,0), clamp(a,0,1) (the ordering of signed zeros is not established for Maxwell)
static int undef_case(int op, uint32_t a) { return !isnanu(a) && ((op == 5 && fb(a) > 1.0f) || (op == 6 && fb(a) < 0.0f)); }
static int ztie_case(int op, uint32_t a) { return op >= 2 && op <= 4 && a == NEGZERO; }
static int zero_case(int op, uint32_t a, uint32_t b) { return op <= 1 && (a & 0x7fffffffu) == 0 && (b & 0x7fffffffu) == 0 && a != b; }

static void t4(void) {
    FILE *o = open_res("t4.tsv");
    if (o) fprintf(o, "variant\tcase\ta\tb\top\tresult\texpected_fmnmx\tmatch\n");
    struct { const char *name; const uint32_t *code; size_t bytes; int needs_preserve; } var[4] = {
        { "F", nanmm_f_spv, sizeof nanmm_f_spv, 0 }, { "N", nanmm_n_spv, sizeof nanmm_n_spv, 0 },
        { "F+preserve", nanmm_fp_spv, sizeof nanmm_fp_spv, 1 }, { "N+preserve", nanmm_np_spv, sizeof nanmm_np_spv, 1 } };
    VkPhysicalDeviceFloatControlsProperties fc = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &fc };
    V.vkGetPhysicalDeviceProperties2(V.pd, &p2);
    S("  t4: shaderSignedZeroInfNanPreserveFloat32 = %u", fc.shaderSignedZeroInfNanPreserveFloat32);
    hbuf_t in, out;
    if (hbuf(&in, 16 + 8 * T4N, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || hbuf(&out, 4 * T4N * T4OPS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) { R("t4", "FAIL", "buffers"); return; }
    uint32_t *ip = in.p; ip[0] = T4N; ip[1] = ip[2] = ip[3] = 0;
    for (int i = 0; i < T4N; i++) { ip[4 + 2 * i] = T4IN[i][0]; ip[5 + 2 * i] = T4IN[i][1]; }
    for (int vi = 0; vi < 4; vi++) {
        ctx("t4/%s", var[vi].name);
        if (var[vi].needs_preserve && !fc.shaderSignedZeroInfNanPreserveFloat32) { R("t4", "SKIP", "%s: no shaderSignedZeroInfNanPreserveFloat32", var[vi].name); continue; }
        vkc_compute_t c;
        VkResult r = vkc_compute(&V, var[vi].code, var[vi].bytes, 2, 0, &c);
        if (r) { char it[48]; snprintf(it, sizeof it, "t4.%s", var[vi].name); R(it, "FAIL", "compute pipeline: %s", vkc_result_str(r)); vkc_compute_destroy(&V, &c); continue; }
        vkc_compute_bind(&V, &c, 0, in.b, 0, VK_WHOLE_SIZE); vkc_compute_bind(&V, &c, 1, out.b, 0, VK_WHOLE_SIZE);
        memset(out.p, 0xcd, out.size);
        vkc_cmd_begin(&V);
        V.vkCmdBindPipeline(V.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.pipe);
        V.vkCmdBindDescriptorSets(V.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.pl, 0, 1, &c.ds, 0, NULL);
        V.vkCmdDispatch(V.cmd, T4N, 1, 1);
        VkMemoryBarrier hb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
        V.vkCmdPipelineBarrier(V.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
        r = vkc_cmd_submit_wait(&V, 5000000000ull);
        const uint32_t *res = out.p;
        int bad_op[T4OPS] = { 0 }, nan_op[T4OPS] = { 0 }, oth_bad[T4OPS] = { 0 }, oth_op[T4OPS] = { 0 }; char zeros[256] = ""; size_t zl = 0;
        for (int i = 0; i < T4N; i++) for (int op = 0; op < T4OPS; op++) {
            uint32_t a = T4IN[i][0], b = T4IN[i][1], got = res[i * T4OPS + op], want = expect(op, a, b);
            int m = isnanu(want) ? isnanu(got) : got == want;
            const char *tag = "";
            if (zero_case(op, a, b)) { tag = "zero"; if (zl < sizeof zeros - 40) zl += snprintf(zeros + zl, sizeof zeros - zl, " %s(%s0,%s0)=%s0", op ? "max" : "min", a ? "-" : "+", b ? "-" : "+", got == NEGZERO ? "-" : got == POSZERO ? "+" : "?"); }
            else if (undef_case(op, a)) tag = "undefined";
            else if (ztie_case(op, a)) { tag = (got & 0x7fffffffu) == 0 ? "zero" : "NO"; if (*tag == 'N') oth_bad[op]++; }
            else if (nan_case(op, a, b)) { nan_op[op]++; if (!m) bad_op[op]++; }
            else { oth_op[op]++; if (!m) oth_bad[op]++; }
            if (o) fprintf(o, "%s\t%d\t%#010x\t%#010x\t%s\t%#010x\t%#010x\t%s\n", var[vi].name, i, a, b, T4OPNAME[op], got, want, *tag ? tag : m ? "yes" : "NO");
        }
        char det[900]; size_t dl = 0; int tot = 0, tot_oth = 0, n_oth = 0;
        for (int op = 0; op < T4OPS; op++) { tot += bad_op[op]; tot_oth += oth_bad[op]; n_oth += oth_op[op]; dl += snprintf(det + dl, sizeof det - dl, "%s%s %d/%d", op ? ", " : "", T4OPNAME[op], bad_op[op], nan_op[op]); }
        dl += snprintf(det + dl, sizeof det - dl, "; mismatches in the other graded cases (no NaN operand): %d/%d (not graded: clamps with minVal > maxVal, and +0/-0 ties)", tot_oth, n_oth);
        tot += tot_oth;
        char it[48]; snprintf(it, sizeof it, "t4.%s", var[vi].name);
        R(it, r ? "FAIL" : tot ? "INFO" : "PASS", "%s: mismatches with Maxwell FMNMX (NaN operand -> the other operand; clamp NaN -> 0) per op, mismatched/NaN-dependent cases: %s; signed zeros (min/max of +0,-0):%s",
          r ? vkc_result_str(r) : "ok", det, zeros);
        vkc_compute_destroy(&V, &c);
    }
    hbuf_free(&in); hbuf_free(&out);
    if (o) fclose(o);
}

// ---------------------------------------------------------------- main
int probe_main(const char *out_dir, int in_app, int argc, char **argv) {
    g_in_app = in_app;
    snprintf(g_resdir, sizeof g_resdir, "%s/res", out_dir); mkdir(g_resdir, 0755);
    char p[900]; snprintf(p, sizeof p, "%s/summary.txt", g_resdir); g_sum = fopen(p, "w");
    const char *label = arg(argc, argv, "--label", "?"), *load = arg(argc, argv, "--load", "system");
    const char *tests = arg(argc, argv, "--tests", "t1,t2,t3,t4"), *setname = arg(argc, argv, "--set", "stripped");
    const char *phases = arg(argc, argv, "--phases", "cold,cold2,fill,warm,reload"), *cache_in = arg(argc, argv, "--cache-in", NULL);
    int validation = flag(argc, argv, "--validation");
    g_layout_merged = !strcmp(arg(argc, argv, "--layout", "split"), "merged");
    g_orig = !strcmp(setname, "orig");
    char dir[700]; const char *d = arg(argc, argv, "--dir", "shader");
    if (*d != '/') snprintf(dir, sizeof dir, "%s/%s", out_dir, d); else snprintf(dir, sizeof dir, "%s", d);
    char cin[800]; if (cache_in && *cache_in != '/') { snprintf(cin, sizeof cin, "%s/%s", out_dir, cache_in); cache_in = cin; }
    if (!flag(argc, argv, "--mesa-cache")) { setenv("MESA_SHADER_CACHE_DISABLE", "true", 1); setenv("MESA_DISK_CACHE_SINGLE_FILE", "0", 1); }
    if (validation) { snprintf(p, sizeof p, "%s/validation.txt", g_resdir); g_val = fopen(p, "w"); if (g_val) fprintf(g_val, "context\tseverity\tid\tmessage\n"); }
    S("shader probe: label %s, context %s, pid %d, set %s, tests %s, phases %s, layout %s%s%s", label, in_app ? "app" : "exe", (int)getpid(), setname, tests, phases,
      g_layout_merged ? "merged" : "split (vertex set 0, pixel set 1)", validation ? ", validation layer" : "", cache_in ? ", cache-in" : "");
    for (int i = 0; i < argc; i++) S("arg[%d] %s", i, argv[i]);
    S("MESA_SHADER_CACHE_DISABLE=%s", getenv("MESA_SHADER_CACHE_DISABLE") ? getenv("MESA_SHADER_CACHE_DISABLE") : "(unset)");
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--env")) {   // --env NAME=VALUE (repeatable): driver debug variables such as TU_DEBUG, set before the driver loads
        char kv[256]; snprintf(kv, sizeof kv, "%s", argv[i + 1]); char *eq = strchr(kv, '=');
        if (eq) { *eq = 0; setenv(kv, eq + 1, 1); S("env %s=%s", kv, eq + 1); }
    }

    char err[512] = "";
#ifdef __APPLE__
    (void)load;
    if (load_apple(err, sizeof err)) { S("LOAD FAILED: %s", err); R("load", "FAIL", "%s", err); return 2; }
#else
    char selfdir[512] = ""; vkc_self_dir(selfdir, sizeof selfdir);
    const char *dd = arg(argc, argv, "--drvdir", ""); char ddabs[800];
    if (*dd && *dd != '/') { snprintf(ddabs, sizeof ddabs, "%s/%s", out_dir, dd); dd = ddabs; }
    vkc_load_opts_t lo = { load, selfdir, dd, arg(argc, argv, "--drvname", "") };
    if (vkc_load(&V, &lo, err, sizeof err)) { S("LOAD FAILED: %s (%s)", err, V.load_desc); R("load", "FAIL", "%s", err); return 2; }
#endif
    S("loaded: %s", V.load_desc);
    if (make_instance(validation, err, sizeof err)) { S("INSTANCE FAILED: %s", err); R("load", "FAIL", "%s", err); return 2; }
    {
        VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f13 };
        VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11 };
        V.vkGetPhysicalDeviceFeatures2(V.pd, &f2);
        g_have = f2.features; g_have11 = f11; g_have13 = f13;
    }
    {   // what the translated modules need beyond core: limits and optional features the shader chain has to strip or emulate
        int has_dcc = vkc_has_ext(&V, "VK_EXT_depth_clip_control"), has_fsi = vkc_has_ext(&V, "VK_EXT_fragment_shader_interlock");
        VkPhysicalDeviceDepthClipControlFeaturesEXT dcc = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT };
        VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT fsi = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT };
        VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        if (has_dcc) { dcc.pNext = f2.pNext; f2.pNext = &dcc; }
        if (has_fsi) { fsi.pNext = f2.pNext; f2.pNext = &fsi; }
        V.vkGetPhysicalDeviceFeatures2(V.pd, &f2);
        VkPhysicalDeviceSubgroupProperties sg = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sg };
        V.vkGetPhysicalDeviceProperties2(V.pd, &p2);
        const VkPhysicalDeviceLimits *l = &V.props.limits;
        R("caps", "INFO", "maxClipDistances %u maxCullDistances %u maxCombinedClipAndCullDistances %u; shaderFloat64 %u; VK_EXT_depth_clip_control %s (depthClipControl %u); "
          "VK_EXT_fragment_shader_interlock %s (pixelInterlock %u); subgroup size %u, stages 0x%x, ops 0x%x (ballot %u shuffle %u vote %u), quadOpsInAllStages %u",
          l->maxClipDistances, l->maxCullDistances, l->maxCombinedClipAndCullDistances, g_have.shaderFloat64, has_dcc ? "yes" : "no", dcc.depthClipControl,
          has_fsi ? "yes" : "no", fsi.fragmentShaderPixelInterlock, sg.subgroupSize, sg.supportedStages, sg.supportedOperations,
          !!(sg.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT), !!(sg.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT), !!(sg.supportedOperations & VK_SUBGROUP_FEATURE_VOTE_BIT),
          sg.quadOperationsInAllStages);
    }
    if (make_device(err, sizeof err)) { S("DEVICE FAILED: %s", err); R("load", "FAIL", "%s", err); return 2; }
    vkc_update_new_libs(&V);
    S("device: %s, api %u.%u.%u, driverID %d \"%s\" / \"%s\"; enabled: %s", V.props.deviceName, VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion),
      VK_API_VERSION_PATCH(V.props.apiVersion), V.drv.driverID, V.drv.driverName, V.drv.driverInfo, g_enabled);
    S("shared objects mapped since before loading:\n%s", V.new_libs);
    const char *xl = arg(argc, argv, "--expect-lib", NULL), *xi = arg(argc, argv, "--expect-info", NULL), *xid = arg(argc, argv, "--expect-id", NULL);
    int id_ok = 1; char idw[512] = "";
    if (xl) { static char all[40000]; snprintf(all, sizeof all, "%s%s", V.maps_before, V.new_libs); if (!strstr(all, xl)) { id_ok = 0; snprintf(idw + strlen(idw), sizeof idw - strlen(idw), " lib %s not mapped;", xl); } }
    if (xid && atoi(xid) != (int)V.drv.driverID) { id_ok = 0; snprintf(idw + strlen(idw), sizeof idw - strlen(idw), " driverID %d != %s;", V.drv.driverID, xid); }
    if (xi && !strstr(V.drv.driverInfo, xi) && !strstr(V.drv.driverName, xi)) { id_ok = 0; snprintf(idw + strlen(idw), sizeof idw - strlen(idw), " driverInfo lacks \"%s\";", xi); }
    R("identity", id_ok ? "PASS" : "FAIL", "%s driverID %d \"%s\" / \"%s\" api %u.%u.%u%s", label, V.drv.driverID, V.drv.driverName, V.drv.driverInfo,
      VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion), VK_API_VERSION_PATCH(V.props.apiVersion), idw);
    if (!id_ok) { S("IDENTITY MISMATCH: not the expected driver"); return 3; }
    if (!g_have13.dynamicRendering) { R("features", "FAIL", "no dynamicRendering"); return 2; }

    double t0 = now_ms();
    int need_manifest = in_list(tests, "t1") || in_list(tests, "t2") || flag(argc, argv, "--controls");
    if (need_manifest && load_manifest(dir, setname)) { R("manifest", "FAIL", "cannot load the manifest or modules from %s (%s)", dir, setname); return 2; }
    count_io(dir);
    if (in_list(tests, "t1")) t1(setname);
    if (in_list(tests, "t2")) t2(setname, phases, cache_in, 1);
    if (flag(argc, argv, "--controls")) controls();
    if (in_list(tests, "t3") || in_list(tests, "t4")) {
        char err2[256];
        drop_device(); if (make_device(err2, sizeof err2)) { R("device", "FAIL", "%s", err2); return 2; }
        if (in_list(tests, "t3")) t3();
        if (in_list(tests, "t4")) t4();
    }
    S("tests took %.1f s; validation messages (warnings and errors): %d", (now_ms() - t0) / 1e3, g_val_n);
    if (validation) R("validation", g_val_n ? "INFO" : "PASS", "%d validation warnings / errors in this run (validation.txt)", g_val_n);
    FILE *rt = open_res("results.tsv");
    S("==== results (%s, %s, %s)", label, in_app ? "app" : "exe", setname);
    for (int i = 0; i < g_nres; i++) { S("%-30s %-4s %s", g_results[i].item, g_results[i].verdict, g_results[i].detail); if (rt) fprintf(rt, "%s\t%s\t%s\n", g_results[i].item, g_results[i].verdict, g_results[i].detail); }
    if (rt) fclose(rt);
    for (int i = 0; i < g_nmod; i++) free(g_mod[i].code);
    drop_device();
    if (g_messenger) pDestroyMessenger(V.inst, g_messenger, NULL);
    if (V.inst) V.vkDestroyInstance(V.inst, NULL);
    free(V.ext);
    if (g_val) fclose(g_val);
    if (g_sum) fclose(g_sum);
    return 0;
}
