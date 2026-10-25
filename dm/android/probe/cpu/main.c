// CPU Phase 0 probes (docs/android/PLAN.md P0.1-P0.6): one binary / one APK, the probe chosen by the first argument.
//   p01 | p02 | p03 [alt|noalt] [seconds] | p04 | p05 | p06 | p02child (run by p02 inside the APK through exec)
// Every probe writes <out_dir>/<probe>[_variant].txt. In the APK the files directory keeps results of earlier runs,
// and run_probe.sh copies every top-level file, so old result files are removed first.
#include <dirent.h>
#include "cpuprobe.h"

int p01_main(const char *out, int in_app, int argc, char **argv);
int p02_main(const char *out, int in_app, int argc, char **argv);
int p02_child_main(const char *out, int in_app, int argc, char **argv);
int p03_main(const char *out, int in_app, int argc, char **argv);
int p04_main(const char *out, int in_app, int argc, char **argv);
int p05_main(const char *out, int in_app, int argc, char **argv);
int p06_main(const char *out, int in_app, int argc, char **argv);

static void clean_results(const char *out) {
    DIR *d = opendir(out); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_type != DT_REG) continue;
        if (!strcmp(e->d_name, "args.txt")) continue;
        char p[700]; snprintf(p, sizeof p, "%s/%s", out, e->d_name);
        unlink(p);
    }
    closedir(d);
}

int probe_main(const char *out, int in_app, int argc, char **argv) {
    const char *which = argc > 0 ? argv[0] : "p01";
    if (strcmp(which, "p02child")) clean_results(out);   // the exec'd child writes into its own subdirectory and must not wipe the parent's files
    int ac = argc > 0 ? argc - 1 : 0; char **av = argc > 0 ? argv + 1 : argv;
    if (!strcmp(which, "p01")) return p01_main(out, in_app, ac, av);
    if (!strcmp(which, "p02")) return p02_main(out, in_app, ac, av);
    if (!strcmp(which, "p02child")) return p02_child_main(out, in_app, ac, av);
    if (!strcmp(which, "p03")) return p03_main(out, in_app, ac, av);
    if (!strcmp(which, "p04")) return p04_main(out, in_app, ac, av);
    if (!strcmp(which, "p05")) return p05_main(out, in_app, ac, av);
    if (!strcmp(which, "p06")) return p06_main(out, in_app, ac, av);
    FILE *f = res_open(out, "error.txt"); if (f) { fprintf(f, "unknown probe %s\n", which); fclose(f); }
    return 64;
}
