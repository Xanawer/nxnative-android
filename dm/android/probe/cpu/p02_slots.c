// P0.2 thread slots (the deciding probe).
// The port needs a stub-shaped sequence that, from any guest thread, reaches a per-thread
// 8-byte slot using only `mrs xN, tpidr_el0` plus at most two dependent loads with constant
// offsets, no calls. This probe investigates four candidates:
//   (a) initial-exec __thread in the executable: is (&var - tpidr_el0) a constant on 64 threads?
//   (b) initial-exec and default-model TLS in a dlopen'd libie.so (nativeLibraryDir).
//   (c) sentinel search with NO hard-coded bionic offsets: find a chain tp -> [tp+A] -> [+B]
//       that is the value slot of a pthread_key, verified on 64 threads, after setspecific
//       changes, in nested threads, and inside a signal handler. Then JIT the exact stub-shaped
//       asm and verify reading and writing through it.
//   (d) TLS_SLOT_APP (tp+16), reserved for apps on API 29+.
#include <dlfcn.h>
#include <pthread.h>
#include <sys/mman.h>
#include "cpuprobe.h"

#define NTHREAD 64

static inline uintptr_t read_tp(void) {
    uintptr_t t; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(t)); return t;
}

// ---- guarded 8-byte read (main thread, during the blind scan only) ----
static int gread(const void *addr, uintptr_t *out) {
    if (GUARD_TRY()) { *out = *(const volatile uintptr_t *)addr; GUARD_OK(); return 1; }
    return 0;
}

// ---- (a) initial-exec TLS in the executable itself ----
// An initial-exec __thread symbol can only live in the main executable (static TLS). Putting one
// in the dlopen'd libprobe.so makes bionic REFUSE TO LOAD the library ("dlopen failed: TLS symbol
// ... using IE access model"), so it is compiled only into the -DPROBE_EXE build.
#ifdef PROBE_EXE
__attribute__((tls_model("initial-exec"))) static __thread uint64_t g_ie_self;
#endif

// ---- key + per-thread sentinel plumbing for (c) ----
static pthread_key_t g_key;
static FILE *g_f;                         // only the main thread writes the report
static long g_A; static long g_B; static int g_chain_found;
// signal-handler verification runs one thread at a time with async-signal-safe plain globals
// (no TLS access inside the handler: reading a global-dynamic __thread var would call the
// non-async-safe __tls_get_addr). The handler reads the slot via the chain and the thread checks.
static volatile sig_atomic_t g_sig_flag;
static volatile uintptr_t g_sig_read;

// ---- JIT of the stub-shaped sequence (reuses P0.1's capability) ----
// reader:  mrs x0, tpidr_el0 ; <load A> ; <load B> ; ret        -> returns the slot VALUE
// writer:  mrs x1, tpidr_el0 ; <load A into x1> ; str x0,[x1+B]  -> stores x0 into the slot
typedef uintptr_t (*rd_t)(void);
typedef void (*wr_t)(uintptr_t);

// pick a load encoding for `off` into register Rt from base Rt (reader) — Rt=0
static uint32_t enc_load(long off, int rt, int rn) {
    if (off >= 0 && (off % 8) == 0 && (off / 8) < 4096)
        return 0xF9400000u | ((uint32_t)(off / 8) << 10) | (rn << 5) | rt;      // ldr xRt,[xRn,#off]
    if (off >= -256 && off < 256)
        return 0xF8400000u | (((uint32_t)off & 0x1ff) << 12) | (rn << 5) | rt;  // ldur xRt,[xRn,#off]
    return 0;  // out of range
}
static uint32_t enc_store(long off, int rt, int rn) {
    if (off >= 0 && (off % 8) == 0 && (off / 8) < 4096)
        return 0xF9000000u | ((uint32_t)(off / 8) << 10) | (rn << 5) | rt;      // str xRt,[xRn,#off]
    if (off >= -256 && off < 256)
        return 0xF8000000u | (((uint32_t)off & 0x1ff) << 12) | (rn << 5) | rt;  // stur xRt,[xRn,#off]
    return 0;
}

