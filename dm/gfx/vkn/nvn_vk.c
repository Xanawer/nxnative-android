// Vulkan GPU backend, first slice (docs/android/PLAN.md Phase 2): the mtl_* set hle_nvn.c calls, begun as a copy of the null backend (dm/gfx/null/nvn_null.c), whose symbol list it keeps.
// Real here: the device (vk_dev.c), NVN render targets as VkImages in plain device memory (PLAN.md 2.2: render-target pools have no shadow), SET_RT, colour and depth-stencil clears (folded
// into the dynamic-rendering load ops when they come before the pass's first draw, as nvn_mtl.m's rp_begin_ex_ does; vkCmdClearAttachments after it), barriers, submission with one timeline
// value per submit and a completion thread that performs NVN's completion effects (event values, counter reports, then the done fence: nvn_mtl.m's completion handlers), and the present of the
// window texture (vk_present.c). As the null backend: draws, binds and texture uploads are counted no-ops, buffer copies / fills / uniform updates run on the CPU at replay, texture reads come
// from guest memory. The ordering convention of nvn_mtl.h holds: a submission's event values and counter records are written before its fence counts as done, and waits look only at that.
// No Vulkan device (no library, no 1.3 driver): everything runs as the null backend does, said in red
#include "vkn.h"
#include "../nvn/nvn_obj.h"
#include "../nvn/nvn_mtl.h"
#include "../nvn/nvn_tex.h"
#include "../nvn/nvn_dirty.h"
#include "../nvn/nvn_hmap.h"
#include "../tex/nvn_deswizzle.h"
#include "../../host/dm.h"
#include <errno.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define NLOG VLOG
#define NRED_ONCE VRED_ONCE
int nvn_game_fps(void);   // hle_nvn.c
static int g_vk;          // the device is up (mtl_init); 0 = the null backend's behaviour
static _Atomic int g_origin_ll = 1;   // NVN's default window origin, lower left (hle_nvn.c sets it at device init and on SetWindowOriginMode)
int vkn_origin_ll(void) { return atomic_load_explicit(&g_origin_ll, memory_order_relaxed); }

// ---------- Surface statistics: what the game asks of a GPU backend (the Vulkan backend's work list, PLAN.md Phase 2), counted where the records are walked anyway ----------
// Kept from the null backend. Cumulative; one [vkn] line at presents 60, 600 and every 1800, every DM_NVN_NULL_EVERY=<n> presents as well (to tell a run's phases apart: a phase is the difference of two lines),
// and at mtl_drain (graceful exit, or the app sent to the background). Never before present 60 (STATS_FROM, DM_NVN_NULL_EVERY and mtl_drain too): the parity cut ends at the first present
enum { S_DRAW_INST = OP_COUNT_, S_DRAW_BINST, S_DRAW_IND, S_DRAW_MDI, S_DRAW_VTX, S_DIDX_INST, S_DIDX_BVTX, S_DIDX_BINST, S_DIDX_IND, S_DIDX_MDI, S_DIDX_IDX,
       S_PRIM, S_ITYPE = S_PRIM + 16, S_CLR_START = S_ITYPE + 3, S_CLR_MID, S_CLR_PART, S_DS_START, S_DS_MID, S_DS_PART,
       S_B2B_B, S_FILL_B, S_UB_B, S_B2T_B, S_T2B_B, S_T2T_RES, S_DISP_IND, S_CNT_TS, S_CNT_SP, S_CNT_OTH, S_TF_WHAT, S_RC_MODE = S_TF_WHAT + 4, S_BAR = S_RC_MODE + 4,
       S_RT_NC = S_BAR + 8, S_RT_DEPTH = S_RT_NC + 9, S_RT_ZONLY, S_RT_MSAA, S_RTX, S_RTX_B, S_RTX_GPU, S_GW_B, S_VPSW, S_STATE, S_N = S_STATE + 6 };
static _Atomic uint64_t g_st[S_N];
#define ST(i, n) atomic_fetch_add_explicit(&g_st[i], (uint64_t)(n), memory_order_relaxed)
static const char *const g_opname[OP_COUNT_] = {[OP_END] = "END", [OP_JUMP] = "JUMP", [OP_BEGIN] = "BEGIN", [OP_CALL] = "CALL", [OP_SIGNAL_EVENT] = "SIGNAL_EVENT", [OP_REPORT_COUNTER] = "REPORT_COUNTER",
    [OP_COPY_B2B] = "COPY_B2B", [OP_UPDATE_UB] = "UPDATE_UB", [OP_SET_RT] = "SET_RT", [OP_CLEAR_COLOR] = "CLEAR_COLOR", [OP_CLEAR_DS] = "CLEAR_DS", [OP_VIEWPORTS] = "VIEWPORTS", [OP_SCISSOR] = "SCISSOR",
    [OP_DEPTH_RANGE] = "DEPTH_RANGE", [OP_BIND_PROGRAM] = "BIND_PROGRAM", [OP_BIND_STATE] = "BIND_STATE", [OP_BIND_VATTR] = "BIND_VATTR", [OP_BIND_VSTREAM] = "BIND_VSTREAM", [OP_BIND_VBUF] = "BIND_VBUF",
    [OP_BIND_UBO] = "BIND_UBO", [OP_BIND_SSBO] = "BIND_SSBO", [OP_BIND_TEX] = "BIND_TEX", [OP_BIND_SAMP] = "BIND_SAMP", [OP_STENCIL] = "STENCIL", [OP_POLY_OFFSET] = "POLY_OFFSET", [OP_DRAW] = "DRAW",
    [OP_DRAW_IDX] = "DRAW_IDX", [OP_DISPATCH] = "DISPATCH", [OP_BARRIER] = "BARRIER", [OP_SET_TEX_POOL] = "SET_TEX_POOL", [OP_SET_SAMP_POOL] = "SET_SAMP_POOL", [OP_COPY_B2T] = "COPY_B2T",
    [OP_COPY_STRIDE] = "COPY_STRIDE", [OP_BLEND_COLOR] = "BLEND_COLOR", [OP_COPY_T2B] = "COPY_T2B", [OP_BIND_IMG] = "BIND_IMG", [OP_COPY_T2T] = "COPY_T2T", [OP_RESET_COUNTER] = "RESET_COUNTER",
    [OP_CLEAR_TEX] = "CLEAR_TEX", [OP_CLEAR_BUF] = "CLEAR_BUF", [OP_DEPTH_CLAMP] = "DEPTH_CLAMP", [OP_RENDER_COND] = "RENDER_COND", [OP_FENCE_SYNC] = "FENCE_SYNC", [OP_TF_BIND] = "TF_BIND",
    [OP_TF_STATE] = "TF_STATE", [OP_RAST_DISCARD] = "RAST_DISCARD", [OP_VP_SWIZZLE] = "VP_SWIZZLE", [OP_BIND_TEX1] = "BIND_TEX1", [OP_PRIM_RESTART] = "PRIM_RESTART"};
// Formats seen, as NVN enums (all below 256): 0 render targets, 1 registered textures (and their views), 2 vertex attributes; texture targets as bits
static _Atomic uint64_t g_fmt[3][4]; static _Atomic uint32_t g_tgt, g_vpsw;   // g_vpsw: viewport swizzles other than the identity, bit 8 * component + NVNviewportSwizzle
static void fmt_note(int k, unsigned f) { if (f < 256) atomic_fetch_or_explicit(&g_fmt[k][f >> 6], 1ull << (f & 63), memory_order_relaxed); }
// Live texture serials (TextureInitialize until Finalize or re-initialization: mtl_tex_init / mtl_tex_release), each with whether the GPU writes it (render targets, copy and clear destinations:
// a ReadTexels of one is a GPU → CPU readback in a real backend, of any other the CPU's own bytes). Value 1 live, 3 live and written. Records snapshot the texture object (hle_nvn.c tex_snap), so the
// replay meets serials already finalized: those are not added, nothing would take them out again (serials are never reused, nvn_new_serial). A leaf lock (taken under vkn_tmu too)
static nvn_hmap g_tser = {.klen = 8}; static pthread_mutex_t g_tsmu = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t tser_get(uint64_t s) { pthread_mutex_lock(&g_tsmu); uintptr_t v = s ? (uintptr_t)nvn_hm_get(&g_tser, &s) : 0; pthread_mutex_unlock(&g_tsmu); return v; }
static int tex_live(uint64_t s) { return tser_get(s) != 0; }
static void gpu_wrote(const tex_t *t) {
    static uint64_t last; if (!t || !t->serial || t->serial == last) return;   // submit thread only; the same few targets come back every pass
    uint64_t s = t->serial;
    pthread_mutex_lock(&g_tsmu); if (nvn_hm_get(&g_tser, &s)) nvn_hm_put(&g_tser, &s, (void *)3); pthread_mutex_unlock(&g_tsmu);
    last = s;
}
static int gpu_written(uint64_t s) { return (tser_get(s) & 2) != 0; }
static uint64_t region_bytes(const tex_t *t, const NVNcopyRegion *r) { int bw, bh, bpb = t ? nvn_fmt_bpb(t->d.format, &bw, &bh) : 0; return bpb ? (uint64_t)((r->width + bw - 1) / bw) * (uint64_t)((r->height + bh - 1) / bh) * (uint64_t)(r->depth > 1 ? r->depth : 1) * (uint64_t)bpb : 0; }
static _Atomic uint64_t g_present_n;   // presents so far = hle_nvn.c's frame index (init = 0), the frame things below are labelled with
static uint64_t g_every;               // DM_NVN_NULL_EVERY

// ---------- Submission state (below) that mtl_init makes ----------
// Submission slots, used round-robin: each its own command pool, and the completion effects (post ops) of the submission it holds until that completes
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; } post_op;   // kind 1 event, 2 counter report
#define NSUB 4
typedef struct { VkCommandPool pool; VkCommandBuffer cb; uint64_t value; int failed; post_op *post; int npost, postcap; } sub_slot;
static sub_slot g_sub[NSUB];
static sub_slot *g_cur;                    // the submission being replayed (submit thread: hle_nvn.c serializes submissions, g_sub_mu)
static uint64_t g_nbegun, g_ncommit, g_ncomp;   // submissions begun (submit thread), handed to the completion thread and completed (both under g_mu)
static VkSemaphore g_tl;                   // the NVN timeline: submission k signals k (= its fence)
static _Atomic uint64_t g_fence, g_done;   // g_fence: last committed; g_done: completed, effects written (g_mu / g_cv for waiters)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER, g_ccv = PTHREAD_COND_INITIALIZER;
static void *comp_main(void *arg);
static _Atomic uint64_t g_stat_pass, g_stat_fold, g_stat_clr_mid, g_stat_bar, g_stat_own;   // passes begun, clears folded into load ops / made with vkCmdClearAttachments, barriers, oversize clears in a pass of their own
// A submission retired without the GPU's answer (comp_main: a lost GPU whose wait keeps timing out, or a wait that failed other than with DEVICE_LOST): the GPU may still use what it named, so from then on nothing is destroyed (vkn_sweep)
// and no command pool is reset (mtl_sub_begin: every later submission is failed). A leak after a broken GPU, never a use after free
static _Atomic int g_leak;

