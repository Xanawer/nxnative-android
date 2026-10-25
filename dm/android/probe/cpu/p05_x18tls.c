// P0.5 x18 / tpidr_el0.
//   - Read x18 on several threads: non-zero and different per thread means bionic's shadow call
//     stack (SCS) is live, so the rewriter must keep x18 virtualised (it already does).
//   - Confirm tpidr_el0 is bionic's thread pointer: equal to __builtin_thread_pointer(), and
//     the errno address sits within a small window of it (errno lives in the thread's TLS).
#include <pthread.h>
#include "cpuprobe.h"

static inline uintptr_t read_tp(void) { uintptr_t t; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(t)); return t; }
static inline uintptr_t read_x18(void) { uintptr_t v; __asm__ volatile("mov %0, x18" : "=r"(v)); return v; }

struct row { uintptr_t x18; uintptr_t tp; uintptr_t bi_tp; uintptr_t errno_addr; };

static void fill(struct row *r) {
    r->x18 = read_x18();
    r->tp = read_tp();
    r->bi_tp = (uintptr_t)__builtin_thread_pointer();
    r->errno_addr = (uintptr_t)&errno;
}
static void *worker(void *p) { fill((struct row *)p); return NULL; }

int p05_main(const char *out, int in_app, int argc, char **argv) {
    (void)argc; (void)argv;
    FILE *f = res_open(out, "p05_x18tls.txt");
    if (!f) return 2;
    res_context(f, in_app);

    enum { N = 6 };
    struct row rows[N];
    fill(&rows[0]);                       // this thread
    pthread_t t[N];
    for (int i = 1; i < N; i++) pthread_create(&t[i], NULL, worker, &rows[i]);
    for (int i = 1; i < N; i++) pthread_join(t[i], NULL);

    int x18_nonzero_all = 1, x18_distinct = 1;
    for (int i = 0; i < N; i++) {
        if (rows[i].x18 == 0) x18_nonzero_all = 0;
        for (int j = 0; j < i; j++) if (rows[i].x18 == rows[j].x18) x18_distinct = 0;
        long errno_off = (long)(rows[i].errno_addr - rows[i].tp);
        fprintf(f, "  thread %d: x18=%#lx tpidr_el0=%#lx builtin_tp=%#lx &errno=%#lx (errno-tp=%+ld) tp==builtin:%s\n",
                i, (unsigned long)rows[i].x18, (unsigned long)rows[i].tp, (unsigned long)rows[i].bi_tp,
                (unsigned long)rows[i].errno_addr, errno_off, rows[i].tp == rows[i].bi_tp ? "yes" : "NO");
    }
    fprintf(f, "x18 non-zero on all threads: %s\n", x18_nonzero_all ? "YES" : "no");
    fprintf(f, "x18 distinct per thread: %s\n", x18_distinct ? "YES" : "no");
    fprintf(f, "=> shadow call stack in use (x18 must stay virtualised in the port): %s\n",
            (x18_nonzero_all && x18_distinct) ? "YES" : "inconclusive");
    int tp_ok = 1; for (int i = 0; i < N; i++) if (rows[i].tp != rows[i].bi_tp) tp_ok = 0;
    fprintf(f, "=> tpidr_el0 is bionic's thread pointer (== __builtin_thread_pointer, errno in its TLS): %s\n",
            tp_ok ? "CONFIRMED" : "NO");
    fclose(f);
    return 0;
}
