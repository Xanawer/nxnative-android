// nvn_dirty_protect_wait (nvn_dirty.c): a page another thread holds BUSY is waited for and then protected. Why it exists: the Vulkan backend's B (nvn_vk.c b_commit) stamps, protects and then
// copies; a page an unprotect has stamped but not yet set NONE was skipped by nvn_dirty_protect and ended writable with nothing newer than the stamp, so the next write was never seen.
// How it is staged (as t_dirty_race does): nvn_dirty.c's mprotect is t_mprotect here. Thread B makes a host pre-write unprotect of a protected page (prep_write, as the movie decoder or a file
// read does): it takes the page BUSY, and its set-writable mprotect is held until the test lets it go. Each case has a pool of its own (one host page).
//   1. Control, nvn_dirty_protect: it returns at once and the page ends as B leaves it, writable (the case above).
//   2. nvn_dirty_protect_wait: it returns 1, only after B was let go, and the page is read-only (a file read into it fails); a write then faults and is seen.
//   3. B held past BUSY_WAIT_MS (2 s): it gives up and returns 0 (one red line, expected), and the pool counts as always written: nvn_dirty_since says 1, and the call returns 1 from then on.
// Expected values do not depend on the implementation: page states are read from the tracker's table, protection from the kernel.
#define mprotect t_mprotect
#include "../nvn/nvn_dirty.c"
#undef mprotect
int mprotect(void *, size_t, int);
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>

module_t g_mod[MAX_MOD];
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

static int g_fail;
#define EXPECT(name, want, got) do { \
    long long want_ = (long long)(want), got_ = (long long)(got); \
    printf("%-72s expected %-4lld → got %-4lld %s\n", name, want_, got_, want_ == got_ ? "✓" : "✗"); \
    if (want_ != got_) g_fail++; \
} while (0)

static _Atomic int g_hold, g_in, g_let;   // g_hold: the next set-writable mprotect is held (g_in: it is) until g_let
int t_mprotect(void *a, size_t n, int prot) {
    if ((prot & PROT_WRITE) && atomic_exchange(&g_hold, 0)) { atomic_store(&g_in, 1); while (!atomic_load(&g_let)) usleep(200); }
    return mprotect(a, n, prot);
}
static void *thread_b(void *page) { prep_write(page, 16); return NULL; }
static void *letter(void *us) { usleep((useconds_t)(uintptr_t)us); atomic_store(&g_let, 1); return NULL; }
static pthread_t start_b(void *page) {   // B holds the page BUSY when this returns
    atomic_store(&g_in, 0); atomic_store(&g_let, 0); atomic_store(&g_hold, 1);
    pthread_t t; pthread_create(&t, NULL, thread_b, page);
    while (!atomic_load(&g_in)) usleep(200);
    return t;
}
static int state_of(void *page) { range_t *r = find_span((uintptr_t)page, 1); return r ? atomic_load(&r->state[((uintptr_t)page - r->base) >> g_shift]) : -1; }
static int writable(void *page) { int fd = open("/dev/zero", O_RDONLY); ssize_t n = read(fd, page, 16); close(fd); return n == 16; }   // a kernel write: EFAULT on a read-only page
static uint64_t ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static uint8_t *pool(size_t pg) {
    uint8_t *m = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); exit(2); }
    nvn_dirty_add_range(m, pg); nvn_dirty_protect(m, pg); return m;
}

int main(void) {
    crash_install();
    size_t pg = (size_t)getpagesize();

    uint8_t *p1 = pool(pg);
    EXPECT("1. the page is protected to begin with (state PROT)", PG_PROT, state_of(p1));
    pthread_t b = start_b(p1);
    EXPECT("   B's unprotect holds it (state BUSY)", PG_BUSY, state_of(p1));
    nvn_dirty_protect(p1, pg);
    atomic_store(&g_let, 1); pthread_join(b, NULL);
    EXPECT("   control: after nvn_dirty_protect and B, the page is writable (state NONE)", PG_NONE, state_of(p1));
    EXPECT("   control: a file read into it succeeds (not protected)", 1, writable(p1));

    uint8_t *p2 = pool(pg);
    b = start_b(p2);
    EXPECT("2. B's unprotect holds the page (state BUSY)", PG_BUSY, state_of(p2));
    pthread_t l; pthread_create(&l, NULL, letter, (void *)(uintptr_t)100000);
    int rc = nvn_dirty_protect_wait(p2, pg), let = atomic_load(&g_let);
    pthread_join(l, NULL); pthread_join(b, NULL);
    EXPECT("   nvn_dirty_protect_wait returns 1", 1, rc);
    EXPECT("   it returned only after B was let go (it waited)", 1, let);
    EXPECT("   the page ends protected (state PROT)", PG_PROT, state_of(p2));
    EXPECT("   a file read into it fails (read-only)", 0, writable(p2));
    uint64_t s = nvn_dirty_now(), f0 = atomic_load(&g_faults);
    ((volatile uint8_t *)p2)[1] = 0x5a;
    EXPECT("   a write into it faults once", 1, atomic_load(&g_faults) - f0);
    EXPECT("   and is seen (nvn_dirty_since against a stamp from after the protect)", 1, nvn_dirty_since(p2, 16, s));
    EXPECT("   and lands", 0x5a, p2[1]);

    uint8_t *p3 = pool(pg);
    b = start_b(p3);
    pthread_create(&l, NULL, letter, (void *)(uintptr_t)((BUSY_WAIT_MS + 500) * 1000));
    printf("3. (the red line about a page stuck 'busy' that follows is expected)\n"); fflush(stdout);
    uint64_t t0 = ms(); rc = nvn_dirty_protect_wait(p3, pg); uint64_t dt = ms() - t0; let = atomic_load(&g_let);
    pthread_join(l, NULL); pthread_join(b, NULL);
    EXPECT("   B held past the limit: nvn_dirty_protect_wait returns 0", 0, rc);
    EXPECT("   it gave up before B was let go", 0, let);
    EXPECT("   after waiting at least BUSY_WAIT_MS", 1, dt >= BUSY_WAIT_MS);
    EXPECT("   the pool counts as always written (nvn_dirty_since 1 against a fresh stamp)", 1, nvn_dirty_since(p3, 16, nvn_dirty_now()));
    EXPECT("   nvn_dirty_protect_wait on it returns 1 from then on", 1, nvn_dirty_protect_wait(p3, pg));
    EXPECT("   and leaves it writable (always written: never protected again)", 1, writable(p3));
    EXPECT("   the other pools are not affected (pool 2: nothing written since a fresh stamp)", 0, nvn_dirty_since(p2, 16, nvn_dirty_now()));

    printf("t_dirty_wait: %s\n", g_fail ? "FAILED" : "all passed"); return g_fail != 0;
}
