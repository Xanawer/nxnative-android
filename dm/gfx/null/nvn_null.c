// Null GPU backend (docs/android/PLAN.md 1.8): the mtl_* set hle_nvn.c calls (nvn_mtl.h, nvn_obj.h, the statistics externs and the hle_vk / hle_gl exports of nvn_mtl.m), with no GPU.
// Nothing is drawn: a submission runs only the commands whose effects the CPU can see (event values, counter reports, buffer copies / fills / uniform updates, sync fences) and completes
// at once, as if the GPU were infinitely fast; texture reads (ReadTexels) come from guest memory, which holds no rendering; presents only count frames. For CPU bring-up on Android and a Linux CI build; the symbol list is the template for the Vulkan backend.
// The ordering convention of nvn_mtl.h holds: a submission's event values and counter records are written before its fence counts as done, and waits look only at the done fence
#include "../nvn/nvn_obj.h"
#include "../nvn/nvn_mtl.h"
#include "../nvn/nvn_tex.h"
#include "../tex/nvn_deswizzle.h"
#include "../../host/dm.h"
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

#define NLOG(...) do { fprintf(stderr, "[dm] [nvn-null] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define NRED_ONCE(...) do { static _Atomic int said_; if (!atomic_exchange(&said_, 1)) NLOG("[RED] " __VA_ARGS__); } while (0)
int nvn_game_fps(void);   // hle_nvn.c

static void null_hello(void) { NLOG("null GPU backend: nothing is drawn; submissions complete at once (events, counters, buffer copies and fills run on the CPU), texture reads come from guest memory (nothing rendered lands there), presents only count frames"); }
void mtl_init(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, null_hello); }

// ---------- Memory pools: the address ranges only (a copy / fill / update outside every pool is refused, where the Metal layer dies: the CPU would write wherever the game pointed) ----------
typedef struct { uintptr_t lo, hi; const void *key; } pool_ent;
static pool_ent *g_pools; static int g_npool, g_cap, g_plast = -1;
static pthread_mutex_t g_pmu = PTHREAD_MUTEX_INITIALIZER;
void mtl_pool_add(const void *mem, uint64_t size) {
    mtl_init();
    uintptr_t pg = (uintptr_t)getpagesize(), lo = (uintptr_t)mem, hi = lo + size;   // expanded to page boundaries as the Metal layer's zero-copy buffers are, so mtl_addr answers the same
    pthread_mutex_lock(&g_pmu);
    if (g_npool == g_cap) { g_cap = g_cap ? g_cap * 2 : 64; g_pools = realloc(g_pools, (size_t)g_cap * sizeof *g_pools); if (!g_pools) die("null backend: pool table allocation failed"); }
    g_pools[g_npool++] = (pool_ent){lo & ~(pg - 1), (hi + pg - 1) & ~(pg - 1), mem};
    pthread_mutex_unlock(&g_pmu);
}
void mtl_pool_del(const void *mem) {
    pthread_mutex_lock(&g_pmu);
    for (int i = 0; i < g_npool; i++) if (g_pools[i].key == mem) { g_pools[i] = g_pools[--g_npool]; g_plast = -1; break; }
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
    return 1;
}

// ---------- Submission ----------
// Counter timestamps (19.2 MHz) from the guest clock, as the Metal layer's gpu_ticks: games compare them with svc GetSystemTick. Monotonic fallback where guest_ns is 0 (unit tests)
static uint64_t gpu_ticks(void) { uint64_t g = guest_ns(); if (!g) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); g = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; } return g * 12 / 625; }
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; } post_op;   // kind 1 event, 2 counter report
static post_op *g_post; static int g_npost, g_postcap;
static void post(post_op o) {
    if (g_npost == g_postcap) { g_postcap = g_postcap ? g_postcap * 2 : 64; g_post = realloc(g_post, (size_t)g_postcap * sizeof *g_post); if (!g_post) die("null backend: completion list allocation failed"); }
    g_post[g_npost++] = o;
}
static uint64_t g_fence, g_done;   // g_fence: last committed (submit thread); g_done: completed, under g_mu (any thread waits on it)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

