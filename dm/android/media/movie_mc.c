// What the movie decoders on MediaCodec share (movie_mc.h): the binder thread pool an adb executable lacks, and the codec calls that can wait on the codec's process run with a bounded wait.
// The latter: a job (one call: open = create + configure + start, flush, or close = stop + delete) runs on its own detached thread; the decoder's thread waits for it until a deadline. Both hold
// a reference to the job and the last one out frees it. When the deadline passes first, the job is marked abandoned and the helper, once its call returns, stops and deletes the codec itself:
// the movie's thread goes on at once (black pictures / silence, movie_avc_mc.c / movie_aac_mc.c), and a codec stuck in its process costs a thread until it answers, not the game.
// Seen on this phone: an adb executable without binder threads got no output and its stop() took 3 s each; the sixth create then never returned
#include "../../host/dm.h"
#include "movie_mc.h"
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/prctl.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

// ABinderProcess_startThreadPool is in the device's libbinder_ndk but not in the NDK's stub library, hence dlsym. In an app (zygote started the pool) isThreadPoolStarted says so;
// where it is missing the start is idempotent anyway. It names the calling thread a binder thread, so the name is put back
static void binder_once(void) {
    void *l = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_LOCAL);
    bool (*started)(void) = l ? (bool (*)(void))dlsym(l, "ABinderProcess_isThreadPoolStarted") : NULL;
    void (*start)(void) = l ? (void (*)(void))dlsym(l, "ABinderProcess_startThreadPool") : NULL;
    if (started && started()) return;
    if (!start) { LOG("[RED] movie: no ABinderProcess_startThreadPool in libbinder_ndk.so: outside an app, MediaCodec's decoders will not answer"); return; }
    char nm[16] = ""; prctl(PR_GET_NAME, nm);
    start();
    if (nm[0]) prctl(PR_SET_NAME, nm);
    LOG("[movie] binder thread pool started for MediaCodec (not an app process)");
}
void mmc_binder_threads(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, binder_once); }

static atomic_int g_live[MMC_LIVE_N];
void mmc_live_add(int what, int d) { atomic_fetch_add_explicit(&g_live[what], d, memory_order_relaxed); }
void mmc_live_get(int out[MMC_LIVE_N]) { for (int i = 0; i < MMC_LIVE_N; i++) out[i] = atomic_load_explicit(&g_live[i], memory_order_relaxed); }

enum { OP_OPEN, OP_FLUSH, OP_CLOSE };
typedef struct {
    int op; AMediaCodec *mc; AMediaFormat *f; const char *mime; char name[64]; int st;
    pthread_mutex_t mu; pthread_cond_t cv; int done, gone; atomic_int refs;
} job;
static void job_put(job *j) { if (atomic_fetch_sub(&j->refs, 1) == 1) { pthread_mutex_destroy(&j->mu); pthread_cond_destroy(&j->cv); free(j); mmc_live_add(MMC_LIVE_JOB, -1); } }
static void codec_delete(AMediaCodec *mc) { AMediaCodec_delete(mc); mmc_live_add(MMC_LIVE_CODEC, -1); }
static void stop_delete(AMediaCodec *mc) { if (mc) { AMediaCodec_stop(mc); codec_delete(mc); } }
static void *job_body(void *x) {
    job *j = x;
    if (j->op == OP_OPEN) {
        j->mc = AMediaCodec_createDecoderByType(j->mime);
        if (!j->mc) j->st = AMEDIA_ERROR_UNSUPPORTED;
        else {
            mmc_live_add(MMC_LIVE_CODEC, 1);
            char *nm = NULL; if (AMediaCodec_getName(j->mc, &nm) == AMEDIA_OK && nm) { snprintf(j->name, sizeof j->name, "%s", nm); AMediaCodec_releaseName(j->mc, nm); }
            j->st = AMediaCodec_configure(j->mc, j->f, NULL, NULL, 0);
            if (j->st == AMEDIA_OK) j->st = AMediaCodec_start(j->mc);
            if (j->st != AMEDIA_OK) { codec_delete(j->mc); j->mc = NULL; }
        }
        AMediaFormat_delete(j->f); j->f = NULL;
    } else if (j->op == OP_FLUSH) j->st = AMediaCodec_flush(j->mc);
    else { stop_delete(j->mc); j->mc = NULL; j->st = AMEDIA_OK; }
    pthread_mutex_lock(&j->mu); j->done = 1; int gone = j->gone; pthread_cond_signal(&j->cv); pthread_mutex_unlock(&j->mu);
    if (gone && j->op != OP_CLOSE) { stop_delete(j->mc); j->mc = NULL; }   // abandoned: the codec is the helper's (a flush's owner fell back without it)
    job_put(j);
    return NULL;
}
static void *job_thread(void *x) { job_body(x); mmc_live_add(MMC_LIVE_THREAD, -1); return NULL; }
// run j on a helper thread and wait until wait_us has passed: 1 = it finished (the caller reads it, then job_put), 0 = abandoned (the helper finishes and frees it)
static int job_run(job *j, int64_t wait_us) {
    pthread_mutex_init(&j->mu, NULL); pthread_cond_init(&j->cv, NULL); atomic_init(&j->refs, 2);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    mmc_live_add(MMC_LIVE_THREAD, 1);
    pthread_t t; int made = !pthread_create(&t, &at, job_thread, j);
    pthread_attr_destroy(&at);
    if (!made) { mmc_live_add(MMC_LIVE_THREAD, -1); job_body(j); return 1; }   // no thread: the call on this one
    struct timespec dl; clock_gettime(CLOCK_MONOTONIC, &dl);
    int64_t ns = dl.tv_nsec + wait_us * 1000; dl.tv_sec += ns / 1000000000; dl.tv_nsec = ns % 1000000000;
    pthread_mutex_lock(&j->mu);
    while (!j->done && pthread_cond_clockwait(&j->cv, &j->mu, CLOCK_MONOTONIC, &dl) != ETIMEDOUT) {}
    int done = j->done; if (!done) j->gone = 1;
    pthread_mutex_unlock(&j->mu);
    if (!done) job_put(j);
    return done;
}
static job *job_new(int op, AMediaCodec *mc) { job *j = calloc(1, sizeof *j); if (!j) die("movie: MediaCodec job allocation failed"); mmc_live_add(MMC_LIVE_JOB, 1); j->op = op; j->mc = mc; return j; }

AMediaCodec *mmc_open(const char *mime, AMediaFormat *f, int64_t wait_us, int *st, char *name, size_t nn) {
    job *j = job_new(OP_OPEN, NULL); j->f = f; j->mime = mime;
    if (!job_run(j, wait_us)) { *st = MMC_TIMEOUT; if (nn) name[0] = 0; return NULL; }
    AMediaCodec *mc = j->mc; *st = j->st; if (nn) snprintf(name, nn, "%s", j->name);
    job_put(j); return mc;
}
int mmc_flush(AMediaCodec *mc, int64_t wait_us) {
    job *j = job_new(OP_FLUSH, mc);
    if (!job_run(j, wait_us)) return MMC_TIMEOUT;
    int st = j->st; job_put(j); return st;
}
int mmc_close(AMediaCodec *mc, int64_t wait_us) {
    if (!mc) return 1;
    job *j = job_new(OP_CLOSE, mc);
    if (!job_run(j, wait_us)) return 0;
    job_put(j); return 1;
}
