// P0.3 scratch below sp (no red zone on Linux).
// A loop thread runs on a 64 KB mmap'd stack (scratch_loop.S) with sp fixed, writing sentinels
// into [sp-112, sp) and checking them continuously. Two stressors run concurrently:
//   - a signal thread tgkill's SIGUSR2 at a high rate (handler: SA_SIGINFO|SA_ONSTACK|SA_RESTART);
//   - a protect thread keeps mprotect'ing a tracked page RO; the loop writes it, faulting, and
//     the SIGSEGV handler unprotects it (the dirty-tracker pattern).
// Variants (argv): "alt" (loop thread has a sigaltstack: expect 0 corruption),
//   "noalt" (no sigaltstack: expect corruption, proving the probe can detect it),
//   default runs "alt". In the APK, ART's libsigchain sits in front of the handlers.
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include "cpuprobe.h"

extern void probe_sp_loop(volatile int *stop, uint64_t *corrupt, uint64_t *iter, volatile uint64_t *tracked);

static pid_t g_loop_tid;
static volatile int g_loop_ready, g_stop;
static uint64_t g_corrupt, g_iter;
static volatile uint64_t *g_tracked;    // one page, flipped RO by the protect thread
static long g_ps;
static _Atomic uint64_t g_usr2_delivered, g_segv_faults, g_segv_other;
static int g_use_altstack;

static pid_t gettid_(void) { return (pid_t)syscall(SYS_gettid); }

static void usr2_handler(int s, siginfo_t *si, void *uc) {
    (void)s; (void)si; (void)uc;
    g_usr2_delivered++;
}

static void segv_handler(int s, siginfo_t *si, void *uc) {
    (void)s; (void)uc;
    uintptr_t a = (uintptr_t)si->si_addr;
    uintptr_t base = (uintptr_t)g_tracked;
    if (a >= base && a < base + (uintptr_t)g_ps) {
        mprotect((void *)base, (size_t)g_ps, PROT_READ | PROT_WRITE);   // unprotect, like nvn_dirty.c
        g_segv_faults++;
        return;   // retry the faulting store
    }
    // a genuine fault (e.g. sentinels written past the stack): record and abort cleanly
    g_segv_other++;
    _exit(139);
}

static void *loop_thread(void *arg) {
    (void)arg;
    g_loop_tid = gettid_();
    if (g_use_altstack) {
        stack_t ss = { .ss_sp = malloc(SIGSTKSZ > 65536 ? SIGSTKSZ : 65536),
                       .ss_size = (size_t)(SIGSTKSZ > 65536 ? SIGSTKSZ : 65536), .ss_flags = 0 };
        sigaltstack(&ss, NULL);
    } else {
        stack_t off = { .ss_flags = SS_DISABLE }; sigaltstack(&off, NULL);
    }
    __atomic_store_n(&g_loop_ready, 1, __ATOMIC_SEQ_CST);
    probe_sp_loop(&g_stop, &g_corrupt, &g_iter, g_tracked);
    return NULL;
}

static void *signal_thread(void *arg) {
    (void)arg;
    while (!__atomic_load_n(&g_loop_ready, __ATOMIC_SEQ_CST)) { }
    pid_t pid = getpid();
    while (!g_stop) {
        syscall(SYS_tgkill, pid, g_loop_tid, SIGUSR2);
        for (volatile int i = 0; i < 200; i++) { }   // pace, but high rate
    }
    return NULL;
}

static void *protect_thread(void *arg) {
    (void)arg;
    while (!__atomic_load_n(&g_loop_ready, __ATOMIC_SEQ_CST)) { }
    while (!g_stop) {
        mprotect((void *)g_tracked, (size_t)g_ps, PROT_READ);   // RO; the loop's store will fault
        for (volatile int i = 0; i < 400; i++) { }
    }
    mprotect((void *)g_tracked, (size_t)g_ps, PROT_READ | PROT_WRITE);
    return NULL;
}

