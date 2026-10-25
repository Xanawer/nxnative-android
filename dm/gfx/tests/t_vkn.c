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
// - no validation message. Each target is read back with a copy to a host-visible buffer.
// T_VKN_STRICT=1 (make check_t_vkn) never skips: no device, validation off or any failure to start is a failure. Without it (make run) a machine with no Vulkan at all (no loader, or a
// loader with no driver: vd.absent) is said to be SKIPPED and passes; a Vulkan that is there but fails to start fails all the same, and validation off is said (its check passes unchecked)
// nvn_dirty_now reaches the backend through t_dirty_now (below), so a check can be staged in the window between nvn_dirty.c's generation increment and the page's generation store
#define nvn_dirty_now t_dirty_now
#include "../vkn/nvn_vk.c"
#undef nvn_dirty_now
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>

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
static uint8_t g_rec[4096]; static size_t g_rn;
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

    mtl_finish();
    uint64_t vm = vkn_validation_count();
    CHECK(!vd.validation || vm == 0, "validation messages: %llu", (unsigned long long)vm);
    fprintf(stderr, "t_vkn: %d checks, %d failed (validation %s, %llu messages; %s)\n", checks, failed, vd.validation ? "on" : "off", (unsigned long long)vm, strict ? "strict" : "not strict");
    return failed != 0;
}
