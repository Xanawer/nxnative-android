// The signal guard of crash.c on Linux / Android (built by dm/android/cmake --tests; there is nothing to test on Darwin). Every thread that runs guest code rescans the handlers as it starts
// (crash_thread_init), so a handler without SA_ONSTACK installed after startup is blocked on threads that start later; bionic's own handlers (36, 39) are put on the alternate stack by the first
// scan; bionic's cross-thread handshakes (33, 40) stay unblocked whatever their handler; what a scan stops blocking is unblocked on threads that start later even when their creator blocks it,
// except the process's own blocked signals (here SIGPIPE, as ART blocks SIGQUIT), which stay blocked after a scan blocked them too and stopped; and android_run_on_all_threads (bionic's, signal 40
// to every thread) returns with guarded threads alive. SIGALRM after 30 s (crash.c's handler) turns a hang into a failure
#include "../dm.h"
#include <dlfcn.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

module_t g_mod[MAX_MOD];
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);
void crash_thread_init(void);

static int g_fail;
#define EXPECT(name, want, got) do { \
    long long want_ = (long long)(want), got_ = (long long)(got); \
    printf("%-78s expected %-4lld → got %-4lld %s\n", name, want_, got_, want_ == got_ ? "✓" : "✗"); \
    if (want_ != got_) g_fail++; \
} while (0)
#define BIT(s) (1ull << ((s) - 1))
struct kact { uintptr_t handler; unsigned long flags; uintptr_t restorer; uint64_t mask; };

static uint64_t mask_now(void) { uint64_t m = 0; syscall(SYS_rt_sigprocmask, SIG_BLOCK, NULL, &m, sizeof m); return m; }
static void handler(int s) { (void)s; }
static void set_raw(int s, uintptr_t h, unsigned long flags) { struct kact k = {h, flags, 0, 0}; if (syscall(SYS_rt_sigaction, s, &k, NULL, sizeof k.mask)) perror("rt_sigaction"); }

// A thread that runs guest code: crash_thread_init, then report its mask; optionally start another such thread from inside it, after a step, before it reports
typedef struct { uint64_t mask, child_mask; void (*between)(void); int park; } th_t;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER; static int g_release;
static void *child(void *p) { th_t *t = p; crash_thread_init(); t->child_mask = mask_now(); return NULL; }
static void *guest(void *p) {
    th_t *t = p; crash_thread_init(); t->mask = mask_now();
    if (t->between) { t->between(); pthread_t c; pthread_create(&c, NULL, child, t); pthread_join(c, NULL); }
    if (t->park) { pthread_mutex_lock(&g_mu); while (!g_release) pthread_cond_wait(&g_cv, &g_mu); pthread_mutex_unlock(&g_mu); }
    return NULL;
}
static void run(th_t *t) { pthread_t th; pthread_create(&th, NULL, guest, t); pthread_join(th, NULL); }
static void usr1_gone(void) { signal(SIGUSR1, SIG_IGN); signal(SIGPIPE, SIG_IGN); }
// The handler the kernel runs for s (raw: what the scan reads): 1 = in libc.so with SA_ONSTACK
static int bionic_onstack(int s) {
    struct kact k; Dl_info di = {0};
    if (syscall(SYS_rt_sigaction, s, NULL, &k, sizeof k.mask) || k.handler <= 1 || !dladdr((void *)k.handler, &di) || !di.dli_fname) return 0;
    const char *lib = strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname;
    return !strcmp(lib, "libc.so") && (k.flags & SA_ONSTACK);
}

static _Atomic int g_ran;
static bool count_thread(void *arg) { (void)arg; atomic_fetch_add(&g_ran, 1); return true; }