int p03_main(const char *out, int in_app, int argc, char **argv) {
    const char *variant = argc > 0 ? argv[0] : "alt";
    double secs = argc > 1 ? atof(argv[1]) : 2.0;
    g_use_altstack = strcmp(variant, "noalt") != 0;

    char fname[64]; snprintf(fname, sizeof fname, "p03_scratch_%s.txt", variant);
    FILE *f = res_open(out, fname);
    if (!f) return 2;
    res_context(f, in_app);
    fprintf(f, "variant=%s altstack=%d duration=%.1fs\n", variant, g_use_altstack, secs);

    g_ps = sysconf(_SC_PAGESIZE);
    g_tracked = mmap(NULL, (size_t)g_ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_tracked == MAP_FAILED) { fprintf(f, "tracked mmap failed\n"); fclose(f); return 3; }
    *g_tracked = 0;
    g_stop = 0; g_corrupt = 0; g_iter = 0;
    g_usr2_delivered = 0; g_segv_faults = 0; g_segv_other = 0;

    // handlers exactly as upstream installs them (crash.c / hle_thread.c): SA_SIGINFO|SA_ONSTACK[|SA_RESTART]
    struct sigaction su; memset(&su, 0, sizeof su);
    su.sa_sigaction = usr2_handler; su.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART; sigemptyset(&su.sa_mask);
    sigaction(SIGUSR2, &su, NULL);
    struct sigaction sv; memset(&sv, 0, sizeof sv);
    sv.sa_sigaction = segv_handler; sv.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sv.sa_mask);
    sigaction(SIGSEGV, &sv, NULL);

    // report whether the installed SIGSEGV handler kept SA_ONSTACK (libsigchain may rewrite it)
    struct sigaction chk; sigaction(SIGSEGV, NULL, &chk);
    fprintf(f, "SIGSEGV handler after install: on_stack_flag=%d siginfo_flag=%d (libsigchain visible if these differ in-app)\n",
            (chk.sa_flags & SA_ONSTACK) ? 1 : 0, (chk.sa_flags & SA_SIGINFO) ? 1 : 0);
    sigaction(SIGUSR2, NULL, &chk);
    fprintf(f, "SIGUSR2 handler after install: on_stack_flag=%d restart_flag=%d\n",
            (chk.sa_flags & SA_ONSTACK) ? 1 : 0, (chk.sa_flags & SA_RESTART) ? 1 : 0);

    // 64 KB stack for the loop thread (like a game's thread)
    size_t stksz = 64 * 1024;
    void *stk = mmap(NULL, stksz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stk == MAP_FAILED) { fprintf(f, "stack mmap failed\n"); fclose(f); return 4; }
    pthread_attr_t a; pthread_attr_init(&a);
    pthread_attr_setstack(&a, stk, stksz);
    pthread_t lt, st, pt;
    if (pthread_create(&lt, &a, loop_thread, NULL)) { fprintf(f, "loop thread create failed\n"); fclose(f); return 5; }
    pthread_attr_destroy(&a);
    pthread_create(&st, NULL, signal_thread, NULL);
    pthread_create(&pt, NULL, protect_thread, NULL);

    struct timespec ts = { (time_t)secs, (long)((secs - (time_t)secs) * 1e9) };
    nanosleep(&ts, NULL);
    __atomic_store_n(&g_stop, 1, __ATOMIC_SEQ_CST);
    pthread_join(lt, NULL); pthread_join(st, NULL); pthread_join(pt, NULL);

    uint64_t usr2 = g_usr2_delivered, segv = g_segv_faults, other = g_segv_other;
    fprintf(f, "iterations=%llu corruptions=%llu SIGUSR2_delivered=%llu SIGSEGV_faults=%llu other_faults=%llu\n",
            (unsigned long long)g_iter, (unsigned long long)g_corrupt,
            (unsigned long long)usr2, (unsigned long long)segv, (unsigned long long)other);

    int pass;
    if (g_use_altstack) {
        pass = (g_corrupt == 0 && usr2 > 0);
        fprintf(f, "RESULT(alt): [sp-112,sp) intact under %llu signals + %llu write-faults: %s\n",
                (unsigned long long)usr2, (unsigned long long)segv, pass ? "PASS" : "FAIL");
    } else {
        // the noalt run is a positive control: it should see corruption, proving detection works
        pass = (g_corrupt > 0);
        fprintf(f, "RESULT(noalt control): corruption detected (expected): %s\n", pass ? "YES (probe can detect)" : "NO (probe blind!)");
    }
    munmap(stk, stksz);
    munmap((void *)g_tracked, (size_t)g_ps);
    fclose(f);
    return pass ? 0 : 1;
}
