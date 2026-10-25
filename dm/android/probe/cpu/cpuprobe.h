// Shared helpers for the CPU Phase 0 probes (docs/android/PLAN.md P0.1-P0.6).
// Each probe is its own translation unit defining probe_main(); this header is
// header-only (static inline / static data) so including it in several probes
// is fine. Nothing here allocates on a signal path except where noted.
#ifndef CPUPROBE_H
#define CPUPROBE_H
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---- result file ----
static inline FILE *res_open(const char *out_dir, const char *name) {
    char p[600];
    snprintf(p, sizeof p, "%s/%s", out_dir, name);
    FILE *f = fopen(p, "w");
    if (!f) { fprintf(stderr, "cannot open %s: %s\n", p, strerror(errno)); }
    return f;
}

// ---- SELinux / context line, so every probe records where it ran ----
static inline void res_context(FILE *f, int in_app) {
    char ctx[256] = "?";
    FILE *c = fopen("/proc/self/attr/current", "r");
    if (c) { if (fgets(ctx, sizeof ctx, c)) ctx[strcspn(ctx, "\n")] = 0; fclose(c); }
    fprintf(f, "context in_app=%d uid=%d pid=%d page=%ld selinux=%s\n",
            in_app, (int)getuid(), (int)getpid(), sysconf(_SC_PAGESIZE), ctx);
}

// ---- fault guard: run a risky block, catch SIGILL/SEGV/BUS/TRAP ----
// Usage:
//   GUARD_INSTALL();
//   if (GUARD_TRY()) { risky(); GUARD_OK(); } else { int s = g_guard_sig; ... }
// The jump target is thread-local, so several threads can each GUARD_TRY their own risky reads
// (each thread must GUARD_TRY before it can fault; a fault with no prior GUARD_TRY on that thread
// jumps through a zero buffer). The handlers are installed process-wide by guard_install().
static __thread volatile sig_atomic_t g_guard_sig;
static __thread sigjmp_buf g_guard_jb;
static void guard_handler(int sig, siginfo_t *si, void *uc) {
    (void)si; (void)uc;
    g_guard_sig = sig;
    siglongjmp(g_guard_jb, 1);
}
static inline void guard_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = guard_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, 0);
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS, &sa, 0);
    sigaction(SIGTRAP, &sa, 0);
}
#define GUARD_INSTALL() guard_install()
#define GUARD_TRY()     (g_guard_sig = 0, sigsetjmp(g_guard_jb, 1) == 0)
#define GUARD_OK()      ((void)0)

static inline const char *signame(int s) {
    switch (s) {
    case 0: return "none";
    case SIGILL: return "SIGILL";
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGTRAP: return "SIGTRAP";
    default: return "sig?";
    }
}

#endif
