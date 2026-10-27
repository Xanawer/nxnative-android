// P0.7 GPU memory probe (docs/android/PLAN.md, Phase 0; results in docs/android/probes-gpu.md).
// One driver per run: the system Vulkan loader, or a custom driver loaded through libadrenotools. Writes <out_dir>/res/:
//   summary.txt (every step, flushed line by line, RESULT lines with PASS / FAIL / SKIP / INFO), results.tsv (the RESULT lines),
//   extensions.txt, features.txt, properties.txt, memory.txt, formats.txt, maps_*.txt.
// Arguments: --label <name> --load system|adrenotools [--drvdir <dir> --drvname <file>] [--expect-lib <s>] [--expect-id <n>] [--expect-info <s>]
//            [--tests caps,a,d,c,bw,kdiag,coh,fresh,dirty,vmas,types,free,b,e,ahb,ahbbw,ahbsize,ahbmany,ahbchurn] [--heavy] [--bw-reps <n>] [--max-total-gb <x>] [--floor-gb <x>]
//            [--ahb-floor-gb <x>] [--ahb-many-max <n>] [--ahb-churn-cycles <n>]
//   ahb*: AHardwareBuffer-backed pools (ahb_probe.c; the method is shirai91/nx2android's). ahbsize, ahbmany and ahbchurn allocate heavily: APK only.
//         No AHB above 64 MB is ever requested or queried (AHB_CAP: a larger query crashed the system allocator service on the Fold 7).
//   --heavy adds b (one 3.5 GB allocation). e (total allocatable up to --max-total-gb) needs a process of its own (--tests e): the drivers
//   tested do not give freed memory back to the system before the process exits (test "free"), so e after b would measure b's leftovers.
//   Run b and e in the APK only: an adb shell process has oom_score_adj -1000, so the low-memory killer would kill other apps first.
#define _GNU_SOURCE
#include <arm_neon.h>
#include <dlfcn.h>
#include <errno.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <android/log.h>
#include "vk_common.h"
#include "vkdump_gen.h"
#include "coh_desc_spv.h"
#include "coh_bda_spv.h"
#include "diag_desc_push_spv.h"
#include "diag_bda_push_spv.h"
#include "diag_desc_buf_spv.h"
#include "diag_bda_buf_spv.h"

#define MB (1024ull * 1024ull)
#define GB (1024ull * MB)

// ---------------------------------------------------------------- output
static FILE *g_sum;
static char g_resdir[600];
static int g_in_app;
typedef struct { char item[48]; char verdict[8]; char detail[600]; } result_t;
#define NRES 400
static result_t g_results[NRES]; static int g_nres;

static void S(const char *fmt, ...) {
    char buf[4096]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (g_sum) { fputs(buf, g_sum); fputc('\n', g_sum); fflush(g_sum); }
    __android_log_print(ANDROID_LOG_INFO, "nxprobe-gpu", "%s", buf);
    if (!g_in_app) { puts(buf); fflush(stdout); }
}
static vkc_t V;
static int g_lost_seen;   // the device was lost: every later GPU result is void (V.lost, set by vkc_cmd_submit_wait)
static void R(const char *item, const char *verdict, const char *fmt, ...) {
    result_t *r = g_nres < NRES ? &g_results[g_nres++] : &g_results[NRES - 1];
    snprintf(r->item, sizeof r->item, "%s", item); snprintf(r->verdict, sizeof r->verdict, "%s", verdict);
    va_list ap; va_start(ap, fmt); vsnprintf(r->detail, sizeof r->detail, fmt, ap); va_end(ap);
    if (g_lost_seen || V.lost) { size_t l = strlen(r->detail); snprintf(r->detail + l, sizeof r->detail - l, " [AFTER VK_ERROR_DEVICE_LOST: void]"); }
    S("RESULT %-26s %-4s %s", r->item, r->verdict, r->detail);
}
static FILE *open_res(const char *name) {
    char p[700]; snprintf(p, sizeof p, "%s/%s", g_resdir, name);
    return fopen(p, "w");
}

// ---------------------------------------------------------------- small utilities
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC_RAW, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static long meminfo_kb(const char *key) {
    FILE *f = fopen("/proc/meminfo", "r"); if (!f) return -1;
    char line[256]; long v = -1; size_t kl = strlen(key);
    while (fgets(line, sizeof line, f)) if (!strncmp(line, key, kl) && line[kl] == ':') { v = atol(line + kl + 1); break; }
    fclose(f); return v;
}
static double avail_gb(void) { return meminfo_kb("MemAvailable") / 1048576.0; }
// cpuset and allowed CPUs of this thread (an app in the background gets /background, cores 0-1, and may be frozen)
static void sched_ctx(char *out, size_t n) {
    char cs[128] = "?", al[128] = "?", line[256];
    FILE *f = fopen("/proc/self/cpuset", "r"); if (f) { if (fgets(cs, sizeof cs, f)) cs[strcspn(cs, "\n")] = 0; fclose(f); }
    f = fopen("/proc/thread-self/status", "r");
    if (f) { while (fgets(line, sizeof line, f)) if (!strncmp(line, "Cpus_allowed_list:", 18)) { snprintf(al, sizeof al, "%s", line + 19); al[strcspn(al, "\n")] = 0; } fclose(f); }
    snprintf(out, n, "cpuset %s, allowed cpus %s, on cpu%d", cs, al, sched_getcpu());
}
static size_t g_pg;
static inline uint32_t h32(uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
static inline uint32_t pattern(uint32_t seed, uint32_t k) { return h32(k ^ h32(seed)); }
static inline uint32_t xform(uint32_t seed, uint32_t k) { return h32(k + seed * 0x9e3779b9u) | 1u; }
static uint32_t g_rng = 0x12345678u;
static uint32_t rnd(void) { g_rng = h32(g_rng + 0x6d2b79f5u); return g_rng; }

// /proc/self/maps lines overlapping [lo, hi), and whether they cover it without a gap
typedef struct { int covered, nlines, any_access, all_anon_none, any_kgsl; char lines[3000]; } range_info_t;
static void range_info(uintptr_t lo, uintptr_t hi, range_info_t *ri) {
    memset(ri, 0, sizeof *ri); ri->all_anon_none = 1;
    FILE *f = fopen("/proc/self/maps", "r"); if (!f) return;
    char line[600]; uintptr_t cur = lo; int gap = 0; size_t used = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long s, e; char perm[8]; int pos = 0;
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %n", &s, &e, perm, &pos) < 3) continue;
        if (e <= lo || s >= hi) continue;
        line[strcspn(line, "\n")] = 0;
        const char *path = pos ? line + pos : "";
        if (s > cur) gap = 1;
        if (e > cur) cur = e;
        ri->nlines++;
        if (perm[0] == 'r' || perm[1] == 'w' || perm[2] == 'x') ri->any_access = 1;
        if (*path || strcmp(perm, "---p")) ri->all_anon_none = 0;
        if (strstr(path, "kgsl")) ri->any_kgsl = 1;
        size_t l = strlen(line);
        if (used + l + 2 < sizeof ri->lines) { memcpy(ri->lines + used, line, l); used += l; ri->lines[used++] = '\n'; ri->lines[used] = 0; }
    }
    fclose(f);
    ri->covered = !gap && cur >= hi;
}
static void maps_snapshot(const char *name) {
    FILE *o = open_res(name), *f = fopen("/proc/self/maps", "r");
    if (o && f) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) fwrite(b, 1, n, o); }
    if (o) fclose(o);
    if (f) fclose(f);
}
static void S_lines(const char *pfx, const char *lines) {
    char buf[3000]; snprintf(buf, sizeof buf, "%s", lines);
    for (char *sp, *t = strtok_r(buf, "\n", &sp); t; t = strtok_r(NULL, "\n", &sp)) S("%s%s", pfx, t);
}

// ---------------------------------------------------------------- fault handling
// The dirty tracker's mechanism (dm/gfx/nvn/nvn_dirty.c): a write to a read-only tracked page faults, the handler records the address
// and makes that page writable again, and the write is re-executed. Any other fault on the probe thread while a guard is armed jumps
// back to the guard (recorded as unexpected); everything else goes to the previous handler so real crashes still produce tombstones.
static volatile uintptr_t g_trk_lo, g_trk_hi;
static volatile int g_trk_n, g_trk_mperr;
static volatile uintptr_t g_trk_addr[2048];
static volatile int g_trk_code[2048];
static volatile int g_guard_on, g_guard_tid, g_guard_sig, g_guard_code;
static volatile uintptr_t g_guard_addr;
static sigjmp_buf g_guard_jb;
static struct sigaction g_prev_segv, g_prev_bus;

// GUARD_BEGIN(ok) ... GUARD_END(ok) runs the block; ok = 1 if it finished, 0 if an unexpected fault jumped out (g_guard_sig / g_guard_addr say which)
#define GUARD_BEGIN(ok) do { ok = 0; g_guard_sig = 0; if (sigsetjmp(g_guard_jb, 1) == 0) { g_guard_tid = gettid(); g_guard_on = 1;
#define GUARD_END(ok) g_guard_on = 0; ok = 1; } else g_guard_on = 0; } while (0)

static void on_fault(int sig, siginfo_t *si, void *uc) {
    uintptr_t a = (uintptr_t)si->si_addr & 0x00ffffffffffffffull;
    if (sig == SIGSEGV && a >= g_trk_lo && a < g_trk_hi && si->si_code == SEGV_ACCERR) {
        int n = g_trk_n;
        if (n < 2048) { g_trk_addr[n] = a; g_trk_code[n] = si->si_code; }
        g_trk_n = n + 1;
        if (mprotect((void *)(a & ~(uintptr_t)(g_pg - 1)), g_pg, PROT_READ | PROT_WRITE) == 0) return;
        g_trk_mperr = errno;
    }
    if (g_guard_on && gettid() == g_guard_tid) {
        g_guard_on = 0; g_guard_sig = sig; g_guard_code = si->si_code; g_guard_addr = a;
        siglongjmp(g_guard_jb, 1);
    }
    struct sigaction *p = sig == SIGBUS ? &g_prev_bus : &g_prev_segv;
    if ((p->sa_flags & SA_SIGINFO) && p->sa_sigaction) { p->sa_sigaction(sig, si, uc); return; }
    if (p->sa_handler != SIG_DFL && p->sa_handler != SIG_IGN) { p->sa_handler(sig); return; }
    signal(sig, SIG_DFL);   // returning re-executes the access under the default action
}
static int install_fault_handler(void) {
    static int done; if (done) return 0;
    stack_t ss = { malloc(256 * 1024), 0, 256 * 1024 };
    if (sigaltstack(&ss, NULL)) S("sigaltstack failed: errno %d", errno);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &g_prev_segv) || sigaction(SIGBUS, &sa, &g_prev_bus)) { S("sigaction failed: errno %d", errno); return -1; }
    S("fault handler installed (SA_SIGINFO|SA_ONSTACK, alt stack 256 KB); previous SIGSEGV handler %p flags %#x", (void *)g_prev_segv.sa_sigaction, g_prev_segv.sa_flags);
    done = 1; return 0;
}
// Self-test on anonymous memory, before trusting the handler with driver pages: the guard catches a fault three times in a row
// (siglongjmp out of the handler must leave the signal chain usable, which matters behind ART's libsigchain in the app),
// and the tracker path (RO page, write, one fault, unprotect, retry) works twice.
static void fault_selftest(void) {
    uint8_t *pn = mmap(NULL, 2 * g_pg, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int caught = 0;
    for (int i = 0; i < 3; i++) { int ok; GUARD_BEGIN(ok) (void)*(volatile uint8_t *)(pn + 8 * i); GUARD_END(ok); if (!ok && g_guard_addr == (uintptr_t)(pn + 8 * i) && g_guard_code == SEGV_ACCERR) caught++; }
    mprotect(pn, 2 * g_pg, PROT_READ | PROT_WRITE); memset(pn, 0x11, 2 * g_pg);
    int trk_ok = 0;
    for (int i = 0; i < 2; i++) {
        mprotect(pn, 2 * g_pg, PROT_READ);
        g_trk_n = 0; g_trk_lo = (uintptr_t)pn; g_trk_hi = (uintptr_t)pn + 2 * g_pg;
        int ok; GUARD_BEGIN(ok) *(volatile uint32_t *)(pn + g_pg + 4 * i) = 0x5e1f0000u + i; GUARD_END(ok);
        if (ok && g_trk_n == 1 && g_trk_addr[0] == (uintptr_t)(pn + g_pg + 4 * i) && *(volatile uint32_t *)(pn + g_pg + 4 * i) == 0x5e1f0000u + i && pn[0] == 0x11) trk_ok++;
    }
    g_trk_lo = g_trk_hi = 0;
    munmap(pn, 2 * g_pg);
    R("fault.selftest_anon", caught == 3 && trk_ok == 2 ? "PASS" : "FAIL", "guard caught %d/3 faults in a row; tracker write-fault cycle on anonymous memory %d/2", caught, trk_ok);
}

// ---------------------------------------------------------------- Vulkan state (V is declared with the output helpers)
static uint32_t g_type = UINT32_MAX; static VkMemoryPropertyFlags g_type_flags; static int g_type_rank;
static int g_placed_ok, g_unmap_reserve, g_range_placed, g_bda;
static VkDeviceSize g_min_align = 4096;
static VkDeviceSize g_max_alloc, g_max_buffer;
static uint8_t *g_res5, *g_rb; static size_t g_res5_len;   // the 5 GB reservation and its 2 MB-aligned base

static int test_on(const char *t);
static void ahb_add_exts(const char **exts, uint32_t *ne);   // ahb_probe.c
static void ahb_after_device(void);
static uint32_t eff_api(void) { return V.props.apiVersion < V.inst_api ? V.props.apiVersion : V.inst_api; }
static int enabler_ok(const char *en) {
    char buf[512], *sp; snprintf(buf, sizeof buf, "%s", en);
    for (char *t = strtok_r(buf, "|", &sp); t; t = strtok_r(NULL, "|", &sp)) {
        if (!strncmp(t, "VK_VERSION_", 11)) { if (eff_api() >= VK_MAKE_API_VERSION(0, t[11] - '0', t[13] - '0', 0)) return 1; }
        else if (vkc_has_ext(&V, t)) return 1;
    }
    return 0;
}
static void q_feat(void *s) { VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, s, {0} }; V.vkGetPhysicalDeviceFeatures2(V.pd, &f2); }
static void q_prop(void *s) { VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, s, {0} }; V.vkGetPhysicalDeviceProperties2(V.pd, &p2); }