static void vkn_hello(void) {
    NLOG("Vulkan GPU backend (dm/gfx/vkn): render targets, clears, barriers, submission and present are real; draws, binds and texture uploads are counted only; buffer copies, fills and uniform updates run on the CPU; texture reads come from guest memory");
    const char *e = getenv("DM_NVN_NULL_EVERY"); g_every = e ? strtoull(e, NULL, 10) : 0;   // the null backend's surface lines, every <n> presents too
    if (!vkn_dev_init()) { NLOG("[RED] no Vulkan device: running as the null backend does, nothing is drawn or shown"); return; }
    VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
    if (vk.vkCreateSemaphore(vd.dev, &si, NULL, &g_tl)) { NLOG("[RED] the timeline semaphore cannot be made: running as the null backend does"); return; }
    for (int i = 0; i < NSUB; i++) {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, vd.qfam};
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        if (vk.vkCreateCommandPool(vd.dev, &pi, NULL, &g_sub[i].pool) || (ai.commandPool = g_sub[i].pool, vk.vkAllocateCommandBuffers(vd.dev, &ai, &g_sub[i].cb))) {
            NLOG("[RED] the submission command pools cannot be made: running as the null backend does"); return; }
    }
    pthread_t t; if (pthread_create(&t, NULL, comp_main, NULL)) { NLOG("[RED] the completion thread cannot start: running as the null backend does"); return; }
    pthread_detach(t);
    g_vk = 1;
}
void mtl_init(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, vkn_hello); }

// ---------- Memory pools: the address ranges only (a copy / fill / update outside every pool is refused, where the Metal layer dies: the CPU would write wherever the game pointed) ----------
typedef struct { uintptr_t lo, hi; const void *key; } pool_ent;
static pool_ent *g_pools; static int g_npool, g_cap, g_plast = -1;
static pthread_mutex_t g_pmu = PTHREAD_MUTEX_INITIALIZER;
// Every pool made and freed, with its size and frame (the surface lines list each once); under g_pmu
typedef struct { uintptr_t mem; uint64_t size, born, died, id; uint8_t said; } pool_log;   // died ~0 = live; id: the order made (the number the lines show); said: 1 its creation listed, 2 its end too
static pool_log *g_plog; static int g_nplog, g_plogcap;
static uint64_t g_pmade, g_pmade_b, g_pfreed, g_pfreed_b, g_plive_b, g_ppeak_b;
void mtl_pool_add(const void *mem, uint64_t size) {
    mtl_init();
    uintptr_t pg = (uintptr_t)getpagesize(), lo = (uintptr_t)mem, hi = lo + size;   // expanded to page boundaries as the Metal layer's zero-copy buffers are, so mtl_addr answers the same
    pthread_mutex_lock(&g_pmu);
    if (g_npool == g_cap) { g_cap = g_cap ? g_cap * 2 : 64; g_pools = realloc(g_pools, (size_t)g_cap * sizeof *g_pools); if (!g_pools) die("vkn backend: pool table allocation failed"); }
    g_pools[g_npool++] = (pool_ent){lo & ~(pg - 1), (hi + pg - 1) & ~(pg - 1), mem};
    if (g_nplog == g_plogcap && g_nplog >= 4096) {   // bounded: sparse pools re-add their runs at every remap (vrun_set), so freed entries go, listed or not (the totals keep them)
        int k = 0; for (int i = 0; i < g_nplog; i++) if (g_plog[i].died == ~0ull) g_plog[k++] = g_plog[i];
        g_nplog = k;
    }
    if (g_nplog == g_plogcap) { g_plogcap = g_plogcap ? g_plogcap * 2 : 64; g_plog = realloc(g_plog, (size_t)g_plogcap * sizeof *g_plog); if (!g_plog) die("vkn backend: pool log allocation failed"); }
    g_plog[g_nplog++] = (pool_log){lo, size, atomic_load(&g_present_n), ~0ull, g_pmade, 0};
    g_pmade++; g_pmade_b += size; g_plive_b += size; if (g_plive_b > g_ppeak_b) g_ppeak_b = g_plive_b;
    pthread_mutex_unlock(&g_pmu);
}
void mtl_pool_del(const void *mem) {
    pthread_mutex_lock(&g_pmu);
    for (int i = 0; i < g_npool; i++) if (g_pools[i].key == mem) { g_pools[i] = g_pools[--g_npool]; g_plast = -1; break; }
    for (int i = g_nplog - 1; i >= 0; i--) if (g_plog[i].mem == (uintptr_t)mem && g_plog[i].died == ~0ull) { g_plog[i].died = atomic_load(&g_present_n); g_pfreed++; g_pfreed_b += g_plog[i].size; g_plive_b -= g_plog[i].size; break; }
    pthread_mutex_unlock(&g_pmu);
}
void mtl_alias_drop_pool(const void *pool) { (void)pool; }
// buf: the pool's base address in place of an id<MTLBuffer> (non-null, so callers that only test success behave as with Metal); off from that base
static int in_pool(int k, uint64_t addr, uint64_t len) { return addr >= g_pools[k].lo && addr <= g_pools[k].hi && len <= g_pools[k].hi - addr; }   // no addr + len: a guest-supplied range near 2^64 wraps
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    int ok = 0;
    pthread_mutex_lock(&g_pmu);
    int i = g_plast >= 0 && g_plast < g_npool && in_pool(g_plast, addr, len) ? g_plast : -1;
    for (int k = 0; i < 0 && k < g_npool; k++) if (in_pool(k, addr, len)) i = g_plast = k;
    if (i >= 0) { *buf = (void *)g_pools[i].lo; *off = addr - g_pools[i].lo; ok = 1; }
    pthread_mutex_unlock(&g_pmu);
    return ok;
}
static int cpu_target(uint64_t addr, uint64_t n, const char *what) {   // 1 = [addr, addr+n) is pool memory the CPU may write; the dirty trackers hear of the write first, as for any host write into guest memory
    void *b; uint64_t o;
    if (!mtl_addr(addr, n, &b, &o)) { NLOG("[RED] %s outside every memory pool dst=%#llx n=%#llx: skipped", what, (unsigned long long)addr, (unsigned long long)n); return 0; }
    dm_guest_write_prepare((void *)(uintptr_t)addr, (size_t)n);
    ST(S_GW_B, n);   // a GPU write into pool memory: what a backend would have to bring back if the CPU reads it
    return 1;
}

