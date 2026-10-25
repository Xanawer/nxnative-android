// Write-protect dirty-page tracking for guest memory: pool pages of packaged textures (CPU writes, GPU reads) are set read-only after upload, and as soon as the game writes it faults -> mark dirty, unprotect;
// before use, check the write generation of the covered pages and re-upload if newer than at upload time. NVN semantics: a texture descriptor points to pool memory, and the GPU reads the current content every time
// (measured: the game writes streamed assets into texture storage that is still being sampled, without re-registering).
// Signal handler constraint: the page-fault path uses only atomic operations and mprotect, no allocation, no locks, no stdio (its red lines are rline's: one write(2)).
#include "nvn_dirty.h"
#include "../../host/dm.h"
#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif
#include <sched.h>

// Order of page state and mprotect: the side changing protection first sets the state to BUSY and sets the final state only after mprotect has landed; other threads wait when they see BUSY.
//   set read-only: NONE -> BUSY -> mprotect(read-only) -> PROT; unprotect: PROT -> BUSY -> mprotect(writable) -> NONE.
// Invariant: a page whose state is NONE is always writable, one whose state is PROT is always read-only. Setting read-only used to be "record PROT, then mprotect"; in between, another thread's unprotect-before-write
// (the movie decoder thread writing a frame into texture storage, a disk read into a guest buffer) could set the state back to NONE, then the mprotect landed => the page read-only with state NONE: a game thread writing it got only
// "re-execute" from the fault handler and spun forever; a host system call reading into it saw NONE in prep_write, skipped, and failed with EFAULT (reproduced by t_dirty_race).
// BUSY lasts at most one mprotect (plus a few atomic writes when setting read-only). The wait is capped at BUSY_WAIT_MS and reports red beyond it: in the fault handler it then returns "not our page" so the crash handler reports truthfully
enum { PG_NONE = 0, PG_PROT = 1, PG_BUSY = 2 };
enum { BUSY_WAIT_MS = 2000 };
// Tracked ranges = the memory pools (registered by MemoryPoolInitialize). After a pool is destroyed its live=0 slot is reused; the signal handler scans without locks.
// The table grows in blocks of 1,024 slots, up to 262,144; a block is never moved or freed, so the signal handler reads it without locks. A pool past the end goes untracked: its textures count
// as dirty and are re-uploaded at every use. The table was a fixed 256 slots (Zelda has about 300 pools at the title), then a fixed 4,096, and Breath of the Wild reaches 4,006 pools in its first
// shrine. The scans only cover slots in use, so the capacity itself costs nothing
enum { BLK_SHIFT = 10, BLK = 1 << BLK_SHIFT, NBLK = 256, MAX_RANGE = BLK * NBLK };
// Ranges are rounded to host pages (16 KB) but guest memory pools are only 4 KB aligned: adjacent pools share their first / last pages, so one address can fall in several registered ranges.
// So: "is this texture tracked" looks for a range that holds its whole span; a fault / host pre-write unprotects and stamps every range covering that page; pool destruction identifies its range by the pool's exact start mem.
// (Previously the first range containing the address was always taken: two SMBW ASTC 6x5 textures whose pool's first page is shared with a small preceding pool were judged untracked and re-uploaded on every use, about 6 MB per frame)
typedef struct { uintptr_t base, end, mem; _Atomic uint8_t *state; _Atomic uint64_t *gen; size_t cap; _Atomic int live, bad; } range_t;   // bad: counts as always written (protect_failed, mark_bad)
static _Atomic(range_t *) g_blk[NBLK];
static inline range_t *rg(int i) { return &atomic_load_explicit(&g_blk[i >> BLK_SHIFT], memory_order_acquire)[i & (BLK - 1)]; }
static _Atomic int g_nr;
static uintptr_t g_pg, g_shift;
static _Atomic uint64_t g_now = 1;
static _Atomic int g_nbad;   // ranges that count as always written (protect_failed, mark_bad); nvn_dirty_now moves on every read while there is one
static _Atomic uint64_t g_faults, g_preps;
static _Atomic uint64_t g_adds_total;
static _Atomic int g_live, g_live_peak;   // number of registered pools and peak (for the memory readout line)
void nvn_dirty_stat(int *live, int *peak, int *cap) { *live = atomic_load(&g_live); *peak = atomic_load(&g_live_peak); *cap = MAX_RANGE; }
static _Atomic int g_overflow_logged;
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;   // Pool creation / destruction (guest threads, possibly concurrent) pick slots under a mutex; the page-fault handling path does not touch it
static _Atomic uint64_t g_mp_calls, g_mp_pages, g_mp_ticks;   // This module's mprotect: call count, pages covered, time spent (mach timebase, safe in the page-fault path too)
static _Atomic int g_mp_red;
// The fault path's red lines (mp, busy_wait): LOG's fprintf takes stderr's lock, which another thread may hold (or was stopped holding) while this page stays BUSY, so the line LOG would print
// is built here by hand and goes out in one write(2)
typedef struct { char b[320]; size_t n; } rline;
static void rl_s(rline *l, const char *s) { while (*s && l->n < sizeof l->b - 1) l->b[l->n++] = *s++; }
static void rl_n(rline *l, uint64_t v, int hex) {   // as %d / %#zx / %p print v (never 0 in hex here)
    char t[24]; int k = 0; unsigned b = hex ? 16 : 10;
    do t[k++] = "0123456789abcdef"[v % b]; while ((v /= b));
    if (hex) rl_s(l, "0x");
    while (k && l->n < sizeof l->b - 1) l->b[l->n++] = t[--k];
}
static void rl_out(rline *l) { l->b[l->n++] = '\n'; if (write(2, l->b, l->n) < 0) {} }
// A range left holding a page read-only it could not make writable counts as always written: another range over the same page may make it writable behind its back, and its writes would go
// unseen (atomics only: the fault path calls it)
// Counted before it is set (and the count taken back if it was set already), so g_nbad is never below the number of ranges marked, even for a moment: nvn_dirty_now moves while any is.
// A fault on a pool being destroyed can mark its slot after the removal cleared it: the slot stays counted until reused (nvn_dirty_add_range), which costs only the shortcuts meanwhile
static void mark_bad(range_t *r) { atomic_fetch_add(&g_nbad, 1); if (atomic_exchange(&r->bad, 1)) atomic_fetch_sub(&g_nbad, 1); }
// 0 = the protection changed. It can fail: Linux refuses a split past vm.max_map_count (65,530 mappings; split KGSL mappings never merge back, docs/android/probes-gpu.md P0.7), and over several
// mappings it may change the first ones and then fail on a later one (ENOMEM). Every caller makes the states true again (a one-page call is all or nothing: one mapping) and marks the range
// always written (mark_bad, protect_failed), and a fault it cannot resolve goes to the crash report instead of retrying forever. Reported red once
static int mp(uintptr_t a, size_t len, int prot) {
    uint64_t t = mach_absolute_time(); int rc = mprotect((void *)a, len, prot), err = errno;
    atomic_fetch_add(&g_mp_ticks, mach_absolute_time() - t); atomic_fetch_add(&g_mp_calls, 1); atomic_fetch_add(&g_mp_pages, len >> g_shift);
    if (rc && !atomic_exchange(&g_mp_red, 1)) {
        rline l; l.n = 0; rl_s(&l, "[dm] [RED] dirty-page tracking: mprotect("); rl_n(&l, a, 1); rl_s(&l, ", "); rl_n(&l, len, 1);
        rl_s(&l, prot & PROT_WRITE ? ", writable) failed (errno " : ", read-only) failed (errno "); rl_n(&l, (uint64_t)err, 0);
        rl_s(&l, "): the tracker makes those pages' states true again and counts the pool as always written, or stops (only the first failure is reported)"); rl_out(&l);
    }
    errno = err; return rc;
}

