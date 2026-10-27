// AHardwareBuffer-backed guest memory pools: zero-copy GPU buffers on a driver without VK_EXT_map_memory_placed (results: docs/android/probes-gpu.md).
// The method comes from shirai91/nx2android (MIT): probes/vkzc.c, pool_zero_copy in nxnative/dm/android/gfx_vk_core.c, and its defragmenting remap
// (nvn_dirty_defrag in nxnative/dm/gfx/nvn/nvn_dirty.c): allocate a BLOB AHardwareBuffer, take its dma-buf fd through AHardwareBuffer_sendHandleToUnixSocket,
// mmap it MAP_SHARED|MAP_FIXED over the pool's guest range, and import the same AHB into Vulkan as a dedicated VkBuffer. The test code here is our own.
// Included by gpu_probe.c (it uses that file's static helpers and Vulkan state); it is not a translation unit of its own.
// Tests (--tests):
//   ahb       the checks that could rule the method out: import capabilities and memory types; the chain over a PROT_NONE reservation and over a live RW
//             heap range (pool init: copy out, map, copy back; finalize: back to anonymous memory with the contents); page-table population; the host's
//             memory paths on the mapping (smaps VmFlags, process_vm_readv, write() into a pipe, futex, read()/write() of files, LDXR/STXR and LSE
//             atomics), with anonymous and KGSL (vkMapMemory) controls; GPU-written contents by offset after VMA splits, after a remap and at a nonzero fd
//             offset; madvise; VMA merging and the remap defrag; 4,096 write faults; the dirty-tracking test (6); no-flush coherence (descriptors and
//             BDA); GPU writes into untouched memory; 256 small pools
//   ahbbw     CPU bandwidth of the AHB mapping against anonymous memory (paired, as bw)
//   ahbsize   pool sizes up to AHB_CAP (64 MB), checked page by page; nothing larger is tried (see AHB_CAP) (APK only)
//   ahbmany   small pools until a step fails (APK only)
//   ahbchurn  pool init / finalize / free cycles: is the memory given back (APK only)
// Imported memory is never vkMapMemory'd, flushed or invalidated here: the CPU reaches it only through the guest-address mapping. No DMA_BUF_IOCTL_SYNC.
#include <android/hardware_buffer.h>
#include <vulkan/vulkan_android.h>
#include <asm/hwcap.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sys/auxv.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>

#ifndef RUSAGE_THREAD
#define RUSAGE_THREAD 1
#endif

static int g_ahb_want, g_ahb_ok, g_ahb_qff;
static PFN_vkGetAndroidHardwareBufferPropertiesANDROID g_ahb_props_fn;
static PFN_vkGetPhysicalDeviceExternalBufferProperties g_ext_buf_fn;
static char g_outdir[600];
static double g_ahb_floor = 2.0;   // GB of MemAvailable left when the heavy tests allocate (--ahb-floor-gb)
// what a pool's VkBuffer may be used for (vertex, index, uniform, storage, indirect, copies), as the backend's pool buffers
static const VkBufferUsageFlags k_ahb_usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

static int ahb_tests_requested(void) { return test_on("ahb") || test_on("ahbbw") || test_on("ahbsize") || test_on("ahbmany") || test_on("ahbchurn"); }
// make_device: the AHB extension (and VK_EXT_queue_family_foreign, which it requires) only when an ahb test runs, so the other tests' device is unchanged
static void ahb_add_exts(const char **exts, uint32_t *ne) {
    g_ahb_want = ahb_tests_requested();
    if (!g_ahb_want || !vkc_has_ext(&V, "VK_ANDROID_external_memory_android_hardware_buffer")) return;
    exts[(*ne)++] = "VK_ANDROID_external_memory_android_hardware_buffer";
    if ((g_ahb_qff = vkc_has_ext(&V, "VK_EXT_queue_family_foreign"))) exts[(*ne)++] = "VK_EXT_queue_family_foreign";
}
static void ahb_after_device(void) {
    if (!g_ahb_want) return;
    g_ahb_props_fn = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)V.vkGetDeviceProcAddr(V.dev, "vkGetAndroidHardwareBufferPropertiesANDROID");
    g_ext_buf_fn = (PFN_vkGetPhysicalDeviceExternalBufferProperties)V.vkGetInstanceProcAddr(V.inst, "vkGetPhysicalDeviceExternalBufferProperties");
    g_ahb_ok = vkc_has_ext(&V, "VK_ANDROID_external_memory_android_hardware_buffer") && g_ahb_props_fn;
}

// ---------------------------------------------------------------- AHB pools
static inline uint32_t ov(uint32_t seed, uint32_t k) { return k ^ seed; }   // word k's value; a wrong word x came from word x ^ seed (seed has the top bits set)
static uint32_t ov_seed(void) { return rnd() | 0xC0000000u; }

typedef struct {
    AHardwareBuffer *ahb; int fd; uint8_t *at; size_t len;
    VkDeviceMemory mem; VkBuffer buf; VkDeviceAddress addr; VkDeviceSize alloc_size, buf_size; uint32_t type, type_bits, req_bits;
    off_t fd_size; int nfds; const char *step; long err; int fin; double t_alloc, t_fd, t_map, t_import, t_copy;
} apool_t;
enum { AP_KEEP = 1, AP_BDA = 2, AP_KEEP_FD = 4, AP_NO_IMPORT = 8 };

// Size cap. On the Galaxy Z Fold 7 (SM-F966B, F966BXXSDCZJ1) an AHardwareBuffer_isSupported query above 64 MB (BLOB, these usages; the 1 GB or the 2 GB one) aborted
// the system allocator service (vendor.qti.hardware.display.allocator-service: ubsan mul-overflow in snapalloc AlignmentToAlignedConstraints, reached
// from IsSupported through the same Allocate path), and surfaceflinger and the framework restarted with it. isSupported is therefore no safe
// pre-check: no descriptor above the cap goes to the allocator, from any test. 64 MB is the largest size that went through it.
#define AHB_CAP (64 * MB)
static int g_ahb_dead;   // an allocate failed as if the allocator service had died (DEAD_OBJECT -32, or -129): the remaining AHB tests stop
static int g_ahb_alive;  // pools a failed finalize left alive (AHB, fd and Vulkan objects kept, range state unknown): the remaining AHB tests stop
static int ahb_alloc_desc(uint32_t width, uint32_t height, AHardwareBuffer_Desc *d) {
    memset(d, 0, sizeof *d);
    if ((uint64_t)width * height > AHB_CAP) return -1;
    d->width = width; d->height = height; d->layers = 1; d->format = AHARDWAREBUFFER_FORMAT_BLOB;
    d->usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;
    return 0;
}
// The AHB's native handle travels as SCM_RIGHTS fds. The pool's fd is the one whose size (lseek SEEK_END) covers len; the others are closed.
static int ahb_dmabuf_fd(AHardwareBuffer *ahb, size_t len, int *nfds, off_t *fsize) {
    int sv[2], fds[16], n = 0, pick = -1; *nfds = 0; *fsize = -1;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -1;
    if (!AHardwareBuffer_sendHandleToUnixSocket(ahb, sv[0])) {
        char data[4096]; union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int) * 16)]; } ctl;
        struct iovec iov = { data, sizeof data }; struct msghdr m; memset(&m, 0, sizeof m);
        m.msg_iov = &iov; m.msg_iovlen = 1; m.msg_control = ctl.b; m.msg_controllen = sizeof ctl.b;
        if (recvmsg(sv[1], &m, MSG_CMSG_CLOEXEC) >= 0)
            for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
                    for (size_t i = 0; i < (c->cmsg_len - CMSG_LEN(0)) / sizeof(int) && n < 16; i++) fds[n++] = ((int *)CMSG_DATA(c))[i];
    }
    close(sv[0]); close(sv[1]);
    for (int i = 0; i < n; i++) { off_t s = lseek(fds[i], 0, SEEK_END); if (pick < 0 && s >= (off_t)len) { pick = fds[i]; *fsize = s; } else close(fds[i]); }
    *nfds = n;
    return pick;
}
// exp_name (the dma-buf heap) and size from /proc/self/fdinfo
static void fd_dmabuf_info(int fd, char *out, size_t n) {
    char p[64], line[256], exp[96] = "?", sz[48] = "?"; snprintf(p, sizeof p, "/proc/self/fdinfo/%d", fd);
    FILE *f = fopen(p, "r");
    if (f) { while (fgets(line, sizeof line, f)) { line[strcspn(line, "\n")] = 0;
                 if (!strncmp(line, "exp_name:", 9)) snprintf(exp, sizeof exp, "%s", line + 9 + strspn(line + 9, " \t"));
                 if (!strncmp(line, "size:", 5)) snprintf(sz, sizeof sz, "%s", line + 5 + strspn(line + 5, " \t")); }
             fclose(f); }
    char lk[128] = "?"; snprintf(p, sizeof p, "/proc/self/fd/%d", fd); ssize_t l = readlink(p, lk, sizeof lk - 1); if (l > 0) lk[l] = 0;
    snprintf(out, n, "fd -> %s, exp_name %s, size %s", lk, exp, sz);
}
static int count_fds(void) { DIR *d = opendir("/proc/self/fd"); int n = 0; if (!d) return -1; while (readdir(d)) n++; closedir(d); return n - 2; }