// ---------- Host images (vkn.h vkn_img), by serial: VkImages in device memory, made when a texture in a CPU-no-access pool is initialized (render-target pools) or first set as a target ----------
pthread_mutex_t vkn_tmu = PTHREAD_MUTEX_INITIALIZER, vkn_qmu = PTHREAD_MUTEX_INITIALIZER;
#define IT_B 1024
static vkn_img *g_it[IT_B], *g_dead_l;   // buckets by serial; released images waiting for their last uses (vkn_sweep)
static uint64_t g_nimg, g_img_b, g_img_made;
// Ghosts: host images of serials already finalized when a record names them (tex_live: hle_nvn.c snapshots the texture object at record time, and Breath of the Wild finalizes its
// transient targets before it submits, nvn_mtl.m's g_ghost). The table would keep them for good (no second release comes), so they live in a small ring instead: the oldest goes to the
// pending-destruction list when a new one comes, and a record replayed again soon still finds its image
#define NGHOST 16
static vkn_img *g_ghost[NGHOST]; static unsigned g_ghost_next; static uint64_t g_ghost_puts;
static vkn_img **it_slot(uint64_t s) { vkn_img **p = &g_it[(s * 0x9e3779b97f4a7c15ull) >> 54]; while (*p && (*p)->serial != s) p = &(*p)->next; return p; }
vkn_img *vkn_img_get(uint64_t s) {
    if (!s) return NULL;
    vkn_img *e = *it_slot(s); if (e || !g_ghost_puts) return e;
    for (int i = 0; i < NGHOST; i++) if (g_ghost[i] && g_ghost[i]->serial == s) return g_ghost[i];
    return NULL;
}
static void img_dead(vkn_img *e) { e->dead = 1; e->next = g_dead_l; g_dead_l = e; }   // destroyed by vkn_sweep once its last uses have completed
static VkFormatFeatureFlags fmt_feat(VkFormat f) { VkFormatProperties p; vk.vkGetPhysicalDeviceFormatProperties(vd.pd, f, &p); return p.optimalTilingFeatures; }
// NVN format → a VkFormat the device can render to (sf: the format the present samples it as, UNORM for an sRGB one; asp: its aspects); 0 = none. Depth falls back to D32_SFLOAT_S8_UINT
// where D24S8 is missing (KosmicKrisp has no D24_UNORM_S8_UINT)
static VkFormat fmt_map(unsigned nf, VkFormat *sf, VkImageAspectFlags *asp) {
    static const struct { uint16_t n; VkFormat f, s; } col[] = {
        {NVN_FORMAT_RGBA8, VK_FORMAT_R8G8B8A8_UNORM, 0}, {NVN_FORMAT_RGBA8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM}, {NVN_FORMAT_RGBX8, VK_FORMAT_R8G8B8A8_UNORM, 0},
        {NVN_FORMAT_BGRA8, VK_FORMAT_B8G8R8A8_UNORM, 0}, {NVN_FORMAT_BGRA8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM}, {NVN_FORMAT_RGB10A2, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 0},
        {NVN_FORMAT_RGBA16F, VK_FORMAT_R16G16B16A16_SFLOAT, 0}, {NVN_FORMAT_RGBA16, VK_FORMAT_R16G16B16A16_UNORM, 0}, {NVN_FORMAT_R11G11B10F, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 0},
        {NVN_FORMAT_RGBA32F, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {NVN_FORMAT_R8, VK_FORMAT_R8_UNORM, 0}, {NVN_FORMAT_RG8, VK_FORMAT_R8G8_UNORM, 0}, {NVN_FORMAT_R16F, VK_FORMAT_R16_SFLOAT, 0},
        {NVN_FORMAT_RG16F, VK_FORMAT_R16G16_SFLOAT, 0}, {NVN_FORMAT_R32F, VK_FORMAT_R32_SFLOAT, 0}};
    static const struct { uint16_t n; VkFormat f[3]; } dep[] = {
        {NVN_FORMAT_DEPTH24_STENCIL8, {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}}, {NVN_FORMAT_DEPTH32F_STENCIL8, {VK_FORMAT_D32_SFLOAT_S8_UINT}},
        {NVN_FORMAT_DEPTH16, {VK_FORMAT_D16_UNORM}}, {NVN_FORMAT_DEPTH32F, {VK_FORMAT_D32_SFLOAT}}, {NVN_FORMAT_DEPTH24, {VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT}},
        {NVN_FORMAT_STENCIL8, {VK_FORMAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}}};
    for (size_t i = 0; i < sizeof col / sizeof *col; i++) if (col[i].n == nf) {
        if ((fmt_feat(col[i].f) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) return 0;
        *sf = col[i].s ? col[i].s : col[i].f; *asp = VK_IMAGE_ASPECT_COLOR_BIT; return col[i].f; }
    for (size_t i = 0; i < sizeof dep / sizeof *dep; i++) if (dep[i].n == nf) for (int k = 0; k < 3 && dep[i].f[k]; k++) {
        VkFormat f = dep[i].f[k]; if (!(fmt_feat(f) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) continue;
        *sf = f; *asp = (f == VK_FORMAT_S8_UINT ? 0 : VK_IMAGE_ASPECT_DEPTH_BIT) | (f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        return f; }
    return 0;
}
// A host image for t (under vkn_tmu). why: NULL at TextureInitialize (a texture this slice cannot render to is passed over quietly: not every texture in such a pool is a target), else the record
// that wants it as a target (said in red when it cannot have one)
static vkn_img *img_make(const tex_t *t, const char *why) {
    VkFormat sf = 0, f = 0; VkImageAspectFlags asp = 0; const char *no = NULL; int tg = t->d.target;
    if (tg != NVN_TEXTURE_TARGET_2D && tg != NVN_TEXTURE_TARGET_2D_ARRAY && tg != NVN_TEXTURE_TARGET_RECTANGLE) no = "its target";
    else if (t->d.samples > 1) no = "multisampling";
    else if (!t->d.width || !t->d.height) no = "a zero size";
    else if (!(f = fmt_map((unsigned)t->d.format, &sf, &asp))) no = "its format";
    if (no) { if (why) NRED_ONCE("%s: texture ser=%llu %ux%u format %#x target %d samples %d has no host image (%s is not handled yet): its passes are skipped", why, (unsigned long long)t->serial,
                                 t->d.width, t->d.height, t->d.format, tg, t->d.samples, no); return NULL; }
    uint32_t w = t->d.width, h = t->d.height, layers = tg == NVN_TEXTURE_TARGET_2D_ARRAY && t->d.depth ? t->d.depth : 1, lv = t->d.levels ? t->d.levels : 1, mx = 1;
    for (uint32_t m = w > h ? w : h; m > 1; m >>= 1) mx++;
    if (lv > mx) lv = mx;
    VkFormatFeatureFlags ff = fmt_feat(f);
    VkImageUsageFlags u = (asp & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) | (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ? VK_IMAGE_USAGE_SAMPLED_BIT : 0)
                        | (ff & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0) | (ff & VK_FORMAT_FEATURE_TRANSFER_DST_BIT ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0);
    VkFormat fl[2] = {f, sf}; VkImageFormatListCreateInfo li = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 2, fl};   // the two formats it is viewed as (a driver may keep its compression)
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, sf != f ? &li : NULL, sf != f ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0, VK_IMAGE_TYPE_2D, f, {w, h, 1}, lv, layers, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_OPTIMAL, u, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    vkn_img *e = calloc(1, sizeof *e); if (!e) die("vkn backend: image entry allocation failed");
    VkMemoryRequirements mr; int mt;
    if (vk.vkCreateImage(vd.dev, &ii, NULL, &e->img)) { NRED_ONCE("vkCreateImage %ux%u format %d failed: texture ser=%llu has no host image", w, h, f, (unsigned long long)t->serial); free(e); return NULL; }
    vk.vkGetImageMemoryRequirements(vd.dev, e->img, &mr);
    if ((mt = vkn_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) < 0) mt = vkn_mem_type(mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, (uint32_t)mt};
    if (mt < 0 || vk.vkAllocateMemory(vd.dev, &ai, NULL, &e->mem) || vk.vkBindImageMemory(vd.dev, e->img, e->mem, 0)) {
        NRED_ONCE("device memory for a %ux%u host image (%llu B) failed: texture ser=%llu has none", w, h, (unsigned long long)mr.size, (unsigned long long)t->serial);
        if (e->mem) vk.vkFreeMemory(vd.dev, e->mem, NULL); vk.vkDestroyImage(vd.dev, e->img, NULL); free(e); return NULL; }
    e->serial = t->serial; e->fmt = f; e->sfmt = sf; e->aspect = asp; e->w = w; e->h = h; e->layers = layers; e->levels = lv; e->nvn_fmt = (uint32_t)t->d.format;
    e->layout = e->rec = VK_IMAGE_LAYOUT_UNDEFINED; g_img_b += mr.size;
    if (tex_live(e->serial)) { vkn_img **p = it_slot(e->serial); e->next = *p; *p = e; g_nimg++; }   // checked under vkn_tmu: a release after this finds it in the table
    else {
        vkn_img **g = &g_ghost[g_ghost_next++ % NGHOST]; if (*g) img_dead(*g);
        *g = e; e->next = NULL;
        if (!g_ghost_puts++) NLOG("texture ser=%llu was finalized before a record naming it was replayed: its host image is a ghost (the last %d are kept)", (unsigned long long)e->serial, NGHOST);
    }
    if (g_img_made++ < 24) NLOG("host image ser=%llu %ux%u x%u levels %u, NVN format %#x as %d, %llu B of device memory (%s; %llu images, %llu B)", (unsigned long long)e->serial, w, h, layers, lv,
                                e->nvn_fmt, f, (unsigned long long)mr.size, why ? why : "a texture in a CPU-no-access pool", (unsigned long long)g_nimg, (unsigned long long)g_img_b);
    return e;
}
VkImageView vkn_img_view(vkn_img *e, uint32_t level, uint32_t layer, int present) {
    uint32_t key = present ? ~0u : level << 16 | layer;
    for (int i = 0; i < e->nviews; i++) if (e->views[i].key == key) return e->views[i].v;
    if (level >= e->levels || layer >= e->layers) { NRED_ONCE("an attachment view of level %u layer %u, beyond a host image's %u / %u: skipped", level, layer, e->levels, e->layers); return VK_NULL_HANDLE; }
    if (e->nviews == (int)(sizeof e->views / sizeof *e->views)) { NRED_ONCE("a host image has more than %d views: the rest are skipped", e->nviews); return VK_NULL_HANDLE; }
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, e->img, VK_IMAGE_VIEW_TYPE_2D, present ? e->sfmt : e->fmt, {0},
        {present ? VK_IMAGE_ASPECT_COLOR_BIT : e->aspect, present ? 0 : level, 1, present ? 0 : layer, 1}};
    VkImageView v; if (vk.vkCreateImageView(vd.dev, &vi, NULL, &v)) { NRED_ONCE("vkCreateImageView failed (host image ser=%llu)", (unsigned long long)e->serial); return VK_NULL_HANDLE; }
    e->views[e->nviews].key = key; e->views[e->nviews++].v = v;
    return v;
}
static void img_free(vkn_img *e) {
    for (int i = 0; i < e->nviews; i++) vk.vkDestroyImageView(vd.dev, e->views[i].v, NULL);
    VkMemoryRequirements mr; vk.vkGetImageMemoryRequirements(vd.dev, e->img, &mr); g_img_b -= mr.size;
    vk.vkDestroyImage(vd.dev, e->img, NULL); vk.vkFreeMemory(vd.dev, e->mem, NULL); free(e);
}
// Under vkn_tmu. g_leak is read after the completed values: comp_main sets it (seq_cst) before it stores the g_done of a submission retired without the GPU's answer, so a g_done that counts
// such a submission is never paired with a g_leak read as 0 (read first, g_leak could be 0, then the retirement, then a g_done past an image the GPU still uses). The present timeline is
// the GPU's own counter (vk_present.c retires nothing without it; vkn_pres_done reads one it cannot get as 0: nothing done)
void vkn_sweep(void) {
    if (!g_dead_l) return;
    uint64_t dn = atomic_load(&g_done), dp = vkn_pres_done();
    if (atomic_load(&g_leak)) return;   // a submission retired without the GPU's answer: g_done says nothing of what the GPU still uses
    for (vkn_img **p = &g_dead_l; *p; ) { vkn_img *e = *p; if (e->use_nvn <= dn && e->use_pres <= dp) { *p = e->next; img_free(e); } else p = &e->next; }
}


// One record's share of the surface statistics. g_pass_drawn: a clear before the pass's first draw folds into its load action
static int g_pass_drawn;
static void surf_rec(const op_hdr *h, const uint8_t *p, const uint8_t *a) {
    if (h->op < OP_COUNT_) ST(h->op, 1);
    switch (h->op) {
        case OP_SET_RT: { const p_set_rt *r = (const p_set_rt *)a; int nc = r->n < 0 ? 0 : r->n > 8 ? 8 : r->n, ms = 0; g_pass_drawn = 0; ST(S_RT_NC + nc, 1);
            for (int i = 0; i < nc; i++) if (r->color[i]) { fmt_note(0, (unsigned)r->color[i]->d.format); gpu_wrote(r->color[i]); ms |= r->color[i]->d.samples > 1; }
            if (r->depth) { ST(S_RT_DEPTH, 1); if (!nc) ST(S_RT_ZONLY, 1); fmt_note(0, (unsigned)r->depth->d.format); gpu_wrote(r->depth); ms |= r->depth->d.samples > 1; }
            if (ms) ST(S_RT_MSAA, 1);
            break; }
        case OP_CLEAR_COLOR: { const p_clear_color *c = (const p_clear_color *)a; ST(g_pass_drawn ? S_CLR_MID : S_CLR_START, 1); if ((c->mask & 0xF) != 0xF) ST(S_CLR_PART, 1); break; }   // a partial mask needs a draw (vkCmdClearAttachments writes every channel)
        case OP_CLEAR_DS: { const p_clear_ds *c = (const p_clear_ds *)a; int sm = c->stencil_mask & 0xff; ST(g_pass_drawn ? S_DS_MID : S_DS_START, 1); if (sm && sm != 0xff) ST(S_DS_PART, 1); break; }
        case OP_DRAW: { p_draw t; const p_draw *d = draw_rec(a, h->size, &t); g_pass_drawn = 1; ST(S_PRIM + (d->prim & 15), 1); ST(S_DRAW_VTX, d->count > 0 ? d->count : 0);
            if (d->instances > 1) ST(S_DRAW_INST, 1); if (d->base_instance) ST(S_DRAW_BINST, 1); if (d->ind) ST(S_DRAW_IND, 1); if (d->mdi_cnt) ST(S_DRAW_MDI, 1);
            break; }
        case OP_DRAW_IDX: { p_draw_idx t; const p_draw_idx *d = draw_idx_rec(a, h->size, &t); int it = d->itype >= 0 && d->itype < 3 ? d->itype : 2; g_pass_drawn = 1;
            ST(S_PRIM + (d->prim & 15), 1); ST(S_ITYPE + it, 1); ST(S_DIDX_IDX, d->count > 0 ? d->count : 0);
            if (d->instances > 1) ST(S_DIDX_INST, 1); if (d->base_vertex) ST(S_DIDX_BVTX, 1); if (d->base_instance) ST(S_DIDX_BINST, 1); if (d->ind) ST(S_DIDX_IND, 1); if (d->mdi_cnt) ST(S_DIDX_MDI, 1);
            break; }
        case OP_DISPATCH: { const p_dispatch *d = (const p_dispatch *)a; if (d->ind) ST(S_DISP_IND, 1); break; }
        case OP_COPY_B2B: { uint64_t n; memcpy(&n, a + 16, 8); ST(S_B2B_B, n); break; }
        case OP_UPDATE_UB: { uint64_t n; memcpy(&n, a + 8, 8); ST(S_UB_B, n); break; }
        case OP_CLEAR_BUF: ST(S_FILL_B, ((const p_clear_buf *)a)->size); break;
        case OP_COPY_B2T: { const p_copy_b2t *c = (const p_copy_b2t *)a; uint64_t n = region_bytes(c->tex, &c->r); ST(S_B2T_B, n); gpu_wrote(c->tex); break; }
        case OP_COPY_T2B: { const p_copy_b2t *c = (const p_copy_b2t *)a; ST(S_T2B_B, region_bytes(c->tex, &c->r)); break; }   // src holds the destination buffer: a GPU write into pool memory
        case OP_COPY_T2T: { const p_copy_t2t *c = (const p_copy_t2t *)a; if (c->flags & DM_COPY_RESOLVE) ST(S_T2T_RES, 1); gpu_wrote(c->dst); break; }
        case OP_CLEAR_TEX: gpu_wrote(((const p_clear_tex *)a)->tex); break;
        case OP_REPORT_COUNTER: { uint64_t t; memcpy(&t, a, 8); ST(t == NVN_COUNTER_TYPE_TIMESTAMP || t == NVN_COUNTER_TYPE_TIMESTAMP_TOP ? S_CNT_TS : t == NVN_COUNTER_TYPE_SAMPLES_PASSED ? S_CNT_SP : S_CNT_OTH, 1); break; }
        case OP_TF_STATE: ST(S_TF_WHAT + (((const p_tf_state *)a)->what & 3), 1); break;
        case OP_RENDER_COND: ST(S_RC_MODE + (((const p_render_cond *)a)->mode & 3), 1); break;
        case OP_BARRIER: { int32_t bits; memcpy(&bits, a, 4); for (int k = 0; k < 8; k++) if (bits >> k & 1) ST(S_BAR + k, 1); break; }
        case OP_VP_SWIZZLE: { const p_vp_swizzle *w = (const p_vp_swizzle *)a; uint32_t m = 0;   // Adreno has no viewport swizzle: each one needs another way (a flipped viewport for NEGATIVE_Y)
            for (int i = 0; i < w->n && i < 16; i++) for (int k = 0; k < 4; k++) if (w->sw[i][k]) m |= 1u << (8 * k + ((w->sw[i][k] ^ 2 * k) & 7));
            if (m) { ST(S_VPSW, 1); atomic_fetch_or_explicit(&g_vpsw, m, memory_order_relaxed); }
            break; }
        case OP_BIND_STATE: ST(S_STATE + (((const p_bind_state *)a)->kind % 6), 1); break;
        case OP_BIND_VATTR: { const p_bind_vattr *v = (const p_bind_vattr *)a; for (int i = 0; i < v->n && i < 16; i++) fmt_note(2, v->st[i] & 0xff); break; }   // va_st.fmt, the low byte
        default: break;
    }
}

// ---------- Passes: the replay's render-target state (submit thread) ----------
// Targets are kept by serial with their view's level and layer (the game may copy or reuse the tex_t), resolved to host images at SET_RT and again at each submission's start (an image
// released between submissions is gone). A clear before the pass's first draw is recorded and lands as that attachment's load op when the pass begins (rp_begin), as nvn_mtl.m's rp_begin_ex_
// folds it; at SET_RT, a barrier or the submission's end, recorded clears with no draw get a pass of their own (rp_flush, nvn_mtl.m's). A clear after the first draw, or under a scissor that does
// not cover the target, is a vkCmdClearAttachments in the open pass (PLAN.md 2.7: nvn_mtl.m draws a triangle there)
static struct {
    int has_rt, n; uint64_t cser[8], dser; uint32_t clv[8], cly[8], dlv, dly;
    vkn_img *ce[8], *de;                           // resolved for this submission
    int clear_c[8], clear_d, clear_s; VkClearColorValue cc[8]; float cd; uint32_t cs;
    int has_sc; p_scissor sc;
    int on; VkExtent2D area; int att_c[8], att_ds;   // a pass is open (vkCmdBeginRendering), its area and what it attached (vkCmdClearAttachments names those only)
} R;
static uint64_t g_sub_id;                          // this submission (vkn_img.rec_sub)
static vkn_img **g_touch; static int g_ntouch, g_touchcap;   // images whose layout this submission tracks (published at commit)
static VkCommandBuffer cb_now(void) { return g_cur->cb; }
static VkImageLayout rec_layout(vkn_img *e) {
    if (e->rec_sub != g_sub_id) {
        e->rec_sub = g_sub_id; e->rec = e->layout;   // the submit thread is the only writer of layout (at commit), so this read needs no lock
        if (g_ntouch == g_touchcap) { g_touchcap = g_touchcap ? g_touchcap * 2 : 64; g_touch = realloc(g_touch, (size_t)g_touchcap * sizeof *g_touch); if (!g_touch) die("vkn backend: touch list allocation failed"); }
        g_touch[g_ntouch++] = e;
    }
    return e->rec;
}
static void use_mark(vkn_img *e) { if (e) e->use_nvn = atomic_load(&g_fence) + 1; }   // under vkn_tmu: this submission's fence; destruction waits for it
static void rt_resolve(const p_set_rt *r) {   // SET_RT: the targets by serial, made here when TextureInitialize did not (a target outside a CPU-no-access pool)
    pthread_mutex_lock(&vkn_tmu);
    for (int i = 0; i < R.n; i++) {
        const tex_t *t = r->color[i]; R.ce[i] = NULL; R.cser[i] = t ? t->serial : 0;
        if (!t || !t->serial) continue;
        R.clv[i] = r->has_cview[i] && r->cview[i].set_levels ? r->cview[i].base_level : 0; R.cly[i] = r->has_cview[i] && r->cview[i].set_layers ? r->cview[i].min_layer : 0;
        if (r->has_cview[i] && r->cview[i].set_format && r->cview[i].format != t->d.format)
            NRED_ONCE("SET_RT: a colour target viewed as format %#x, the texture's is %#x (ser=%llu): rendered in the texture's own format (views of another format are a later slice)", r->cview[i].format, t->d.format, (unsigned long long)t->serial);
        int dup = 0; for (int k = 0; k < i; k++) dup |= R.cser[k] == R.cser[i] && R.clv[k] == R.clv[i] && R.cly[k] == R.cly[i];
        if (dup) { NRED_ONCE("SET_RT attaches one target view to two colour slots: the second is skipped"); R.cser[i] = 0; continue; }
        if (!(R.ce[i] = vkn_img_get(t->serial))) R.ce[i] = img_make(t, "SET_RT");
        use_mark(R.ce[i]);
    }
    const tex_t *d = r->depth; R.de = NULL; R.dser = d ? d->serial : 0;
    if (d && d->serial) {
        R.dlv = r->has_dview && r->dview.set_levels ? r->dview.base_level : 0; R.dly = r->has_dview && r->dview.set_layers ? r->dview.min_layer : 0;
        if (r->has_dview && r->dview.set_format && r->dview.format != d->d.format)
            NRED_ONCE("SET_RT: a depth target viewed as format %#x, the texture's is %#x (ser=%llu): rendered in the texture's own format (views of another format are a later slice)", r->dview.format, d->d.format, (unsigned long long)d->serial);
        if (!(R.de = vkn_img_get(d->serial))) R.de = img_make(d, "SET_RT");
        use_mark(R.de);
    }
    pthread_mutex_unlock(&vkn_tmu);
}
static void rt_reresolve(void) {   // a submission's start: the targets the last SET_RT left, as they are now
    if (!R.has_rt) return;
    pthread_mutex_lock(&vkn_tmu);
    for (int i = 0; i < R.n; i++) { R.ce[i] = vkn_img_get(R.cser[i]); use_mark(R.ce[i]); }
    R.de = vkn_img_get(R.dser); use_mark(R.de);
    pthread_mutex_unlock(&vkn_tmu);
}
static VkExtent2D lv_ext(const vkn_img *e, uint32_t lv) { VkExtent2D x = {e->w >> lv, e->h >> lv}; if (!x.width) x.width = 1; if (!x.height) x.height = 1; return x; }
static int sc_full(VkExtent2D x) { return !R.has_sc || (R.sc.x <= 0 && R.sc.y <= 0 && (int64_t)R.sc.x + R.sc.w >= (int64_t)x.width && (int64_t)R.sc.y + R.sc.h >= (int64_t)x.height); }
static void mem_barrier(VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, ss, sa, ds, da};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &b, 0, NULL, 0, NULL};
    vk.vkCmdPipelineBarrier2(cb_now(), &di);
}
// An attachment's barrier at a pass's start: into its attachment layout (the whole image: one tracked layout each), after everything before it wrote (an earlier pass on it included)
static void att_barrier(VkImageMemoryBarrier2 *b, vkn_img *e) {
    int col = e->aspect == VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout to = col ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    *b = (VkImageMemoryBarrier2){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
        col ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        col ? VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        rec_layout(e), to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, e->img, {e->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    e->rec = to;
}
static void clears_reset(void) { memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0; }
// Opens the pass on the targets. clear_only: attach only the targets with a recorded clear (rp_flush). 0 = nothing to attach (the pass is skipped, as nvn_mtl.m skips one with no texture)
static int rp_begin(int clear_only) {
    if (R.on) return 1;
    if (!R.has_rt) { NRED_ONCE("a draw or clear with no render target set: skipped"); clears_reset(); return 0; }
    VkRenderingAttachmentInfo ca[8], da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, sa = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkExtent2D ex[9]; VkImageMemoryBarrier2 b[9]; uint32_t nb = 0, nc = 0, w = UINT32_MAX, h = UINT32_MAX; int any = 0, has_d = 0, has_s = 0;
    for (int i = 0; i < 8; i++) { ca[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; R.att_c[i] = 0; }
    R.att_ds = 0;
    pthread_mutex_lock(&vkn_tmu);   // views are made under it (the present thread makes its own on the same images)
    for (int i = 0; i < R.n; i++) {
        vkn_img *e = R.ce[i]; VkImageView v;
        if (!e || (clear_only && !R.clear_c[i]) || !(v = vkn_img_view(e, R.clv[i], R.cly[i], 0))) continue;
        ca[i].imageView = v; ca[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca[i].loadOp = R.clear_c[i] ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE; ca[i].clearValue.color = R.cc[i];
        ex[i] = lv_ext(e, R.clv[i]); if (ex[i].width < w) w = ex[i].width; if (ex[i].height < h) h = ex[i].height;
        att_barrier(&b[nb++], e); nc = (uint32_t)i + 1; any = 1; R.att_c[i] = 1;
        if (R.clear_c[i]) { memcpy(e->clear, R.cc[i].float32, sizeof e->clear); e->cleared = 1; g_stat_fold++; }
    }
    vkn_img *de = R.de; VkImageView dv = VK_NULL_HANDLE;
    if (de && (!clear_only || R.clear_d || R.clear_s) && (dv = vkn_img_view(de, R.dlv, R.dly, 0))) {
        VkClearValue cv; cv.depthStencil.depth = R.cd; cv.depthStencil.stencil = R.cs;
        if (de->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { has_d = 1; da.imageView = dv; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; da.loadOp = R.clear_d ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                                                      da.storeOp = VK_ATTACHMENT_STORE_OP_STORE; da.clearValue = cv; g_stat_fold += R.clear_d; }
        if (de->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) { has_s = 1; sa.imageView = dv; sa.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; sa.loadOp = R.clear_s ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                                                        sa.storeOp = VK_ATTACHMENT_STORE_OP_STORE; sa.clearValue = cv; g_stat_fold += R.clear_s; }
        ex[8] = lv_ext(de, R.dlv); if (ex[8].width < w) w = ex[8].width; if (ex[8].height < h) h = ex[8].height;
        att_barrier(&b[nb++], de); any = 1; R.att_ds = 1;
    }
    pthread_mutex_unlock(&vkn_tmu);
    clears_reset();
    if (!any) return 0;
    VkCommandBuffer cb = cb_now();
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, nb, b};
    vk.vkCmdPipelineBarrier2(cb, &di);
    // A pass covers only its smallest attachment, and so does a clear folded into its load op, while NVN clears the whole target (nvn_mtl.m: Bayonetta 2's corner-only depth clear):
    // a cleared attachment larger than the pass is cleared in a pass of its own first, then loaded
    int own = 0;
    for (int i = 0; i < 9; i++) {
        int ds = i == 8; VkRenderingAttachmentInfo *c = ds ? NULL : &ca[i];
        if (ds ? !R.att_ds || !((has_d && da.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) || (has_s && sa.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)) : !R.att_c[i] || c->loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR) continue;
        if (ex[i].width <= w && ex[i].height <= h) continue;
        VkRenderingAttachmentInfo d1 = da, s1 = sa;
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, ex[i]}, 1, 0, ds ? 0 : 1, ds ? NULL : c, ds && has_d ? &d1 : NULL, ds && has_s ? &s1 : NULL};
        vk.vkCmdBeginRendering(cb, &ri); vk.vkCmdEndRendering(cb);
        if (ds) { da.loadOp = sa.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; } else c->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        own = 1; g_stat_own++;
    }
    if (own) mem_barrier(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    R.area = (VkExtent2D){w, h};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, R.area}, 1, 0, nc, ca, has_d ? &da : NULL, has_s ? &sa : NULL};
    vk.vkCmdBeginRendering(cb, &ri);
    R.on = 1; g_stat_pass++;
    return 1;
}
static void rp_end(void) { if (R.on) { vk.vkCmdEndRendering(cb_now()); R.on = 0; } }
static void rp_flush(void) {   // recorded clears with no draw land in a pass of their own; an open pass ends
    int pend = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
    if (pend && !R.on) rp_begin(1);
    rp_end();
}
// vkCmdClearAttachments in the open pass: over its area, or the scissor's part of it
static void clear_in_pass(const VkClearAttachment *a, VkExtent2D x) {
    VkClearRect r = {{{0, 0}, R.area}, 0, 1};
    if (!sc_full(x)) {
        int64_t x0 = R.sc.x > 0 ? R.sc.x : 0, y0 = R.sc.y > 0 ? R.sc.y : 0, x1 = (int64_t)R.sc.x + R.sc.w, y1 = (int64_t)R.sc.y + R.sc.h;
        if (x1 > R.area.width) x1 = R.area.width; if (y1 > R.area.height) y1 = R.area.height;
        if (x1 <= x0 || y1 <= y0) return;
        r.rect = (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    }
    vk.vkCmdClearAttachments(cb_now(), 1, a, 1, &r); g_stat_clr_mid++;
}
// A full clear in the open pass of an attachment larger than the pass: vkCmdClearAttachments reaches the pass's area only, NVN clears the whole target. The pass ends and the clear moves to the
// start of the next one, as nvn_mtl.m does with a mid-pass depth-stencil clear: rp_begin clears an oversize attachment in a pass of its own, then loads it (the attachments not cleared load too).
// A scissored clear stays in the pass, clipped to its area (nvn_mtl.m's triangle is too). 1 = the pass was ended, record the clear
static int oversize_mid(VkExtent2D x) {
    if (x.width <= R.area.width && x.height <= R.area.height) return 0;
    rp_end();
    static int once; if (!once++) NLOG("mid-pass full clear of a %ux%u attachment in a %ux%u pass: the pass ends, the clear moves to the start of the next (first case pass %llu)", x.width, x.height, R.area.width, R.area.height, (unsigned long long)g_stat_pass);
    return 1;
}
// The channels a target's format has (mask bits R 1, G 2, B 4, A 8): a clear whose mask covers them all writes the whole texel. RGBX8's X is undefined, so it counts as RGB
static unsigned chan_mask(const vkn_img *e) {
    switch (e->fmt) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R32_SFLOAT: return 1;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16G16_SFLOAT: return 3;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return 7;
        default: return e->nvn_fmt == NVN_FORMAT_RGBX8 ? 7 : 0xf;
    }
}
static void op_clear_color(const p_clear_color *c) {
    if (c->index < 0 || c->index >= R.n || !R.cser[c->index]) { NRED_ONCE("colour clear of slot %d, which has no target: skipped", c->index); return; }
    vkn_img *e = R.ce[c->index]; if (!e) return;   // no host image: said at SET_RT
    unsigned all = chan_mask(e); if ((c->mask & all) != all) { NRED_ONCE("colour clear with channel mask %#x of a target with channels %#x: needs a draw (a later slice), skipped", c->mask, all); return; }
    VkExtent2D x = lv_ext(e, R.clv[c->index]);
    if (sc_full(x) && (!R.on || oversize_mid(x))) { R.clear_c[c->index] = 1; memcpy(R.cc[c->index].float32, c->rgba, sizeof c->rgba); return; }   // folded into the next pass's load op
    if (!rp_begin(0) || !R.att_c[c->index]) return;
    VkClearAttachment a = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)c->index, {{{c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]}}}};
    clear_in_pass(&a, x);
    pthread_mutex_lock(&vkn_tmu); memcpy(e->clear, c->rgba, sizeof e->clear); e->cleared = 1; pthread_mutex_unlock(&vkn_tmu);   // the present thread reads it under the lock
}
static void op_clear_ds(const p_clear_ds *c) {
    vkn_img *e = R.de; if (!R.dser || !e) return;   // no depth target: nothing to clear (none made: said at SET_RT)
    unsigned sm = (unsigned)c->stencil_mask & 0xff; int cd = c->depth_mask && (e->aspect & VK_IMAGE_ASPECT_DEPTH_BIT), cs = sm && (e->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    if (cs && sm != 0xff) { NRED_ONCE("depth-stencil clear with stencil mask %#x: needs a draw (a later slice), its stencil part is skipped", sm); cs = 0; }
    if (!cd && !cs) return;
    VkExtent2D x = lv_ext(e, R.dlv);
    float dv = c->depth >= 0.0f ? (c->depth <= 1.0f ? c->depth : 1.0f) : 0.0f;   // Vulkan wants [0, 1] (no VK_EXT_depth_range_unrestricted); NaN goes to 0
    if (sc_full(x) && (!R.on || oversize_mid(x))) { if (cd) { R.clear_d = 1; R.cd = dv; } if (cs) { R.clear_s = 1; R.cs = (uint32_t)c->stencil & 0xff; } return; }
    if (!rp_begin(0) || !R.att_ds) return;
    VkClearAttachment a = {(cd ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (cs ? VK_IMAGE_ASPECT_STENCIL_BIT : 0), 0, {{{0}}}};
    a.clearValue.depthStencil.depth = dv; a.clearValue.depthStencil.stencil = (uint32_t)c->stencil & 0xff;
    clear_in_pass(&a, x);
}
// NVN barrier bits order earlier GPU work before later work (and invalidate caches): one global memory barrier. Inside a pass the pass ends first (a pipeline barrier inside dynamic rendering
// is only for framebuffer-local dependencies); the next draw opens it again with load ops. Clears still recorded stay recorded: they land after the barrier, as on NVN
static void op_barrier(int32_t bits) {
    if (!bits) return;
    rp_end();
    mem_barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    g_stat_bar++;
}

// ---------- Submission ----------
// Counter timestamps (19.2 MHz) from the guest clock, as the Metal layer's gpu_ticks: games compare them with svc GetSystemTick. Monotonic fallback where guest_ns is 0 (unit tests)
static uint64_t gpu_ticks(void) { uint64_t g = guest_ns(); if (!g) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); g = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; } return g * 12 / 625; }
static void post(post_op o) {
    sub_slot *s = g_cur ? g_cur : &g_sub[0];
    if (s->npost == s->postcap) { s->postcap = s->postcap ? s->postcap * 2 : 64; s->post = realloc(s->post, (size_t)s->postcap * sizeof *s->post); if (!s->post) die("vkn backend: completion list allocation failed"); }
    s->post[s->npost++] = o;
}
// A submission's completion effects, in record order: event values, then counter reports (all with this moment's time, as nvn_mtl.m's handler writes them)
static void post_run(sub_slot *s) {
    uint64_t t = gpu_ticks();
    for (int i = 0; i < s->npost; i++) {
        post_op *o = &s->post[i];
        if (o->kind == 1) { if (o->mode == 1) *o->val += o->v; else *o->val = o->v; continue; }
        // A timestamp report's counter field holds the time (0 under render_cond, as on the GPU); samples passed: none were drawn; other counters: 0
        uint64_t rec[2] = {(o->type == NVN_COUNTER_TYPE_TIMESTAMP || o->type == NVN_COUNTER_TYPE_TIMESTAMP_TOP) && !DM_GAME_RENDER_COND ? t : 0, t};
        if (cpu_target(o->addr, sizeof rec, "counter report")) memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec);
    }
    s->npost = 0;
}
// The completion thread: each committed submission in order, once the GPU has signalled its timeline value; its effects, then its fence counts as done (mtl_wait's waiters wake)
static void *comp_main(void *arg) {
    (void)arg;
    pthread_setname_np("vkn-complete");   // Darwin's form: dm/host/plat/plat.h maps it on Linux
    pthread_mutex_lock(&g_mu);
    for (;;) {
        while (g_ncomp == g_ncommit) pthread_cond_wait(&g_ccv, &g_mu);
        sub_slot *s = &g_sub[g_ncomp % NSUB]; uint64_t v = s->value; int failed = s->failed;
        pthread_mutex_unlock(&g_mu);
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &g_tl, &v};
        // A submitted one waits for the GPU's answer, even once vkn_lost is set (a failed vkQueueSubmit2 is not always a lost device: earlier submissions still run); on a lost device the wait
        // returns in finite time (VK_ERROR_DEVICE_LOST or VK_SUCCESS). Only a driver that breaks that (10 s more of timeouts with vkn_lost set) gets its effects written without the GPU, and then
        // g_leak: nothing the GPU may still use is freed from here on. VK_ERROR_DEVICE_LOST is the device's answer (nothing of it runs any more). Any other error (out of host / device memory)
        // says nothing of the GPU's work: retired without it, and g_leak as well. g_leak is set before g_done moves past it (vkn_sweep's order). Not submitted (failed): at once, in order
        for (int k = 0; !failed; k++) {
            VkResult r = vk.vkWaitSemaphores(vd.dev, &wi, 1000000000ull);
            if (r == VK_SUCCESS) break;
            if (r == VK_ERROR_DEVICE_LOST) { vkn_gpu_lost("the completion thread's vkWaitSemaphores", r); break; }
            if (r != VK_TIMEOUT) {
                vkn_gpu_lost("the completion thread's vkWaitSemaphores", r);
                if (!atomic_exchange(&g_leak, 1)) NLOG("[RED] submission %llu: vkWaitSemaphores %d, not an answer from the GPU: retired without it; from here nothing the GPU may still use is freed (host images and command buffers are leaked)", (unsigned long long)v, r);
                break;
            }
            if (k == 5) NRED_ONCE("submission %llu has not completed on the GPU in 5 s", (unsigned long long)v);
            if (k >= 10 && atomic_load(&vkn_lost)) {
                if (!atomic_exchange(&g_leak, 1)) NLOG("[RED] submission %llu: no answer from the GPU in %d s although it is lost to this run: retired without it; from here nothing the GPU may still use is freed (host images and command buffers are leaked)", (unsigned long long)v, k + 1);
                break;
            }
        }
        post_run(s);
        pthread_mutex_lock(&g_mu); atomic_store(&g_done, v); g_ncomp++; pthread_cond_broadcast(&g_cv);
    }
    return NULL;
}

