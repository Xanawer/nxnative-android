// Copy of ../common/probe_glue.c for the GPU probe, with one change: the activity keeps the screen on and shows over the lock screen
// (ANativeActivity_setWindowFlags), so the probe runs as the top app (top-app cpuset, all cores, not frozen) for its whole duration.
// Two entry points around one probe body, so the adb executable and the APK run identical code (docs/android/PLAN.md, Phase 0).
// The probe defines: int probe_main(const char *out_dir, int in_app, int argc, char **argv) and writes its results under out_dir.
//   Executable (built with -DPROBE_EXE): out_dir = argv[1] (default: the current directory); the remaining arguments go to the probe.
//   NativeActivity (libprobe.so, no Java code): out_dir = the app's files directory; arguments come from the intent extra "args"
//   is not available without Java, so they come from <files>/args.txt (one per line, pushed with run-as) when it exists.
// Both write <out_dir>/done with the probe's return code last, which run_probe.sh waits for.
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int probe_main(const char *out_dir, int in_app, int argc, char **argv);

static void write_done(const char *out_dir, int rc) {
    char p[512]; snprintf(p, sizeof p, "%s/done", out_dir);
    FILE *f = fopen(p, "w"); if (f) { fprintf(f, "%d\n", rc); fclose(f); }
}

#ifdef PROBE_EXE
int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : ".";
    char p[512]; snprintf(p, sizeof p, "%s/done", out); unlink(p);
    int rc = probe_main(out, 0, argc > 1 ? argc - 2 : 0, argc > 1 ? argv + 2 : argv + argc);
    write_done(out, rc);
    return rc;
}
#else
#include <android/log.h>
#include <android/native_activity.h>
#include <android/window.h>

static char g_out[512];
static ANativeActivity *g_act;

static void *probe_thread(void *arg) {
    (void)arg;
    char *av[64]; int ac = 0; static char buf[8192];
    char p[600]; snprintf(p, sizeof p, "%s/args.txt", g_out);
    FILE *f = fopen(p, "r");
    if (f) {
        size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
        for (char *s = strtok(buf, "\n"); s && ac < 63; s = strtok(NULL, "\n")) if (*s) av[ac++] = s;
    }
    av[ac] = NULL;
    snprintf(p, sizeof p, "%s/done", g_out); unlink(p);
    __android_log_print(ANDROID_LOG_INFO, "nxprobe", "probe start, out=%s, %d args", g_out, ac);
    int rc = probe_main(g_out, 1, ac, av);
    write_done(g_out, rc);
    __android_log_print(ANDROID_LOG_INFO, "nxprobe", "probe done rc=%d", rc);
    ANativeActivity_finish(g_act);
    return NULL;
}

// Runs once per launch: the probe runs on its own thread (the main thread must keep serving the activity), with a large stack like the host's main guest thread
void ANativeActivity_onCreate(ANativeActivity *act, void *saved, size_t saved_size) {
    (void)saved; (void)saved_size;
    g_act = act;
    ANativeActivity_setWindowFlags(act, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_TURN_SCREEN_ON | AWINDOW_FLAG_SHOW_WHEN_LOCKED, 0);
    snprintf(g_out, sizeof g_out, "%s", act->internalDataPath ? act->internalDataPath : "/data/local/tmp");
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
    pthread_t t; if (pthread_create(&t, &a, probe_thread, NULL)) __android_log_print(ANDROID_LOG_ERROR, "nxprobe", "pthread_create failed: %d", errno);
    else pthread_detach(t);
    pthread_attr_destroy(&a);
}
#endif