void mtl_sub_begin(void) { mtl_init(); g_npost = 0; }
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
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); mtl_exec((const uint8_t *)(uintptr_t)t); break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); mtl_sub_event(val, m, v); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); mtl_sub_counter(ad, (uint32_t)t); break; }
            case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = g_fence + 1; s->fenced = 1; break; }   // the fence this submission commits with
            case OP_COPY_B2B: { uint64_t s_, d, n; memcpy(&s_, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); mtl_sub_copy(s_, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); sub_update(a + 16, d, n); break; }
            case OP_CLEAR_BUF: { const p_clear_buf *cb = (const p_clear_buf *)a; mtl_sub_fill(cb->addr, cb->size, cb->value); break; }
            default: break;   // drawing, binding, texture copies and clears: no GPU, nothing the CPU sees
        }
        p += h.size;
    }
}
uint64_t mtl_sub_commit(void) {
    uint64_t fence = ++g_fence, t = gpu_ticks();
    for (int i = 0; i < g_npost; i++) {
        post_op *o = &g_post[i];
        if (o->kind == 1) { if (o->mode == 1) *o->val += o->v; else *o->val = o->v; continue; }
        // A timestamp report's counter field holds the time (0 under render_cond, as on the GPU); samples passed: none were drawn; other counters: 0
        uint64_t rec[2] = {(o->type == NVN_COUNTER_TYPE_TIMESTAMP || o->type == NVN_COUNTER_TYPE_TIMESTAMP_TOP) && !DM_GAME_RENDER_COND ? t : 0, t};
        if (cpu_target(o->addr, sizeof rec, "counter report")) memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec);
    }
    g_npost = 0;
    pthread_mutex_lock(&g_mu); g_done = fence; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
    return fence;
}
uint64_t mtl_last_fence(void) { return g_fence; }
// A fence beyond the last commit (a sync object fenced by a submission still being replayed on another thread) is waited for, as with Metal
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t ns = (uint64_t)dl.tv_nsec + (timeout_ns > 3600000000000ull ? 3600000000000ull : timeout_ns);
    dl.tv_sec += (time_t)(ns / 1000000000ull); dl.tv_nsec = (long)(ns % 1000000000ull);
    pthread_mutex_lock(&g_mu);
    while (g_done < fence) if (pthread_cond_timedwait(&g_cv, &g_mu, &dl)) break;
    int ok = g_done >= fence;
    pthread_mutex_unlock(&g_mu);
    return ok;
}
void mtl_finish(void) {}   // every commit has completed by the time it returns
void mtl_drain(void) {}

// ---------- Present: counted, never shown; pacing stays with hle_nvn.c's own clock (no display link) ----------
extern _Atomic uint64_t dm_present_ns;   // hle_thread.c (DM_HITCH_DUMP): when the last present was made
static _Atomic uint64_t g_present_n;
static uint64_t present_count(void) {
    atomic_store(&dm_present_ns, now_ns());
    uint64_t n = atomic_fetch_add(&g_present_n, 1) + 1;
    // Not worded "first present": lognorm.py --until-present ends the parity cut at that phrase, and this line comes before the host's own (DM_DETCLOCK, [nvn] frame 1) that end it on the Metal backend
    if (n == 1) { NLOG("presenting: frames are counted, nothing is shown"); if (getenv("DM_NVN_SHOT")) NLOG("[RED] DM_NVN_SHOT: no pictures on the null backend"); }
    return n;
}
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) { (void)t; (void)period_s; (void)crop; present_count(); return 0; }   // ticket 0 = nothing pending: the next acquire does not wait
uint64_t mtl_present_ext(uint64_t serial, double period, void *ev, uint64_t ev_val, const int crop[4]) { (void)serial; (void)period; (void)ev; (void)ev_val; (void)crop; present_count(); return 0; }
uint64_t mtl_present_count(void) { return atomic_load(&g_present_n); }
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
void mtl_present_flush(void) {}
void mtl_present_discard(int on) { (void)on; }
void mtl_present_test_stall(uint64_t ns) { (void)ns; }
double mtl_presented_time(uint64_t n) { (void)n; return 0; }   // never presented on a screen: "not ready", so no frame counts as dropped
int mtl_display_hz(void) { return 0; }
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