void mtl_sub_begin(void) {
    mtl_init();
    if (!g_vk) { g_cur = &g_sub[0]; g_cur->npost = 0; return; }
    uint64_t i = g_nbegun++;
    pthread_mutex_lock(&g_mu); while (g_ncomp + NSUB <= i) pthread_cond_wait(&g_cv, &g_mu); pthread_mutex_unlock(&g_mu);   // the slot's last submission has completed
    g_cur = &g_sub[i % NSUB]; g_cur->npost = 0; g_cur->failed = atomic_load(&vkn_lost) || atomic_load(&g_leak);   // failed from the start: nothing is recorded (its pool may hold a buffer the lost GPU never finished)
    g_sub_id++; g_ntouch = 0;
    pthread_mutex_lock(&vkn_tmu); vkn_sweep(); pthread_mutex_unlock(&vkn_tmu);
    if (g_cur->failed) return;
    vk.vkResetCommandPool(vd.dev, g_cur->pool, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vk.vkBeginCommandBuffer(g_cur->cb, &bi);
    rt_reresolve();
}
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *h, int n) { (void)h; (void)n; }
// In submission order on the CPU, at once: later commands of the same submission see the bytes (on the GPU they run in order too).
// The source must be pool memory as well: a sparse virtual pool's range stays PROT_NONE on Linux (hle_nvn.c vpool_map keeps its pages in the page table only), and reading it would kill the process
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    void *b; uint64_t o;
    if (!n) return;
    if (!mtl_addr(src, n, &b, &o)) { NLOG("[RED] buffer copy source outside every memory pool src=%#llx n=%#llx: skipped", (unsigned long long)src, (unsigned long long)n); return; }
    if (cpu_target(dst, n, "buffer copy")) memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, (size_t)n);
}
void mtl_sub_fill(uint64_t dst, uint64_t n, uint32_t value) {
    if (!n || !cpu_target(dst, n, "buffer fill")) return;
    uint8_t *d = (uint8_t *)(uintptr_t)dst; uint64_t i = 0;
    for (; i + 4 <= n; i += 4) memcpy(d + i, &value, 4);
    if (i < n) memcpy(d + i, &value, (size_t)(n - i));   // a size that is not a multiple of 4 takes the pattern's leading bytes
}
static void sub_update(const void *data, uint64_t dst, uint64_t n) { if (n && cpu_target(dst, n, "uniform update")) memcpy((void *)(uintptr_t)dst, data, (size_t)n); }
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post((post_op){1, val, mode, v, 0, 0}); }
void mtl_sub_counter(uint64_t addr, uint32_t type) { post((post_op){2, NULL, 0, 0, addr, type}); }