int main(void) {
    sigset_t pipe_set; sigemptyset(&pipe_set); sigaddset(&pipe_set, SIGPIPE); pthread_sigmask(SIG_BLOCK, &pipe_set, NULL);   // the process's own (ART blocks SIGQUIT, SIGUSR1, SIGPIPE)
    crash_install();
    alarm(30);
    EXPECT("after crash_install, 36 (bionic's profiler signal) runs libc.so's handler with SA_ONSTACK", 1, bionic_onstack(36));
    EXPECT("... and 39 (bionic's fdtrack signal)", 1, bionic_onstack(39));

    struct sigaction sa = {.sa_handler = handler}; sigemptyset(&sa.sa_mask);   // no SA_ONSTACK: installed after startup, as a library might
    sigaction(SIGUSR1, &sa, NULL); sigaction(SIGRTMIN + 3, &sa, NULL); sigaction(SIGPIPE, &sa, NULL);   // SIGPIPE: blocked by the process (the first scan's mask) as well
    int rt = SIGRTMIN + 3;
    th_t a = {0}; run(&a);
    EXPECT("a handler installed after startup: blocked on a thread that starts later", 1, (a.mask & BIT(SIGUSR1)) != 0);
    EXPECT("... an application real-time signal too", 1, (a.mask & BIT(rt)) != 0);
    EXPECT("... and SIGPIPE, which the process blocks anyway", 1, (a.mask & BIT(SIGPIPE)) != 0);

    set_raw(40, (uintptr_t)handler, 0); set_raw(33, (uintptr_t)handler, 0);   // handshake signals with handlers lacking SA_ONSTACK (40's always does)
    th_t b = {0}; run(&b);
    EXPECT("40 (android_run_on_all_threads) with a handler lacking SA_ONSTACK: not blocked", 0, (b.mask & BIT(40)) != 0);
    EXPECT("33 (libunwindstack's thread unwind): not blocked", 0, (b.mask & BIT(33)) != 0);
    set_raw(40, 0, 0); set_raw(33, 0, 0);

    th_t c = {.between = usr1_gone}; run(&c);   // c's scan blocks SIGUSR1 and SIGPIPE (handlers lacking SA_ONSTACK), then both are ignored and c starts a thread, whose scan no longer blocks them
    EXPECT("the creator still blocks SIGUSR1", 1, (c.mask & BIT(SIGUSR1)) != 0);
    EXPECT("SIGUSR1 ignored now: the thread it starts unblocks it (inherited blocked)", 0, (c.child_mask & BIT(SIGUSR1)) != 0);
    EXPECT("... and still blocks the application real-time signal", 1, (c.child_mask & BIT(rt)) != 0);
    EXPECT("SIGPIPE ignored now too, but the process blocked it before the first scan: still blocked", 1, (c.child_mask & BIT(SIGPIPE)) != 0);

    { uint64_t inherit = BIT(40) | BIT(33); syscall(SYS_rt_sigprocmask, SIG_BLOCK, &inherit, NULL, sizeof inherit); }   // a creator that blocks the handshakes (only raw syscalls can)
    th_t d = {0}; run(&d);
    { uint64_t inherit = BIT(40) | BIT(33); syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, &inherit, NULL, sizeof inherit); }
    EXPECT("handshakes inherited blocked: unblocked on a thread that runs guest code", 0, (d.mask & (BIT(40) | BIT(33))) != 0);

    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 1000; i++) crash_thread_init();   // a rescan and the mask on every call
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("1000 rescans: %.1f µs each\n", ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1000 / 1e3);

    bool (*run_all)(bool (*)(void *), void *) = (bool (*)(bool (*)(void *), void *))dlsym(RTLD_DEFAULT, "android_run_on_all_threads");
    if (!run_all) printf("android_run_on_all_threads: not found in this C library (skipped)\n");
    else {
        enum { NT = 4 }; th_t p[NT]; pthread_t th[NT];
        for (int i = 0; i < NT; i++) { p[i] = (th_t){.park = 1}; pthread_create(&th[i], NULL, guest, &p[i]); }
        usleep(100000);
        bool ok = run_all(count_thread, NULL);   // signal 40 to every thread, waiting for each (with the thread creation lock held): hangs if a guarded thread blocks 40
        EXPECT("android_run_on_all_threads returns true with guarded threads parked", 1, ok);
        EXPECT("... having run on this thread and every parked one (at least)", 1, atomic_load(&g_ran) >= NT + 1);
        pthread_mutex_lock(&g_mu); g_release = 1; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
        for (int i = 0; i < NT; i++) pthread_join(th[i], NULL);
    }
    printf("t_sig_guard: %s\n", g_fail ? "FAILED" : "all passed"); return g_fail != 0;
}
