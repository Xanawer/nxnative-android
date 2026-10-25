// Null GPU backend (docs/android/PLAN.md 1.8): the mtl_* set hle_nvn.c calls (nvn_mtl.h, nvn_obj.h, the statistics externs and the hle_vk / hle_gl exports of nvn_mtl.m), with no GPU.
// Nothing is drawn: a submission runs only the commands whose effects the CPU can see (event values, counter reports, buffer copies / fills / uniform updates, sync fences) and completes
// at once, as if the GPU were infinitely fast; texture reads (ReadTexels) come from guest memory, which holds no rendering; presents only count frames. For CPU bring-up on Android and a Linux CI build; the symbol list is the template for the Vulkan backend.
// The ordering convention of nvn_mtl.h holds: a submission's event values and counter records are written before its fence counts as done, and waits look only at the done fence
#include "../nvn/nvn_obj.h"
#include "../nvn/nvn_mtl.h"
#include "../nvn/nvn_tex.h"
#include "../nvn/nvn_dirty.h"
#include "../tex/nvn_deswizzle.h"
#include "../../host/dm.h"
#include <errno.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define NLOG(...) do { fprintf(stderr, "[dm] [nvn-null] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define NRED_ONCE(...) do { static _Atomic int said_; if (!atomic_exchange(&said_, 1)) NLOG("[RED] " __VA_ARGS__); } while (0)
int nvn_game_fps(void);   // hle_nvn.c

// ---------- Surface statistics: what the game asks of a GPU backend (the Vulkan backend's work list, PLAN.md Phase 2), counted where the records are walked anyway ----------
// Cumulative; one [nvn-null] line at presents 60, 600 and every 1800, every DM_NVN_NULL_EVERY=<n> presents as well (to tell a run's phases apart: a phase is the difference of two lines),
// and at mtl_drain (graceful exit, or the app sent to the background). Never before present 60: the parity cut (lognorm.py, parity.py) ends at the first present, and these lines are backend-tagged
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
// Textures the GPU writes (render targets, copy and clear destinations), by serial: a ReadTexels of one is a GPU → CPU readback in a real backend, of any other the CPU's own bytes.
// Open addressing under g_gwtmu; a released texture leaves (mtl_tex_release), so the set holds the live ones only. g_gwlast: the last one added, passed over without the lock (submit thread)
static uint64_t *g_gwt; static size_t g_gwtcap, g_gwtn; static _Atomic uint64_t g_gwlast; static pthread_mutex_t g_gwtmu = PTHREAD_MUTEX_INITIALIZER;
static size_t gwt_home(uint64_t s) { return (size_t)(s * 0x9e3779b97f4a7c15ull) & (g_gwtcap - 1); }
static size_t gwt_slot(uint64_t s) { size_t i = gwt_home(s); while (g_gwt[i] && g_gwt[i] != s) i = (i + 1) & (g_gwtcap - 1); return i; }
static void gpu_wrote(const tex_t *t) {
    if (!t || !t->serial || t->serial == atomic_load_explicit(&g_gwlast, memory_order_relaxed)) return;   // the same few targets come back every pass
    pthread_mutex_lock(&g_gwtmu);
    if (2 * (g_gwtn + 1) > g_gwtcap) { uint64_t *o = g_gwt; size_t oc = g_gwtcap; g_gwtcap = oc ? oc * 2 : 256; g_gwt = calloc(g_gwtcap, sizeof *g_gwt); if (!g_gwt) die("null backend: texture set allocation failed");
        for (size_t i = 0; i < oc; i++) if (o[i]) g_gwt[gwt_slot(o[i])] = o[i]; free(o); }
    size_t i = gwt_slot(t->serial); if (!g_gwt[i]) { g_gwt[i] = t->serial; g_gwtn++; }
    atomic_store_explicit(&g_gwlast, t->serial, memory_order_relaxed); pthread_mutex_unlock(&g_gwtmu);
}
static void gpu_wrote_drop(uint64_t s) {   // backward-shift deletion: each later entry of the probe run moves into the hole when its home slot does not lie after it (no tombstones)
    if (!s) return;
    pthread_mutex_lock(&g_gwtmu);
    uint64_t l = s; atomic_compare_exchange_strong_explicit(&g_gwlast, &l, 0, memory_order_relaxed, memory_order_relaxed);
    size_t m = g_gwtcap - 1, i = g_gwtcap ? gwt_slot(s) : 0;
    if (g_gwtcap && g_gwt[i]) {
        g_gwt[i] = 0; g_gwtn--;
        for (size_t j = (i + 1) & m; g_gwt[j]; j = (j + 1) & m) if (((j - gwt_home(g_gwt[j])) & m) >= ((j - i) & m)) { g_gwt[i] = g_gwt[j]; g_gwt[j] = 0; i = j; }
    }
    pthread_mutex_unlock(&g_gwtmu);
}
static int gpu_written(uint64_t s) { pthread_mutex_lock(&g_gwtmu); int r = s && g_gwtcap && g_gwt[gwt_slot(s)] == s; pthread_mutex_unlock(&g_gwtmu); return r; }
static uint64_t region_bytes(const tex_t *t, const NVNcopyRegion *r) { int bw, bh, bpb = t ? nvn_fmt_bpb(t->d.format, &bw, &bh) : 0; return bpb ? (uint64_t)((r->width + bw - 1) / bw) * (uint64_t)((r->height + bh - 1) / bh) * (uint64_t)(r->depth > 1 ? r->depth : 1) * (uint64_t)bpb : 0; }
static _Atomic uint64_t g_present_n;   // presents so far = hle_nvn.c's frame index (init = 0), the frame things below are labelled with
static uint64_t g_every;               // DM_NVN_NULL_EVERY