static void *jit(const uint32_t *words, int n) {
    long ps = sysconf(_SC_PAGESIZE);
    void *m = mmap(NULL, (size_t)ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return NULL;
    memcpy(m, words, (size_t)n * 4);
    __builtin___clear_cache((char *)m, (char *)m + n * 4);
    if (mprotect(m, (size_t)ps, PROT_READ | PROT_EXEC) != 0) { munmap(m, (size_t)ps); return NULL; }
    return m;
}

// ---- worker thread for (c): set a unique sentinel, verify the chain ----
struct work { int idx; int nested; int rc_read; int rc_write; int rc_nested; uintptr_t got; uintptr_t want; };

static uintptr_t slot_addr_from_chain(void) {   // compute &slot on the current thread using (A,B)
    uintptr_t tp = read_tp();
    uintptr_t p  = *(uintptr_t *)(tp + g_A);
    return p + (uintptr_t)g_B;
}

// async-signal-safe: only loads via the chain and a store to a volatile global (no TLS, no libc)
static void sig_slot_handler(int s, siginfo_t *si, void *uc) {
    (void)s; (void)si; (void)uc;
    g_sig_read = *(uintptr_t *)slot_addr_from_chain();
    g_sig_flag = 1;
}

static void *nested_fn(void *p) {
    struct work *w = p;
    uintptr_t sent = (uintptr_t)0xB1A5000000000000ULL | (0x2000u + (unsigned)w->idx);
    pthread_setspecific(g_key, (void *)sent);
    uintptr_t v = *(uintptr_t *)slot_addr_from_chain();
    w->rc_nested = (v == sent);
    return NULL;
}

static void *worker(void *p) {
    struct work *w = p;
    uintptr_t sent = (uintptr_t)0xA5A5000000000000ULL | (0x1000u + (unsigned)w->idx);
    pthread_setspecific(g_key, (void *)sent);
    w->want = sent;

    // 1. read through the chain
    uintptr_t v = *(uintptr_t *)slot_addr_from_chain();
    w->got = v;
    w->rc_read = (v == sent);

    // 2. change the sentinel, re-read (after a setspecific change)
    uintptr_t sent2 = sent ^ 0x0F0F0F0FULL;
    pthread_setspecific(g_key, (void *)sent2);
    uintptr_t v2 = *(uintptr_t *)slot_addr_from_chain();
    int changed_ok = (v2 == sent2);
    pthread_setspecific(g_key, (void *)sent);   // restore

    // 3. write THROUGH the chain, check pthread_getspecific sees it
    uintptr_t wv = sent ^ 0x00001234ULL;
    *(uintptr_t *)slot_addr_from_chain() = wv;
    w->rc_write = ((uintptr_t)pthread_getspecific(g_key) == wv) && changed_ok;
    pthread_setspecific(g_key, (void *)sent);   // restore

    // 4. a thread created by this thread (threads-of-threads)
    struct work nw; memset(&nw, 0, sizeof nw); nw.idx = w->idx + 1000;
    pthread_t nt; if (pthread_create(&nt, NULL, nested_fn, &nw) == 0) { pthread_join(nt, NULL); w->rc_nested = nw.rc_nested; }
    else w->rc_nested = -1;
    return NULL;
}

// serialized signal-handler verification: one thread at a time sets its own sentinel, raises the
// signal, and checks that the handler read the slot correctly. Returns the signo used, and sets *ok.
static int g_sig_signo;
struct sig_arg { int ok; };
static void *sig_test_thread(void *p) {
    struct sig_arg *a = p;
    uintptr_t s = 0x6000000000000000ULL | (uintptr_t)(uintptr_t)pthread_self();
    pthread_setspecific(g_key, (void *)s);
    g_sig_flag = 0; g_sig_read = 0;
    pthread_kill(pthread_self(), g_sig_signo);
    for (int i = 0; i < 20000000 && !g_sig_flag; i++) { }
    a->ok = (g_sig_flag && g_sig_read == s);
    return NULL;
}

// ================= part (a) =================
#ifdef PROBE_EXE
struct a_work { int idx; long off; int nested_consistent; long nested_off; };
static void *a_nested(void *p) {
    struct a_work *w = p;
    w->nested_off = (long)((uintptr_t)&g_ie_self - read_tp());
    return NULL;
}
static void *a_worker(void *p) {
    struct a_work *w = p;
    g_ie_self = 0xC0DE;
    w->off = (long)((uintptr_t)&g_ie_self - read_tp());
    struct a_work nw; memset(&nw, 0, sizeof nw); nw.idx = w->idx + 1000;
    pthread_t nt; if (pthread_create(&nt, NULL, a_nested, &nw) == 0) { pthread_join(nt, NULL); w->nested_off = nw.nested_off; }
    return NULL;
}
static void part_a(FILE *f) {
    fprintf(f, "\n[a] initial-exec __thread in this image (EXECUTABLE)\n");
    long ref = (long)((uintptr_t)&g_ie_self - read_tp());
    struct a_work w[NTHREAD]; pthread_t t[NTHREAD];
    int consistent = 1, nested_consistent = 1;
    for (int i = 0; i < NTHREAD; i++) { w[i].idx = i; w[i].off = 0; w[i].nested_off = 0x7fffffff; pthread_create(&t[i], NULL, a_worker, &w[i]); }
    for (int i = 0; i < NTHREAD; i++) {
        pthread_join(t[i], NULL);
        if (w[i].off != ref) consistent = 0;
        if (w[i].nested_off != ref) nested_consistent = 0;
    }
    fprintf(f, "    main offset (&g_ie_self - tpidr_el0) = %ld (%#lx)\n", ref, (unsigned long)ref);
    fprintf(f, "    same constant on %d threads: %s\n", NTHREAD, consistent ? "YES" : "NO");
    fprintf(f, "    same constant in threads-of-threads: %s\n", nested_consistent ? "YES" : "NO");
    fprintf(f, "    => initial-exec at a fixed tp offset works in this image: %s\n",
            (consistent && nested_consistent) ? "YES (usable as a slot in an EXECUTABLE)" : "NO");
}
#else
static void part_a(FILE *f) {
    fprintf(f, "\n[a] initial-exec __thread in this image (APK / dlopen'd libprobe.so)\n");
    fprintf(f, "    NOT APPLICABLE: an initial-exec __thread symbol compiled into libprobe.so makes bionic\n");
    fprintf(f, "    REFUSE TO LOAD the library at dlopen time (\"dlopen failed: TLS symbol ... using IE access\n");
    fprintf(f, "    model\"), observed directly on this device. So candidate (a) cannot be used in the APK.\n");
    fprintf(f, "    The dlopen'd-library case is measured in part (b) via libie.so.\n");
}
#endif

// ================= part (b) =================
struct b_targ { void *(*fn)(void); long off; };
static void *b_thread(void *pp) { struct b_targ *a = pp; a->off = (long)((uintptr_t)a->fn() - read_tp()); return NULL; }

static void part_b(FILE *f, const char *libdir) {
    fprintf(f, "\n[b] TLS models in a dlopen'd library (nativeLibraryDir)\n");

    // (b.1) initial-exec in a dlopen'd .so: does bionic even load it?
    char p1[700]; snprintf(p1, sizeof p1, "%s/libieonly.so", libdir);
    dlerror();
    void *h1 = dlopen(p1, RTLD_NOW | RTLD_LOCAL);
    if (!h1) {
        fprintf(f, "    initial-exec lib: dlopen FAILED: %s\n", dlerror());
        fprintf(f, "      => bionic refuses a dlopen'd .so that references an initial-exec __thread symbol.\n");
        fprintf(f, "      => candidate (a) is unavailable for the port's rewritten modules / libdmhost.so.\n");
    } else {
        void *(*ie_addr)(void) = (void *(*)(void))dlsym(h1, "ie_tls_addr");
        long ref = ie_addr ? (long)((uintptr_t)ie_addr() - read_tp()) : 0;
        long offs[8]; int cst = 1;
        for (int i = 0; i < 8; i++) { struct b_targ ta = { ie_addr, 0 }; pthread_t th; pthread_create(&th, NULL, b_thread, &ta); pthread_join(th, NULL); offs[i] = ta.off; if (offs[i] != ref) cst = 0; }
        fprintf(f, "    initial-exec lib: LOADED, &var-tp main=%#lx constant across threads=%s (bionic used static TLS surplus)\n",
                (unsigned long)ref, cst ? "YES" : "NO");
        dlclose(h1);
    }

    // (b.2) default model (global-dynamic) in a dlopen'd .so: is &var a constant offset from tp?
    char p2[700]; snprintf(p2, sizeof p2, "%s/libgd.so", libdir);
    dlerror();
    void *h2 = dlopen(p2, RTLD_NOW | RTLD_LOCAL);
    if (!h2) { fprintf(f, "    default-model lib: dlopen FAILED: %s\n", dlerror()); return; }
    void *(*gd_addr)(void) = (void *(*)(void))dlsym(h2, "gd_tls_addr");
    if (!gd_addr) { fprintf(f, "    default-model lib: dlsym failed: %s\n", dlerror()); dlclose(h2); return; }
    long gref = (long)((uintptr_t)gd_addr() - read_tp());
    long goffs[8]; int gcst = 1;
    for (int i = 0; i < 8; i++) { struct b_targ ta = { gd_addr, 0 }; pthread_t th; pthread_create(&th, NULL, b_thread, &ta); pthread_join(th, NULL); goffs[i] = ta.off; if (goffs[i] != gref) gcst = 0; }
    fprintf(f, "    default-model lib: LOADED, &var-tp main=%#lx constant across threads=%s (expected NO: __tls_get_addr block)\n",
            (unsigned long)gref, gcst ? "YES" : "NO");
    fprintf(f, "      per-thread offsets:"); for (int i = 0; i < 8; i++) fprintf(f, " %#lx", (unsigned long)goffs[i]); fprintf(f, "\n");
    dlclose(h2);
}

// ================= part (c) =================
typedef struct { long A; long B; } cand_t;
#define MAXCAND 1024

// cross-thread stability filter: a candidate (A,B) that only aliases the main thread's slot (e.g. a
// stack value that happens to point near it) fails here; a true chain through the TCB holds on every
// thread. Each round runs on its own thread with a unique sentinel and clears pass[] for any miss.
struct filt { const cand_t *c; int n; unsigned char *pass; int round; };
static void *filter_fn(void *p) {
    struct filt *F = p;
    uintptr_t s = 0x7000000000000000ULL | (uintptr_t)(F->round * 0x1000 + 0x101);
    pthread_setspecific(g_key, (void *)s);
    uintptr_t tp = read_tp();
    for (int i = 0; i < F->n; i++) {
        if (!F->pass[i]) continue;
        uintptr_t v = 0; int ok = 0;
        if (GUARD_TRY()) { uintptr_t pp = *(uintptr_t *)(tp + F->c[i].A); v = *(uintptr_t *)(pp + F->c[i].B); ok = 1; GUARD_OK(); }
        if (!ok || v != s) F->pass[i] = 0;
    }
    return NULL;
}

static void part_c(FILE *f, int in_app) {
    fprintf(f, "\n[c] sentinel search for a pthread_key slot chain\n");
    if (pthread_key_create(&g_key, NULL)) { fprintf(f, "    pthread_key_create failed\n"); return; }
    unsigned long raw_key = (unsigned long)(uint32_t)g_key & ~0x80000000U;   // public key = index | KEY_VALID_FLAG (int, so mask 32-bit)
    fprintf(f, "    key=%#x (raw index=%lu)\n", (unsigned)g_key, raw_key);

    volatile uintptr_t SENT = 0xDEADBEEF12345678ULL;   // not const: the write-back test must never let the
    pthread_setspecific(g_key, (void *)SENT);           // compiler assume this never changes
    uintptr_t tp = read_tp();

    GUARD_INSTALL();
    // blind scan: A in [-4096,4096], B in [0,4096] (step 8). A raw match is any (A,B) with
    // *(*(tp+A)+B)==SENT; write-back (guarded) keeps only those that actually reach the key's slot.
    static cand_t cand[MAXCAND]; int ncand = 0, nfound = 0;
    for (long A = -4096; A <= 4096; A += 8) {
        uintptr_t p;
        if (!gread((void *)(tp + A), &p)) continue;
        if (p == 0 || (p & 7)) continue;
        for (long B = 0; B <= 4096; B += 8) {
            uintptr_t v;
            if (!gread((void *)(p + B), &v)) break;   // p+B unmapped: stop scanning this p
            if (v != SENT) continue;
            nfound++;
            int ok = 0;
            if (GUARD_TRY()) {                        // guard: a candidate in read-only memory must not kill the run
                uintptr_t orig = *(volatile uintptr_t *)(p + B);   // save the EXACT original (==SENT); restoring the
                uintptr_t probe = 0x1122334455667788ULL;           // global SENT would corrupt the address if it aliases
                *(volatile uintptr_t *)(p + B) = probe;            // our own SENT storage or the slot mid-write
                ok = ((uintptr_t)pthread_getspecific(g_key) == probe);
                *(volatile uintptr_t *)(p + B) = orig;             // restore exactly what was there
                GUARD_OK();
            }
            if (ok && ncand < MAXCAND) { cand[ncand].A = A; cand[ncand].B = B; ncand++; }
        }
    }
    fprintf(f, "    raw sentinel matches=%d, write-back-verified (main thread)=%d\n", nfound, ncand);
    if (ncand == 0) { fprintf(f, "    => no chain found; mechanism (c) NOT viable\n"); return; }

    // cross-thread stability filter
    static unsigned char pass[MAXCAND];
    for (int i = 0; i < ncand; i++) pass[i] = 1;
    for (int round = 0; round < 8; round++) {
        struct filt F = { cand, ncand, pass, round };
        pthread_t th; pthread_create(&th, NULL, filter_fn, &F); pthread_join(th, NULL);
    }
    pthread_setspecific(g_key, (void *)SENT);
    int nstable = 0; for (int i = 0; i < ncand; i++) nstable += pass[i];
    fprintf(f, "    chains stable across 8 threads (own sentinel each): %d\n", nstable);

    // choose the canonical chain: among stable chains, prefer the one whose B identifies key_data[raw_index]
    // of bionic_tls -- i.e. (B-8)%16==0 and (B-8)/16 == this key's own index -- then the smallest |A|.
    // This pins A to TLS_SLOT_BIONIC_TLS without hard-coding its value; the key's index does the pinning.
    long best_A = 0, best_B = 0; int have = 0, canonical = 0; int listed = 0;
    for (int i = 0; i < ncand; i++) {
        if (!pass[i]) continue;
        if (listed < 12) { fprintf(f, "    stable candidate A=%ld B=%ld (key_data idx=%ld)\n", cand[i].A, cand[i].B, (cand[i].B - 8) / 16); listed++; }
        int is_canon = (cand[i].B >= 8 && (cand[i].B - 8) % 16 == 0 && (unsigned long)((cand[i].B - 8) / 16) == raw_key);
        int better;
        if (is_canon && !canonical) better = 1;                                  // first canonical beats any non-canonical
        else if (is_canon == canonical) better = (!have || labs(cand[i].A) < labs(best_A)); // tie: smallest |A|
        else better = 0;
        if (better) { best_A = cand[i].A; best_B = cand[i].B; canonical = is_canon; have = 1; }
    }
    if (listed < nstable) fprintf(f, "    ... (%d more stable chains; all are aliases of the same per-thread slot)\n", nstable - listed);
    if (!have) { fprintf(f, "    => no stable chain; mechanism (c) NOT viable\n"); return; }

    g_A = best_A; g_B = best_B; g_chain_found = 1;
    long bionic_idx = (best_B - 8) / 16;
    fprintf(f, "    chosen chain: value = *(*(tpidr_el0 %+ld) + %ld)  [%s]\n",
            best_A, best_B, canonical ? "canonical: B matches this key's index" : "non-canonical alias");
    fprintf(f, "    bionic mapping: tp%+ld = TLS_SLOT_BIONIC_TLS (-> struct bionic_tls*); +%ld = key_data[%ld].data\n",
            best_A, best_B, bionic_idx);
    fprintf(f, "    cross-check: (B-8)/16=%ld vs this key's raw index=%lu -> %s\n",
            bionic_idx, raw_key, (unsigned long)bionic_idx == raw_key ? "MATCH (confirms bionic_tls.key_data layout)" : "differ");

    // ---- JIT the exact stub-shaped sequence and verify ----
    uint32_t rd[8]; int n = 0;
    rd[n++] = 0xD53BD040;                 // mrs x0, tpidr_el0
    uint32_t l1 = enc_load(best_A, 0, 0); // ldr/ldur x0,[x0,#A]
    uint32_t l2 = enc_load(best_B, 0, 0); // ldr/ldur x0,[x0,#B]
    if (!l1 || !l2) { fprintf(f, "    cannot encode load offsets A=%ld B=%ld in a single instruction\n", best_A, best_B); return; }
    rd[n++] = l1; rd[n++] = l2; rd[n++] = 0xD65F03C0;   // ret
    rd_t reader = (rd_t)jit(rd, n);

    uint32_t wr[8]; int m = 0;
    wr[m++] = 0xD53BD041;                 // mrs x1, tpidr_el0
    uint32_t wl = enc_load(best_A, 1, 1); // ldr/ldur x1,[x1,#A]
    uint32_t ws = enc_store(best_B, 0, 1);// str/stur x0,[x1,#B]
    wr[m++] = wl; wr[m++] = ws; wr[m++] = 0xD65F03C0;   // ret
    wr_t writer = (wr_t)jit(wr, m);

    char rdbytes[64] = ""; for (int i = 0; i < n; i++) { char t[12]; snprintf(t, sizeof t, "%08x ", rd[i]); strcat(rdbytes, t); }
    fprintf(f, "    reader stub words: %s\n", rdbytes);

    int stub_read_ok = -1, stub_write_ok = -1;
    if (reader) {
        pthread_setspecific(g_key, (void *)SENT);
        uintptr_t rv1 = reader();
        uintptr_t rv2 = reader();                 // second call: distinguishes an icache race from a real miss
        uintptr_t via = (uintptr_t)pthread_getspecific(g_key);
        uintptr_t raw = *(uintptr_t *)slot_addr_from_chain();
        stub_read_ok = (rv1 == SENT);
        fprintf(f, "    reader diag: rv1=%#lx rv2=%#lx getspecific=%#lx raw[slot]=%#lx SENT=%#lx\n",
                (unsigned long)rv1, (unsigned long)rv2, (unsigned long)via, (unsigned long)raw, (unsigned long)SENT);
    }
    if (writer && reader) {
        uintptr_t nv = 0xCAFEF00DBAADF00DULL;
        writer(nv);
        stub_write_ok = ((uintptr_t)pthread_getspecific(g_key) == nv) && (reader() == nv);
        pthread_setspecific(g_key, (void *)SENT);
    }
    fprintf(f, "    JIT reader stub reads slot: %s\n", stub_read_ok == 1 ? "PASS" : stub_read_ok == 0 ? "FAIL" : "n/a");
    fprintf(f, "    JIT writer stub writes slot (seen by pthread_getspecific): %s\n",
            stub_write_ok == 1 ? "PASS" : stub_write_ok == 0 ? "FAIL" : "n/a");

    // ---- 64-thread read/write/nested verification ----
    struct work w[NTHREAD]; pthread_t t[NTHREAD];
    for (int i = 0; i < NTHREAD; i++) { memset(&w[i], 0, sizeof w[i]); w[i].idx = i; pthread_create(&t[i], NULL, worker, &w[i]); }
    int all_read = 1, all_write = 1, all_nested = 1;
    for (int i = 0; i < NTHREAD; i++) {
        pthread_join(t[i], NULL);
        if (!w[i].rc_read) all_read = 0;
        if (!w[i].rc_write) all_write = 0;
        if (w[i].rc_nested != 1) all_nested = 0;
    }
    fprintf(f, "    %d threads: read-chain=%s write-through=%s nested-thread=%s\n",
            NTHREAD, all_read ? "all PASS" : "FAIL", all_write ? "all PASS" : "FAIL", all_nested ? "all PASS" : "FAIL");

    // ---- in-signal-handler verification (serialized, async-safe, real-time signal to dodge ART) ----
    g_sig_signo = SIGRTMIN + 5; if (g_sig_signo > SIGRTMAX) g_sig_signo = SIGUSR1;
    struct sigaction ssa; memset(&ssa, 0, sizeof ssa);
    ssa.sa_sigaction = sig_slot_handler; ssa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&ssa.sa_mask);
    sigaction(g_sig_signo, &ssa, NULL);
    struct sigaction schk; sigaction(g_sig_signo, NULL, &schk);
    int sig_installed = (schk.sa_sigaction == sig_slot_handler);
    int all_sig = 1, sig_rounds = 8;
    for (int i = 0; i < sig_rounds; i++) {
        struct sig_arg a = { 0 };
        pthread_t st; pthread_create(&st, NULL, sig_test_thread, &a); pthread_join(st, NULL);
        if (!a.ok) all_sig = 0;
    }
    pthread_setspecific(g_key, (void *)SENT);
    fprintf(f, "    in-signal-handler (signo=%d, our handler installed=%d): %d/%d threads read slot correctly -> %s\n",
            g_sig_signo, sig_installed, all_sig ? sig_rounds : 0, sig_rounds, all_sig ? "all PASS" : "FAIL");

    int viable = (stub_read_ok == 1 && stub_write_ok == 1 && all_read && all_write && all_sig && all_nested);
    fprintf(f, "    => mechanism (c) viable: %s%s\n", viable ? "YES" : "NO", in_app ? " (in APK)" : " (exe)");
}

