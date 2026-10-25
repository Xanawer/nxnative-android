#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
// Crash state: prints the faulting address, pc, lr (converted to module-relative offsets) and the registers; dispatches page fault hooks.
// Only signal handling itself lives here: diagnostic features (probes / UI messages / sampling) are in diag.c, and this file does not depend on them (the graphics unit tests link only this file).
#include "dm.h"
#include <signal.h>
#include <dlfcn.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <sys/mman.h>
#ifdef __APPLE__
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#endif
#include <stdatomic.h>
#include <time.h>

static _Atomic uint64_t g_watch_last_ns;
static _Atomic uint64_t g_watch_count;
static _Atomic uint32_t g_watch_tick;
static _Atomic int g_watch_pregame = 1;
static _Atomic int g_watch_stage_idx;
static char g_watch_stage[2][256] = {"startup", "startup"};
static unsigned g_watch_stall;
static void (*g_dump_hook)(void);
static void (*g_wait_dump)(void);

static uint64_t watch_now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t crash_watch_progress_count(void) { return atomic_load(&g_watch_count); }
void crash_watch_progress(uint32_t tick, const char *stage) {
    int next = 1 - atomic_load(&g_watch_stage_idx);
    if (stage && *stage) snprintf(g_watch_stage[next], sizeof g_watch_stage[next], "%s", stage);
    atomic_store(&g_watch_tick, tick);
    atomic_store(&g_watch_stage_idx, next);
    atomic_fetch_add(&g_watch_count, 1);
    atomic_store(&g_watch_last_ns, watch_now_ns());
}
void crash_watch_frame(void) { if (atomic_load(&g_watch_pregame)) crash_watch_progress(0, "present frame"); }
void crash_watch_pregame_done(void) { atomic_store(&g_watch_pregame, 0); }
void crash_watch_pregame_again(void) { atomic_store(&g_watch_pregame, 1); }   // Back from a run to the front end: count progress by presented frames again (diag.c)
void crash_dump_now(const char *why);
static void *watchdog_main(void *arg) {
    (void)arg;
    for (;;) {
        usleep(250000);
        // Read the last progress time first, then read now: reading in the opposite order, if the game thread happens to record progress between the two reads, last is newer than now and now − last underflows as unsigned into "no progress since second 18446744073",
        // which is misjudged on the spot as a hang and exits (accept inv_drag_out, local load 11, the watchdog thread was switched out between the two reads). Reading this way gives last ≤ now; and if the clock ever wraps around it is not judged either
        uint64_t last = atomic_load(&g_watch_last_ns), now = watch_now_ns();
        if (last > now) continue;
        // Second-scale stall (progress has stopped for more than 1 second but has not yet reached the kill threshold): do not exit, dump what each thread is waiting on. When hitting a case of "only one frame every 2 seconds", the 15-second threshold cannot catch it and the log has nothing on what it is waiting for.
        // Dump only after entering a run; at most 3 dumps per process, at least 10 seconds between two dumps (the loading screen and the pause menu also stop ticks, do not let them flood the log)
        { static int dumps; static uint64_t last_dump, dumped_for;
          if (g_watch_stall && !atomic_load(&g_watch_pregame) && atomic_load(&g_watch_tick) > 0 && now - last >= 1000000000ull && now - last < (uint64_t)g_watch_stall * 1000000000ull
              && dumps < 3 && dumped_for != last && (!last_dump || now - last_dump >= 10000000000ull)) { dumps++; last_dump = now; dumped_for = last; crash_dump_now("progress stalled for over 1 second"); } }
        if (g_watch_stall && now - last >= (uint64_t)g_watch_stall * 1000000000ull) {
            uint64_t sec = (now - last) / 1000000000ull;
            int idx = atomic_load(&g_watch_stage_idx);
            LOG("[RED] watchdog: no progress since second %llu, stuck at tick %u / stage %s", (unsigned long long)sec,
                atomic_load(&g_watch_tick), g_watch_stage[idx]);
            if (g_dump_hook) g_dump_hook();
            if (g_wait_dump) g_wait_dump();   // State of all blocking wait points on the graphics side (registered when nvn_mtl.m initializes)
#if TARGET_OS_OSX
            { const char *d = getenv("DM_WATCH_SAMPLE");   // Mac diagnostics: when stuck, sample itself for 1 second and write the thread stacks to this path
              if (d) { char c[1200]; snprintf(c, sizeof c, "/usr/bin/sample %d 1 -f '%s' >/dev/null 2>&1", (int)getpid(), d); if (system(c)) LOG("watchdog: sampling failed"); else LOG("watchdog: thread stacks written to %s", d); } }
#endif
            _exit(128 + SIGALRM);
        }
    }
    return NULL;
}
void crash_off(const char *tag, uint64_t a) {   // Also used by diag.c
#define off crash_off

    module_t *m = module_of((void *)a);
    Dl_info di;
    if (m) fprintf(stderr, " %s%s%s+%#llx", tag, *tag ? "=" : "", m->name, a - (uint64_t)m->base);
    else if (dladdr((void *)a, &di) && di.dli_sname) fprintf(stderr, " %s%s%s+%#lx", tag, *tag ? "=" : "", di.dli_sname, (unsigned long)(a - (uint64_t)di.dli_saddr));
    else fprintf(stderr, " %s%s%#llx", tag, *tag ? "=" : "", a);
}
// Frame pointer backtrace: x29 chain [fp]=previous frame's fp, [fp+8]=return address; only 8-byte aligned, monotonically increasing frames are walked.
// Each frame is read with vm_read_overwrite, which fails instead of faulting: code built without frame pointers keeps data in x29 (STEINS;GATE RE:BOOT's main+0x38921c: x29 = 0x430),
// and a probe's backtrace there used to crash the host
void dm_backtrace(uint64_t fp) {
    fprintf(stderr, "[dm] bt:");
    for (int i = 0; i < 32 && fp && !(fp & 7); i++) {
        uint64_t w[2]; vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), (vm_address_t)fp, sizeof w, (vm_address_t)w, &got) != KERN_SUCCESS || got != sizeof w) break;
        uint64_t nfp = w[0], ra = w[1];
        off("", ra);
        if (nfp <= fp) break;
        fp = nfp;
    }
    fputc('\n', stderr);
}
// ---------- Page fault hooks (write-protect dirty page tracking) ----------
// Called in registration order before on_crash prints the crash state; returning 1 = handled (return directly and re-execute the faulting instruction); the crash path is taken only when all return 0.
// Hooks run in signal handler context: only table lookups / atomic operations / mprotect are allowed, no memory allocation and no locking.
#define MAX_FAULT_HOOK 4
static int (*g_fault_hook[MAX_FAULT_HOOK])(void *addr, int is_write, void *uc);
static void (*g_write_prep[MAX_FAULT_HOOK])(void *addr, size_t len);
static int g_nfault_hook, g_nwrite_prep;
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc)) {
    if (g_nfault_hook >= MAX_FAULT_HOOK) die("too many page fault hooks");
    g_fault_hook[g_nfault_hook++] = fn;
}
static void (*g_res_report)(uint32_t tick, const char *why);   // Resource count callback (registered by the graphics layer, called by diag.c per tick / event)
void dm_res_report_register(void (*fn)(uint32_t tick, const char *why)) { g_res_report = fn; }
void (*dm_res_report_cb(void))(uint32_t, const char *) { return g_res_report; }
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len)) {
    if (g_nwrite_prep >= MAX_FAULT_HOOK) die("too many host pre-write hooks");
    g_write_prep[g_nwrite_prep++] = fn;
}
// Called before the host itself writes guest memory (read() straight into a guest buffer, etc.): for a system call a write-protected page does not trigger a page fault but returns EFAULT
void dm_guest_write_prepare(void *addr, size_t len) { for (int i = 0; i < g_nwrite_prep; i++) g_write_prep[i](addr, len); }

