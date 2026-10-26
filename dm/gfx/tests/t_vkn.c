// The Vulkan backend's replay (dm/gfx/vkn, #included) on a real device with no window (make check_t_vkn: KosmicKrisp and the validation layer, as dm/android/phase2's Mac runs):
// - submissions that end with a recorded clear, or with a pass still open: mtl_sub_commit lands them through the submission's command buffer (it used to clear g_cur first and crash in cb_now);
// - a full clear inside a pass narrower than its target (a 64x64 colour target with 128x128 depth, and the other way round): NVN clears the whole target, a vkCmdClearAttachments in the pass
//   reaches its area only, so the pass ends and the clear goes to the next pass's start (op_clear_* oversize_mid);
// - a DM_NVN_SHOT screenshot (vk_present.c shot_one) is the picture the right way up: NVN's lower-left origin stores the window image bottom-up and the file is written top row first;
// - strategy B (PLAN.md 2.2 "As built, slice 2a"): bytes written into a CPU-visible pool reach its shadow buffer (read back on the GPU), a rewrite uploads only the pages written, and a pool deleted while
//   a submission that read it is in flight (held back on the GPU by a semaphore the test signals) has its pages writable at once and its shadow destroyed only after that submission completes.
//   Two pools sharing a host page (16 KB pages; skipped at 4 KB, where 4 KB-aligned pools share none): an address there resolves to the pool whose bytes hold it, the page is protected by
//   neither and both shadows get it at every submission. The real write tracking is linked in (nvn_dirty.c, and host/crash.c for its fault handler), as in the host. A write whose fault's
//   unprotect is under way at the commit's stamp (held between its BUSY and its generation increment by the interposed mprotect below, the stamp read one ahead) is picked by the commit's
//   re-scan after its drain, reaches the shadow, and its page is protected again;
// - sampled textures (PLAN.md 2.3 "As built, slice 2b"): a CPU-written RGBA8 texture with a mip chain, swizzled into a pool as the guest lays it out, bound and drawn with, reads back level by level as
//   written (uploaded through the CPU deswizzle); it is uploaded again only after a guest write (one into its own bytes; one beside it in a page it shares is only compared), its pages protected;
//   one nothing wrote is cleared to zero; a format this slice does not upload gets no image; on 16 KB hosts one in an edge page of two pools is never protected and uploaded at every use;
//   ReadTexels reads it from guest memory; a render target later registered as a texture is one image, its layout moved from rendered to sampled ahead of the submission and within one;
//   samplers for every filter and wrap mode (and anisotropy, compare, reduction, border, bias) are made, the same parameters sharing one;
//   a write whose page generation the tracker stores after a check has read the moved generation (staged through t_dirty_now) is still uploaded at the next use; a LINEAR texture the CPU
//   writes straight into its pool is uploaded and tracked, and stride 0 means nvn_tex_layout's row pitch; storage the CPU flushed, or a CPU_NO_ACCESS pool's initial content, fills a
//   texture once (a COMPRESSIBLE one there is cleared to zero); an RGBX view and a biased sampler are valid, and under a portability subset without those features they fall back;
//   a pool read by a commit before its write tracking starts (hle_nvn.c adds the range just after mtl_pool_add) has a write made then reach the shadow at the next commit (the span gap),
//   also when the range appears in the middle of a commit and a texture's upload is the first to find it (staged through t_dirty_span);
// - draws (PLAN.md 2.5 "As built, slice 2c"): test modules (vkn_kernels/, t_vkn_spv.h) under made-up keys with slice 0's reflection, their bindings moved into the one push-descriptor set;
//   the orientation matrix (origin x NEGATIVE_Y x front face x cull, an asymmetric triangle) read back, a viewport off the origin and short of the target and a partial scissor with either
//   origin and NEGATIVE_Y, the target then sampled in the next pass of the same submission and by the next submission; a CPU-written
//   texture through the pushed descriptors and a dummy for an unbound slot; uniform buffers at an aligned address, at one the device cannot bind (copied into the ring) and cut to the pool's
//   end; a vertex input nothing feeds; base vertex; near at -w and at 0; pipeline cache hits and misses; refused modules, and a pair past the push-descriptor limit, skipped; a stencil
//   view of a depth-stencil target sampled as unsigned integers; a linear sampler on an integer binding (the unsigned dummy, the stencil view) sampled through its nearest twin; indices, and
//   the vertices of a draw without indices, that run past their pool's end, skipped; a module declaring InterpolationFunction made only with sampleRateShading; alpha to coverage off for a
//   pixel shader writing no colour 0; a pair whose pixel shader reads a location the vertex shader does not write skipped; a vertex shader's SPIR-V under a pixel reflection, a module whose
//   bytes are not its reflection's (spv_sha256), slots past their
//   range or not integers, sampler arrays (count 1 or null) and a reflection cut inside a string refused; polygon mode LINE filled for want of fillModeNonSolid with the line offset applied;
//   signed integers (the reflection's sint) through a vertex input nothing feeds, a flat varying and an isampler2D; pairs matched by interface variable (a vec4[2] read is drawn with a
//   vec4[2] written, refused with two vec4s, and a vec4[2] written refused with one vec4 read) and arrays the reflection does not describe refused; alpha to coverage off for a pixel
//   shader whose location 0 is Index 1; a screenshot of a drawn corner with either origin;
// - no validation message. Each target is read back with a copy to a host-visible buffer.
// T_VKN_STRICT=1 (make check_t_vkn) never skips: no device, validation off or any failure to start is a failure. Without it (make run) a machine with no Vulkan at all (no loader, or a
// loader with no driver: vd.absent) is said to be SKIPPED and passes; a Vulkan that is there but fails to start fails all the same, and validation off is said (its check passes unchecked)
// nvn_dirty_now reaches the backend through t_dirty_now (below), so a check can be staged in the window between nvn_dirty.c's generation increment and the page's generation store;
// nvn_dirty_span through t_dirty_span, so a pool's range can be made to appear in the middle of a commit (between its buffer pass and its texture uploads)
#define nvn_dirty_now t_dirty_now
#define nvn_dirty_span t_dirty_span
#include "../vkn/nvn_vk.c"
#undef nvn_dirty_now
#undef nvn_dirty_span
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include "t_vkn_spv.h"   // the draws' test shaders (vkn_kernels/gen_spv.sh)
#include "../../host/plat/sha256.h"   // the modules' spv_sha256, as dmsh_build_vk.py writes it

// What the backend needs of the host and of hle_nvn.c
void dm_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); }
int nvn_game_fps(void) { return 60; }
uint64_t guest_ns(void) { return 0; }
module_t g_mod[MAX_MOD]; int g_nmod;   // host/crash.c's (as t_dirty_slots)
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);
void *front_metal_layer(void) { return NULL; }
void *front_native_window(void) { return NULL; }
_Atomic uint64_t dm_present_ns;
uint64_t now_ns(void) { return mono_ns(); }
static uint8_t *g_tflush_lo, *g_tflush_hi;   // the one range the test says the CPU flushed (hle_nvn.c's nvn_flushed keeps every nvnMemoryPoolFlushMappedRange)
int nvn_flushed(const void *p, uint64_t n) { const uint8_t *a = p; return g_tflush_lo && a < g_tflush_hi && a + n > g_tflush_lo; }
uint64_t nvn_dirty_now(void);
static _Atomic uint64_t g_now_ahead;   // what the backend reads is this far ahead of the tracker's generation (0: the same)
uint64_t t_dirty_now(void) { return nvn_dirty_now() + atomic_load(&g_now_ahead); }
const void *nvn_dirty_span(const void *addr, size_t len);
// Armed with a range (g_span_late, g_span_late_n), the next lookup of exactly that range registers it with the tracker and answers NULL (one shot): the range appears just after the backend
// looked, as when hle_nvn.c's nvn_dirty_add_range (on the game's thread, after mtl_pool_add) lands in the middle of a commit
static _Atomic uintptr_t g_span_late; static size_t g_span_late_n;
const void *t_dirty_span(const void *addr, size_t len) {
    uintptr_t a = atomic_load(&g_span_late);
    if (a && (uintptr_t)addr == a && len == g_span_late_n && atomic_compare_exchange_strong(&g_span_late, &a, 0)) { nvn_dirty_add_range(addr, len); return NULL; }
    return nvn_dirty_span(addr, len);
}

// mprotect, interposed: nvn_dirty.c is linked into this executable, so its calls take this definition (the real one is the next image's, resolved before main: dlsym is no signal handler's
// call). Armed with a page (g_mp_hold), the next set-writable call covering it waits g_mp_ms first (one shot; nanosleep and atomics only: it runs in the fault handler): a write fault's
// unprotect (nvn_dirty.c unprotect_page) is held after its state change PROT -> BUSY and before its generation increment, with g_mp_held saying so
static int (*g_real_mprotect)(void *, size_t, int);
static _Atomic uintptr_t g_mp_hold; static _Atomic int g_mp_held; static _Atomic uint64_t g_mp_n; static const long g_mp_ms = 300;
__attribute__((constructor)) static void mp_resolve(void) { g_real_mprotect = (int (*)(void *, size_t, int))dlsym(RTLD_NEXT, "mprotect"); }
int mprotect(void *a, size_t n, int prot) {
    atomic_fetch_add(&g_mp_n, 1);
    uintptr_t h = atomic_load(&g_mp_hold);
    if (h && (prot & PROT_WRITE) && h >= (uintptr_t)a && h < (uintptr_t)a + n && atomic_compare_exchange_strong(&g_mp_hold, &h, 0)) {
        atomic_store(&g_mp_held, 1);
        struct timespec ts = {0, g_mp_ms * 1000000L}; while (nanosleep(&ts, &ts) && errno == EINTR) {}
    }
    if (!g_real_mprotect) { errno = ENOSYS; return -1; }
    return g_real_mprotect(a, n, prot);
}
static void *t_writer(void *p) { *(volatile uint8_t *)p = 0x5a; return NULL; }   // one store, from another thread: its fault is the one held

static int checks, failed;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

// ---------- Records ----------
static uint8_t g_rec[32768]; static size_t g_rn;
static void rec(uint32_t op, const void *a, uint32_t n) { op_hdr h = {op, 8 + ((n + 7) & ~7u)}; memcpy(g_rec + g_rn, &h, 8); memset(g_rec + g_rn + 8, 0, h.size - 8); if (n) memcpy(g_rec + g_rn + 8, a, n); g_rn += h.size; }
static void rec_rt(const tex_t *c, const tex_t *d) { p_set_rt r; memset(&r, 0, sizeof r); r.n = c ? 1 : 0; r.color[0] = c; r.depth = d; rec(OP_SET_RT, &r, sizeof r); }
static void rec_clear(float r, float g, float b, float a) { p_clear_color c = {0, 0xf, {r, g, b, a}}; rec(OP_CLEAR_COLOR, &c, sizeof c); }
static void rec_clear_d(float d) { p_clear_ds c = {d, 1, 0, 0}; rec(OP_CLEAR_DS, &c, sizeof c); }
static void rec_draw(void) { p_draw d; memset(&d, 0, sizeof d); d.prim = 4; d.count = 3; rec(OP_DRAW, &d, sizeof d); }
static void submit(void) { rec(OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(g_rec); mtl_sub_commit(); g_rn = 0; }

static tex_t *tex(unsigned w, unsigned h, int fmt) {
    tex_t *t = calloc(1, sizeof *t); t->d.width = w; t->d.height = h; t->d.depth = 1; t->d.levels = 1; t->d.samples = 0; t->d.target = NVN_TEXTURE_TARGET_2D; t->d.format = fmt;
    t->serial = nvn_new_serial(); mtl_tex_init(t);   // live; its host image is made at SET_RT (no pool: not a CPU-no-access one)
    return t;
}
uint64_t nvn_new_serial(void) { static uint64_t n; return ++n; }

// ---------- Readback: texel (x, y) of the target's level 0 (4 bytes: RGBA8 or D32_SFLOAT), the queue idle around it. Its own functions (the backend's table has no buffers yet) ----------
#define T_FN(X) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkResetCommandBuffer) X(vkCmdCopyImageToBuffer) X(vkQueueSubmit) X(vkSignalSemaphore)
#define T_PFN(n) static PFN_##n t_##n;
T_FN(T_PFN)
static VkCommandPool g_rpool; static VkCommandBuffer g_rcb; static VkBuffer g_rbuf; static VkDeviceMemory g_rmem; static void *g_rmap;
static int rb_init(void) {
#define T_GET(n) if (!(t_##n = (PFN_##n)vk.vkGetDeviceProcAddr(vd.dev, #n))) return 0;
    T_FN(T_GET)
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, vd.qfam};
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (vk.vkCreateCommandPool(vd.dev, &pi, NULL, &g_rpool)) return 0;
    ai.commandPool = g_rpool; if (vk.vkAllocateCommandBuffers(vd.dev, &ai, &g_rcb)) return 0;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (t_vkCreateBuffer(vd.dev, &bi, NULL, &g_rbuf)) return 0;
    VkMemoryRequirements mr; t_vkGetBufferMemoryRequirements(vd.dev, g_rbuf, &mr);
    int mt = vkn_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); if (mt < 0) return 0;
    VkMemoryAllocateInfo mi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, (uint32_t)mt};
    if (vk.vkAllocateMemory(vd.dev, &mi, NULL, &g_rmem) || t_vkBindBufferMemory(vd.dev, g_rbuf, g_rmem, 0) || t_vkMapMemory(vd.dev, g_rmem, 0, 4, 0, &g_rmap)) return 0;
    return 1;
}
// A buffer's bytes [off, off+n) read back on the GPU into g_xmap (n up to XCAP), the queue idle around it. 1 = read
#define XCAP (1u << 20)
static VkBuffer g_xbuf; static VkDeviceMemory g_xmem; static uint8_t *g_xmap;
static int xb_init(void) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, XCAP, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (t_vkCreateBuffer(vd.dev, &bi, NULL, &g_xbuf)) return 0;
    VkMemoryRequirements mr; t_vkGetBufferMemoryRequirements(vd.dev, g_xbuf, &mr);
    int mt = vkn_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); if (mt < 0) return 0;
    VkMemoryAllocateInfo mi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, (uint32_t)mt}; void *m;
    if (vk.vkAllocateMemory(vd.dev, &mi, NULL, &g_xmem) || t_vkBindBufferMemory(vd.dev, g_xbuf, g_xmem, 0) || t_vkMapMemory(vd.dev, g_xmem, 0, XCAP, 0, &m)) return 0;
    g_xmap = m; return 1;
}
static int buf_read(VkBuffer src, VkDeviceSize off, VkDeviceSize n) {
    if (!src || n > XCAP) return 0;
    mtl_finish();
    pthread_mutex_lock(&vkn_qmu);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    t_vkResetCommandBuffer(g_rcb, 0); vk.vkBeginCommandBuffer(g_rcb, &bi);
    VkMemoryBarrier2 b0 = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
    VkDependencyInfo d0 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &b0, 0, NULL, 0, NULL}; vk.vkCmdPipelineBarrier2(g_rcb, &d0);
    VkBufferCopy r = {off, 0, n}; vk.vkCmdCopyBuffer(g_rcb, src, g_xbuf, 1, &r);
    VkMemoryBarrier2 b1 = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo d1 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &b1, 0, NULL, 0, NULL}; vk.vkCmdPipelineBarrier2(g_rcb, &d1);
    vk.vkEndCommandBuffer(g_rcb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &g_rcb, 0, NULL};
    int ok = !t_vkQueueSubmit(vd.q, 1, &si, VK_NULL_HANDLE) && !vk.vkQueueWaitIdle(vd.q);
    pthread_mutex_unlock(&vkn_qmu);
    return ok;
}
// A CPU-visible test pool of n bytes (whole host pages, so it shares none with another), registered as hle_nvn.c's MemoryPoolInitialize does; freed as its Finalize does (the memory stays mapped)
static uint8_t *pool_new(size_t n) {
    void *m = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); if (m == MAP_FAILED) return NULL;
    mtl_pool_add(m, n); nvn_dirty_add_range(m, n); return m;
}
static void pool_fin(uint8_t *m) { nvn_dirty_remove_range(m); mtl_pool_del(m); }
static void rec_ubo(const void *p, uint64_t n) { p_bind_buf u = {0, 0, (uint64_t)(uintptr_t)p, n}; rec(OP_BIND_UBO, &u, sizeof u); }
static uint64_t b_pages(int why) { return atomic_load(&g_bs_pg[why]); }   // nvn_vk.c b_commit's counts: 0 written since the last commit, 1 read for the first time, 2 copied every time
static uint64_t faults(void) { uint64_t f, p; nvn_dirty_stats(&f, &p); return f; }
static int shadow_of(const void *p, uint64_t n, VkBuffer *b, uint64_t *off, uint64_t *gpu) { void *mb; int ok = mtl_gpu_addr((uint64_t)(uintptr_t)p, n, &mb, off, gpu); *b = ok ? (VkBuffer)mb : VK_NULL_HANDLE; return ok; }
static int writable(uint8_t *p, size_t n) {   // a write by the kernel (read from /dev/zero): EFAULT on a read-only page, where a store would fault
    int fd = open("/dev/zero", O_RDONLY); if (fd < 0) return 0;
    int ok = 1; size_t pg = (size_t)getpagesize();
    for (size_t o = 0; o < n && ok; o += pg) ok = read(fd, p + o, pg) == (ssize_t)pg;
    close(fd); return ok;
}