void mtl_exec(const uint8_t *p) {
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        const uint8_t *a = p + 8;
        surf_rec(&h, p, a);
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); mtl_exec((const uint8_t *)(uintptr_t)t); break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); mtl_sub_event(val, m, v); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); mtl_sub_counter(ad, (uint32_t)t); break; }
            case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = atomic_load(&g_fence) + 1; s->fenced = 1; break; }   // the fence this submission commits with
            case OP_COPY_B2B: { uint64_t s_, d, n; memcpy(&s_, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); mtl_sub_copy(s_, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); sub_update(a + 16, d, n); break; }
            case OP_CLEAR_BUF: { const p_clear_buf *cb = (const p_clear_buf *)a; mtl_sub_fill(cb->addr, cb->size, cb->value); break; }
            default: break;
        }
        if (g_vk && !g_cur->failed) switch (h.op) {   // the GPU part: targets, clears, passes, barriers (none once the GPU is lost: mtl_sub_begin)
            case OP_SET_RT: { const p_set_rt *r = (const p_set_rt *)a; rp_flush(); R.n = r->n < 0 ? 0 : r->n > 8 ? 8 : r->n; R.has_rt = 1; rt_resolve(r); break; }
            case OP_CLEAR_COLOR: op_clear_color((const p_clear_color *)a); break;
            case OP_CLEAR_DS: op_clear_ds((const p_clear_ds *)a); break;
            case OP_DRAW: case OP_DRAW_IDX: rp_begin(0); break;   // counted only: the pass opens (its clears land), nothing is drawn yet
            case OP_BARRIER: { int32_t bits; memcpy(&bits, a, 4); op_barrier(bits); break; }
            case OP_SCISSOR: { memcpy(&R.sc, a, sizeof R.sc); R.has_sc = 1; break; }
            default: break;
        }
        p += h.size;
    }
}
uint64_t mtl_sub_commit(void) {
    sub_slot *s = g_cur;
    if (!g_vk || s->failed) g_cur = NULL;   // nothing more is recorded (the GPU path below ends the pass first: rp_flush records through g_cur)
    if (!g_vk) {   // the null backend's: done at once
        uint64_t fence = atomic_load(&g_fence) + 1; post_run(s);
        pthread_mutex_lock(&g_mu); atomic_store(&g_fence, fence); atomic_store(&g_done, fence); pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
        return fence;
    }
    if (s->failed) {   // begun after the GPU was lost: nothing recorded, it completes behind the earlier ones (the completion thread, in order)
        pthread_mutex_lock(&vkn_qmu); uint64_t v = atomic_load(&g_fence) + 1; atomic_store(&g_fence, v); pthread_mutex_unlock(&vkn_qmu);
        pthread_mutex_lock(&g_mu); s->value = v; g_ncommit++; pthread_cond_signal(&g_ccv); pthread_mutex_unlock(&g_mu);
        return v;
    }
    rp_flush();   // an open pass or recorded clears land in this submission's command buffer (cb_now: g_cur, still s)
    vk.vkEndCommandBuffer(s->cb);
    g_cur = NULL;
    // Under the queue lock: the layouts this submission leaves become the published ones as it is queued, so the present thread (which samples the window texture and restores its layout
    // under the same lock) always records against the layout the queue will have
    pthread_mutex_lock(&vkn_qmu);
    uint64_t v = atomic_load(&g_fence) + 1;
    for (int i = 0; i < g_ntouch; i++) g_touch[i]->layout = g_touch[i]->rec;
    VkSemaphoreSubmitInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, g_tl, v, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
    VkCommandBufferSubmitInfo c = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, s->cb, 0};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 0, NULL, 1, &c, 1, &sig};
    VkResult r = vk.vkQueueSubmit2(vd.q, 1, &si, VK_NULL_HANDLE);
    atomic_store(&g_fence, v);
    pthread_mutex_unlock(&vkn_qmu);
    if (r) { vkn_gpu_lost("vkQueueSubmit2", r); s->failed = 1; }
    g_ntouch = 0;
    pthread_mutex_lock(&g_mu); s->value = v; g_ncommit++; pthread_cond_signal(&g_ccv); pthread_mutex_unlock(&g_mu);
    return v;
}
uint64_t mtl_last_fence(void) { return atomic_load(&g_fence); }
// A fence beyond the last commit (a sync object fenced by a submission still being replayed on another thread) is waited for, as with Metal
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    if (atomic_load(&g_done) >= fence) return 1;
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t ns = (uint64_t)dl.tv_nsec + (timeout_ns > 3600000000000ull ? 3600000000000ull : timeout_ns);
    dl.tv_sec += (time_t)(ns / 1000000000ull); dl.tv_nsec = (long)(ns % 1000000000ull);
    pthread_mutex_lock(&g_mu);
    while (atomic_load(&g_done) < fence) if (pthread_cond_timedwait(&g_cv, &g_mu, &dl)) break;
    int ok = atomic_load(&g_done) >= fence;
    pthread_mutex_unlock(&g_mu);
    return ok;
}
void mtl_finish(void) { uint64_t f = atomic_load(&g_fence); while (!mtl_wait(f, 3600000000000ull)) {} }
static void surf_report(const char *why);
static void vkn_report(const char *why) {   // the backend's own line beside the surface one: what reached the GPU
    pthread_mutex_lock(&vkn_tmu); uint64_t ni = g_nimg, ib = g_img_b, gp = g_ghost_puts; pthread_mutex_unlock(&vkn_tmu);
    NLOG("GPU work up to frame %llu (%s): submissions %llu, passes %llu, clears folded into load ops %llu, clears in a pass %llu, oversize clears in a pass of their own %llu, barriers %llu; host images %llu (%llu B, ghosts included), ghosts made %llu; validation %s %llu",
         (unsigned long long)atomic_load(&g_present_n), why, (unsigned long long)atomic_load(&g_fence), (unsigned long long)g_stat_pass, (unsigned long long)g_stat_fold, (unsigned long long)g_stat_clr_mid,
         (unsigned long long)g_stat_own, (unsigned long long)g_stat_bar, (unsigned long long)ni, (unsigned long long)ib, (unsigned long long)gp, vd.validation ? "on, messages" : "off", (unsigned long long)vkn_validation_count());
}
#define STATS_FROM 60   // no statistics line before this present: the parity cut ends at the host's own line of present 1 or 2 (parity.py PRESENT), and lines before it must be the HLE's alone
// The suspend path's bound (mtl_drain): Android calls it on the UI thread (front_android.c on_stop → dm_gfx_suspend), where a GPU that hangs must not hang the app into an ANR, the same 4 s as
// the window loss (vk_present.c dm_gfx_window_lost). Elsewhere it waits as long as it takes (the graceful exit's lines come after it)
#ifdef __ANDROID__
#define DRAIN_NS 4000000000ull
#else
#define DRAIN_NS 3600000000000ull
#endif
static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
void mtl_drain(void) {   // the last lines of a run (the graceful exit ends in _exit, no atexit), or the app going to the background: every present handed over, the GPU idle
    if (g_vk) {   // what has not finished when the bound runs out finishes behind it: nothing here depends on it (resume reopens the gate only; destruction still waits for the GPU's answer)
        uint64_t t0 = mono_ns(); int ok = vkn_present_drain(DRAIN_NS); uint64_t used = mono_ns() - t0;
        ok = ok && used < DRAIN_NS && mtl_wait(atomic_load(&g_fence), DRAIN_NS - used);
        if (!ok) NLOG("[RED] graphics suspend: the GPU work in flight has not finished in %llu s: suspended regardless (it completes behind it)", (unsigned long long)(DRAIN_NS / 1000000000ull));
    }
    if (atomic_load(&g_present_n) >= STATS_FROM) { if (g_vk) vkn_report("graphics suspended: exit, or the app in the background"); surf_report("graphics suspended: exit, or the app in the background"); }
}