// ---------- Strategy B simulation (DM_NVN_NULL_BSIM=<tsv path>, a measurement switch): what shadow VkBuffers kept current by write tracking would cost (PLAN.md 2.2 B) ----------
// At every submission the pages the GPU reads as buffers (vertex, index, uniform, storage, indirect arguments, copy sources: PF_BUF) and texture storage (PF_TEX) are write-protected, as a backend
// that uploads dirty pages into its shadows before replaying would; command memory never is (the host replays it from guest memory, no GPU reads it). Per submission it counts the pages written
// since the previous one, by class, at the host's 4 KB and as 16 / 64 KB granules (the Mac's page, P0.7's placed-memory tracking unit), and writes one TSV row. The null backend's own CPU work
// for GPU ops (copies, fills, uniform updates, counters, events) is a GPU write under B, not an upload: its pages are protected again right after it and only a later write counts
enum { PF_BUF = 1, PF_TEX = 2, PF_CMD = 4, PF_NEW = 8, PF_DUE = 16 };   // NEW: classified since the last submission; DUE: to protect in this one
static int g_bsim; static FILE *g_btsv; static uintptr_t g_pgsz, g_sh;
typedef struct { uint64_t a, n; } gw_rng;
static gw_rng *g_gw; static int g_ngw, g_gwcap;   // the GPU-op writes of the submission being replayed (submit thread)
static uint64_t g_stamp_b, g_stamp_c;             // dirty generation at the last epoch's start / at the last commit, before its GPU-op pages were protected again
static uint64_t g_bs[12];   // cumulative: 0 submissions, 1-3 buffer pages / 16 KB / 64 KB, 4-6 texture the same, 7 GPU-op pages, 8 simulation ns, 9 max buffer pages in one submission, 10-11 tracked buffer / texture pages now
static void gw_note(uint64_t a, uint64_t n) {
    if (!g_bsim || !n) return;
    if (g_ngw == g_gwcap) { g_gwcap = g_gwcap ? g_gwcap * 2 : 64; g_gw = realloc(g_gw, (size_t)g_gwcap * sizeof *g_gw); if (!g_gw) die("null backend: B simulation list allocation failed"); }
    g_gw[g_ngw++] = (gw_rng){a, n};
}

