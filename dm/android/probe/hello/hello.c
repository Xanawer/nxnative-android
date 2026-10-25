// Smoke test for the probe tooling: records the context the probe runs in (uid, SELinux domain, page size, the files it can see).
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int probe_main(const char *out_dir, int in_app, int argc, char **argv) {
    char p[512]; snprintf(p, sizeof p, "%s/hello.txt", out_dir);
    FILE *f = fopen(p, "w"); if (!f) return 2;
    char ctx[256] = "?"; FILE *c = fopen("/proc/self/attr/current", "r"); if (c) { if (fgets(ctx, sizeof ctx, c)) ctx[strcspn(ctx, "\n")] = 0; fclose(c); }
    fprintf(f, "in_app=%d uid=%d pid=%d page=%ld selinux=%s argc=%d\n", in_app, (int)getuid(), (int)getpid(), sysconf(_SC_PAGESIZE), ctx, argc);
    for (int i = 0; i < argc; i++) fprintf(f, "arg[%d]=%s\n", i, argv[i]);
    fclose(f);
    return 0;
}