// ---------- The surface lines (see the statistics at the top): one line of counts and the pools made or freed since the last one ----------
static void fmt_list(char *b, size_t cap, size_t *o, const _Atomic uint64_t *bits, int nbits) {
    int any = 0;
    for (int f = 0; f < nbits && *o < cap; f++) if (atomic_load_explicit(&bits[f >> 6], memory_order_relaxed) >> (f & 63) & 1) { *o += (size_t)snprintf(b + *o, cap - *o, "%s%#x", any ? "," : " ", f); any = 1; }
    if (!any && *o < cap) *o += (size_t)snprintf(b + *o, cap - *o, " none");
}
static void surf_report(const char *why) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&mu);   // the present thread and mtl_drain
    static const char *const prim[16] = {"points", "lines", "line_loop", "line_strip", "triangles", "triangle_strip", "triangle_fan", "quads", "quad_strip", "polygon", "lines_adj", "line_strip_adj", "triangles_adj", "triangle_strip_adj", "patches", "?"};
    static const char *const bar[8] = {"order_primitives", "order_fragments", "order_fragments_tiled", "order_indirect", "invalidate_texture", "invalidate_shader", "invalidate_texture_descriptor", "invalidate_zcull"};
    unsigned long long v[S_N], n = atomic_load(&g_present_n); for (int i = 0; i < S_N; i++) v[i] = atomic_load_explicit(&g_st[i], memory_order_relaxed);
    static char b[8192]; size_t o = 0;