static void null_hello(void) {
    NLOG("null GPU backend: nothing is drawn; submissions complete at once (events, counters, buffer copies and fills run on the CPU), texture reads come from guest memory (nothing rendered lands there), presents only count frames");
    const char *e = getenv("DM_NVN_NULL_EVERY"); g_every = e ? strtoull(e, NULL, 10) : 0;
    g_pgsz = (uintptr_t)getpagesize(); g_sh = (uintptr_t)__builtin_ctzl(g_pgsz);
    const char *b = getenv("DM_NVN_NULL_BSIM"); g_bsim = b && *b;   // said in the surface lines only: a line here would land before the parity cut
    if (g_bsim && strcmp(b, "1") && (g_btsv = fopen(b, "w"))) { setvbuf(g_btsv, NULL, _IOLBF, 0); fprintf(g_btsv, "present\tbuf_4k\tbuf_16k\tbuf_64k\ttex_4k\ttex_16k\ttex_64k\tgpuop_pages\tfaults\tpreps\tmprotect_calls\tmprotect_pages\tmprotect_us\tsim_us\ttracked_buf_pages\ttracked_tex_pages\n"); }
}
void mtl_init(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, null_hello); }

// ---------- Memory pools: the address ranges only (a copy / fill / update outside every pool is refused, where the Metal layer dies: the CPU would write wherever the game pointed) ----------
typedef struct { uintptr_t lo, hi; const void *key; uint8_t *pf; const void *span; } pool_ent;   // pf, span: the B simulation's page classes and the pool's tracked range
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
    if (g_npool == g_cap) { g_cap = g_cap ? g_cap * 2 : 64; g_pools = realloc(g_pools, (size_t)g_cap * sizeof *g_pools); if (!g_pools) die("null backend: pool table allocation failed"); }
    pool_ent e = {lo & ~(pg - 1), (hi + pg - 1) & ~(pg - 1), mem, NULL, NULL};
    if (g_bsim && !(e.pf = calloc((e.hi - e.lo) / pg, 1))) die("null backend: B simulation page table allocation failed");
    g_pools[g_npool++] = e;
    if (g_nplog == g_plogcap && g_nplog >= 4096) {   // bounded: sparse pools re-add their runs at every remap (vrun_set), so freed entries go, listed or not (the totals keep them)
        int k = 0; for (int i = 0; i < g_nplog; i++) if (g_plog[i].died == ~0ull) g_plog[k++] = g_plog[i];
        g_nplog = k;
    }
    if (g_nplog == g_plogcap) { g_plogcap = g_plogcap ? g_plogcap * 2 : 64; g_plog = realloc(g_plog, (size_t)g_plogcap * sizeof *g_plog); if (!g_plog) die("null backend: pool log allocation failed"); }
    g_plog[g_nplog++] = (pool_log){lo, size, atomic_load(&g_present_n), ~0ull, g_pmade, 0};
    g_pmade++; g_pmade_b += size; g_plive_b += size; if (g_plive_b > g_ppeak_b) g_ppeak_b = g_plive_b;
    pthread_mutex_unlock(&g_pmu);
}
void mtl_pool_del(const void *mem) {
    pthread_mutex_lock(&g_pmu);
    for (int i = 0; i < g_npool; i++) if (g_pools[i].key == mem) {
        pool_ent *e = &g_pools[i];
        // nvn_dirty_remove_range ran just before (hle_nvn.c) and released the pool's read-only pages, but an epoch or commit protecting it then can take pages that pass had already gone by;
        // the range is dead, so nothing would release those (a write there reaches the crash handler). Both protect under g_pmu and cache the span first (bsim_protect), so none runs now and
        // a pool they may have protected has one: other ranges' pages first, then all writable
        if (e->pf && e->span) {
            dm_guest_write_prepare((void *)e->lo, e->hi - e->lo);
            if (mprotect((void *)e->lo, e->hi - e->lo, PROT_READ | PROT_WRITE)) NRED_ONCE("B simulation: a freed pool's pages could not be made writable again (errno %d)", errno);
        }
        free(e->pf); *e = g_pools[--g_npool]; g_plast = -1; break;
    }
    for (int i = g_nplog - 1; i >= 0; i--) if (g_plog[i].mem == (uintptr_t)mem && g_plog[i].died == ~0ull) { g_plog[i].died = atomic_load(&g_present_n); g_pfreed++; g_pfreed_b += g_plog[i].size; g_plive_b -= g_plog[i].size; break; }
    pthread_mutex_unlock(&g_pmu);
}
// The B simulation's page classes: the pages of [addr, addr+len) in any pool get flag (a range outside every pool is no GPU memory: nothing to track)
static void bsim_mark(uint64_t addr, uint64_t len, uint8_t flag) {
    if (!g_bsim || !len) return;
    uint64_t end = addr + len < addr ? UINT64_MAX : addr + len;
    pthread_mutex_lock(&g_pmu);
    for (int k = 0; k < g_npool; k++) {
        pool_ent *e = &g_pools[k]; uint64_t lo = addr > e->lo ? addr : e->lo, hi = end < e->hi ? end : e->hi;
        if (!e->pf || lo >= hi) continue;
        for (uint64_t i = (lo - e->lo) >> g_sh, j = (hi - 1 - e->lo) >> g_sh; i <= j; i++) if (!(e->pf[i] & flag)) e->pf[i] |= flag | PF_NEW;   // PF_NEW: protect at the next submission
    }
    pthread_mutex_unlock(&g_pmu);
}
static void bsim_cmd(const uint8_t *p, uint32_t n) {   // a record's pages hold command memory (submit thread; records follow each other, so only a page change looks the pool up)
    static uintptr_t last = ~(uintptr_t)0; uintptr_t a = (uintptr_t)p >> g_sh, b = ((uintptr_t)p + (n ? n : 1) - 1) >> g_sh;
    if (a == last && b == last) return;
    last = b; bsim_mark((uint64_t)a << g_sh, (uint64_t)(b - a + 1) << g_sh, PF_CMD);
}
static int bsim_eligible(uint8_t f) { return (f & (PF_BUF | PF_TEX)) && !(f & PF_CMD); }
static void bsim_protect(pool_ent *e, uintptr_t i, uintptr_t n, int due) {   // protect pages i..n-1 of a pool that are eligible (and PF_DUE if due, which it clears), one call per run
    // The span is cached before any protect, the commit's included (a pool made after the epoch has none yet): mtl_pool_del's clean-up keys on it. Untracked: nothing would be protected
    if (!e->span) e->span = nvn_dirty_span((const void *)e->lo, e->hi - e->lo);
    if (!e->span) return;
    while (i < n) {
        if (!bsim_eligible(e->pf[i]) || (due && !(e->pf[i] & PF_DUE))) { i++; continue; }
        uintptr_t j = i; while (j < n && bsim_eligible(e->pf[j]) && (!due || (e->pf[j] & PF_DUE))) e->pf[j++] &= (uint8_t)~PF_DUE;
        nvn_dirty_protect((const void *)(e->lo + (i << g_sh)), (size_t)(j - i) << g_sh); i = j;
    }
}
static int gw_has(uintptr_t pa) { for (int i = 0; i < g_ngw; i++) if (pa + g_pgsz > g_gw[i].a && pa < g_gw[i].a + g_gw[i].n) return 1; return 0; }
// Submission start: count what was written since the last one and protect those pages (and newly classified ones) again. The stamp is taken first, so a page unprotected while this runs
// counts (and is protected) at the next submission; a page written between the scan and its protect lands before the upload a real backend would make after protecting
static void bsim_epoch(void) {
    uint64_t t0 = now_ns(), stamp = nvn_dirty_now(), d[6] = {0}, trk[2] = {0};
    uintptr_t cpg = g_pgsz < 65536 ? 65536 >> g_sh : 1;   // pages per 64 KB: a chunk not written since the stamp is passed over with one check
    pthread_mutex_lock(&g_pmu);
    for (int k = 0; k < g_npool; k++) {
        pool_ent *e = &g_pools[k];
        if (!e->pf) continue;
        if (!e->span) e->span = nvn_dirty_span((const void *)e->lo, e->hi - e->lo);
        if (!e->span) continue;   // untracked (the tracker's table is full): it would count as always written
        uintptr_t np = (e->hi - e->lo) >> g_sh, l16[2] = {~(uintptr_t)0, ~(uintptr_t)0}, l64[2] = {~(uintptr_t)0, ~(uintptr_t)0};
        for (uintptr_t c0 = 0; c0 < np; c0 += cpg) {
            uintptr_t a = c0, z = c0 + cpg < np ? c0 + cpg : np;
            while (a < z && !bsim_eligible(e->pf[a])) a++;
            if (a == z) continue;
            while (!bsim_eligible(e->pf[z - 1])) z--;
            int any = nvn_dirty_since_span(e->span, (const void *)(e->lo + (a << g_sh)), (size_t)(z - a) << g_sh, g_stamp_b);
            for (uintptr_t i = a; i < z; i++) {
                uint8_t f = e->pf[i]; if (!bsim_eligible(f)) continue;
                int c = !(f & PF_BUF); uintptr_t pa = e->lo + (i << g_sh); trk[c]++;
                if (f & PF_NEW) { e->pf[i] = (uint8_t)((f & ~PF_NEW) | PF_DUE); continue; }
                if (!any || !nvn_dirty_since_span(e->span, (const void *)pa, g_pgsz, g_stamp_b)) continue;
                if (g_ngw && gw_has(pa) && !nvn_dirty_since_span(e->span, (const void *)pa, g_pgsz, g_stamp_c)) continue;   // written only by the GPU-op work of the last submission
                e->pf[i] |= PF_DUE;
                d[3 * c]++; if (pa >> 14 != l16[c]) { l16[c] = pa >> 14; d[3 * c + 1]++; } if (pa >> 16 != l64[c]) { l64[c] = pa >> 16; d[3 * c + 2]++; }
            }
        }
        bsim_protect(e, 0, np, 1);
    }
    pthread_mutex_unlock(&g_pmu);
    g_ngw = 0; g_stamp_b = stamp;
    uint64_t ns = now_ns() - t0; g_bs[0]++; for (int i = 0; i < 6; i++) g_bs[1 + i] += d[i]; g_bs[8] += ns; if (d[0] > g_bs[9]) g_bs[9] = d[0]; g_bs[10] = trk[0]; g_bs[11] = trk[1];
    static uint64_t pfa, ppr, pmc, pmp, pmn, pgw; uint64_t fa, pr, mc, mp, mn; nvn_dirty_stats(&fa, &pr); nvn_dirty_mp_stats(&mc, &mp, &mn);
    if (g_btsv) fprintf(g_btsv, "%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%.1f\t%.1f\t%llu\t%llu\n", (unsigned long long)atomic_load(&g_present_n),
        (unsigned long long)d[0], (unsigned long long)d[1], (unsigned long long)d[2], (unsigned long long)d[3], (unsigned long long)d[4], (unsigned long long)d[5], (unsigned long long)(g_bs[7] - pgw),
        (unsigned long long)(fa - pfa), (unsigned long long)(pr - ppr), (unsigned long long)(mc - pmc), (unsigned long long)(mp - pmp), (mn - pmn) / 1e3, ns / 1e3, (unsigned long long)trk[0], (unsigned long long)trk[1]);
    pfa = fa; ppr = pr; pmc = mc; pmp = mp; pmn = mn; pgw = g_bs[7];   // a row's faults and mprotect time are those since the last submission started, this simulation's protect included
}
// Submission end: the GPU-op pages written by the CPU here are protected again, so that only a later write marks them. The stamp comes first: a guest write that faults right after
// a page's protect is newer than it, so the next epoch counts it and protects the page again (taken after, it would pass as GPU-op work and the page would stay writable for good)
static void bsim_commit(void) {
    if (!g_ngw) return;
    g_stamp_c = nvn_dirty_now();
    pthread_mutex_lock(&g_pmu);
    for (int i = 0; i < g_ngw; i++) for (int k = 0; k < g_npool; k++) {
        pool_ent *e = &g_pools[k]; uint64_t a = g_gw[i].a, z = a + g_gw[i].n, lo = a > e->lo ? a : e->lo, hi = z < e->hi ? z : e->hi;
        if (!e->pf || lo >= hi) continue;
        lo &= ~(uint64_t)(g_pgsz - 1); hi = (hi + g_pgsz - 1) & ~(uint64_t)(g_pgsz - 1);
        for (uint64_t pa = lo; pa < hi; pa += g_pgsz) g_bs[7] += bsim_eligible(e->pf[(pa - e->lo) >> g_sh]);
        bsim_protect(e, (uintptr_t)(lo - e->lo) >> g_sh, (uintptr_t)(hi - e->lo) >> g_sh, 0);
    }
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
    ST(S_GW_B, n); gw_note(addr, n);   // a GPU write into pool memory: what a backend would have to bring back if the CPU reads it
    return 1;
}