static mach_timebase_info_data_t g_tb;   // obtained when the first pool is registered (the fault path only multiplies and divides)
static _Atomic int g_busy_red;
// Wait for this page to leave BUSY. Returns 1 = it has left; 0 = waited past the limit (reported red once). Usable in a signal handler: only atomic reads, mach_absolute_time, sched_yield and rline
static int busy_wait(range_t *r, uintptr_t p) {
    uint64_t t0 = 0;
    for (unsigned n = 1; atomic_load(&r->state[p]) == PG_BUSY; n++) {
        if (n & 0x3ff) continue;
        uint64_t now = mach_absolute_time(); if (!t0) t0 = now;
        if ((now - t0) * g_tb.numer / g_tb.denom > (uint64_t)BUSY_WAIT_MS * 1000000ull) {
            if (!atomic_exchange(&g_busy_red, 1)) { rline l; l.n = 0; rl_s(&l, "[dm] [RED] dirty-page tracking: page "); rl_n(&l, r->base + (p << g_shift), 1); rl_s(&l, " stuck 'busy' for over ");
                rl_n(&l, BUSY_WAIT_MS, 0); rl_s(&l, " ms (the thread changing its protection has not finished), no longer waiting"); rl_out(&l); }
            return 0; }
        sched_yield();
    }
    return 1;
}
static int live_at(int i) { return atomic_load_explicit(&rg(i)->live, memory_order_acquire); }
// The registered range that holds all of [a, a+len) (used for a texture's tracking / protection / dirty check)
static range_t *find_span(uintptr_t a, size_t len) {
    int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) { range_t *r = rg(i); if (live_at(i) && a >= r->base && a + len <= r->end) return r; }
    return NULL;
}
static int unprotect_page(range_t *r, uintptr_t p) {   // The caller has already changed the state from PROT to BUSY. 0 = the page stayed read-only (mp failed): PROT again, the range always written
    if (mp(r->base + (p << g_shift), g_pg, PROT_READ | PROT_WRITE)) { atomic_store(&r->state[p], PG_PROT); mark_bad(r); return 0; }
    atomic_store(&r->gen[p], atomic_fetch_add(&g_now, 1) + 1);
    atomic_store(&r->state[p], PG_NONE);
    return 1;
}
// Unprotect one page of a range: if protected, unprotect and stamp it; if another thread is changing its protection, wait for it to finish and look again -- if the other side was setting read-only, the page is PROT after the wait and still has to be unprotected
// (an unprotect-before-write never reaches the fault handler, so it must be done here). Returns 1 = this page is managed by this module; 0 = it is not, or the wait on BUSY timed out (the
// page's state is unknown then, so the range counts as always written); -1 = it could not be unprotected (still read-only for this range, which now counts as always written)
static int release_page(range_t *r, uintptr_t p, _Atomic uint64_t *count) {
    for (;;) {
        uint8_t st = PG_PROT;
        if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { if (!unprotect_page(r, p)) return -1; if (count) atomic_fetch_add(count, 1); return 1; }
        if (st != PG_BUSY) return atomic_load(&r->gen[p]) != 0;   // already NONE: just unprotected by another thread (the fault happened before the unprotect) -> re-execute; pages never protected are not ours
        if (!busy_wait(r, p)) { mark_bad(r); return 0; }
    }
}
static int on_fault(void *addr, int is_write, void *uc) {
    (void)uc;
    if (!is_write) return 0;
    uintptr_t a = (uintptr_t)addr; int ours = 0, stuck = 0, n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++)   // Every range sharing the page must be released: releasing only one leaves the other range's state at "protected", and later writes no longer fault and are missed
        { range_t *r = rg(i); if (live_at(i) && a >= r->base && a < r->end) { int v = release_page(r, (a - r->base) >> g_shift, &g_faults); if (v < 0) stuck = 1; else ours |= v; } }
    if (!stuck) return ours;
    // A range's set-writable failed (it counts as always written), so the loop cannot tell whether the page is writable: another range's set-writable, here or on a thread that held it BUSY
    // meanwhile, may have made it so, and an "already writable" range may be stale. One direct set-writable decides (it also succeeds, changing nothing, on a page already writable)
    if (mp(a & ~(g_pg - 1), g_pg, PROT_READ | PROT_WRITE)) return 0;   // still read-only: it would fault again at once, so the crash handler reports the write instead
    // Writable now, but a range may have started a set-read-only since the loop passed it (its mprotect landing before the direct one, its PROT after), or still be BUSY after a timed-out
    // wait: either would end up PROT over a writable page. So every covering range is released again: the failed one's set-writable now succeeds (NONE, true and stamped), a BUSY one is
    // waited for and released (or counts as always written if the wait times out), and one still PROT after that counts as always written. What is left is the window of the normal path:
    // a set-read-only that starts after this pass has visited its range, racing another range's set-writable of the same page
    for (int i = 0; i < n; i++) { range_t *r = rg(i); if (live_at(i) && a >= r->base && a < r->end) release_page(r, (a - r->base) >> g_shift, NULL); }   // not counted: the first loop counted the fault
    for (int i = 0; i < n; i++)
        { range_t *r = rg(i); if (live_at(i) && a >= r->base && a < r->end && atomic_load(&r->state[(a - r->base) >> g_shift]) == PG_PROT) mark_bad(r); }
    return 1;
}
// Unprotect pages p..q of a range with one mprotect per run of protected pages (a host write over a whole texture, e.g. WriteTexels, used to make one system call per 16 KB page:
// Link's Awakening, about 275 a frame). Pages another thread is changing are handled one by one after the run so far is finished, so no BUSY page is held while waiting
static void release_pages(range_t *r, uintptr_t p, uintptr_t q, _Atomic uint64_t *count) {
    uintptr_t run = 0; int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_PROT; int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (took) { if (!open) { run = i; open = 1; } continue; }
        if (open) {
            if (!mp(r->base + (run << g_shift), (i - run) << g_shift, PROT_READ | PROT_WRITE)) {
                for (uintptr_t k = run; k < i; k++) { atomic_store(&r->gen[k], atomic_fetch_add(&g_now, 1) + 1); atomic_store(&r->state[k], PG_NONE); }
                if (count) atomic_fetch_add(count, i - run);
            } else for (uintptr_t k = run; k < i; k++) {   // it may have made the first pages writable: one page at a time, each all or nothing, so each state is true
                // Still read-only (mark_bad: another range may unprotect it). The host's own stores there then fault into on_fault (hle_fpslock.c, nvn_null.c cpu_target, hle_nvn.c write_texels),
                // but a system call writing into it gets EFAULT: hle_fs.c ReadFile's pread prepares again up to 8 times, then fails the read with a red line (not changed here)
                if (mp(r->base + (k << g_shift), g_pg, PROT_READ | PROT_WRITE)) { atomic_store(&r->state[k], PG_PROT); mark_bad(r); continue; }
                atomic_store(&r->gen[k], atomic_fetch_add(&g_now, 1) + 1); atomic_store(&r->state[k], PG_NONE);
                if (count) atomic_fetch_add(count, 1);
            }
            open = 0;
        }
        if (i <= q && st == PG_BUSY) release_page(r, i, count);   // another thread is changing it: wait, then unprotect it if it ended up protected
    }
}
static void prep_write(void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr, e = a + len; int n = atomic_load(&g_nr);
    if (!len) return;
    for (int i = 0; i < n; i++) {
        range_t *r = rg(i);
        if (!live_at(i) || e <= r->base || a >= r->end) continue;
        uintptr_t lo = a > r->base ? a : r->base, hi = e < r->end ? e : r->end;
        release_pages(r, (lo - r->base) >> g_shift, (hi - 1 - r->base) >> g_shift, &g_preps);
    }
}