static void apool_vk_free(apool_t *p) {
    if (p->buf) V.vkDestroyBuffer(V.dev, p->buf, NULL);
    if (p->mem) V.vkFreeMemory(V.dev, p->mem, NULL);
    p->buf = 0; p->mem = 0; p->addr = 0;
}
static void apool_release(apool_t *p) {
    apool_vk_free(p);
    if (p->ahb) AHardwareBuffer_release(p->ahb);
    if (p->fd >= 0) close(p->fd);
    p->ahb = NULL; p->fd = -1;
}
// Imports p->ahb as a dedicated VkBuffer (size: the pool, capped at maxBufferSize). The memory type is the lowest one the AHB and the buffer both allow.
static VkResult ahb_import(apool_t *p, int bda) {
    VkAndroidHardwareBufferPropertiesANDROID ap = { VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID };
    VkResult r = g_ahb_props_fn(V.dev, p->ahb, &ap);
    if (r) { p->step = "vkGetAndroidHardwareBufferPropertiesANDROID"; return r; }
    p->alloc_size = ap.allocationSize; p->type_bits = ap.memoryTypeBits;
    VkExternalMemoryBufferCreateInfo eb = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID };
    VkDeviceSize bsz = p->len; if (g_max_buffer && bsz > g_max_buffer) bsz = g_max_buffer & ~(VkDeviceSize)4095;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, 0, bsz, k_ahb_usage | (bda ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0), VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    if ((r = V.vkCreateBuffer(V.dev, &bi, NULL, &p->buf))) { p->buf = 0; p->step = "vkCreateBuffer(external AHB)"; return r; }
    p->buf_size = bsz;
    VkMemoryRequirements req; V.vkGetBufferMemoryRequirements(V.dev, p->buf, &req); p->req_bits = req.memoryTypeBits;
    uint32_t bits = ap.memoryTypeBits & req.memoryTypeBits;
    if (!bits) { p->step = "no memory type both the AHB and the buffer allow"; return VK_ERROR_FEATURE_NOT_PRESENT; }
    p->type = (uint32_t)__builtin_ctz(bits);
    VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, p->buf };
    VkImportAndroidHardwareBufferInfoANDROID imp = { VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, &ded, p->ahb };
    VkMemoryAllocateFlagsInfo fl = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &imp, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0 };
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, bda ? (const void *)&fl : (const void *)&imp, ap.allocationSize, p->type };
    if ((r = V.vkAllocateMemory(V.dev, &ai, NULL, &p->mem))) { p->mem = 0; p->step = "vkAllocateMemory(import AHB, dedicated)"; return r; }
    if ((r = V.vkBindBufferMemory(V.dev, p->buf, p->mem, 0))) { p->step = "vkBindBufferMemory"; return r; }
    if (bda && V.GetBufferDeviceAddress) { VkBufferDeviceAddressInfo di = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, p->buf }; p->addr = V.GetBufferDeviceAddress(V.dev, &di); }
    return VK_SUCCESS;
}
// Pool finalize: the range becomes anonymous memory again, with the pool's contents when keep (copy out, anonymous MAP_FIXED, copy back), else
// PROT_NONE (a reservation); then the Vulkan objects and the AHB are freed. 0 = ok.
// -1: the contents could not be saved (malloc): nothing changed, the pool is still mapped and alive. -2: the anonymous MAP_FIXED failed: the pool's
// objects are kept (not released) and nothing is copied; the range may have lost its mapping (a failed MAP_FIXED can unmap first).
static int apool_finalize(apool_t *p, int keep) {
    if (!p->at) { apool_release(p); return 0; }
    uint8_t *save = NULL;
    if (keep) { if (!(save = malloc(p->len))) return -1; memcpy(save, p->at, p->len); }
    if (mmap(p->at, p->len, keep ? PROT_READ | PROT_WRITE : PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != p->at) { free(save); return -2; }
    if (save) { memcpy(p->at, save, p->len); free(save); }
    apool_release(p); p->at = NULL;
    return 0;
}
// Pool init: [at, at + len) becomes an AHB's memory. keep (AP_KEEP): the range holds data (a live heap range): copy it out, map, copy it back.
// Otherwise the range is part of a PROT_NONE reservation. Returns 0, or the failing step's number (p->step names it, p->err holds its error);
// on a failure the range is put back as it was, except 5: the import failed and so did its cleanup (apool_finalize's code in p->fin). The pool is
// then still alive (AHB, fd, Vulkan objects; the range may be unmapped): it is counted in g_ahb_alive, and the caller must not reuse or overwrite it.
// p must not hold a live pool (the memset below would drop its handles).
static int apool_create(apool_t *p, uint8_t *at, size_t len, int fl) {
    memset(p, 0, sizeof *p); p->fd = -1; p->at = at; p->len = len;
    AHardwareBuffer_Desc d;
    if (g_ahb_dead) { p->step = "not tried (the allocator service failed earlier)"; p->err = 0; p->at = NULL; return 1; }
    if (g_ahb_alive) { p->step = "not tried (an earlier pool could not be finalized and is still alive)"; p->err = 0; p->at = NULL; return 1; }
    if (len > AHB_CAP || ahb_alloc_desc((uint32_t)len, 1, &d)) { p->step = "not tried: above the 64 MB AHB_CAP"; p->err = EFBIG; p->at = NULL; return 1; }
    double t = now_s(); int e = AHardwareBuffer_allocate(&d, &p->ahb); p->t_alloc = now_s() - t;
    if (e) { p->step = "AHardwareBuffer_allocate"; p->err = e; p->ahb = NULL; p->at = NULL; if (e == -32 || e == -129) g_ahb_dead = 1; return 1; }
    t = now_s(); p->fd = ahb_dmabuf_fd(p->ahb, len, &p->nfds, &p->fd_size); p->t_fd = now_s() - t;
    if (p->fd < 0) { p->step = "dma-buf fd (sendHandleToUnixSocket, size check)"; p->err = errno; p->at = NULL; apool_release(p); return 2; }
    uint8_t *save = NULL;
    if (fl & AP_KEEP) {
        if (!(save = malloc(len))) { p->step = "malloc"; p->err = ENOMEM; p->at = NULL; apool_release(p); return 3; }
        t = now_s(); memcpy(save, at, len); p->t_copy = now_s() - t;
    }
    t = now_s(); void *q = mmap(at, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, p->fd, 0); p->t_map = now_s() - t;
    if (q != at) {
        p->step = "mmap(MAP_SHARED|MAP_FIXED) of the dma-buf"; p->err = errno;
        // a failed MAP_FIXED may have unmapped the range: put anonymous memory back, and the saved contents only if that worked
        void *rq = mmap(at, len, (fl & AP_KEEP) ? PROT_READ | PROT_WRITE : PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (rq == at && save) memcpy(at, save, len);
        if (rq != at) p->step = "mmap(MAP_SHARED|MAP_FIXED) of the dma-buf, and the anonymous restore failed too";
        free(save);
        p->at = NULL; apool_release(p); return 3;
    }
    if (save) { t = now_s(); memcpy(at, save, len); p->t_copy += now_s() - t; free(save); }
    if (!(fl & AP_KEEP_FD)) { close(p->fd); p->fd = -1; }
    if (fl & AP_NO_IMPORT) return 0;
    t = now_s(); VkResult r = ahb_import(p, (fl & AP_BDA) != 0); p->t_import = now_s() - t;
    if (r) {
        p->err = r; int fr = apool_finalize(p, (fl & AP_KEEP) != 0);
        if (!fr) return 4;
        p->fin = fr; g_ahb_alive++;
        p->step = fr == -1 ? "Vulkan import failed, and its cleanup could not save the range (malloc): the pool is still mapped and alive"
                           : "Vulkan import failed, and its cleanup's anonymous MAP_FIXED failed: the pool's objects are alive, the range may be unmapped";
        return 5;
    }
    return 0;
}
static void apool_errstr(const apool_t *p, int rc, char *out, size_t n) {
    if (rc == 4 || rc == 5) snprintf(out, n, "step %d %s: %s%s", rc, p->step, vkc_result_str((VkResult)p->err), rc == 5 ? (p->fin == -1 ? " (finalize -1)" : " (finalize -2)") : "");
    else snprintf(out, n, "step %d %s: %ld (%s)", rc, p->step ? p->step : "?", p->err, strerror(p->err < 0 ? (int)-p->err : (int)p->err));
}
// Finalize at the end of a test. A failure leaves the pool alive (AHB, fd and Vulkan objects; with -2 the range may be unmapped): it is reported as
// <item>.finalize FAIL and counted in g_ahb_alive, which stops the remaining AHB tests; the pool is not touched again.
static int apool_end(apool_t *p, int keep, const char *item) {
    int fr = apool_finalize(p, keep);
    if (fr) {
        char it[64]; snprintf(it, sizeof it, "%s.finalize", item); g_ahb_alive++;
        R(it, "FAIL", "apool_finalize returned %d (%s): the pool's AHB, fd and Vulkan objects are left alive, %s", fr,
          fr == -1 ? "malloc of the save buffer failed" : "the anonymous MAP_FIXED failed", fr == -1 ? "still mapped" : "and the range may be unmapped");
    }
    return fr;
}
static gmap_t apool_gmap(const apool_t *p) { gmap_t g; memset(&g, 0, sizeof g); g.mem = p->mem; g.ptr = p->at; g.buf = p->buf; g.addr = p->addr; g.size = p->len; return g; }

// ---------------------------------------------------------------- GPU helpers on VkBuffers (an imported AHB's memory binds only its dedicated buffer)
typedef struct { VkBuffer b; VkDeviceMemory m; uint8_t *p; size_t len; } stg_t;
static void stg_free(stg_t *s) {
    if (s->b) V.vkDestroyBuffer(V.dev, s->b, NULL);
    if (s->p) V.vkUnmapMemory(V.dev, s->m);
    if (s->m) V.vkFreeMemory(V.dev, s->m, NULL);
    memset(s, 0, sizeof *s);
}
static int stg_new(stg_t *s, size_t len) {   // a normally mapped staging buffer of the chosen (cached, coherent) type
    memset(s, 0, sizeof *s); s->len = len; void *q = NULL;
    if (vkc_alloc(&V, len, g_type, 0, &s->m) || V.vkMapMemory(V.dev, s->m, 0, VK_WHOLE_SIZE, 0, &q)) { stg_free(s); return -1; }
    s->p = q;
    if (vkc_buffer(&V, s->m, 0, len, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &s->b, NULL)) { stg_free(s); return -1; }
    return 0;
}
static VkResult cmd_end_host(void) {   // transfer writes -> host reads, submit, wait
    VkMemoryBarrier hb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
    V.vkCmdPipelineBarrier(V.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    return vkc_cmd_submit_wait(&V, 20000000000ull);
}
static VkResult copy_regions(VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy *rg) {
    VkResult r = vkc_cmd_begin(&V); if (r) return r;
    V.vkCmdCopyBuffer(V.cmd, src, dst, n, rg);
    return cmd_end_host();
}
static VkResult fill_buf(VkBuffer b, size_t off, size_t len, uint32_t v) {
    VkResult r = vkc_cmd_begin(&V); if (r) return r;
    V.vkCmdFillBuffer(V.cmd, b, off, len, v);
    return cmd_end_host();
}
// The GPU writes ov(seed, k0 + k) into buf [off, off + len) (a staging buffer filled by the CPU, then vkCmdCopyBuffer): the destination's CPU caches see none of it
static VkResult gpu_write_ov(VkBuffer buf, size_t off, size_t len, uint32_t seed, uint32_t k0) {
    const size_t CH = 64 * MB; stg_t s; if (stg_new(&s, len < CH ? len : CH)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkResult r = VK_SUCCESS;
    for (size_t o = 0; o < len && !r; o += CH) {
        size_t n = len - o < CH ? len - o : CH;
        for (size_t k = 0; k < n / 4; k++) ((uint32_t *)s.p)[k] = ov(seed, k0 + (uint32_t)((o / 4) + k));
        VkBufferCopy rg = { 0, off + o, n }; r = copy_regions(s.b, buf, 1, &rg);
    }
    stg_free(&s); return r;
}
// What the GPU sees: buf [off, off + len) copied out in chunks and compared word by word with ov(seed, k0 + k). Returns wrong words, < 0 on error
static long gpu_check_ov(VkBuffer buf, size_t off, size_t len, uint32_t seed, uint32_t k0) {
    const size_t CH = 64 * MB; stg_t s; if (stg_new(&s, len < CH ? len : CH)) return -2;
    long bad = 0;
    for (size_t o = 0; o < len; o += CH) {
        size_t n = len - o < CH ? len - o : CH;
        VkBufferCopy rg = { off + o, 0, n };
        if (copy_regions(buf, s.b, 1, &rg)) { bad = -3; break; }
        for (size_t k = 0; k < n / 4; k++) bad += ((volatile uint32_t *)s.p)[k] != ov(seed, k0 + (uint32_t)((o / 4) + k));
    }
    stg_free(&s); return bad;
}
// The same for a sample: two words at the start of every stride-th page, up to 65,536 regions (large pools)
static long gpu_check_ov_pages(VkBuffer buf, size_t len, uint32_t seed, uint32_t k0, size_t *checked) {
    size_t np = len / g_pg, stride = 1; while (np / stride > 65536) stride *= 2;
    size_t nr = (np + stride - 1) / stride; *checked = nr;
    stg_t s; if (stg_new(&s, (nr * 8 + 4095) & ~(size_t)4095)) return -2;
    VkBufferCopy *rg = malloc(nr * sizeof *rg); if (!rg) { stg_free(&s); return -2; }
    for (size_t i = 0; i < nr; i++) rg[i] = (VkBufferCopy){ i * stride * g_pg, i * 8, 8 };
    long bad = -3;
    if (!copy_regions(buf, s.b, (uint32_t)nr, rg)) {
        bad = 0;
        for (size_t i = 0; i < nr; i++) { uint32_t k = (uint32_t)(i * stride * g_pg / 4); bad += ((volatile uint32_t *)s.p)[2 * i] != ov(seed, k0 + k) || ((volatile uint32_t *)s.p)[2 * i + 1] != ov(seed, k0 + k + 1); }
    }
    free(rg); stg_free(&s); return bad;
}
// The CPU's view, guarded: wrong words against ov(seed, k0 + k), -1 on a fault (g_guard_* say where). first: index of the first wrong word
static volatile long g_cw_bad, g_cw_zero, g_cw_first;
static long cpu_check_ov(const uint8_t *p, size_t nw, uint32_t seed, uint32_t k0, long *zero, long *first) {
    int ok; g_cw_bad = 0; g_cw_zero = 0; g_cw_first = -1;
    GUARD_BEGIN(ok)
        const volatile uint32_t *w = (const volatile uint32_t *)p;
        for (size_t k = 0; k < nw; k++) { uint32_t x = w[k]; if (x != ov(seed, k0 + (uint32_t)k)) { if (g_cw_first < 0) g_cw_first = (long)k; g_cw_bad++; if (!x) g_cw_zero++; } }
    GUARD_END(ok);
    if (zero) *zero = g_cw_zero;
    if (first) *first = g_cw_first;
    return ok ? g_cw_bad : -1;
}
static int cpu_fill_ov(uint8_t *p, size_t nw, uint32_t seed, uint32_t k0) {
    int ok; GUARD_BEGIN(ok) for (size_t k = 0; k < nw; k++) ((volatile uint32_t *)p)[k] = ov(seed, k0 + (uint32_t)k); GUARD_END(ok);
    return ok;
}
// Where wrong words came from: for the first few, the word index their value belongs to (x ^ seed), as a delta in KB
static void where_from(const uint8_t *p, size_t nw, uint32_t seed, uint32_t k0, long first, char *out, size_t n) {
    size_t u = 0; out[0] = 0; int found = 0;
    for (size_t k = first < 0 ? nw : (size_t)first; k < nw && found < 3; k++) {
        uint32_t x = ((const volatile uint32_t *)p)[k]; if (x == ov(seed, k0 + (uint32_t)k)) continue;
        if (!x) { u += snprintf(out + u, u < n ? n - u : 0, "%sword %#zx reads 0", found ? "; " : "", k * 4); found++; continue; }
        long src = (long)(x ^ seed) - (long)k0;
        u += snprintf(out + u, u < n ? n - u : 0, "%sbyte offset %#zx holds the word of offset %#lx (delta %+ld KB)", found ? "; " : "", k * 4, src * 4, (src - (long)k) * 4 / 1024);
        found++;
    }
}

// ---------------------------------------------------------------- /proc helpers
typedef struct { char hdr[200]; char flags[200]; long size_kb, rss_kb; int found; } smaps_t;
static void smaps_at(uintptr_t a, smaps_t *o) {
    memset(o, 0, sizeof *o); o->rss_kb = -1;
    FILE *f = fopen("/proc/self/smaps", "r"); if (!f) return;
    char line[512]; int in = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long s, e;
        if (sscanf(line, "%lx-%lx ", &s, &e) == 2) { in = a >= s && a < e; if (in) { o->found = 1; line[strcspn(line, "\n")] = 0; snprintf(o->hdr, sizeof o->hdr, "%s", line); } else if (o->found) break; continue; }
        if (!in) continue;
        if (!strncmp(line, "Size:", 5)) o->size_kb = atol(line + 5);
        else if (!strncmp(line, "Rss:", 4)) o->rss_kb = atol(line + 4);
        else if (!strncmp(line, "VmFlags:", 8)) { line[strcspn(line, "\n")] = 0; snprintf(o->flags, sizeof o->flags, "%s", line + 8 + strspn(line + 8, " ")); }
    }
    fclose(f);
}
static int has_flag(const char *flags, const char *f) { char b[220]; snprintf(b, sizeof b, " %s ", flags); char k[8]; snprintf(k, sizeof k, " %s ", f); return strstr(b, k) != NULL; }
static int proc_vmas(void) { int t; count_vmas(0, 0, &t); return t; }
static int dmabuf_vmas(size_t *bytes) {
    FILE *f = fopen("/proc/self/maps", "r"); int n = 0; *bytes = 0; if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof line, f)) if (strstr(line, "dmabuf")) { unsigned long lo, hi; if (sscanf(line, "%lx-%lx", &lo, &hi) == 2) { *bytes += hi - lo; n++; } }
    fclose(f); return n;
}
static void max_map_count_str(char *out, size_t n) {
    snprintf(out, n, "unreadable (kernel default 65530)");
    FILE *f = fopen("/proc/sys/vm/max_map_count", "r"); char b[32];
    if (f) { if (fgets(b, sizeof b, f)) { b[strcspn(b, "\n")] = 0; snprintf(out, n, "%s", b); } fclose(f); }
}

// ---------------------------------------------------------------- caps
static void ahb_caps(void) {
    S("== (ahb) capabilities");
    int has = vkc_has_ext(&V, "VK_ANDROID_external_memory_android_hardware_buffer");
    R("ahb.ext", has && g_ahb_ok ? "PASS" : "FAIL", "VK_ANDROID_external_memory_android_hardware_buffer %s%s, VK_EXT_queue_family_foreign %s, VK_EXT_external_memory_dma_buf %s, VK_KHR_external_memory_fd %s",
      has ? "present" : "ABSENT", has && !g_ahb_props_fn ? " (no vkGetAndroidHardwareBufferPropertiesANDROID)" : "", g_ahb_qff ? "present" : "absent",
      vkc_has_ext(&V, "VK_EXT_external_memory_dma_buf") ? "present" : "absent", vkc_has_ext(&V, "VK_KHR_external_memory_fd") ? "present" : "absent");
    if (g_ext_buf_fn) {
        VkPhysicalDeviceExternalBufferInfo bi = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, NULL, 0, k_ahb_usage | (g_bda ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0),
                                                  VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID };
        VkExternalBufferProperties ep = { VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
        g_ext_buf_fn(V.pd, &bi, &ep);
        VkExternalMemoryFeatureFlags f = ep.externalMemoryProperties.externalMemoryFeatures;
        R("ahb.ext_buffer_props", (f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ? "PASS" : "FAIL", "AHB handle type for pool-buffer usage: features %#x (dedicated only %u, exportable %u, importable %u), compatible handle types %#x",
          f, !!(f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT), !!(f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT), !!(f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT),
          ep.externalMemoryProperties.compatibleHandleTypes);
    }
    const VkPhysicalDeviceLimits *L = &V.props.limits;
    char mm[64]; max_map_count_str(mm, sizeof mm);
    R("ahb.limits", "INFO", "maxMemoryAllocationCount %u, maxMemoryAllocationSize %#llx, maxBufferSize %#llx, maxStorageBufferRange %#x, maxUniformBufferRange %#x; vm.max_map_count %s; process VMAs now %d, fds %d",
      L->maxMemoryAllocationCount, (unsigned long long)g_max_alloc, (unsigned long long)g_max_buffer, L->maxStorageBufferRange, L->maxUniformBufferRange, mm, proc_vmas(), count_fds());
    AHardwareBuffer_Desc d; char sup[600]; size_t u = 0; sup[0] = 0;
    // only the sizes the tests allocate (all at or below AHB_CAP): a query above it crashed the allocator service on the Fold 7
    static const uint64_t sz[] = { 16 * 1024, 64 * 1024, 1 * MB, 4 * MB, 16 * MB, 32 * MB, 40 * MB, 64 * MB };
    for (size_t i = 0; i < sizeof sz / sizeof *sz; i++) { if (ahb_alloc_desc((uint32_t)sz[i], 1, &d)) continue; u += snprintf(sup + u, sizeof sup - u, "%s%#llx:%d", i ? " " : "", (unsigned long long)sz[i], AHardwareBuffer_isSupported(&d)); }
    R("ahb.is_supported", "INFO", "AHardwareBuffer_isSupported (BLOB, CPU_READ_OFTEN|CPU_WRITE_OFTEN|GPU_DATA_BUFFER) by width: %s; nothing above the %zu MB cap is queried (a query above it aborted the allocator service on SM-F966B)", sup, (size_t)(AHB_CAP / MB));
}

// ---------------------------------------------------------------- (1) the chain
static void type_flags_list(uint32_t bits, char *out, size_t n) {
    size_t u = 0; out[0] = 0;
    for (uint32_t t = 0; t < V.mem.memoryTypeCount; t++) if ((bits >> t) & 1) { char fl[128]; vkc_memflags_str(V.mem.memoryTypes[t].propertyFlags, fl, sizeof fl); u += snprintf(out + u, u < n ? n - u : 0, "%s%u:%s", u ? " " : "", t, fl); }
}
// Over a PROT_NONE reservation (a new pool in unused guest space)
static void ahb_chain_reserve(void) {
    S("== (ahb 1a) AHB pool over a PROT_NONE reservation");
    uint8_t *at = g_rb + 64 * MB + 0x3000; size_t len = 64 * MB; apool_t p; char e[300];
    int rc = apool_create(&p, at, len, AP_BDA * !!g_bda);
    if (rc) { apool_errstr(&p, rc, e, sizeof e); R("ahb.chain_reserve", "FAIL", "%s", e); return; }
    char info[300] = "?"; range_info_t ri; range_info((uintptr_t)at, (uintptr_t)at + len, &ri); smaps_t sm; smaps_at((uintptr_t)at, &sm);
    { int fd = -1, nf; off_t fs; fd = ahb_dmabuf_fd(p.ahb, len, &nf, &fs); if (fd >= 0) { fd_dmabuf_info(fd, info, sizeof info); close(fd); } }
    char tl[400]; type_flags_list(p.type_bits, tl, sizeof tl); char cf[128]; vkc_memflags_str(V.mem.memoryTypes[p.type].propertyFlags, cf, sizeof cf);
    S("  AHB %zu MB: allocate %.2f ms, fd %.2f ms (%d fds in the handle, dma-buf size %lld; %s), mmap %.3f ms, import %.2f ms; maps:", (size_t)(len / MB), p.t_alloc * 1e3, p.t_fd * 1e3, p.nfds,
      (long long)p.fd_size, info, p.t_map * 1e3, p.t_import * 1e3); S_lines("    ", ri.lines);
    S("  smaps: %s; Size %ld kB, Rss %ld kB before any touch; VmFlags: %s", sm.hdr, sm.size_kb, sm.rss_kb, sm.flags);
    R("ahb.import_types", "INFO", "allocationSize %#llx for %#zx; AHB memoryTypeBits %#x [%s]; buffer memoryTypeBits %#x; chosen type %u (%s); buffer device address %#llx",
      (unsigned long long)p.alloc_size, len, p.type_bits, tl, p.req_bits, p.type, cf, (unsigned long long)p.addr);
    R("ahb.vmflags", "INFO", "AHB mapping VmFlags [%s]: pf (PFN map) %s, io %s; %d maps line(s); %s", sm.flags, has_flag(sm.flags, "pf") ? "YES" : "no", has_flag(sm.flags, "io") ? "YES" : "no", ri.nlines, info);
    uint32_t s1 = ov_seed(), s2 = ov_seed(); size_t nw = len / 4;
    int wok = cpu_fill_ov(at, nw, s1, 0);
    long cbad = cpu_check_ov(at, nw, s1, 0, NULL, NULL), gbad = gpu_check_ov(p.buf, 0, len, s1, 0);
    VkResult r = gpu_write_ov(p.buf, 0, len, s2, 0);   // the GPU overwrites everything; the CPU's cached copies of the old words must not show
    long cbad2 = r ? -3 : cpu_check_ov(at, nw, s2, 0, NULL, NULL);
    S("  CPU wrote every word (%s), CPU reads back %ld wrong; GPU copy-out of all of it: %ld wrong; GPU overwrote everything (%s), CPU then reads %ld wrong (no flush, no invalidate)",
      wok ? "ok" : "FAULT", cbad, gbad, vkc_result_str(r), cbad2);
    // finalize: back to anonymous memory with the contents
    double t = now_s(); int fr = apool_finalize(&p, 1); t = now_s() - t; if (fr) g_ahb_alive++;
    long abad = cpu_check_ov(at, nw, s2, 0, NULL, NULL); range_info(( uintptr_t)at, (uintptr_t)at + len, &ri);
    S("  finalize (copy out, anonymous MAP_FIXED, copy back, free): rc %d, %.1f ms; contents afterwards %ld wrong; maps: %s", fr, t * 1e3, abad, ri.lines);
    mmap(at, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    R("ahb.chain_reserve", (wok && !cbad && !gbad && !r && !cbad2 && !fr && !abad) ? "PASS" : "FAIL",
      "64 MB at %p (4 KB-aligned, not 2 MB): GPU reads the CPU's writes at the guest address %s (%ld wrong), CPU reads the GPU's writes after the fence %s (%ld wrong), finalize keeps the data %s",
      at, gbad == 0 ? "yes" : "NO", gbad, cbad2 == 0 ? "yes" : "NO", cbad2, abad == 0 ? "yes" : "NO");
}
// Over part of a live RW heap range with data (SetHeapSize's mprotect): copy out, map, copy back; then finalize
static void ahb_chain_heap(void) {
    S("== (ahb 1b) AHB pool over part of a live RW heap range");
    uint8_t *base = g_rb + 256 * MB; size_t blen = 128 * MB;
    if (mprotect(base, blen, PROT_READ | PROT_WRITE)) { R("ahb.chain_heap", "FAIL", "mprotect RW: errno %d", errno); return; }
    uint32_t s = ov_seed(); cpu_fill_ov(base, blen / 4, s, 0);   // word k of the heap range = ov(s, k): neighbours and pool told apart by offset
    size_t poff = 16 * MB + 0x7000, len = 40 * MB; uint8_t *at = base + poff; apool_t p; char e[300];
    int v0; int vb = count_vmas((uintptr_t)base, (uintptr_t)base + blen, &v0);
    int rc = apool_create(&p, at, len, AP_KEEP);
    if (rc) { apool_errstr(&p, rc, e, sizeof e); R("ahb.chain_heap", "FAIL", "%s", e); mmap(base, blen, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0); return; }
    int vd = count_vmas((uintptr_t)base, (uintptr_t)base + blen, &v0);
    long pool_cpu = cpu_check_ov(at, len / 4, s, (uint32_t)(poff / 4), NULL, NULL);
    long nb_lo = cpu_check_ov(base, poff / 4, s, 0, NULL, NULL), nb_hi = cpu_check_ov(at + len, (blen - poff - len) / 4, s, (uint32_t)((poff + len) / 4), NULL, NULL);
    long gbad = gpu_check_ov(p.buf, 0, len, s, (uint32_t)(poff / 4));
    // the GPU writes a 1 MB window; the CPU reads it at the guest address
    uint32_t s2 = ov_seed(); VkResult r = gpu_write_ov(p.buf, 4 * MB, 1 * MB, s2, 0);
    long win = r ? -3 : cpu_check_ov(at + 4 * MB, MB / 4, s2, 0, NULL, NULL);
    double t = now_s(); int fr = apool_finalize(&p, 1); t = now_s() - t; if (fr) g_ahb_alive++;
    long a1 = cpu_check_ov(at, 4 * MB / 4, s, (uint32_t)(poff / 4), NULL, NULL), a2 = cpu_check_ov(at + 4 * MB, MB / 4, s2, 0, NULL, NULL),
         a3 = cpu_check_ov(at + 5 * MB, (len - 5 * MB) / 4, s, (uint32_t)((poff + 5 * MB) / 4), NULL, NULL);
    int va = count_vmas((uintptr_t)base, (uintptr_t)base + blen, &v0);
    S("  pool %zu MB at %p inside the 128 MB RW range: init copy %.1f ms (%.2f GB/s for the two copies), allocate %.2f ms, mmap %.3f ms, import %.2f ms; VMAs in the range %d -> %d (pool mapped) -> %d (finalized)",
      (size_t)(len / MB), at, p.t_copy * 1e3, 2.0 * len / p.t_copy / 1e9, p.t_alloc * 1e3, p.t_map * 1e3, p.t_import * 1e3, vb, vd, va);
    S("  after init: pool words %ld wrong (CPU), %ld wrong (GPU), neighbours below / above %ld / %ld wrong; GPU-written window: CPU reads %ld wrong; finalize %.1f ms rc %d, then %ld / %ld / %ld wrong",
      pool_cpu, gbad, nb_lo, nb_hi, win, t * 1e3, fr, a1, a2, a3);
    int pass = !pool_cpu && !gbad && !nb_lo && !nb_hi && !r && !win && !fr && !a1 && !a2 && !a3;
    R("ahb.chain_heap", pass ? "PASS" : "FAIL", "40 MB pool at heap offset %#zx: contents kept through init (CPU %ld, GPU %ld wrong), neighbours intact (%ld/%ld), GPU writes seen (%ld), finalize keeps all (%ld/%ld/%ld); VMAs %d -> %d -> %d",
      poff, pool_cpu, gbad, nb_lo, nb_hi, win, a1, a2, a3, vb, vd, va);
    R("ahb.pool_init_cost", "INFO", "40 MB pool: copy out + copy back %.1f ms, AHardwareBuffer_allocate %.2f ms, dma-buf fd %.2f ms, mmap %.3f ms, Vulkan import %.2f ms; finalize %.1f ms",
      p.t_copy * 1e3, p.t_alloc * 1e3, p.t_fd * 1e3, p.t_map * 1e3, p.t_import * 1e3, t * 1e3);
    mmap(base, blen, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
}

// ---------------------------------------------------------------- page-table population: faults on the first touch?
static void populate_one(const char *kind, uint8_t *p, size_t len) {
    smaps_t s0, s1; smaps_at((uintptr_t)p, &s0);
    struct rusage r0, r1; getrusage(RUSAGE_THREAD, &r0); double t = now_s();
    int ok; GUARD_BEGIN(ok) for (size_t o = 0; o < len; o += g_pg) g_sink += ((volatile uint8_t *)p)[o]; GUARD_END(ok);
    t = now_s() - t; getrusage(RUSAGE_THREAD, &r1); smaps_at((uintptr_t)p, &s1);
    long flt = (r1.ru_minflt - r0.ru_minflt) + (r1.ru_majflt - r0.ru_majflt); size_t np = len / g_pg;
    char item[48]; snprintf(item, sizeof item, "ahb.populate.%s", kind);
    R(item, ok ? "INFO" : "FAIL", "first read of %zu fresh pages: %ld page faults (%.2f per page), %.3f us/page%s; Rss %ld -> %ld kB; VmFlags [%s]",
      np, flt, (double)flt / np, t * 1e6 / np, ok ? "" : " (FAULTED)", s0.rss_kb, s1.rss_kb, s1.flags);
}
static void ahb_populate(void) {
    S("== (ahb 2a) page tables: is the mapping populated at mmap, or faulted in page by page?");
    size_t len = 16 * MB;
    uint8_t *an = g_rb + 1 * GB; mprotect(an, len, PROT_READ | PROT_WRITE); populate_one("anon", an, len); mmap(an, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    gmap_t g; if (!gmap_create(&g, len, NULL, 0)) populate_one("kgsl", g.ptr, len); gmap_destroy(&g);
    apool_t p; uint8_t *at = g_rb + 1 * GB + 0x1000; int rc = apool_create(&p, at, len, 0);
    if (!rc) { gpu_write_ov(p.buf, 0, len, ov_seed(), 0); populate_one("ahb", at, len); apool_end(&p, 0, "ahb.populate.ahb"); }
    else { char e[300]; apool_errstr(&p, rc, e, sizeof e); R("ahb.populate.ahb", "FAIL", "%s", e); }
}

// ---------------------------------------------------------------- (2) the host's memory paths
typedef struct { volatile uint32_t *a; int op; uint32_t val; long rc; int err; double t; int ready, done; } fxw_t;
static long sys_futex(volatile uint32_t *a, int op, uint32_t v, const struct timespec *ts) { return syscall(SYS_futex, (uint32_t *)a, op, v, ts, NULL, 0); }
static void *fx_waiter(void *arg) {
    fxw_t *w = arg; struct timespec ts = { 5, 0 };
    __atomic_store_n(&w->ready, 1, __ATOMIC_SEQ_CST);   // about to wait (it may not be inside FUTEX_WAIT yet: the waker retries)
    double t0 = now_s(); w->rc = sys_futex(w->a, w->op, w->val, &ts); w->err = w->rc ? errno : 0; w->t = now_s() - t0;
    __atomic_store_n(&w->done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}
static int futex_test(volatile uint32_t *a, int priv, char *d, size_t dn) {
    int W = priv ? FUTEX_WAIT_PRIVATE : FUTEX_WAIT, K = priv ? FUTEX_WAKE_PRIVATE : FUTEX_WAKE;
    struct timespec ts = { 0, 20000000 };
    *a = 5;
    long r1 = sys_futex(a, W, 6, &ts); int e1 = r1 ? errno : 0;   // a different value: EAGAIN at once
    double t = now_s(); long r2 = sys_futex(a, W, 5, &ts); int e2 = r2 ? errno : 0; t = now_s() - t;   // the value: ETIMEDOUT after 20 ms
    // wake of a waiting thread: the word stays 5 and the wake is retried (1 ms apart, up to 3 s) until it wakes one waiter, the waiter returns on
    // its own (an error, or its 5 s timeout), or the wake fails; then the word changes and a last wake releases a waiter still there
    fxw_t w; memset(&w, 0, sizeof w); w.a = a; w.op = W; w.val = 5; pthread_t th;
    int pc = pthread_create(&th, NULL, fx_waiter, &w);
    long nw = -2; int ew = 0, tries = 0; double tw = 0;
    if (!pc) {
        double t0 = now_s();
        while (!__atomic_load_n(&w.ready, __ATOMIC_SEQ_CST) && now_s() - t0 < 3) usleep(100);
        for (;;) {
            nw = sys_futex(a, K, 1, NULL); tries++;
            if (nw < 0) { ew = errno; break; }
            if (nw >= 1 || __atomic_load_n(&w.done, __ATOMIC_SEQ_CST) || now_s() - t0 > 3) break;
            usleep(1000);
        }
        tw = now_s() - t0;
        *a = 6; if (nw != 1) sys_futex(a, K, 1, NULL);
        pthread_join(th, NULL);
    }
    int pass = !pc && r1 == -1 && e1 == EAGAIN && r2 == -1 && e2 == ETIMEDOUT && nw == 1 && w.rc == 0;
    if (pc) { snprintf(d, dn, "pthread_create failed: %s", strerror(pc)); return 0; }
    snprintf(d, dn, "wait on another value: %s; wait on the value: %s after %.1f ms; wake of a waiting thread: %ld woken after %d wake call(s) in %.1f ms%s%s, the waiter returned %s after %.1f ms",
             r1 ? strerror(e1) : "returned 0", r2 ? strerror(e2) : "returned 0", t * 1e3, nw, tries, tw * 1e3, ew ? ", errno " : "", ew ? strerror(ew) : "", w.rc ? strerror(w.err) : "0", w.t * 1e3);
    return pass;
}
static long llsc_inc(volatile uint32_t *p, long maxtry) {   // LDAXR/STLXR increment, retries bounded (exclusives can fail for ever on some memory types)
    uint32_t v, fail; long t = 0;
    do { __asm__ volatile("ldaxr %w0, [%2]\n\tadd %w0, %w0, #1\n\tstlxr %w1, %w0, [%2]" : "=&r"(v), "=&r"(fail) : "r"(p) : "memory"); t++; } while (fail && t < maxtry);
    return fail ? -1 : t - 1;
}
static inline uint32_t lse_add(volatile uint32_t *p, uint32_t v) { uint32_t old; __asm__ volatile(".arch_extension lse\n\tldaddal %w1, %w0, [%2]" : "=r"(old) : "r"(v), "r"(p) : "memory"); return old; }
static inline uint32_t lse_cas(volatile uint32_t *p, uint32_t expect, uint32_t nv) { __asm__ volatile(".arch_extension lse\n\tcasal %w0, %w2, [%1]" : "+r"(expect) : "r"(p), "r"(nv) : "memory"); return expect; }
typedef struct { volatile uint32_t *w; int n, lse; long retries; int bad; } atw_t;
static void *at_worker(void *arg) {
    atw_t *a = arg;
    for (int i = 0; i < a->n; i++) { if (a->lse) lse_add(a->w, 1); else { long r = llsc_inc(a->w, 1000000); if (r < 0) { a->bad = 1; break; } a->retries += r; } }
    return NULL;
}
static int atomics_test(volatile uint32_t *w, int lse, char *d, size_t dn) {
    const int N1 = 1000000, NT = 4, NM = 250000; int ok; volatile long r1 = 0; volatile int bad1 = 0; volatile double t1 = 0;
    *w = 0;
    GUARD_BEGIN(ok)
        double t = now_s();
        if (lse) { for (int i = 0; i < N1; i++) lse_add(w, 1); for (uint32_t i = 0; i < 1000; i++) if (lse_cas(w, (uint32_t)N1 + i, (uint32_t)N1 + i + 1) != (uint32_t)N1 + i) bad1++; }
        else for (int i = 0; i < N1; i++) { long r = llsc_inc(w, 1000000); if (r < 0) { bad1 = 1; break; } r1 += r; }
        t1 = now_s() - t;
    GUARD_END(ok);
    uint32_t want1 = (uint32_t)N1 + (lse ? 1000 : 0);
    if (!ok || bad1 || *w != want1) { snprintf(d, dn, "single thread: %s, %s, value %u (want %u)", ok ? "no fault" : "FAULT", bad1 ? (lse ? "CAS returned wrong old values" : "STLXR never succeeded within 10^6 tries") : "completed", *w, want1); return 0; }
    // the host's own compare-and-swap (hle_thread.c cas32: __atomic_compare_exchange_n, NDK outline atomics)
    uint32_t e = want1, nb = 0; for (int i = 0; i < 1000; i++) { if (!__atomic_compare_exchange_n((uint32_t *)w, &e, e + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) nb++; e = *w; }
    *w = 0; atw_t a[4]; pthread_t th[4]; int started[4] = { 0 }, nst = 0, pce = 0; double t = now_s();
    for (int i = 0; i < NT; i++) { a[i] = (atw_t){ w, NM, lse, 0, 0 }; int pe = pthread_create(&th[i], NULL, at_worker, &a[i]); if (pe) pce = pe; else { started[i] = 1; nst++; } }
    long rt = 0; int badm = 0; for (int i = 0; i < NT; i++) if (started[i]) { pthread_join(th[i], NULL); rt += a[i].retries; badm |= a[i].bad; }
    t = now_s() - t;
    if (pce) { snprintf(d, dn, "%s: 1 thread %d ops in %.1f ms (%.1f ns/op); pthread_create failed for %d of %d threads: %s (the %d started were joined: total %u)",
                        lse ? "LSE LDADDAL + CASAL" : "LDAXR/STLXR", N1, t1 * 1e3, t1 * 1e9 / N1, NT - nst, NT, strerror(pce), nst, *w); return 0; }
    int pass = !badm && *w == (uint32_t)(NT * NM) && nb == 0;
    snprintf(d, dn, "%s: 1 thread %d ops in %.1f ms (%.1f ns/op, %ld STLXR retries); %d threads x %d: total %u (want %u), %.1f ms, %ld retries%s; __atomic_compare_exchange_n %d/1000 failed",
             lse ? "LSE LDADDAL + CASAL" : "LDAXR/STLXR", N1, t1 * 1e3, t1 * 1e9 / N1, (long)r1, NT, NM, *w, NT * NM, t * 1e3, rt, badm ? ", STLXR STUCK" : "", nb);
    return pass;
}
// One kind of memory (p: at least 4 MB, RW, contents ov(s, k)); buf: a VkBuffer over it at offset 0 for the GPU check of a file read (VK_NULL_HANDLE: none)
static void host_paths_one(const char *kind, uint8_t *p, size_t len, VkBuffer buf, uint32_t s) {
    char item[48], d[600]; smaps_t sm; smaps_at((uintptr_t)p, &sm);
    snprintf(item, sizeof item, "ahb.host.vmflags.%s", kind);
    R(item, "INFO", "VmFlags [%s] (pf %s, io %s); %s", sm.flags, has_flag(sm.flags, "pf") ? "yes" : "no", has_flag(sm.flags, "io") ? "yes" : "no", sm.hdr);
    // process_vm_readv on this process (plat_linux.c vm_read_overwrite: the host's fault-safe read)
    uint8_t *tmp = malloc(2 * MB); size_t n = 64 * 1024, off = 0x1234;
    struct iovec l = { tmp, n }, r = { p + off, n }; memset(tmp, 0, n);
    ssize_t got = process_vm_readv(getpid(), &l, 1, &r, 1, 0); int e = got < 0 ? errno : 0;
    int same = got == (ssize_t)n && !memcmp(tmp, p + off, n);
    snprintf(item, sizeof item, "ahb.host.pvr.%s", kind);
    R(item, same ? "PASS" : "FAIL", "process_vm_readv(self) of 64 KB at +%#zx: returned %zd%s%s, data %s", off, got, e ? ", errno " : "", e ? strerror(e) : "", same ? "matches" : "DIFFERS or missing");
    // write() from it into a pipe: a fault-safe read without page pinning (EFAULT on an unreadable address, no signal)
    int pf[2]; long wn = -1, rn = -1; int we = 0, pmatch = 0;
    if (!pipe2(pf, O_CLOEXEC)) { wn = write(pf[1], p + 0x2345, 4096); we = wn < 0 ? errno : 0; if (wn > 0) rn = read(pf[0], tmp, (size_t)wn); pmatch = wn == 4096 && rn == 4096 && !memcmp(tmp, p + 0x2345, 4096); close(pf[0]); close(pf[1]); }
    snprintf(item, sizeof item, "ahb.host.pipe.%s", kind);
    R(item, pmatch ? "PASS" : "FAIL", "write(pipe, mapping + 0x2345, 4096) = %ld%s%s, read back %ld, data %s", wn, we ? " errno " : "", we ? strerror(we) : "", rn, pmatch ? "matches" : "DIFFERS");
    // futex, private and shared, on a word in it
    volatile uint32_t *fw = (volatile uint32_t *)(p + 0x3000);
    snprintf(item, sizeof item, "ahb.host.futex_private.%s", kind); R(item, futex_test(fw, 1, d, sizeof d) ? "PASS" : "FAIL", "%s", d);
    snprintf(item, sizeof item, "ahb.host.futex_shared.%s", kind); R(item, futex_test(fw, 0, d, sizeof d) ? "PASS" : "FAIL", "%s", d);
    // read() from a file into it (fsp-srv ReadFile: pread into guest memory), write() from it to a file
    char fp1[700], fp2[700]; snprintf(fp1, sizeof fp1, "%s/ahb_in.bin", g_outdir); snprintf(fp2, sizeof fp2, "%s/ahb_out.bin", g_outdir);
    size_t fl = 1 * MB + 3; uint8_t *src = malloc(fl); for (size_t i = 0; i < fl; i++) src[i] = (uint8_t)(h32((uint32_t)i ^ s) >> 7);
    int fd = open(fp1, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600); long wr0 = fd >= 0 ? write(fd, src, fl) : -1; if (fd >= 0) close(fd);
    size_t roff = 0x10003; long rd = -1; int re = 0, rmatch = 0; long gbad = -9;
    fd = open(fp1, O_RDONLY | O_CLOEXEC);
    if (fd >= 0 && wr0 == (long)fl) { rd = pread(fd, p + roff, fl, 0); re = rd < 0 ? errno : 0; close(fd); rmatch = rd == (long)fl && !memcmp(p + roff, src, fl); }
    if (rmatch && buf) {   // the GPU sees the kernel's copy (whole words 0x10004.. of the file data)
        stg_t st; if (!stg_new(&st, 2 * MB)) { VkBufferCopy rg = { 0x10000, 0, 2 * MB }; gbad = copy_regions(buf, st.b, 1, &rg) ? -3 : memcmp(st.p + roff - 0x10000, src, fl) != 0; stg_free(&st); }
    }
    snprintf(item, sizeof item, "ahb.host.read_file.%s", kind);
    R(item, rmatch && (!buf || gbad == 0) ? "PASS" : "FAIL", "pread of a 1 MB + 3 file to mapping + %#zx: %ld%s%s, data %s%s", roff, rd, re ? " errno " : "", re ? strerror(re) : "", rmatch ? "matches" : "DIFFERS",
      buf ? (gbad == 0 ? "; the GPU sees it" : "; the GPU does NOT see it") : "");
    fd = open(fp2, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600); long ww = -1, rb = -1; int wwe = 0, wmatch = 0;
    if (fd >= 0) { ww = write(fd, p + 0x5001, fl); wwe = ww < 0 ? errno : 0; rb = pread(fd, tmp, fl, 0); wmatch = ww == (long)fl && rb == (long)fl && !memcmp(tmp, p + 0x5001, fl); close(fd); }
    snprintf(item, sizeof item, "ahb.host.write_file.%s", kind);
    R(item, wmatch ? "PASS" : "FAIL", "write of 1 MB + 3 from mapping + 0x5001 to a file: %ld%s%s, read back %ld, data %s", ww, wwe ? " errno " : "", wwe ? strerror(wwe) : "", rb, wmatch ? "matches" : "DIFFERS");
    fd = open(fp1, O_RDONLY | O_DIRECT | O_CLOEXEC); long od = -2; int ode = 0, odm = 0;
    if (fd >= 0) { od = pread(fd, p + 0x20000, 64 * 1024, 0); ode = od < 0 ? errno : 0; odm = od == 64 * 1024 && !memcmp(p + 0x20000, src, 64 * 1024); close(fd); } else ode = errno;
    snprintf(item, sizeof item, "ahb.host.odirect.%s", kind);
    R(item, "INFO", "O_DIRECT pread of 64 KB into mapping + 0x20000: %ld%s%s%s (page pinning; the host does not use O_DIRECT)", od, ode ? " errno " : "", ode ? strerror(ode) : "", odm ? ", data matches" : "");
    unlink(fp1); unlink(fp2); free(src);
    // the guest's exclusives, and the host's LSE atomics
    volatile uint32_t *aw = (volatile uint32_t *)(p + 0x4000);
    snprintf(item, sizeof item, "ahb.host.llsc.%s", kind); R(item, atomics_test(aw, 0, d, sizeof d) ? "PASS" : "FAIL", "%s", d);
    if (getauxval(AT_HWCAP) & HWCAP_ATOMICS) { snprintf(item, sizeof item, "ahb.host.lse.%s", kind); R(item, atomics_test(aw + 16, 1, d, sizeof d) ? "PASS" : "FAIL", "%s", d); }
    else { snprintf(item, sizeof item, "ahb.host.lse.%s", kind); R(item, "SKIP", "no HWCAP_ATOMICS"); }
    free(tmp);
}
static void ahb_host_paths(void) {
    S("== (ahb 2b) the host's memory paths on an AHB mapping; controls: anonymous heap memory and a KGSL vkMapMemory mapping");
    size_t len = 4 * MB; uint32_t s = ov_seed();
    uint8_t *an = g_rb + 1 * GB; mprotect(an, len, PROT_READ | PROT_WRITE); cpu_fill_ov(an, len / 4, s, 0);
    host_paths_one("anon", an, len, VK_NULL_HANDLE, s); mmap(an, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    gmap_t g; if (!gmap_create(&g, len, NULL, 0)) { cpu_fill_ov(g.ptr, len / 4, s, 0); host_paths_one("kgsl", g.ptr, len, g.buf, s); } gmap_destroy(&g);
    apool_t p; uint8_t *at = g_rb + 1 * GB + 0x5000; int rc = apool_create(&p, at, len, 0);
    if (rc) { char e[300]; apool_errstr(&p, rc, e, sizeof e); R("ahb.host", "FAIL", "%s", e); return; }
    cpu_fill_ov(at, len / 4, s, 0); host_paths_one("ahb", at, len, p.buf, s); apool_end(&p, 0, "ahb.host");
    // control for the pipe read: an unreadable address returns EFAULT, no signal
    int pf[2]; volatile long wn = 0; volatile int we = 0; if (!pipe2(pf, O_CLOEXEC)) { int ok; GUARD_BEGIN(ok) wn = write(pf[1], g_res5 + g_res5_len - 2 * MB, 4096); we = errno; GUARD_END(ok); close(pf[0]); close(pf[1]); if (!ok) wn = -99; }
    R("ahb.host.pipe.prot_none", wn == -1 && we == EFAULT ? "PASS" : "FAIL", "write(pipe, PROT_NONE address, 4096) = %ld errno %s (want -1 EFAULT, no signal)", wn, strerror(we));
}

// ---------------------------------------------------------------- (3) splits, remaps, offsets, madvise
// single-page mprotects every 509 pages (odd offsets); leave_ro: every other one stays read-only. Returns the number of failed mprotect calls
// (*err: the last errno); a split test is valid only with 0 failures and more than one VMA in the range afterwards
static int split_pages(uint8_t *p, size_t np, int leave_ro, int *err) {
    int fails = 0;
    for (size_t i = 7, j = 0; i < np; i += 509, j++) {
        if (mprotect(p + i * g_pg, g_pg, PROT_READ)) { fails++; *err = errno; }
        if (!(leave_ro && (j & 1)) && mprotect(p + i * g_pg, g_pg, PROT_READ | PROT_WRITE)) { fails++; *err = errno; }
    }
    return fails;
}
static int unsplit(uint8_t *p, size_t len) { return mprotect(p, len, PROT_READ | PROT_WRITE) ? errno : 0; }
static const char *split_note(int fails, int err, int pieces, char *b, size_t n) {   // "" when the split happened; else why the test is invalid
    if (fails) snprintf(b, n, " -- INVALID: %d mprotect calls failed (errno %d)", fails, err);
    else if (pieces < 2) snprintf(b, n, " -- INVALID: the range is still %d VMA (no split happened)", pieces);
    else b[0] = 0;
    return b;
}
static void ahb_split(void) {
    S("== (ahb 3a) contents by offset after VMA splits, a remap and a nonzero fd offset (the KGSL split-VMA fault bug, probes-gpu.md surprise 1)");
    size_t len = 16 * MB, nw = len / 4, np = len / g_pg; uint8_t *at = g_rb + 512 * MB + 0x5000; apool_t p; char e[300], wh[400];
    int rc = apool_create(&p, at, len, AP_KEEP_FD);
    if (rc) { apool_errstr(&p, rc, e, sizeof e); R("ahb.split", "FAIL", "%s", e); return; }
    uint32_t s1 = ov_seed(); VkResult r = gpu_write_ov(p.buf, 0, len, s1, 0);   // GPU-written; the CPU has touched nothing
    int se = 0, sf = split_pages(at, np, 1, &se);
    range_info_t ri; range_info((uintptr_t)at, (uintptr_t)at + len, &ri); int nsplit = ri.nlines;
    char sn[120]; split_note(sf, se, nsplit, sn, sizeof sn); int split_ok = !*sn;
    // a CPU store into an untouched page of a later piece: where does it land in the object?
    size_t woff = 3001 * g_pg + 0x40; const uint32_t M = 0xDEADBEEFu; int ok; GUARD_BEGIN(ok) *(volatile uint32_t *)(at + woff) = M; GUARD_END(ok);
    long landed = -2;
    { stg_t st; if (!stg_new(&st, len)) { VkBufferCopy rg = { 0, 0, len }; if (!copy_regions(p.buf, st.b, 1, &rg)) { landed = -1; for (size_t k = 0; k < nw; k++) if (((volatile uint32_t *)st.p)[k] == M) { landed = (long)k * 4; break; } } stg_free(&st); } }
    R("ahb.split.cpu_write", split_ok && ok && landed == (long)woff ? "PASS" : "FAIL", "after %d-piece split, a CPU store meant for object offset %#zx landed at %#lx%s%s", nsplit, woff, landed, landed == (long)woff ? "" : " -- WRONG PLACE", sn);
    if (ok) *(volatile uint32_t *)(at + woff) = ov(s1, (uint32_t)(woff / 4));
    long zero, first; long bad = cpu_check_ov(at, nw, s1, 0, &zero, &first); wh[0] = 0;
    if (bad > 0) where_from(at, nw, s1, 0, first, wh, sizeof wh);
    R("ahb.split.read", split_ok && !r && bad == 0 ? "PASS" : "FAIL", "GPU-written 16 MB, %d VMA pieces after single-page mprotects (half left RO, %d mprotect failures), then the first CPU read of every word: %ld wrong (%ld zero)%s%s%s",
      nsplit, sf, bad, zero, bad ? " -- " : "", wh, sn);
    // the defrag remap (nx2android nvn_dirty_defrag): the same fd range mapped again over the fragmented range
    double t = now_s(); void *q = mmap(at, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, p.fd, 0); t = now_s() - t;
    range_info((uintptr_t)at, (uintptr_t)at + len, &ri);
    long bad2 = q == at ? cpu_check_ov(at, nw, s1, 0, &zero, &first) : -9;
    uint32_t s2 = ov_seed(); VkResult r2 = gpu_write_ov(p.buf, 0, len, s2, 0); long bad3 = r2 ? -3 : cpu_check_ov(at, nw, s2, 0, NULL, NULL);
    R("ahb.split.remap", split_ok && q == at && ri.nlines == 1 && !bad2 && !bad3 ? "PASS" : "FAIL", "mmap of the same fd over the %d pieces: %s in %.3f ms (%.1f ms/GB), now %d VMA; contents %ld wrong; then a GPU rewrite read by the CPU: %ld wrong",
      nsplit, q == at ? "ok" : strerror(errno), t * 1e3, t * 1e3 * GB / len, ri.nlines, bad2, bad3);
    // a mapping at a nonzero fd offset (vm_pgoff != 0), split before its first CPU touch
    size_t foff = 0x101000, l2 = len - foff; uint8_t *at2 = g_rb + 576 * MB;
    q = mmap(at2, l2, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, p.fd, (off_t)foff); int me = q == at2 ? 0 : errno;
    long bad4 = -9; ri.nlines = 0; wh[0] = 0; int sf4 = 0, se4 = 0; char sn4[120] = "";
    if (q == at2) { sf4 = split_pages(at2, l2 / g_pg, 1, &se4); range_info((uintptr_t)at2, (uintptr_t)at2 + l2, &ri); split_note(sf4, se4, ri.nlines, sn4, sizeof sn4);
                    bad4 = cpu_check_ov(at2, l2 / 4, s2, (uint32_t)(foff / 4), &zero, &first); if (bad4 > 0) where_from(at2, l2 / 4, s2, (uint32_t)(foff / 4), first, wh, sizeof wh); unsplit(at2, l2); }
    R("ahb.split.offset", q == at2 && !*sn4 && bad4 == 0 ? "PASS" : "FAIL", "the fd mapped at offset %#zx (%s), split into %d pieces before the first touch (%d mprotect failures): %ld words wrong%s%s%s",
      foff, me ? strerror(me) : "ok", ri.nlines, sf4, bad4, bad4 ? " -- " : "", wh, sn4);
    mmap(at2, l2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    // madvise on the mapping: what each returns, and the contents afterwards
    static const struct { int adv; const char *n; } A[] = { { 22, "POPULATE_READ" }, { 23, "POPULATE_WRITE" }, { 20, "COLD" }, { 21, "PAGEOUT" }, { MADV_FREE, "FREE" }, { MADV_DONTNEED, "DONTNEED" } };
    char md[600]; size_t u = 0; int mbad = 0; md[0] = 0;
    int sem = 0, sfm = split_pages(at, np, 0, &sem); range_info((uintptr_t)at, (uintptr_t)at + len, &ri); char snm[120]; split_note(sfm, sem, ri.nlines, snm, sizeof snm);
    for (size_t i = 0; i < sizeof A / sizeof *A; i++) {
        int mr = madvise(at, len, A[i].adv), mer = mr ? errno : 0; long b = mr ? 0 : cpu_check_ov(at, nw, s2, 0, NULL, NULL);
        if (b) mbad = 1;
        u += snprintf(md + u, u < sizeof md ? sizeof md - u : 0, "%s%s %s", i ? "; " : "", A[i].n, mr ? strerror(mer) : (b == 0 ? "0, contents ok" : (b < 0 ? "0, then a FAULT" : "0, then CONTENTS WRONG")));
    }
    R("ahb.split.madvise", mbad ? "FAIL" : "INFO", "on the split AHB mapping (%d VMAs): %s%s", ri.nlines, md, snm);
    apool_end(&p, 0, "ahb.split");
}

// ---------------------------------------------------------------- (3) VMA merging, 4,096 write faults, the dirty test
static void ahb_vmas(void) {
    char mm[64]; max_map_count_str(mm, sizeof mm);
    S("== (ahb 3b) do split AHB VMAs merge back? vm.max_map_count %s", mm);
    size_t len = 64 * MB, nw = len / 4; uint8_t *at = g_rb + 1 * GB + 0x2000; apool_t p; char e[300];
    int rc = apool_create(&p, at, len, AP_KEEP_FD);
    if (rc) { apool_errstr(&p, rc, e, sizeof e); R("ahb.vmas", "FAIL", "%s", e); return; }
    uint32_t s = ov_seed(); cpu_fill_ov(at, nw, s, 0);
    uintptr_t lo = (uintptr_t)at, hi = lo + len; int tot0, tot; int v0 = count_vmas(lo, hi, &tot0), fails = 0, er = 0;
    if (mprotect(at, len, PROT_READ)) { fails++; er = errno; }
    int v1 = count_vmas(lo, hi, &tot);
    double t0 = now_s(); for (int i = 0; i < 4096; i++) if (mprotect(at + (size_t)(4 * i + 1) * g_pg, g_pg, PROT_READ | PROT_WRITE)) { fails++; er = errno; } double t1 = now_s();
    int v2 = count_vmas(lo, hi, &tot); int tot2 = tot;
    if (mprotect(at, len, PROT_READ)) { fails++; er = errno; } int v3 = count_vmas(lo, hi, &tot);
    if (mprotect(at, len, PROT_READ | PROT_WRITE)) { fails++; er = errno; } int v4 = count_vmas(lo, hi, &tot);
    long cb = cpu_check_ov(at, nw, s, 0, NULL, NULL), gb = gpu_check_ov(p.buf, 0, len, s, 0);
    // valid only if every mprotect worked, the unprotects fragmented the range, and the data survived; then PASS = the pieces merged back
    int valid = !fails && v2 > v1 && !cb && !gb;
    R("ahb.vmas", valid && v4 <= 2 ? "PASS" : "FAIL", "64 MB AHB: VMAs %d -> RO %d -> 4,096 single-page unprotects %d (process %d, from %d) -> RO %d -> RW %d%s; %.2f us per mprotect; %d mprotect failures (errno %d); data CPU %ld / GPU %ld wrong%s",
      v0, v1, v2, tot2, tot0, v3, v4, !valid ? "" : v4 <= 2 ? " (merged back)" : " -- the pieces never merge", (t1 - t0) * 1e6 / 4096, fails, er, cb, gb,
      valid ? "" : " -- INVALID: an mprotect failed, the range did not fragment, or the data changed");
    double t = now_s(); void *q = mmap(at, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, p.fd, 0); t = now_s() - t;
    int v5 = count_vmas(lo, hi, &tot); long cb2 = q == at ? cpu_check_ov(at, nw, s, 0, NULL, NULL) : -9, gb2 = gpu_check_ov(p.buf, 0, len, s, 0);
    R("ahb.vmas.defrag", valid && v4 > 1 && q == at && v5 == 1 && !cb2 && !gb2 ? "PASS" : "FAIL", "the fd mapped again over the %d pieces: %s, %.2f ms (%.1f ms/GB); now %d VMA (process %d); data CPU %ld / GPU %ld wrong",
      v4, q == at ? "ok" : strerror(errno), t * 1e3, t * 1e3 * GB / len, v5, tot, cb2, gb2);
    apool_end(&p, 0, "ahb.vmas");
}
static void ahb_faults(void) {
    S("== (ahb 3c) 4,096 write faults (nvn_dirty.c's cycle) on an AHB mapping, then the defrag remap");
    size_t len = 32 * MB, nw = len / 4; uint8_t *at = g_rb + 1 * GB + 0x9000; apool_t p; char e[300];
    int rc = apool_create(&p, at, len, AP_KEEP_FD);
    if (rc) { apool_errstr(&p, rc, e, sizeof e); R("ahb.faults4096", "FAIL", "%s", e); return; }
    uint32_t s = ov_seed(); cpu_fill_ov(at, nw, s, 0);
    int tot0, tot; count_vmas(0, 0, &tot0);
    int mr = mprotect(at, len, PROT_READ);
    g_trk_n = 0; g_trk_mperr = 0; g_trk_lo = (uintptr_t)at; g_trk_hi = (uintptr_t)at + len;
    int ok; double t0 = now_s();
    GUARD_BEGIN(ok) for (int i = 0; i < 4096; i++) *(volatile uint32_t *)(at + (size_t)(2 * i) * g_pg + 0x40) = 0xF0000000u + (uint32_t)i; GUARD_END(ok);
    double tf = now_s() - t0; int nf = g_trk_n, mperr = g_trk_mperr; g_trk_lo = g_trk_hi = 0;
    int vr = count_vmas((uintptr_t)at, (uintptr_t)at + len, &tot);
    long landed = 0; for (int i = 0; i < 4096; i++) landed += *(volatile uint32_t *)(at + (size_t)(2 * i) * g_pg + 0x40) == 0xF0000000u + (uint32_t)i;
    // every other word must be untouched; the GPU must see the 4,096 stores
    long other = 0; for (size_t k = 0; k < nw; k++) { size_t pg = k * 4 / g_pg, o = k * 4 % g_pg; if (!(pg % 2 == 0 && o == 0x40)) other += ((volatile uint32_t *)at)[k] != ov(s, (uint32_t)k); }
    long gseen = -2; { stg_t st; if (!stg_new(&st, len)) { VkBufferCopy rg = { 0, 0, len }; if (!copy_regions(p.buf, st.b, 1, &rg)) { gseen = 0; for (int i = 0; i < 4096; i++) gseen += ((uint32_t *)st.p)[((size_t)(2 * i) * g_pg + 0x40) / 4] == 0xF0000000u + (uint32_t)i; } stg_free(&st); } }
    R("ahb.faults4096", ok && !mr && nf == 4096 && !mperr && landed == 4096 && !other && gseen == 4096 ? "PASS" : "FAIL",
      "32 MB AHB read-only, one store in every other page: %d faults (want 4096), %.2f us per fault, handler mprotect errno %d, %ld stores landed, %ld other words changed, GPU sees %ld; VMAs in the range %d, process %d (from %d)",
      nf, tf * 1e6 / (nf ? nf : 1), mperr, landed, other, gseen, vr, tot, tot0);
    // the whole-range RW mprotect first (a kernel that merged the pieces there would leave the remap nothing to defragment: count again after it)
    int ue = unsplit(at, len); int vu = count_vmas((uintptr_t)at, (uintptr_t)at + len, &tot);
    double t = now_s(); void *q = mmap(at, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, p.fd, 0); int qe = q == at ? 0 : errno; t = now_s() - t;
    int v5 = count_vmas((uintptr_t)at, (uintptr_t)at + len, &tot);
    volatile long cl = -1; int cok = 0;   // the contents are read only after a successful map, and under the fault guard
    if (q == at) { cl = 0; GUARD_BEGIN(cok) for (int i = 0; i < 4096; i++) cl += *(volatile uint32_t *)(at + (size_t)(2 * i) * g_pg + 0x40) == 0xF0000000u + (uint32_t)i; GUARD_END(cok); }
    const char *note = ue ? " -- the RW mprotect before it FAILED" : q != at ? " -- the remap FAILED, contents not read" : !cok ? " -- reading the contents FAULTED"
                     : vr <= 1 ? " -- INVALID: nothing was fragmented" : vu <= 1 ? " -- INVALID: the RW mprotect merged the pieces, nothing left to defragment" : "";
    R("ahb.faults4096.defrag", !ue && vr > 1 && vu > 1 && q == at && cok && v5 == 1 && cl == 4096 ? "PASS" : "FAIL",
      "remap of the fd over the pieces (%d after the faults, %d after the RW mprotect): %s in %.2f ms, now %d VMA (process %d), the 4,096 stores still there: %ld%s",
      vr, vu, qe ? strerror(qe) : "ok", t * 1e3, v5, tot, (long)cl, note);
    apool_end(&p, 0, "ahb.faults4096");
}
static void dirty_on(gmap_t *g, const char *kind);
static void ahb_dirty(void) {
    S("== (ahb 3d) the dirty-tracking test (6) on an AHB mapping");
    if (kernels_init()) { R("dirty.pipeline", "FAIL", "compute pipeline creation failed"); return; }
    apool_t p; uint8_t *at = g_rb + 1 * GB + 0xb000; int rc = apool_create(&p, at, 64 * MB, 0);
    if (rc) { char e[300]; apool_errstr(&p, rc, e, sizeof e); R("dirty.ahb", "FAIL", "%s", e); return; }
    gmap_t g = apool_gmap(&p); dirty_on(&g, "ahb");
    apool_end(&p, 0, "dirty.ahb");
}

// ---------------------------------------------------------------- (5) coherence, fresh GPU writes, BDA, small pools
static void ahb_coherence(void) {
    S("== (ahb 5) coherence without flush or invalidate on an AHB mapping (descriptors and buffer device address)");
    if (kernels_init()) { R("coh.pipeline", "FAIL", "compute pipeline creation failed"); return; }
    apool_t p; uint8_t *at = g_rb + 1 * GB + 0xd000; int rc = apool_create(&p, at, AHB_CAP, g_bda ? AP_BDA : 0);
    // rc 4: the import failed and its cleanup worked, so p is free to reuse; rc 5 (cleanup failed, pool alive) is never retried
    if (rc == 4 && g_bda) { char e[300]; apool_errstr(&p, rc, e, sizeof e); R("ahb.bda_import", "FAIL", "import with VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT: %s; retrying without", e); rc = apool_create(&p, at, AHB_CAP, 0); }
    if (rc) { char e[300]; apool_errstr(&p, rc, e, sizeof e); R("coh.ahb", "FAIL", "%s", e); return; }
    gmap_t g = apool_gmap(&p);
    S("  AHB pool at %p, buffer device address %#llx", at, (unsigned long long)p.addr);
    coherence_one(&g, "ahb.desc.noflush", 0, 0);
    if (g_bda && g_kb.ok && p.addr) { R("ahb.bda", "PASS", "buffer device address of the imported buffer: %#llx", (unsigned long long)p.addr); coherence_one(&g, "ahb.bda.noflush", 1, 0); }
    else R("ahb.bda", g_bda ? "FAIL" : "SKIP", "bufferDeviceAddress %s, address %#llx", g_bda ? "enabled" : "not enabled", (unsigned long long)p.addr);
    if (apool_end(&p, 0, "coh.ahb")) { R("ahb.fresh_fill", "FAIL", "not run: the previous pool at the same address could not be finalized"); return; }
    // the GPU fills a fresh pool the CPU never touched; the CPU reads it after the fence
    rc = apool_create(&p, at, 32 * MB, 0);
    if (!rc) {
        VkResult r = fill_buf(p.buf, 0, 32 * MB, 0xA5C3F00Fu); long bad = 0, zero = 0; int ok;
        GUARD_BEGIN(ok) for (size_t k = 0; k < 8 * MB; k++) { uint32_t x = ((volatile uint32_t *)at)[k]; if (x != 0xA5C3F00Fu) { bad++; zero += !x; } } GUARD_END(ok);
        R("ahb.fresh_fill", !r && ok && !bad ? "PASS" : "FAIL", "vkCmdFillBuffer over a fresh 32 MB pool the CPU never touched: %s, the CPU then reads %ld wrong words (%ld zero)%s", vkc_result_str(r), bad, zero, ok ? "" : " FAULT");
        apool_end(&p, 0, "ahb.fresh_fill");
    } else { char e[300]; apool_errstr(&p, rc, e, sizeof e); R("ahb.fresh_fill", "FAIL", "%s", e); }
}
// the fds a pool keeps open (listed once, for the first pool): the fds above the highest one open before it
static void list_new_fds(int maxfd0) {
    DIR *d = opendir("/proc/self/fd"); if (!d) return; struct dirent *e; char out[900]; size_t u = 0; out[0] = 0;
    while ((e = readdir(d))) {
        int fd = atoi(e->d_name); if (e->d_name[0] == '.' || fd <= maxfd0) continue;
        char path[64], tgt[160]; snprintf(path, sizeof path, "/proc/self/fd/%d", fd); ssize_t k = readlink(path, tgt, sizeof tgt - 1); tgt[k > 0 ? k : 0] = 0;
        char info[120]; fd_dmabuf_info(fd, info, sizeof info);
        u += snprintf(out + u, u < sizeof out ? sizeof out - u : 0, "%s%d -> %s%s%s", u ? "; " : "", fd, tgt, *info ? " " : "", info);
    }
    closedir(d); S("  the first pool's open fds: %s", out);
}
static int max_fd(void) { DIR *d = opendir("/proc/self/fd"); int m = -1; struct dirent *e; if (!d) return -1; while ((e = readdir(d))) if (e->d_name[0] != '.' && atoi(e->d_name) > m) m = atoi(e->d_name); closedir(d); return m; }
static void pools_run(const char *item, int maxn, size_t plen, int heavy) {
    S("== (ahb 4) %s: up to %d pools of %zu KB, each a dedicated import with buffer device address, back to back in the reservation", item, maxn, (size_t)(plen / 1024));
    apool_t *P = calloc((size_t)maxn, sizeof *P); if (!P) { R(item, "FAIL", "calloc"); return; }
    uint8_t *base = g_rb + 64 * MB; if ((size_t)maxn * plen > 4 * GB) maxn = (int)(4 * GB / plen);
    // heavy: stop before the process runs out of fds or VMAs (an allocate that cannot receive its fds is not provoked: the allocator service is shared)
    struct rlimit rl = { 0, 0 }; getrlimit(RLIMIT_NOFILE, &rl); const int vm_cap = 65530;
    int fd0 = count_fds(), vm0 = proc_vmas(), n = 0, rc = 0, mfd0 = max_fd(); double av0 = avail_gb(), t0 = now_s(); char stop[300] = "reached the cap";
    S("  RLIMIT_NOFILE soft %llu hard %llu; fds open %d, VMAs %d", (unsigned long long)rl.rlim_cur, (unsigned long long)rl.rlim_max, fd0, vm0);
    for (; n < maxn; n++) {
        if (heavy && (n & 255) == 0 && avail_gb() < g_ahb_floor) { snprintf(stop, sizeof stop, "MemAvailable %.2f GB below the %.1f GB floor", avail_gb(), g_ahb_floor); break; }
        if (heavy && (n & 63) == 0) {
            int f = count_fds(), v = (n & 255) == 0 ? proc_vmas() : 0;
            if (f >= 0 && (unsigned long long)f + 256 > (unsigned long long)rl.rlim_cur) { snprintf(stop, sizeof stop, "fd budget: %d fds open, RLIMIT_NOFILE %llu, %lld left (checked every 64 pools; stops once fewer than 256 remain)", f, (unsigned long long)rl.rlim_cur, (long long)rl.rlim_cur - f); break; }
            if (v + 1024 > vm_cap) { snprintf(stop, sizeof stop, "VMA budget: %d VMAs, max_map_count %d, %d left (checked every 256 pools; stops once fewer than 1024 remain)", v, vm_cap, vm_cap - v); break; }
        }
        rc = apool_create(&P[n], base + (size_t)n * plen, plen, g_bda ? AP_BDA : 0);
        if (rc) { char e[300]; apool_errstr(&P[n], rc, e, sizeof e); snprintf(stop, sizeof stop, "pool %d failed at %s", n, e); break; }
        if (n == 0) list_new_fds(mfd0);
        if (heavy && n && (n % 2048) == 0) S("  %d pools: %.2f ms each so far, MemAvailable %.2f GB, fds %d, VMAs %d", n, (now_s() - t0) * 1e3 / n, avail_gb(), count_fds(), proc_vmas());
    }
    double tc = now_s() - t0; int fd1 = count_fds(), vm1 = proc_vmas(); double av1 = avail_gb();
    // every pool: the CPU writes a word at its guest address, the GPU copies it out (one command buffer)
    long bad = -2; stg_t st;
    if (n && !stg_new(&st, ((size_t)n * 8 + 4095) & ~(size_t)4095)) {
        for (int i = 0; i < n; i++) *(volatile uint64_t *)P[i].at = 0x5100000000000000ull + (uint64_t)i;
        if (!vkc_cmd_begin(&V)) {
            for (int i = 0; i < n; i++) { VkBufferCopy rg = { 0, (VkDeviceSize)i * 8, 8 }; V.vkCmdCopyBuffer(V.cmd, P[i].buf, st.b, 1, &rg); }
            if (!cmd_end_host()) { bad = 0; for (int i = 0; i < n; i++) bad += ((volatile uint64_t *)st.p)[i] != 0x5100000000000000ull + (uint64_t)i; }
        }
        stg_free(&st);
    }
    // a pool whose finalize fails stays alive and is not touched again (its slot is freed with P, its objects are leaked on purpose)
    int nfin = 0; double t1 = now_s(); for (int i = 0; i < n; i++) if (apool_finalize(&P[i], 0)) nfin++; double tf = now_s() - t1;
    g_ahb_alive += nfin;
    free(P);
    char left[160] = ""; if (nfin || rc == 5) snprintf(left, sizeof left, " -- %d pool(s) could not be finalized%s and were left alive", nfin, rc == 5 ? " (plus the failed one)" : "");
    R(item, (heavy || n == maxn) && bad == 0 && !nfin && rc != 5 ? (heavy ? "INFO" : "PASS") : "FAIL",
      "%d pools of %zu KB (%s): %.3f ms per pool to create, %.3f to free; per pool %.2f fds, %.2f VMAs, %.1f KB of MemAvailable; the GPU read every pool's CPU-written word: %ld wrong; maxMemoryAllocationCount %u%s",
      n, plen / 1024, stop, n ? tc * 1e3 / n : 0, n ? tf * 1e3 / n : 0, n ? (double)(fd1 - fd0) / n : 0, n ? (double)(vm1 - vm0) / n : 0, n ? (av0 - av1) * 1048576.0 / n : 0, bad, V.props.limits.maxMemoryAllocationCount, left);
}

// ---------------------------------------------------------------- the suite
static void ahb_suite(void) {
    S("== AHardwareBuffer-backed pools (method from shirai91/nx2android, MIT)");
    ahb_caps();
    if (!g_ahb_ok) { R("ahb", "SKIP", "the AHB import extension is not available"); return; }
    install_fault_handler();
    static void (*const step[])(void) = { ahb_chain_reserve, ahb_chain_heap, ahb_populate, ahb_host_paths, ahb_split, ahb_vmas, ahb_faults, ahb_dirty, ahb_coherence };
    for (size_t i = 0; i < sizeof step / sizeof *step; i++) {
        if (g_ahb_dead) { R("ahb.stopped", "FAIL", "an AHardwareBuffer_allocate returned DEAD_OBJECT / -129 (the allocator service died or refused): the remaining AHB tests were not run"); return; }
        if (g_ahb_alive) { R("ahb.stopped", "FAIL", "%d pool(s) could not be finalized and were left alive (see the FAIL above): the remaining AHB tests were not run", g_ahb_alive); return; }
        step[i]();
    }
    if (g_ahb_alive) { R("ahb.stopped", "FAIL", "%d pool(s) could not be finalized and were left alive (see the FAIL above): ahb.pools256 was not run", g_ahb_alive); return; }
    if (!g_ahb_dead) pools_run("ahb.pools256", 256, 64 * 1024, 0);
}

// ---------------------------------------------------------------- bandwidth
static void bw_pairs(bwk_t *k, int nk, size_t len, int reps, const char *pfx);
static void ahb_bw(int reps) {
    if (reps > BW_MAX) reps = BW_MAX;
    if (!g_ahb_ok) { R("ahb.bw", "SKIP", "no AHB import"); return; }
    size_t len = AHB_CAP;
    S("== (ahb bw) CPU bandwidth, AHB mapping at the guest address vs anonymous memory, %zu MB buffers, %d paired repetitions", (size_t)(len / MB), reps);
    bwk_t k[2]; memset(k, 0, sizeof k); apool_t pa, pb;
    k[0].name = "anon"; k[0].a = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); k[0].b = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (k[0].a == MAP_FAILED || k[0].b == MAP_FAILED) { R("ahb.bw.anon", "FAIL", "anonymous mmap failed"); return; }
    uint8_t *at = g_rb + 64 * MB + 0x1000; int r1 = apool_create(&pa, at, len, 0), r2 = r1 ? -1 : apool_create(&pb, at + len + 0x1000, len, 0);
    if (r1 || r2) { char e[300]; apool_errstr(r1 ? &pa : &pb, r1 ? r1 : r2, e, sizeof e); R("ahb.bw.ahb", "FAIL", "%s", e); if (!r1) apool_end(&pa, 0, "ahb.bw"); munmap(k[0].a, len); munmap(k[0].b, len); return; }
    k[1].name = "ahb"; k[1].a = pa.at; k[1].b = pb.at;
    bw_pairs(k, 2, len, reps, "ahb.bw.");
    munmap(k[0].a, len); munmap(k[0].b, len); apool_end(&pa, 0, "ahb.bw"); apool_end(&pb, 0, "ahb.bw");
}

// ---------------------------------------------------------------- heavy: size, many, churn (APK only)
static int heavy_ok(const char *item) {
    if (!g_in_app) { R(item, "SKIP", "allocation-heavy: run in the APK only (an adb shell process has oom_score_adj -1000)"); return 0; }
    if (!g_ahb_ok) { R(item, "SKIP", "no AHB import"); return 0; }
    return 1;
}
static void ahb_size(void) {
    if (!heavy_ok("ahb.size")) return;
    S("== (ahb size) pool sizes up to the %zu MB cap: allocate, map, import, check; MemAvailable floor %.1f GB", (size_t)(AHB_CAP / MB), g_ahb_floor);
    R("ahb.size.above_cap", "SKIP", "sizes above %zu MB (256 MB ... 4 GB - 4 KB) not tried: an AHardwareBuffer_isSupported query above 64 MB (1 GB or 2 GB) aborted the system allocator service "
      "and restarted surfaceflinger and the framework on SM-F966B; finding the real limit means more such restarts", (size_t)(AHB_CAP / MB));
    static const uint64_t sz[] = { 16 * MB, 32 * MB, AHB_CAP - 4096, AHB_CAP };
    uint64_t best = 0; char last[300] = "";
    for (size_t i = 0; i < sizeof sz / sizeof *sz; i++) {
        size_t len = (size_t)sz[i]; char item[48]; snprintf(item, sizeof item, "ahb.size.%zuKB", (size_t)(len / 1024));
        if (g_ahb_dead) { snprintf(last, sizeof last, "stopped: the allocator service failed"); break; }
        AHardwareBuffer_Desc d; if (ahb_alloc_desc((uint32_t)len, 1, &d)) continue; int sup = AHardwareBuffer_isSupported(&d);
        for (int w = 0; w < 5 && avail_gb() < g_ahb_floor + len / (double)GB; w++) sleep(1);
        double av = avail_gb();
        if (av - len / (double)GB < g_ahb_floor) { R(item, "INFO", "isSupported %d; not allocated: MemAvailable %.2f GB, the floor is %.1f GB", sup, av, g_ahb_floor); snprintf(last, sizeof last, "%zu MB not tried (memory)", (size_t)(len / MB)); break; }
        apool_t p; uint8_t *at = g_rb + 64 * MB; double t = now_s();
        int rc = apool_create(&p, at, len, 0); double tc = now_s() - t, av1 = avail_gb();
        if (rc) { char e[300]; apool_errstr(&p, rc, e, sizeof e); R(item, "FAIL", "isSupported %d; %s (MemAvailable %.2f GB)", sup, e, av); snprintf(last, sizeof last, "%zu MB failed: %s", (size_t)(len / MB), e); break; }
        uint32_t s = ov_seed(); t = now_s(); int ok; GUARD_BEGIN(ok) for (size_t o = 0; o < len; o += g_pg) { ((volatile uint32_t *)(at + o))[0] = ov(s, (uint32_t)(o / 4)); ((volatile uint32_t *)(at + o))[1] = ov(s, (uint32_t)(o / 4) + 1); } GUARD_END(ok);
        double tt = now_s() - t; size_t checked = 0; long gb = ok ? gpu_check_ov_pages(p.buf, p.buf_size, s, 0, &checked) : -9;
        VkResult fr = fill_buf(p.buf, p.buf_size - 4096, 4096, 0x7e57f111u); long fb = 0; for (size_t k = 0; k < 1024; k++) fb += ((volatile uint32_t *)(at + p.buf_size - 4096))[k] != 0x7e57f111u;
        int pass = ok && gb == 0 && !fr && !fb;
        R(item, pass ? "PASS" : "FAIL", "isSupported %d; created in %.0f ms (MemAvailable %.2f -> %.2f GB), dma-buf %lld bytes, allocationSize %#llx, VkBuffer %#llx%s; CPU wrote every page (%.0f ms), GPU read %zu sampled pages: %ld wrong; GPU fill at the end read by the CPU: %ld wrong",
          sup, tc * 1e3, av, av1, (long long)p.fd_size, (unsigned long long)p.alloc_size, (unsigned long long)p.buf_size, p.buf_size < len ? " (capped at maxBufferSize)" : "", tt * 1e3, checked, gb, fb);
        if (apool_end(&p, 0, item)) { snprintf(last, sizeof last, "%zu MB could not be finalized (left alive)", (size_t)(len / MB)); break; }
        if (pass) best = len; else { snprintf(last, sizeof last, "%zu MB failed its checks", (size_t)(len / MB)); break; }
        for (int w = 0; w < 10 && avail_gb() < av - 0.2; w++) usleep(500000);
    }
    R("ahb.size.largest", best ? "INFO" : "FAIL", "largest pool allocated, mapped, imported and checked: %llu KB (%#llx), with nothing above the %zu MB cap tried%s%s",
      (unsigned long long)(best / 1024), (unsigned long long)best, (size_t)(AHB_CAP / MB), *last ? "; then: " : "", last);
}
static void ahb_many(int maxn) {
    if (!heavy_ok("ahb.many")) return;
    pools_run("ahb.many", maxn, 16 * 1024, 1);
}
static void ahb_churn(int cycles) {
    if (!heavy_ok("ahb.churn")) return;
    // the growth check compares the end with a baseline taken after cycle `warm` (half the cycles, at most 20), so it needs 2 cycles at least
    if (cycles < 2) { R("ahb.churn", "SKIP", "--ahb-churn-cycles %d: at least 2 cycles are needed (the leak check compares the end with a baseline taken after the first half)", cycles); return; }
    size_t len = 16 * MB, blen = 64 * MB; uint8_t *base = g_rb + 1 * GB;
    S("== (ahb churn) %d cycles of pool init (copy out, map, copy back, import), a GPU check, finalize (back to anonymous with the contents), free; %zu MB pools", cycles, (size_t)(len / MB));
    if (mprotect(base, blen, PROT_READ | PROT_WRITE)) { R("ahb.churn", "FAIL", "mprotect"); return; }
    uint32_t s = ov_seed(); cpu_fill_ov(base, blen / 4, s, 0);
    sleep(1); int nk0, nk1; size_t k0 = kgsl_mapped(&nk0), db0, db1; int nd0 = dmabuf_vmas(&db0), fd0 = count_fds(); double av0 = avail_gb(), tin = 0, tfin = 0, minav = av0;
    // resource growth is judged against a warmed baseline (after cycle `warm`: the driver's own first-use mappings and fds come before it)
    int i = 0, bad = 0, notanon = 0, warm = cycles >= 40 ? 20 : cycles / 2, fdw = -1, ndw = -1; size_t dbw = 0; char stop[300] = "all cycles";
    for (; i < cycles; i++) {
        double av = avail_gb(); if (av < minav) minav = av;
        if (av < g_ahb_floor) { snprintf(stop, sizeof stop, "stopped at cycle %d: MemAvailable %.2f GB below the floor", i, av); break; }
        size_t poff = 8 * MB + (size_t)(i % 16) * 0x1000; apool_t p; double t = now_s();
        int rc = apool_create(&p, base + poff, len, AP_KEEP); tin += now_s() - t;
        if (rc) { char e[300]; apool_errstr(&p, rc, e, sizeof e); snprintf(stop, sizeof stop, "cycle %d: %s", i, e); break; }
        size_t checked; long gb = gpu_check_ov_pages(p.buf, len, s, (uint32_t)(poff / 4), &checked); if (gb) bad++;
        t = now_s(); int fr = apool_finalize(&p, 1); tfin += now_s() - t;
        if (fr) { g_ahb_alive++; snprintf(stop, sizeof stop, "cycle %d: finalize failed (%d: %s), the pool was left alive", i, fr, fr == -1 ? "malloc of the save buffer" : "anonymous MAP_FIXED"); break; }
        { range_info_t ri; range_info((uintptr_t)base, (uintptr_t)base + blen, &ri); if (!ri.covered || strstr(ri.lines, "dmabuf")) notanon++; }   // the range is anonymous again
        if (i + 1 == warm) { fdw = count_fds(); ndw = dmabuf_vmas(&dbw); }
        if (i % 20 == 19) { int n; size_t kb = kgsl_mapped(&n), db; int nd = dmabuf_vmas(&db); S("  %d cycles: MemAvailable %.2f GB, KGSL mappings %zu MB in %d VMAs, dmabuf mappings %d, fds %d", i + 1, avail_gb(), (size_t)(kb / MB), n, nd, count_fds()); }
    }
    long cb = cpu_check_ov(base, blen / 4, s, 0, NULL, NULL);
    double t0 = now_s(), av = avail_gb(); while (now_s() - t0 < 8 && av < av0 - 0.1) { usleep(250000); av = avail_gb(); }
    size_t k1 = kgsl_mapped(&nk1); int nd1 = dmabuf_vmas(&db1), fd1 = count_fds();
    // a leak of one fd or one dma-buf mapping per cycle would add 80 after the warm-up; 2 of slack covers an unrelated fd of another thread
    int grow_ok = fdw >= 0 && ndw >= 0 && fd1 - fdw <= 2 && nd1 - ndw <= 2;
    R("ahb.churn", !bad && !cb && !notanon && grow_ok && i == cycles ? "PASS" : "FAIL",
      "%d cycles of a %zu MB pool (%s): init %.2f ms, finalize %.2f ms per cycle (%.2f / %.2f ms per MB); GPU checks wrong in %d cycles; range not anonymous after finalize in %d cycles; heap data afterwards %ld wrong; MemAvailable %.2f -> lowest %.2f -> %.2f GB (8 s later); KGSL mappings %zu -> %zu MB (%d -> %d VMAs); dmabuf mappings %d -> %d after cycle %d -> %d at the end; fds %d -> %d after cycle %d -> %d at the end%s",
      i, (size_t)(len / MB), stop, i ? tin * 1e3 / i : 0, i ? tfin * 1e3 / i : 0, i ? tin * 1e3 / i / ((size_t)(len / MB)) : 0, i ? tfin * 1e3 / i / ((size_t)(len / MB)) : 0, bad, notanon, cb, av0, minav, av,
      (size_t)(k0 / MB), (size_t)(k1 / MB), nk0, nk1, nd0, ndw, warm, nd1, fd0, fdw, warm, fd1, grow_ok ? "" : " -- GROWTH after the warm-up (a leak), or the warm-up was not reached");
    mmap(base, blen, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
}