// One record's share of the surface statistics (and, under the B simulation, of the page classes). g_pass_drawn: a clear before the pass's first draw folds into its load action
static int g_pass_drawn;
static void surf_rec(const op_hdr *h, const uint8_t *p, const uint8_t *a) {
    if (h->op < OP_COUNT_) ST(h->op, 1);
    if (g_bsim) bsim_cmd(p, h->size);
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
            bsim_mark(d->ind, d->ind ? 16 : 0, PF_BUF); bsim_mark(d->mdi_cnt, d->mdi_cnt ? 4 : 0, PF_BUF);
            break; }
        case OP_DRAW_IDX: { p_draw_idx t; const p_draw_idx *d = draw_idx_rec(a, h->size, &t); int it = d->itype >= 0 && d->itype < 3 ? d->itype : 2; g_pass_drawn = 1;
            ST(S_PRIM + (d->prim & 15), 1); ST(S_ITYPE + it, 1); ST(S_DIDX_IDX, d->count > 0 ? d->count : 0);
            if (d->instances > 1) ST(S_DIDX_INST, 1); if (d->base_vertex) ST(S_DIDX_BVTX, 1); if (d->base_instance) ST(S_DIDX_BINST, 1); if (d->ind) ST(S_DIDX_IND, 1); if (d->mdi_cnt) ST(S_DIDX_MDI, 1);
            bsim_mark(d->ibuf, d->count > 0 ? (uint64_t)d->count << it : 0, PF_BUF); bsim_mark(d->ind, d->ind ? 20 : 0, PF_BUF); bsim_mark(d->mdi_cnt, d->mdi_cnt ? 4 : 0, PF_BUF);
            break; }
        case OP_DISPATCH: { const p_dispatch *d = (const p_dispatch *)a; if (d->ind) ST(S_DISP_IND, 1); bsim_mark(d->ind, d->ind ? 12 : 0, PF_BUF); break; }
        case OP_COPY_B2B: { uint64_t s_, n; memcpy(&s_, a, 8); memcpy(&n, a + 16, 8); ST(S_B2B_B, n); bsim_mark(s_, n, PF_BUF); break; }
        case OP_UPDATE_UB: { uint64_t n; memcpy(&n, a + 8, 8); ST(S_UB_B, n); break; }
        case OP_CLEAR_BUF: ST(S_FILL_B, ((const p_clear_buf *)a)->size); break;
        case OP_COPY_B2T: { const p_copy_b2t *c = (const p_copy_b2t *)a; uint64_t n = region_bytes(c->tex, &c->r); ST(S_B2T_B, n); gpu_wrote(c->tex); bsim_mark(c->src, n, PF_BUF); break; }
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
        case OP_BIND_VBUF: { const p_bind_vbuf *v = (const p_bind_vbuf *)a; for (int i = 0; i < v->n && i < 16; i++) bsim_mark(v->r[i].address, v->r[i].size, PF_BUF); break; }
        case OP_BIND_UBO: case OP_BIND_SSBO: { const p_bind_buf *u = (const p_bind_buf *)a; bsim_mark(u->addr, u->size, PF_BUF); break; }
        default: break;
    }
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