#define AP(...) do { if (o < sizeof b) o += (size_t)snprintf(b + o, sizeof b - o, __VA_ARGS__); } while (0)
    AP("surface up to frame %llu (%s): records", n, why);
    for (int i = 0; i < OP_COUNT_; i++) if (v[i]) AP(" %s=%llu", g_opname[i] ? g_opname[i] : "?", v[i]);
    AP("; draws %llu (instanced %llu, base instance %llu, indirect %llu, indirect-count %llu; %llu vertices), indexed %llu (instanced %llu, base vertex %llu, base instance %llu, indirect %llu, indirect-count %llu; %llu indices); primitives",
       v[OP_DRAW], v[S_DRAW_INST], v[S_DRAW_BINST], v[S_DRAW_IND], v[S_DRAW_MDI], v[S_DRAW_VTX], v[OP_DRAW_IDX], v[S_DIDX_INST], v[S_DIDX_BVTX], v[S_DIDX_BINST], v[S_DIDX_IND], v[S_DIDX_MDI], v[S_DIDX_IDX]);
    for (int i = 0; i < 16; i++) if (v[S_PRIM + i]) AP(" %s=%llu", prim[i], v[S_PRIM + i]);
    AP("; index types u8=%llu u16=%llu u32=%llu", v[S_ITYPE], v[S_ITYPE + 1], v[S_ITYPE + 2]);
    AP("; colour clears %llu before the pass's first draw / %llu after it / %llu with a partial mask; depth-stencil clears %llu / %llu / %llu with a partial stencil mask; texture clears %llu; buffer fills %llu (%llu B)",
       v[S_CLR_START], v[S_CLR_MID], v[S_CLR_PART], v[S_DS_START], v[S_DS_MID], v[S_DS_PART], v[OP_CLEAR_TEX], v[OP_CLEAR_BUF], v[S_FILL_B]);
    AP("; copies buffer to buffer %llu (%llu B), buffer to texture %llu (%llu B), texture to buffer %llu (%llu B), texture to texture %llu (%llu resolves); uniform updates %llu (%llu B); bytes GPU ops wrote into pool memory %llu",
       v[OP_COPY_B2B], v[S_B2B_B], v[OP_COPY_B2T], v[S_B2T_B], v[OP_COPY_T2B], v[S_T2B_B], v[OP_COPY_T2T], v[S_T2T_RES], v[OP_UPDATE_UB], v[S_UB_B], v[S_GW_B]);
    AP("; compute dispatches %llu (%llu indirect); counter reports timestamp %llu / samples passed %llu / other %llu, resets %llu; transform feedback binds %llu, begin %llu end %llu pause %llu resume %llu; render-enable records %llu (equal %llu, not equal %llu, on %llu, off %llu)",
       v[OP_DISPATCH], v[S_DISP_IND], v[S_CNT_TS], v[S_CNT_SP], v[S_CNT_OTH], v[OP_RESET_COUNTER], v[OP_TF_BIND], v[S_TF_WHAT], v[S_TF_WHAT + 1], v[S_TF_WHAT + 2], v[S_TF_WHAT + 3],
       v[OP_RENDER_COND], v[S_RC_MODE], v[S_RC_MODE + 1], v[S_RC_MODE + 2], v[S_RC_MODE + 3]);
    { static const char *const sw[8] = {"+x", "-x", "+y", "-y", "+z", "-z", "+w", "-w"}; uint32_t m = atomic_load(&g_vpsw); AP("; viewport swizzle records not the identity %llu", v[S_VPSW]);
      for (int i = 0; i < 32; i++) if (m >> i & 1) AP(" %c=%s", "xyzw"[i / 8], sw[i % 8]); }
    AP("; state binds blend %llu, colour %llu, channel mask %llu, multisample %llu, polygon %llu, depth-stencil %llu", v[S_STATE], v[S_STATE + 1], v[S_STATE + 2], v[S_STATE + 3], v[S_STATE + 4], v[S_STATE + 5]);
    AP("; barriers %llu, bits", v[OP_BARRIER]); for (int i = 0; i < 8; i++) if (v[S_BAR + i]) AP(" %s=%llu", bar[i], v[S_BAR + i]);
    AP("; render-target sets %llu, colour targets", v[OP_SET_RT]); for (int i = 0; i < 9; i++) if (v[S_RT_NC + i]) AP(" %d:%llu", i, v[S_RT_NC + i]);
    AP(", with depth %llu, depth only %llu, multisampled %llu, formats", v[S_RT_DEPTH], v[S_RT_ZONLY], v[S_RT_MSAA]); fmt_list(b, sizeof b, &o, g_fmt[0], 256);
    AP("; registered texture formats"); fmt_list(b, sizeof b, &o, g_fmt[1], 256);
    { _Atomic uint64_t t[1]; atomic_init(&t[0], atomic_load(&g_tgt)); AP(", targets"); fmt_list(b, sizeof b, &o, t, 32); }
    AP("; vertex attribute formats"); fmt_list(b, sizeof b, &o, g_fmt[2], 256);
    AP("; ReadTexels %llu (%llu B), %llu of them of textures the GPU writes", v[S_RTX], v[S_RTX_B], v[S_RTX_GPU]);
    pthread_mutex_lock(&g_pmu);
    AP("; memory pools made %llu (%llu B), freed %llu (%llu B), live %llu B, peak %llu B", (unsigned long long)g_pmade, (unsigned long long)g_pmade_b, (unsigned long long)g_pfreed, (unsigned long long)g_pfreed_b,
       (unsigned long long)g_plive_b, (unsigned long long)g_ppeak_b);
    NLOG("%s", b);
    // Each pool once when made, once more when freed (64 lines at most per report: the rest wait for the next)
    for (int i = 0, k = 0; i < g_nplog && k < 64; i++) {
        pool_log *l = &g_plog[i]; int end = l->died != ~0ull;
        if (l->said == 2 || (l->said == 1 && !end)) continue;
        if (end) NLOG("memory pool %llu: %#llx size %#llx (%llu B) made at frame %llu, freed at frame %llu", (unsigned long long)l->id, (unsigned long long)l->mem, (unsigned long long)l->size, (unsigned long long)l->size, (unsigned long long)l->born, (unsigned long long)l->died);
        else NLOG("memory pool %llu: %#llx size %#llx (%llu B) made at frame %llu", (unsigned long long)l->id, (unsigned long long)l->mem, (unsigned long long)l->size, (unsigned long long)l->size, (unsigned long long)l->born);
        l->said = end ? 2 : 1; k++;
    }
    pthread_mutex_unlock(&g_pmu);
#undef AP
    pthread_mutex_unlock(&mu);
}