static uint32_t texel(const tex_t *t, int x, int y) {
    mtl_finish();
    pthread_mutex_lock(&vkn_qmu); pthread_mutex_lock(&vkn_tmu);
    vkn_img *e = vkn_img_get(t->serial); uint32_t v = 0xdeadbeef;
    if (e) {
        VkImageAspectFlags asp = e->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
        t_vkResetCommandBuffer(g_rcb, 0); vk.vkBeginCommandBuffer(g_rcb, &bi);
        VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            e->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, e->img, {e->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b};
        vk.vkCmdPipelineBarrier2(g_rcb, &di);
        VkBufferImageCopy r = {0, 0, 0, {asp, 0, 0, 1}, {x, y, 0}, {1, 1, 1}};
        t_vkCmdCopyImageToBuffer(g_rcb, e->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_rbuf, 1, &r);
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b.srcAccessMask = 0; b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = e->layout;   // back as the backend tracks it
        VkMemoryBarrier2 hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
        VkDependencyInfo d2 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &hb, 0, NULL, 1, &b};
        vk.vkCmdPipelineBarrier2(g_rcb, &d2);
        vk.vkEndCommandBuffer(g_rcb);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &g_rcb, 0, NULL};
        if (!t_vkQueueSubmit(vd.q, 1, &si, VK_NULL_HANDLE) && !vk.vkQueueWaitIdle(vd.q)) memcpy(&v, g_rmap, 4);
    }
    pthread_mutex_unlock(&vkn_tmu); pthread_mutex_unlock(&vkn_qmu);
    return v;
}
// ---------- Screenshots: what vk_present.c png_write writes (stored deflate blocks only) read back as RGB rows, top row first. 1 = read ----------
static uint32_t be32r(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int png_read_stored(const char *path, uint32_t *w, uint32_t *h, uint8_t **rgb) {
    *w = *h = 0; *rgb = NULL;
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    long n = fseek(f, 0, SEEK_END) ? -1 : ftell(f); rewind(f);
    uint8_t *b = n > 0 ? malloc((size_t)n) : NULL, *z = NULL, *raw = NULL; size_t zn = 0;
    int ok = b && fread(b, 1, (size_t)n, f) == (size_t)n; fclose(f);
    ok = ok && n >= 8 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8);
    for (long o = 8; ok && o + 12 <= n; ) {   // chunks: IHDR (8-bit RGB), IDATs, IEND
        uint32_t len = be32r(b + o); if ((long)len > n - o - 12) { ok = 0; break; }
        const uint8_t *t = b + o + 4, *d = b + o + 8;
        if (!memcmp(t, "IHDR", 4)) { ok = len == 13 && d[8] == 8 && d[9] == 2; *w = be32r(d); *h = be32r(d + 4); }
        else if (!memcmp(t, "IDAT", 4)) { uint8_t *nz = realloc(z, zn + len + 1); if (!nz) { ok = 0; break; } z = nz; memcpy(z + zn, d, len); zn += len; }
        else if (!memcmp(t, "IEND", 4)) break;
        o += 12 + (long)len;
    }
    size_t row = 1 + 3 * (size_t)*w, rn = row * *h, got = 0;
    ok = ok && *w && *h && zn > 2 && (raw = malloc(rn));
    for (size_t o = 2; ok; ) {   // past the zlib header: stored blocks (BTYPE 00), LEN little-endian, NLEN
        if (o + 5 > zn || (z[o] & 6)) { ok = 0; break; }
        size_t k = z[o + 1] | (size_t)z[o + 2] << 8; int last = z[o] & 1; o += 5;
        if (o + k > zn || got + k > rn) { ok = 0; break; }
        memcpy(raw + got, z + o, k); got += k; o += k;
        if (last) break;
    }
    ok = ok && got == rn && (*rgb = malloc((size_t)*w * *h * 3));
    for (uint32_t y = 0; ok && y < *h; y++) { if (raw[y * row]) ok = 0; else memcpy(*rgb + (size_t)y * *w * 3, raw + y * row + 1, (size_t)*w * 3); }   // filter 0 on every row
    if (!ok) { free(*rgb); *rgb = NULL; }
    free(raw); free(z); free(b);
    return ok;
}
static int px_is(const uint8_t *rgb, uint32_t w, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b) { const uint8_t *p = rgb + ((size_t)y * w + x) * 3; return p[0] == r && p[1] == g && p[2] == b; }
// One DM_NVN_SHOT present of t into path (nvn_vk.c's list made to pick the next present; the present thread writes it before the ticket is done)
static int shoot(const tex_t *t, const char *path) {
    g_shots[0] = (long)atomic_load(&g_present_n) + 1; g_nshot = 1; g_shot_path = path;
    uint64_t tk = mtl_present(t, 1 / 60., NULL);
    if (tk) vkn_present_wait(tk);
    g_nshot = 0;
    return tk != 0;
}

static uint32_t rgba(float r, float g, float b, float a) { return (uint32_t)(r * 255.f + .5f) | (uint32_t)(g * 255.f + .5f) << 8 | (uint32_t)(b * 255.f + .5f) << 16 | (uint32_t)(a * 255.f + .5f) << 24; }
static float depth_at(const tex_t *t, int x, int y) { uint32_t v = texel(t, x, y); float f; memcpy(&f, &v, 4); return f; }

// ---------- Sampled textures ----------
// A texture whose storage is in a test pool at off (laid out by nvn_tex.c, as hle_nvn.c's TextureInitialize does): live, no host image until a draw looks it up
static tex_t *tex_in(uint8_t *pool, size_t off, unsigned w, unsigned h, unsigned levels, int fmt) {
    tex_t *t = calloc(1, sizeof *t); t->d.width = w; t->d.height = h; t->d.depth = 1; t->d.levels = levels; t->d.target = NVN_TEXTURE_TARGET_2D; t->d.format = fmt;
    nvn_tex_layout(&t->d, &t->L); t->mem = pool + off; t->serial = nvn_new_serial(); mtl_tex_init(t);
    return t;
}
// The same with texture flags, a row stride and the pool object (hle_nvn.c's TextureInitialize gives every texture with storage its pool)
static tex_t *tex_in_p(uint8_t *mem, pool_t *pool, unsigned w, unsigned h, int fmt, int flags, unsigned stride) {
    tex_t *t = calloc(1, sizeof *t); t->d.width = w; t->d.height = h; t->d.depth = 1; t->d.levels = 1; t->d.target = NVN_TEXTURE_TARGET_2D; t->d.format = fmt; t->d.flags = flags; t->d.stride = stride;
    nvn_tex_layout(&t->d, &t->L); t->mem = mem; t->pool = pool; t->serial = nvn_new_serial(); mtl_tex_init(t);
    return t;
}
static void rec_bind(int tid, int sid) { p_bind_tex1 b = {4, 0, (uint64_t)tid | (uint64_t)sid << 20, 0, 0}; rec(OP_BIND_TEX1, &b, sizeof b); }   // fragment stage, slot 0
static void draw_with(const tex_t *rt, int tid, int sid) { rec_rt(rt, NULL); rec_bind(tid, sid); rec_draw(); submit(); }
static uint64_t ts_n(int i) { return atomic_load(&g_ts[i]); }
static VkImageLayout layout_of(const tex_t *t) { pthread_mutex_lock(&vkn_tmu); vkn_img *e = vkn_img_get(t->serial); VkImageLayout l = e ? e->layout : (VkImageLayout)-1; pthread_mutex_unlock(&vkn_tmu); return l; }
static vkn_img *img_of(const tex_t *t) { pthread_mutex_lock(&vkn_tmu); vkn_img *e = vkn_img_get(t->serial); pthread_mutex_unlock(&vkn_tmu); return e; }
// Level lv of t's host image (w x h texels of 4 bytes) into g_xmap, the queue idle around it; the image goes back to the layout the backend tracks. 1 = read
static int img_read(const tex_t *t, uint32_t lv, uint32_t w, uint32_t h) {
    mtl_finish();
    pthread_mutex_lock(&vkn_qmu); pthread_mutex_lock(&vkn_tmu);
    vkn_img *e = vkn_img_get(t->serial); int ok = 0;
    if (e && e->layout != VK_IMAGE_LAYOUT_UNDEFINED && (size_t)w * h * 4 <= XCAP) {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
        t_vkResetCommandBuffer(g_rcb, 0); vk.vkBeginCommandBuffer(g_rcb, &bi);
        VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            e->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, e->img, {e->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b};
        vk.vkCmdPipelineBarrier2(g_rcb, &di);
        VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, lv, 0, 1}, {0, 0, 0}, {w, h, 1}};
        t_vkCmdCopyImageToBuffer(g_rcb, e->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_xbuf, 1, &r);
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b.srcAccessMask = 0; b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = e->layout;
        VkMemoryBarrier2 hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
        VkDependencyInfo d2 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &hb, 0, NULL, 1, &b};
        vk.vkCmdPipelineBarrier2(g_rcb, &d2);
        vk.vkEndCommandBuffer(g_rcb);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &g_rcb, 0, NULL};
        ok = !t_vkQueueSubmit(vd.q, 1, &si, VK_NULL_HANDLE) && !vk.vkQueueWaitIdle(vd.q);
    }
    pthread_mutex_unlock(&vkn_tmu); pthread_mutex_unlock(&vkn_qmu);
    return ok;
}
// The linear content of level lv (gen: which version), RGBA8 rows of w texels
static void tex_pattern(uint8_t *p, uint32_t w, uint32_t h, uint32_t lv, int gen) {
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) { uint8_t *q = p + ((size_t)y * w + x) * 4; q[0] = (uint8_t)(lv * 50 + x + gen * 7); q[1] = (uint8_t)(y * 3 + gen); q[2] = (uint8_t)(x ^ y); q[3] = (uint8_t)(255 - lv - gen); }
}
static uint64_t uploads(void) { return ts_n(TS_UP1) + ts_n(TS_RE) + ts_n(TS_REALW); }

// ---------- Draws (slice 2c) ----------
// hle_nvn.c's prog_key: here a program's stage s names its key's 8 bytes at sd[s].control
int prog_key(const prog_t *p, unsigned st, uint8_t key[8]) { if (st >= p->nstage || !p->sd[st].control) return 0; memcpy(key, p->sd[st].control, 8); return 1; }
// A test module (t_vkn_spv.h, vkn_kernels/) under key k, with the reflection dmsh_build_vk.py writes for it: bindings and the interface as given; its spv_sha256 that of the bytes given, or of
// those at sha_of (another module's reflection)
static uint8_t g_tkeys[64][8]; static int g_ntkeys;
static const uint8_t *t_key(uint64_t v) { uint8_t *k = g_tkeys[g_ntkeys++]; for (int i = 0; i < 8; i++) k[i] = (uint8_t)(v >> (56 - 8 * i)); return k; }
static vkn_shader *t_mod_sha(const uint8_t *k, int stage, const char *status, const uint32_t *spv, size_t n, const char *binds, const char *io, const uint32_t *sha_of) {
    char hk[17], hs[17], js[4096]; uint8_t d[32]; for (int i = 0; i < 8; i++) snprintf(hk + 2 * i, 3, "%02x", k[i]);
    dm_sha256(sha_of ? sha_of : spv, n, d); for (int i = 0; i < 8; i++) snprintf(hs + 2 * i, 3, "%02x", d[i]);
    snprintf(js, sizeof js, "{\"key\": \"%s_b\", \"dm_ctl\": \"%s\", \"chain\": \"%s\", \"status\": \"%s\", \"stage\": \"%s\", \"entry\": \"main\", \"spv_bytes\": %zu, \"spv_sha256\": \"%s\", "
             "\"bindings\": [%s], %s}", hk, hk, VKN_CHAIN, status, stage ? "ps" : "vs", n, hs, binds, io);
    return vkn_shader_add(k, stage, js, spv, n);
}
static vkn_shader *t_mod(const uint8_t *k, int stage, const char *status, const uint32_t *spv, size_t n, const char *binds, const char *io) { return t_mod_sha(k, stage, status, spv, n, binds, io, NULL); }
static prog_t *t_prog(const uint8_t *kv, const uint8_t *kp) { prog_t *p = calloc(1, sizeof *p); p->nstage = 2; p->sd[0].control = kv; p->sd[1].control = kp; p->serial = nvn_new_serial(); return p; }
typedef struct { float p[4], c[4]; } t_vtx;   // the test vertex shaders' attributes: RGBA32F position (location 0) and colour (1), 32 B a vertex
static void rec_prog(const prog_t *p) { p_bind_program b = {p, 0x1f}; rec(OP_BIND_PROGRAM, &b, sizeof b); }
static void rec_state(int kind, const void *s, size_t n) { p_bind_state b; memset(&b, 0, sizeof b); b.kind = kind; memcpy(b.bytes, s, n); rec(OP_BIND_STATE, &b, sizeof b); }
static void rec_poly(unsigned cull, unsigned front) { poly_st p = {cull, front, NVN_POLYGON_MODE_FILL, 0}; rec_state(4, &p, sizeof p); }
static void rec_vpr(float x, float y, float w, float h) { p_viewports v; memset(&v, 0, sizeof v); v.n = 1; v.v[0][0] = x; v.v[0][1] = y; v.v[0][2] = w; v.v[0][3] = h; v.v[0][5] = 1; rec(OP_VIEWPORTS, &v, sizeof v); }
static void rec_vp(float w, float h) { rec_vpr(0, 0, w, h); }
static void rec_scr(int x, int y, int w, int h) { p_scissor s = {x, y, w, h}; rec(OP_SCISSOR, &s, sizeof s); }
static void rec_sc(int w, int h) { rec_scr(0, 0, w, h); }
static void rec_swz(int negy) { p_vp_swizzle s; memset(&s, 0, sizeof s); s.n = 1; s.sw[0][1] = negy ? 1 : 0; rec(OP_VP_SWIZZLE, &s, sizeof s); }   // NEGATIVE_Y (3) XOR the identity's y (2)
static void rec_vbuf(const void *vb, uint64_t n, int nattr) {   // stream 0, stride 32: position at 0, colour at 16 (nattr 1: the colour fed by nothing)
    p_bind_vattr a; memset(&a, 0, sizeof a); a.n = nattr; va_st p = {NVN_FORMAT_RGBA32F, 0, 0}, c = {NVN_FORMAT_RGBA32F, 16, 0}; memcpy(&a.st[0], &p, 4); memcpy(&a.st[1], &c, 4); rec(OP_BIND_VATTR, &a, sizeof a);
    p_bind_vstream s; memset(&s, 0, sizeof s); s.n = 1; s.st[0].stride = 32; rec(OP_BIND_VSTREAM, &s, sizeof s);
    p_bind_vbuf b; memset(&b, 0, sizeof b); b.n = 1; b.r[0].address = (uint64_t)(uintptr_t)vb; b.r[0].size = n; rec(OP_BIND_VBUF, &b, sizeof b);
}
static void rec_idx(const uint16_t *ib, int count, int base) { p_draw_idx d; memset(&d, 0, sizeof d); d.prim = NVN_DRAW_PRIMITIVE_TRIANGLES; d.itype = NVN_INDEX_TYPE_UNSIGNED_SHORT; d.count = count; d.base_vertex = base; d.ibuf = (uint64_t)(uintptr_t)ib; rec(OP_DRAW_IDX, &d, sizeof d); }
static void rec_tex(int st, int slot, int tid, int sid) { p_bind_tex1 b = {st, slot, (uint64_t)tid | (uint64_t)sid << 20, 0, 0}; rec(OP_BIND_TEX1, &b, sizeof b); }
static void rec_ubo_at(int st, int idx, const void *p, uint64_t n) { p_bind_buf u = {st, idx, (uint64_t)(uintptr_t)p, n}; rec(OP_BIND_UBO, &u, sizeof u); }
// A 64x64 RGBA8 target read into g_xmap (img_read): the pixels equal to v by quadrant (bit 0: columns 32-63, bit 1: rows 32-63), and the rows and columns they span
typedef struct { int q[4], n, r0, r1, c0, c1; } t_cov;
static t_cov cover(uint32_t v) {
    t_cov c = {{0}, 0, 64, -1, 64, -1};
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { uint32_t p; memcpy(&p, g_xmap + ((size_t)y * 64 + x) * 4, 4); if (p != v) continue;
        c.q[(x >= 32) | (y >= 32) << 1]++; c.n++; if (y < c.r0) c.r0 = y; if (y > c.r1) c.r1 = y; if (x < c.c0) c.c0 = x; if (x > c.c1) c.c1 = x; }
    return c;
}
static uint32_t px64(int x, int y) { uint32_t p; memcpy(&p, g_xmap + ((size_t)y * 64 + x) * 4, 4); return p; }
static int row_n(uint32_t v, int y) { int n = 0; for (int x = 0; x < 64; x++) n += px64(x, y) == v; return n; }   // the pixels of row y equal to v
static uint64_t dk(int i) { return atomic_load(&g_dr_skip[i]); }

