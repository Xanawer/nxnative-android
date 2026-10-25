// The Vulkan backend's replay (dm/gfx/vkn, #included) on a real device with no window (make check_t_vkn: KosmicKrisp and the validation layer, as dm/android/phase2's Mac runs):
// - submissions that end with a recorded clear, or with a pass still open: mtl_sub_commit lands them through the submission's command buffer (it used to clear g_cur first and crash in cb_now);
// - a full clear inside a pass narrower than its target (a 64x64 colour target with 128x128 depth, and the other way round): NVN clears the whole target, a vkCmdClearAttachments in the pass
//   reaches its area only, so the pass ends and the clear goes to the next pass's start (op_clear_* oversize_mid);
// - a DM_NVN_SHOT screenshot (vk_present.c shot_one) is the picture the right way up: NVN's lower-left origin stores the window image bottom-up and the file is written top row first;
// - strategy B (PLAN.md 2.2 "As built, slice 2a"): bytes written into a CPU-visible pool reach its shadow buffer (read back on the GPU), a rewrite uploads only the pages written, and a pool deleted while
//   a submission that read it is in flight (held back on the GPU by a semaphore the test signals) has its pages writable at once and its shadow destroyed only after that submission completes.
//   Two pools sharing a host page (16 KB pages; skipped at 4 KB, where 4 KB-aligned pools share none): an address there resolves to the pool whose bytes hold it, the page is protected by
//   neither and both shadows get it at every submission. The real write tracking is linked in (nvn_dirty.c, and host/crash.c for its fault handler), as in the host;
// - no validation message. Each target is read back with a copy to a host-visible buffer.
// T_VKN_STRICT=1 (make check_t_vkn) never skips: no device, validation off or any failure to start is a failure. Without it (make run) a machine with no Vulkan at all (no loader, or a
// loader with no driver: vd.absent) is said to be SKIPPED and passes; a Vulkan that is there but fails to start fails all the same, and validation off is said (its check passes unchecked)
#include "../vkn/nvn_vk.c"
#include <fcntl.h>
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

    mtl_finish();
    uint64_t vm = vkn_validation_count();
    CHECK(!vd.validation || vm == 0, "validation messages: %llu", (unsigned long long)vm);
    fprintf(stderr, "t_vkn: %d checks, %d failed (validation %s, %llu messages; %s)\n", checks, failed, vd.validation ? "on" : "off", (unsigned long long)vm, strict ? "strict" : "not strict");
    return failed != 0;
}
