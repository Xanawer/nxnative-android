// P0.6 system registers. Reads cntvct_el0, cntfrq_el0, cntpct_el0, ctr_el0, dczid_el0, midr_el1,
// each guarded against SIGILL. Compares ctr_el0 against the rewriter's constant (movz #4,lsl#16 =
// 0x40000, DminLine=4). Checks cntvct monotonicity across cores and its rate vs CLOCK_MONOTONIC.
#define _GNU_SOURCE
#include <sched.h>
#include <time.h>
#include "cpuprobe.h"

#define RDSYS(name, var) __asm__ volatile("mrs %0, " name : "=r"(var))

static int rd_guarded(const char *name, uint64_t *out,
                      uint64_t (*rdfn)(void)) {
    int sig;
    if (GUARD_TRY()) { *out = rdfn(); GUARD_OK(); return 0; }
    sig = g_guard_sig; (void)name; return sig ? sig : -1;
}

static uint64_t rd_cntvct(void){ uint64_t v; RDSYS("cntvct_el0", v); return v; }
static uint64_t rd_cntpct(void){ uint64_t v; RDSYS("cntpct_el0", v); return v; }
static uint64_t rd_cntfrq(void){ uint64_t v; RDSYS("cntfrq_el0", v); return v; }
static uint64_t rd_ctr(void)   { uint64_t v; RDSYS("ctr_el0",    v); return v; }
static uint64_t rd_dczid(void) { uint64_t v; RDSYS("dczid_el0",  v); return v; }
static uint64_t rd_midr(void)  { uint64_t v; RDSYS("midr_el1",   v); return v; }

static void one(FILE *f, const char *name, uint64_t (*fn)(void), uint64_t *out, int *ok) {
    uint64_t v = 0; int r = rd_guarded(name, &v, fn);
    if (r == 0) { fprintf(f, "  %-12s = %#018llx\n", name, (unsigned long long)v); *out = v; *ok = 1; }
    else { fprintf(f, "  %-12s : trapped (%s)\n", name, signame(r)); *out = 0; *ok = 0; }
}

static double mono_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

int p06_main(const char *out, int in_app, int argc, char **argv) {
    (void)argc; (void)argv;
    FILE *f = res_open(out, "p06_sysreg.txt");
    if (!f) return 2;
    res_context(f, in_app);
    GUARD_INSTALL();

    uint64_t vct = 0, pct = 0, frq = 0, ctr = 0, dcz = 0, midr = 0;
    int ok_vct = 0, ok_pct = 0, ok_frq = 0, ok_ctr = 0, ok_dcz = 0, ok_midr = 0;
    fprintf(f, "system register reads (guarded against SIGILL):\n");
    one(f, "cntvct_el0", rd_cntvct, &vct, &ok_vct);
    one(f, "cntpct_el0", rd_cntpct, &pct, &ok_pct);
    one(f, "cntfrq_el0", rd_cntfrq, &frq, &ok_frq);
    one(f, "ctr_el0",    rd_ctr,    &ctr, &ok_ctr);
    one(f, "dczid_el0",  rd_dczid,  &dcz, &ok_dcz);
    one(f, "midr_el1",   rd_midr,   &midr, &ok_midr);

    if (ok_frq) {
        double mhz = frq / 1e6;
        fprintf(f, "cntfrq = %.4f MHz (Switch rate is 19.2 MHz: %s)\n", mhz, (frq == 19200000) ? "MATCH" : "differs");
    }
    if (ok_ctr) {
        unsigned dminline = (unsigned)((ctr >> 16) & 0xf);
        unsigned iminline = (unsigned)(ctr & 0xf);
        fprintf(f, "ctr_el0: DminLine=%u (rewriter forces 4 via movz #4,lsl#16=0x40000) IminLine=%u -> %s\n",
                dminline, iminline, (dminline == 4) ? "MATCH rewriter constant" : "DIFFERS from rewriter constant");
        fprintf(f, "  (rewriter replaces mrs ctr_el0 with 0x40000 => DminLine 4 = 64-byte line; this core's DminLine line = %u bytes)\n",
                (1u << dminline) * 4u);
    }
    if (ok_dcz) {
        fprintf(f, "dczid_el0: DZP=%llu BlockSize_log2words=%llu (DC ZVA block = %u bytes)\n",
                (unsigned long long)((dcz >> 4) & 1), (unsigned long long)(dcz & 0xf), (1u << (dcz & 0xf)) * 4u);
    }

    // cntvct monotonicity across cores
    if (ok_vct) {
        int ncpu = (int)sysconf(_SC_NPROCESSORS_CONF);
        if (ncpu > 16) ncpu = 16;
        fprintf(f, "cntvct across %d cores (pinned):\n", ncpu);
        uint64_t prev = 0; int mono = 1, pinned_any = 0;
        for (int c = 0; c < ncpu; c++) {
            cpu_set_t set; CPU_ZERO(&set); CPU_SET(c, &set);
            if (sched_setaffinity(0, sizeof set, &set) != 0) { fprintf(f, "  cpu %d: cannot pin (%s)\n", c, strerror(errno)); continue; }
            struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL);
            uint64_t v = rd_cntvct(); pinned_any = 1;
            fprintf(f, "  cpu %d: cntvct=%llu %s\n", c, (unsigned long long)v, (v >= prev) ? "" : "<-- NON-MONOTONIC");
            if (v < prev) mono = 0;
            prev = v;
        }
        cpu_set_t all; CPU_ZERO(&all); for (int c = 0; c < 64; c++) CPU_SET(c, &all); sched_setaffinity(0, sizeof all, &all);
        fprintf(f, "cntvct monotonic across cores: %s\n", (pinned_any && mono) ? "YES" : (pinned_any ? "NO" : "could not pin"));

        // rate vs CLOCK_MONOTONIC
        double t0 = mono_ns(); uint64_t c0 = rd_cntvct();
        struct timespec sl = {0, 100000000}; nanosleep(&sl, NULL);  // 100 ms
        double t1 = mono_ns(); uint64_t c1 = rd_cntvct();
        double measured_hz = (double)(c1 - c0) / ((t1 - t0) / 1e9);
        fprintf(f, "cntvct measured rate = %.4f MHz over %.1f ms (ticks=%llu); cntfrq says %.4f MHz\n",
                measured_hz / 1e6, (t1 - t0) / 1e6, (unsigned long long)(c1 - c0), ok_frq ? frq / 1e6 : 0.0);
        if (ok_frq) {
            double err = (measured_hz - (double)frq) / (double)frq * 100.0;
            fprintf(f, "cntvct rate vs cntfrq error = %.3f%% (%s)\n", err, (err < 1.0 && err > -1.0) ? "consistent" : "CHECK");
        }
    }

    fprintf(f, "cntvct==cntpct (EL0 sees the same counter): %s\n",
            (ok_vct && ok_pct) ? (llabs((long long)(vct - pct)) < 100000000 ? "yes (close)" : "far apart") : "n/a");
    fclose(f);
    return 0;
}
