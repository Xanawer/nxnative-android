// mprotect failures in dirty-page tracking (nvn_dirty.c). Linux refuses a split past vm.max_map_count, and over several mappings it may change the first ones before failing on a later one (ENOMEM).
// The mprotect in nvn_dirty.c is replaced by t_mprotect here (as t_dirty_race does), which fails on request: completely (nothing changed) or partially (the first half of the range changed, then
// ENOMEM), for a set-read-only, for a set-writable over several pages, or for every set-writable (the undo of a failed set-read-only).
// Expected, whatever the implementation: no page is read-only while the tracker thinks it writable (no write faults into a loop, a host read() into the memory is not refused); a pool whose
// protection failed counts as written at every check, and the generation moves on every read while one exists (callers' "nothing written since" shortcuts); other pools behave as before; a pool
// sharing a page with the failed one hears that the page became writable; and when the undo fails too, the host stops with a red line rather than run on with unknown protections.
// A pool left holding a page read-only that it could not make writable counts as always written (another pool over the page may make it writable); a write fault waiting on a page whose
// set-read-only is being undone resumes; a fault on a page one pool could make writable resumes (also when that happens on another thread while the handler waits), one no pool could goes
// to the crash report; and the fault path never waits on stderr's lock.
// SIGALRM after 30 s (crash.c's handler) turns a fault loop into a failure instead of a hang
#define mprotect t_mprotect
#include "../nvn/nvn_dirty.c"
#undef mprotect
int mprotect(void *, size_t, int);
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/wait.h>
#include <time.h>

module_t g_mod[MAX_MOD];
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

static int g_fail;
#define EXPECT(name, want, got) do { \
    long long want_ = (long long)(want), got_ = (long long)(got); \
    printf("%-74s expected %-6lld → got %-6lld %s\n", name, want_, got_, want_ == got_ ? "✓" : "✗"); \
    if (want_ != got_) g_fail++; \
} while (0)

// F_EVERY: every set-writable fails; F_UPPER: one-page set-writables in the upper half of the armed range fail too; F_PAGE / F_ONCE: every / the first one-page set-writable of page g_rw_lo
// fails; F_HALF_WAIT: a set-read-only changes its first half, then fails only once a write fault on it has reached the handlers (note_fault) and 100 ms more have passed; F_OVERLAP: a one-page
// set-writable of g_rw_lo fails until a set-writable over several pages has landed, and that one waits (g_in_bulk) until such a one-page call has failed meanwhile (g_go), then 50 ms more;
// F_SECOND: the first one-page set-writable of g_rw_lo fails, the second starts a set-read-only on another thread (g_start_ro) and goes on once its mprotect has landed (g_ro_done).
// F_HOLD / F_HOLD_WAIT: a set-read-only lands, then returns (its pages still BUSY until then) 50 ms later / 20 ms after a wait on BUSY has timed out (busy_wait's g_busy_red)
enum { F_NONE, F_ALL, F_HALF, F_EVERY, F_UPPER, F_PAGE, F_ONCE, F_HALF_WAIT, F_OVERLAP, F_SECOND, F_HOLD, F_HOLD_WAIT };
static _Atomic int g_ro, g_rw; static uint8_t *g_rw_lo, *g_rw_hi; static size_t g_pg;
static _Atomic int g_partial, g_entered, g_note, g_rw_fails, g_in_bulk, g_go, g_bulk_done, g_rw_n, g_start_ro, g_ro_done;
int t_mprotect(void *a, size_t n, int prot) {
    size_t half = n / g_pg / 2 * g_pg;
    if (prot == PROT_READ) {
        int m = atomic_exchange(&g_ro, F_NONE);
        if ((m == F_HALF || m == F_HALF_WAIT) && half) mprotect(a, half, PROT_READ);
        if (m == F_HALF_WAIT) { atomic_store(&g_partial, 1); for (int i = 0; i < 5000 && !atomic_load(&g_entered); i++) usleep(1000); usleep(100000); }
        if (m == F_HOLD || m == F_HOLD_WAIT) {
            int rc = mprotect(a, n, prot); atomic_store(&g_ro_done, 1);
            for (int i = 0; m == F_HOLD_WAIT && i < 5000 && !atomic_load(&g_busy_red); i++) usleep(1000);
            usleep(m == F_HOLD ? 50000 : 20000); return rc;
        }
        if (m != F_NONE) { errno = ENOMEM; return -1; }
    } else if (prot == (PROT_READ | PROT_WRITE)) {
        int m = atomic_load(&g_rw);
        if (m == F_EVERY) { atomic_fetch_add(&g_rw_fails, 1); errno = ENOMEM; return -1; }
        if (m == F_OVERLAP && n > g_pg) {
            atomic_store(&g_in_bulk, 1); while (!atomic_load(&g_go)) usleep(1000);
            usleep(50000); int rc = mprotect(a, n, prot); atomic_store(&g_bulk_done, 1); return rc;
        }
        if (m == F_OVERLAP && (uint8_t *)a == g_rw_lo && !atomic_load(&g_bulk_done)) { if (atomic_load(&g_in_bulk)) atomic_store(&g_go, 1); errno = ENOMEM; return -1; }
        if (m == F_SECOND && n == g_pg && (uint8_t *)a == g_rw_lo) {
            int k = atomic_fetch_add(&g_rw_n, 1) + 1;
            if (k == 1) { errno = ENOMEM; return -1; }
            if (k == 2) { atomic_store(&g_start_ro, 1); for (int i = 0; i < 5000 && !atomic_load(&g_ro_done); i++) usleep(1000); }
        }
        if ((m == F_PAGE || m == F_ONCE) && n == g_pg && (uint8_t *)a == g_rw_lo) { if (m == F_ONCE) atomic_store(&g_rw, F_NONE); errno = ENOMEM; return -1; }
        if (m == F_UPPER && n == g_pg && (uint8_t *)a >= g_rw_hi) { errno = ENOMEM; return -1; }
        if ((m == F_ALL || m == F_HALF || m == F_UPPER) && n > g_pg) {
            if (m != F_UPPER) atomic_store(&g_rw, F_NONE);
            if (m != F_ALL && half) mprotect(a, half, PROT_READ | PROT_WRITE);
            errno = ENOMEM; return -1;
        }
    }
    return mprotect(a, n, prot);
}