// The diagnostics module (the probes in diag.c) and the thread state dump (hle_thread.c) each register themselves here: a unit test that links only this file does not depend on them
static int (*g_trap_hook)(ucontext_t *uc);
void crash_set_trap_hook(int (*fn)(ucontext_t *uc)) { g_trap_hook = fn; }
void crash_set_dump_hook(void (*fn)(void)) { g_dump_hook = fn; }
void crash_set_wait_dump(void (*fn)(void)) { g_wait_dump = fn; }
// Dump the current state on demand (without exiting): the iOS app has no no-progress watchdog, so when the smoke test fails it sends dump through DM_LOG_PORT to trigger this -- what each guest thread is waiting on + the graphics-side wait points, the same thing the watchdog prints
void crash_dump_now(const char *why) { fprintf(stderr, "[dm] state dump (%s): tick %u / stage %s\n", why, atomic_load(&g_watch_tick), g_watch_stage[atomic_load(&g_watch_stage_idx)]); if (g_dump_hook) g_dump_hook(); if (g_wait_dump) g_wait_dump(); }
// Only the first crashing thread prints its state: when several threads crash at once (Unity's job threads often do), the dumps used to interleave and the process hung in the dump without exiting.
// Later threads stop in pause() until the process exits; the first thread re-entering (crashing again while printing, or the SIGALRM of the 15 s safety below) exits at once.
static _Atomic uintptr_t g_crash_owner;
static void crash_gate(int sig) {
    uintptr_t me = (uintptr_t)pthread_self(), none = 0;
    if (atomic_compare_exchange_strong(&g_crash_owner, &none, me)) { alarm(15); return; }
    if (none == me) _exit(128 + sig);
    for (;;) pause();
}
static void on_crash(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    if (sig == SIGTRAP && g_trap_hook && g_trap_hook(uc)) return;
    if (sig == SIGSEGV || sig == SIGBUS) {
        int is_write = (int)((DM_UC_ESR(uc) >> 6) & 1);   // The WnR bit of ESR
        for (int i = 0; i < g_nfault_hook; i++) if (g_fault_hook[i](si->si_addr, is_write, uc)) return;
    }
    crash_gate(sig);
    // The stderr lock may be held by another thread (stuck or suspended): the fprintf below would hang with it, the time limit's SIGALRM could not end the run, and the log would get nothing.
    // If it still cannot be taken after 1 s, write a single line with write() (pc / lr as module offsets) and exit. When this thread itself holds the lock, ftrylockfile still succeeds (file locks are recursive), unaffected
    for (int i = 0; ftrylockfile(stderr); i++) {
        if (i < 100) { usleep(10000); continue; }
        char b[256]; int n = snprintf(b, sizeof b, "[dm] CRASH sig=%d (stderr held for 1 s, no dump)", sig);
        uint64_t at[2] = {DM_UC_PC(uc), DM_UC_LR(uc)};
        for (int k = 0; k < 2; k++) { module_t *m = module_of((void *)at[k]);
            n += m ? snprintf(b + n, sizeof b - n, " %s=%s+%#llx", k ? "lr" : "pc", m->name, at[k] - (uint64_t)m->base) : snprintf(b + n, sizeof b - n, " %s=%#llx", k ? "lr" : "pc", at[k]); }
        n += snprintf(b + n, sizeof b - n, "\n");
        if (write(2, b, (size_t)n) < 0) {}
        _exit(128 + sig);
    }
    funlockfile(stderr);
    fprintf(stderr, "[dm] CRASH sig=%d", sig);
    off("addr", (uint64_t)si->si_addr); off("pc", DM_UC_PC(uc)); off("lr", DM_UC_LR(uc));
    fprintf(stderr, " sp=%#llx\n", DM_UC_SP(uc));
    for (int i = 0; i < 29; i++) fprintf(stderr, "x%d=%#llx%s", i, DM_UC_X(uc, i), i % 6 == 5 ? "\n" : " ");
    fprintf(stderr, "\n");
    // What the registers point at: the first six words of each readable target (read with vm_read_overwrite, which fails instead of faulting); a word inside a module prints as a module offset, so an object's vtable names its class
    for (int i = 0; i < 29; i++) {
        uint64_t a = DM_UC_X(uc, i), w[6]; vm_size_t got = 0;
        if (a < 0x100000000ull || (a & 7) || vm_read_overwrite(mach_task_self(), (vm_address_t)a, sizeof w, (vm_address_t)w, &got) != KERN_SUCCESS || got != sizeof w) continue;
        fprintf(stderr, "[dm] *x%d:", i);
        for (int k = 0; k < 6; k++) off("", w[k]);
        fputc('\n', stderr);
    }
    dm_backtrace(DM_UC_FP(uc));
    // Stack scan: when the frame chain is broken (a return address was overwritten, pc = lr lands on garbage) the backtrace prints nothing; scan 512 words up from sp and list everything that resolves to a module / host symbol (nearest callers first)
    {   uint64_t sp = DM_UC_SP(uc);   // The frame just popped (when a return address was overwritten, the bad value and the leftovers of whatever overwrote it sit below sp)
        fprintf(stderr, "[dm] fp=%#llx  0x100 bytes below sp:", (unsigned long long)DM_UC_FP(uc));
        for (int i = -32; i < 0 && sp && !(sp & 7); i++) fprintf(stderr, "%s%llx", (i + 32) % 4 ? " " : "\n[dm]   ", (unsigned long long)((uint64_t *)sp)[i]);
        fputc('\n', stderr); }
    {   uint64_t sp = DM_UC_SP(uc); int n = 0;
        fprintf(stderr, "[dm] stack scan sp=%#llx:", (unsigned long long)sp);
        for (int i = -32; i < 512 && n < 24 && sp && !(sp & 7); i++) {
            uint64_t v = ((uint64_t *)sp)[i]; Dl_info di;
            if (v < 0x100000000ull || (v & 3)) continue;
            int mod = -1; for (int k = 0; k < g_nmod; k++) { uint64_t b = (uint64_t)(uintptr_t)g_mod[k].base; if (v >= b && v < b + g_mod[k].size) mod = k; }
            if (mod >= 0) { fprintf(stderr, " [%+d]%s+%#llx", i * 8, g_mod[mod].name, (unsigned long long)(v - (uint64_t)(uintptr_t)g_mod[mod].base)); n++; }
            else if (dladdr((void *)v, &di) && di.dli_sname && di.dli_fname && strstr(di.dli_fname, "dmhost")) { fprintf(stderr, " [%+d]%s+%#llx", i * 8, di.dli_sname, (unsigned long long)(v - (uint64_t)di.dli_saddr)); n++; }
        }
        fputc('\n', stderr); }
    if (g_dump_hook) g_dump_hook();
    _exit(128 + sig);
}
// The signal alternate stack is per thread: every guest thread must call this when it starts running (only then can the crash state be printed on the alternate stack on a stack overflow)
// The alternate stack is released when the thread exits: the pthread key destructor is called at thread exit (disable the alternate stack first, then free). The game frequently starts short-lived threads, and without releasing each one leaks 64 KiB.
static pthread_key_t g_altstack_key;
static pthread_once_t g_altstack_once = PTHREAD_ONCE_INIT;
static void altstack_free(void *p) { stack_t off = {.ss_flags = SS_DISABLE}; sigaltstack(&off, 0); free(p); }
static void altstack_key_init(void) { if (pthread_key_create(&g_altstack_key, altstack_free)) die("altstack key"); }
#ifndef __APPLE__
// Linux: a handler installed without SA_ONSTACK runs on the interrupted thread's own stack, and the rewriter's stubs keep scratch below sp (no red zone; docs/android/probes-cpu.md P0.3), so one
// arriving while a guest thread runs guest code corrupts it silently. Bionic installs two so (36, the platform profilers' __SIGRTMIN+4, SA_SIGINFO | SA_RESTART; 39, fdtrack's __SIGRTMIN+7,
// through signal()) and ART or another library may add more at any time. So every thread that runs guest code scans, when it starts (crash_thread_init), what the kernel will run (raw rt_sigaction,
// past libsigchain, which answers sigaction with the handler it chains to; 64 reads), and blocks what the scan says (raw rt_sigprocmask: bionic's sigprocmask quietly leaves its reserved 33-41 alone):
//  - the first scan (crash_install) puts bionic's own handlers (in libc.so) back as they were plus SA_ONSTACK, so they run on the thread's alternate stack (bionic gives every thread one; guest
//    threads get crash_thread_init's). Later scans only read: a write could freeze a handler someone holds in place for a moment (android_run_on_all_threads' on 40)
//  - anything else's handler without SA_ONSTACK (ART's, another library's) is blocked, apart from the set below. Two of those are in bionic's "33-41 must stay unblocked" (reserved_signals.h), and
//    blocking them only moves them: 34 (libcore's AsynchronousCloseMonitor pthread_kills only a Java thread blocked in I/O, which a guest thread never is) and 38 (perfetto_hprof's heap dump,
//    process-directed: the kernel gives it to a thread that does not block it, an activity or host thread)
//  - never blocked: the synchronous faults and seccomp's SIGSYS (blocked, the kernel kills the process instead of running the handler); the host's own SIGUSR2 (stop the world), SIGALRM, SIGTERM and
//    SIGABRT, which a foreign handler may still hold when a scan runs; bionic's POSIX timer signal 32, which bionic itself keeps blocked (never touched either way); and bionic's cross-thread
//    handshakes, thread-directed signals whose sender waits for each thread to answer: 33 (libunwindstack's ThreadUnwinder: tgkill, then waits for the ucontext; ART dumps unattached threads in an
//    ANR trace through it), 40 (android_run_on_all_threads: tgkill to every thread, then sem_wait for each holding g_thread_creation_lock for writing, so one thread blocking it would stop every
//    pthread_create and pthread_exit in the process for good) and 41 (MTE's per-thread re-enable). A thread that inherited 33, 40 or 41 blocked has them unblocked
//  - a thread inherits its creator's mask: what an earlier scan blocked and the latest no longer does is unblocked, unless the first scan's thread blocked it already (ART's SIGQUIT, SIGUSR1, SIGPIPE)
// Left open (the full fix is a rewriter change, stubs that move sp before they save; docs/android/PLAN.md P0.3):
//  - a handler without SA_ONSTACK that appears (or loses SA_ONSTACK) after a guest thread started is not blocked on that thread, as only a thread changes its own mask: its signal, landing there
//    while guest code runs, corrupts up to 112 bytes below sp. Threads started between the handler's arrival and its removal block it; others do not
//  - the handshakes stay unblocked whatever their handler: 40's never has SA_ONSTACK (SA_SIGINFO only). On this phone's bionic (android16) its one caller is SetHeapTaggingLevel, which signals
//    only in a process that runs MTE (ASYNC / SYNC; TBI -> NONE skips it) and this device has none; 33's handler (libunwindstack) has SA_ONSTACK; 41's is installed only under MTE
struct dm_kact { uintptr_t handler; unsigned long flags; uintptr_t restorer; uint64_t mask; };   // the kernel's arm64 struct sigaction: 64-bit mask, sigsetsize 8
#define SIG_BIT(s) (1ull << ((s) - 1))
static const uint64_t g_sig_shake = SIG_BIT(33) | SIG_BIT(40) | SIG_BIT(41);   // bionic's cross-thread handshakes (above): never blocked, unblocked when inherited
static pthread_mutex_t g_sig_mu = PTHREAD_MUTEX_INITIALIZER;   // the scan state: threads that run guest code start concurrently
static uint64_t g_sig_block;   // bit sig - 1: what a thread that runs guest code blocks, by the latest scan
static uint64_t g_sig_ours;    // every bit a scan has put in g_sig_block
static uint64_t g_sig_base;    // the first scan's thread's mask before anything was blocked: the process's own reasons, never unblocked here
static struct { uintptr_t handler; unsigned long flags; } g_sig_seen[65];   // each signal's handler at the last scan: a rescan decides, looks up and logs only what changed
static int g_sig_scans;
static _Atomic int g_sig_shown;
static int sig_kept(int s) {
    switch (s) { case SIGSEGV: case SIGBUS: case SIGILL: case SIGFPE: case SIGTRAP: case SIGSYS: case SIGKILL: case SIGSTOP: case SIGUSR2: case SIGALRM: case SIGTERM: case SIGABRT: return 1; }
    return 0;
}
static void sig_scan(void) {   // holding g_sig_mu
    int first = !g_sig_scans++, n = 0, lack = 0, reinst = 0; char list[256] = ""; size_t ll = 0; uint64_t was = g_sig_block;
    if (first) syscall(SYS_rt_sigprocmask, SIG_BLOCK, NULL, &g_sig_base, sizeof g_sig_base);
    for (int s = 1; s <= 64; s++) {
        struct dm_kact k; if (syscall(SYS_rt_sigaction, s, NULL, &k, sizeof k.mask)) continue;
        if (k.handler > 1) { n++; lack += !(k.flags & SA_ONSTACK); }   // SIG_DFL 0, SIG_IGN 1
        if (k.handler == g_sig_seen[s].handler && k.flags == g_sig_seen[s].flags) continue;   // as at the last scan, and so is the decision
        g_sig_seen[s].handler = k.handler; g_sig_seen[s].flags = k.flags;
        static const char *const ab[32] = {"", "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE", "KILL", "USR1", "SEGV", "USR2", "PIPE", "ALRM", "TERM", "STKFLT",
                                           "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU", "URG", "XCPU", "XFSZ", "VTALRM", "PROF", "WINCH", "IO", "PWR", "SYS"};   // the generic Linux numbering arm64 uses
        char nm[24]; if (s < 32) snprintf(nm, sizeof nm, "SIG%s", ab[s]); else snprintf(nm, sizeof nm, "__SIGRTMIN+%d", s - 32);
        uint64_t bit = SIG_BIT(s); int had = (g_sig_block & bit) != 0; g_sig_block &= ~bit;
        if (k.handler <= 1 || (k.flags & SA_ONSTACK)) {   // nothing to guard against (any more)
            if (had) LOG("signals: %d (%s) now %s: no longer blocked on guest threads that start from here", s, nm, k.handler > 1 ? "on the alternate stack" : k.handler ? "ignored" : "default");
            continue;
        }
        Dl_info di = {0}; char how[128], pre[64] = ""; const char *red = "";
        int found = dladdr((void *)k.handler, &di) && di.dli_fname; const char *lib = found ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?";
        int bionic = found && !strcmp(lib, "libc.so"), never = sig_kept(s) || s == 32 || (g_sig_shake & bit);
        unsigned long fl = k.flags; struct dm_kact back = {0}; int onstack = 0;
        if (bionic && first && !never) {   // bionic's own: the same handler, mask and restorer with SA_ONSTACK, read back
            k.flags |= SA_ONSTACK;
            if (!syscall(SYS_rt_sigaction, s, &k, NULL, sizeof k.mask) && !syscall(SYS_rt_sigaction, s, NULL, &back, sizeof back.mask) && back.handler == k.handler && (back.flags & SA_ONSTACK)) {
                snprintf(how, sizeof how, "reinstalled with SA_ONSTACK (bionic's own; flags now %#lx)", back.flags); reinst++; onstack = 1; g_sig_seen[s].flags = back.flags;
            } else { red = "[RED] "; snprintf(pre, sizeof pre, "not reinstalled (errno %d), ", errno); }   // then treated as anyone else's
        } else if (bionic && !never) snprintf(pre, sizeof pre, "bionic's, not reinstalled (only the first scan writes), ");
        if (!onstack) {
            if (sig_kept(s)) snprintf(how, sizeof how, "%skept (a fault or the host's own signal: never blocked)", pre);
            else if (s == 32) snprintf(how, sizeof how, "%skept (bionic's POSIX timer signal: never touched)", pre);
            else if (never) snprintf(how, sizeof how, "%skept (a bionic cross-thread handshake: never blocked)", pre);
            else {
                snprintf(how, sizeof how, "%sblocked on guest threads%s", pre, first ? "" : " that start from here"); g_sig_block |= bit; g_sig_ours |= bit;
                int w = snprintf(list + ll, sizeof list - ll, " %d", s); if (w > 0 && (size_t)w < sizeof list - ll) ll += (size_t)w;
            }
        }
        LOG("%ssignals: %d (%s) handler %s%s%s+%#lx, flags %#lx, no SA_ONSTACK: %s", red, s, nm, lib, di.dli_sname ? " " : "", di.dli_sname ? di.dli_sname : "",
            (unsigned long)(k.handler - (uintptr_t)(di.dli_sname ? di.dli_saddr : di.dli_fbase)), fl, how);
    }
    if (first) LOG("signals: %d handlers installed, %d without SA_ONSTACK, %d of them bionic's put on the alternate stack; threads that run guest code block%s", n, lack, reinst, *list ? list : " none");
    else if (g_sig_block != was) LOG("signals: guest threads that start from here block %#llx (was %#llx)", (unsigned long long)g_sig_block, (unsigned long long)was);
}
static void sig_guard_thread(int show) {
    pthread_mutex_lock(&g_sig_mu); sig_scan(); uint64_t blk = g_sig_block, un = (g_sig_ours & ~blk & ~g_sig_base) | g_sig_shake; pthread_mutex_unlock(&g_sig_mu);
    if (syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, &un, NULL, sizeof un) || (blk && syscall(SYS_rt_sigprocmask, SIG_BLOCK, &blk, NULL, sizeof blk))) LOG("[RED] signals: rt_sigprocmask failed (errno %d)", errno);
    if (!show || atomic_fetch_add(&g_sig_shown, 1) >= 2) return;   // the mask read back on the first two such threads (the host thread that runs guest_main, the first CreateThread one)
    uint64_t now = 0; syscall(SYS_rt_sigprocmask, SIG_BLOCK, NULL, &now, sizeof now);
    int held = (now & blk) == blk, shake = (now & g_sig_shake) != 0;
    LOG("%ssignals: thread %d blocks %#llx (the guest set %#llx %s)", held && !shake ? "" : "[RED] ", (int)gettid(), (unsigned long long)now, (unsigned long long)blk,
        !held ? "missing" : shake ? "held, and a handshake signal too" : "held");
}
#endif
void crash_thread_init(void) {
    pthread_once(&g_altstack_once, altstack_key_init);
#ifndef __APPLE__
    sig_guard_thread(!pthread_getspecific(g_altstack_key));   // every call: a thread that runs guest code scans and sets its mask as it starts (main.c comes back here after an early load)
#endif
    if (pthread_getspecific(g_altstack_key)) return;                 // Repeated calls on the same thread do not allocate again
    stack_t ss = {.ss_sp = malloc(65536), .ss_size = 65536}; sigaltstack(&ss, 0);
    pthread_setspecific(g_altstack_key, ss.ss_sp);
}
void crash_install(void) {
    crash_thread_init();
    struct sigaction sa = {.sa_sigaction = on_crash, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigaction(SIGALRM, &sa, 0);
    // Both watchdogs are turned on only when a test explicitly sets DM_WATCH / DM_WATCH_STALL: the product (the player launches from the desktop, no environment variables) turns on neither.
    // Found by reading the code: iOS main.m did setenv DM_WATCH=30 by default → the player's launch was killed by SIGALRM at 30 s; the no-progress watchdog defaulted to 15 s → the pause menu (ticks stopped) was killed at 15 s
    const char *watch = getenv("DM_WATCH"), *stall = getenv("DM_WATCH_STALL");
    if (watch) alarm(atoi(watch));
    if (watch || stall) {
        g_watch_stall = stall && atoi(stall) > 0 ? (unsigned)atoi(stall) : 15;
        atomic_store(&g_watch_last_ns, watch_now_ns());
        pthread_t th; if (pthread_create(&th, NULL, watchdog_main, NULL)) die("watchdog thread");
        pthread_detach(th);
    }
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGILL, &sa, 0); sigaction(SIGTRAP, &sa, 0);
    // abort() too (an assertion or an uncaught exception in the host or a framework): Big Brain Academy: Brain vs. Brain died once with SIGABRT 11 minutes into a run with nothing
    // in the log and no crash report, so where it came from was lost
    sigaction(SIGABRT, &sa, 0);
}