// ---------- Present: counted, then handed to the present thread (vk_present.c); pacing stays with hle_nvn.c's own clock (no display link, as the null backend) ----------
extern _Atomic uint64_t dm_present_ns;   // hle_thread.c (DM_HITCH_DUMP): when the last present was made
static uint64_t present_count(void) {
    mtl_init();   // DM_NVN_NULL_EVERY
    atomic_store(&dm_present_ns, now_ns());
    uint64_t n = atomic_fetch_add(&g_present_n, 1) + 1;
    // Not worded "first present": lognorm.py --until-present ends the parity cut at that phrase, and this line comes before the host's own (DM_DETCLOCK, [nvn] frame 1) that end it on the Metal backend
    if (n == 1) { NLOG("%s", g_vk ? "presenting: frames go to the swapchain" : "presenting: frames are counted, nothing is shown"); if (getenv("DM_NVN_SHOT")) NLOG("[RED] DM_NVN_SHOT: no screenshots on the Vulkan backend yet"); }
    if (n >= STATS_FROM && (n == 60 || n == 600 || n % 1800 == 0 || (g_every && n % g_every == 0))) { surf_report("periodic"); if (g_vk) vkn_report("periodic"); }   // the count before this present: labelled with the frame just ended
    return n;
}
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {   // ticket 0 = nothing pending: the next acquire does not wait
    (void)period_s; uint64_t n = present_count();
    return g_vk && t ? vkn_present_enqueue(t->serial, t->d.width, t->d.height, crop, n == 1 || n == 60 || n == 600 || n % 1800 == 0) : 0;
}
uint64_t mtl_present_ext(uint64_t serial, double period, void *ev, uint64_t ev_val, const int crop[4]) { (void)serial; (void)period; (void)ev; (void)ev_val; (void)crop; present_count(); return 0; }   // no external producers (dm/gfx/vk is not built)
uint64_t mtl_present_count(void) { return atomic_load(&g_present_n); }
void mtl_present_wait(uint64_t ticket) { if (g_vk && ticket) vkn_present_wait(ticket); }
void mtl_present_flush(void) { if (g_vk) vkn_present_flush(); }
void mtl_present_discard(int on) { (void)on; }
void mtl_present_test_stall(uint64_t ns) { (void)ns; }
double mtl_presented_time(uint64_t n) { (void)n; return 0; }   // no present feedback yet: "not ready", so no frame counts as dropped
int mtl_display_hz(void) { return 0; }   // as the null backend: hle_nvn.c paces by its own clock (the display-link paths stay off until the pacer is ported, PLAN.md 2.1)
int mtl_has_window(void) { return 0; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; return 0; }
uint32_t mtl_present_qos(void) { return 0; }
int mtl_fps_target(void) {   // as nvn_mtl.m: DM_FPS=30 / 60 pins it, otherwise the game's own rate
    static int env = -2; if (env == -2) { const char *e = getenv("DM_FPS"); env = e ? atoi(e) : -1; if (env != 30 && env != 60) env = -1; else NLOG("DM_FPS=%d: frame rate pinned by the test switch", env); }
    return env > 0 ? env : nvn_game_fps() == 30 ? 30 : 60;
}
void mtl_set_frame(uint64_t f) { (void)f; }
void mtl_ub_present(double iv_ms) { (void)iv_ms; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
int mtl_warm_wait(int timeout_ms) { (void)timeout_ms; return 0; }   // no pipeline warm-up running
void *mtl_device(void) { return NULL; }
void *mtl_device_classic(void) { return NULL; }
uint64_t mtl_ext_tex(void *tex) { (void)tex; return 0; }

// ---------- Textures: host images for render targets (above); samplers and programs have no host objects yet ----------
void mtl_set_depth_zero(int on) { (void)on; }
void mtl_set_origin_lower_left(int on) { if (atomic_exchange(&g_origin_ll, on) != on) NLOG("window origin: %s", on ? "lower left (the present flips the picture)" : "upper left"); }
void mtl_set_tex_pool_n(int n) { (void)n; }
void mtl_tex_init(const tex_t *t) {   // a texture in a CPU-no-access pool (the render-target pools) gets its host image now; others when first set as a target
    mtl_init();
    if (!t || !t->serial) return;
    uint64_t s = t->serial; pthread_mutex_lock(&g_tsmu); nvn_hm_put(&g_tser, &s, (void *)1); pthread_mutex_unlock(&g_tsmu);   // live (without a device too: the statistics use it)
    if (!g_vk || !t->pool || !(t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) return;
    pthread_mutex_lock(&vkn_tmu); if (!vkn_img_get(t->serial)) img_make(t, NULL); pthread_mutex_unlock(&vkn_tmu);
}
void mtl_tex_release(tex_t *t) {   // out of the table now; destroyed once the submissions and presents that used it have completed
    if (!t || !t->serial) return;
    uint64_t s = t->serial; pthread_mutex_lock(&g_tsmu); nvn_hm_del(&g_tser, &s); pthread_mutex_unlock(&g_tsmu);   // not live: a record replayed later gets a ghost image (img_make)
    if (!g_vk) return;
    pthread_mutex_lock(&vkn_tmu);
    vkn_img **p = it_slot(t->serial), *e = *p;
    if (e) { *p = e->next; img_dead(e); g_nimg--; }
    vkn_sweep();
    pthread_mutex_unlock(&vkn_tmu);
}
void mtl_tex_cpu_written(const tex_t *t) { (void)t; }
int mtl_tex_gpu_owned(uint64_t serial) { if (!g_vk) return 0; pthread_mutex_lock(&vkn_tmu); int r = vkn_img_get(serial) != NULL; pthread_mutex_unlock(&vkn_tmu); return r; }
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) {   // counted only: the formats a texture table needs
    (void)id; if (!t) return;
    fmt_note(1, (unsigned)t->d.format); if (v && v->set_format) fmt_note(1, v->format);
    atomic_fetch_or_explicit(&g_tgt, 1u << (t->d.target & 31), memory_order_relaxed);
}
void mtl_reg_samp(int id, const samp_par *p) { (void)id; (void)p; }
void mtl_prog_release(prog_t *p) { (void)p; }
void bf_want_note(const char *key) { (void)key; }

// ReadTexels (row / image strides as hle_nvn.c read_texels worked them out), as the null backend: the region is read from guest memory (render targets: no readback yet, so their storage reads as the zeros the pool holds),
// de-laid-out as the Metal layer's CPU path reads textures the CPU filled (nvn_mtl.m mtl_read_texels); zeros only for a texture without storage or a layout it cannot undo
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh); if (!bpb) { NRED_ONCE("ReadTexels: format %#x unknown, nothing read", t->d.format); return; }
    size_t wb = (size_t)((r->width + bw - 1) / bw) * (size_t)bpb, hb = (size_t)((r->height + bh - 1) / bh), x0 = (size_t)(r->xoffset / bw) * (size_t)bpb, y0 = (size_t)(r->yoffset / bh);
    int nz = r->depth ? r->depth : 1, is3d = t->d.target == 2;
    uint32_t lw = t->d.width >> level, lh = t->d.height >> level, lz = is3d ? t->d.depth >> level : 1;
    if (!lw) lw = 1; if (!lh) lh = 1; if (!lz) lz = 1;
    size_t lp = (size_t)((lw + bw - 1) / bw) * (size_t)bpb, lr = (lh + bh - 1) / bh;
    ST(S_RTX, 1); ST(S_RTX_B, wb * hb * (size_t)nz); if (gpu_written(t->serial)) ST(S_RTX_GPU, 1);   // one of a texture the GPU writes would be a readback in a real backend
    uint8_t *lin = t->mem ? malloc(lp * lr * lz) : NULL;
    for (int z = 0; z < nz; z++) {
        const uint8_t *src = NULL;
        if (lin && ((is3d && z == 0) || !is3d) && nvn_deswizzle(&t->d, level, is3d ? 0 : layer0 + (unsigned)r->zoffset + (unsigned)z, t->mem, lin, lp)) { NRED_ONCE("ReadTexels: storage layout not undone, the region reads as zeros"); free(lin); lin = NULL; }
        if (lin) src = lin + (is3d ? (size_t)(r->zoffset + z) * lp * lr : 0);
        for (size_t y = 0; y < hb; y++) { uint8_t *d = dst + (size_t)z * img + y * row; if (src) memcpy(d, src + (y0 + y) * lp + x0, wb); else memset(d, 0, wb); }
    }
    free(lin);
}

// ---------- Statistics and diagnostics: the pass count; the rest has nothing to count yet ----------
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = atomic_load(&g_stat_pass); *draws = 0; }   // nothing is drawn yet
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = *reuse = 0; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = *skip = 0; }
void mtl_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip) { *resolve = *reuse = *set = *skip = 0; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { *load = *store = *full = 0; }
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = *reuploads = 0; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = *pso = 0; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = *pso_ns = 0; }
void mtl_alloc_stats(uint64_t *waits, uint64_t *grown, int *n) { *waits = *grown = 0; *n = 0; }
void mtl_barrier_stats(uint64_t *skip, uint64_t *full) { *skip = *full = 0; }
void mtl_ubd_stats(uint64_t *deferred, uint64_t *conflicts) { *deferred = *conflicts = 0; }
void mtl_ubp_stats(uint64_t *made, uint64_t *reused, uint64_t *refused) { *made = *reused = *refused = 0; }
void mtl_ubp_why(uint64_t w[5]) { memset(w, 0, 5 * sizeof *w); }
void mtl_lc_stats(uint64_t *taken, uint64_t *landed) { *taken = *landed = 0; }
uint64_t mtl_ds_omit(void) { return 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
uint64_t mtl_pkg_edge_skips(void) { return 0; }
uint64_t mtl_pkg_miss(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
uint64_t mtl_cut_count(void) { return 0; }
uint64_t mtl_copy_cut_count(void) { return 0; }
uint64_t mtl_blit_splits(void) { return 0; }
double mtl_gpu_ms(uint64_t f) { (void)f; return -1; }   // no GPU time to report
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; *t0 = *t1 = 0; return 0; }
void mtl_res_stats(char *out, size_t n) { if (n) snprintf(out, n, "-"); }
void mtl_res_counts(uint32_t tick, const char *why) { (void)tick; (void)why; }
void mtl_res_snapshot(mtl_res_count *out) { memset(out, 0, sizeof *out); }
void xv_test_counts(int *dc, int *viol) { if (dc) *dc = 0; if (viol) *viol = 0; }
// The memory readout line (hle_nvn.c, every 300 frames): resident memory only, with the Metal layer's throttle (a change of 64 MB or 3000 frames, at most 400 lines)
void mtl_mem_stat(unsigned long long frame, unsigned long long rss_mb) {
    static unsigned long long last_rss, last_frame; static int lines;
    unsigned long long dr = rss_mb > last_rss ? rss_mb - last_rss : last_rss - rss_mb;
    if (lines >= 400 || (lines && dr < 64 && frame - last_frame < 3000)) return;
    lines++; last_rss = rss_mb; last_frame = frame;
    NLOG("memory: frame %llu resident %llu MB (host images not counted here: the GPU work lines have them)", frame, rss_mb);
}
// DM_CTL screenshots and dumps (hle_hid.c): no pictures to take
void dm_gfx_shot_at(long k, const char *path) { (void)k; NRED_ONCE("screenshot %s: no pictures on the Vulkan backend yet", path); }
void dm_gfx_texdump_next(const char *path) { (void)path; NRED_ONCE("texture dump: no textures on the Vulkan backend yet"); }
void dm_gfx_passdump_next(long n) { (void)n; NRED_ONCE("pass dump: no passes on the Vulkan backend yet"); }
void dm_gfx_texlist(int target) { (void)target; NRED_ONCE("texture list: no textures on the Vulkan backend yet"); }
void dm_gfx_texraw(uint64_t serial, const char *path) { (void)serial; (void)path; NRED_ONCE("raw texture dump: no textures on the Vulkan backend yet"); }