void mtl_sub_begin(void) { mtl_init(); g_npost = 0; if (g_bsim) bsim_epoch(); }
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
        if (o->kind == 1) { gw_note((uint64_t)(uintptr_t)o->val, 4); if (o->mode == 1) *o->val += o->v; else *o->val = o->v; continue; }
        // A timestamp report's counter field holds the time (0 under render_cond, as on the GPU); samples passed: none were drawn; other counters: 0
        uint64_t rec[2] = {(o->type == NVN_COUNTER_TYPE_TIMESTAMP || o->type == NVN_COUNTER_TYPE_TIMESTAMP_TOP) && !DM_GAME_RENDER_COND ? t : 0, t};
        if (cpu_target(o->addr, sizeof rec, "counter report")) memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec);
    }
    g_npost = 0;
    if (g_bsim) bsim_commit();
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
static void surf_report(const char *why);
void mtl_drain(void) { if (atomic_load(&g_present_n) >= 60) surf_report("graphics suspended: exit, or the app in the background"); }   // the last line of a run (the graceful exit ends in _exit, no atexit); from present 60 on, as every report

// ---------- The surface lines (see the statistics at the top): one line of counts, the pools made or freed since the last one, and the B simulation's totals when it runs ----------
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
    if (g_bsim) {   // the VMA count: B splits ordinary memory, which merges back when protected again (unlike KGSL's, P0.7)
        unsigned long long vmas = 0; FILE *m = fopen("/proc/self/maps", "r"); if (m) { int c; while ((c = fgetc(m)) != EOF) vmas += c == '\n'; fclose(m); }
        uint64_t fa, pr, mc, mp, mn; nvn_dirty_stats(&fa, &pr); nvn_dirty_mp_stats(&mc, &mp, &mn);
        NLOG("B simulation up to frame %llu: %llu submissions; pages written since the previous submission, buffers %llu (16 KB granules %llu, 64 KB %llu; at most %llu in one), texture storage %llu (%llu, %llu); "
             "GPU-op pages protected again %llu; tracked now %llu buffer pages, %llu texture pages (%llu B page); write faults %llu, host pre-writes %llu; mprotect %llu calls, %llu pages, %.1f ms; simulation %.1f ms; VMAs %llu",
             n, (unsigned long long)g_bs[0], (unsigned long long)g_bs[1], (unsigned long long)g_bs[2], (unsigned long long)g_bs[3], (unsigned long long)g_bs[9], (unsigned long long)g_bs[4], (unsigned long long)g_bs[5],
             (unsigned long long)g_bs[6], (unsigned long long)g_bs[7], (unsigned long long)g_bs[10], (unsigned long long)g_bs[11], (unsigned long long)g_pgsz, (unsigned long long)fa, (unsigned long long)pr,
             (unsigned long long)mc, (unsigned long long)mp, mn / 1e6, g_bs[8] / 1e6, vmas);
        if (g_btsv) fflush(g_btsv);
    }