void nvn_dirty_add_range(const void *mem, size_t size) {
    static int inited;
    if (!inited) { inited = 1; g_pg = (uintptr_t)getpagesize(); g_shift = __builtin_ctzl(g_pg); mach_timebase_info(&g_tb); dm_fault_hook_register(on_fault); dm_guest_write_prep_register(prep_write); }
    atomic_fetch_add(&g_adds_total, 1);
    pthread_mutex_lock(&g_slot_mu);
    int n = atomic_load(&g_nr);
    int slot = -1;
    for (int i = 0; i < n; i++) if (!live_at(i)) { slot = i; break; }
    if (slot < 0) {
        if (n >= MAX_RANGE) {
            int expected = 0;
            if (atomic_compare_exchange_strong(&g_overflow_logged, &expected, 1)) LOG("[RED] dirty-page tracking ranges are full (%d), pool %p not tracked", MAX_RANGE, mem);
            pthread_mutex_unlock(&g_slot_mu); return;
        }
        slot = n;
        if (!atomic_load_explicit(&g_blk[n >> BLK_SHIFT], memory_order_acquire)) {   // published before g_nr counts the slot (the scans read g_nr first)
            range_t *b = calloc(BLK, sizeof *b); if (!b) die("dirty-page tracking range table allocation failed (block %d)", n >> BLK_SHIFT);
            atomic_store_explicit(&g_blk[n >> BLK_SHIFT], b, memory_order_release);
        }
    }
    { int lv = atomic_fetch_add(&g_live, 1) + 1, pk = atomic_load(&g_live_peak); while (lv > pk && !atomic_compare_exchange_weak(&g_live_peak, &pk, lv)) {} }
    range_t *r = rg(slot);
    if (atomic_exchange(&r->bad, 0)) atomic_fetch_sub(&g_nbad, 1);   // a fault that timed out on the previous pool here may have marked it after its removal: not this pool's (one still running may mark this pool: conservative)
    uintptr_t base = (uintptr_t)mem & ~(g_pg - 1), end = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
    size_t np = (end - base) >> g_shift;
    _Atomic uint8_t *state = r->state;
    _Atomic uint64_t *gen = r->gen;
    if (r->cap < np) {
        state = calloc(np, 1); gen = calloc(np, sizeof *gen);
        if (!state || !gen) die("dirty-page table allocation failed (%zu pages)", np);
        // The old array is not freed: the signal handler may still have a very brief stale reader; leak = 9 bytes per page.
        r->cap = np;
    } else {
        memset((void *)state, 0, np); memset((void *)gen, 0, np * sizeof *gen);
    }
    r->base = base; r->end = end; r->mem = (uintptr_t)mem; r->state = state; r->gen = gen;
    if (slot == n) atomic_store(&g_nr, n + 1);
    atomic_store_explicit(&r->live, 1, memory_order_release);
    pthread_mutex_unlock(&g_slot_mu);
}
// Pool destruction: first restore still-protected pages to writable (the pool memory may be reused for something else once returned to the game), then mark invalid
void nvn_dirty_remove_range(const void *mem) {
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = NULL; int n = atomic_load(&g_nr);
    for (int i = 0; i < n && !r; i++) if (live_at(i) && rg(i)->mem == (uintptr_t)mem) r = rg(i);   // Match by the pool's exact start: an adjacent pool sharing a page may have the same rounded base
    if (r) { prep_write((void *)r->base, r->end - r->base); if (atomic_exchange(&r->bad, 0)) atomic_fetch_sub(&g_nbad, 1); atomic_store_explicit(&r->live, 0, memory_order_release); atomic_fetch_sub(&g_live, 1); }   // a fault's mark_bad landing between the clear and live 0 is undone when the slot is reused (nvn_dirty_add_range)
    pthread_mutex_unlock(&g_slot_mu);
}
int nvn_dirty_tracked(const void *addr, size_t len) {
    return find_span((uintptr_t)addr, len) != NULL;
}
// The tracked range holding [addr, addr+len), as a handle a caller keeps (packaged textures: the scan over every range ran twice per texture lookup). NULL = untracked
const void *nvn_dirty_span(const void *addr, size_t len) { return len ? find_span((uintptr_t)addr, len) : NULL; }
// A set-read-only of pages run..end-1 of r failed. Linux may have made the run's first mappings read-only before failing on a later one, so which of its pages are read-only is not known:
// the whole run goes back to writable (all its pages were writable, NONE, when the call took them), and r counts as always written from now on, as an untracked pool does: nvn_dirty_since says 1,
// nvn_dirty_now moves on every read (no caller's "nothing written since" shortcut holds) and r is not protected again. Other ranges sharing those pages hear of the write (prep_write: a page
// read-only for them is writable now). If even this mprotect fails nothing is known about the run: a write could fault forever or land unseen, so the host stops. Host path only (LOG, die):
// nvn_dirty_protect and nvn_dirty_protect_wait are its callers, never the fault handler
static _Atomic int g_bad_logged;
static void protect_failed(range_t *r, uintptr_t run, uintptr_t end) {
    uintptr_t a = r->base + (run << g_shift); size_t n = (end - run) << g_shift;
    if (mp(a, n, PROT_READ | PROT_WRITE)) {
        int err = errno;
        LOG("[RED] dirty-page tracking: a failed set-read-only of %p..%p could not be undone (mprotect back to writable failed, errno %d): those pages' protection is unknown", (void *)a, (void *)(a + n), err);
        die("dirty-page tracking: stopping rather than risk a write that faults forever or lands unseen (pool %p)", (void *)r->mem);
    }
    for (uintptr_t k = run; k < end; k++) { atomic_store(&r->gen[k], atomic_fetch_add(&g_now, 1) + 1); atomic_store(&r->state[k], PG_NONE); }   // stamped: a write fault waiting on BUSY then takes NONE as "ours" and re-executes
    mark_bad(r);
    if (!atomic_exchange(&g_bad_logged, 1)) LOG("dirty-page tracking: pool %p counts as always written from now on (its textures are uploaded at every use); later such pools are not reported", (void *)r->mem);
    prep_write((void *)a, n);   // r's own pages there are NONE now, so only other ranges' read-only pages are released
}
// Set read-only: mark the pages BUSY first and mark them PROT only after mprotect has landed (for the order and the invariant see the state notes at the top of the file)
void nvn_dirty_protect(const void *addr, size_t len) {
    range_t *r = len ? find_span((uintptr_t)addr, len) : NULL;
    if (!r || atomic_load(&r->bad)) return;   // always written (protect_failed): not protected again
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    // Consecutive pages are merged into one mprotect (per-page calls reach thousands per frame on the first upload of large textures; character select page startup frame 5: 4888 calls).
    // Pages already PROT / BUSY break the consecutive run: do not protect again, and do not touch pages another thread is changing
    uintptr_t run = 0; int open = 0, failed = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_NONE;
        int took = i <= q && !failed && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);   // after a failure no more pages are taken
        if (took && !open) { run = i; open = 1; }
        if (!took && open) {
            if (!mp(r->base + (run << g_shift), (i - run) << g_shift, PROT_READ)) for (uintptr_t k = run; k < i; k++) atomic_store(&r->state[k], PG_PROT);
            else { protect_failed(r, run, i); failed = 1; }
            open = 0;
        }
    }
}
// nvn_dirty_protect for a caller that stamps first and copies the pages after protecting them (nvn_vk.c b_commit, the Vulkan backend's strategy B). nvn_dirty_protect skips a page another
// thread holds BUSY; when that is an unprotect (a fault's unprotect_page, a host pre-write's release_pages) that has stamped the page and not yet set NONE, the caller's stamp already covers
// that generation, the page ends writable with nothing newer, and the write after it is never seen. Here such a page is waited for (busy_wait) and taken if it ended NONE; PROT after the wait
// is protected already. The run taken so far is protected first, so no page is held BUSY while waiting (as release_pages does).
// 1 = every page ended PROT, or r is untracked or always written (nvn_dirty_since says 1 there). 0 = not: a set-read-only failed (protect_failed: r is always written now), or a wait timed out
// (the page's state is unknown, so r counts as always written, as release_page does: the caller is not left waiting BUSY_WAIT_MS at every later call).
// Deadlock: the wait may run under a caller's lock (nvn_vk.c's g_pmu). Every holder of a BUSY page (on_fault through release_page and unprotect_page, prep_write through release_pages,
// nvn_dirty_protect and protect_failed, this function) does only atomics and mprotect while holding it (protect_failed's die path logs first: stderr's lock, which nothing holds while it waits
// for a caller's lock) and takes no lock of its caller's: the fault and host pre-write hooks reach this file only (host/crash.c; nvn_vk.c registers none, and its g_pmu is static there).
// The wait is capped at BUSY_WAIT_MS anyway.
// Slot lifetime: the whole call holds g_slot_mu, so the range cannot be removed and its slot reused by another pool while this waits (r->base would then point the index computed here at that
// pool's memory). Order: a caller's lock (g_pmu), then g_slot_mu. Only nvn_dirty_add_range and nvn_dirty_remove_range take g_slot_mu, and neither is reached from the fault or pre-write
// hooks or under g_pmu; a BUSY page a removal holds (its prep_write) is released before it lets g_slot_mu go, so this waits for the removal and then finds the range gone
static int protect_wait_locked(const void *addr, size_t len);
int nvn_dirty_protect_wait(const void *addr, size_t len) {
    pthread_mutex_lock(&g_slot_mu);
    int ok = protect_wait_locked(addr, len);
    pthread_mutex_unlock(&g_slot_mu);
    return ok;
}
static int protect_wait_locked(const void *addr, size_t len) {
    range_t *r = len ? find_span((uintptr_t)addr, len) : NULL;
    if (!r || atomic_load(&r->bad)) return 1;
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    uintptr_t run = 0; int open = 0, ok = 1;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_NONE;
        int took = i <= q && ok && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);   // after a failure no more pages are taken
        if (!took && open) {   // the run so far, before any wait
            if (!mp(r->base + (run << g_shift), (i - run) << g_shift, PROT_READ)) for (uintptr_t k = run; k < i; k++) atomic_store(&r->state[k], PG_PROT);
            else { protect_failed(r, run, i); ok = 0; }
            open = 0;
        }
        while (!took && ok && i <= q && st == PG_BUSY) {   // another thread is changing it: wait, then take it if it ended NONE (st PROT: protected already)
            if (!busy_wait(r, i)) { mark_bad(r); ok = 0; break; }
            st = PG_NONE; took = atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        }
        if (took && !open) { run = i; open = 1; }
    }
    return ok;
}
uint64_t nvn_dirty_now(void) { return atomic_load(&g_nbad) ? atomic_fetch_add(&g_now, 1) + 1 : atomic_load(&g_now); }   // see protect_failed
int nvn_dirty_since_span(const void *span, const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    range_t *r = (range_t *)span; uintptr_t a0 = (uintptr_t)addr;   // the kept handle still holds the span (a slot reused by another pool over the same memory is as good): no scan
    if (!r || !atomic_load_explicit(&r->live, memory_order_acquire) || a0 < r->base || a0 + len > r->end) r = find_span(a0, len);
    if (!r || atomic_load_explicit(&r->bad, memory_order_relaxed)) return 1;   // When untracked (or always written: protect_failed), conservatively treat as dirty: better to upload more than to show stale content
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    for (uintptr_t i = p; i <= q; i++) if (atomic_load(&r->gen[i]) > stamp) return 1;   // The generation of the last write before upload is <= the stamp; any later write must be > the stamp
    return 0;
}
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) { return nvn_dirty_since_span(NULL, addr, len, stamp); }
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total) {
    int n = atomic_load(&g_nr), active = 0;
    for (int i = 0; i < n; i++) active += live_at(i) != 0;
    *live = active; *used_slots = n; *adds_total = atomic_load(&g_adds_total);
}
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps) { *faults = atomic_load(&g_faults); *preps = atomic_load(&g_preps); }
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    *calls = atomic_load(&g_mp_calls); *pages = atomic_load(&g_mp_pages); *ns = atomic_load(&g_mp_ticks) * tb.numer / tb.denom;
}
