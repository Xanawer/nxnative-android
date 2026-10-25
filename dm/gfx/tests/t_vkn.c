// The Vulkan backend's replay (dm/gfx/vkn, #included) on a real device with no window (make check_t_vkn: KosmicKrisp and the validation layer, as dm/android/phase2's Mac runs):
// - submissions that end with a recorded clear, or with a pass still open: mtl_sub_commit lands them through the submission's command buffer (it used to clear g_cur first and crash in cb_now);
// - a full clear inside a pass narrower than its target (a 64x64 colour target with 128x128 depth, and the other way round): NVN clears the whole target, a vkCmdClearAttachments in the pass
//   reaches its area only, so the pass ends and the clear goes to the next pass's start (op_clear_* oversize_mid);
// - no validation message. Each target is read back with a copy to a host-visible buffer.
// T_VKN_STRICT=1 (make check_t_vkn) never skips: no device, validation off or any failure to start is a failure. Without it (make run) a machine with no Vulkan at all (no loader, or a
// loader with no driver: vd.absent) is said to be SKIPPED and passes; a Vulkan that is there but fails to start fails all the same, and validation off is said (its check passes unchecked)
#include "../vkn/nvn_vk.c"
#include <stdarg.h>

// What the backend needs of the host and of hle_nvn.c
void dm_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); }
int nvn_game_fps(void) { return 60; }
uint64_t guest_ns(void) { return 0; }
void dm_guest_write_prepare(void *p, size_t n) { (void)p; (void)n; }
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
#define T_FN(X) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkResetCommandBuffer) X(vkCmdCopyImageToBuffer) X(vkQueueSubmit)
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
static uint32_t rgba(float r, float g, float b, float a) { return (uint32_t)(r * 255.f + .5f) | (uint32_t)(g * 255.f + .5f) << 8 | (uint32_t)(b * 255.f + .5f) << 16 | (uint32_t)(a * 255.f + .5f) << 24; }
static float depth_at(const tex_t *t, int x, int y) { uint32_t v = texel(t, x, y); float f; memcpy(&f, &v, 4); return f; }

int main(void) {
    const char *st = getenv("T_VKN_STRICT"); int strict = st && atoi(st) > 0;
    mtl_init();
    if (!g_vk && vd.absent && !strict) { fprintf(stderr, "t_vkn: SKIPPED: no Vulkan loader or driver on this machine, nothing was checked (make check_t_vkn runs it on KosmicKrisp, strict)\n"); return 0; }
    if (!g_vk) { fprintf(stderr, "t_vkn: FAIL: the Vulkan backend did not start (%s; the lines above say why)\n", vd.absent ? "no Vulkan loader or driver, and T_VKN_STRICT=1 does not skip" : "Vulkan is there"); return 1; }
    if (!rb_init()) { fprintf(stderr, "t_vkn: FAIL: readback objects could not be made\n"); return 1; }
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

    mtl_finish();
    uint64_t vm = vkn_validation_count();
    CHECK(!vd.validation || vm == 0, "validation messages: %llu", (unsigned long long)vm);
    fprintf(stderr, "t_vkn: %d checks, %d failed (validation %s, %llu messages; %s)\n", checks, failed, vd.validation ? "on" : "off", (unsigned long long)vm, strict ? "strict" : "not strict");
    return failed != 0;
}