#undef AP
    pthread_mutex_unlock(&mu);
}

// ---------- Present: counted, never shown; pacing stays with hle_nvn.c's own clock (no display link) ----------
extern _Atomic uint64_t dm_present_ns;   // hle_thread.c (DM_HITCH_DUMP): when the last present was made
static uint64_t present_count(void) {
    mtl_init();   // DM_NVN_NULL_EVERY
    atomic_store(&dm_present_ns, now_ns());
    uint64_t n = atomic_fetch_add(&g_present_n, 1) + 1;
    // Not worded "first present": lognorm.py --until-present ends the parity cut at that phrase, and this line comes before the host's own (DM_DETCLOCK, [nvn] frame 1) that end it on the Metal backend
    if (n == 1) { NLOG("presenting: frames are counted, nothing is shown"); if (getenv("DM_NVN_SHOT")) NLOG("[RED] DM_NVN_SHOT: no pictures on the null backend"); }
    // The count before this present: the lines are labelled with the frame just ended. None before present 60 (DM_NVN_NULL_EVERY=1 included), well past the parity cut
    if (n >= 60 && (n == 60 || n == 600 || n % 1800 == 0 || (g_every && n % g_every == 0))) surf_report("periodic");
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
void mtl_tex_release(tex_t *t) { if (t) gpu_wrote_drop(t->serial); }
void mtl_tex_cpu_written(const tex_t *t) { (void)t; }
int mtl_tex_gpu_owned(uint64_t serial) { (void)serial; return 0; }
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) {   // counted only: the formats a texture table needs, and (B simulation) the storage a re-upload would read
    (void)id; if (!t) return;
    fmt_note(1, (unsigned)t->d.format); if (v && v->set_format) fmt_note(1, v->format);
    atomic_fetch_or_explicit(&g_tgt, 1u << (t->d.target & 31), memory_order_relaxed);
    if (t->mem) bsim_mark((uint64_t)(uintptr_t)t->mem, t->L.size, PF_TEX);
}
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
