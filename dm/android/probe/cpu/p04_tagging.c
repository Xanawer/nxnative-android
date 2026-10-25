// P0.4 pointer tagging. Records the top byte (bits 63:56) of malloc pointers (small and large)
// and of mmap pointers. The same code runs:
//   - as the executable: by default, then again after mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, NONE);
//   - in the APK built --tagging off (package org.nxnative.probe.cpu);
//   - in the APK built --tagging on  (package org.nxnative.probe.cputag).
// The port asserts every host pointer handed to the guest has a zero top byte, so this measures
// whether that holds and whether mallopt can turn tagging off at runtime.
#include <malloc.h>
#include <sys/mman.h>
#include "cpuprobe.h"

static unsigned topbyte(void *p) { return (unsigned)(((uintptr_t)p) >> 56) & 0xff; }

static void sample(FILE *f, const char *label) {
    unsigned small_or = 0, large_or = 0, mmap_or = 0;
    void *keep[8];
    fprintf(f, "  [%s]\n", label);
    for (int i = 0; i < 8; i++) {
        void *p = malloc(64);
        keep[i] = p;
        small_or |= topbyte(p);
        if (i < 3) fprintf(f, "    malloc(64)   = %p  top=0x%02x\n", p, topbyte(p));
    }
    for (int i = 0; i < 8; i++) free(keep[i]);
    for (int i = 0; i < 4; i++) {
        void *p = malloc(1u << 20);
        large_or |= topbyte(p);
        if (i < 3) fprintf(f, "    malloc(1MiB) = %p  top=0x%02x\n", p, topbyte(p));
        free(p);
    }
    for (int i = 0; i < 4; i++) {
        void *p = mmap(NULL, 1u << 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) continue;
        mmap_or |= topbyte(p);
        if (i < 2) fprintf(f, "    mmap(64KiB)  = %p  top=0x%02x\n", p, topbyte(p));
        munmap(p, 1u << 16);
    }
    fprintf(f, "    OR of top bytes: small=0x%02x large=0x%02x mmap=0x%02x -> heap tagged=%s mmap tagged=%s\n",
            small_or, large_or, mmap_or,
            (small_or | large_or) ? "YES" : "no", mmap_or ? "YES" : "no");
}

int p04_main(const char *out, int in_app, int argc, char **argv) {
    (void)argc; (void)argv;
    FILE *f = res_open(out, "p04_tagging.txt");
    if (!f) return 2;
    res_context(f, in_app);
    fprintf(f, "manifest allowNativeHeapPointerTagging is set per-package: .cpu=off .cputag=on; exe has no manifest\n");

    sample(f, "default");

    int rc = mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, M_HEAP_TAGGING_LEVEL_NONE);
    fprintf(f, "  mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, NONE) returned %d\n", rc);
    sample(f, "after mallopt NONE");

    fprintf(f, "NOTE: tagging cannot be re-enabled after NONE; the port will set NONE at startup (exe) or rely on the manifest (app).\n");
    fclose(f);
    return 0;
}