// ---------- Textures, samplers, programs: no host objects ----------
void mtl_set_depth_zero(int on) { (void)on; }
void mtl_set_origin_lower_left(int on) { (void)on; }
void mtl_set_tex_pool_n(int n) { (void)n; }
void mtl_tex_init(const tex_t *t) { (void)t; }
void mtl_tex_release(tex_t *t) { (void)t; }
void mtl_tex_cpu_written(const tex_t *t) { (void)t; }
int mtl_tex_gpu_owned(uint64_t serial) { (void)serial; return 0; }
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) { (void)id; (void)t; (void)v; }
void mtl_reg_samp(int id, const samp_par *p) { (void)id; (void)p; }
void mtl_prog_release(prog_t *p) { (void)p; }
void bf_want_note(const char *key) { (void)key; }
// ReadTexels (row / image strides as hle_nvn.c read_texels worked them out): with no GPU, guest memory is the texture's whole content, so the region is read there,
// de-laid-out as the Metal layer's CPU path reads textures the CPU filled (nvn_mtl.m mtl_read_texels); zeros only for a texture without storage or a layout it cannot undo
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh); if (!bpb) { NRED_ONCE("ReadTexels: format %#x unknown, nothing read", t->d.format); return; }
    size_t wb = (size_t)((r->width + bw - 1) / bw) * (size_t)bpb, hb = (size_t)((r->height + bh - 1) / bh), x0 = (size_t)(r->xoffset / bw) * (size_t)bpb, y0 = (size_t)(r->yoffset / bh);
    int nz = r->depth ? r->depth : 1, is3d = t->d.target == 2;
    uint32_t lw = t->d.width >> level, lh = t->d.height >> level, lz = is3d ? t->d.depth >> level : 1;
    if (!lw) lw = 1; if (!lh) lh = 1; if (!lz) lz = 1;
    size_t lp = (size_t)((lw + bw - 1) / bw) * (size_t)bpb, lr = (lh + bh - 1) / bh;
    uint8_t *lin = t->mem ? malloc(lp * lr * lz) : NULL;
    for (int z = 0; z < nz; z++) {
        const uint8_t *src = NULL;
        if (lin && ((is3d && z == 0) || !is3d) && nvn_deswizzle(&t->d, level, is3d ? 0 : layer0 + (unsigned)r->zoffset + (unsigned)z, t->mem, lin, lp)) { NRED_ONCE("ReadTexels: storage layout not undone, the region reads as zeros"); free(lin); lin = NULL; }
        if (lin) src = lin + (is3d ? (size_t)(r->zoffset + z) * lp * lr : 0);
        for (size_t y = 0; y < hb; y++) { uint8_t *d = dst + (size_t)z * img + y * row; if (src) memcpy(d, src + (y0 + y) * lp + x0, wb); else memset(d, 0, wb); }
    }
    free(lin);
}

// ---------- Statistics and diagnostics: nothing to count ----------
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = *draws = 0; }
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
    NLOG("memory: frame %llu resident %llu MB (no GPU allocations)", frame, rss_mb);
}
// DM_CTL screenshots and dumps (hle_hid.c): no pictures to take
void dm_gfx_shot_at(long k, const char *path) { (void)k; NRED_ONCE("screenshot %s: no pictures on the null backend", path); }
void dm_gfx_texdump_next(const char *path) { (void)path; NRED_ONCE("texture dump: no textures on the null backend"); }
void dm_gfx_passdump_next(long n) { (void)n; NRED_ONCE("pass dump: no passes on the null backend"); }
void dm_gfx_texlist(int target) { (void)target; NRED_ONCE("texture list: no textures on the null backend"); }
void dm_gfx_texraw(uint64_t serial, const char *path) { (void)serial; (void)path; NRED_ONCE("raw texture dump: no textures on the null backend"); }