static VkResult map_placed(VkDeviceMemory mem, void *addr, void **out) {
    VkMemoryMapPlacedInfoEXT pi = { VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, NULL, addr };
    VkMemoryMapInfo mi = { VK_STRUCTURE_TYPE_MEMORY_MAP_INFO, &pi, VK_MEMORY_MAP_PLACED_BIT_EXT, mem, 0, VK_WHOLE_SIZE };
    *out = NULL;
    return V.MapMemory2(V.dev, &mi, out);
}
static VkResult unmap2(VkDeviceMemory mem, int reserve) {
    VkMemoryUnmapInfo ui = { VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO, NULL, reserve ? VK_MEMORY_UNMAP_RESERVE_BIT_EXT : 0, mem };
    return V.UnmapMemory2(V.dev, &ui);
}
// Unmap a placed mapping (keeping the range reserved when the driver can), free the memory, and make sure the range is PROT_NONE again
static void placed_release(VkDeviceMemory mem, void *addr, size_t len) {
    if (g_unmap_reserve) unmap2(mem, 1); else V.vkUnmapMemory(V.dev, mem);
    V.vkFreeMemory(V.dev, mem, NULL);
    if (mmap(addr, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) S("re-reserve %p +%#zx failed: errno %d", addr, len, errno);
}

// ---------------------------------------------------------------- capabilities
static const struct { VkFormat f; const char *n; } k_formats[] = {
    { VK_FORMAT_BC1_RGB_UNORM_BLOCK, "BC1_RGB_UNORM" }, { VK_FORMAT_BC1_RGB_SRGB_BLOCK, "BC1_RGB_SRGB" },
    { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1_RGBA_UNORM" }, { VK_FORMAT_BC1_RGBA_SRGB_BLOCK, "BC1_RGBA_SRGB" },
    { VK_FORMAT_BC2_UNORM_BLOCK, "BC2_UNORM" }, { VK_FORMAT_BC2_SRGB_BLOCK, "BC2_SRGB" },
    { VK_FORMAT_BC3_UNORM_BLOCK, "BC3_UNORM" }, { VK_FORMAT_BC3_SRGB_BLOCK, "BC3_SRGB" },
    { VK_FORMAT_BC4_UNORM_BLOCK, "BC4_UNORM" }, { VK_FORMAT_BC4_SNORM_BLOCK, "BC4_SNORM" },
    { VK_FORMAT_BC5_UNORM_BLOCK, "BC5_UNORM" }, { VK_FORMAT_BC5_SNORM_BLOCK, "BC5_SNORM" },
    { VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H_UFLOAT" }, { VK_FORMAT_BC6H_SFLOAT_BLOCK, "BC6H_SFLOAT" },
    { VK_FORMAT_BC7_UNORM_BLOCK, "BC7_UNORM" }, { VK_FORMAT_BC7_SRGB_BLOCK, "BC7_SRGB" },
    { VK_FORMAT_ASTC_4x4_UNORM_BLOCK, "ASTC_4x4_UNORM" }, { VK_FORMAT_ASTC_4x4_SRGB_BLOCK, "ASTC_4x4_SRGB" },
    { VK_FORMAT_ASTC_8x8_UNORM_BLOCK, "ASTC_8x8_UNORM" }, { VK_FORMAT_ASTC_12x12_SRGB_BLOCK, "ASTC_12x12_SRGB" },
    { VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, "ETC2_RGBA8_UNORM" }, { VK_FORMAT_R8_UINT, "R8_UINT(ref)" },
    { VK_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT" }, { VK_FORMAT_D32_SFLOAT_S8_UINT, "D32_SFLOAT_S8_UINT" },
    { VK_FORMAT_A8B8G8R8_UNORM_PACK32, "A8B8G8R8_UNORM" }, { VK_FORMAT_B5G6R5_UNORM_PACK16, "B5G6R5_UNORM" },
};

static void dump_caps(void) {
    char t[256];
    S("device: %s  vendor %#x device %#x type %d", V.props.deviceName, V.props.vendorID, V.props.deviceID, V.props.deviceType);
    S("apiVersion %u.%u.%u (raw %#x)  driverVersion raw %#x (as VK_VERSION %u.%u.%u)  instance version %u.%u.%u, instance api used %u.%u",
      VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion), VK_API_VERSION_PATCH(V.props.apiVersion), V.props.apiVersion,
      V.props.driverVersion, VK_API_VERSION_MAJOR(V.props.driverVersion), VK_API_VERSION_MINOR(V.props.driverVersion), VK_API_VERSION_PATCH(V.props.driverVersion),
      VK_API_VERSION_MAJOR(V.inst_version), VK_API_VERSION_MINOR(V.inst_version), VK_API_VERSION_PATCH(V.inst_version),
      VK_API_VERSION_MAJOR(V.inst_api), VK_API_VERSION_MINOR(V.inst_api));
    S("driverID %d  driverName \"%s\"  driverInfo \"%s\"  conformance %u.%u.%u.%u", V.drv.driverID, V.drv.driverName, V.drv.driverInfo,
      V.drv.conformanceVersion.major, V.drv.conformanceVersion.minor, V.drv.conformanceVersion.subminor, V.drv.conformanceVersion.patch);

    FILE *fe = open_res("extensions.txt");
    if (fe) {
        fprintf(fe, "# device extensions (%u)\n", V.next);
        for (uint32_t i = 0; i < V.next; i++) fprintf(fe, "%s %u\n", V.ext[i].extensionName, V.ext[i].specVersion);
        uint32_t ni = 0; V.vkEnumerateInstanceExtensionProperties(NULL, &ni, NULL);
        VkExtensionProperties *ie = calloc(ni ? ni : 1, sizeof *ie); V.vkEnumerateInstanceExtensionProperties(NULL, &ni, ie);
        fprintf(fe, "# instance extensions (%u)\n", ni);
        for (uint32_t i = 0; i < ni; i++) fprintf(fe, "%s %u\n", ie[i].extensionName, ie[i].specVersion);
        free(ie); fclose(fe);
    }
    S("device extensions: %u (extensions.txt)", V.next);
    static const char *k_ext[] = { "VK_KHR_map_memory2", "VK_EXT_map_memory_placed", "VK_EXT_external_memory_host", "VK_KHR_pipeline_binary",
        "VK_EXT_graphics_pipeline_library", "VK_KHR_push_descriptor", "VK_EXT_descriptor_buffer", "VK_EXT_index_type_uint8", "VK_KHR_index_type_uint8",
        "VK_KHR_buffer_device_address", "VK_EXT_memory_budget", "VK_KHR_maintenance5", "VK_KHR_maintenance6", "VK_EXT_custom_border_color",
        "VK_EXT_transform_feedback", "VK_EXT_depth_clip_control", "VK_EXT_conditional_rendering", "VK_KHR_draw_indirect_count",
        "VK_EXT_descriptor_indexing", "VK_EXT_extended_dynamic_state3", "VK_EXT_shader_object", "VK_KHR_dynamic_rendering", "VK_EXT_external_memory_dma_buf",
        "VK_ANDROID_external_memory_android_hardware_buffer", "VK_KHR_external_memory_fd", "VK_EXT_depth_bias_control", "VK_EXT_depth_clamp_zero_one",
        "VK_EXT_primitive_topology_list_restart", "VK_EXT_vertex_input_dynamic_state", "VK_EXT_robustness2", "VK_EXT_image_view_min_lod" };
    for (size_t i = 0; i < sizeof k_ext / sizeof *k_ext; i++) S("ext %-50s %s", k_ext[i], vkc_has_ext(&V, k_ext[i]) ? "present" : "absent");

    // full feature and property dumps
    FILE *ff = open_res("features.txt"), *fp = open_res("properties.txt");
    VkPhysicalDeviceFeatures cf; V.vkGetPhysicalDeviceFeatures(V.pd, &cf);
    if (ff) vkdump_VkPhysicalDeviceFeatures(ff, "VkPhysicalDeviceFeatures.", &cf);
    if (fp) {
        fprintf(fp, "deviceName=%s\nvendorID=%#x\ndeviceID=%#x\ndeviceType=%d\napiVersion=%#x\ndriverVersion=%#x\n", V.props.deviceName, V.props.vendorID,
                V.props.deviceID, V.props.deviceType, V.props.apiVersion, V.props.driverVersion);
        fprintf(fp, "pipelineCacheUUID="); for (int i = 0; i < VK_UUID_SIZE; i++) fprintf(fp, "%02x", V.props.pipelineCacheUUID[i]); fputc('\n', fp);
        vkdump_VkPhysicalDeviceLimits(fp, "limits.", &V.props.limits);
        vkdump_VkPhysicalDeviceSparseProperties(fp, "sparse.", &V.props.sparseProperties);
    }
    int nq = 0, nskip = 0;
    if (V.vkGetPhysicalDeviceFeatures2 && V.vkGetPhysicalDeviceProperties2) {
        for (int i = 0; i < VKDUMP_NSTRUCTS; i++) {
            const vkdump_struct_t *d = &vkdump_structs[i];
            FILE *o = d->is_props ? fp : ff; if (!o) continue;
            if (!enabler_ok(d->enablers)) { nskip++; continue; }
            void *buf = calloc(1, d->size + 64);
            *(VkStructureType *)buf = d->stype;
            if (d->is_props) q_prop(buf); else q_feat(buf);
            char pfx[160]; snprintf(pfx, sizeof pfx, "%s.", d->name);
            d->dump(o, pfx, buf); free(buf); nq++;
        }
    }
    if (ff) fclose(ff);
    if (fp) fclose(fp);
    S("feature/property structs dumped: %d (skipped %d not exposed by the device's version / extensions)", nq, nskip);

    S("core features: geometryShader=%u depthBounds=%u textureCompressionBC=%u shaderFloat64=%u sparseBinding=%u sparseResidencyBuffer=%u multiDrawIndirect=%u drawIndirectFirstInstance=%u",
      cf.geometryShader, cf.depthBounds, cf.textureCompressionBC, cf.shaderFloat64, cf.sparseBinding, cf.sparseResidencyBuffer, cf.multiDrawIndirect, cf.drawIndirectFirstInstance);
    S("core features: shaderInt64=%u shaderInt16=%u logicOp=%u dualSrcBlend=%u fillModeNonSolid=%u wideLines=%u depthClamp=%u textureCompressionASTC_LDR=%u tessellationShader=%u robustBufferAccess=%u",
      cf.shaderInt64, cf.shaderInt16, cf.logicOp, cf.dualSrcBlend, cf.fillModeNonSolid, cf.wideLines, cf.depthClamp, cf.textureCompressionASTC_LDR, cf.tessellationShader, cf.robustBufferAccess);

    if (V.vkGetPhysicalDeviceFeatures2) {
        VkPhysicalDeviceIndexTypeUint8Features u8 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES };
        if (enabler_ok("VK_VERSION_1_4|VK_KHR_index_type_uint8|VK_EXT_index_type_uint8")) { q_feat(&u8); S("indexTypeUint8=%u", u8.indexTypeUint8); }
        else S("indexTypeUint8: not exposed (no VK_KHR/EXT_index_type_uint8, api < 1.4)");
        if (eff_api() >= VK_API_VERSION_1_4) { VkPhysicalDeviceVulkan14Features f14 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES }; q_feat(&f14); S("Vulkan14Features.pushDescriptor=%u", f14.pushDescriptor); }
        if (vkc_has_ext(&V, "VK_KHR_push_descriptor") || eff_api() >= VK_API_VERSION_1_4) { VkPhysicalDevicePushDescriptorProperties pd = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES }; q_prop(&pd); S("maxPushDescriptors=%u", pd.maxPushDescriptors); }
        if (vkc_has_ext(&V, "VK_EXT_descriptor_buffer")) { VkPhysicalDeviceDescriptorBufferFeaturesEXT db = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT }; q_feat(&db); S("descriptorBuffer=%u descriptorBufferPushDescriptors=%u", db.descriptorBuffer, db.descriptorBufferPushDescriptors); }
        if (vkc_has_ext(&V, "VK_EXT_graphics_pipeline_library")) { VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT }; q_feat(&gpl); S("graphicsPipelineLibrary=%u", gpl.graphicsPipelineLibrary); }
        if (vkc_has_ext(&V, "VK_KHR_pipeline_binary")) { VkPhysicalDevicePipelineBinaryFeaturesKHR pb = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR }; q_feat(&pb); S("pipelineBinaries=%u", pb.pipelineBinaries); }
        VkPhysicalDeviceSubgroupProperties sg = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES }; q_prop(&sg);
        S("subgroup: size %u supportedStages %#x (vertex %u fragment %u compute %u geometry %u) supportedOperations %#x quadOperationsInAllStages %u",
          sg.subgroupSize, sg.supportedStages, !!(sg.supportedStages & VK_SHADER_STAGE_VERTEX_BIT), !!(sg.supportedStages & VK_SHADER_STAGE_FRAGMENT_BIT),
          !!(sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT), !!(sg.supportedStages & VK_SHADER_STAGE_GEOMETRY_BIT), sg.supportedOperations, sg.quadOperationsInAllStages);
        VkPhysicalDeviceMaintenance3Properties m3 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES }; q_prop(&m3);
        g_max_alloc = m3.maxMemoryAllocationSize;
        if (enabler_ok("VK_VERSION_1_3|VK_KHR_maintenance4")) { VkPhysicalDeviceMaintenance4Properties m4 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES }; q_prop(&m4); g_max_buffer = m4.maxBufferSize; }
        S("maxMemoryAllocationSize %llu (%#llx, %.3f GB)  maxBufferSize %llu (%#llx)", (unsigned long long)g_max_alloc, (unsigned long long)g_max_alloc, g_max_alloc / (double)GB,
          (unsigned long long)g_max_buffer, (unsigned long long)g_max_buffer);
    }

    // memory heaps and types
    FILE *fm = open_res("memory.txt");
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bud = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
    int have_bud = vkc_has_ext(&V, "VK_EXT_memory_budget") && V.vkGetPhysicalDeviceMemoryProperties2;
    if (have_bud) { VkPhysicalDeviceMemoryProperties2 mp2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &bud, {0} }; V.vkGetPhysicalDeviceMemoryProperties2(V.pd, &mp2); }
    for (uint32_t i = 0; i < V.mem.memoryHeapCount; i++) {
        char l[256]; snprintf(l, sizeof l, "heap %u: size %llu (%.3f GB) flags %#x%s%s", i, (unsigned long long)V.mem.memoryHeaps[i].size, V.mem.memoryHeaps[i].size / (double)GB,
                              V.mem.memoryHeaps[i].flags, V.mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " DEVICE_LOCAL" : "", V.mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_MULTI_INSTANCE_BIT ? " MULTI_INSTANCE" : "");
        S("%s", l); if (fm) fprintf(fm, "%s\n", l);
        if (have_bud) { S("heap %u budget %.3f GB usage %.3f GB", i, bud.heapBudget[i] / (double)GB, bud.heapUsage[i] / (double)GB); if (fm) fprintf(fm, "heap %u budget %llu usage %llu\n", i, (unsigned long long)bud.heapBudget[i], (unsigned long long)bud.heapUsage[i]); }
    }
    for (uint32_t i = 0; i < V.mem.memoryTypeCount; i++) {
        vkc_memflags_str(V.mem.memoryTypes[i].propertyFlags, t, sizeof t);
        S("type %u: heap %u flags %#x %s", i, V.mem.memoryTypes[i].heapIndex, V.mem.memoryTypes[i].propertyFlags, t);
        if (fm) fprintf(fm, "type %u: heap %u flags %#x %s\n", i, V.mem.memoryTypes[i].heapIndex, V.mem.memoryTypes[i].propertyFlags, t);
    }
    if (fm) fclose(fm);

    // formats
    FILE *fo = open_res("formats.txt");
    char bcline[1024] = ""; int bc_all = 1;
    for (size_t i = 0; i < sizeof k_formats / sizeof *k_formats; i++) {
        VkFormatProperties fpr; V.vkGetPhysicalDeviceFormatProperties(V.pd, k_formats[i].f, &fpr);
        VkImageFormatProperties ifp; memset(&ifp, 0, sizeof ifp);
        VkResult r = V.vkGetPhysicalDeviceImageFormatProperties(V.pd, k_formats[i].f, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &ifp);
        int sampled = !!(fpr.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        int linear = !!(fpr.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        if (fo) fprintf(fo, "%-20s optimal %#010x linear %#010x buffer %#010x sampled=%d filter_linear=%d imageFormatProperties(2D,optimal,sampled|transfer_dst)=%s maxExtent %ux%u mips %u layers %u\n",
                        k_formats[i].n, fpr.optimalTilingFeatures, fpr.linearTilingFeatures, fpr.bufferFeatures, sampled, linear, vkc_result_str(r),
                        ifp.maxExtent.width, ifp.maxExtent.height, ifp.maxMipLevels, ifp.maxArrayLayers);
        if (!strncmp(k_formats[i].n, "BC", 2)) {
            int ok = sampled && r == VK_SUCCESS;
            if (!ok) bc_all = 0;
            size_t l = strlen(bcline); snprintf(bcline + l, sizeof bcline - l, "%s%s=%s", l ? " " : "", k_formats[i].n, ok ? "yes" : (sampled ? "feat-only" : "no"));
        }
    }
    if (fo) fclose(fo);
    S("BC sampled (optimal tiling): %s", bcline);
    R("caps.bc1-7", bc_all ? "PASS" : "FAIL", "%s", bc_all ? "all 16 BC1-BC7 UNORM/SRGB/SNORM/FLOAT formats sample with optimal tiling" : bcline);
    R("caps.geometryShader", cf.geometryShader ? "PASS" : "FAIL", "geometryShader=%u", cf.geometryShader);
}

// ---------------------------------------------------------------- device creation with the features the tests use
static int make_device(char *err, size_t errn) {
    const char *exts[8]; uint32_t ne = 0;
    int has_m2 = vkc_has_ext(&V, "VK_KHR_map_memory2"), has_pl = vkc_has_ext(&V, "VK_EXT_map_memory_placed");
    if (has_m2) exts[ne++] = "VK_KHR_map_memory2";
    if (has_pl) exts[ne++] = "VK_EXT_map_memory_placed";
    ahb_add_exts(exts, &ne);
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT sup_pl = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT };
    VkPhysicalDeviceMapMemoryPlacedPropertiesEXT pl_props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT };
    if (has_pl) { q_feat(&sup_pl); q_prop(&pl_props); g_min_align = pl_props.minPlacedMemoryMapAlignment; }
    S("VK_KHR_map_memory2 %s, VK_EXT_map_memory_placed %s%s", has_m2 ? "present" : "absent", has_pl ? "present" : "absent",
      (!has_m2 && has_pl && eff_api() < VK_API_VERSION_1_4) ? " (placed without map_memory2 below 1.4: invalid driver exposure)" : "");
    if (has_pl) S("placed features: memoryMapPlaced=%u memoryMapRangePlaced=%u memoryUnmapReserve=%u  minPlacedMemoryMapAlignment=%llu (%#llx)",
                  sup_pl.memoryMapPlaced, sup_pl.memoryMapRangePlaced, sup_pl.memoryUnmapReserve, (unsigned long long)pl_props.minPlacedMemoryMapAlignment,
                  (unsigned long long)pl_props.minPlacedMemoryMapAlignment);
    R("caps.map_memory_placed", has_pl && sup_pl.memoryMapPlaced ? "PASS" : "FAIL", "ext %s, memoryMapPlaced=%u memoryMapRangePlaced=%u memoryUnmapReserve=%u minAlign=%#llx",
      has_pl ? "present" : "absent", sup_pl.memoryMapPlaced, sup_pl.memoryMapRangePlaced, sup_pl.memoryUnmapReserve, (unsigned long long)g_min_align);

    int bda_sup = 0;
    VkPhysicalDeviceVulkan12Features en12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceBufferDeviceAddressFeatures enbda = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT enpl = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, NULL, {0} };
    void **tail = &f2.pNext;
    if (eff_api() >= VK_API_VERSION_1_2) {
        VkPhysicalDeviceVulkan12Features s12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES }; q_feat(&s12);
        bda_sup = s12.bufferDeviceAddress; en12.bufferDeviceAddress = s12.bufferDeviceAddress;
        *tail = &en12; tail = &en12.pNext;
    } else if (vkc_has_ext(&V, "VK_KHR_buffer_device_address")) {
        VkPhysicalDeviceBufferDeviceAddressFeatures sb = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES }; q_feat(&sb);
        bda_sup = sb.bufferDeviceAddress; enbda.bufferDeviceAddress = sb.bufferDeviceAddress;
        exts[ne++] = "VK_KHR_buffer_device_address"; *tail = &enbda; tail = &enbda.pNext;
    }
    if (has_pl) { enpl.memoryMapPlaced = sup_pl.memoryMapPlaced; enpl.memoryMapRangePlaced = sup_pl.memoryMapRangePlaced; enpl.memoryUnmapReserve = sup_pl.memoryUnmapReserve; *tail = &enpl; tail = &enpl.pNext; }
    char el[512] = ""; for (uint32_t i = 0; i < ne; i++) { size_t l = strlen(el); snprintf(el + l, sizeof el - l, "%s%s", i ? " " : "", exts[i]); }
    S("creating device: extensions [%s], bufferDeviceAddress=%u, placed features %u/%u/%u", el, bda_sup, enpl.memoryMapPlaced, enpl.memoryMapRangePlaced, enpl.memoryUnmapReserve);
    if (vkc_device(&V, exts, ne, &f2, err, errn)) return -1;
    g_placed_ok = has_pl && sup_pl.memoryMapPlaced && V.MapMemory2 && V.UnmapMemory2;
    g_unmap_reserve = g_placed_ok && sup_pl.memoryUnmapReserve;
    g_range_placed = g_placed_ok && sup_pl.memoryMapRangePlaced;
    g_bda = bda_sup && V.GetBufferDeviceAddress;
    ahb_after_device();
    S("device created; vkMapMemory2 %p vkUnmapMemory2 %p vkGetBufferDeviceAddress %p", (void *)V.MapMemory2, (void *)V.UnmapMemory2, (void *)V.GetBufferDeviceAddress);
    return 0;
}