// ================= part (d) =================
static void part_d(FILE *f) {
    fprintf(f, "\n[d] TLS_SLOT_APP (tp+16), reserved for apps on API 29+\n");
    // Write a value directly at tp+16 and read it back on this and worker threads.
    uintptr_t tp = read_tp();
    uintptr_t *app_slot = (uintptr_t *)(tp + 16);
    uintptr_t saved = *app_slot;
    const uintptr_t APPV = 0x51070A9900000000ULL | 0x0BADCAFEULL;
    *app_slot = APPV;
    int ok_self = (*(uintptr_t *)(read_tp() + 16) == APPV);
    *app_slot = saved;
    fprintf(f, "    tp+16 writable and read-back on this thread: %s\n", ok_self ? "YES" : "NO");
    fprintf(f, "    note: usable as a SINGLE-load slot (mrs; str/ldr [x,#16]) but only one 8-byte word,\n");
    fprintf(f, "          and it is shared with anything else using TLS_SLOT_APP; the port needs 4 slots.\n");
}

int p02_main(const char *out, int in_app, int argc, char **argv) {
    (void)argc; (void)argv;
    g_f = res_open(out, "p02_slots.txt");
    if (!g_f) return 2;
    FILE *f = g_f;
    res_context(f, in_app);

    // locate libie.so: in the APK, our own dir (nativeLibraryDir); as an exe, /proc/self/exe's dir
    char libdir[512] = ".";
    Dl_info di;
    if (in_app && dladdr((void *)read_tp, &di) && di.dli_fname) {
        const char *s = strrchr(di.dli_fname, '/');
        if (s) { size_t l = (size_t)(s - di.dli_fname); if (l < sizeof libdir) { memcpy(libdir, di.dli_fname, l); libdir[l] = 0; } }
    } else {
        char exe[512]; ssize_t r = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (r > 0) { exe[r] = 0; char *s = strrchr(exe, '/'); if (s) { *s = 0; snprintf(libdir, sizeof libdir, "%s", exe); } }
    }
    fprintf(f, "libdir=%s\n", libdir);

    part_a(f);
    part_b(f, libdir);
    part_c(f, in_app);
    part_d(f);

    fflush(f); fclose(f);
    return g_chain_found ? 0 : 1;
}

// p02child: not used on this path (kept for symmetry with the dispatcher)
int p02_child_main(const char *out, int in_app, int argc, char **argv) {
    (void)in_app; (void)argc; (void)argv;
    FILE *f = res_open(out, "p02child.txt"); if (f) { fprintf(f, "unused\n"); fclose(f); }
    return 0;
}