int main(void) {
    const char *st = getenv("T_VKN_STRICT"); int strict = st && atoi(st) > 0;
    crash_install();   // the write-fault handler nvn_dirty.c hooks into (B protects pool pages)
    mtl_init();
    if (!g_vk && vd.absent && !strict) { fprintf(stderr, "t_vkn: SKIPPED: no Vulkan loader or driver on this machine, nothing was checked (make check_t_vkn runs it on KosmicKrisp, strict)\n"); return 0; }
    if (!g_vk) { fprintf(stderr, "t_vkn: FAIL: the Vulkan backend did not start (%s; the lines above say why)\n", vd.absent ? "no Vulkan loader or driver, and T_VKN_STRICT=1 does not skip" : "Vulkan is there"); return 1; }
    if (!rb_init() || !xb_init()) { fprintf(stderr, "t_vkn: FAIL: readback objects could not be made\n"); return 1; }
    CHECK(vd.validation || !strict, "validation is off (T_VKN_STRICT=1 needs DM_VK_VALIDATION=1 and the layer: VK_LAYER_PATH)");
    if (!vd.validation && !strict) fprintf(stderr, "t_vkn: validation is off: the validation-message check is not made (make check_t_vkn turns it on)\n");
    tex_t *c64 = tex(64, 64, NVN_FORMAT_RGBA8), *c128 = tex(128, 128, NVN_FORMAT_RGBA8), *d64 = tex(64, 64, NVN_FORMAT_DEPTH32F), *d128 = tex(128, 128, NVN_FORMAT_DEPTH32F);

    // A submission that ends with its clear still recorded: it lands in its own pass at the commit
    rec_rt(c64, NULL); rec_clear(0.25f, 0.5f, 0.75f, 1.f); submit();
    CHECK(texel(c64, 5, 5) == rgba(0.25f, 0.5f, 0.75f, 1.f), "a clear recorded at the submission's end: %#x", texel(c64, 5, 5));
    // One that ends with its pass open (a draw opened it, its clear landed as the load op)
    rec_rt(c64, NULL); rec_clear(1.f, 0.f, 0.f, 1.f); rec_draw(); submit();
    CHECK(texel(c64, 63, 63) == rgba(1.f, 0.f, 0.f, 1.f), "a pass open at the submission's end: %#x", texel(c64, 63, 63));
    // Ends with a clear recorded after a draw's pass was ended by SET_RT on another target
    rec_rt(c64, NULL); rec_draw(); rec_rt(c128, NULL); rec_clear(0.f, 0.f, 1.f, 1.f); submit();
    CHECK(texel(c128, 100, 100) == rgba(0.f, 0.f, 1.f, 1.f), "a clear recorded after a pass on another target: %#x", texel(c128, 100, 100));

    // Depth 128x128 under a 64x64 colour target: cleared to 1 before the pass (an oversize load-op clear: a pass of its own), then to 0.25 inside the open pass
    rec_rt(c64, d128); rec_clear_d(1.f); rec_draw(); submit();
    CHECK(depth_at(d128, 100, 100) == 1.f, "depth cleared before the pass, outside the pass's 64x64: %g", depth_at(d128, 100, 100));
    uint64_t own0 = g_stat_own;
    rec_rt(c64, d128); rec_draw(); rec_clear_d(0.25f); rec_draw(); submit();
    CHECK(depth_at(d128, 100, 100) == 0.25f, "a full depth clear inside a 64x64 pass reaches the whole 128x128 target: %g at 100,100", depth_at(d128, 100, 100));
    CHECK(depth_at(d128, 10, 10) == 0.25f, "and inside the pass's area: %g", depth_at(d128, 10, 10));
    CHECK(g_stat_own > own0, "it was cleared in a pass of its own (%llu)", (unsigned long long)(g_stat_own - own0));
    // A colour target 128x128 with 64x64 depth: the same for a colour clear in the pass, here at the submission's end
    rec_rt(c128, d64); rec_clear(0.f, 0.f, 0.f, 1.f); rec_clear_d(1.f); rec_draw(); rec_clear(0.f, 1.f, 0.f, 1.f); submit();
    CHECK(texel(c128, 120, 120) == rgba(0.f, 1.f, 0.f, 1.f), "a full colour clear inside a 64x64 pass reaches the whole 128x128 target: %#x", texel(c128, 120, 120));
    // A clear in a pass as large as its target stays in the pass (vkCmdClearAttachments)
    uint64_t mid0 = g_stat_clr_mid;
    rec_rt(c64, d64); rec_draw(); rec_clear(1.f, 1.f, 1.f, 1.f); rec_draw(); submit();
    CHECK(texel(c64, 32, 32) == rgba(1.f, 1.f, 1.f, 1.f) && g_stat_clr_mid == mid0 + 1, "a clear in a pass of its target's size: %#x, in the pass %llu", texel(c64, 32, 32), (unsigned long long)(g_stat_clr_mid - mid0));

    // A screenshot's orientation (vk_present.c shot_one): red above, blue in memory rows 0..23 (a scissored clear in the pass: NVN's y is the image's row). With NVN's lower-left origin the window
    // image is stored bottom-up, so the file (top row first) has blue at the bottom, as Metal's shot() writes it; with the upper-left origin, at the top. No window here: the present is dropped, the shot is not
    {
        tex_t *win = tex(64, 48, NVN_FORMAT_RGBA8); p_scissor half = {0, 0, 64, 24}, all = {0, 0, 64, 48};
        rec_rt(win, NULL); rec_clear(1.f, 0.f, 0.f, 1.f); rec_draw(); rec(OP_SCISSOR, &half, sizeof half); rec_clear(0.f, 0.f, 1.f, 1.f); rec(OP_SCISSOR, &all, sizeof all); submit();
        CHECK(texel(win, 7, 0) == rgba(0.f, 0.f, 1.f, 1.f) && texel(win, 7, 23) == rgba(0.f, 0.f, 1.f, 1.f) && texel(win, 7, 24) == rgba(1.f, 0.f, 0.f, 1.f) && texel(win, 7, 47) == rgba(1.f, 0.f, 0.f, 1.f),
              "the window texture: blue in memory rows 0..23, red in 24..47: %#x %#x %#x %#x", texel(win, 7, 0), texel(win, 7, 23), texel(win, 7, 24), texel(win, 7, 47));
        const char *td = getenv("TMPDIR"); char dir[1024], path[1100]; snprintf(dir, sizeof dir, "%s/t_vkn_shot.XXXXXX", td && *td ? td : "/tmp");
        int have = mkdtemp(dir) != NULL; snprintf(path, sizeof path, "%s/f%%d.png", dir);
        CHECK(have, "a temporary directory for the screenshots: %s", strerror(errno));
        for (int ll = 1; have && ll >= 0; ll--) {
            mtl_set_origin_lower_left(ll);
            uint64_t n = atomic_load(&g_present_n) + 1; char file[1200]; snprintf(file, sizeof file, "%s/f%llu.png", dir, (unsigned long long)n);
            uint32_t w, h; uint8_t *rgb = NULL;
            int took = shoot(win, path), read = took && png_read_stored(file, &w, &h, &rgb);
            CHECK(read && w == 64 && h == 48, "%s origin: the screenshot %s was written and reads back (%dx%d)", ll ? "lower-left" : "upper-left", file, read ? (int)w : -1, read ? (int)h : -1);
            if (read && w == 64 && h == 48) {
                int top_red = 1, bot_blue = 1, top_blue = 1, bot_red = 1;
                for (uint32_t y = 0; y < 48; y++) for (uint32_t x = 0; x < 64; x++) {
                    int red = px_is(rgb, w, x, y, 255, 0, 0), blue = px_is(rgb, w, x, y, 0, 0, 255);
                    if (y < 24) { top_red &= red; top_blue &= blue; } else { bot_blue &= blue; bot_red &= red; }
                }
                if (ll) CHECK(top_red && bot_blue, "lower-left origin: the file's top half is red and its bottom half blue (top red %d, bottom blue %d)", top_red, bot_blue);
                else CHECK(top_blue && bot_red, "upper-left origin: the file's top half is blue and its bottom half red (top blue %d, bottom red %d)", top_blue, bot_red);
            }
            free(rgb); unlink(file);
        }
        mtl_set_origin_lower_left(1);
        if (have) rmdir(dir);
    }

    // ---------- B: write, submit, read the shadow back on the GPU ----------
    {
        size_t pg = (size_t)getpagesize(), n = 4 * pg; uint8_t *pm = pool_new(n);
        CHECK(pm != NULL, "a test pool of %zu B", n);
        if (pm) {
            for (size_t i = 0; i < n; i++) pm[i] = (uint8_t)(i * 7 + 1);
            VkBuffer sb; uint64_t off, gpu;
            CHECK(!shadow_of(pm, 1, &sb, &off, &gpu), "no shadow before a record reads the pool as a buffer");
            uint64_t w0 = b_pages(0), n0 = b_pages(1), a0 = b_pages(2);
            rec_ubo(pm + pg + pg / 2, pg); submit();   // reads pages 1 and 2
            int has = shadow_of(pm + pg, pg, &sb, &off, &gpu);
            CHECK(has && off == pg && (gpu != 0) == (vd.bda != 0), "a uniform buffer read gives the pool a shadow: found %d, offset %#llx, device address %#llx (bda %d)", has, (unsigned long long)off, (unsigned long long)gpu, vd.bda);
            CHECK(b_pages(1) - n0 == 2 && b_pages(0) == w0 && b_pages(2) == a0, "the first submission uploads the 2 pages read, as first reads: %llu first, %llu written since, %llu always",
                  (unsigned long long)(b_pages(1) - n0), (unsigned long long)(b_pages(0) - w0), (unsigned long long)(b_pages(2) - a0));
            CHECK(has && buf_read(sb, pg, 2 * pg) && !memcmp(g_xmap, pm + pg, 2 * pg), "the shadow holds pages 1-2 as written (GPU readback)");
            // They are protected now: a write faults once, and only that page goes up at the next submission
            uint64_t f0 = faults(); pm[pg + 5] = 0xab;
            CHECK(faults() == f0 + 1, "a write into an uploaded page faults (the tracker sees it): %llu faults", (unsigned long long)(faults() - f0));
            w0 = b_pages(0); n0 = b_pages(1);
            rec_ubo(pm + pg + pg / 2, pg); submit();
            CHECK(b_pages(0) - w0 == 1 && b_pages(1) == n0, "a rewrite of one page uploads that page only: %llu written since, %llu first", (unsigned long long)(b_pages(0) - w0), (unsigned long long)(b_pages(1) - n0));
            CHECK(buf_read(sb, pg, 2 * pg) && !memcmp(g_xmap, pm + pg, 2 * pg), "the shadow has the rewrite (GPU readback)");
            w0 = b_pages(0);
            rec_ubo(pm + pg + pg / 2, pg); submit();
            CHECK(b_pages(0) == w0, "nothing written: nothing uploaded (%llu)", (unsigned long long)(b_pages(0) - w0));
            pm[2 * pg + 9] = 0xcd; pm[2 * pg + 10] = 0xce;   // page 2, twice: one fault
            rec_ubo(pm + pg + pg / 2, pg); submit();
            CHECK(b_pages(0) - w0 == 1 && buf_read(sb, pg, 2 * pg) && !memcmp(g_xmap, pm + pg, 2 * pg), "the other page rewritten: it alone goes up (%llu), and the shadow has it", (unsigned long long)(b_pages(0) - w0));
            pool_fin(pm);
            CHECK(writable(pm, n), "a freed pool's pages are writable");
            munmap(pm, n);
        }
    }
    // ---------- B: an unprotect under way at the commit's stamp (nvn_vk.c b_commit: the drain and the re-scan) ----------
    // A writer thread's fault on a protected page is held in nvn_dirty.c's unprotect, BUSY and not yet counted (the interposed mprotect); the commit runs meanwhile with its stamp one ahead of the
    // tracker (t_dirty_now), so the stamp covers the generation the unprotect is about to take, as when the increment lands before the stamp read and the page's generation store after the scan.
    // The scan does not pick the page. Without the drain (waiting it out of BUSY, then protecting it) and the re-scan (its generation newer than the last stamp), it would end writable with a
    // generation no newer than this stamp: no later commit would copy it, and the write would never reach the shadow
    {
        size_t pg = (size_t)getpagesize(), n = 2 * pg; uint8_t *pm = pool_new(n);
        CHECK(g_real_mprotect && atomic_load(&g_mp_n) > 0, "the tracker's mprotect calls reach the test's (resolved %d, %llu calls so far)", g_real_mprotect != NULL, (unsigned long long)atomic_load(&g_mp_n));
        CHECK(pm != NULL, "a test pool of %zu B", n);
        if (pm) {
            memset(pm, 0x11, n);
            rec_ubo(pm, n); submit();   // both pages read: uploaded, then protected
            VkBuffer sb; uint64_t off, gpu; int has = shadow_of(pm, n, &sb, &off, &gpu);
            CHECK(has && buf_read(sb, off, n) && !memcmp(g_xmap, pm, n) && !writable(pm + pg, pg), "set up: the pool's shadow holds it and page 1 is protected");
            uint64_t f0 = faults(), l0 = atomic_load(&g_bs_late), w0 = b_pages(0);
            atomic_store(&g_mp_held, 0); atomic_store(&g_mp_hold, (uintptr_t)(pm + pg));
            pthread_t th; int started = !pthread_create(&th, NULL, t_writer, pm + pg + 8);
            int held = 0; for (int i = 0; started && i < 2000 && !(held = atomic_load(&g_mp_held)); i++) { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }
            CHECK(started && held, "the writer's fault is held in the unprotect (thread started %d, held %d)", started, held);
            if (held) {
                atomic_store(&g_now_ahead, 1); rec_ubo(pm, n); submit(); atomic_store(&g_now_ahead, 0);   // the stamp is read in b_commit: one ahead across the whole commit
                CHECK(atomic_load(&g_bs_late) - l0 == 1 && b_pages(0) - w0 == 1, "the commit's scan missed the page and its re-scan after the drain picked it: %llu late, %llu written since",
                      (unsigned long long)(atomic_load(&g_bs_late) - l0), (unsigned long long)(b_pages(0) - w0));
            }
            atomic_store(&g_mp_hold, 0);
            if (started) pthread_join(th, NULL);
            CHECK(pm[pg + 8] == 0x5a && faults() - f0 >= 1, "the write landed (%#x) through the tracker (%llu faults)", pm[pg + 8], (unsigned long long)(faults() - f0));
            rec_ubo(pm, n); submit();
            CHECK(buf_read(sb, off, n) && !memcmp(g_xmap, pm, n), "the next submission's shadow holds the write (GPU readback: %#x at the written byte)", g_xmap[pg + 8]);
            uint64_t f1 = faults(); pm[pg + 16] = 0x6b;
            rec_ubo(pm, n); submit();
            CHECK(faults() == f1 + 1 && buf_read(sb, off, n) && !memcmp(g_xmap, pm, n), "the page is protected again: a later write faults (%llu) and reaches the shadow", (unsigned long long)(faults() - f1));
            pool_fin(pm); munmap(pm, n);
        }
    }
    // ---------- B: two pools sharing a host page ----------
    // Pool A is [m, m + pg + 4 KB), pool B [m + pg + 4 KB, m + 3 pg): page 1 is in both rounded ranges, an edge page of each (nvn_vk.c mtl_pool_add). Neither protects it (two ranges protecting
    // one page leave one stale), both shadows get it at every submission, and an address in it resolves to the pool whose bytes hold it (the lookup cache included: A is looked up first)
    {
        size_t pg = (size_t)getpagesize();
        if (pg <= 4096) fprintf(stderr, "t_vkn: 4 KB host pages: 4 KB-aligned pools share no page, the shared-page checks are skipped\n");
        else {
            uint8_t *m = mmap(NULL, 3 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0), *pa = m, *pb = m + pg + 4096; size_t na = pg + 4096, nb = 2 * pg - 4096;
            CHECK(m != MAP_FAILED, "the shared-page test's memory");
            if (m != MAP_FAILED) {
                mtl_pool_add(pa, na); nvn_dirty_add_range(pa, na); mtl_pool_add(pb, nb); nvn_dirty_add_range(pb, nb);
                for (size_t i = 0; i < 3 * pg; i++) m[i] = (uint8_t)(i * 13 + 5);
                uint64_t al0 = b_pages(2);
                rec_ubo(pa, na); rec_ubo(pb, nb); submit();
                VkBuffer sa, sb; uint64_t oa = 0, ob = 0, ga, gb;
                int ha = shadow_of(pa + pg + 16, 16, &sa, &oa, &ga), hb = shadow_of(pb + 16, 16, &sb, &ob, &gb);
                CHECK(ha && hb && sa != sb && oa == pg + 16 && ob == 4096 + 16, "an address in the shared page resolves to the pool whose bytes hold it: A's at %#llx, B's at %#llx in another shadow (%d)",
                      (unsigned long long)oa, (unsigned long long)ob, ha && hb && sa != sb);
                CHECK(b_pages(2) - al0 == 2, "the shared page goes up for both pools as copied every time: %llu", (unsigned long long)(b_pages(2) - al0));
                CHECK(!writable(m, pg) && !writable(m + 2 * pg, pg), "the pools' own pages are protected");
                CHECK(writable(m + pg, pg), "the shared page is protected by neither pool");   // (the check wrote zeros there)
                uint64_t f0 = faults(); memset(m + pg, 0x77, pg); m[pg + 4095] = 0x11; m[pg + 4096] = 0x22;   // A's last byte, B's first
                rec_ubo(pa, na); rec_ubo(pb, nb); submit();
                int a_ok = ha && buf_read(sa, pg, pg) && !memcmp(g_xmap, m + pg, pg), b_ok = hb && buf_read(sb, 0, pg) && !memcmp(g_xmap, m + pg, pg);
                CHECK(faults() == f0 && a_ok && b_ok, "a write into it does not fault (%llu faults), and both shadows have it at the next submission (A %d, B %d)", (unsigned long long)(faults() - f0), a_ok, b_ok);
                nvn_dirty_remove_range(pa); mtl_pool_del(pa); nvn_dirty_remove_range(pb); mtl_pool_del(pb);
                munmap(m, 3 * pg);
            }
        }
    }
    // ---------- B: a pool deleted while a submission that read it is in flight ----------
    // A batch whose one command waits on a timeline semaphore the test signals goes first; the backend's upload barrier waits for every earlier command, so its submission stays in flight
    {
        size_t pg = (size_t)getpagesize(), n = 2 * pg; uint8_t *pd = pool_new(n);
        VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
        VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti}; VkSemaphore gate = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_rpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1}; VkCommandBuffer bcb = VK_NULL_HANDLE;
        int ready = pd && !vk.vkCreateSemaphore(vd.dev, &sci, NULL, &gate) && !vk.vkAllocateCommandBuffers(vd.dev, &ai, &bcb);
        CHECK(ready, "the in-flight test's pool, semaphore and command buffer");
        if (ready) {
            memset(pd, 0x5a, n);
            VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
            vk.vkBeginCommandBuffer(bcb, &bi); VkBufferCopy r = {0, 0, 4}; vk.vkCmdCopyBuffer(bcb, g_xbuf, g_rbuf, 1, &r); vk.vkEndCommandBuffer(bcb);
            VkSemaphoreSubmitInfo w = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, gate, 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
            VkCommandBufferSubmitInfo c = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, bcb, 0};
            VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 1, &w, 1, &c, 0, NULL};
            pthread_mutex_lock(&vkn_qmu); VkResult qr = vk.vkQueueSubmit2(vd.q, 1, &si, VK_NULL_HANDLE); pthread_mutex_unlock(&vkn_qmu);
            CHECK(!qr, "the blocking batch was queued (%d)", qr);
            uint64_t freed0 = atomic_load(&g_shadow_freed);
            rec_ubo(pd, 64); submit();
            uint64_t v = atomic_load(&g_fence); usleep(100000);
            int flight = atomic_load(&g_done) < v;
            CHECK(flight, "the submission that read the pool is still in flight before the delete (done %llu, its fence %llu)", (unsigned long long)atomic_load(&g_done), (unsigned long long)v);
            pool_fin(pd);
            pthread_mutex_lock(&vkn_tmu); int queued = g_dead_b != NULL; pthread_mutex_unlock(&vkn_tmu);
            CHECK(!flight || (queued && atomic_load(&g_shadow_freed) == freed0), "deleted in flight: its shadow waits for the submission (queued %d, destroyed %llu)", queued, (unsigned long long)(atomic_load(&g_shadow_freed) - freed0));
            CHECK(writable(pd, n), "the deleted pool's pages are writable at once");
            VkSemaphoreSignalInfo sg = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, gate, 1};
            CHECK(!t_vkSignalSemaphore(vd.dev, &sg), "the gate is opened");
            mtl_finish();
            pthread_mutex_lock(&vkn_tmu); vkn_sweep(); queued = g_dead_b != NULL; pthread_mutex_unlock(&vkn_tmu);
            CHECK(!queued && atomic_load(&g_shadow_freed) == freed0 + 1, "once it has completed the shadow is destroyed (left %d, destroyed %llu)", queued, (unsigned long long)(atomic_load(&g_shadow_freed) - freed0));
            pthread_mutex_lock(&vkn_qmu); vk.vkQueueWaitIdle(vd.q); pthread_mutex_unlock(&vkn_qmu);
        }
        if (gate) vk.vkDestroySemaphore(vd.dev, gate, NULL);
        if (pd) munmap(pd, n);
    }
    // ---------- B: a pool read before its write tracking starts (the span gap; a slice 2a/2b fix landing with 2c) ----------
    // hle_nvn.c's MemoryPoolInitialize calls mtl_pool_add, then nvn_dirty_add_range: a commit in between finds no range and copies the pool as untracked. A write after the range appears
    // and before any protect does not fault (the fresh range's pages are writable, their generations 0); the next commit, finding the range, must take every buffer page as new (copied,
    // then protected), or the write never reaches the shadow (b_commit; the adversarial review's t_span_gap.c repro, ported)
    {
        size_t pg = (size_t)getpagesize(), n = 2 * pg; uint8_t *a = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(a != MAP_FAILED, "the span-gap test's pool memory");
        if (a != MAP_FAILED) {
            mtl_pool_add(a, n); memset(a, 0x11, n);
            uint64_t a0 = b_pages(2);
            rec_ubo(a, n); submit();
            VkBuffer sb; uint64_t off, gpu; int has = shadow_of(a, n, &sb, &off, &gpu);
            CHECK(has && b_pages(2) - a0 == 2 && buf_read(sb, off, n) && !memcmp(g_xmap, a, n), "a pool read before its tracking starts: both pages copied as untracked (%llu), the shadow holds them",
                  (unsigned long long)(b_pages(2) - a0));
            nvn_dirty_add_range(a, n);
            uint64_t f0 = faults(); a[pg + 8] = 0x5a;
            CHECK(faults() == f0, "a write once the tracking starts, before any protect, does not fault (%llu)", (unsigned long long)(faults() - f0));
            uint64_t n0 = b_pages(1);
            rec_ubo(a, n); submit();
            CHECK(has && b_pages(1) - n0 == 2 && buf_read(sb, off, n) && !memcmp(g_xmap, a, n), "the next commit takes both pages as new (%llu) and its shadow holds the write (%#x at the written byte)",
                  (unsigned long long)(b_pages(1) - n0), g_xmap[pg + 8]);
            uint64_t f1 = faults(); a[pg + 16] = 0x6b;
            rec_ubo(a, n); submit();
            CHECK(faults() == f1 + 1 && buf_read(sb, off, n) && !memcmp(g_xmap, a, n), "they are protected from then on: a later write faults (%llu) and reaches the shadow", (unsigned long long)(faults() - f1));
            nvn_dirty_remove_range(a); mtl_pool_del(a); munmap(a, n);
        }
    }
    // ---------- B: the span gap reached first through a texture (slice 2c, review round 1) ----------
    // The range can appear between a commit's buffer pass and its texture uploads (t_dirty_span adds it at the buffer pass's lookup, which then finds none). A CPU-written texture in the pool is
    // uploaded in that commit, so its upload is the first to find the range: the pool's buffer pages, copied unprotected until then, must become NEW there too (pool_span), or the next
    // commit's drain protects them unpicked and a write made before it (no fault: the fresh range's pages are writable) never reaches the shadow (the review's repro: guest 0x5a, shadow 0x11)
    {
        size_t pg = (size_t)getpagesize(), n = 4 * pg, bn = 2 * pg; uint8_t *a = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(a != MAP_FAILED, "the texture span-gap test's pool memory");
        if (a != MAP_FAILED) {
            mtl_pool_add(a, n); memset(a, 0x11, bn);   // pages 0-1 read as a uniform buffer, the texture at page 2
            tex_t *gt = tex_in(a, 2 * pg, 16, 16, 1, NVN_FORMAT_RGBA8), *grt = tex(64, 64, NVN_FORMAT_RGBA8);
            memset(gt->mem, 0x22, (size_t)gt->L.size); mtl_tex_cpu_written(gt); mtl_reg_tex(60, gt, NULL);
            rec_ubo(a, bn); submit();   // read before the range exists: copied untracked
            g_span_late_n = n; atomic_store(&g_span_late, (uintptr_t)a);
            uint64_t u1 = ts_n(TS_UP1);
            rec_ubo(a, bn); draw_with(grt, 60, 0);   // one submission: the buffer pass finds no range, then it appears, then the texture's upload finds it
            pthread_mutex_lock(&g_pmu); int found = 0; for (int i = 0; i < g_npool; i++) if (g_pools[i].key == a) found = g_pools[i].span != NULL; pthread_mutex_unlock(&g_pmu);
            CHECK(!atomic_load(&g_span_late) && found && ts_n(TS_UP1) == u1 + 1 && texel(gt, 1, 1) == 0x22222222, "the range appeared during the commit and the texture's upload found it first (the pool has it %d, %llu upload)",
                  found, (unsigned long long)(ts_n(TS_UP1) - u1));
            uint64_t f0 = faults(); a[pg + 8] = 0x5a;
            uint64_t n0 = b_pages(1);
            rec_ubo(a, bn); submit();
            VkBuffer sb; uint64_t off, gpu; int has = shadow_of(a, bn, &sb, &off, &gpu), rd = has && buf_read(sb, off, bn);
            CHECK(faults() == f0 && rd && !memcmp(g_xmap, a, bn) && b_pages(1) - n0 == 2, "a write then (no fault: %llu) reaches the shadow at the next commit (%#x at the written byte, guest %#x): its buffer pages were taken as new (%llu)",
                  (unsigned long long)(faults() - f0), rd ? g_xmap[pg + 8] : 0, a[pg + 8], (unsigned long long)(b_pages(1) - n0));
            uint64_t f1 = faults(); a[pg + 16] = 0x6b;
            rec_ubo(a, bn); submit();
            CHECK(faults() == f1 + 1 && has && buf_read(sb, off, bn) && !memcmp(g_xmap, a, bn), "they are protected from then on: a later write faults (%llu) and reaches the shadow", (unsigned long long)(faults() - f1));
            draw_with(grt, 0, 0); mtl_reg_tex(60, NULL, NULL); mtl_tex_release(gt);   // nothing names the texture or its pool any more
            nvn_dirty_remove_range(a); mtl_pool_del(a); munmap(a, n);
        }
    }

    // ---------- Sampled textures (slice 2b) ----------
    {
        size_t pg = (size_t)getpagesize(), n = 16 * pg; uint8_t *tp = pool_new(n);
        tex_t *rtc = tex(64, 64, NVN_FORMAT_RGBA8);   // the target the draws render to (it reads nothing)
        samp_par lin = {NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR, NVN_MAG_FILTER_LINEAR, NVN_WRAP_MODE_REPEAT, NVN_WRAP_MODE_REPEAT, NVN_WRAP_MODE_REPEAT, 0, 0, 0, 0.f, 1000.f, 0.f, 1.f, {0, 0, 0, 0}};
        mtl_reg_samp(3, &lin);
        CHECK(tp != NULL, "the texture test pool");
        if (tp) {
            // A 40x24 RGBA8 texture with 4 levels, written by the CPU as the guest lays it out (block-linear), at a page boundary of the pool (its end shares a page with the pool's other bytes)
            tex_t *st = tex_in(tp, 2 * pg, 40, 24, 4, NVN_FORMAT_RGBA8);
            uint8_t *lin0 = malloc(40 * 24 * 4); int sw_ok = 1;
            for (uint32_t l = 0; l < 4; l++) { uint32_t w = 40 >> l, h = 24 >> l; tex_pattern(lin0, w, h, l, 0); sw_ok &= nvn_swizzle(&st->d, l, 0, lin0, w * 4, st->mem) == 0; }
            CHECK(sw_ok && st->L.size < 14 * pg, "the texture is swizzled into the pool (%llu B)", (unsigned long long)st->L.size);
            mtl_tex_cpu_written(st);
            mtl_reg_tex(5, st, NULL);
            uint64_t u1 = ts_n(TS_UP1), made0 = ts_n(TS_MADE);
            draw_with(rtc, 5, 3);
            CHECK(ts_n(TS_UP1) == u1 + 1 && ts_n(TS_MADE) == made0 + 1, "its first draw makes its host image and uploads it once (%llu uploads, %llu images)", (unsigned long long)(ts_n(TS_UP1) - u1), (unsigned long long)(ts_n(TS_MADE) - made0));
            vkn_img *e = img_of(st);
            CHECK(e && e->levels == 4 && e->fmt == VK_FORMAT_R8G8B8A8_UNORM && layout_of(st) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, "its image: 4 levels, R8G8B8A8_UNORM, shader-read after the submission (levels %u, layout %d)",
                  e ? e->levels : 0, (int)layout_of(st));
            for (uint32_t l = 0; l < 4; l++) { uint32_t w = 40 >> l, h = 24 >> l; tex_pattern(lin0, w, h, l, 0);
                CHECK(img_read(st, l, w, h) && !memcmp(g_xmap, lin0, (size_t)w * h * 4), "level %u (%ux%u) reads back as written (GPU readback of the deswizzled upload)", l, w, h); }
            CHECK(!writable(tp + 2 * pg, pg), "its pages are protected after the upload");
            // No guest write: nothing uploaded. A write beside it in the page it shares: compared, not uploaded. A write into level 0: uploaded once, then nothing again
            uint64_t up0 = uploads(), es0 = ts_n(TS_EDGE), f0 = faults();
            draw_with(rtc, 5, 3);
            CHECK(uploads() == up0, "drawn again with no guest write: no upload (%llu)", (unsigned long long)(uploads() - up0));
            size_t end = 2 * pg + (size_t)st->L.size;
            if (end % pg) {
                tp[end + 8] = 0x42;
                draw_with(rtc, 5, 3);
                CHECK(faults() > f0 && uploads() == up0 && ts_n(TS_EDGE) == es0 + 1, "a write beside it in its last page faults (%llu) and is only compared: %llu uploads, %llu beside", (unsigned long long)(faults() - f0),
                      (unsigned long long)(uploads() - up0), (unsigned long long)(ts_n(TS_EDGE) - es0));
            }
            uint64_t re0 = ts_n(TS_RE); f0 = faults();
            tex_pattern(lin0, 40, 24, 0, 1); nvn_swizzle(&st->d, 0, 0, lin0, 40 * 4, st->mem);
            CHECK(faults() > f0, "the rewrite of level 0 faults (%llu)", (unsigned long long)(faults() - f0));
            draw_with(rtc, 5, 3);
            CHECK(ts_n(TS_RE) == re0 + 1 && uploads() == up0 + 1, "a guest write into it: one re-upload (%llu, %llu uploads)", (unsigned long long)(ts_n(TS_RE) - re0), (unsigned long long)(uploads() - up0));
            CHECK(img_read(st, 0, 40, 24) && !memcmp(g_xmap, lin0, 40 * 24 * 4), "level 0 reads back as rewritten");
            tex_pattern(lin0, 20, 12, 1, 0);
            CHECK(img_read(st, 1, 20, 12) && !memcmp(g_xmap, lin0, 20 * 12 * 4), "level 1 as it was");
            up0 = uploads(); draw_with(rtc, 5, 3);
            CHECK(uploads() == up0, "and nothing after it (%llu)", (unsigned long long)(uploads() - up0));
            // ReadTexels of a CPU-written texture: guest memory, de-laid-out
            { NVNcopyRegion r = {0, 0, 0, 40, 24, 1}; uint8_t *rt = calloc(40 * 24, 4); tex_pattern(lin0, 40, 24, 0, 1);
              mtl_read_texels(st, 0, 0, &r, rt, 40 * 4, 40 * 24 * 4);
              CHECK(!memcmp(rt, lin0, 40 * 24 * 4), "ReadTexels reads it from guest memory as written"); free(rt); }
            // A texture created later, which nothing wrote: cleared to zero at its first use
            tex_t *zt = tex_in(tp, 8 * pg, 16, 16, 1, NVN_FORMAT_RGBA8); memset(zt->mem, 0xee, (size_t)zt->L.size);   // bytes in guest memory, but not CPU-written: not uploaded
            mtl_reg_tex(8, zt, NULL);
            uint64_t z0 = ts_n(TS_ZERO);
            draw_with(rtc, 8, 3);
            CHECK(ts_n(TS_ZERO) == z0 + 1 && texel(zt, 3, 3) == 0 && layout_of(zt) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, "a texture nothing wrote is cleared to zero (%llu fills, texel %#x)", (unsigned long long)(ts_n(TS_ZERO) - z0), texel(zt, 3, 3));
            // It becomes CPU-written later (WriteTexels): uploaded from then on
            u1 = ts_n(TS_UP1); mtl_tex_cpu_written(zt);
            draw_with(rtc, 8, 3);
            CHECK(ts_n(TS_UP1) == u1 + 1 && texel(zt, 3, 3) == 0xeeeeeeee, "once CPU-written it is uploaded (%llu, texel %#x)", (unsigned long long)(ts_n(TS_UP1) - u1), texel(zt, 3, 3));
            // A format this slice does not upload: no host image (said in red once)
            tex_t *ft = tex_in(tp, 10 * pg, 16, 16, 1, NVN_FORMAT_RGBA16F); mtl_tex_cpu_written(ft); mtl_reg_tex(9, ft, NULL);
            draw_with(rtc, 9, 3);
            CHECK(img_of(ft) == NULL, "a CPU-written RGBA16F texture gets no host image");
            draw_with(rtc, 0, 0);
            free(lin0);
            pool_fin(tp);
            CHECK(writable(tp, n), "the texture pool's pages are writable once it is freed");
            munmap(tp, n);
        }
        // A pool whose first page is an edge page shared with the memory before it (16 KB hosts): a texture there is never protected and uploaded at every use
        if (pg > 4096) {
            uint8_t *m = mmap(NULL, 3 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0), *pa = m + 4096; size_t na = 3 * pg - 4096;
            CHECK(m != MAP_FAILED, "the edge-page test's memory");
            if (m != MAP_FAILED) {
                mtl_pool_add(pa, na); nvn_dirty_add_range(pa, na);
                tex_t *et = tex_in(pa, 0, 16, 16, 1, NVN_FORMAT_RGBA8); memset(et->mem, 0x33, (size_t)et->L.size); mtl_tex_cpu_written(et); mtl_reg_tex(11, et, NULL);
                uint64_t u1 = ts_n(TS_UP1), ra = ts_n(TS_REALW);
                draw_with(rtc, 11, 3);
                CHECK(ts_n(TS_UP1) == u1 + 1 && texel(et, 1, 1) == 0x33333333, "a texture in a pool's edge page is uploaded (%llu, %#x)", (unsigned long long)(ts_n(TS_UP1) - u1), texel(et, 1, 1));
                CHECK(writable(m, pg), "its page is not protected");   // (zeros written there by the check)
                draw_with(rtc, 11, 3);
                CHECK(ts_n(TS_REALW) == ra + 1 && texel(et, 1, 1) == 0, "it is uploaded at every use, written or not (%llu), and has the bytes as they are (%#x)", (unsigned long long)(ts_n(TS_REALW) - ra), texel(et, 1, 1));
                draw_with(rtc, 0, 0);
                nvn_dirty_remove_range(pa); mtl_pool_del(pa); munmap(m, 3 * pg);
            }
        }
        // ---------- Review round 1 (PLAN.md 2.3 "As built, slice 2b") ----------
        {
            size_t n2 = 16 * pg; uint8_t *p2 = pool_new(n2); pool_t pp = {0, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED, p2, n2};
            CHECK(p2 != NULL, "the second texture test pool");
            uint8_t *pat = malloc(64 * 64 * 4);
            if (p2 && pat) {
                // The dirty check's window: nvn_dirty.c's unprotect moves the generation, then stores the page's. A check between the two reads the new generation and finds the page clean.
                // Staged: one submission's replay reads the generation one ahead (as if a fault's unprotect had just taken it), its commit does not; then the guest write lands and faults (its
                // unprotect takes that same value). A "the generation has not moved since the last check" shortcut would skip the next check for good; the write must be uploaded
                tex_t *wt = tex_in_p(p2, &pp, 64, 64, NVN_FORMAT_RGBA8, 0, 0);
                tex_pattern(pat, 64, 64, 0, 3); nvn_swizzle(&wt->d, 0, 0, pat, 64 * 4, wt->mem); mtl_tex_cpu_written(wt); mtl_reg_tex(21, wt, NULL);
                draw_with(rtc, 21, 3);
                CHECK(!writable(p2, (size_t)wt->L.size), "the staged-window texture (%llu B) is uploaded and its pages protected", (unsigned long long)wt->L.size);
                uint64_t up0 = uploads(), re0 = ts_n(TS_RE), f0 = faults();
                rec_rt(rtc, NULL); rec_bind(21, 3); rec_draw(); rec(OP_END, NULL, 0);
                mtl_sub_begin(); atomic_store(&g_now_ahead, 1); mtl_exec(g_rec); atomic_store(&g_now_ahead, 0); mtl_sub_commit(); g_rn = 0;
                CHECK(uploads() == up0, "the staged submission uploads nothing (%llu)", (unsigned long long)(uploads() - up0));
                tex_pattern(pat, 64, 64, 0, 4); nvn_swizzle(&wt->d, 0, 0, pat, 64 * 4, wt->mem);
                CHECK(faults() > f0, "the guest write after it faults (%llu)", (unsigned long long)(faults() - f0));
                draw_with(rtc, 21, 3);
                CHECK(ts_n(TS_RE) == re0 + 1 && img_read(wt, 0, 64, 64) && !memcmp(g_xmap, pat, 64 * 64 * 4), "a write whose generation was taken before a check and stored after it is uploaded at the next use (%llu re-uploads)",
                      (unsigned long long)(ts_n(TS_RE) - re0));
                draw_with(rtc, 0, 0);
                // A LINEAR texture the CPU writes straight into its pool (nvn_mtl.m reads the pool memory itself; the front half sends no CPU-written notice): uploaded from the start and
                // tracked. Stride 0 (the builder's default): rows at nvn_tex_layout's pitch, 20 RGBA8 texels = 80 B rounded up to 96
                tex_t *lt = tex_in_p(p2 + 4 * pg, &pp, 20, 6, NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_LINEAR, 0);
                CHECK(lt->L.size == 96 * 6, "a stride-0 linear 20x6 RGBA8 texture is 96 B a row in storage (%llu B)", (unsigned long long)lt->L.size);
                tex_pattern(pat, 20, 6, 0, 5); for (int y = 0; y < 6; y++) memcpy(lt->mem + 96 * y, pat + 80 * y, 80);
                mtl_reg_tex(22, lt, NULL);
                uint64_t u1 = ts_n(TS_UP1), z0 = ts_n(TS_ZERO);
                draw_with(rtc, 22, 3);
                CHECK(ts_n(TS_UP1) == u1 + 1 && ts_n(TS_ZERO) == z0 && img_read(lt, 0, 20, 6) && !memcmp(g_xmap, pat, 20 * 6 * 4), "a linear texture written through its pool is uploaded from it, not zero-filled (%llu uploads, %llu fills)",
                      (unsigned long long)(ts_n(TS_UP1) - u1), (unsigned long long)(ts_n(TS_ZERO) - z0));
                up0 = uploads(); tex_pattern(pat, 20, 6, 0, 6); for (int y = 0; y < 6; y++) memcpy(lt->mem + 96 * y, pat + 80 * y, 80);
                draw_with(rtc, 22, 3);
                CHECK(uploads() == up0 + 1 && img_read(lt, 0, 20, 6) && !memcmp(g_xmap, pat, 20 * 6 * 4), "and uploaded again after the CPU rewrites it there (%llu)", (unsigned long long)(uploads() - up0));
                // A CPU-written (WriteTexels) linear 32x2 RGBA8 texture with stride 0: 128 B rows, 256 B of storage
                tex_t *l2 = tex_in_p(p2 + 5 * pg, &pp, 32, 2, NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_LINEAR, 0);
                tex_pattern(pat, 32, 2, 0, 7); memcpy(l2->mem, pat, 256); mtl_tex_cpu_written(l2); mtl_reg_tex(23, l2, NULL);
                u1 = ts_n(TS_UP1);
                draw_with(rtc, 23, 3);
                CHECK(l2->L.size == 256 && ts_n(TS_UP1) == u1 + 1 && img_read(l2, 0, 32, 2) && !memcmp(g_xmap, pat, 256), "a CPU-written stride-0 linear 32x2 texture (%llu B) is uploaded by rows of 128 B (%llu)",
                      (unsigned long long)l2->L.size, (unsigned long long)(ts_n(TS_UP1) - u1));
                // An explicit stride still rules: 16x4 RGBA8 at 256 B a row
                tex_t *l3 = tex_in_p(p2 + 6 * pg, &pp, 16, 4, NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_LINEAR, 256);
                tex_pattern(pat, 16, 4, 0, 8); for (int y = 0; y < 4; y++) memcpy(l3->mem + 256 * y, pat + 64 * y, 64);
                mtl_reg_tex(24, l3, NULL); draw_with(rtc, 24, 3);
                CHECK(img_read(l3, 0, 16, 4) && !memcmp(g_xmap, pat, 16 * 4 * 4), "a linear texture with a 256 B stride reads back as written");
                // Storage the CPU flushed before the texture's first use (nvn_flushed), nothing else writing it: filled from it once, not tracked; CPU-written later, uploaded as such
                tex_t *ff = tex_in_p(p2 + 7 * pg, &pp, 16, 16, NVN_FORMAT_RGBA8, 0, 0);
                tex_pattern(pat, 16, 16, 0, 9); nvn_swizzle(&ff->d, 0, 0, pat, 16 * 4, ff->mem); g_tflush_lo = ff->mem; g_tflush_hi = ff->mem + 64;
                mtl_reg_tex(25, ff, NULL);
                uint64_t i0 = ts_n(TS_INIT); u1 = ts_n(TS_UP1); z0 = ts_n(TS_ZERO);
                draw_with(rtc, 25, 3);
                CHECK(ts_n(TS_INIT) == i0 + 1 && ts_n(TS_UP1) == u1 && ts_n(TS_ZERO) == z0 && img_read(ff, 0, 16, 16) && !memcmp(g_xmap, pat, 16 * 16 * 4),
                      "a texture in flushed storage is filled from it at its first use (%llu fills, %llu uploads, %llu zero fills)", (unsigned long long)(ts_n(TS_INIT) - i0), (unsigned long long)(ts_n(TS_UP1) - u1),
                      (unsigned long long)(ts_n(TS_ZERO) - z0));
                draw_with(rtc, 25, 3);
                CHECK(ts_n(TS_INIT) == i0 + 1 && ts_n(TS_UP1) == u1, "once only (%llu fills)", (unsigned long long)(ts_n(TS_INIT) - i0));
                g_tflush_lo = g_tflush_hi = NULL;
                mtl_tex_cpu_written(ff); draw_with(rtc, 25, 3);
                CHECK(ts_n(TS_UP1) == u1 + 1, "once CPU-written it is uploaded as such (%llu)", (unsigned long long)(ts_n(TS_UP1) - u1));
                // A texture with an alpha-less format (RGBX8: the view reads alpha as one, a component swizzle) and a sampler with a LOD bias: valid on a portability-subset driver too
                // (MoltenVK: the features enabled; make check_t_vkn's KosmicKrisp has no such extension). Where the subset lacks the feature: an identity view and no bias (said once)
                tex_t *xt = tex_in_p(p2 + 8 * pg, &pp, 16, 16, NVN_FORMAT_RGBX8, 0, 0);
                tex_pattern(pat, 16, 16, 0, 10); nvn_swizzle(&xt->d, 0, 0, pat, 16 * 4, xt->mem); mtl_tex_cpu_written(xt); mtl_reg_tex(26, xt, NULL);
                samp_par bp = lin; bp.lod_bias = 1.5f; mtl_reg_samp(27, &bp);
                draw_with(rtc, 26, 27);
                CHECK(T.tv[4][0] != VK_NULL_HANDLE && T.ts[4][0] != VK_NULL_HANDLE && img_read(xt, 0, 16, 16) && !memcmp(g_xmap, pat, 16 * 16 * 4), "an RGBX8 texture's view and a biased sampler are made, its bytes uploaded as they are");
                { int sp = vd.port, sb = vd.port_bias, ss = vd.port_swz; const uint8_t sw[4] = {2, 3, 4, 1}; VkComponentSwizzle c[4];
                  vd.port = 1; vd.port_bias = 1; vd.port_swz = 1; sview_comp(sw, c); float b1 = samp_bias(1.5f);
                  CHECK(c[0] == VK_COMPONENT_SWIZZLE_IDENTITY && c[3] == VK_COMPONENT_SWIZZLE_ONE && b1 == (1.5f < vd.props.limits.maxSamplerLodBias ? 1.5f : vd.props.limits.maxSamplerLodBias),
                        "with the portability features: alpha mapped to one (%d), the bias kept (%g)", (int)c[3], b1);
                  vd.port_bias = 0; vd.port_swz = 0; sview_comp(sw, c); float b0 = samp_bias(1.5f);
                  CHECK(c[0] == VK_COMPONENT_SWIZZLE_IDENTITY && c[3] == VK_COMPONENT_SWIZZLE_IDENTITY && b0 == 0, "without them: an identity view (%d) and no bias (%g)", (int)c[3], b0);
                  vd.port = sp; vd.port_bias = sb; vd.port_swz = ss; }
                draw_with(rtc, 0, 0);
            }
            // Initial content in a CPU_NO_ACCESS pool (nvn_mtl.m tex_init_fill): what the storage held when the pool was made is the texture's content, filled once at its first use; a
            // COMPRESSIBLE one there (a render target) is cleared to zero instead
            uint8_t *nm = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); pool_t np = {0, NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED, nm, 2 * pg};
            CHECK(nm != MAP_FAILED, "the CPU_NO_ACCESS test pool");
            if (nm != MAP_FAILED && pat) {
                tex_t probe; memset(&probe, 0, sizeof probe); probe.d.width = probe.d.height = 32; probe.d.depth = 1; probe.d.levels = 1; probe.d.target = NVN_TEXTURE_TARGET_2D; probe.d.format = NVN_FORMAT_RGBA8;
                tex_pattern(pat, 32, 32, 0, 11); nvn_swizzle(&probe.d, 0, 0, pat, 32 * 4, nm); memset(nm + pg, 0x5c, pg);   // the pool's bytes before it is made
                mtl_pool_add(nm, 2 * pg); nvn_dirty_add_range(nm, 2 * pg);
                tex_t *it = tex_in_p(nm, &np, 32, 32, NVN_FORMAT_RGBA8, 0, 0), *ct = tex_in_p(nm + pg, &np, 32, 32, NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_COMPRESSIBLE, 0);
                CHECK(img_of(it) != NULL && layout_of(it) == VK_IMAGE_LAYOUT_UNDEFINED, "a texture in a CPU_NO_ACCESS pool has its host image from TextureInitialize, holding nothing yet");
                mtl_reg_tex(28, it, NULL); mtl_reg_tex(29, ct, NULL);
                uint64_t i0 = ts_n(TS_INIT), up0 = uploads(), z0 = ts_n(TS_ZERO);
                draw_with(rtc, 28, 3);
                CHECK(ts_n(TS_INIT) == i0 + 1 && uploads() == up0 && img_read(it, 0, 32, 32) && !memcmp(g_xmap, pat, 32 * 32 * 4), "it is filled from the pool's initial content at its first use (%llu fills, %llu uploads)",
                      (unsigned long long)(ts_n(TS_INIT) - i0), (unsigned long long)(uploads() - up0));
                draw_with(rtc, 28, 3);
                CHECK(ts_n(TS_INIT) == i0 + 1 && uploads() == up0, "once only (%llu)", (unsigned long long)(ts_n(TS_INIT) - i0));
                draw_with(rtc, 29, 3);
                CHECK(ts_n(TS_ZERO) == z0 + 1 && ts_n(TS_INIT) == i0 + 1 && texel(ct, 1, 1) == 0, "a COMPRESSIBLE one there is cleared to zero (%llu fills, texel %#x)", (unsigned long long)(ts_n(TS_ZERO) - z0), texel(ct, 1, 1));
                CHECK(writable(nm, pg), "the pool's pages are not protected");
                draw_with(rtc, 0, 0);
                nvn_dirty_remove_range(nm); mtl_pool_del(nm); munmap(nm, 2 * pg);
            }
            free(pat);
            if (p2) { pool_fin(p2); munmap(p2, n2); }
        }
        // A render target sampled later: one image. Across submissions (a layout job ahead of the submission), then within one (a barrier in it), then rendered again
        tex_t *rt = tex(32, 32, NVN_FORMAT_RGBA8);
        rec_rt(rt, NULL); rec_clear(0.f, 1.f, 0.f, 1.f); rec_draw(); submit();
        vkn_img *e0 = img_of(rt);
        CHECK(e0 && layout_of(rt) == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, "the target is rendered (layout %d)", (int)layout_of(rt));
        mtl_reg_tex(7, rt, NULL);
        uint64_t made0 = ts_n(TS_MADE), lu0 = ts_n(TS_LAY_UP), lc0 = ts_n(TS_LAY_CB), fb0 = ts_n(TS_FEED);
        draw_with(rtc, 7, 3);
        CHECK(img_of(rt) == e0 && ts_n(TS_MADE) == made0 && ts_n(TS_LAY_UP) == lu0 + 1 && layout_of(rt) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && texel(rt, 5, 5) == rgba(0.f, 1.f, 0.f, 1.f),
              "registered as a texture it is the same image (%d, %llu made), moved to shader-read ahead of the submission (%llu), with what was drawn (%#x)", img_of(rt) == e0,
              (unsigned long long)(ts_n(TS_MADE) - made0), (unsigned long long)(ts_n(TS_LAY_UP) - lu0), texel(rt, 5, 5));
        rec_rt(rt, NULL); rec_clear(1.f, 0.f, 0.f, 1.f); rec_draw(); rec_rt(rtc, NULL); rec_draw(); submit();   // slot 0 still holds it: bound while it is the target, then sampled after SET_RT
        CHECK(ts_n(TS_LAY_CB) == lc0 + 1 && ts_n(TS_FEED) == fb0 + 1 && layout_of(rt) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && texel(rt, 5, 5) == rgba(1.f, 0.f, 0.f, 1.f),
              "rendered and sampled in one submission: left alone while the target (%llu), a barrier to shader-read in it (%llu), layout %d, content %#x", (unsigned long long)(ts_n(TS_FEED) - fb0),
              (unsigned long long)(ts_n(TS_LAY_CB) - lc0), (int)layout_of(rt), texel(rt, 5, 5));
        rec_bind(0, 0); rec_rt(rt, NULL); rec_clear(0.f, 0.f, 1.f, 1.f); rec_draw(); submit();
        CHECK(img_of(rt) == e0 && layout_of(rt) == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && texel(rt, 5, 5) == rgba(0.f, 0.f, 1.f, 1.f), "and rendered again (layout %d, %#x)", (int)layout_of(rt), texel(rt, 5, 5));
        // Samplers: every min / mag filter and wrap mode, and the rest of the parameters; the same parameters share one VkSampler
        int all_ok = 1, sid = 100; uint64_t sm0 = ts_n(TS_SMADE), nset = 0;
        for (int mi = 0; mi <= 5; mi++) for (int ma = 0; ma <= 1; ma++) { samp_par p = lin; p.minf = (uint8_t)mi; p.magf = (uint8_t)ma; mtl_reg_samp(sid, &p); all_ok &= samp_lookup(sid++) != VK_NULL_HANDLE; nset += !(mi == 5 && ma == 1); }
        for (int w = 0; w <= 7; w++) { samp_par p = lin; p.ws = p.wt = p.wr = (uint8_t)w; if (w == 4 || w == 5) p.border[3] = p.border[0] = p.border[1] = p.border[2] = 1.f; mtl_reg_samp(sid, &p); all_ok &= samp_lookup(sid++) != VK_NULL_HANDLE; nset += w != 1; }
        { samp_par p = lin; p.aniso = 8.f; p.lod_bias = 0.5f; p.lod_min = 1.f; p.lod_max = 3.f; mtl_reg_samp(sid, &p); all_ok &= samp_lookup(sid++) != VK_NULL_HANDLE; nset++; }
        { samp_par p = lin; p.cmp_mode = 1; p.cmp_func = NVN_COMPARE_FUNC_LEQUAL; p.ws = p.wt = p.wr = NVN_WRAP_MODE_CLAMP_TO_BORDER; p.border[3] = 1.f; mtl_reg_samp(sid, &p); all_ok &= samp_lookup(sid++) != VK_NULL_HANDLE; nset++; }
        if (vd.minmax) { samp_par p = lin; p.reduction = NVN_SAMPLER_REDUCTION_MIN; p.minf = NVN_MIN_FILTER_NEAREST; mtl_reg_samp(sid, &p); all_ok &= samp_lookup(sid++) != VK_NULL_HANDLE; nset++; }
        CHECK(all_ok, "a VkSampler for every filter, wrap mode and parameter tried");
        CHECK(ts_n(TS_SMADE) - sm0 == nset, "one per distinct parameter set: %llu made, %llu sets", (unsigned long long)(ts_n(TS_SMADE) - sm0), (unsigned long long)nset);
        mtl_reg_samp(sid, &lin);
        CHECK(samp_lookup(sid) == samp_lookup(3) && ts_n(TS_SMADE) - sm0 == nset, "another ID with the same parameters shares its VkSampler");
    }

    // ---------- Draws (slice 2c, PLAN.md 2.5 "As built, slice 2c") ----------
    // Test modules (vkn_kernels/) under made-up keys with the reflection slice 0 writes; programs of two stages; vertices, indices and uniform data in one test pool (strategy B's shadow)
    {
        size_t pg = (size_t)getpagesize(), n = 16 * pg; uint8_t *dp = pool_new(n);
        CHECK(dp != NULL, "the draws' test pool");
        // The interface: the vertex shaders' inputs and their output (location 0, the colour), the pixel shaders' output (colour 0) and their input (location 0)
        static const char VIN2[] = "\"vertex_inputs\": [{\"location\": 0, \"type\": \"float\", \"components\": 4}, {\"location\": 1, \"type\": \"float\", \"components\": 4}], "
                                   "\"outputs\": [{\"location\": 0, \"type\": \"float\", \"components\": 4}]",
                          VIN1[] = "\"vertex_inputs\": [{\"location\": 0, \"type\": \"float\", \"components\": 4}], \"outputs\": [{\"location\": 0, \"type\": \"float\", \"components\": 4}]",
                          OUT1[] = "\"outputs\": [{\"location\": 0, \"type\": \"float\", \"components\": 4}], \"inputs\": [{\"location\": 0, \"type\": \"float\", \"components\": 4}]",
                          UBO[] = "{\"name\": \"vp_c3\", \"kind\": \"cbuf\", \"type_name\": \"UNIFORM_BUFFER\", \"set\": 0, \"binding\": 1, \"count\": 1, \"reg\": 0}",
                          TEX2[] = "{\"name\": \"t0\", \"kind\": \"tex\", \"type_name\": \"COMBINED_IMAGE_SAMPLER\", \"set\": 1, \"binding\": 1, \"count\": 1, \"reg\": 0, \"sampler\": {\"kind\": \"samp_tex\", \"reg\": 0}, "
                                   "\"image\": {\"dim\": \"2D\", \"arrayed\": 0, \"ms\": 0, \"depth\": 0, \"comp\": \"float\"}}, "
                                   "{\"name\": \"t1\", \"kind\": \"tex\", \"type_name\": \"COMBINED_IMAGE_SAMPLER\", \"set\": 1, \"binding\": 2, \"count\": 1, \"reg\": 1, \"sampler\": {\"kind\": \"samp_tex\", \"reg\": 1}, "
                                   "\"image\": {\"dim\": \"2D\", \"arrayed\": 0, \"ms\": 0, \"depth\": 0, \"comp\": \"float\"}}";
        const uint8_t *kv = t_key(0x7e57c0de00000001ull), *kvu = t_key(0x7e57c0de00000002ull), *kp = t_key(0x7e57c0de00000003ull), *kpt = t_key(0x7e57c0de00000004ull),
                      *kbad = t_key(0x7e57c0de00000005ull), *kv3 = t_key(0x7e57c0de00000006ull), *kpt3 = t_key(0x7e57c0de00000007ull), *kmis = t_key(0x7e57c0de00000008ull);
        uint64_t ld0, rf0, ld1, rf1; vkn_shader_stats(&ld0, &rf0);
        vkn_shader *sv = t_mod(kv, 0, "ok", t_vs_vert_spv, sizeof t_vs_vert_spv, "", VIN2), *svu = t_mod(kvu, 0, "ok", t_vs_ubo_vert_spv, sizeof t_vs_ubo_vert_spv, UBO, VIN1),
                   *sp = t_mod(kp, 1, "ok", t_ps_frag_spv, sizeof t_ps_frag_spv, "", OUT1), *spt = t_mod(kpt, 1, "ok", t_ps_tex_frag_spv, sizeof t_ps_tex_frag_spv, TEX2, OUT1),
                   *sv3 = t_mod(kv3, 0, "ok", t_vs_ubo_vert_spv, sizeof t_vs_ubo_vert_spv, UBO, VIN1), *spt3 = t_mod(kpt3, 1, "ok", t_ps_tex_frag_spv, sizeof t_ps_tex_frag_spv, TEX2, OUT1);
        vkn_shader_stats(&ld1, &rf1);
        CHECK(sv && svu && sp && spt && sv3 && spt3 && ld1 - ld0 == 6 && rf1 == rf0, "the test modules are made (%llu loaded, %llu refused)", (unsigned long long)(ld1 - ld0), (unsigned long long)(rf1 - rf0));
        CHECK(sv && svu && sp && spt && sv->nin == 2 && svu->nb == 1 && svu->b[0].binding == 1 && spt->nb == 2 && spt->b[0].binding == 33 && spt->b[1].binding == 34 && sp->out_mask == 1,
              "in the one set: the vertex shader's binding 1 stays 1, the pixel shader's 1 and 2 become 33 and 34");
        vkn_shader *sbad = t_mod(kbad, 0, "unsupported", t_vs_vert_spv, sizeof t_vs_vert_spv, "", VIN2), *smis = t_mod(kmis, 1, "ok", t_ps_tex_frag_spv, sizeof t_ps_tex_frag_spv, "", OUT1);
        vkn_shader_stats(&ld1, &rf1);
        CHECK(!sbad && !smis && rf1 - rf0 == 2, "a module whose build status is not ok, and one whose SPIR-V has resources its reflection lacks, are refused (said in red; %llu refused)", (unsigned long long)(rf1 - rf0));
        prog_t *P = t_prog(kv, kp), *PU = t_prog(kvu, kp), *PT = t_prog(kv, kpt), *PB = t_prog(kbad, kp), *PL = t_prog(kv3, kpt3);
        // Vertices (stream 0 at the pool's start) and u16 indices (at 4 KiB); the uniform data at an aligned address, at one 16 B past an aligned one, and 1 KiB before the pool's end
        t_vtx *V = (t_vtx *)(void *)dp; uint16_t *I = (uint16_t *)(void *)(dp + 4096);
        static const float cx[3] = {-1, 0, -1}, cy[3] = {-1, -1, 0}, ax[3] = {-1, 1, -1}, ay[3] = {-1, -1, 0}, qx[4] = {-1, 1, -1, 1}, qy[4] = {-1, -1, 1, 1};
        for (int i = 0; dp && i < 3; i++) {
            V[i] = (t_vtx){{cx[i], cy[i], 0, 1}, {1, 0, 0, 1}};            // 0-2: the corner triangle (NDC's lower-left quarter, counter-clockwise in NDC), red
            V[8 + i] = (t_vtx){{cx[i], cy[i], 0, 1}, {0, 1, 0, 1}};        // 8-10: the same in green (base vertex 8)
            V[12 + i] = (t_vtx){{cx[i], cy[i], -0.5f, 1}, {1, 0, 0, 1}};   // 12-14: red at z -0.5
            V[16 + i] = (t_vtx){{cx[i], cy[i], 0.5f, 1}, {0, 0, 1, 1}};    // 16-18: blue at z 0.5
            V[20 + i] = (t_vtx){{ax[i], ay[i], 0, 1}, {1, 0, 0, 1}};       // 20-22: the asymmetric triangle (no mirror image of itself about x = y), red
        }
        for (int i = 0; dp && i < 4; i++) V[4 + i] = (t_vtx){{qx[i], qy[i], 0, 1}, {(qx[i] + 1) / 2, (qy[i] + 1) / 2, 0, 1}};   // 4-7: the whole viewport; the colour's xy is the texture coordinate, (0, 0) at NDC's lower left
        static const uint16_t ix[9] = {0, 1, 2, 0, 1, 2, 2, 1, 3};   // I: the triangle; I + 3: the quad's two (base vertex 4)
        const float ud[8] = {0.2f, 0, 0, 0.5f, 0, 0.6f, 0, 0.5f};   // c[0] + c[1] = (0.2, 0.6, 0, 1)
        uint8_t *ua = dp + 2 * pg, *uu = dp + 2 * pg + 1024 + 16, *ue = dp + n - 1024;
        if (dp) { memcpy(I, ix, sizeof ix); memcpy(ua, ud, sizeof ud); memcpy(uu, ud, sizeof ud); memcpy(ue, ud, sizeof ud); }
        tex_t *A = tex(64, 64, NVN_FORMAT_RGBA8), *Bt = tex(64, 64, NVN_FORMAT_RGBA8), *Dz = tex(64, 64, NVN_FORMAT_DEPTH32F);
        const uint32_t red = rgba(1, 0, 0, 1), green = rgba(0, 1, 0, 1), blue = rgba(0, 0, 1, 1), black = rgba(0, 0, 0, 1);
        if (dp && sv && sp && spt && svu) {
            // The orientation matrix: origin lower left / upper left x NEGATIVE_Y off / on x front face CW / CCW x cull none / back / front, an asymmetric triangle read back by a copy (V 20-22:
            // NDC (-1,-1), (1,-1), (-1,0); its long edge on NDC's y -1, the viewport's width wide and half its height high, so a transposed picture fails as a mirrored one does). Rows: NVN's
            // lower-left origin puts NDC's y -1 at row 0 (as GL), the upper-left one at the last row; NEGATIVE_Y mirrors y in clip space. Facing: the triangle is counter-clockwise in NDC, so
            // front-facing for front face CCW with either origin (GL clip control's upper left reverses the winding with the rows), and NEGATIVE_Y's mirror makes it back-facing. That is the
            // code's model (facing after the swizzle, as NV_viewport_swizzle); the matrix cannot catch an error the model shares (PLAN.md 2.5, open after slice 2c)
            static const char *const cn[3] = {"none", "back", "front"};
            uint64_t di0 = atomic_load(&g_dr_idx), pm0 = atomic_load(&g_pso_made);
            for (int ll = 1; ll >= 0; ll--) for (int neg = 0; neg < 2; neg++) for (int fr = 0; fr < 2; fr++) for (int cu = 0; cu < 3; cu++) {
                unsigned front = fr ? NVN_FRONT_FACE_CCW : NVN_FRONT_FACE_CW, cull = cu == 0 ? NVN_FACE_NONE : cu == 1 ? NVN_FACE_BACK : NVN_FACE_FRONT;
                mtl_set_origin_lower_left(ll);
                rec_rt(A, NULL); rec_clear(0, 0, 0, 1); rec_prog(P); rec_vbuf(V, 1024, 2); rec_vp(64, 64); rec_sc(64, 64); rec_swz(neg); rec_poly(cull, front); rec_idx(I, 3, 20); submit();
                int is_front = fr ^ neg, drawn = cu == 0 || (cu == 1) == is_front, flip = !ll ^ neg, ew = -1;
                t_cov c = {{0}, -1, 0, 0, 0, 0}; if (img_read(A, 0, 64, 64)) { c = cover(red); ew = row_n(red, flip ? 63 : 0); }
                int ok = drawn ? c.n == 1024 && c.c0 == 0 && c.c1 == 62 && ew == 63 && (flip ? c.r0 == 32 && c.r1 == 63 : c.r0 == 0 && c.r1 == 31) : c.n == 0;
                CHECK(ok, "origin %s, NEGATIVE_Y %s, front face %s, cull %s: %s; %d pixels covered (want 1024), rows %d-%d, columns %d-%d, %d on the long edge's row (want 63)", ll ? "lower left" : "upper left",
                      neg ? "on" : "off", fr ? "CCW" : "CW", cn[cu], !drawn ? "culled" : flip ? "the triangle in rows 32-63, its long edge on row 63 (NDC's y -1 the last row)" : "the triangle in rows 0-31, its long edge on row 0 (NDC's y -1 row 0)",
                      c.n, c.r0, c.r1, c.c0, c.c1, ew);
            }
            CHECK(atomic_load(&g_dr_idx) - di0 == 24 && atomic_load(&g_pso_made) > pm0, "the 24 were recorded as indexed draws (%llu), %llu pipelines made", (unsigned long long)(atomic_load(&g_dr_idx) - di0),
                  (unsigned long long)(atomic_load(&g_pso_made) - pm0));
            // A viewport off the target's origin and short of its height (y 8, height 40: rows 8-47; mirrored about the target's middle it would be rows 16-55), with either origin and NEGATIVE_Y:
            // its rows are the same with either origin and NDC's y -1 is its first row (lower left) or its last (upper left), NEGATIVE_Y swapping them, as nvn_mtl.m's viewport (originY + height,
            // height negated, for the lower-left origin and again for NEGATIVE_Y, over Metal's top-down rows). Then a scissor of rows 4-43 and columns 0-47 (mirrored: rows 20-59) over the whole
            // viewport: a scissor's rows are memory rows with either origin (nvn_mtl.m passes it as it is)
            for (int ll = 1; ll >= 0; ll--) for (int neg = 0; neg < 2; neg++) {
                mtl_set_origin_lower_left(ll); int flip = !ll ^ neg, ew = -1;
                rec_rt(A, NULL); rec_clear(0, 0, 0, 1); rec_prog(P); rec_vbuf(V, 1024, 2); rec_vpr(0, 8, 64, 40); rec_sc(64, 64); rec_swz(neg); rec_poly(NVN_FACE_NONE, NVN_FRONT_FACE_CCW); rec_idx(I, 3, 20); submit();
                t_cov c = {{0}, -1, 0, 0, 0, 0}; if (img_read(A, 0, 64, 64)) { c = cover(red); ew = row_n(red, flip ? 47 : 8); }
                CHECK(c.n >= 636 && c.n <= 644 && c.c0 == 0 && c.c1 == 61 && ew == 62 && (flip ? c.r0 == 28 && c.r1 == 47 : c.r0 == 8 && c.r1 == 27),
                      "viewport rows 8-47, origin %s, NEGATIVE_Y %s: the triangle in rows %s; %d pixels (want 640), rows %d-%d, columns %d-%d, %d on the long edge's row (want 62)", ll ? "lower left" : "upper left",
                      neg ? "on" : "off", flip ? "28-47, its long edge on row 47" : "8-27, its long edge on row 8", c.n, c.r0, c.r1, c.c0, c.c1, ew);
                rec_rt(A, NULL); rec_clear(0, 0, 0, 1); rec_vp(64, 64); rec_scr(0, 4, 48, 40); rec_idx(I, 3, 20); submit();
                c = (t_cov){{0}, -1, 0, 0, 0, 0}; if (img_read(A, 0, 64, 64)) c = cover(red);
                CHECK(c.c0 == 0 && (flip ? c.n == 144 && c.r0 == 32 && c.r1 == 43 && c.c1 == 22 : c.n == 768 && c.r0 == 4 && c.r1 == 31 && c.c1 == 47),
                      "scissor rows 4-43, columns 0-47, origin %s, NEGATIVE_Y %s: the triangle cut to rows %s; %d pixels (want %d), rows %d-%d, columns %d-%d", ll ? "lower left" : "upper left", neg ? "on" : "off",
                      flip ? "32-43 (columns 0-22)" : "4-31 (columns 0-47)", c.n, flip ? 144 : 768, c.r0, c.r1, c.c0, c.c1);
                rec_sc(64, 64);
            }
            mtl_set_origin_lower_left(1); rec_swz(0); submit();
            // The target drawn, then sampled in the next pass of the same submission over the whole viewport at texture coordinates from NDC's lower left (a barrier to shader-read in the
            // command buffer): with the lower-left origin NDC's y -1 is row 0 as texture row 0, so the copy is the target as it is; with the upper-left one it is the target upside down.
            // Slot 1 of the pixel shader has nothing bound: the dummy (zeros) is pushed for it
            samp_par nst = {NVN_MIN_FILTER_NEAREST, NVN_MAG_FILTER_NEAREST, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_CLAMP_TO_EDGE, 0, 0, 0, 0.f, 1000.f, 0.f, 1.f, {0, 0, 0, 0}};
            mtl_reg_samp(41, &nst); mtl_reg_tex(40, A, NULL);
            uint8_t *apx = malloc(64 * 64 * 4);
            for (int ll = 1; apx && ll >= 0; ll--) {
                mtl_set_origin_lower_left(ll);
                uint64_t lc0 = ts_n(TS_LAY_CB), dd0 = atomic_load(&g_ddum);
                rec_rt(A, NULL); rec_clear(0, 0, 0, 1); rec_prog(P); rec_vbuf(V, 1024, 2); rec_vp(64, 64); rec_sc(64, 64); rec_swz(0); rec_poly(NVN_FACE_NONE, NVN_FRONT_FACE_CCW); rec_idx(I, 3, 0);
                rec_rt(Bt, NULL); rec_clear(0, 0, 1, 1); rec_prog(PT); rec_tex(1, 0, 40, 41); rec_idx(I + 3, 6, 4); submit();
                int ra = img_read(A, 0, 64, 64); if (ra) memcpy(apx, g_xmap, 64 * 64 * 4);
                int same = ra && img_read(Bt, 0, 64, 64), rows = 0;
                for (int y = 0; same && y < 64; y++) { same = !memcmp(g_xmap + (size_t)y * 256, apx + (size_t)(ll ? y : 63 - y) * 256, 256); rows += same; }
                CHECK(same && ts_n(TS_LAY_CB) > lc0 && atomic_load(&g_ddum) > dd0, "%s origin: drawn, then sampled in the next pass of the submission (a barrier in it: %llu), the copy is the target %s (%d rows); the unbound slot reads the dummy (%llu)",
                      ll ? "lower-left" : "upper-left", (unsigned long long)(ts_n(TS_LAY_CB) - lc0), ll ? "as it is" : "upside down", rows, (unsigned long long)(atomic_load(&g_ddum) - dd0));
            }
            mtl_set_origin_lower_left(1);
            // Across submissions: the target ends one as a colour target, the next samples it (its layout moved ahead of that submission)
            uint64_t lu0 = ts_n(TS_LAY_UP);
            rec_rt(A, NULL); rec_clear(0, 0, 0, 1); rec_prog(P); rec_vbuf(V, 1024, 2); rec_vp(64, 64); rec_sc(64, 64); rec_idx(I, 3, 8); submit();
            rec_rt(Bt, NULL); rec_clear(0, 0, 1, 1); rec_prog(PT); rec_tex(1, 0, 40, 41); rec_idx(I + 3, 6, 4); submit();
            int ra = apx && img_read(A, 0, 64, 64); if (ra) memcpy(apx, g_xmap, 64 * 64 * 4);
            t_cov cg = ra ? cover(green) : (t_cov){{0}, -1, 0, 0, 0, 0};
            int same = ra && img_read(Bt, 0, 64, 64) && !memcmp(g_xmap, apx, 64 * 64 * 4);
            CHECK(same && ts_n(TS_LAY_UP) == lu0 + 1 && cg.n == cg.q[0] && cg.n >= 480 && cg.r0 == 0 && cg.c0 == 0, "base vertex 8 draws the green copy (%d pixels in rows %d-%d); sampled by the next submission (its layout moved ahead of it: %llu), the copy is the target",
                  cg.n, cg.r0, cg.r1, (unsigned long long)(ts_n(TS_LAY_UP) - lu0));
            // A CPU-written texture (slice 2b's upload) sampled through the pushed descriptors: the copy is its bytes
            tex_t *ct = tex_in(dp, 4 * pg, 64, 64, 1, NVN_FORMAT_RGBA8); uint8_t *pat = malloc(64 * 64 * 4);
            if (pat) { tex_pattern(pat, 64, 64, 0, 12); nvn_swizzle(&ct->d, 0, 0, pat, 64 * 4, ct->mem); }
            mtl_tex_cpu_written(ct); mtl_reg_tex(42, ct, NULL);
            uint64_t pu0 = atomic_load(&g_push), dw0 = atomic_load(&g_dwr);
            rec_rt(Bt, NULL); rec_clear(0, 0, 1, 1); rec_prog(PT); rec_vbuf(V, 1024, 2); rec_vp(64, 64); rec_sc(64, 64); rec_tex(1, 0, 42, 41); rec_idx(I + 3, 6, 4); submit();
            CHECK(pat && img_read(Bt, 0, 64, 64) && !memcmp(g_xmap, pat, 64 * 64 * 4) && atomic_load(&g_push) == pu0 + 1 && atomic_load(&g_dwr) == dw0 + 2,
                  "a CPU-written texture sampled through the pushed descriptors: the copy is its bytes (%llu push, %llu writes)", (unsigned long long)(atomic_load(&g_push) - pu0), (unsigned long long)(atomic_load(&g_dwr) - dw0));
            free(pat);
            // Uniform buffers (vertex stage, slot 0; the shader declares 64 KiB, its colour is the first two vec4 summed): 256 B at an aligned address (a range short of the block: valid);
            // 256 B 16 B past one, which the device cannot bind (minUniformBufferOffsetAlignment): copied into the submission's ring; 64 KiB 1 KiB before the pool's end: cut to the pool
            const uint64_t al = vd.props.limits.minUniformBufferOffsetAlignment; const uint32_t uw = rgba(0.2f, 0.6f, 0, 1);
            const struct { const uint8_t *p; uint64_t sz; const char *what; } uc[3] = {{ua, 256, "256 B at an aligned address"}, {uu, 256, "256 B 16 B past an aligned address"}, {ue, 65536, "64 KiB 1 KiB before the pool's end"}};
            for (int i = 0; i < 3; i++) {
                uint64_t un0 = atomic_load(&g_ubo_unal), sh0 = atomic_load(&g_ubo_short), nb0 = atomic_load(&g_ubo_n); int unal = al && (uint64_t)(uc[i].p - dp) % al != 0;
                rec_rt(A, NULL); rec_clear(0, 0, 0, 1); rec_prog(PU); rec_vbuf(V, 1024, 1); rec_vp(64, 64); rec_sc(64, 64); rec_ubo_at(0, 0, uc[i].p, uc[i].sz); rec_idx(I, 3, 0); submit();
                uint32_t t = texel(A, 2, 2);
                CHECK(t == uw && atomic_load(&g_ubo_n) == nb0 + 1 && atomic_load(&g_ubo_unal) - un0 == (uint64_t)unal && atomic_load(&g_ubo_short) - sh0 == (i == 2),
                      "a uniform buffer of %s (alignment %llu): the colour %#x (want %#x); %llu copied for its alignment, %llu cut short", uc[i].what, (unsigned long long)al, t, uw,
                      (unsigned long long)(atomic_load(&g_ubo_unal) - un0), (unsigned long long)(atomic_load(&g_ubo_short) - sh0));
            }
            // A vertex input nothing feeds (only the position bound): it reads the zero buffer
            rec_rt(A, NULL); rec_clear(0, 0, 1, 1); rec_prog(P); rec_vbuf(V, 1024, 1); rec_vp(64, 64); rec_sc(64, 64); rec_idx(I, 3, 0); submit();
            CHECK(texel(A, 2, 2) == 0 && texel(A, 60, 60) == blue, "a vertex input nothing feeds reads zeros: %#x", texel(A, 2, 2));
            // Depth, NVN's clip z from -w (depth clip control's negativeOneToOne): z -0.5 stores 0.25, and the triangle behind it (z 0, depth 0.5) fails the test LESS. With NEAR_IS_ZERO
            // (mtl_set_depth_zero) z -0.5 is clipped and z 0.5 stores 0.5
            ds_st dson = {1, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1}, dsoff = dson; dsoff.dt = 0;
            rec_rt(A, Dz); rec_clear(0, 0, 0, 1); rec_clear_d(1.f); rec_state(5, &dson, sizeof dson); rec_prog(P); rec_vbuf(V, 1024, 2); rec_vp(64, 64); rec_sc(64, 64); rec_idx(I, 3, 12); rec_idx(I, 3, 8); submit();
            float d1 = depth_at(Dz, 2, 2), d1o = depth_at(Dz, 60, 60); uint32_t c1 = texel(A, 2, 2);
            CHECK(vd.clip_ctl && d1 == 0.25f && d1o == 1.f && c1 == red, "near at -w (depth clip control %d): z -0.5 stores depth %g (want 0.25; %g outside), the triangle behind it fails the test (%#x)", vd.clip_ctl, d1, d1o, c1);
            mtl_set_depth_zero(1);
            rec_rt(A, Dz); rec_clear(0, 0, 0, 1); rec_clear_d(1.f); rec_idx(I, 3, 12); submit();
            float d2 = depth_at(Dz, 2, 2); uint32_t c2 = texel(A, 2, 2);
            rec_rt(A, Dz); rec_idx(I, 3, 16); submit();
            float d3 = depth_at(Dz, 2, 2); uint32_t c3 = texel(A, 2, 2);
            CHECK(d2 == 1.f && c2 == black && d3 == 0.5f && c3 == blue, "near at 0 (NEAR_IS_ZERO): z -0.5 is clipped (depth %g, %#x), z 0.5 stores 0.5 (%g, %#x)", d2, c2, d3, c3);
            mtl_set_depth_zero(0);
            // The pipeline cache: the same draw again is a hit; blending on is a new pipeline (ONE, ONE: the clear's green plus red); blending off again is a hit of the first
            rec_state(5, &dsoff, sizeof dsoff);
            rec_rt(A, NULL); rec_clear(0, 0.2f, 0, 1); rec_prog(P); rec_vbuf(V, 1024, 2); rec_vp(64, 64); rec_sc(64, 64); rec_swz(0); rec_poly(NVN_FACE_NONE, NVN_FRONT_FACE_CCW); rec_idx(I, 3, 0); submit();
            uint64_t m1 = atomic_load(&g_pso_made), h1 = atomic_load(&g_pso_hit);
            rec_rt(A, NULL); rec_clear(0, 0.2f, 0, 1); rec_idx(I, 3, 0); submit();
            CHECK(atomic_load(&g_pso_made) == m1 && atomic_load(&g_pso_hit) == h1 + 1 && texel(A, 2, 2) == red, "the same draw again: a cache hit (%llu made, %llu hits)", (unsigned long long)(atomic_load(&g_pso_made) - m1),
                  (unsigned long long)(atomic_load(&g_pso_hit) - h1));
            blend_st b1 = {0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD, 0, 0, 0};
            color_st con = {1, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}, coff = {0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS};
            rec_rt(A, NULL); rec_clear(0, 0.2f, 0, 1); rec_state(0, &b1, sizeof b1); rec_state(1, &con, sizeof con); rec_idx(I, 3, 0); submit();
            uint32_t cb1 = texel(A, 2, 2);
            CHECK(atomic_load(&g_pso_made) == m1 + 1 && cb1 == rgba(1, 0.2f, 0, 1), "blending on: a new pipeline (%llu), red added to the clear's green (%#x)", (unsigned long long)(atomic_load(&g_pso_made) - m1), cb1);
            uint64_t h2 = atomic_load(&g_pso_hit);
            rec_rt(A, NULL); rec_clear(0, 0.2f, 0, 1); rec_state(1, &coff, sizeof coff); rec_idx(I, 3, 0); submit();
            CHECK(atomic_load(&g_pso_made) == m1 + 1 && atomic_load(&g_pso_hit) == h2 + 1 && texel(A, 2, 2) == red, "blending off again: the first pipeline, from the cache (%llu made, %llu hits)",
                  (unsigned long long)(atomic_load(&g_pso_made) - m1), (unsigned long long)(atomic_load(&g_pso_hit) - h2));
            // A program whose vertex shader was refused: its draws are skipped (the target as cleared). A pair past the device's push descriptors (lowered to 2 here, the pair has 3): skipped
            uint64_t s0 = dk(DK_SHADER);
            rec_rt(A, NULL); rec_clear(0, 0, 1, 1); rec_prog(PB); rec_idx(I, 3, 0); submit();
            CHECK(dk(DK_SHADER) == s0 + 1 && texel(A, 2, 2) == blue, "a draw whose vertex shader was refused is skipped (%llu), the target as cleared (%#x)", (unsigned long long)(dk(DK_SHADER) - s0), texel(A, 2, 2));
            uint32_t mp = vd.max_push; vd.max_push = 2; s0 = dk(DK_SHADER);
            rec_rt(A, NULL); rec_clear(0, 0, 1, 1); rec_prog(PL); rec_ubo_at(0, 0, ua, 256); rec_tex(1, 0, 42, 41); rec_idx(I + 3, 6, 4); submit();
            vd.max_push = mp;
            CHECK(sv3 && spt3 && dk(DK_SHADER) == s0 + 1 && texel(A, 2, 2) == blue, "a pair with 3 bindings past 2 push descriptors is skipped (said in red; %llu)", (unsigned long long)(dk(DK_SHADER) - s0));
            // ---- Review round 1 of slice 2c ----
            // A pixel shader reading combined slot 0 as unsigned integers (usampler2D, texture()): its red is the texel's red over 255
            static const char UTEX1[] = "{\"name\": \"t0\", \"kind\": \"tex\", \"type_name\": \"COMBINED_IMAGE_SAMPLER\", \"set\": 1, \"binding\": 1, \"count\": 1, \"reg\": 0, \"sampler\": {\"kind\": \"samp_tex\", \"reg\": 0}, "
                                        "\"image\": {\"dim\": \"2D\", \"arrayed\": 0, \"ms\": 0, \"depth\": 0, \"comp\": \"uint\"}}";
            const uint8_t *kpu = t_key(0x7e57c0de00000009ull);
            vkn_shader *spu = t_mod(kpu, 1, "ok", t_ps_utex_frag_spv, sizeof t_ps_utex_frag_spv, UTEX1, OUT1);
            prog_t *PS = t_prog(kv, kpu);
            CHECK(spu && spu->nb == 1 && spu->b[0].comp == 1, "the unsigned-integer test module is made");
            // A stencil-mode view of a depth-stencil target (DEPTH24_STENCIL8: D24_UNORM_S8_UINT, or D32_SFLOAT_S8_UINT where the device lacks it), cleared to stencil 0x5a, then sampled in the
            // next pass as unsigned integers with a nearest sampler: the view's class is its stencil aspect's (unsigned integers), not the image's depth format's, so the view itself is pushed
            tex_t *Ds = tex(64, 64, NVN_FORMAT_DEPTH24_STENCIL8);
            view_t dsv; memset(&dsv, 0, sizeof dsv); dsv.set_dsmode = 1; dsv.dsmode = NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL;
            mtl_reg_tex(43, Ds, &dsv);
            const p_clear_ds cst = {1.f, 1, 0x5a, 0xff};
            uint64_t dd1 = atomic_load(&g_ddum);
            rec_rt(A, Ds); rec_clear(0, 0, 0, 1); rec(OP_CLEAR_DS, &cst, sizeof cst);
            rec_rt(Bt, NULL); rec_clear(0, 0, 1, 1); rec_prog(PS); rec_vbuf(V, 1024, 2); rec_vp(64, 64); rec_sc(64, 64); rec_tex(1, 0, 43, 41); rec_idx(I + 3, 6, 4); submit();
            uint32_t sv1 = texel(Bt, 10, 10), sv2 = texel(Bt, 50, 40);
            CHECK(spu && (sv1 & 0xff) == 0x5a && (sv2 & 0xff) == 0x5a && atomic_load(&g_ddum) == dd1, "a stencil view of a depth-stencil target sampled as unsigned integers reads its stencil (red %#x and %#x, want 0x5a; %llu dummies)",
                  sv1 & 0xff, sv2 & 0xff, (unsigned long long)(atomic_load(&g_ddum) - dd1));
            // A linear sampler on a binding read as integers: the dummy of an unbound slot (R8G8B8A8_UINT) and the stencil view both get its nearest twin (no linear filter on an integer format:
            // VUID-vkCmdDrawIndexed-magFilter-04553), validation silent around these draws
            samp_par lint = {NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR, NVN_MAG_FILTER_LINEAR, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_CLAMP_TO_EDGE, 0, 0, 0, 0.f, 1000.f, 0.f, 4.f, {0, 0, 0, 0}};
            mtl_reg_samp(44, &lint);
            mtl_finish(); uint64_t vq0 = vkn_validation_count(); dd1 = atomic_load(&g_ddum);
            rec_rt(Bt, NULL); rec_clear(0, 0, 1, 1); rec_prog(PS); rec_tex(1, 0, 0, 44); rec_idx(I + 3, 6, 4); submit();
            uint32_t sd = texel(Bt, 10, 10); uint64_t dd2 = atomic_load(&g_ddum);
            rec_rt(Bt, NULL); rec_clear(0, 0, 1, 1); rec_tex(1, 0, 43, 44); rec_idx(I + 3, 6, 4); submit();
            uint32_t sl = texel(Bt, 10, 10); uint64_t vq1 = vkn_validation_count();
            CHECK(spu && (sd & 0xff) == 0 && dd2 == dd1 + 1 && (sl & 0xff) == 0x5a && vq1 == vq0, "a linear sampler on an integer binding: the unsigned dummy (red %#x, %llu dummies) and the stencil view (red %#x) are sampled through its nearest twin, "
                  "%llu validation messages", sd & 0xff, (unsigned long long)(dd2 - dd1), sl & 0xff, (unsigned long long)(vq1 - vq0));
            // Indices past their pool's end: the pool's last 4 bytes hold 2 u16 indices; a draw of 3 reads 6 B from there and is skipped (said in red), one of 2 is recorded
            uint16_t *Ie = (uint16_t *)(void *)(dp + n - 4); Ie[0] = 0; Ie[1] = 1;
            mtl_finish(); vq0 = vkn_validation_count(); uint64_t rs0 = dk(DK_RES), xi0 = atomic_load(&g_dr_idx);
            rec_rt(A, NULL); rec_clear(0, 0, 1, 1); rec_prog(P); rec_vbuf(V, 1024, 2); rec_vp(64, 64); rec_sc(64, 64); rec_idx(Ie, 3, 0); rec_idx(Ie, 2, 0); submit();
            uint32_t ci = texel(A, 2, 2); vq1 = vkn_validation_count();
            CHECK(dk(DK_RES) == rs0 + 1 && atomic_load(&g_dr_idx) == xi0 + 1 && vq1 == vq0 && ci == blue, "3 indices with 4 B left in their pool are skipped (%llu), 2 recorded (%llu); %llu validation messages, the target %#x",
                  (unsigned long long)(dk(DK_RES) - rs0), (unsigned long long)(atomic_load(&g_dr_idx) - xi0), (unsigned long long)(vq1 - vq0), ci);
            // Without indices: a stream bound 96 B before the pool's end feeds the 3 vertices of 32 B exactly (drawn); bound 64 B before it, the same draw would read 32 B past the pool (skipped)
            t_vtx *Ve = (t_vtx *)(void *)(dp + n - 96);
            for (int i = 0; i < 3; i++) Ve[i] = (t_vtx){{cx[i], cy[i], 0, 1}, {1, 0, 0, 1}};
            p_draw dr3; memset(&dr3, 0, sizeof dr3); dr3.prim = NVN_DRAW_PRIMITIVE_TRIANGLES; dr3.count = 3;
            rs0 = dk(DK_RES); uint64_t dn0 = atomic_load(&g_dr_n);
            rec_rt(A, NULL); rec_clear(0, 0, 1, 1); rec_vbuf(Ve, 96, 2); rec(OP_DRAW, &dr3, sizeof dr3); submit();
            uint32_t cv1 = texel(A, 2, 2);
            rec_rt(A, NULL); rec_clear(0, 0, 1, 1); rec_vbuf(dp + n - 64, 64, 2); rec(OP_DRAW, &dr3, sizeof dr3); submit();
            uint32_t cv2 = texel(A, 2, 2);
            CHECK(cv1 == red && cv2 == blue && dk(DK_RES) == rs0 + 1 && atomic_load(&g_dr_n) == dn0 + 1, "a draw of 3 vertices from a stream with 96 B left in its pool is drawn (%#x), with 64 B left skipped (%#x; %llu skipped, %llu recorded)",
                  cv1, cv2, (unsigned long long)(dk(DK_RES) - rs0), (unsigned long long)(atomic_load(&g_dr_n) - dn0));
            // ---- Review round 2 of slice 2c ----
            // A pixel shader with interpolateAtCentroid (capability InterpolationFunction): Vulkan takes it only with sampleRateShading (VUID-VkShaderModuleCreateInfo-pCode-08740), which vk_dev.c
            // enables where the device has it: there the module is made and draws (one sample: the centroid is the centre), elsewhere it is refused; validation silent either way
            mtl_finish(); vq0 = vkn_validation_count(); vkn_shader_stats(&ld0, &rf0);
            const uint8_t *kpi = t_key(0x7e57c0de0000000aull);
            vkn_shader *spi = t_mod(kpi, 1, "ok", t_ps_interp_frag_spv, sizeof t_ps_interp_frag_spv, "", OUT1);
            uint32_t cpi = 0;
            if (spi) { prog_t *PI = t_prog(kv, kpi); rec_rt(A, NULL); rec_clear(0, 0, 1, 1); rec_prog(PI); rec_vbuf(V, 1024, 2); rec_vp(64, 64); rec_sc(64, 64); rec_idx(I, 3, 0); submit(); cpi = texel(A, 2, 2); }
            mtl_finish(); vq1 = vkn_validation_count(); vkn_shader_stats(&ld1, &rf1);
            CHECK(vq1 == vq0 && (vd.sample_rate ? spi && cpi == red && ld1 - ld0 == 1 : !spi && rf1 - rf0 == 1), "a module declaring InterpolationFunction, sampleRateShading %s: %s (%#x); %llu validation messages", vd.sample_rate ? "on" : "absent",
                  spi ? "made and drawn" : "refused", cpi, (unsigned long long)(vq1 - vq0));
            // Alpha to coverage on, with a pixel shader writing colour 1 only: Vulkan's coverage takes location 0's alpha (VUID-VkGraphicsPipelineCreateInfo-alphaToCoverageEnable-08891), so that
            // pipeline has it off (said in red) and its draw is recorded (two colour targets: Bt, target 1, gets the colour; A, target 0, is not written); with t_ps (location 0, alpha 1) it stays
            // on, a pipeline of its own, all covered
            static const char OUT_1[] = "\"outputs\": [{\"location\": 1, \"type\": \"float\", \"components\": 4}], \"inputs\": [{\"location\": 0, \"type\": \"float\", \"components\": 4}]";
            const uint8_t *kpo = t_key(0x7e57c0de0000000bull);
            vkn_shader *spo = t_mod(kpo, 1, "ok", t_ps_o1_frag_spv, sizeof t_ps_o1_frag_spv, "", OUT_1);
            prog_t *PO = t_prog(kv, kpo);
            const ms_st ma = {1, 0, 1, 0, 0, 0, 0, 0, 0, 0}, mo = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            mtl_finish(); vq0 = vkn_validation_count(); xi0 = atomic_load(&g_dr_idx); uint64_t pa0 = atomic_load(&g_pso_made);
            { p_set_rt r2; memset(&r2, 0, sizeof r2); r2.n = 2; r2.color[0] = A; r2.color[1] = Bt; rec(OP_SET_RT, &r2, sizeof r2); }
            { p_clear_color c0 = {0, 0xf, {0, 0, 1, 1}}, c1 = {1, 0xf, {0, 0, 1, 1}}; rec(OP_CLEAR_COLOR, &c0, sizeof c0); rec(OP_CLEAR_COLOR, &c1, sizeof c1); }
            rec_prog(PO); rec_vbuf(V, 1024, 2); rec_vp(64, 64); rec_sc(64, 64); rec_state(3, &ma, sizeof ma); rec_idx(I, 3, 0); submit();
            uint32_t ca1 = texel(A, 2, 2), ct1 = texel(Bt, 2, 2);
            rec_rt(A, NULL); rec_clear(0, 0, 1, 1); rec_prog(P); rec_idx(I, 3, 0); rec_state(3, &mo, sizeof mo); submit();
            uint32_t ca2 = texel(A, 2, 2); mtl_finish(); vq1 = vkn_validation_count();
            CHECK(spo && atomic_load(&g_dr_idx) == xi0 + 2 && atomic_load(&g_pso_made) == pa0 + 2 && vq1 == vq0 && ca1 == blue && ct1 == red && ca2 == red,
                  "alpha to coverage with a pixel shader writing colour 1 only: off, drawn (target 1 %#x, target 0 not written %#x); with one writing colour 0: on, drawn (%#x); %llu recorded, %llu pipelines, "
                  "%llu validation messages", ct1, ca1, ca2, (unsigned long long)(atomic_load(&g_dr_idx) - xi0), (unsigned long long)(atomic_load(&g_pso_made) - pa0), (unsigned long long)(vq1 - vq0));
            // A pixel shader reading location 2, which the vertex shader does not write (VUID-RuntimeSpirv-OpEntryPoint-08743): the pair is refused with its layout (said in red), its draws skipped
            static const char IN02[] = "\"outputs\": [{\"location\": 0, \"type\": \"float\", \"components\": 4}], \"inputs\": [{\"location\": 0, \"type\": \"float\", \"components\": 4}, "
                                       "{\"location\": 2, \"type\": \"float\", \"components\": 4}]";
            const uint8_t *kp2 = t_key(0x7e57c0de0000000cull);
            vkn_shader *sp2 = t_mod(kp2, 1, "ok", t_ps_in2_frag_spv, sizeof t_ps_in2_frag_spv, "", IN02);
            prog_t *P2 = t_prog(kv, kp2);
            mtl_finish(); vq0 = vkn_validation_count(); s0 = dk(DK_SHADER);
            rec_rt(A, NULL); rec_clear(0, 0, 1, 1); rec_prog(P2); rec_vbuf(V, 1024, 2); rec_vp(64, 64); rec_sc(64, 64); rec_idx(I, 3, 0); submit();
            uint32_t c2p = texel(A, 2, 2); mtl_finish(); vq1 = vkn_validation_count();
            CHECK(sp2 && dk(DK_SHADER) == s0 + 1 && c2p == blue && vq1 == vq0, "a pixel shader reading a location its vertex shader does not write: the pair is skipped (%llu), the target as cleared (%#x); %llu validation messages",
                  (unsigned long long)(dk(DK_SHADER) - s0), c2p, (unsigned long long)(vq1 - vq0));
            // A vertex shader's SPIR-V under a pixel shader's reflection (its size right, no resources either side): the entry point is a vertex one, so it is refused
            vkn_shader_stats(&ld0, &rf0);
            const uint8_t *kx = t_key(0x7e57c0de0000000dull);
            vkn_shader *sx = t_mod(kx, 1, "ok", t_vs_vert_spv, sizeof t_vs_vert_spv, "", OUT1);
            vkn_shader_stats(&ld1, &rf1);
            CHECK(!sx && rf1 - rf0 == 1 && ld1 == ld0, "a vertex shader's SPIR-V under a pixel shader's reflection is refused (its entry point's execution model)");
            // t_ps's SPIR-V with its generator word changed (still valid SPIR-V, the same size) under t_ps's reflection, its spv_sha256 that of the original: refused (the SHA-256 differs)
            uint32_t *pw = malloc(sizeof t_ps_frag_spv); vkn_shader *sw = NULL;
            if (pw) { memcpy(pw, t_ps_frag_spv, sizeof t_ps_frag_spv); pw[2] ^= 1; vkn_shader_stats(&ld0, &rf0); sw = t_mod_sha(t_key(0x7e57c0de0000000eull), 1, "ok", pw, sizeof t_ps_frag_spv, "", OUT1, t_ps_frag_spv); vkn_shader_stats(&ld1, &rf1); }
            CHECK(pw && !sw && rf1 - rf0 == 1 && ld1 == ld0, "a module of the same size and stage as its reflection's, other bytes, is refused (its SHA-256)");
            free(pw);
            // Reflection numbers: a texture slot or a sampler slot of 65536 (cut to 16 bits it was slot 0, and taken), NaN, 1e300 and 1.5 are refused; a sampler array (sampler2D t[4]) is
            // refused whether the reflection's count says 1 (the patcher finds the array type) or null; a reflection that breaks off inside its status string is refused as unreadable
            static const char *const regs[][2] = {{"65536", "0"}, {"0", "65536"}, {"nan", "0"}, {"1e300", "0"}, {"1.5", "0"}, {"0", "-1"}};
            int nrej = 0; char rb[1200];
            vkn_shader_stats(&ld0, &rf0);
            for (int i = 0; i < 6; i++) {
                snprintf(rb, sizeof rb, "{\"name\": \"t0\", \"kind\": \"tex\", \"type_name\": \"COMBINED_IMAGE_SAMPLER\", \"set\": 1, \"binding\": 1, \"count\": 1, \"reg\": %s, \"sampler\": {\"kind\": \"samp_tex\", \"reg\": %s}, "
                         "\"image\": {\"dim\": \"2D\", \"arrayed\": 0, \"ms\": 0, \"depth\": 0, \"comp\": \"float\"}}, "
                         "{\"name\": \"t1\", \"kind\": \"tex\", \"type_name\": \"COMBINED_IMAGE_SAMPLER\", \"set\": 1, \"binding\": 2, \"count\": 1, \"reg\": 1, \"sampler\": {\"kind\": \"samp_tex\", \"reg\": 1}, "
                         "\"image\": {\"dim\": \"2D\", \"arrayed\": 0, \"ms\": 0, \"depth\": 0, \"comp\": \"float\"}}", regs[i][0], regs[i][1]);
                vkn_shader *sr = t_mod(t_key(0x7e57c0de00000010ull + (uint64_t)i), 1, "ok", t_ps_tex_frag_spv, sizeof t_ps_tex_frag_spv, rb, OUT1);
                if (!sr) nrej++; else fprintf(stderr, "t_vkn: texture slot %s, sampler slot %s taken\n", regs[i][0], regs[i][1]);
            }
            vkn_shader_stats(&ld1, &rf1);
            CHECK(nrej == 6 && rf1 - rf0 == 6 && ld1 == ld0, "slots of 65536, NaN, 1e300, 1.5 and -1 are refused (%d of 6)", nrej);
            static const char ARR[] = "{\"name\": \"t\", \"kind\": \"tex\", \"type_name\": \"COMBINED_IMAGE_SAMPLER\", \"set\": 1, \"binding\": 1, \"count\": %s, \"reg\": 0, \"sampler\": {\"kind\": \"samp_tex\", \"reg\": 0}, "
                                      "\"image\": {\"dim\": \"2D\", \"arrayed\": 0, \"ms\": 0, \"depth\": 0, \"comp\": \"float\"}}";
            vkn_shader_stats(&ld0, &rf0);
            snprintf(rb, sizeof rb, ARR, "1"); vkn_shader *sa1 = t_mod(t_key(0x7e57c0de00000018ull), 1, "ok", t_ps_arr_frag_spv, sizeof t_ps_arr_frag_spv, rb, OUT1);
            snprintf(rb, sizeof rb, ARR, "null"); vkn_shader *san = t_mod(t_key(0x7e57c0de00000019ull), 1, "ok", t_ps_arr_frag_spv, sizeof t_ps_arr_frag_spv, rb, OUT1);
            const uint8_t *kt = t_key(0x7e57c0de0000001aull); char hk[17], tj[256]; for (int i = 0; i < 8; i++) snprintf(hk + 2 * i, 3, "%02x", kt[i]);
            snprintf(tj, sizeof tj, "{\"key\": \"%s_b\", \"dm_ctl\": \"%s\", \"chain\": \"%s\", \"status\": \"o", hk, hk, VKN_CHAIN);
            vkn_shader *stt = vkn_shader_add(kt, 0, tj, t_vs_vert_spv, sizeof t_vs_vert_spv);
            vkn_shader_stats(&ld1, &rf1);
            CHECK(!sa1 && !san && !stt && rf1 - rf0 == 3 && ld1 == ld0, "a sampler array is refused with count 1 (%s) and with count null (%s); a reflection cut inside its status is refused (%s)", sa1 ? "taken" : "refused",
                  san ? "taken" : "refused", stt ? "taken" : "refused");
            // Polygon mode LINE with only the line offset enabled, on a device without fillModeNonSolid (forced off here; KosmicKrisp has none): drawn filled, with the line offset (NVN's mode
            // picks the enable, not the fill drawn instead): z 0 (depth 0.5) moved by 2^20 units; with no offset enabled it stays 0.5
            int fns = vd.fill_ns; vd.fill_ns = 0;
            const poly_st pln = {NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_LINE, NVN_POLYGON_OFFSET_ENABLE_LINE}, pl0 = {NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_LINE, 0};
            const p_poly_offset po = {0, 1048576.f, 0}, po0 = {0, 0, 0};
            rec_rt(A, Dz); rec_clear(0, 0, 0, 1); rec_clear_d(1.f); rec_state(5, &dson, sizeof dson); rec_prog(P); rec_vbuf(V, 1024, 2); rec_vp(64, 64); rec_sc(64, 64); rec(OP_POLY_OFFSET, &po, sizeof po);
            rec_state(4, &pln, sizeof pln); rec_idx(I, 3, 8); submit();
            float dbl = depth_at(Dz, 2, 2);
            rec_rt(A, Dz); rec_clear_d(1.f); rec_state(4, &pl0, sizeof pl0); rec_idx(I, 3, 8); submit();
            float db0 = depth_at(Dz, 2, 2);
            rec(OP_POLY_OFFSET, &po0, sizeof po0); rec_poly(NVN_FACE_NONE, NVN_FRONT_FACE_CCW); rec_state(5, &dsoff, sizeof dsoff); submit();
            vd.fill_ns = fns;
            CHECK(dbl > 0.5f + 1e-3f && dbl < 1.f && db0 == 0.5f, "polygon mode LINE drawn filled (no fillModeNonSolid) takes the line offset enable: depth %g with it (want above 0.5), %g without (want 0.5)", dbl, db0);
            // ---- Review round 3 of slice 2c ----
            // Signed integers, as the reflection names them (sint; the reflections below are dmsh_build_vk.py's own of these modules): a vertex shader with a sint x4 input nothing feeds (the zero
            // buffer, as R32G32B32A32_SINT) and a flat sint x4 output at location 1; a pixel shader reading it, with an isampler2D at combined slot 0 that nothing is bound to (the signed dummy).
            // Both are made, and the pair draws the corner yellow (red from the colour; green where the input is as the vertex shader wrote it and the texel zero); validation silent
            static const char VINI[] = "\"vertex_inputs\": [{\"location\": 0, \"type\": \"float\", \"components\": 4}, {\"location\": 1, \"type\": \"float\", \"components\": 4}, "
                                       "{\"location\": 2, \"type\": \"sint\", \"components\": 4}], \"outputs\": [{\"location\": 0, \"type\": \"float\", \"components\": 4}, "
                                       "{\"location\": 1, \"type\": \"sint\", \"components\": 4, \"interp\": [\"flat\"]}]",
                              OUTI[] = "\"outputs\": [{\"location\": 0, \"type\": \"float\", \"components\": 4}], \"inputs\": [{\"location\": 0, \"type\": \"float\", \"components\": 4}, "
                                       "{\"location\": 1, \"type\": \"sint\", \"components\": 4, \"interp\": [\"flat\"]}]",
                              ITEX[] = "{\"name\": \"t0\", \"kind\": \"tex\", \"type_name\": \"COMBINED_IMAGE_SAMPLER\", \"set\": 1, \"binding\": 1, \"count\": 1, \"reg\": 0, \"sampler\": {\"kind\": \"samp_tex\", \"reg\": 0}, "
                                       "\"image\": {\"dim\": \"2D\", \"arrayed\": 0, \"ms\": 0, \"depth\": 0, \"comp\": \"sint\"}}";
            mtl_finish(); vq0 = vkn_validation_count(); vkn_shader_stats(&ld0, &rf0);
            const uint8_t *kvi = t_key(0x7e57c0de0000001bull), *kpn = t_key(0x7e57c0de0000001cull);
            vkn_shader *svi = t_mod(kvi, 0, "ok", t_vs_int_vert_spv, sizeof t_vs_int_vert_spv, "", VINI), *spn = t_mod(kpn, 1, "ok", t_ps_int_frag_spv, sizeof t_ps_int_frag_spv, ITEX, OUTI);
            uint32_t cin = 0; uint64_t ddi = atomic_load(&g_ddum), dri = atomic_load(&g_dr_idx);
            if (svi && spn) { prog_t *PN = t_prog(kvi, kpn); rec_rt(A, NULL); rec_clear(0, 0, 1, 1); rec_prog(PN); rec_vbuf(V, 1024, 2); rec_vp(64, 64); rec_sc(64, 64); rec_tex(1, 0, 0, 41); rec_idx(I, 3, 0); submit(); cin = texel(A, 2, 2); }
            mtl_finish(); vq1 = vkn_validation_count(); vkn_shader_stats(&ld1, &rf1);
            CHECK(svi && spn && svi->nin == 3 && svi->in[2].ct == 2 && svi->io_ct[1] == 2 && spn->io_ct[1] == 2 && spn->b[0].comp == 2 && cin == rgba(1, 1, 0, 1) && atomic_load(&g_ddum) == ddi + 1 &&
                  atomic_load(&g_dr_idx) == dri + 1 && ld1 - ld0 == 2 && rf1 == rf0 && vq1 == vq0,
                  "sint: a vertex input nothing feeds, a flat varying and an isampler2D (the signed dummy) are taken (%s, %s) and drawn (%#x, want yellow; %llu dummies); %llu validation messages",
                  svi ? "vertex made" : "vertex refused", spn ? "pixel made" : "pixel refused", cin, (unsigned long long)(atomic_load(&g_ddum) - ddi), (unsigned long long)(vq1 - vq0));
            // Interface variables, not locations (as dmsh_build_vk.py pair_inputs): a pixel shader reading vec4 v[2] at location 0 draws with a vertex shader writing vec4 v[2] there (the colour
            // and zero: red); with one writing two vec4s at locations 0 and 1 the pair is refused (said in red, its draws skipped, the target as cleared), as is the vertex shader's vec4[2] with
            // t_ps reading one vec4 at location 0. KosmicKrisp's validation says nothing of either mismatch: the skips are the evidence
            static const char AR0[] = "{\"location\": 0, \"type\": \"float\", \"components\": 4, \"array\": {\"location\": 0, \"dims\": [2]}}",
                              AR1[] = "{\"location\": 1, \"type\": \"float\", \"components\": 4, \"array\": {\"location\": 0, \"dims\": [2]}}",
                              VI2[] = "\"vertex_inputs\": [{\"location\": 0, \"type\": \"float\", \"components\": 4}, {\"location\": 1, \"type\": \"float\", \"components\": 4}]",
                              O1[] = "\"outputs\": [{\"location\": 0, \"type\": \"float\", \"components\": 4}]";
            char iov[400], ioo[400], iop[400];
            snprintf(iov, sizeof iov, "%s, \"outputs\": [%s, %s]", VI2, AR0, AR1);
            snprintf(ioo, sizeof ioo, "%s, \"outputs\": [{\"location\": 0, \"type\": \"float\", \"components\": 4}, {\"location\": 1, \"type\": \"float\", \"components\": 4}]", VI2);
            snprintf(iop, sizeof iop, "%s, \"inputs\": [%s, %s]", O1, AR0, AR1);
            const uint8_t *kva = t_key(0x7e57c0de0000001dull), *kvo = t_key(0x7e57c0de0000001eull), *kpa = t_key(0x7e57c0de0000001full);
            vkn_shader *sva = t_mod(kva, 0, "ok", t_vs_va_vert_spv, sizeof t_vs_va_vert_spv, "", iov), *svo = t_mod(kvo, 0, "ok", t_vs_o2_vert_spv, sizeof t_vs_o2_vert_spv, "", ioo),
                       *spa = t_mod(kpa, 1, "ok", t_ps_va_frag_spv, sizeof t_ps_va_frag_spv, "", iop);
            mtl_finish(); vq0 = vkn_validation_count(); s0 = dk(DK_SHADER);
            uint32_t cva[3] = {0, 0, 0};
            if (sva && svo && spa) {
                prog_t *PV[3] = {t_prog(kva, kpa), t_prog(kvo, kpa), t_prog(kva, kp)};
                for (int i = 0; i < 3; i++) { rec_rt(A, NULL); rec_clear(0, 0, 1, 1); rec_prog(PV[i]); rec_vbuf(V, 1024, 2); rec_vp(64, 64); rec_sc(64, 64); rec_idx(I, 3, 0); submit(); cva[i] = texel(A, 2, 2); }
            }
            mtl_finish(); vq1 = vkn_validation_count();
            CHECK(sva && svo && spa && sva->io_nd[1] == 1 && sva->io_first[1] == 0 && cva[0] == red && cva[1] == blue && cva[2] == blue && dk(DK_SHADER) == s0 + 2 && vq1 == vq0,
                  "a vec4[2] read is drawn with a vec4[2] written (%#x), refused with two vec4s written (%#x) and a vec4[2] written refused with one vec4 read (%#x): %llu skipped; %llu validation messages",
                  cva[0], cva[1], cva[2], (unsigned long long)(dk(DK_SHADER) - s0), (unsigned long long)(vq1 - vq0));
            // An array the reflection does not describe, on location 1 of that pixel shader: no lengths, five, a length of 0, of 33 or of 1.5, a first location past it, a location past the
            // array's (dims [1] from 0), no dims and an array that is no object, each refused
            static const char *const badar[] = {"{\"location\": 0, \"dims\": []}", "{\"location\": 0, \"dims\": [2, 1, 1, 1, 1]}", "{\"location\": 0, \"dims\": [0]}", "{\"location\": 0, \"dims\": [33]}",
                                                "{\"location\": 0, \"dims\": [1.5]}", "{\"location\": 2, \"dims\": [2]}", "{\"location\": 0, \"dims\": [1]}", "{\"location\": 0}", "3"};
            nrej = 0; vkn_shader_stats(&ld0, &rf0);
            for (int i = 0; i < 9; i++) {
                snprintf(rb, sizeof rb, "%s, \"inputs\": [%s, {\"location\": 1, \"type\": \"float\", \"components\": 4, \"array\": %s}]", O1, AR0, badar[i]);
                if (!t_mod(t_key(0x7e57c0de00000020ull + (uint64_t)i), 1, "ok", t_ps_va_frag_spv, sizeof t_ps_va_frag_spv, "", rb)) nrej++; else fprintf(stderr, "t_vkn: array %s taken\n", badar[i]);
            }
            vkn_shader_stats(&ld1, &rf1);
            CHECK(nrej == 9 && rf1 - rf0 == 9 && ld1 == ld0, "arrays the reflection does not describe are refused (%d of 9)", nrej);
            // Alpha to coverage on, with a pixel shader whose one output is at location 0, Index 1 (its reflection, the builder's, lists location 0's four components as for Index 0): Vulkan's
            // coverage takes an Index 0 output (VUID-VkGraphicsPipelineCreateInfo-alphaToCoverageEnable-08891), so the patcher notes the Index (dual) and that pipeline has it off (said in red);
            // the draw is recorded, validation silent. Colour 0 gets no Index 0 output, so the target is not read
            const uint8_t *kpx = t_key(0x7e57c0de00000029ull);
            vkn_shader *spx = t_mod(kpx, 1, "ok", t_ps_idx_frag_spv, sizeof t_ps_idx_frag_spv, "", OUT1);
            mtl_finish(); vq0 = vkn_validation_count(); xi0 = atomic_load(&g_dr_idx);
            if (spx) { prog_t *PX = t_prog(kv, kpx); rec_rt(A, NULL); rec_clear(0, 0, 1, 1); rec_prog(PX); rec_vbuf(V, 1024, 2); rec_vp(64, 64); rec_sc(64, 64); rec_state(3, &ma, sizeof ma); rec_idx(I, 3, 0);
                       rec_state(3, &mo, sizeof mo); submit(); }
            mtl_finish(); vq1 = vkn_validation_count();
            uint32_t cpx = spx ? texel(A, 2, 2) : 0;
            CHECK(spx && spx->dual && !sp->dual && atomic_load(&g_dr_idx) == xi0 + 1 && vq1 == vq0, "alpha to coverage with a pixel shader whose location 0 is Index 1: noted (%s), off, drawn (%llu); "
                  "%llu validation messages", spx && spx->dual ? "dual" : "not dual", (unsigned long long)(atomic_load(&g_dr_idx) - xi0), (unsigned long long)(vq1 - vq0));
            // ... and location 0, which only that Index 1 output covers, is not written: the target keeps its clear (Vulkan leaves colour 0 undefined with no Index 0 output)
            CHECK(spx && !(spx->out_mask & 1) && cpx == blue, "a location only an Index 1 output covers is not written (out_mask %#x, the target %#x, its clear %#x)", spx ? spx->out_mask : 0, cpx, blue);
            // A screenshot of the drawn corner (vk_present.c shot_one): NDC's lower left is the file's lower left with either origin, and nothing is elsewhere
            {
                tex_t *win = tex(64, 48, NVN_FORMAT_RGBA8);
                const char *td = getenv("TMPDIR"); char dir[1024], path[1100]; snprintf(dir, sizeof dir, "%s/t_vkn_shot2.XXXXXX", td && *td ? td : "/tmp");
                int have = mkdtemp(dir) != NULL; snprintf(path, sizeof path, "%s/f%%d.png", dir);
                CHECK(have, "a temporary directory for the screenshots: %s", strerror(errno));
                for (int ll = 1; have && ll >= 0; ll--) {
                    mtl_set_origin_lower_left(ll);
                    rec_rt(win, NULL); rec_clear(0, 0, 1, 1); rec_prog(P); rec_vbuf(V, 1024, 2); rec_vp(64, 48); rec_sc(64, 48); rec_idx(I, 3, 0); submit();
                    uint64_t pn = atomic_load(&g_present_n) + 1; char file[1200]; snprintf(file, sizeof file, "%s/f%llu.png", dir, (unsigned long long)pn);
                    uint32_t w, h; uint8_t *rgb = NULL; int got = shoot(win, path) && png_read_stored(file, &w, &h, &rgb) && w == 64 && h == 48, qn[4] = {0};
                    for (uint32_t y = 0; got && y < 48; y++) for (uint32_t x = 0; x < 64; x++) qn[(x >= 32) | (y >= 24) << 1] += px_is(rgb, w, x, y, 255, 0, 0);
                    CHECK(got && qn[2] >= 340 && qn[2] <= 430 && !qn[0] && !qn[1] && !qn[3] && px_is(rgb, w, 0, 47, 255, 0, 0) && px_is(rgb, w, 0, 0, 0, 0, 255),
                          "%s origin: the screenshot has the corner triangle at its lower left (%d red pixels there; %d %d %d in the other quarters)", ll ? "lower-left" : "upper-left", qn[2], qn[0], qn[1], qn[3]);
                    free(rgb); unlink(file);
                }
                mtl_set_origin_lower_left(1);
                if (have) rmdir(dir);
            }
            free(apx);
        }
        // Back to no program and NVN's initial state, as the checks above found it
        rec_prog(NULL); rec_swz(0); rec_poly(NVN_FACE_NONE, NVN_FRONT_FACE_CCW); submit();
        if (dp) { pool_fin(dp); munmap(dp, n); }
    }

    mtl_finish();
    uint64_t vm = vkn_validation_count();
    CHECK(!vd.validation || vm == 0, "validation messages: %llu", (unsigned long long)vm);
    fprintf(stderr, "t_vkn: %d checks, %d failed (validation %s, %llu messages; %s)\n", checks, failed, vd.validation ? "on" : "off", (unsigned long long)vm, strict ? "strict" : "not strict");
    return failed != 0;
}