static uint64_t faults(void) { uint64_t f, p; nvn_dirty_stats(&f, &p); return f; }
static uint64_t mp_calls(void) { uint64_t c, p, ns; nvn_dirty_mp_stats(&c, &p, &ns); return c; }
static uint8_t *map(size_t npg) { uint8_t *p = mmap(NULL, npg * g_pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); if (p == MAP_FAILED) { perror("mmap"); exit(2); } return p; }
static void write_all(uint8_t *p, size_t npg, uint8_t v) { for (size_t i = 0; i < npg; i++) ((volatile uint8_t *)p)[i * g_pg + 1] = v; }
static ssize_t read_into(uint8_t *p, size_t n) { int fd = open("/dev/zero", O_RDONLY); ssize_t r = read(fd, p, n); close(fd); return r; }
// Fault hooks around nvn_dirty.c's on_fault (crash.c calls them in registration order until one returns 1): note_fault, registered first, records that a write fault reached the handlers;
// after_fault, registered last by fault_logging's child, makes the page writable when on_fault could not, so that child's writer resumes and can say whether the handler returned
static int note_fault(void *addr, int is_write, void *uc) { (void)addr; (void)is_write; (void)uc; if (atomic_load(&g_note)) atomic_store(&g_entered, 1); return 0; }
static _Atomic int g_after, g_returned; static uint8_t *g_after_page;
static int after_fault(void *addr, int is_write, void *uc) { (void)addr; (void)is_write; (void)uc; if (!atomic_load(&g_after)) return 0; atomic_store(&g_returned, 1); mprotect(g_after_page, g_pg, PROT_READ | PROT_WRITE); return 1; }
// A child whose stderr goes to a pipe: runs fn, then the parent gets its exit status and what it wrote
static int child_run(void (*fn)(void), char *out, size_t cap) {
    int fds[2]; if (pipe(fds)) { perror("pipe"); exit(2); }
    fflush(stdout); fflush(stderr);
    pid_t pid = fork();
    if (!pid) { dup2(fds[1], 2); close(fds[0]); close(fds[1]); fn(); _exit(0); }
    close(fds[1]);
    size_t got = 0; ssize_t n;
    while (got < cap - 1 && (n = read(fds[0], out + got, cap - 1 - got)) > 0) got += (size_t)n;
    out[got] = 0; close(fds[0]);
    int status = 0; waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// A set-read-only of a whole pool fails (mode: completely or after changing the first half): every page writable, the pool always written, the generation moving; a clean pool unaffected
static void protect_fails(int mode, const char *how) {
    printf("-- set-read-only fails %s\n", how);
    enum { N = 8 };
    uint8_t *p = map(N), *c = map(2);
    nvn_dirty_add_range(p, N * g_pg); nvn_dirty_add_range(c, 2 * g_pg);
    nvn_dirty_protect(c, 2 * g_pg); uint64_t cst = nvn_dirty_now();   // the control pool, protected normally
    atomic_store(&g_ro, mode);
    nvn_dirty_protect(p, N * g_pg);
    uint64_t st = nvn_dirty_now(), f0 = faults();
    write_all(p, N, 0x11);
    EXPECT("writes to every page of the failed pool fault 0 times (all writable)", 0, faults() - f0);
    EXPECT("a host read() into the whole failed pool fills it", (long long)(N * g_pg), read_into(p, N * g_pg));
    EXPECT("the failed pool counts as written against a stamp taken after the writes", 1, nvn_dirty_since(p, N * g_pg, nvn_dirty_now()));
    EXPECT("... and against the stamp from before them", 1, nvn_dirty_since(p, g_pg, st));
    uint64_t n1 = nvn_dirty_now(), n2 = nvn_dirty_now();
    EXPECT("the generation moves between two reads while the pool exists", 1, n2 > n1);
    uint64_t c0 = mp_calls(); nvn_dirty_protect(p, N * g_pg);
    EXPECT("protecting the failed pool again calls mprotect 0 times", 0, mp_calls() - c0);
    EXPECT("the control pool, unwritten since its protect, is clean", 0, nvn_dirty_since(c, 2 * g_pg, cst));
    uint64_t f1 = faults(); c[1] = 1;
    EXPECT("a write to the control pool faults once (still protected)", 1, faults() - f1);
    EXPECT("... and makes it dirty", 1, nvn_dirty_since(c, g_pg, cst));
    nvn_dirty_remove_range(p);
    n1 = nvn_dirty_now(); n2 = nvn_dirty_now();
    EXPECT("after the failed pool is destroyed, two reads of the generation are equal", 1, n1 == n2);
    nvn_dirty_remove_range(c); munmap(p, N * g_pg); munmap(c, 2 * g_pg);
}

// Two pools sharing a page (A = pages 0-1, B = pages 1-3): A protected normally, then B's set-read-only fails after changing its first half. Page 1 is writable again, and A must know it
static void shared_page(void) {
    printf("-- set-read-only fails on a pool sharing a page with a protected one\n");
    uint8_t *m = map(4);
    nvn_dirty_add_range(m, 2 * g_pg); nvn_dirty_add_range(m + g_pg, 3 * g_pg);
    nvn_dirty_protect(m, 2 * g_pg); uint64_t sa = nvn_dirty_now();
    atomic_store(&g_ro, F_HALF);
    nvn_dirty_protect(m + g_pg, 3 * g_pg);
    uint64_t f0 = faults(); m[g_pg + 1] = 2;
    EXPECT("a write to the shared page faults 0 times (B's undo made it writable)", 0, faults() - f0);
    const void *ha = nvn_dirty_span(m, 2 * g_pg);   // A's own range, not B's (which counts as written anyway)
    EXPECT("A counts the shared page as written", 1, nvn_dirty_since_span(ha, m + g_pg, g_pg, sa));
    EXPECT("A's own page 0, unwritten, is clean", 0, nvn_dirty_since_span(ha, m, g_pg, sa));
    f0 = faults(); m[1] = 3;
    EXPECT("a write to A's page 0 faults once (still protected)", 1, faults() - f0);
    EXPECT("... and makes it dirty", 1, nvn_dirty_since_span(ha, m, g_pg, sa));
    nvn_dirty_remove_range(m + g_pg); nvn_dirty_remove_range(m); munmap(m, 4 * g_pg);
}

// A host pre-write over a protected pool (prep_write) whose set-writable over the run fails after changing its first half (and, with F_UPPER, page by page in the upper half too)
static void unprotect_fails(int mode, const char *how) {
    printf("-- set-writable fails %s\n", how);
    enum { N = 8 };
    uint8_t *p = map(N);
    nvn_dirty_add_range(p, N * g_pg); nvn_dirty_protect(p, N * g_pg); uint64_t st = nvn_dirty_now();
    g_rw_lo = p; g_rw_hi = p + N / 2 * g_pg; atomic_store(&g_rw, mode);
    dm_guest_write_prepare(p, N * g_pg);
    atomic_store(&g_rw, F_NONE);
    if (mode == F_HALF) {
        uint64_t f0 = faults();
        EXPECT("a host read() into the whole pool fills it (every page writable)", (long long)(N * g_pg), read_into(p, N * g_pg));
        write_all(p, N, 0x22);
        EXPECT("writes to every page fault 0 times", 0, faults() - f0);
        EXPECT("the pool counts as written", 1, nvn_dirty_since(p, N * g_pg, st));
        uint64_t c0 = mp_calls(); nvn_dirty_protect(p, N * g_pg); uint64_t s2 = nvn_dirty_now();
        EXPECT("the pool is not marked always written: protecting it again calls mprotect", 1, mp_calls() > c0);
        EXPECT("... and it is clean after that protect", 0, nvn_dirty_since(p, N * g_pg, s2));
    } else {
        uint64_t f0 = faults();
        write_all(p, N / 2, 0x33);
        EXPECT("writes to the lower half (unprotected one by one) fault 0 times", 0, faults() - f0);
        EXPECT("the lower half counts as written", 1, nvn_dirty_since(p, N / 2 * g_pg, st));
        EXPECT("the upper half, left read-only (and recorded so), counts as written: the pool is always written now", 1, nvn_dirty_since(g_rw_hi, N / 2 * g_pg, st));
        write_all(g_rw_hi, N / 2, 0x44);
        EXPECT("writes to the upper half fault once per page (state and protection agree)", N / 2, faults() - f0);
        EXPECT("... and it is dirty", 1, nvn_dirty_since(g_rw_hi, N / 2 * g_pg, st));
        uint64_t c0 = mp_calls(); nvn_dirty_protect(p, N * g_pg);
        EXPECT("protecting the pool again calls mprotect 0 times (always written)", 0, mp_calls() - c0);
    }
    nvn_dirty_remove_range(p); munmap(p, N * g_pg);
}

// The undo of a failed set-read-only fails too: the protection of those pages is unknown, so the host stops (in a child: a red line, FATAL, and crash.c's SIGABRT exit 128 + 6)
static void undo_fails(void) {
    printf("-- set-read-only fails and so does its undo\n");
    int fds[2]; if (pipe(fds)) { perror("pipe"); exit(2); }
    fflush(stdout);
    pid_t pid = fork();
    if (!pid) {
        dup2(fds[1], 2); close(fds[0]); close(fds[1]);
        uint8_t *p = map(4); nvn_dirty_add_range(p, 4 * g_pg);
        atomic_store(&g_ro, F_HALF); atomic_store(&g_rw, F_EVERY);
        nvn_dirty_protect(p, 4 * g_pg);
        fprintf(stderr, "returned\n"); _exit(0);
    }
    close(fds[1]);
    static char out[65536]; size_t got = 0; ssize_t n;
    while (got < sizeof out - 1 && (n = read(fds[0], out + got, sizeof out - 1 - got)) > 0) got += (size_t)n;
    out[got] = 0; close(fds[0]);
    int status = 0; waitpid(pid, &status, 0);
    EXPECT("the child stops with crash.c's abort exit (128 + SIGABRT)", 128 + SIGABRT, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    EXPECT("its log has a [RED] line about the undo", 1, strstr(out, "[RED] dirty-page tracking: a failed set-read-only") != NULL);
    EXPECT("its log has the FATAL line", 1, strstr(out, "FATAL dirty-page tracking: stopping") != NULL);
    EXPECT("nvn_dirty_protect did not return", 0, strstr(out, "returned\n") != NULL);
}

// A write from another thread faults on a page the set-read-only made read-only before it failed; the fault waits on BUSY while the run is made writable again, then must take the page as its
// own (stamped, not "never protected") and re-execute the write
static uint8_t *g_wpage;
static void *rr_writer(void *p) { (void)p; while (!atomic_load(&g_partial)) sched_yield(); ((volatile uint8_t *)g_wpage)[1] = 0x55; return NULL; }
static void rollback_race(void) {
    printf("-- a write faults on a page of a set-read-only that fails after changing the first half (the fault waits on BUSY through the undo)\n");
    enum { N = 4 };
    uint8_t *p = map(N); nvn_dirty_add_range(p, N * g_pg);
    g_wpage = p; atomic_store(&g_partial, 0); atomic_store(&g_entered, 0); atomic_store(&g_note, 1);
    pthread_t t; pthread_create(&t, NULL, rr_writer, NULL);
    atomic_store(&g_ro, F_HALF_WAIT);
    nvn_dirty_protect(p, N * g_pg);
    pthread_join(t, NULL); atomic_store(&g_note, 0);
    EXPECT("the writer's fault reached the handlers while the set-read-only was failing", 1, atomic_load(&g_entered));
    EXPECT("... and the write resumed after the undo (the handler took the page as its own)", 0x55, p[1]);
    EXPECT("the pool counts as written", 1, nvn_dirty_since(p, g_pg, nvn_dirty_now()));
    nvn_dirty_remove_range(p); munmap(p, N * g_pg);
}

// A = pages 0-1, B = pages 1-2, both protected; a host pre-write over B's pages: A's one-page set-writable of the shared page 1 fails (A keeps it read-only), then B's run over pages 1-2 succeeds
// and page 1 is writable after all. A must not count it clean
static void shared_unprotect(void) {
    printf("-- a host pre-write over a page shared by two pools: one's set-writable fails, the other's succeeds\n");
    uint8_t *m = map(3);
    nvn_dirty_add_range(m, 2 * g_pg); nvn_dirty_add_range(m + g_pg, 2 * g_pg);
    nvn_dirty_protect(m, 2 * g_pg); nvn_dirty_protect(m + g_pg, 2 * g_pg);
    const void *ha = nvn_dirty_span(m, 2 * g_pg), *hb = nvn_dirty_span(m + g_pg, 2 * g_pg); uint64_t st = nvn_dirty_now();
    g_rw_lo = m + g_pg; atomic_store(&g_rw, F_PAGE);
    dm_guest_write_prepare(m + g_pg, 2 * g_pg);
    uint64_t f0 = faults(); m[g_pg + 1] = 42;
    atomic_store(&g_rw, F_NONE);
    EXPECT("the write to the shared page faults 0 times (B made it writable)", 0, faults() - f0);
    EXPECT("A, whose set-writable failed, counts the shared page as written", 1, nvn_dirty_since_span(ha, m + g_pg, g_pg, st));
    EXPECT("... and its own page 0 too (always written now)", 1, nvn_dirty_since_span(ha, m, g_pg, st));
    EXPECT("B counts the shared page as written", 1, nvn_dirty_since_span(hb, m + g_pg, g_pg, st));
    uint64_t c0 = mp_calls(); nvn_dirty_protect(m, 2 * g_pg);
    EXPECT("protecting A again calls mprotect 0 times", 0, mp_calls() - c0);
    nvn_dirty_remove_range(m + g_pg); nvn_dirty_remove_range(m); munmap(m, 3 * g_pg);
}

// The same pools, a guest write fault on the shared page: A's set-writable fails, B's succeeds, so the page is writable and the write resumes, A counting as always written
static void shared_fault(void) {
    printf("-- a write fault on a page shared by two pools: one's set-writable fails, the other's succeeds\n");
    uint8_t *m = map(3);
    nvn_dirty_add_range(m, 2 * g_pg); nvn_dirty_add_range(m + g_pg, 2 * g_pg);
    nvn_dirty_protect(m, 2 * g_pg); nvn_dirty_protect(m + g_pg, 2 * g_pg);
    const void *ha = nvn_dirty_span(m, 2 * g_pg), *hb = nvn_dirty_span(m + g_pg, 2 * g_pg); uint64_t st = nvn_dirty_now();
    g_rw_lo = m + g_pg; atomic_store(&g_rw, F_ONCE);
    uint64_t f0 = faults(); ((volatile uint8_t *)m)[g_pg + 1] = 43;
    atomic_store(&g_rw, F_NONE);
    EXPECT("the write resumed: one fault, released by B", 1, faults() - f0);
    EXPECT("... and landed", 43, m[g_pg + 1]);
    EXPECT("A, whose set-writable failed, counts the page as written", 1, nvn_dirty_since_span(ha, m + g_pg, g_pg, st));
    EXPECT("B counts it as written", 1, nvn_dirty_since_span(hb, m + g_pg, g_pg, st));
    nvn_dirty_remove_range(m + g_pg); nvn_dirty_remove_range(m); munmap(m, 3 * g_pg);
}

// A = pages 0-1, B = pages 1-2, both protected: a host pre-write over pages 1-2 fails A's set-writable of the shared page 1, then holds B's pages BUSY in one set-writable over both; meanwhile
// a guest write to page 1 faults, the handler's set-writable for A fails too (the page is still read-only), it waits on B's BUSY page, and B's run lands. The page is writable now, so the write
// must resume, not go to the crash report. In a child, so a crash is a failed check: exit bits 1 = resumed and landed, 2 = the handler's A call failed while B's run was held, 4 = A counts the
// page as written, 8 = B does
static void *ov_prep(void *m) { dm_guest_write_prepare((uint8_t *)m + g_pg, 2 * g_pg); return NULL; }
static void overlap_child(void) {
    uint8_t *m = map(3);
    nvn_dirty_add_range(m, 2 * g_pg); nvn_dirty_add_range(m + g_pg, 2 * g_pg);   // A scanned before B (lower slot), by the pre-write and by the handler
    nvn_dirty_protect(m, 2 * g_pg); nvn_dirty_protect(m + g_pg, 2 * g_pg);
    const void *ha = nvn_dirty_span(m, 2 * g_pg), *hb = nvn_dirty_span(m + g_pg, 2 * g_pg); uint64_t st = nvn_dirty_now();
    g_rw_lo = m + g_pg; atomic_store(&g_rw, F_OVERLAP);
    pthread_t t; pthread_create(&t, NULL, ov_prep, m);
    while (!atomic_load(&g_in_bulk)) usleep(1000);
    ((volatile uint8_t *)m)[g_pg + 1] = 45;
    pthread_join(t, NULL); atomic_store(&g_rw, F_NONE);
    _exit((m[g_pg + 1] == 45) | atomic_load(&g_go) << 1 | nvn_dirty_since_span(ha, m + g_pg, g_pg, st) << 2 | nvn_dirty_since_span(hb, m + g_pg, g_pg, st) << 3);
}
static void overlap(void) {
    printf("-- a write fault on a shared page: one pool's set-writable fails, the other's lands on a host thread while the handler waits on it\n");
    static char out[65536]; int st = child_run(overlap_child, out, sizeof out);
    EXPECT("the child's write resumed instead of crashing (exit, not 128 + signal)", 1, st >= 0 && st < 16);
    EXPECT("... it landed", 1, st >= 0 && st < 16 && (st & 1));
    EXPECT("the handler's set-writable for A failed while B's run was held", 1, st >= 0 && st < 16 && (st & 2));
    EXPECT("A counts the page as written", 1, st >= 0 && st < 16 && (st & 4));
    EXPECT("B counts it as written", 1, st >= 0 && st < 16 && (st & 8));
    EXPECT("no CRASH report", 0, strstr(out, "[dm] CRASH sig=") != NULL);
    if (st < 0 || st >= 16) printf("child exit %d, stderr:\n%s", st, out);
}

// A = pages 0-1, B = pages 1-2, page 1 shared; a guest write to page 1 faults, A's set-writable fails and the handler's direct set-writable makes the page writable, while B is being set
// read-only on another thread: (timeout) B's mprotect landed before the fault and B stays BUSY until the handler's wait on it has timed out; (late) B's set-read-only starts after the handler
// passed B, its mprotect lands just before the direct one and its PROT 50 ms later. Either way B would end up "protected" over a writable page and miss writes: it must count page 1 as
// written (always written after the timeout, or released again and stamped), and its state must be true. In a child: exit bits 1 = the write landed, 2 = B counts the page as written since
// before the fault, 4 = B's page 1 state is NONE, 8 = a write there faults 0 times (writable, as the states say), 16 = protected again and written, B counts it as written, 32 = B always
// written (timeout) / the fault took under 1 s (late, no wait timed out)
static void *b_protect(void *m) { while (!atomic_load(&g_start_ro)) usleep(1000); nvn_dirty_protect((uint8_t *)m + g_pg, 2 * g_pg); return NULL; }
static void b_race_child(int late) {
    uint8_t *m = map(3);
    nvn_dirty_add_range(m, 2 * g_pg); nvn_dirty_add_range(m + g_pg, 2 * g_pg);   // A scanned before B (lower slot)
    nvn_dirty_protect(m, 2 * g_pg);
    const void *hb = nvn_dirty_span(m + g_pg, 2 * g_pg); range_t *rb = (range_t *)hb; uint64_t st = nvn_dirty_now();
    atomic_store(&g_busy_red, 0); atomic_store(&g_ro_done, 0); atomic_store(&g_rw_n, 0); atomic_store(&g_start_ro, !late);
    atomic_store(&g_ro, late ? F_HOLD : F_HOLD_WAIT);
    pthread_t t; pthread_create(&t, NULL, b_protect, m);
    if (!late) while (!atomic_load(&g_ro_done)) usleep(1000);
    g_rw_lo = m + g_pg; atomic_store(&g_rw, late ? F_SECOND : F_ONCE);
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    ((volatile uint8_t *)m)[g_pg + 1] = 46;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    pthread_join(t, NULL); atomic_store(&g_rw, F_NONE);
    int landed = m[g_pg + 1] == 46, dirty = nvn_dirty_since_span(hb, m + g_pg, g_pg, st), none = atomic_load(&rb->state[0]) == PG_NONE;
    uint64_t f0 = faults(); ((volatile uint8_t *)m)[g_pg + 2] = 47; int quiet = faults() == f0;
    uint64_t s2 = nvn_dirty_now(); nvn_dirty_protect(m + g_pg, 2 * g_pg); ((volatile uint8_t *)m)[g_pg + 1] = 48; int dirty2 = nvn_dirty_since_span(hb, m + g_pg, g_pg, s2);
    int last = late ? (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 < 1000 : atomic_load(&rb->bad);
    _exit(landed | dirty << 1 | none << 2 | quiet << 3 | dirty2 << 4 | last << 5);
}
static void b_timeout_child(void) { b_race_child(0); }
static void b_late_child(void) { b_race_child(1); }
static void b_race(int late) {
    printf("-- a write fault on a shared page whose set-writable fails for one pool, while the other pool is being set read-only (%s)\n",
           late ? "it starts after the handler passed it" : "the handler's wait on it times out");
    static char out[65536]; int st = child_run(late ? b_late_child : b_timeout_child, out, sizeof out), ok = st >= 0 && st < 64;
    EXPECT("the child's write resumed instead of crashing (exit, not 128 + signal)", 1, ok);
    EXPECT("... it landed", 1, ok && (st & 1));
    EXPECT("B counts the page as written since before the fault", 1, ok && (st & 2));
    EXPECT("B's state of the page is NONE (released again by the handler)", 1, ok && (st & 4));
    EXPECT("... and true: a write to the page faults 0 times", 1, ok && (st & 8));
    EXPECT("B protected again, then written: B counts the page as written", 1, ok && (st & 16));
    EXPECT(late ? "no wait timed out: the fault took under 1 s" : "B, whose state was unknown after the timeout, counts as always written", 1, ok && (st & 32));
    if (!ok) printf("child exit %d, stderr:\n%s", st, out);
}

// Both pools' set-writables fail, and so does the handler's direct one after them: the page is still read-only, so the fault goes to the crash report instead of re-executing forever.
// report_fails, a fault hook after on_fault, says how many set-writables failed before the report
static int report_fails(void *addr, int is_write, void *uc) {
    (void)addr; (void)is_write; (void)uc; char b[64]; int n = snprintf(b, sizeof b, "set-writables failed: %d\n", atomic_load(&g_rw_fails)); if (write(2, b, (size_t)n) < 0) {} return 0;
}
static void both_fail_child(void) {
    uint8_t *m = map(3);
    nvn_dirty_add_range(m, 2 * g_pg); nvn_dirty_add_range(m + g_pg, 2 * g_pg);
    nvn_dirty_protect(m, 2 * g_pg); nvn_dirty_protect(m + g_pg, 2 * g_pg);
    dm_fault_hook_register(report_fails);
    atomic_store(&g_rw_fails, 0); atomic_store(&g_rw, F_EVERY);
    ((volatile uint8_t *)m)[g_pg + 1] = 44;
    fprintf(stderr, "returned\n");
}
static void both_fail(void) {
    printf("-- a write fault on a shared page whose set-writable fails for both pools\n");
    static char out[65536]; int st = child_run(both_fail_child, out, sizeof out);
    EXPECT("the child stops in crash.c's report (128 + SIGBUS or SIGSEGV)", 1, st == 128 + SIGBUS || st == 128 + SIGSEGV);
    EXPECT("... which says CRASH", 1, strstr(out, "[dm] CRASH sig=") != NULL);
    EXPECT("the handler tried 3 set-writables (A, B, the direct one) and all failed", 1, strstr(out, "set-writables failed: 3\n") != NULL);
    EXPECT("the write was not re-executed into a loop (no 'returned')", 0, strstr(out, "returned\n") != NULL);
}

// The fault path while another thread holds stderr's lock (a thread stopped in the middle of a log line): a failed set-writable in the fault handler, and a page stuck BUSY past the wait limit,
// report red with write(2) and return; with stdio they would wait for the lock, the page BUSY all the while. Both lines are LOG's text exactly
static uint8_t *g_fl_p, *g_fl_q;
static void *fl_writer(void *p) { (void)p; ((volatile uint8_t *)g_fl_p)[1] = 1; return NULL; }
static _Atomic int g_busy_back = -1;
static void *fl_busy(void *p) { (void)p; atomic_store(&g_busy_back, on_fault(g_fl_q + 1, 1, NULL)); return NULL; }
static void fault_logging_child(void) {
    nvn_dirty_add_range(g_fl_p, g_pg); nvn_dirty_protect(g_fl_p, g_pg);
    nvn_dirty_add_range(g_fl_q, g_pg); atomic_store(&find_span((uintptr_t)g_fl_q, g_pg)->state[0], PG_BUSY);   // as if a thread stopped while changing its protection
    dm_fault_hook_register(after_fault); g_after_page = g_fl_p; atomic_store(&g_after, 1);
    atomic_store(&g_mp_red, 0); atomic_store(&g_busy_red, 0);   // only the first failure is reported: earlier scenarios used it up
    atomic_store(&g_rw, F_EVERY);
    flockfile(stderr);
    pthread_t t, u; pthread_create(&t, NULL, fl_writer, NULL);
    for (int i = 0; i < 1200 && !atomic_load(&g_returned); i++) usleep(1000);
    int fault_back = atomic_load(&g_returned);
    pthread_create(&u, NULL, fl_busy, NULL);
    for (int i = 0; i < 3000 && atomic_load(&g_busy_back) < 0; i++) usleep(1000);
    int busy_back = atomic_load(&g_busy_back) == 0;
    funlockfile(stderr); pthread_join(t, NULL); pthread_join(u, NULL);
    _exit(fault_back | busy_back << 1);
}
static void fault_logging(void) {
    printf("-- the fault path with stderr's lock held by another thread\n");
    g_fl_p = map(1); g_fl_q = map(1);
    static char out[4096]; int st = child_run(fault_logging_child, out, sizeof out);
    EXPECT("a write fault whose set-writable fails: the handler returned within 1.2 s", 1, st >= 0 && (st & 1));
    EXPECT("a page stuck BUSY: on_fault returned 0 after the 2 s limit, within 3 s", 1, st >= 0 && (st & 2));
    char want[1024]; snprintf(want, sizeof want, "[dm] [RED] dirty-page tracking: mprotect(%p, %#zx, writable) failed (errno %d): the tracker makes those pages' states true again and counts the pool "
        "as always written, or stops (only the first failure is reported)\n[dm] [RED] dirty-page tracking: page %p stuck 'busy' for over %d ms (the thread changing its protection has not "
        "finished), no longer waiting\n", (void *)g_fl_p, g_pg, ENOMEM, (void *)g_fl_q, BUSY_WAIT_MS);
    EXPECT("stderr got the two red lines, as LOG would print them", 1, !strcmp(out, want));
    if (strcmp(out, want)) printf("got:\n%swanted:\n%s", out, want);
}

int main(void) {
    dm_fault_hook_register(note_fault);   // before nvn_dirty.c's on_fault (its first nvn_dirty_add_range)
    crash_install();
    alarm(30);
    g_pg = (size_t)getpagesize();
    printf("t_dirty_fail: page size %zu\n", g_pg);
    protect_fails(F_HALF, "after changing the first half of the pool");
    protect_fails(F_ALL, "completely");
    shared_page();
    unprotect_fails(F_HALF, "after changing the first half");
    unprotect_fails(F_UPPER, "after changing the first half, and page by page in the upper half");
    undo_fails();
    rollback_race();
    shared_unprotect();
    shared_fault();
    overlap();
    b_race(0);
    b_race(1);
    both_fail();
    fault_logging();
    printf("t_dirty_fail: %s\n", g_fail ? "FAILED" : "all passed"); return g_fail != 0;
}