// ---------------------------------------------------------------- memory type
static const VkBufferUsageFlags k_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
static void choose_type(void) {
    VkBuffer b; uint32_t bits = 0;
    VkBufferUsageFlags u = k_usage | (g_bda ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
    if (vkc_buffer(&V, VK_NULL_HANDLE, 0, 64 * MB, u, &b, &bits) == VK_SUCCESS) V.vkDestroyBuffer(V.dev, b, NULL);
    const VkMemoryPropertyFlags HV = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, HC = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, CA = VK_MEMORY_PROPERTY_HOST_CACHED_BIT, DL = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const VkMemoryPropertyFlags avoid = VK_MEMORY_PROPERTY_PROTECTED_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
    const VkMemoryPropertyFlags want[] = { DL | HV | HC | CA, HV | HC | CA, HV | CA, HV | HC, HV };
    const char *names[] = { "DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT|HOST_CACHED", "HOST_VISIBLE|HOST_COHERENT|HOST_CACHED", "HOST_VISIBLE|HOST_CACHED (not coherent)",
                            "HOST_VISIBLE|HOST_COHERENT (uncached)", "HOST_VISIBLE" };
    for (int i = 0; i < 5; i++) {
        int t = vkc_memtype(&V, bits, want[i], avoid);
        if (t >= 0) { g_type = (uint32_t)t; g_type_flags = V.mem.memoryTypes[t].propertyFlags; g_type_rank = i; break; }
    }
    char fl[256]; vkc_memflags_str(g_type_flags, fl, sizeof fl);
    S("storage-buffer memoryTypeBits %#x; chosen type %d (%s), wanted rank %d = %s", bits, (int)g_type, fl, g_type_rank, names[g_type_rank]);
    R("mem.type_cached_coherent", g_type_rank <= 1 ? "PASS" : "FAIL", "type %d flags %s%s", (int)g_type, fl, g_type_rank == 0 ? "" : (g_type_rank == 1 ? " (not DEVICE_LOCAL)" : ""));
}

// ---------------------------------------------------------------- page touching (guarded)
static volatile long g_touch_bad;
// Writes a word per page, value (tag + offset) ^ seed, then reads every page back. tag is normally the address the data is written at;
// passing an older address checks data written through an earlier mapping. Returns bad pages, or -1 on a fault.
static long touch_pages(uint8_t *p, size_t len, uintptr_t tag, uint64_t seed, int write) {
    int ok; g_touch_bad = 0;
    GUARD_BEGIN(ok)
        if (write) for (size_t o = 0; o < len; o += g_pg) *(volatile uint64_t *)(p + o) = (uint64_t)(tag + o) ^ seed;
        for (size_t o = 0; o < len; o += g_pg) if (*(volatile uint64_t *)(p + o) != ((uint64_t)(tag + o) ^ seed)) g_touch_bad++;
    GUARD_END(ok);
    if (!ok) { S("  touch %p +%#zx: fault sig %d code %d at %#lx", p, len, g_guard_sig, g_guard_code, (unsigned long)g_guard_addr); return -1; }
    return g_touch_bad;
}
// GPU-side check: vkCmdCopyBuffer copies the first 8 bytes of every page of mem [0, len) into a separate, normally mapped staging buffer,
// and the CPU compares them with (tag + offset) ^ seed. Proves that what the CPU wrote through a mapping is the GPU's memory. Returns bad pages, < 0 on error.
static long gpu_check_pages(VkDeviceMemory mem, size_t len, uintptr_t tag, uint64_t seed) {
    size_t np = len / g_pg, sl = (np * 8 + 4095) & ~(size_t)4095; long bad = -2;
    VkBuffer src = 0, dst = 0; VkDeviceMemory sm = 0; void *sp = NULL; VkBufferCopy *rg = NULL;
    if (vkc_buffer(&V, mem, 0, len, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &src, NULL)) goto out;
    if (vkc_alloc(&V, sl, g_type, 0, &sm) || V.vkMapMemory(V.dev, sm, 0, VK_WHOLE_SIZE, 0, &sp)) goto out;
    if (vkc_buffer(&V, sm, 0, sl, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &dst, NULL)) goto out;
    memset(sp, 0xEE, sl);
    rg = malloc(np * sizeof *rg); if (!rg) goto out;
    for (size_t i = 0; i < np; i++) rg[i] = (VkBufferCopy){ i * g_pg, i * 8, 8 };
    if (vkc_cmd_begin(&V)) goto out;
    V.vkCmdCopyBuffer(V.cmd, src, dst, (uint32_t)np, rg);
    VkMemoryBarrier hb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
    V.vkCmdPipelineBarrier(V.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    if (vkc_cmd_submit_wait(&V, 10000000000ull)) { bad = -3; goto out; }
    bad = 0;
    for (size_t i = 0; i < np; i++) if (((volatile uint64_t *)sp)[i] != ((uint64_t)(tag + i * g_pg) ^ seed)) bad++;
out:
    free(rg);
    if (dst) V.vkDestroyBuffer(V.dev, dst, NULL);
    if (src) V.vkDestroyBuffer(V.dev, src, NULL);
    if (sp) V.vkUnmapMemory(V.dev, sm);
    if (sm) V.vkFreeMemory(V.dev, sm, NULL);
    return bad;
}
// GPU copy of mem [off, off + len) into out
static int gpu_copy_out(VkDeviceMemory mem, size_t memlen, size_t off, size_t len, void *out) {
    int rc = -1; VkBuffer src = 0, dst = 0; VkDeviceMemory sm = 0; void *sp = NULL;
    if (vkc_buffer(&V, mem, 0, memlen, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &src, NULL)) goto out;
    if (vkc_alloc(&V, len, g_type, 0, &sm) || V.vkMapMemory(V.dev, sm, 0, VK_WHOLE_SIZE, 0, &sp)) goto out;
    if (vkc_buffer(&V, sm, 0, len, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &dst, NULL)) goto out;
    if (vkc_cmd_begin(&V)) goto out;
    VkBufferCopy rg = { off, 0, len };
    V.vkCmdCopyBuffer(V.cmd, src, dst, 1, &rg);
    VkMemoryBarrier hb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
    V.vkCmdPipelineBarrier(V.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    if (vkc_cmd_submit_wait(&V, 10000000000ull)) goto out;
    memcpy(out, sp, len); rc = 0;
out:
    if (dst) V.vkDestroyBuffer(V.dev, dst, NULL);
    if (src) V.vkDestroyBuffer(V.dev, src, NULL);
    if (sp) V.vkUnmapMemory(V.dev, sm);
    if (sm) V.vkFreeMemory(V.dev, sm, NULL);
    return rc;
}
// After a placed mapping was unmapped: does a plain vkMapMemory of the same memory give a usable mapping with the data? (not part of the
// P0.7 pass test; the port must know whether placed and normal mappings of one VkDeviceMemory can alternate)
static void remap_normal_check(const char *item, VkDeviceMemory mem, uintptr_t placed_at, size_t len, uintptr_t tag, uint64_t seed) {
    void *q = NULL; VkResult r = V.vkMapMemory(V.dev, mem, 0, VK_WHOLE_SIZE, 0, &q);
    if (r) { R(item, "WARN", "vkMapMemory after the placed unmap: %s", vkc_result_str(r)); return; }
    range_info_t ri; range_info((uintptr_t)q, (uintptr_t)q + g_pg, &ri);
    long bad = touch_pages(q, len, tag, seed, 0);
    S("  plain vkMapMemory after the placed unmap returned %p (%s the old placed address); maps there:", q, (uintptr_t)q == placed_at ? "SAME AS" : "not"); S_lines("    ", ri.lines);
    V.vkUnmapMemory(V.dev, mem);
    if (bad == 0) R(item, "PASS", "returned %p (%s the placed address), data written through the placed mapping visible", q, (uintptr_t)q == placed_at ? "same as" : "not");
    else R(item, "WARN", "DRIVER BUG: returned %p (%s the placed address) and %s", q, (uintptr_t)q == placed_at ? "same as" : "not",
           bad < 0 ? "reading it faults (nothing mapped there by the driver)" : "the data differs");
    if ((uintptr_t)q == placed_at && g_unmap_reserve) mmap(q, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);   // keep the reservation whole
}

// ---------------------------------------------------------------- (a) placed map into the reservation
static void test_a(void) {
    S("== (a) placed mapping inside a 5 GB PROT_NONE reservation");
    if (!g_placed_ok) { R("a.placed_map", "SKIP", "VK_EXT_map_memory_placed / memoryMapPlaced not available"); return; }
    VkDeviceMemory mem; size_t len = 256 * MB;
    VkResult r = vkc_alloc(&V, len, g_type, 0, &mem);
    if (r) { R("a.placed_map", "FAIL", "vkAllocateMemory(256 MB, type %u): %s", g_type, vkc_result_str(r)); return; }
    uint8_t *want = g_rb + 64 * MB; void *got;
    range_info_t r0; range_info((uintptr_t)want, (uintptr_t)want + len, &r0);
    double t0 = now_s(); r = map_placed(mem, want, &got); double tm = now_s() - t0;
    range_info_t ri; range_info((uintptr_t)want, (uintptr_t)want + len, &ri);
    S("  before: range %s by the reservation (%s)", r0.covered ? "covered" : "NOT covered", r0.all_anon_none ? "anonymous ---p" : "other");
    S("  vkMapMemory2(PLACED at %p) = %s, returned %p (%.3f ms); maps over the range:", want, vkc_result_str(r), got, tm * 1e3); S_lines("    ", ri.lines);
    if (r || got != want) { R("a.placed_map", "FAIL", "vkMapMemory2 placed: %s, returned %p for %p", vkc_result_str(r), got, want); V.vkFreeMemory(V.dev, mem, NULL); return; }
    uint64_t seed = ((uint64_t)rnd() << 32) | rnd();
    long bad = touch_pages(want, len, (uintptr_t)want, seed, 1);
    int ok; volatile uint64_t s = 0;
    GUARD_BEGIN(ok) memset(want + 4096 + 8, 0xA5, 4096 - 8); for (size_t i = 8; i < 4096; i++) s += want[4096 + i]; GUARD_END(ok);
    int fill_ok = ok && s == 0xA5ull * (4096 - 8);
    long gbad = gpu_check_pages(mem, len, (uintptr_t)want, seed);
    S("  CPU write+read of every page: %ld bad (-1 = fault); memset/sum of a page %s; GPU copy of every page's word: %ld bad (< 0 = error)", bad, fill_ok ? "ok" : "BAD", gbad);
    R("a.placed_map", (bad == 0 && fill_ok && gbad == 0 && ri.covered && ri.nlines == 1) ? "PASS" : "FAIL",
      "256 MB at %p (2 MB aligned): ptr==requested, one VMA %s, CPU rw %s, GPU sees the CPU's writes %s", want,
      ri.any_kgsl ? "of /kgsl-3d0" : "(not kgsl)", bad == 0 && fill_ok ? "ok" : "BAD", gbad == 0 ? "yes" : "NO");
    // (a2) the same memory placed again at an address aligned only to minPlacedMemoryMapAlignment: the old data must be there
    if (g_unmap_reserve) unmap2(mem, 1); else { V.vkUnmapMemory(V.dev, mem); mmap(want, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0); }
    uint8_t *want2 = g_rb + 512 * MB + g_min_align;
    r = map_placed(mem, want2, &got);
    long old2 = -3, bad2 = -3;
    if (!r && got == want2) { old2 = touch_pages(want2, len, (uintptr_t)want, seed, 0); bad2 = touch_pages(want2, len, (uintptr_t)want2, seed ^ 1, 1); }
    long gbad2 = (!r && got == want2) ? gpu_check_pages(mem, len, (uintptr_t)want2, seed ^ 1) : -3;
    S("  (a2) placed at %p (512 MB + minAlign %#llx): %s, returned %p; data from the first placement %ld bad; rw %ld bad; GPU check %ld bad",
      want2, (unsigned long long)g_min_align, vkc_result_str(r), got, old2, bad2, gbad2);
    R("a.placed_min_align", (!r && got == want2 && old2 == 0 && bad2 == 0 && gbad2 == 0) ? "PASS" : "FAIL", "re-placed at 2 MB-aligned + %#llx: %s ptr %s, earlier data kept %s, GPU check %s",
      (unsigned long long)g_min_align, vkc_result_str(r), got == want2 ? "==" : "!=", old2 == 0 ? "yes" : "NO", gbad2 == 0 ? "ok" : "BAD");
    if (!r) { if (g_unmap_reserve) unmap2(mem, 1); else { V.vkUnmapMemory(V.dev, mem); mmap(want2, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0); } }
    remap_normal_check("a.normal_map_after_placed", mem, (uintptr_t)want2, len, (uintptr_t)want2, seed ^ 1);
    V.vkFreeMemory(V.dev, mem, NULL);
    mmap(want, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    mmap(want2, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
}

// ---------------------------------------------------------------- (d) unmap with VK_MEMORY_UNMAP_RESERVE_BIT_EXT
static int mmap_lands_in(uintptr_t lo, uintptr_t hi, int hinted, char *desc, size_t dn) {
    static const size_t sizes[] = { 4096, 64 * 1024, 2 * MB, 16 * MB, 64 * MB, 256 * MB };
    int landed = 0; size_t used = 0; desc[0] = 0;
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        void *hint = hinted ? (void *)lo : NULL;
        void *q = mmap(hint, sizes[i], PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (q == MAP_FAILED) continue;
        int in = (uintptr_t)q < hi && (uintptr_t)q + sizes[i] > lo;
        landed |= in;
        used += snprintf(desc + used, used < dn ? dn - used : 0, "%s%zuK->%p%s", i ? " " : "", sizes[i] / 1024, q, in ? "(IN)" : "");
        munmap(q, sizes[i]);
    }
    return landed;
}
static void test_d(void) {
    S("== (d) vkUnmapMemory2 with VK_MEMORY_UNMAP_RESERVE_BIT_EXT");
    if (!g_placed_ok) { R("d.unmap_reserve", "SKIP", "no placed mapping"); return; }
    if (!g_unmap_reserve) { R("d.unmap_reserve", "FAIL", "memoryUnmapReserve feature not supported"); }
    VkDeviceMemory mem; size_t len = 64 * MB;
    VkResult r = vkc_alloc(&V, len, g_type, 0, &mem);
    if (r) { R("d.unmap_reserve", "FAIL", "vkAllocateMemory: %s", vkc_result_str(r)); return; }
    uint8_t *at = g_rb + 64 * MB; void *got; char desc[1024]; range_info_t ri;
    uint64_t seed = 0x5eed5eedull;
    if (g_unmap_reserve) {
        r = map_placed(mem, at, &got);
        long bad = (!r && got == at) ? touch_pages(at, len, (uintptr_t)at, seed, 1) : -3;
        VkResult ru = unmap2(mem, 1);
        range_info((uintptr_t)at, (uintptr_t)at + len, &ri);
        S("  placed %s (%ld bad), vkUnmapMemory2(RESERVE) = %s; maps over the range afterwards:", vkc_result_str(r), bad, vkc_result_str(ru)); S_lines("    ", ri.lines);
        int hinted = mmap_lands_in((uintptr_t)at, (uintptr_t)at + len, 1, desc, sizeof desc);
        S("  mmap with the range start as hint (no MAP_FIXED): %s", desc);
        int plain = mmap_lands_in((uintptr_t)at, (uintptr_t)at + len, 0, desc, sizeof desc);
        S("  mmap without hint: %s", desc);
        // place again over the reserved range (heap shrink / grow cycle): the data must still be there
        VkResult r2 = map_placed(mem, at, &got);
        long bad2 = (!r2 && got == at) ? touch_pages(at, len, (uintptr_t)at, seed, 0) : -3;
        S("  re-placing the same memory at the reserved range: %s, returned %p, data from before the unmap: %ld bad pages", vkc_result_str(r2), got, bad2);
        if (!r2) unmap2(mem, 1);
        int pass = !r && bad == 0 && !ru && ri.covered && !ri.any_access && !hinted && !plain && !r2 && bad2 == 0;
        R("d.unmap_reserve", pass ? "PASS" : "FAIL", "range %s after unmap (%s), hinted mmap %s, unhinted mmap %s, re-place %s",
          ri.covered ? "still mapped" : "HAS A HOLE", ri.all_anon_none ? "anonymous ---p" : (ri.any_access ? "ACCESSIBLE" : "---p, not anonymous"),
          hinted ? "LANDED INSIDE" : "kept out", plain ? "LANDED INSIDE" : "kept out", (!r2 && bad2 == 0) ? "ok, data kept" : "FAILED");
    }
    // control: without the RESERVE bit the range becomes a hole, and a hinted mmap does land there (so the test above can tell)
    r = map_placed(mem, at, &got);
    VkResult ru = g_placed_ok ? unmap2(mem, 0) : VK_ERROR_UNKNOWN;
    range_info((uintptr_t)at, (uintptr_t)at + len, &ri);
    int hinted = mmap_lands_in((uintptr_t)at, (uintptr_t)at + len, 1, desc, sizeof desc);
    S("  control: placed %s, vkUnmapMemory2(flags 0) = %s, range %s; hinted mmap: %s", vkc_result_str(r), vkc_result_str(ru), ri.covered ? "still mapped" : "a hole", desc);
    if (!r) { remap_normal_check("d.normal_map_after_unmap", mem, (uintptr_t)at, len, (uintptr_t)at, seed); }
    R("d.unmap_no_reserve", "INFO", "vkUnmapMemory2 WITHOUT the RESERVE bit: the range is %s (%s), and a hinted mmap %s", ri.covered ? "still reserved" : "a hole",
      ri.covered ? (ri.all_anon_none ? "anonymous ---p: the driver re-reserves either way" : "still mapped by something") : "as the spec says", hinted ? "lands in it" : "does not land in it");
    // method control: a real hole (munmap by hand) must attract the hinted mmap, otherwise the 'kept out' results above prove nothing
    munmap(at, len);
    range_info((uintptr_t)at, (uintptr_t)at + len, &ri);
    int ctl = mmap_lands_in((uintptr_t)at, (uintptr_t)at + len, 1, desc, sizeof desc);
    S("  method control, after munmap of the range by hand: range %s; hinted mmap: %s", ri.covered ? "still mapped" : "a hole", desc);
    R("d.method_control", (!ri.covered && ctl) ? "PASS" : "FAIL", "a hand-made hole %s the hinted mmap, so 'kept out' above %s", ctl ? "attracts" : "does NOT attract", ctl ? "is meaningful" : "IS NOT MEANINGFUL");
    if (mmap(at, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) S("  re-reserve failed: errno %d", errno);
    V.vkFreeMemory(V.dev, mem, NULL);
}

// ---------------------------------------------------------------- (c) two allocations back to back
static void test_c_one(const char *item, size_t lenA, size_t lenB, uint8_t *at) {
    VkDeviceMemory A, B; VkResult ra = vkc_alloc(&V, lenA, g_type, 0, &A), rb = vkc_alloc(&V, lenB, g_type, 0, &B);
    if (ra || rb) { R(item, "FAIL", "vkAllocateMemory: %s / %s", vkc_result_str(ra), vkc_result_str(rb)); if (!ra) V.vkFreeMemory(V.dev, A, NULL); if (!rb) V.vkFreeMemory(V.dev, B, NULL); return; }
    void *ga, *gb; uint8_t *atB = at + lenA;
    ra = map_placed(A, at, &ga); rb = map_placed(B, atB, &gb);
    range_info_t ri; range_info((uintptr_t)at, (uintptr_t)atB + lenB, &ri);
    S("  A %#zx at %p: %s -> %p; B %#zx at %p: %s -> %p; maps:", lenA, at, vkc_result_str(ra), ga, lenB, atB, vkc_result_str(rb), gb); S_lines("    ", ri.lines);
    int placed = !ra && !rb && ga == at && gb == atB;
    size_t span = 8 * MB; uint8_t *src = malloc(span), *back = malloc(span);
    for (size_t i = 0; i < span; i += 4) *(uint32_t *)(src + i) = pattern(77, (uint32_t)(i / 4));
    int ok = 0, cmp1 = -1, cmp2 = -1, cmp3 = -1; long ba = -1, bb = -1;
    if (placed) {
        uint8_t *d = atB - span / 2;
        GUARD_BEGIN(ok) memcpy(d, src, span); memcpy(back, d, span); GUARD_END(ok);
        if (ok) { cmp1 = memcmp(back, src, span) != 0; }
        // unaligned copy across the seam, byte offset 3
        int ok2;
        GUARD_BEGIN(ok2) memcpy(d + 3, src, span - 64); memcpy(back, d + 3, span - 64); GUARD_END(ok2);
        if (ok2) cmp2 = memcmp(back, src, span - 64) != 0;
        ok = ok && ok2;
        // put the aligned copy back and check through normal mappings that each half went to the right allocation
        int ok3; GUARD_BEGIN(ok3) memcpy(d, src, span); GUARD_END(ok3); ok = ok && ok3;
        if (!gpu_copy_out(A, lenA, lenA - span / 2, span / 2, back)) ba = memcmp(back, src, span / 2) != 0;
        if (!gpu_copy_out(B, lenB, 0, span / 2, back)) bb = memcmp(back, src + span / 2, span / 2) != 0;
        cmp3 = ba == 0 && bb == 0 ? 0 : 1;
        unmap2(A, g_unmap_reserve); unmap2(B, g_unmap_reserve);
    } else {
        if (!ra) unmap2(A, g_unmap_reserve);
        if (!rb) unmap2(B, g_unmap_reserve);
    }
    S("  memcpy across the seam: %s, readback %s, unaligned (+3) %s; A's tail / B's head copied out by the GPU: %ld / %ld (0 = match)",
      ok ? "no fault" : "FAULT", cmp1 == 0 ? "match" : "MISMATCH", cmp2 == 0 ? "match" : "MISMATCH", ba, bb);
    R(item, (placed && ok && cmp1 == 0 && cmp2 == 0 && cmp3 == 0) ? "PASS" : "FAIL", "A %#zx + B %#zx contiguous at %p: placed %s, %d VMA lines, seam memcpy %s, halves in the right allocations %s",
      lenA, lenB, at, placed ? "ok" : "FAILED", ri.nlines, (ok && !cmp1 && !cmp2) ? "ok" : "BAD", cmp3 == 0 ? "yes" : "NO");
    V.vkFreeMemory(V.dev, A, NULL); V.vkFreeMemory(V.dev, B, NULL);
    mmap(at, lenA + lenB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    free(src); free(back);
}
static void test_c(void) {
    S("== (c) two allocations placed back to back");
    if (!g_placed_ok) { R("c.contiguous", "SKIP", "no placed mapping"); return; }
    test_c_one("c.contiguous", 64 * MB, 64 * MB, g_rb + 1 * GB);
    if (g_min_align < 2 * MB) test_c_one("c.contiguous_min_align", 64 * MB + g_min_align, 64 * MB, g_rb + 1 * GB);
}

// ---------------------------------------------------------------- (b) one 3.5 GB allocation
static int try_alloc_place(size_t len, uint8_t *at, int keep, VkDeviceMemory *out, char *desc, size_t dn) {
    VkDeviceMemory m; double a0 = avail_gb();
    VkResult r = vkc_alloc(&V, len, g_type, 0, &m);
    double a1 = avail_gb();
    if (r) { snprintf(desc, dn, "alloc %.3f GB: %s", len / (double)GB, vkc_result_str(r)); return 0; }
    void *got; VkResult rm = map_placed(m, at, &got);
    snprintf(desc, dn, "alloc %.3f GB: ok (MemAvailable %.2f -> %.2f GB), placed: %s%s", len / (double)GB, a0, a1, vkc_result_str(rm), (!rm && got != at) ? " (WRONG ADDRESS)" : "");
    if (rm || got != at) {
        if (!rm) unmap2(m, g_unmap_reserve);
        void *q = NULL; VkResult rn = V.vkMapMemory(V.dev, m, 0, VK_WHOLE_SIZE, 0, &q);
        size_t l = strlen(desc); snprintf(desc + l, dn - l, "; normal vkMapMemory: %s", vkc_result_str(rn));
        if (!rn) V.vkUnmapMemory(V.dev, m);
        V.vkFreeMemory(V.dev, m, NULL);
        mmap(at, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        return 0;
    }
    if (keep) { *out = m; return 1; }
    placed_release(m, at, len);
    return 1;
}
static void test_b(void) {
    S("== (b) one 3.5 GB allocation, placed");
    if (!g_placed_ok) { R("b.single_3.5GB", "SKIP", "no placed mapping"); return; }
    uint8_t *at = g_rb + 1280 * MB; size_t want = 3584 * MB; char desc[512];
    S("  maxMemoryAllocationSize %.3f GB; MemAvailable %.2f GB", g_max_alloc / (double)GB, avail_gb());
    VkDeviceMemory m;
    size_t best = 0;
    if (try_alloc_place(want, at, 1, &m, desc, sizeof desc)) best = want;
    S("  %s", desc);
    if (!best) {   // bisection, 64 MB granularity, on allocate + place
        size_t lo = 0, hi = want;
        if (try_alloc_place(256 * MB, at, 0, NULL, desc, sizeof desc)) lo = 256 * MB;
        S("  %s", desc);
        while (lo && hi - lo > 64 * MB) {
            size_t mid = ((lo + hi) / 2) & ~(64 * MB - 1);
            int ok = try_alloc_place(mid, at, 0, NULL, desc, sizeof desc);
            S("  bisect %s", desc);
            if (ok) lo = mid; else hi = mid;
        }
        best = lo;
        if (best && !try_alloc_place(best, at, 1, &m, desc, sizeof desc)) best = 0;
    }
    if (!best) { R("b.single_3.5GB", "FAIL", "no allocation could be placed (see summary)"); return; }
    double a0 = avail_gb(), t0 = now_s();
    long bad = touch_pages(at, best, (uintptr_t)at, 0xb16b00b5ull, 1);
    double t1 = now_s(), a1 = avail_gb();
    S("  touched every page of %.3f GB at %p: %ld bad (-1 = fault) in %.2f s, MemAvailable %.2f -> %.2f GB", best / (double)GB, at, bad, t1 - t0, a0, a1);
    placed_release(m, at, best);
    S("  released; MemAvailable %.2f GB", avail_gb());
    R("b.single_3.5GB", (best == want && bad == 0) ? "PASS" : "FAIL", "largest placed single allocation %.3f GB (of 3.5 tried), every page rw %s", best / (double)GB, bad == 0 ? "ok" : "BAD");
}

// ---------------------------------------------------------------- (f) is freed memory given back to the system?
// 1 GB (or maxMemoryAllocationSize) allocated, mapped, every page written, unmapped and freed. MemAvailable is polled for 8 s after
// vkFreeMemory, and /proc/self/maps is checked for KGSL mappings the driver left behind. Variants: normal mapping; placed and unmapped
// with VK_MEMORY_UNMAP_RESERVE_BIT_EXT; placed and unmapped with plain vkUnmapMemory.
static size_t kgsl_mapped(int *nvma) {
    FILE *f = fopen("/proc/self/maps", "r"); size_t t = 0; *nvma = 0; if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof line, f)) if (strstr(line, "kgsl-3d0")) { unsigned long lo, hi; if (sscanf(line, "%lx-%lx", &lo, &hi) == 2) { t += hi - lo; (*nvma)++; } }
    fclose(f); return t;
}
static void test_free(void) {
    S("== (f) freed memory: given back to the system before the process exits?");
    size_t len = GB; if (g_max_alloc && len > g_max_alloc) len = g_max_alloc & ~(size_t)(2 * MB - 1);
    static const char *vn[3] = { "mapped", "placed_reserve", "placed_plain_unmap" };
    for (int v = 0; v < 3; v++) {
        if (v >= 1 && !g_placed_ok) continue;
        if (v == 1 && !g_unmap_reserve) continue;
        char item[48]; snprintf(item, sizeof item, "free.%s", vn[v]);
        sleep(2);
        int n0, n2, n3; size_t k0 = kgsl_mapped(&n0);
        double a0 = avail_gb(); VkDeviceMemory m; void *q = NULL; uint8_t *at = g_rb + 1280 * MB;
        if (vkc_alloc(&V, len, g_type, 0, &m)) { R(item, "FAIL", "allocation failed"); continue; }
        double a1 = avail_gb();
        VkResult r = v ? map_placed(m, at, &q) : V.vkMapMemory(V.dev, m, 0, VK_WHOLE_SIZE, 0, &q);
        if (r || (v && q != at)) { R(item, "FAIL", "map: %s", vkc_result_str(r)); V.vkFreeMemory(V.dev, m, NULL); continue; }
        long bad = touch_pages(q, len, (uintptr_t)q, 0xf4eeull + v, 1);
        double a2 = avail_gb(); size_t k2 = kgsl_mapped(&n2);
        if (v == 0) V.vkUnmapMemory(V.dev, m); else if (v == 1) unmap2(m, 1); else V.vkUnmapMemory(V.dev, m);
        V.vkFreeMemory(V.dev, m, NULL);
        if (v) mmap(at, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);   // our reservation back, as placed_release does
        size_t k3 = kgsl_mapped(&n3);
        double a3 = avail_gb(), t0 = now_s(), back_at = -1, a = a3;
        while (now_s() - t0 < 8.0) { a = avail_gb(); if (a - a2 >= 0.8 * (len / (double)GB)) { back_at = now_s() - t0; break; } usleep(250000); }
        double gb = len / (double)GB;
        S("  [%s] %.2f GB: MemAvailable %.2f, after alloc %.2f, after touching %.2f, right after free %.2f, last %.2f GB; KGSL mappings: before %zu MB in %d VMAs, mapped %zu MB in %d, after free %zu MB in %d; %ld bad pages",
          item, gb, a0, a1, a2, a3, a, (size_t)(k0 / MB), n0, (size_t)(k2 / MB), n2, (size_t)(k3 / MB), n3, bad);
        char kl[160] = "";
        if (k3 > k0) snprintf(kl, sizeof kl, "; %zu MB of KGSL mappings LEFT BEHIND after vkFreeMemory", (size_t)((k3 - k0) / MB));
        if (back_at >= 0) R(item, "INFO", "%.2f GB freed: given back to the system %.2f s after vkFreeMemory%s", gb, back_at, kl);
        else R(item, "INFO", "%.2f GB freed: NOT given back within 8 s (MemAvailable %+.2f GB)%s", gb, a - a2, kl);
    }
}

// ---------------------------------------------------------------- (e) total allocatable and mappable
static void test_e(double max_gb, double floor_gb) {
    S("== (e) total placed memory, 512 MB chunks up to %.1f GB, stopping when MemAvailable < %.1f GB", max_gb, floor_gb);
    if (!g_placed_ok) { R("e.total", "SKIP", "no placed mapping"); return; }
    size_t rlen = (size_t)((max_gb + 0.5) * GB) + 2 * MB;
    uint8_t *res = mmap(NULL, rlen, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (res == MAP_FAILED) { R("e.total", "FAIL", "reserve %.1f GB: errno %d", rlen / (double)GB, errno); return; }
    uint8_t *base = (uint8_t *)(((uintptr_t)res + 2 * MB - 1) & ~(uintptr_t)(2 * MB - 1));
    size_t chunk = 512 * MB, total = 0, cap = (size_t)(max_gb * GB);
    if (g_max_alloc && chunk > g_max_alloc) chunk = g_max_alloc & ~(size_t)(2 * MB - 1);
    VkDeviceMemory mems[64]; int n = 0; char stop[256] = "reached the cap";
    while (total + chunk <= cap && n < 64) {
        double av = avail_gb();
        for (int w = 0; av < floor_gb && w < 5; w++) { sleep(1); av = avail_gb(); }
        if (av < floor_gb) { snprintf(stop, sizeof stop, "MemAvailable %.2f GB below the floor", av); break; }
        VkResult r = vkc_alloc(&V, chunk, g_type, 0, &mems[n]);
        if (r) { snprintf(stop, sizeof stop, "vkAllocateMemory chunk %d: %s", n, vkc_result_str(r)); break; }
        void *got; r = map_placed(mems[n], base + total, &got);
        if (r || got != base + total) { snprintf(stop, sizeof stop, "placing chunk %d: %s", n, vkc_result_str(r)); if (!r) unmap2(mems[n], g_unmap_reserve); V.vkFreeMemory(V.dev, mems[n], NULL); break; }
        long bad = touch_pages(base + total, chunk, (uintptr_t)(base + total), 0xe0e0ull + (uint64_t)n, 1);
        if (bad) { snprintf(stop, sizeof stop, "touching chunk %d: %ld bad", n, bad); n++; total += chunk; break; }
        n++; total += chunk;
        S("  %5.2f GB placed and touched, MemAvailable %.2f GB", total / (double)GB, avail_gb());
    }
    long bad = 0;
    for (int i = 0; i < n; i++) { long b = touch_pages(base + (size_t)i * chunk, chunk, (uintptr_t)(base + (size_t)i * chunk), 0xe0e0ull + (uint64_t)i, 0); bad += b < 0 ? 1 : b; }
    S("  stopped: %s; total %.2f GB in %d chunks; re-check of every page: %ld bad", stop, total / (double)GB, n, bad);
    for (int i = 0; i < n; i++) { if (g_unmap_reserve) unmap2(mems[i], 1); else V.vkUnmapMemory(V.dev, mems[i]); V.vkFreeMemory(V.dev, mems[i], NULL); }
    munmap(res, rlen);
    S("  released; MemAvailable %.2f GB", avail_gb());
    R("e.total", (total >= 4 * GB && bad == 0) ? "PASS" : "FAIL", "%.2f GB placed contiguously and touched (%s), pass needs >= 4 GB", total / (double)GB, stop);
}

// ---------------------------------------------------------------- (4) CPU bandwidth
static uint64_t readsum(const uint8_t *p, size_t n) {
    uint64x2_t a0 = vdupq_n_u64(0), a1 = a0, a2 = a0, a3 = a0;
    for (size_t i = 0; i < n; i += 128) {
        uint32x4x4_t x = vld1q_u32_x4((const uint32_t *)(p + i)), y = vld1q_u32_x4((const uint32_t *)(p + i + 64));
        a0 = vpadalq_u32(a0, x.val[0]); a1 = vpadalq_u32(a1, x.val[1]); a2 = vpadalq_u32(a2, x.val[2]); a3 = vpadalq_u32(a3, x.val[3]);
        a0 = vpadalq_u32(a0, y.val[0]); a1 = vpadalq_u32(a1, y.val[1]); a2 = vpadalq_u32(a2, y.val[2]); a3 = vpadalq_u32(a3, y.val[3]);
    }
    a0 = vaddq_u64(vaddq_u64(a0, a1), vaddq_u64(a2, a3));
    return vgetq_lane_u64(a0, 0) + vgetq_lane_u64(a0, 1);
}
static volatile uint64_t g_sink;
static double bw_memset(uint8_t *p, size_t n) { double t = now_s(); memset(p, 0x5a, n); __asm__ volatile("" ::: "memory"); return n / (now_s() - t) / 1e9; }
static double bw_memcpy(uint8_t *d, const uint8_t *s, size_t n) { double t = now_s(); memcpy(d, s, n); __asm__ volatile("" ::: "memory"); return n / (now_s() - t) / 1e9; }
static double bw_read(const uint8_t *p, size_t n) { double t = now_s(); g_sink += readsum(p, n); return n / (now_s() - t) / 1e9; }

static int g_bw_cpu = -1; static cpu_set_t g_old_aff;
static void pin_big(void) {
    long bf = 0; int best = 7;
    for (int c = 0; c < 16; c++) {
        char p[128]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
        FILE *f = fopen(p, "r"); if (!f) continue;
        long v = 0; if (fscanf(f, "%ld", &v) == 1 && v >= bf) { bf = v; best = c; }
        fclose(f);
    }
    sched_getaffinity(0, sizeof g_old_aff, &g_old_aff);
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(best, &s);
    int rc = sched_setaffinity(0, sizeof s, &s);
    g_bw_cpu = best;
    S("  pinned to cpu%d (max %ld kHz): %s; running on cpu%d", best, bf, rc ? strerror(errno) : "ok", sched_getcpu());
}
static void unpin(void) { sched_setaffinity(0, sizeof g_old_aff, &g_old_aff); }
static void spin_warm(double secs) { double t = now_s(); volatile uint64_t x = 0; while (now_s() - t < secs) for (int i = 0; i < 100000; i++) x += i; }   // let DVFS raise the core clock
#define BW_MAX 32
static double median_of(const double *v, int n) { double t[BW_MAX]; if (n > BW_MAX) n = BW_MAX; memcpy(t, v, n * sizeof *t); for (int i = 1; i < n; i++) for (int j = i; j > 0 && t[j] < t[j - 1]; j--) { double x = t[j]; t[j] = t[j - 1]; t[j - 1] = x; } return n ? t[n / 2] : 0; }
static double best_of(const double *v, int n) { double m = 0; for (int i = 0; i < n; i++) if (v[i] > m) m = v[i]; return m; }
// Sustained memory traffic so the memory-bus governor settles before measuring (in the app the bus clock moves a lot without it)
static void mem_warm(uint8_t *p, size_t len, double secs) { double t = now_s(); while (now_s() - t < secs) { memset(p, 0x33, len); g_sink += readsum(p, len); } }

// Paired design: within each repetition, the same operation runs on every kind back to back (the order rotates per repetition), so the
// kinds of one pair see the same core and bus clocks. The verdict uses the median over repetitions of the per-pair ratio to anonymous
// memory, which drifting clocks do not bias; best-of values are reported too.
static const char *bw_opn[3] = { "memset", "memcpy", "read-sum" };
typedef struct { const char *name; uint8_t *a, *b; double v[3][BW_MAX]; } bwk_t;
static double bw_op(int op, bwk_t *k, size_t len) { return op == 0 ? bw_memset(k->b, len) : op == 1 ? bw_memcpy(k->b, k->a, len) : bw_read(k->a, len); }
// The paired measurement over nk kinds (k[0] is anonymous memory, the reference); result items <pfx><kind name>
static void bw_pairs(bwk_t *k, int nk, size_t len, int reps, const char *pfx) {
    pin_big();
    char sc[256]; sched_ctx(sc, sizeof sc);
    int pinned = sched_getcpu() == g_bw_cpu;
    S("  scheduling: %s; pinned to the fastest core: %s", sc, pinned ? "yes" : "NO (numbers not comparable)");
    for (int i = 0; i < nk; i++) { memset(k[i].a, 1, len); memset(k[i].b, 2, len); g_sink += readsum(k[i].a, len); }   // fault in every page
    spin_warm(0.3); mem_warm(k[0].b, len, 1.0);
    for (int r = 0; r < reps; r++)
        for (int op = 0; op < 3; op++)
            for (int j = 0; j < nk; j++) { int i = (j + r) % nk; k[i].v[op][r] = bw_op(op, &k[i], len); }
    pinned = pinned && sched_getcpu() == g_bw_cpu;
    unpin();
    for (int i = 0; i < nk; i++) {
        for (int op = 0; op < 3; op++) {
            char l[700]; size_t u = snprintf(l, sizeof l, "  %-12s %-8s", k[i].name, bw_opn[op]);
            for (int r = 0; r < reps; r++) u += snprintf(l + u, sizeof l - u, " %.1f", k[i].v[op][r]);
            S("%s GB/s; best %.2f median %.2f", l, best_of(k[i].v[op], reps), median_of(k[i].v[op], reps));
        }
        char item[48]; snprintf(item, sizeof item, "%s%s", pfx, k[i].name);
        if (i == 0) { R(item, pinned ? "INFO" : "FAIL", "best memset %.2f memcpy %.2f read-sum %.2f GB/s; median %.2f %.2f %.2f (cpu%d, %d reps)%s", best_of(k[0].v[0], reps), best_of(k[0].v[1], reps), best_of(k[0].v[2], reps),
                        median_of(k[0].v[0], reps), median_of(k[0].v[1], reps), median_of(k[0].v[2], reps), g_bw_cpu, reps, pinned ? "" : " NOT PINNED, INVALID"); continue; }
        if (!pinned) { R(item, "FAIL", "INVALID: not running pinned on the fastest core (%s)", sc); continue; }
        double mr[3], br[3];
        for (int op = 0; op < 3; op++) {
            double q[BW_MAX]; for (int r = 0; r < reps; r++) q[r] = k[i].v[op][r] / k[0].v[op][r];
            mr[op] = median_of(q, reps); br[op] = best_of(k[i].v[op], reps) / best_of(k[0].v[op], reps);
            char l[700]; size_t u = snprintf(l, sizeof l, "  %-12s %-8s ratio to anon per pair:", k[i].name, bw_opn[op]);
            for (int r = 0; r < reps; r++) u += snprintf(l + u, sizeof l - u, " %.2f", q[r]);
            S("%s", l);
        }
        int pass = mr[0] >= 0.9 && mr[1] >= 0.9 && mr[2] >= 0.9;
        R(item, pass ? "PASS" : "FAIL", "median paired ratio to anon: memset %.0f%% memcpy %.0f%% read-sum %.0f%% (pass >= 90%% each); best memset %.2f (%.0f%%) memcpy %.2f (%.0f%%) read-sum %.2f (%.0f%%) GB/s",
          mr[0] * 100, mr[1] * 100, mr[2] * 100, best_of(k[i].v[0], reps), br[0] * 100, best_of(k[i].v[1], reps), br[1] * 100, best_of(k[i].v[2], reps), br[2] * 100);
    }
}
static void bw_suite(int reps) {
    if (reps > BW_MAX) reps = BW_MAX;
    S("== (4) CPU bandwidth, 256 MB buffers, %d paired repetitions (each operation on every kind back to back, order rotating)", reps);
    size_t len = 256 * MB;
    bwk_t k[3]; int nk = 0;
    memset(k, 0, sizeof k);
    // anonymous
    k[nk].name = "anon"; k[nk].a = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); k[nk].b = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (k[nk].a == MAP_FAILED || k[nk].b == MAP_FAILED) { R("bw.anon", "FAIL", "anonymous mmap failed"); return; }
    nk++;
    // placed (two adjacent allocations in the reservation) and normal vkMapMemory, same memory type
    VkDeviceMemory pm[2] = {0}, nm[2] = {0}; int have_p = 0, have_n = 0;
    if (g_placed_ok) {
        VkResult r0 = vkc_alloc(&V, len, g_type, 0, &pm[0]), r1 = vkc_alloc(&V, len, g_type, 0, &pm[1]);
        void *g0 = NULL, *g1 = NULL;
        if (!r0 && !r1 && !map_placed(pm[0], g_rb + 64 * MB, &g0) && !map_placed(pm[1], g_rb + 64 * MB + len, &g1) && g0 == g_rb + 64 * MB && g1 == g_rb + 64 * MB + len) {
            k[nk].name = "placed"; k[nk].a = g0; k[nk].b = g1; nk++; have_p = 1;
        } else S("  placed buffers for the bandwidth test failed: %s %s", vkc_result_str(r0), vkc_result_str(r1));
    }
    {
        VkResult r0 = vkc_alloc(&V, len, g_type, 0, &nm[0]), r1 = vkc_alloc(&V, len, g_type, 0, &nm[1]);
        void *q0 = NULL, *q1 = NULL;
        if (!r0 && !r1 && !V.vkMapMemory(V.dev, nm[0], 0, VK_WHOLE_SIZE, 0, &q0) && !V.vkMapMemory(V.dev, nm[1], 0, VK_WHOLE_SIZE, 0, &q1)) {
            k[nk].name = "vkMapMemory"; k[nk].a = q0; k[nk].b = q1; nk++; have_n = 1;
        } else S("  normal-mapped buffers for the bandwidth test failed: %s %s", vkc_result_str(r0), vkc_result_str(r1));
    }
    bw_pairs(k, nk, len, reps, "bw.");
    munmap(k[0].a, len); munmap(k[0].b, len);
    if (have_p) { placed_release(pm[0], g_rb + 64 * MB, len); placed_release(pm[1], g_rb + 64 * MB + len, len); }
    else { if (pm[0]) V.vkFreeMemory(V.dev, pm[0], NULL); if (pm[1]) V.vkFreeMemory(V.dev, pm[1], NULL); }
    if (have_n) { V.vkUnmapMemory(V.dev, nm[0]); V.vkUnmapMemory(V.dev, nm[1]); }
    if (nm[0]) V.vkFreeMemory(V.dev, nm[0], NULL);
    if (nm[1]) V.vkFreeMemory(V.dev, nm[1], NULL);
}
// every host-visible type, normal mapping, 64 MB, paired with anonymous memory: shows what an uncached type looks like with the same code
static void types_bw(void) {
    const int reps = 7; const size_t len = 64 * MB;
    S("== per-memory-type bandwidth (normal vkMapMemory, 64 MB, %d pairs with anonymous memory, median ratio)", reps);
    VkBuffer b; uint32_t bits = 0;
    if (vkc_buffer(&V, VK_NULL_HANDLE, 0, len, k_usage, &b, &bits) == VK_SUCCESS) V.vkDestroyBuffer(V.dev, b, NULL);
    uint8_t *an = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (an == MAP_FAILED) { S("  anonymous mmap failed"); return; }
    pin_big(); memset(an, 1, len); spin_warm(0.3); mem_warm(an, len, 0.5);
    for (int32_t t = g_placed_ok ? -1 : 0; t < (int32_t)V.mem.memoryTypeCount; t++) {   // -1: the chosen type, placed
        uint32_t ty = t < 0 ? g_type : (uint32_t)t;
        VkMemoryPropertyFlags f = V.mem.memoryTypes[ty].propertyFlags;
        if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) || (f & VK_MEMORY_PROPERTY_PROTECTED_BIT)) continue;
        VkDeviceMemory m; void *q = NULL;
        if (vkc_alloc(&V, len, ty, 0, &m)) continue;
        if (t < 0) { if (map_placed(m, g_rb + 64 * MB, &q) || q != g_rb + 64 * MB) { S("  placed 64 MB failed"); V.vkFreeMemory(V.dev, m, NULL); continue; } }
        else if (V.vkMapMemory(V.dev, m, 0, VK_WHOLE_SIZE, 0, &q)) { V.vkFreeMemory(V.dev, m, NULL); continue; }
        memset(q, 1, len);
        double ts[BW_MAX], tr[BW_MAX], qs[BW_MAX], qr[BW_MAX];
        for (int r = 0; r < reps; r++) {
            double as, ar;
            if (r & 1) { ts[r] = bw_memset(q, len); as = bw_memset(an, len); tr[r] = bw_read(q, len); ar = bw_read(an, len); }
            else { as = bw_memset(an, len); ts[r] = bw_memset(q, len); ar = bw_read(an, len); tr[r] = bw_read(q, len); }
            qs[r] = ts[r] / as; qr[r] = tr[r] / ar;
        }
        char fl[256]; vkc_memflags_str(f, fl, sizeof fl);
        S("  type %u%s (%s)%s: memset median %.2f GB/s (%.0f%% of anon), read-sum median %.2f GB/s (%.0f%% of anon)", ty, t < 0 ? " PLACED" : "", fl, (bits >> ty) & 1 ? "" : " [not in storage-buffer bits]",
          median_of(ts, reps), median_of(qs, reps) * 100, median_of(tr, reps), median_of(qr, reps) * 100);
        if (t < 0) placed_release(m, g_rb + 64 * MB, len); else { V.vkUnmapMemory(V.dev, m); V.vkFreeMemory(V.dev, m, NULL); }
    }
    unpin(); munmap(an, len);
}

// ---------------------------------------------------------------- GPU-visible mapping used by the coherence and dirty tests
typedef struct { VkDeviceMemory mem; uint8_t *ptr; VkBuffer buf; VkDeviceAddress addr; size_t size; int placed; } gmap_t;
static int gmap_create(gmap_t *g, size_t size, uint8_t *placed_at, int bda) {
    memset(g, 0, sizeof *g); g->size = size; g->placed = placed_at != NULL;
    VkResult r = vkc_alloc(&V, size, g_type, bda, &g->mem);
    if (r) { S("  gmap alloc %zu MB: %s", size / MB, vkc_result_str(r)); return -1; }
    void *p = NULL;
    if (placed_at) { r = map_placed(g->mem, placed_at, &p); if (!r && p != placed_at) { S("  gmap placed at %p returned %p", placed_at, p); r = VK_ERROR_MEMORY_MAP_FAILED; } }
    else r = V.vkMapMemory(V.dev, g->mem, 0, VK_WHOLE_SIZE, 0, &p);
    if (r) { S("  gmap map: %s", vkc_result_str(r)); V.vkFreeMemory(V.dev, g->mem, NULL); g->mem = 0; return -1; }
    g->ptr = p;
    uint32_t bits = 0;
    r = vkc_buffer(&V, g->mem, 0, size, k_usage | (bda ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0), &g->buf, &bits);
    if (r || !((bits >> g_type) & 1)) { S("  gmap buffer: %s, memoryTypeBits %#x (type %u %s)", vkc_result_str(r), bits, g_type, (bits >> g_type) & 1 ? "ok" : "NOT ALLOWED"); return -1; }
    if (bda) { VkBufferDeviceAddressInfo ai = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, g->buf }; g->addr = V.GetBufferDeviceAddress(V.dev, &ai); }
    return 0;
}
static void gmap_destroy(gmap_t *g) {
    if (g->buf) V.vkDestroyBuffer(V.dev, g->buf, NULL);
    if (g->mem) { if (g->placed) placed_release(g->mem, g->ptr, g->size); else { V.vkUnmapMemory(V.dev, g->mem); V.vkFreeMemory(V.dev, g->mem, NULL); } }
    memset(g, 0, sizeof *g);
}
typedef struct { uint32_t mism, first, sum, pad; } kres_t;
// A compute pipeline built from coherence.comp or diag.comp: descriptors or buffer device addresses, parameters in push constants or in a
// storage buffer at res + 256 that the GPU writes (vkCmdUpdateBuffer) in the same command buffer.
typedef struct { vkc_compute_t c; int ok, bda, push; const char *name; } kern_t;
static int g_kern_no_xfer;   // kdiag control: the result / parameter block written by the CPU instead of vkCmdUpdateBuffer
static kern_t g_kd = { .name = "coherence/descriptors" }, g_kb = { .bda = 1, .name = "coherence/BDA" };
static kern_t g_dk[4] = { { .push = 1, .name = "sanity/descriptors/push constants" }, { .bda = 1, .push = 1, .name = "sanity/BDA/push constants" },
                          { .name = "sanity/descriptors/parameter buffer" }, { .bda = 1, .name = "sanity/BDA/parameter buffer" } };
// Runs one of them over n words (one invocation per word, grid-stride beyond 1024 groups); offsets in bytes inside g; res needs 296 bytes
static VkResult run_kernel_c(kern_t *k, gmap_t *g, size_t src, size_t dst, size_t dst_bytes, size_t res, uint32_t n, uint32_t seed, uint32_t check) {
    vkc_compute_t *c = &k->c; const size_t prm = res + 256;   // 256: any minStorageBufferOffsetAlignment
    struct { uint64_t s, d, r; uint32_t seed, n, check, pad; } pb = { g->addr + src, g->addr + dst, g->addr + res, seed, n, check, 0 };
    uint32_t pd[4] = { seed, n, check, 0 };
    if (!k->bda) { vkc_compute_bind(&V, c, 0, g->buf, src, (VkDeviceSize)n * 4); vkc_compute_bind(&V, c, 1, g->buf, dst, dst_bytes); vkc_compute_bind(&V, c, 2, g->buf, res, 16); if (!k->push) vkc_compute_bind(&V, c, 3, g->buf, prm, 16); }
    else if (!k->push) vkc_compute_bind(&V, c, 0, g->buf, prm, sizeof pb);
    VkResult r = vkc_cmd_begin(&V); if (r) return r;
    uint8_t blk[256 + sizeof pb]; memset(blk, 0, sizeof blk);
    const uint32_t init[4] = { 0, 0xffffffffu, 0, 0 };   // mism, first, sum, pad, then the parameters: one command (two overlapping fills would need a barrier between them)
    memcpy(blk, init, sizeof init);
    if (k->bda) memcpy(blk + 256, &pb, sizeof pb); else memcpy(blk + 256, pd, sizeof pd);
    if (g_kern_no_xfer) memcpy(g->ptr + res, blk, sizeof blk);   // control: CPU store (coherent memory), no transfer command before the dispatch
    else {
        V.vkCmdUpdateBuffer(V.cmd, g->buf, res, sizeof blk, blk);
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
        V.vkCmdPipelineBarrier(V.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    V.vkCmdBindPipeline(V.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->pipe);
    if (c->ds) V.vkCmdBindDescriptorSets(V.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c->pl, 0, 1, &c->ds, 0, NULL);
    if (k->push) { if (k->bda) V.vkCmdPushConstants(V.cmd, c->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pb, &pb); else V.vkCmdPushConstants(V.cmd, c->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pd, pd); }
    uint32_t groups = (n + 255) / 256; if (groups > 1024) groups = 1024;
    V.vkCmdDispatch(V.cmd, groups, 1, 1);
    VkMemoryBarrier hb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
    V.vkCmdPipelineBarrier(V.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    r = vkc_cmd_submit_wait(&V, 5000000000ull);
    if (V.lost && !g_lost_seen) { g_lost_seen = 1; S("!!!! VK_ERROR_DEVICE_LOST: every later GPU result in this run is void"); }
    return r;
}
// dst[k] = src[k] ^ xform(seed, k) on the GPU (coherence.comp)
static VkResult run_kernel(gmap_t *g, int bda, size_t src, size_t dst, size_t res, uint32_t n, uint32_t seed, uint32_t check) {
    return run_kernel_c(bda ? &g_kb : &g_kd, g, src, dst, (size_t)n * 4, res, n, seed, check);
}
static int kernels_init(void) {
    static int done; if (done) return 0; done = 1;
    struct { kern_t *k; const uint32_t *code; size_t bytes; uint32_t nbind, push; } t[6] = {
        { &g_kd, coh_desc_spv, sizeof coh_desc_spv, 4, 0 }, { &g_kb, coh_bda_spv, sizeof coh_bda_spv, 1, 0 },
        { &g_dk[0], diag_desc_push_spv, sizeof diag_desc_push_spv, 3, 16 }, { &g_dk[1], diag_bda_push_spv, sizeof diag_bda_push_spv, 0, 40 },
        { &g_dk[2], diag_desc_buf_spv, sizeof diag_desc_buf_spv, 4, 0 }, { &g_dk[3], diag_bda_buf_spv, sizeof diag_bda_buf_spv, 1, 0 } };
    for (int i = 0; i < 6; i++) {
        if (t[i].k->bda && !g_bda) continue;
        VkResult r = vkc_compute(&V, t[i].code, t[i].bytes, t[i].nbind, t[i].push, &t[i].k->c); t[i].k->ok = r == VK_SUCCESS;
        S("  pipeline %s: %s", t[i].k->name, vkc_result_str(r));
    }
    return g_kd.ok ? 0 : -1;
}

// ---------------------------------------------------------------- (5a) compute sanity: does the driver compute what the kernels ask?
// Runs diag.comp (each invocation writes 8 words that isolate one operation) and coherence.comp over a small range, on a normal mapping,
// with the CPU checking every word. A FAIL here means the coherence / fresh / dirty kernel results on this driver measure the compiler,
// not memory visibility.
static void kdiag_one(gmap_t *g, int bda, int diag, int push, uint32_t n) {
    char item[48]; snprintf(item, sizeof item, "kdiag.%s.%s", diag ? (push ? (g_kern_no_xfer ? "ops_push_noxfer" : "ops_push") : "ops_buf") : "coh", bda ? "bda" : "desc");
    kern_t *c = diag ? &g_dk[(push ? 0 : 2) + bda] : (bda ? &g_kb : &g_kd);
    if (!c->ok) { R(item, "SKIP", "no pipeline"); return; }
    const size_t SRC = 0, DST = 1 * MB, RES = 3 * MB;   // n <= 32768: dst (8 words per invocation) fits in 1 MB
    volatile uint32_t *src = (uint32_t *)(g->ptr + SRC), *dst = (uint32_t *)(g->ptr + DST), *res = (uint32_t *)(g->ptr + RES);
    uint32_t seed = rnd(), sum = 0; size_t dw = diag ? 8 * (size_t)n : n;
    for (uint32_t k = 0; k < n; k++) { src[k] = pattern(seed, k); sum += src[k]; }
    for (size_t k = 0; k < dw; k++) dst[k] = 0xCDCDCDCDu;
    VkResult r = run_kernel_c(c, g, SRC, DST, dw * 4, RES, n, seed, 1);
    if (r) { R(item, "FAIL", "%u invocations: submit/wait %s", n, vkc_result_str(r)); return; }
    uint32_t r0 = res[0], r1 = res[1], r2 = res[2];
    if (diag) {
        static const char *fn[8] = { "invocation id", "push seed", "push n", "i*0x7feb352d", "hash(i)", "load src[i]", "src[i]^hash(i^seed)", "NumWorkGroups" };
        long bad[8] = { 0 }; char first[8][96]; uint32_t groups = (n + 255) / 256;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t want[8] = { i, seed, n, i * 0x7feb352du, h32(i), pattern(seed, i), pattern(seed, i) ^ h32(i ^ seed), groups };
            for (int f = 0; f < 8; f++) { uint32_t got = dst[8 * (size_t)i + f]; if (got != want[f]) { if (!bad[f]) snprintf(first[f], sizeof first[f], "i %u got %08x want %08x", i, got, want[f]); bad[f]++; } }
        }
        long tot = 0; for (int f = 0; f < 8; f++) { tot += bad[f]; if (bad[f]) S("  [%s] %-20s %ld of %u wrong, first %s", item, fn[f], bad[f], n, first[f]); }
        if (tot) { S("  [%s] first 4 invocations as written:", item); for (uint32_t i = 0; i < 4 && i < n; i++) S("    %08x %08x %08x %08x %08x %08x %08x %08x", dst[8*i], dst[8*i+1], dst[8*i+2], dst[8*i+3], dst[8*i+4], dst[8*i+5], dst[8*i+6], dst[8*i+7]); }
        int ok = !tot && r0 == n && r2 == sum;
        R(item, ok ? "PASS" : "FAIL", "%u invocations in %u groups: atomic count %u (want %u), atomic sum %s; wrong words: id %ld, seed %ld, n %ld, mul %ld, hash %ld, load %ld, load^hash %ld, groups %ld",
          n, groups, r0, n, r2 == sum ? "ok" : "WRONG", bad[0], bad[1], bad[2], bad[3], bad[4], bad[5], bad[6], bad[7]);
    } else {
        long bad = 0; char ex[3][120]; int nex = 0;
        for (uint32_t k = 0; k < n; k++) {
            uint32_t want = pattern(seed, k) ^ xform(seed, k), got = dst[k];
            if (got != want) { if (nex < 3) snprintf(ex[nex++], sizeof ex[0], "k %u got %08x want %08x (got^src %08x, xform %08x)", k, got, want, got ^ pattern(seed, k), xform(seed, k)); bad++; }
        }
        for (int i = 0; i < nex; i++) S("  [%s] %s", item, ex[i]);
        int ok = !bad && r0 == 0 && r1 == 0xffffffffu && r2 == sum;
        R(item, ok ? "PASS" : "FAIL", "coherence kernel over %u words, check on: GPU mismatch count %u (want 0), first %#x (want 0xffffffff), sum %s; CPU sees %ld of %u dst words wrong",
          n, r0, r1, r2 == sum ? "ok" : "WRONG", bad, n);
    }
}
// push_only: -1 everything, 0 without the push-constant runs, 1 only those. With BDA, push constants carry pointers: on a driver that reads
// garbage there the kernel writes through garbage pointers, so those runs come last of all (they can lose the device or scribble).
static void kdiag_suite(int bda, int push_only) {
    S("== (5a) compute sanity (%s%s)", bda ? "buffer device address" : "descriptors", push_only == 1 ? ", push constants" : "");
    if (kernels_init()) { R("kdiag.pipeline", "FAIL", "compute pipeline creation failed"); return; }
    gmap_t g;
    if (gmap_create(&g, 4 * MB, NULL, g_bda)) { R(bda ? "kdiag.bda" : "kdiag.desc", "FAIL", "could not create the mapping / buffer"); gmap_destroy(&g); return; }
    if (bda && !g.addr) { R("kdiag.bda", "SKIP", "no buffer device address"); gmap_destroy(&g); return; }
    if (push_only < 0 || !push_only) {
        kdiag_one(&g, bda, 1, 0, 8192);   // parameters in a storage buffer (32 workgroups)
        kdiag_one(&g, bda, 0, 0, 8192);   // the coherence kernel
    }
    if (push_only < 0 || push_only) {     // parameters in push constants: after a transfer command (as everywhere else), and without one
        kdiag_one(&g, bda, 1, 1, 8192);
        g_kern_no_xfer = 1; kdiag_one(&g, bda, 1, 1, 8192); g_kern_no_xfer = 0;
    }
    gmap_destroy(&g);
}

// ---------------------------------------------------------------- (5) coherence
static void coherence_one(gmap_t *g, const char *label, int bda, int flush) {
    // source and destination halves of 32 MB, the result block after them; a smaller mapping (the 64 MB AHB pool) gets smaller halves
    size_t half = 32 * MB; if (g->size < 2 * half + MB) half = ((g->size - MB) / 2) & ~(size_t)(MB - 1);
    const size_t SRC = 0, DST = half, RES = 2 * half;
    int rounds_small = 200, rounds_large = 4, fail_gpu = 0, fail_sum = 0, fail_cpu = 0, fail_sub = 0, total = 0; long bad_words = 0;
    VkMappedMemoryRange mr = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, g->mem, 0, VK_WHOLE_SIZE };
    volatile uint32_t *src = (uint32_t *)(g->ptr + SRC), *dst = (uint32_t *)(g->ptr + DST);
    volatile kres_t *res = (kres_t *)(g->ptr + RES);
    double t0 = now_s();
    for (int i = 0; i < rounds_small + rounds_large; i++) {
        uint32_t n = i < rounds_small ? 256 * 1024 : (uint32_t)(half / 4);   // 1 MB (stays in the CPU caches) or a half (32 MB)
        uint32_t seed = rnd();
        uint32_t prime = 0; for (uint32_t k = 0; k < n; k += 16) prime += dst[k];   // leave the destination's old lines in the CPU caches
        (void)res->sum; g_sink += prime;
        uint32_t sum = 0;
        for (uint32_t k = 0; k < n; k++) { uint32_t w = pattern(seed, k); src[k] = w; sum += w; }
        if (flush) V.vkFlushMappedMemoryRanges(V.dev, 1, &mr);
        VkResult r = run_kernel(g, bda, SRC, DST, RES, n, seed, 1);
        if (r) { fail_sub++; S("  [%s] submit/wait: %s", label, vkc_result_str(r)); break; }
        if (flush) V.vkInvalidateMappedMemoryRanges(V.dev, 1, &mr);
        total++;
        uint32_t gm = res->mism, gs = res->sum, gf = res->first;
        long bad = 0; for (uint32_t k = 0; k < n; k++) if (dst[k] != (pattern(seed, k) ^ xform(seed, k))) bad++;
        if (gm) { fail_gpu++; if (fail_gpu <= 3) S("  [%s] round %d: GPU saw %u stale words (first %u)", label, i, gm, gf); }
        if (!gm && gf != 0xffffffffu) { fail_sum++; if (fail_sum <= 3) S("  [%s] round %d: result block not reset (first %#x)", label, i, gf); }
        else if (gs != sum) { fail_sum++; if (fail_sum <= 3) S("  [%s] round %d: GPU sum %#x expected %#x", label, i, gs, sum); }
        if (bad) { fail_cpu++; bad_words += bad; if (fail_cpu <= 3) S("  [%s] round %d: CPU saw %ld stale GPU-written words", label, i, bad); }
    }
    char item[48]; snprintf(item, sizeof item, "coh.%s", label);
    int pass = total == rounds_small + rounds_large && !fail_gpu && !fail_sum && !fail_cpu;
    R(item, pass ? "PASS" : "FAIL", "%d rounds (%d x 1 MB, %d x %zu MB) in %.2f s: GPU saw stale CPU writes in %d, wrong sum in %d, CPU saw stale GPU writes in %d (%ld words)%s",
      total, rounds_small, rounds_large, (size_t)(half / MB), now_s() - t0, fail_gpu, fail_sum, fail_cpu, bad_words, fail_sub ? ", SUBMIT FAILED" : "");
}
static void coherence_suite(const char *kind, int placed, int bda) {
    S("== (5) coherence on %s memory (%s)", kind, bda ? "buffer device address" : "descriptors");
    if (kernels_init()) { R("coh.pipeline", "FAIL", "compute pipeline creation failed"); return; }
    gmap_t g;
    if (gmap_create(&g, 72 * MB, placed ? g_rb + 64 * MB : NULL, g_bda)) { char it[48]; snprintf(it, sizeof it, "coh.%s", kind); R(it, "FAIL", "could not create the mapping / buffer"); gmap_destroy(&g); return; }
    S("  %s mapping at %p, buffer device address %#llx", kind, g.ptr, (unsigned long long)g.addr);
    char l[64];
    if (!bda) { snprintf(l, sizeof l, "%s.desc.noflush", kind); coherence_one(&g, l, 0, 0); snprintf(l, sizeof l, "%s.desc.flush", kind); coherence_one(&g, l, 0, 1); }
    else if (g_bda && g_kb.ok && g.addr) { snprintf(l, sizeof l, "%s.bda.noflush", kind); coherence_one(&g, l, 1, 0); snprintf(l, sizeof l, "%s.bda.flush", kind); coherence_one(&g, l, 1, 1); }
    else { snprintf(l, sizeof l, "coh.%s.bda", kind); R(l, "SKIP", "bufferDeviceAddress %s, pipeline %s, address %#llx", g_bda ? "on" : "off", g_kb.ok ? "ok" : "none", (unsigned long long)g.addr); }
    gmap_destroy(&g);
}

// ---------------------------------------------------------------- (5b) GPU writes into memory the CPU has never touched
// A fresh allocation, mapped (placed or normal) but never accessed by the CPU, is filled by the GPU (vkCmdFillBuffer), then read by the CPU
// after the fence. Control: the same with one CPU read per page before the fill. Then the GPU copies the range out (what the GPU sees) and
// the CPU reads again after one more fence. Under strategy A, render targets and GPU-written buffers live in guest memory the CPU may never have touched.
static long count_ne(volatile uint32_t *p, size_t nw, uint32_t v, long *zero) { long b = 0; *zero = 0; for (size_t i = 0; i < nw; i++) { uint32_t w = p[i]; if (w != v) { b++; *zero += w == 0; } } return b; }
// mode 0: vkCmdFillBuffer over the whole fresh allocation. mode 1: the compute kernel reads a CPU-written source (first 16 MB) and writes
// the untouched second half (dst = src ^ xform), as the dirty test does.
static void fresh_one(const char *kind, int placed, int touch_first, int mode) {
    const size_t len = 32 * MB, half = 16 * MB; const uint32_t V1 = 0xA5C3F00Fu; long z;
    gmap_t g; char item[64]; snprintf(item, sizeof item, "fresh.%s.%s.%s", kind, mode ? "kernel" : "fill", touch_first ? "touched" : "untouched");
    if (gmap_create(&g, len + MB, placed ? g_rb + 64 * MB : NULL, 0)) { R(item, "FAIL", "could not create the mapping"); gmap_destroy(&g); return; }
    volatile uint32_t *w = (volatile uint32_t *)g.ptr;
    size_t off0 = mode ? half : 0, nw = (mode ? half : len) / 4; uint32_t seed = rnd();
    volatile uint32_t *d = (volatile uint32_t *)(g.ptr + off0);
    uint32_t *exp = malloc(nw * 4);
    if (mode) for (size_t k = 0; k < nw; k++) { uint32_t v = pattern(seed, (uint32_t)k); w[k] = v; exp[k] = v ^ xform(seed, (uint32_t)k); }
    else for (size_t k = 0; k < nw; k++) exp[k] = V1;
    if (touch_first) for (size_t o = 0; o < nw; o += g_pg / 4) g_sink += d[o];
    VkResult r;
    if (mode) r = run_kernel(&g, 0, 0, half, len, (uint32_t)nw, seed, 1);
    else {
        r = vkc_cmd_begin(&V);
        V.vkCmdFillBuffer(V.cmd, g.buf, 0, len, V1);
        VkMemoryBarrier hb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
        V.vkCmdPipelineBarrier(V.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
        if (!r) r = vkc_cmd_submit_wait(&V, 5000000000ull);
    }
    #define CNT(out, zo) do { out = 0; zo = 0; for (size_t i = 0; i < nw; i++) { uint32_t x = d[i]; if (x != exp[i]) { out++; zo += x == 0; } } } while (0)
    long bad1, z1, bad2, bad3; (void)z;
    CNT(bad1, z1);
    uint32_t *tmp = malloc(1 * MB); long gbad = 0;
    for (size_t o = 0; o < nw * 4; o += 4 * MB) if (!gpu_copy_out(g.mem, len + MB, off0 + o, 1 * MB, tmp)) for (size_t i = 0; i < MB / 4; i++) gbad += tmp[i] != exp[o / 4 + i];
    free(tmp);
    CNT(bad2, z);
    VkMappedMemoryRange mr = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, g.mem, 0, VK_WHOLE_SIZE };
    V.vkInvalidateMappedMemoryRanges(V.dev, 1, &mr);
    CNT(bad3, z);
    #undef CNT
    S("  [%s] %s; CPU read after the fence: %ld of %zu words wrong (%ld zero); GPU copy-out samples: %ld wrong; CPU re-read later: %ld wrong; after vkInvalidateMappedMemoryRanges: %ld wrong",
      item, vkc_result_str(r), bad1, nw, z1, gbad, bad2, bad3);
    R(item, (!r && bad1 == 0) ? "PASS" : "FAIL", "GPU %s into a fresh %s allocation%s: CPU sees %ld wrong words right after the fence (%ld zero), %ld on a later re-read, %ld after an invalidate; GPU itself sees %ld wrong",
      mode ? "compute writes" : "vkCmdFillBuffer", kind, touch_first ? " (CPU read every page first)" : " (CPU never touched the written part)", bad1, z1, bad2, bad3, gbad);
    free(exp);
    gmap_destroy(&g);
}
// (5c) CPU page faults after the driver's VMA was split by mprotect. The GPU writes unique words into a fresh allocation; the CPU has not touched
// it. Then one page in the middle is mprotected (variant 1: left read-only, so the VMA stays split in three; variant 2: protected and released,
// the kernel may merge the VMA back; variant 0: no mprotect). Then the CPU reads every word. When a read is wrong, the GPU copy of the whole
// object locates where the CPU's value really is (offset error = what the fault handler mapped instead).
static void split_one(const char *kind, int placed, int variant) {
    const size_t len = 16 * MB, half = 8 * MB; const uint32_t C = 0x13579BDFu; char item[64];
    static const char *vn[] = { "none", "ro_page_kept", "ro_then_rw", "prefaulted_then_split", "cpu_write_after_split", "populated_then_split", "populated_split_dontneed", "populated_split_free_pageout", "populated_split_dontneed_only" };
    snprintf(item, sizeof item, "split.%s.%s", kind, vn[variant]);
    gmap_t g;
    if (gmap_create(&g, len + MB, placed ? g_rb + 64 * MB : NULL, 0)) { R(item, "FAIL", "could not create the mapping"); gmap_destroy(&g); return; }
    uint32_t seed = rnd();
    VkResult r = vkc_cmd_begin(&V);   // src = C everywhere (GPU fill), then the kernel writes dst[k] = C ^ xform(seed, k): unique words, all GPU-written
    V.vkCmdFillBuffer(V.cmd, g.buf, 0, half, C);
    VkMemoryBarrier hb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
    V.vkCmdPipelineBarrier(V.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    if (!r) r = vkc_cmd_submit_wait(&V, 5000000000ull);
    if (!r) r = run_kernel(&g, 0, 0, half, len, (uint32_t)(half / 4), seed, 0);
    uint8_t *pg = g.ptr + 2 * MB + 0x1000 * 5;   // a page inside the source half
    int rc = 0;
    char pre[160] = "";
    if (variant == 3) {   // the mitigation: fault every page in first (one CPU read per page)
        double t = now_s();
        for (size_t o = 0; o < len + MB; o += g_pg) g_sink += *(volatile uint8_t *)(g.ptr + o);
        t = now_s() - t; snprintf(pre, sizeof pre, "; prefault by touch %.1f us/page, %.0f ms/GB", t * 1e6 / ((len + MB) / g_pg), t * 1e3 * (double)GB / (len + MB));
    }
    if (variant >= 5) {   // the same through madvise(MADV_POPULATE_WRITE) (Linux 5.14+)
        double t = now_s(); int mr = madvise(g.ptr, len + MB, 23 /* MADV_POPULATE_WRITE */), me = mr ? errno : 0; t = now_s() - t;
        snprintf(pre, sizeof pre, "; madvise(MADV_POPULATE_WRITE) rc %d errno %d (%s), %.0f ms/GB", mr, me, me ? strerror(me) : "ok", t * 1e3 * (double)GB / (len + MB));
        if (mr) { R(item, "FAIL", "madvise(MADV_POPULATE_WRITE) on the driver mapping failed%s", pre); gmap_destroy(&g); return; }
    }
    if (variant >= 1) rc = mprotect(pg, g_pg, PROT_READ);
    if (variant == 2 && !rc) rc = mprotect(pg, g_pg, PROT_READ | PROT_WRITE);
    if (variant == 6) {   // does the mitigation survive the pages being dropped? (MADV_DONTNEED / MADV_PAGEOUT on the GPU-written half)
        int rd = madvise(g.ptr + half, half, MADV_DONTNEED), ed = rd ? errno : 0, rp = madvise(g.ptr + half, half, 21 /* MADV_PAGEOUT */), ep = rp ? errno : 0;
        size_t u = strlen(pre); snprintf(pre + u, sizeof pre - u, "; after the split MADV_DONTNEED rc %d errno %d, MADV_PAGEOUT rc %d errno %d", rd, ed, rp, ep);
    }
    if (variant == 7) {   // MADV_FREE (as UnmapPhysicalMemory does, dm/host/hle_svc.c) and MADV_PAGEOUT only
        int rf = madvise(g.ptr + half, half, MADV_FREE), ef = rf ? errno : 0, rp = madvise(g.ptr + half, half, 21 /* MADV_PAGEOUT */), ep = rp ? errno : 0;
        size_t u = strlen(pre); snprintf(pre + u, sizeof pre - u, "; after the split MADV_FREE rc %d errno %d, MADV_PAGEOUT rc %d errno %d", rf, ef, rp, ep);
    }
    if (variant == 8) {
        int rd = madvise(g.ptr + half, half, MADV_DONTNEED), ed = rd ? errno : 0;
        size_t u = strlen(pre); snprintf(pre + u, sizeof pre - u, "; after the split MADV_DONTNEED rc %d errno %d", rd, ed);
    }
    range_info_t ri; range_info((uintptr_t)g.ptr, (uintptr_t)g.ptr + len + MB, &ri);
    volatile uint32_t *d = (volatile uint32_t *)(g.ptr + half); size_t nw = half / 4;
    if (variant == 4) {   // a CPU write into an untouched page after the split: where does it land in the object?
        size_t woff = half + 100 * g_pg + 0x40; const uint32_t M = 0xDEADBEEFu;
        *(volatile uint32_t *)(g.ptr + woff) = M;
        uint32_t *all = malloc(len + MB); long at = -2;
        if (all && !gpu_copy_out(g.mem, len + MB, 0, len + MB, all)) { at = -1; for (size_t j = 0; j < (len + MB) / 4; j++) if (all[j] == M) { at = (long)j * 4; break; } }
        free(all);
        S("  [%s] %d VMA lines; CPU wrote %#x at object offset %#zx through the mapping; the GPU finds it at %#lx", item, ri.nlines, M, woff, at);
        R(item, at == (long)woff ? "PASS" : "FAIL", "CPU store to an untouched page after a VMA split: meant for object offset %#zx, landed at %#lx (delta %+ld KB)%s", woff, at,
          at >= 0 ? (at - (long)woff) / 1024 : 0, at == (long)woff ? "" : " -- SILENT CORRUPTION of another part of the object");
        mprotect(pg, g_pg, PROT_READ | PROT_WRITE);
        gmap_destroy(&g);
        return;
    }
    long bad = 0, zero = 0; size_t first_bad = (size_t)-1;
    for (size_t k = 0; k < nw; k++) { uint32_t x = d[k]; if (x != (C ^ xform(seed, (uint32_t)k))) { bad++; zero += x == 0; if (first_bad == (size_t)-1) first_bad = k; } }
    long srcbad = 0; for (size_t k = 0; k < half / 4; k++) srcbad += ((volatile uint32_t *)g.ptr)[k] != C;
    char where[256] = "";
    if (bad) {   // where does the CPU's value at the first wrong dst words live in the object, according to the GPU?
        uint32_t *all = malloc(len + MB);
        if (all && !gpu_copy_out(g.mem, len + MB, 0, len + MB, all)) {
            size_t u = 0;
            for (size_t k = first_bad, found = 0; k < nw && found < 3; k++) {
                uint32_t x = d[k]; if (x == (C ^ xform(seed, (uint32_t)k)) || x == 0 || x == C) continue;
                long at = -1; for (size_t j = 0; j < (len + MB) / 4; j++) if (all[j] == x) { at = (long)j; break; }
                u += snprintf(where + u, sizeof where - u, "%s cpu off %#zx -> object off %#lx (delta %+ld KB)", found ? ";" : "", half + k * 4, at < 0 ? -1L : at * 4L, at < 0 ? 0 : (at * 4L - (long)(half + k * 4)) / 1024);
                found++;
            }
        }
        free(all);
    }
    S("  [%s] mprotect rc %d; %d VMA lines:", item, rc, ri.nlines); S_lines("    ", ri.lines);
    S("  [%s] CPU read of GPU-written words: %ld of %zu wrong (%ld zero), first at word %zd; source half %ld wrong; %s", item, bad, nw, zero, bad ? (ssize_t)first_bad : -1, srcbad, where);
    if (bad) for (size_t k = first_bad, i = 0; k < nw && i < 4; k++, i++)
        S("    word %zu: got %08x want %08x; got^C %08x, xform %08x, src %08x", k, d[k], C ^ xform(seed, (uint32_t)k), d[k] ^ C, xform(seed, (uint32_t)k), ((volatile uint32_t *)g.ptr)[k]);
    static const char *vd[] = { "no mprotect", "a VMA split (one page kept RO)", "mprotect RO + RW of one page", "every page faulted in, then a split", "", "MADV_POPULATE_WRITE, then a split",
                                "MADV_POPULATE_WRITE, a split, then MADV_DONTNEED + MADV_PAGEOUT", "MADV_POPULATE_WRITE, a split, then MADV_FREE + MADV_PAGEOUT",
                                "MADV_POPULATE_WRITE, a split, then MADV_DONTNEED" };
    R(item, (!r && !rc && bad == 0 && srcbad == 0) ? "PASS" : "FAIL", "%s; CPU reads after %s: %ld of %zu GPU-written words wrong%s%s%s", vkc_result_str(r), vd[variant], bad, nw, pre, bad ? " -- " : "", where);
    if (variant == 1 || variant == 3 || variant >= 5) mprotect(pg, g_pg, PROT_READ | PROT_WRITE);
    gmap_destroy(&g);
}
static void fresh_suite(void) {
    S("== (5b) GPU writes into memory the CPU has not touched yet");
    for (int v = 0; v < 9; v++) { if (g_placed_ok) split_one("placed", 1, v); split_one("mapped", 0, v); }
    for (int mode = 0; mode < 2; mode++) {
        if (g_placed_ok) { fresh_one("placed", 1, 0, mode); fresh_one("placed", 1, 1, mode); }
        fresh_one("mapped", 0, 0, mode); fresh_one("mapped", 0, 1, mode);
    }
}

// ---------------------------------------------------------------- (6) dirty tracking on driver pages
static int store_expect_fault(uint8_t *a, uint32_t v, int expect_n_before) {
    int ok;
    GUARD_BEGIN(ok) *(volatile uint32_t *)a = v; GUARD_END(ok);
    if (!ok) S("    store at %p: unexpected fault sig %d code %d addr %#lx", a, g_guard_sig, g_guard_code, (unsigned long)g_guard_addr);
    return ok && g_trk_n == expect_n_before + 1 && g_trk_addr[expect_n_before] == ((uintptr_t)a & 0x00ffffffffffffffull) && g_trk_code[expect_n_before] == SEGV_ACCERR;
}
static void dirty_on(gmap_t *g, const char *kind);
static void dirty_suite(const char *kind, int placed) {
    S("== (6) mprotect + write-fault dirty tracking on %s memory", kind);
    if (kernels_init()) { R("dirty.pipeline", "FAIL", "compute pipeline creation failed"); return; }
    gmap_t g; char item[48];
    if (gmap_create(&g, 64 * MB, placed ? g_rb + 64 * MB : NULL, 0)) { snprintf(item, sizeof item, "dirty.%s", kind); R(item, "FAIL", "could not create the mapping"); gmap_destroy(&g); return; }
    dirty_on(&g, kind);
    gmap_destroy(&g);
}
// The test on an existing 64 MB GPU-visible mapping (g->ptr, g->buf); the caller creates and destroys it
static void dirty_on(gmap_t *gp, const char *kind) {
    gmap_t g = *gp; char item[48];
    const size_t SRCB = 0, DSTB = 32 * MB, RESB = 60 * MB, REG = 16 * MB; const uint32_t NW = REG / 4;
    uint8_t *base = g.ptr; uint32_t *shadow = malloc(REG);
    uint32_t seedA = rnd();
    for (size_t o = 0; o < g.size; o += g_pg) g_sink += *(volatile uint8_t *)(base + o);   // fault in every page before any mprotect (see the split tests: KGSL maps the wrong page on a first touch after a VMA split)
    for (uint32_t k = 0; k < NW; k++) { shadow[k] = pattern(seedA, k); ((volatile uint32_t *)base)[k] = shadow[k]; }
    size_t P = g_pg; size_t tp = 1000;
    uint8_t *page = base + tp * P;
    int rc = mprotect(page, P, PROT_READ); int e = errno;
    range_info_t ri; range_info((uintptr_t)page - P, (uintptr_t)page + 2 * P, &ri);
    S("  mprotect(%p, %zu, PROT_READ) = %d errno %d; maps around it:", page, P, rc, rc ? e : 0); S_lines("    ", ri.lines);
    snprintf(item, sizeof item, "dirty.%s.mprotect", kind);
    R(item, rc == 0 ? "PASS" : "FAIL", "mprotect RO on a driver page in the middle of the mapping: rc %d errno %d, VMA lines around it %d", rc, rc ? e : 0, ri.nlines);
    if (rc) { free(shadow); return; }
    int okr; volatile long rbad = 0;
    GUARD_BEGIN(okr) for (size_t i = 0; i < P / 4; i++) if (((volatile uint32_t *)page)[i] != shadow[tp * P / 4 + i]) rbad++; GUARD_END(okr);
    S("  read under PROT_READ: %s, %ld words differ from what was written", okr ? "no fault" : "FAULT", rbad);
    // arm the tracker over the source region
    g_trk_n = 0; g_trk_mperr = 0; g_trk_lo = (uintptr_t)base; g_trk_hi = (uintptr_t)base + REG;
    uint32_t V1 = 0xD1A7F00Du; size_t off = 0x124;
    int f1 = store_expect_fault(page + off, V1, 0); shadow[(tp * P + off) / 4] = V1;
    int landed = *(volatile uint32_t *)(page + off) == V1;
    int intact = !memcmp((void *)page, shadow + tp * P / 4, P);
    int f2ok; GUARD_BEGIN(f2ok) *(volatile uint32_t *)(page + off + 4) = V1 + 1; GUARD_END(f2ok); shadow[(tp * P + off + 4) / 4] = V1 + 1;
    int no_second = f2ok && g_trk_n == 1;
    S("  first write: %s (faults %d, addr %#lx expected %p, code %d, mprotect in handler errno %d); value landed %d, rest of page intact %d; second write without a fault %d",
      f1 ? "one fault at the exact address" : "WRONG", g_trk_n, (unsigned long)g_trk_addr[0], page + off, g_trk_code[0], g_trk_mperr, landed, intact, no_second);
    // 8 consecutive pages protected by one call, then written one by one
    size_t p8 = tp + 10; int seq_ok = 1;
    if (mprotect(base + p8 * P, 8 * P, PROT_READ)) { seq_ok = 0; S("  mprotect 8 pages: errno %d", errno); }
    range_info((uintptr_t)base + (p8 - 1) * P, (uintptr_t)base + (p8 + 9) * P, &ri); int vma8 = ri.nlines;
    for (int i = 0; i < 8 && seq_ok; i++) {
        uint8_t *a = base + (p8 + i) * P + 0x40 + 8 * i; uint32_t v = 0xC0DE0000u + i;
        int n0 = g_trk_n; if (!store_expect_fault(a, v, n0)) { seq_ok = 0; S("  consecutive page %d: wrong fault record (n %d)", i, g_trk_n); }
        shadow[(size_t)(a - base) / 4] = v;
    }
    // alternating pages: 32 separate mprotect calls -> many VMAs, then write all 64 pages
    size_t pa = tp + 100; int alt_ok = 1;
    for (int i = 0; i < 64; i += 2) if (mprotect(base + (pa + i) * P, P, PROT_READ)) alt_ok = 0;
    range_info((uintptr_t)base + pa * P, (uintptr_t)base + (pa + 64) * P, &ri); int vma_alt = ri.nlines;
    int n_alt0 = g_trk_n, okw;
    GUARD_BEGIN(okw) for (int i = 0; i < 64; i++) { uint8_t *a = base + (pa + i) * P + 0x80; *(volatile uint32_t *)a = 0xA17E0000u + i; } GUARD_END(okw);
    for (int i = 0; i < 64; i++) shadow[((pa + i) * P + 0x80) / 4] = 0xA17E0000u + i;
    int alt_faults = g_trk_n - n_alt0;
    for (int i = 0; i < 64 && alt_ok; i += 2) if (g_trk_addr[n_alt0 + i / 2] != (uintptr_t)(base + (pa + i) * P + 0x80)) alt_ok = 0;
    alt_ok = alt_ok && okw && alt_faults == 32;
    // PROT_NONE and back: contents kept
    size_t pn = tp + 300; int none_ok = !mprotect(base + pn * P, P, PROT_NONE) && !mprotect(base + pn * P, P, PROT_READ | PROT_WRITE);
    int none_kept = none_ok && !memcmp(base + pn * P, shadow + pn * P / 4, P);
    S("  8 consecutive pages (one mprotect, %d VMA lines): %s; 32 alternate pages (%d VMA lines): %d faults %s; PROT_NONE -> RW keeps data: %d",
      vma8, seq_ok ? "8 faults at the exact addresses" : "WRONG", vma_alt, alt_faults, alt_ok ? "at the exact addresses" : "WRONG", none_kept);
    // fault cost: 1024 pages protected by one call, one write each
    size_t pt = 2048; int okt = 0; double tt = 0; int n_t0 = g_trk_n;
    if (!mprotect(base + pt * P, 1024 * P, PROT_READ)) {
        double t0 = now_s();
        GUARD_BEGIN(okt) for (int i = 0; i < 1024; i++) { uint8_t *a = base + (pt + i) * P; *(volatile uint32_t *)a = 0x7177u + i; } GUARD_END(okt);
        tt = now_s() - t0;
        for (int i = 0; i < 1024; i++) shadow[(pt + i) * P / 4] = 0x7177u + i;
    }
    int n_timed = g_trk_n - n_t0; if (!okt) S("  fault-cost loop: unexpected fault sig %d at %#lx", g_guard_sig, (unsigned long)g_guard_addr);
    S("  fault cost: %d faults over 1024 pages, %.2f us per fault (signal + mprotect + retry)", n_timed, n_timed ? tt * 1e6 / n_timed : 0);
    g_trk_lo = g_trk_hi = 0;
    // GPU reads the region: it must see every CPU write, including the ones that went through the fault path
    int gpu_read_ok = 0, gpu_write_ok = 0; uint32_t S1 = rnd(), S2 = rnd();
    VkResult r = run_kernel(&g, 0, SRCB, DSTB, RESB, NW, S1, 0);
    if (!r) {
        long bad = 0, zero = 0, eqsrc = 0; volatile uint32_t *dv = (volatile uint32_t *)(base + DSTB), *sv = (volatile uint32_t *)base;
        for (uint32_t k = 0; k < NW; k++) {
            uint32_t got = dv[k], exp = shadow[k] ^ xform(S1, k);
            if (got != exp) { bad++; zero += got == 0; eqsrc += got == sv[k]; if (bad <= 4) S("    dst[%u] = %#x expected %#x (shadow %#x, src now %#x)", k, got, exp, shadow[k], sv[k]); }
        }
        gpu_read_ok = bad == 0; S("  GPU read of the region after the faults: %ld words wrong (%ld zero, %ld equal to src)", bad, zero, eqsrc);
    }
    else S("  GPU read dispatch: %s", vkc_result_str(r));
    // GPU writes into the region (the pages that were protected and released): the CPU mapping must still be the GPU's memory
    r = run_kernel(&g, 0, DSTB, SRCB, RESB, NW, S2, 0);
    if (!r) { long bad = 0; for (uint32_t k = 0; k < NW; k++) if (((volatile uint32_t *)base)[k] != (shadow[k] ^ xform(S1, k) ^ xform(S2, k))) bad++; gpu_write_ok = bad == 0;
              S("  CPU read of GPU writes into the formerly protected pages: %ld words wrong", bad); }
    else S("  GPU write dispatch: %s", vkc_result_str(r));
    snprintf(item, sizeof item, "dirty.%s", kind);
    int pass = f1 && landed && intact && no_second && seq_ok && alt_ok && none_kept && okr && rbad == 0 && gpu_read_ok && gpu_write_ok;
    R(item, pass ? "PASS" : "FAIL", "mapping pre-populated before any mprotect (required: see split.*); 1 fault at the exact address %s, write landed %s, page kept %s, 8 consecutive %s, 32 alternate %s, PROT_NONE->RW kept %s, GPU sees writes %s, CPU sees GPU writes on released pages %s, %.2f us/fault",
      f1 ? "yes" : "NO", landed ? "yes" : "NO", intact ? "yes" : "NO", seq_ok ? "ok" : "BAD", alt_ok ? "ok" : "BAD", none_kept ? "yes" : "NO",
      gpu_read_ok ? "yes" : "NO", gpu_write_ok ? "yes" : "NO", n_timed ? tt * 1e6 / n_timed : 0);
    free(shadow);
}

// ---------------------------------------------------------------- main
// ---------------------------------------------------------------- (6b) do split VMAs merge back? (dirty-tracking cycles at scale)
// The dirty tracker (nvn_dirty.c) protects a range read-only, unprotects single pages as they fault, and re-protects the range later.
// On anonymous memory the kernel merges the pieces back into one VMA. If driver mappings never merge, every page boundary ever
// toggled stays a VMA boundary for good, and a process has at most vm.max_map_count VMAs (65530 by default).
static int count_vmas(uintptr_t lo, uintptr_t hi, int *total) {
    FILE *f = fopen("/proc/self/maps", "r"); int n = 0, t = 0; if (!f) { *total = -1; return -1; }
    char line[512];
    while (fgets(line, sizeof line, f)) { t++; unsigned long a, b; if (sscanf(line, "%lx-%lx", &a, &b) == 2 && a < hi && b > lo) n++; }
    fclose(f); *total = t; return n;
}
static void vmas_one(const char *kind, int which, int npages) {
    const size_t len = 64 * MB; char item[48]; snprintf(item, sizeof item, "vmas.%s", kind);
    gmap_t g; memset(&g, 0, sizeof g); uint8_t *p;
    if (which == 0) { p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); if (p == MAP_FAILED) { R(item, "FAIL", "mmap"); return; } }
    else { if (gmap_create(&g, len, which == 2 ? g_rb + 64 * MB : NULL, 0)) { R(item, "FAIL", "could not create the mapping"); gmap_destroy(&g); return; } p = g.ptr; }
    int mr = madvise(p, len, 23 /* MADV_POPULATE_WRITE */);   // the mitigation for the split-fault bug, as strategy A would do it
    if (mr) for (size_t o = 0; o < len; o += g_pg) g_sink += *(volatile uint8_t *)(p + o);
    uint64_t seed = 0x5eedull + (uint64_t)which;
    touch_pages(p, len, (uintptr_t)p, seed, 1);
    uintptr_t lo = (uintptr_t)p, hi = lo + len; int tot0, tot;
    int v0 = count_vmas(lo, hi, &tot0), fails = 0, e = 0;
    if (mprotect(p, len, PROT_READ)) { fails++; e = errno; }
    int v1 = count_vmas(lo, hi, &tot);
    double t0 = now_s();
    for (int i = 0; i < npages; i++) if (mprotect(p + (size_t)(4 * i + 1) * g_pg, g_pg, PROT_READ | PROT_WRITE)) { fails++; e = errno; }   // as the fault handler does
    double t1 = now_s();
    int v2 = count_vmas(lo, hi, &tot); int tot2 = tot;
    double t2 = now_s(); if (mprotect(p, len, PROT_READ)) { fails++; e = errno; } double t3 = now_s();   // re-arm the whole range
    int v3 = count_vmas(lo, hi, &tot);
    if (mprotect(p, len, PROT_READ | PROT_WRITE)) { fails++; e = errno; }
    int v4 = count_vmas(lo, hi, &tot);
    long cbad = touch_pages(p, len, (uintptr_t)p, seed, 0), gbad = which ? gpu_check_pages(g.mem, len, (uintptr_t)p, seed) : 0;
    S("  [%s] %d pages unprotected one by one inside a read-only %zu MB range: VMAs in the range %d -> RO %d -> after unprotects %d (process total %d) -> whole range RO again %d -> whole range RW %d",
      item, npages, len / MB, v0, v1, v2, tot2, v3, v4);
    S("  [%s] %.2f us per single-page mprotect, %.2f ms to re-protect the whole range; %d mprotect failures (errno %d); data after: CPU %ld bad pages, GPU %ld bad pages; process VMAs at start %d",
      item, (t1 - t0) * 1e6 / npages, (t3 - t2) * 1e3, fails, e, cbad, gbad, tot0);
    int merged = v4 <= 2;
    R(item, which == 0 ? "INFO" : (merged ? "PASS" : "FAIL"), "after %d single-page unprotects and a full re-protect + RW, the %zu MB range is %d VMA(s)%s; data CPU %ld / GPU %ld bad pages; mprotect failures %d",
      npages, len / MB, v4, merged ? " (merged back)" : " -- the pieces NEVER merge: each toggled page boundary stays a VMA (vm.max_map_count)", cbad, gbad, fails);
    if (which == 0) munmap(p, len); else gmap_destroy(&g);
}
static void vmas_suite(void) {
    char mm[64] = "unreadable (default 65530)"; FILE *f = fopen("/proc/sys/vm/max_map_count", "r"); if (f) { if (fgets(mm, sizeof mm, f)) mm[strcspn(mm, "\n")] = 0; fclose(f); }
    S("== (6b) do split VMAs merge back? vm.max_map_count %s", mm);
    vmas_one("anon", 0, 4096);
    vmas_one("mapped", 1, 4096);
    if (g_placed_ok) vmas_one("placed", 2, 4096);
}

#include "ahb_probe.c"

static const char *arg(int argc, char **argv, const char *name, const char *def) {
    for (int i = 0; i + 1 < argc; i++) if (!strcmp(argv[i], name)) return argv[i + 1];
    return def;
}
static int flag(int argc, char **argv, const char *name) { for (int i = 0; i < argc; i++) if (!strcmp(argv[i], name)) return 1; return 0; }
static char g_tests[256];
static int test_on(const char *t) { char k[64]; snprintf(k, sizeof k, ",%s,", t); return strstr(g_tests, k) != NULL; }

static void prop_cb(void *cookie, const char *name, const char *value, uint32_t serial) { (void)name; (void)serial; snprintf((char *)cookie, 256, "%s", value); }

int probe_main(const char *out_dir, int in_app, int argc, char **argv) {
    g_in_app = in_app; g_pg = (size_t)sysconf(_SC_PAGESIZE);
    snprintf(g_resdir, sizeof g_resdir, "%s/res", out_dir); mkdir(g_resdir, 0755);
    char p[700]; snprintf(p, sizeof p, "%s/summary.txt", g_resdir); g_sum = fopen(p, "w");
    const char *label = arg(argc, argv, "--label", "?"), *load = arg(argc, argv, "--load", "system");
    snprintf(g_tests, sizeof g_tests, ",%s,%s", arg(argc, argv, "--tests", "caps,a,d,c,bw,kdiag,coh,fresh,dirty,vmas,types,free"), flag(argc, argv, "--heavy") ? "b," : "");
    int reps = atoi(arg(argc, argv, "--bw-reps", "32"));
    double max_gb = atof(arg(argc, argv, "--max-total-gb", "6")), floor_gb = atof(arg(argc, argv, "--floor-gb", "1.5"));
    g_rng ^= (uint32_t)(now_s() * 1e6) ^ (uint32_t)getpid() * 2654435761u;
    g_ahb_floor = atof(arg(argc, argv, "--ahb-floor-gb", "2")); snprintf(g_outdir, sizeof g_outdir, "%s", out_dir);
    int ahb_many_max = atoi(arg(argc, argv, "--ahb-many-max", "36000")), ahb_cycles = atoi(arg(argc, argv, "--ahb-churn-cycles", "100"));

    char ctx[256] = "?"; FILE *c = fopen("/proc/self/attr/current", "r"); if (c) { if (fgets(ctx, sizeof ctx, c)) ctx[strcspn(ctx, "\n")] = 0; fclose(c); }
    struct utsname un; uname(&un);
    char fp[256] = "", gd[PROP_VALUE_MAX] = "", hv[PROP_VALUE_MAX] = "", model[PROP_VALUE_MAX] = "";
    const prop_info *pi = __system_property_find("ro.build.fingerprint");
    if (pi) __system_property_read_callback(pi, prop_cb, fp);
    __system_property_get("ro.gfx.driver.1", gd); __system_property_get("ro.hardware.vulkan", hv); __system_property_get("ro.product.model", model);
    S("gpu probe: label %s, context %s, uid %d, pid %d, selinux %s, page %zu, kernel %s", label, in_app ? "app" : "exe", (int)getuid(), (int)getpid(), ctx, g_pg, un.release);
    S("device %s; build %s; ro.hardware.vulkan=%s ro.gfx.driver.1=%s; MemAvailable %.2f GB; tests [%s]", model, fp, hv, gd, avail_gb(), g_tests);
    for (int i = 0; i < argc; i++) S("arg[%d] %s", i, argv[i]);
    { int fd_ok = access("/dev/kgsl-3d0", R_OK | W_OK); S("/dev/kgsl-3d0 access: %s", fd_ok ? strerror(errno) : "rw ok"); }
    { char sc[256]; sched_ctx(sc, sizeof sc); S("scheduling at start: %s", sc); }

    char selfdir[512] = ""; vkc_self_dir(selfdir, sizeof selfdir);
    const char *dd = arg(argc, argv, "--drvdir", ""); char ddabs[700];
    if (*dd && *dd != '/') { snprintf(ddabs, sizeof ddabs, "%s/%s", out_dir, dd); dd = ddabs; }   // relative: against the files directory
    vkc_load_opts_t lo = { load, selfdir, dd, arg(argc, argv, "--drvname", "") };
    char err[512] = "";
    if (vkc_load(&V, &lo, err, sizeof err)) { S("LOAD FAILED: %s (%s)", err, V.load_desc); R("load", "FAIL", "%s", err); return 2; }
    S("loaded: %s", V.load_desc);
    if (vkc_instance(&V, VK_API_VERSION_1_4, err, sizeof err)) { S("INSTANCE FAILED: %s", err); R("load", "FAIL", "%s", err); return 2; }
    for (char *q = V.drv.driverInfo; *q; q++) if (*q == '\n' || *q == '\r') *q = ';';   // one line in the logs
    if (make_device(err, sizeof err)) { S("DEVICE FAILED: %s", err); R("load", "FAIL", "%s", err); return 2; }
    vkc_update_new_libs(&V);
    S("shared objects mapped since before loading:"); S_lines("    ", V.new_libs);
    maps_snapshot("maps_after_device.txt");

    // which driver is this, really
    const char *xl = arg(argc, argv, "--expect-lib", NULL), *xi = arg(argc, argv, "--expect-info", NULL), *xid = arg(argc, argv, "--expect-id", NULL);
    int id_ok = 1; char idw[512] = "";
    if (xl) { char all[40000]; snprintf(all, sizeof all, "%s%s", V.maps_before, V.new_libs); if (!strstr(all, xl)) { id_ok = 0; snprintf(idw + strlen(idw), sizeof idw - strlen(idw), " lib %s not mapped;", xl); } }
    if (xid && atoi(xid) != (int)V.drv.driverID) { id_ok = 0; snprintf(idw + strlen(idw), sizeof idw - strlen(idw), " driverID %d != %s;", V.drv.driverID, xid); }
    if (xi && !strstr(V.drv.driverInfo, xi) && !strstr(V.drv.driverName, xi)) { id_ok = 0; snprintf(idw + strlen(idw), sizeof idw - strlen(idw), " driverInfo \"%s\" lacks \"%s\";", V.drv.driverInfo, xi); }
    dump_caps();
    R("identity", id_ok ? "PASS" : "FAIL", "%s driverID %d \"%s\" / \"%s\"%s", label, V.drv.driverID, V.drv.driverName, V.drv.driverInfo, idw);
    if (!id_ok) { S("IDENTITY MISMATCH: not the expected driver, memory tests not run"); return 3; }

    choose_type();
    if (g_type == UINT32_MAX) { R("mem.type", "FAIL", "no host-visible memory type for storage buffers"); return 2; }
    install_fault_handler();
    fault_selftest();
    g_res5_len = 5 * GB;
    g_res5 = mmap(NULL, g_res5_len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);   // as mem_init (dm/host/hle_svc.c)
    if (g_res5 == MAP_FAILED) { R("reserve", "FAIL", "mmap 5 GB PROT_NONE: errno %d", errno); return 2; }
    g_rb = (uint8_t *)(((uintptr_t)g_res5 + 2 * MB - 1) & ~(uintptr_t)(2 * MB - 1));
    S("5 GB reservation at %p (2 MB-aligned base %p)", g_res5, g_rb);

    double t0 = now_s();
    if (test_on("a")) test_a();
    if (test_on("d")) test_d();
    if (test_on("c")) test_c();
    if (test_on("b")) test_b();
    if (test_on("bw")) bw_suite(reps < 5 ? 5 : reps);
    if (test_on("kdiag")) kdiag_suite(0, -1);
    if (test_on("coh")) { if (g_placed_ok) coherence_suite("placed", 1, 0); coherence_suite("mapped", 0, 0); }
    if (test_on("fresh")) { if (kernels_init()) R("fresh.pipeline", "FAIL", "compute pipeline creation failed"); else fresh_suite(); }
    if (test_on("dirty")) { if (g_placed_ok) dirty_suite("placed", 1); dirty_suite("mapped", 0); }
    if (test_on("types")) types_bw();
    if (test_on("vmas")) vmas_suite();
    if (test_on("free")) test_free();
    if (test_on("e")) test_e(max_gb, floor_gb);
    if (test_on("ahb")) ahb_suite();
    if (test_on("ahbbw")) ahb_bw(reps < 5 ? 5 : reps);
    if (test_on("ahbsize")) ahb_size();
    if (test_on("ahbmany")) ahb_many(ahb_many_max);
    if (test_on("ahbchurn")) ahb_churn(ahb_cycles);
    // kernels using buffer device addresses last: a driver that loses the device there does not void the results above
    if (g_bda && test_on("kdiag")) kdiag_suite(1, 0);
    if (g_bda && test_on("coh")) { if (g_placed_ok) coherence_suite("placed", 1, 1); coherence_suite("mapped", 0, 1); }
    if (g_bda && test_on("kdiag")) kdiag_suite(1, 1);
    maps_snapshot("maps_end.txt");
    S("tests took %.1f s; MemAvailable %.2f GB", now_s() - t0, avail_gb());

    FILE *rt = open_res("results.tsv");
    S("==== results (%s, %s)", label, in_app ? "app" : "exe");
    for (int i = 0; i < g_nres; i++) { S("%-28s %-4s %s", g_results[i].item, g_results[i].verdict, g_results[i].detail); if (rt) fprintf(rt, "%s\t%s\t%s\n", g_results[i].item, g_results[i].verdict, g_results[i].detail); }
    if (rt) fclose(rt);
    munmap(g_res5, g_res5_len);
    kern_t *all[6] = { &g_kd, &g_kb, &g_dk[0], &g_dk[1], &g_dk[2], &g_dk[3] };
    for (int i = 0; i < 6; i++) if (all[i]->ok) vkc_compute_destroy(&V, &all[i]->c);
    vkc_destroy(&V);
    if (g_sum) fclose(g_sum);
    return 0;
}
