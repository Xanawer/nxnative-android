// The movie decoders on MediaCodec (dm/android/media) against synthetic clips and ffmpeg's decode of them (dm/android/tests/gen_movie_clips.py makes both), driven the way hle_movie.c drives them:
//   H.264: the decoder made and given its parameter sets (Configure's csd-0 / csd-1, or the avcC record) on a small-stack thread, as on a guest thread; each access unit decoded and one picture taken
//   on this thread (the decoder's host thread); mavc_drop on the small-stack thread; at the end of stream one take with flush, then one per mavc_queued (md_pump's drain), each of which must give a picture.
//   AAC: maac_settle as Configure calls it (the rate and channels the format event announces), then maac_new / maac_decode as md_aac calls them, into buffers of MD_PCM_CAP bytes: each packet
//   decoded, then the PCM that came late taken while maac_ready (md_pump's slots), at the end of stream taken with flush while maac_queued (md_pump's drain); maac_drop on the small-stack thread.
// Checks: picture count, presentation order and times, pixels (NV12 against ffmpeg's decode: PSNR per plane at least 50 dB and largest error at most 4, H.264 being exact; RGBA against the reference
// converted in double precision with the matrix the decoder chose: largest error at most 2), crop and padding, parameter sets in band after a flush, every picture kept when none is taken until the
// end; AAC: rate / channels / samples per packet as announced (960-sample frames too), each output exactly the size GetOutputBufferSize advertises, one output per packet with that packet's time,
// in order; the PCM placed by those times against ffmpeg's decode: the offset within
// 4 frames of the one calibrated beforehand (dm-work scripts/calib_lag.py), priming, the whole length within one packet, PSNR over all of it (at least 60 dB for AAC-LC, 30 dB with SBR, whose
// decoders legitimately differ; parametric stereo on the downmix), over the last 4,096 frames and per packet; Flush then on; latency, create / free cycles (the slopes of descriptors and native heap; the live counts of
// movie_mc.h back to the start, here and after the next two), decoders at once, and the fallbacks
// (DM_MOVIE_MC_FAIL: black pictures / silence at every time, one [RED], calls returning at once); a damaged access unit / packet: one picture / one output per unit at its time all the same,
// whether the decoder conceals it, passes over it or fails.
// Usage: t_movie_mc <out dir> [clips dir] (default: the out dir); the report goes to stdout and <out>/result.txt, the decoders' log to <out>/stderr.txt. T_MOVIE_MC_CYCLES=N: only N
// create / free cycles and the live counts (for a heap profile of them). Built two ways with the probe harness's
// main (probe_glue.c): an adb executable (CMake's tests target) and a NativeActivity APK (build_probe.sh), where the arguments come from files/args.txt
#include "../media/movie_mc.h"
#include "../../host/hle_movie_avc.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include <dirent.h>
#include <malloc.h>

#define PCM_CAP (8192 * 8 * 2)   // hle_movie.c MD_PCM_CAP: the bytes of one audio output slot

static FILE *g_rep; static int g_fail, g_pass; static char g_out[512];
static void say(const char *fmt, ...) {
    va_list v; va_start(v, fmt); vprintf(fmt, v); va_end(v); fputc('\n', stdout); fflush(stdout);
    if (g_rep) { va_start(v, fmt); vfprintf(g_rep, fmt, v); va_end(v); fputc('\n', g_rep); fflush(g_rep); }
}
static int check(int ok, const char *fmt, ...) {
    char b[1024]; va_list v; va_start(v, fmt); vsnprintf(b, sizeof b, fmt, v); va_end(v);
    say("  %s %s", ok ? "ok  " : "FAIL", b); if (ok) g_pass++; else g_fail++;
    return ok;
}

// ---------- clips ----------
typedef struct { const uint8_t *p; uint32_t n; } span;
typedef struct { int64_t pts; int key; span d; } unit;
typedef struct {
    char name[32], prof[16]; int kind, w, h, n, key2;
    span sps, pps, avcc, asc; int rate, ch, exp_rate, exp_ch, exp_frames;
    unit *u; uint8_t *file; int64_t *spts;
    uint8_t *ref; size_t nref; uint32_t *roff;   // video: each picture's zlib data; audio: s16le stereo at exp_rate
} clip;
static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l + 1); if (b && fread(b, 1, (size_t)l, f) != (size_t)l) { free(b); b = NULL; }
    fclose(f); if (n) *n = (size_t)l; return b;
}
static uint32_t rd32(const uint8_t **p) { uint32_t v; memcpy(&v, *p, 4); *p += 4; return v; }
static span rdspan(const uint8_t **p) { span s; s.n = rd32(p); s.p = *p; *p += s.n; return s; }
static int cmp64(const void *a, const void *b) { int64_t x = *(const int64_t *)a, y = *(const int64_t *)b; return x < y ? -1 : x > y; }
static int load(clip *c, const char *dir) {
    char p[700]; size_t n;
    snprintf(p, sizeof p, "%s/%s.nxc", dir, c->name);
    const uint8_t *b = c->file = slurp(p, &n); if (!b || n < 24 || memcmp(b, "NXMC", 4)) return 0;
    b += 4; rd32(&b); int kind = (int)rd32(&b); if (kind != c->kind) return 0;
    if (kind == 1) { c->w = (int)rd32(&b); c->h = (int)rd32(&b); c->n = (int)rd32(&b); c->sps = rdspan(&b); c->pps = rdspan(&b); c->avcc = rdspan(&b); }
    else { c->rate = (int)rd32(&b); c->ch = (int)rd32(&b); c->n = (int)rd32(&b); c->asc = rdspan(&b); }
    c->u = calloc((size_t)c->n, sizeof *c->u); c->spts = calloc((size_t)c->n, sizeof *c->spts);
    for (int i = 0; i < c->n; i++) { int64_t t; memcpy(&t, b, 8); b += 8; c->u[i].pts = c->spts[i] = t; if (kind == 1) c->u[i].key = (int)rd32(&b); c->u[i].d = rdspan(&b); }
    qsort(c->spts, (size_t)c->n, sizeof *c->spts, cmp64);
    if (kind == 1) {
        snprintf(p, sizeof p, "%s/%s.ref", dir, c->name); c->ref = slurp(p, &c->nref); if (!c->ref) return 0;
        c->roff = calloc((size_t)c->n + 1, sizeof *c->roff); size_t o = 0;
        for (int i = 0; i < c->n; i++) { if (o + 4 > c->nref) return 0; c->roff[i] = (uint32_t)o; uint32_t l; memcpy(&l, c->ref + o, 4); o += 4 + l; }
    } else { snprintf(p, sizeof p, "%s/%s.%d.pcm", dir, c->name, c->exp_rate); c->ref = slurp(p, &c->nref); if (!c->ref) return 0; }
    return 1;
}
static int ref_pic(const clip *c, int k, uint8_t *dst) {   // the k-th picture in presentation order, NV12 w × h
    uint32_t l; memcpy(&l, c->ref + c->roff[k], 4); uLongf dl = (uLongf)c->w * c->h * 3 / 2;
    return uncompress(dst, &dl, c->ref + c->roff[k] + 4, l) == Z_OK && dl == (uLongf)c->w * c->h * 3 / 2;
}
static int pts_rank(const clip *c, int64_t t) { int lo = 0, hi = c->n - 1; while (lo <= hi) { int m = (lo + hi) / 2; if (c->spts[m] == t) return m; if (c->spts[m] < t) lo = m + 1; else hi = m - 1; } return -1; }

// ---------- a guest thread: the calls hle_movie.c makes from guest threads run on a fresh thread with a small stack ----------
typedef struct { void (*fn)(void *); void *arg; } gcall;
static void *g_body(void *x) { gcall *g = x; g->fn(g->arg); return NULL; }
static void on_guest(void (*fn)(void *), void *arg) {
    gcall g = {fn, arg}; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64 << 10);
    pthread_t t; if (pthread_create(&t, &at, g_body, &g)) { fn(arg); } else pthread_join(t, NULL);
    pthread_attr_destroy(&at);
}
typedef struct { mavc *a; const clip *c; int rgba, avcc; } vsetup;
static void g_setup(void *x) {   // h_movie_Configure: md_avc_open, then csd-0 / csd-1 (Annex B) or the avcC record
    vsetup *s = x; s->a = mavc_new(); if (s->rgba) mavc_set_rgba(s->a);
    if (s->avcc) { mavc_avcc(s->a, s->c->avcc.p, s->c->avcc.n); return; }
    uint8_t b[600] = {0, 0, 0, 1};
    memcpy(b + 4, s->c->sps.p, s->c->sps.n); mavc_param_sets(s->a, b, s->c->sps.n + 4);
    memcpy(b + 4, s->c->pps.p, s->c->pps.n); mavc_param_sets(s->a, b, s->c->pps.n + 4);
}
static void g_drop(void *x) { mavc_drop(x); }

// ---------- latency ----------
static void stats(double *v, int n, double *p50, double *p95, double *mx) {
    if (!n) { *p50 = *p95 = *mx = 0; return; }
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && v[j - 1] > v[j]; j--) { double t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    *p50 = v[n / 2]; *p95 = v[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1]; *mx = v[n - 1];
}

// ---------- H.264 ----------
typedef struct {
    int rgba, avcc, lenpfx;   // RGBA output (mavc_set_rgba); parameter sets from the avcC record; access units with 4-byte lengths
    int drop_at, resume_at;   // mavc_drop before access unit drop_at, then on from resume_at (a key frame); drop_at 0: none
    int dw, dh;               // the output size, the clip's when 0 (hle_movie.c's d->w / d->h)
    int nmax;                 // only the first nmax access units (0: all)
    int expect_black;         // the fallback: every picture black
    int no_take;              // no picture taken until the end of stream (all of them queued at once)
    int lat_from;             // mx_from: the largest latency from this access unit on (after a forced failure)
    int dmg;                  // this access unit damaged (its second half's bytes flipped); pictures from presentation rank 6 up to the second key frame are not compared (errors propagate); 0: none
} vopt;
typedef struct {
    int pics, pre, order_bad, unknown, black, eos_short, extra_after, wrong_set, pad_bad, ref_bad;
    double sse[3]; double cnt[3]; int maxerr[3];
    double p50, p95, mx, take_mx, mx_from; mmc_info info;
} vres;
static double psnr(double sse, double n) { return sse <= 0 ? INFINITY : 10 * log10(255.0 * 255.0 / (sse / n)); }
static void ycc_rgb(int Y, int U, int V, int matrix, int full, double o[3]) {
    double kr = matrix == 709 ? 0.2126 : matrix == 2020 ? 0.2627 : 0.299, kb = matrix == 709 ? 0.0722 : matrix == 2020 ? 0.0593 : 0.114, kg = 1 - kr - kb;
    double y = full ? Y : (Y - 16) * 255.0 / 219.0, u = (U - 128) * (full ? 1.0 : 255.0 / 224.0), v = (V - 128) * (full ? 1.0 : 255.0 / 224.0);
    o[0] = y + 2 * (1 - kr) * v; o[2] = y + 2 * (1 - kb) * u; o[1] = y - (2 * (1 - kb) * kb / kg) * u - (2 * (1 - kr) * kr / kg) * v;
}
// one picture against the reference: the window both cover, then the padding beyond the picture
static void cmp_pic(const clip *c, const vopt *o, const uint8_t *dst, const uint8_t *ref, int w, int h, vres *r, const mmc_info *inf) {
    int cw = c->w < w ? c->w : w, chh = c->h < h ? c->h : h, black = 1;
    if (o->rgba) {
        for (int y = 0; y < chh; y++) for (int x = 0; x < cw; x++) {
            double e[3]; ycc_rgb(ref[(size_t)y * c->w + x], ref[(size_t)c->w * c->h + (size_t)(y / 2) * c->w + (x & ~1)], ref[(size_t)c->w * c->h + (size_t)(y / 2) * c->w + (x & ~1) + 1], inf->matrix ? inf->matrix : 601, inf->full, e);
            const uint8_t *q = dst + ((size_t)y * w + x) * 4;
            for (int k = 0; k < 3; k++) { double ev = e[k] < 0 ? 0 : e[k] > 255 ? 255 : e[k]; int d = abs(q[k] - (int)lrint(ev)); r->sse[k] += (double)d * d; r->cnt[k]++; if (d > r->maxerr[k]) r->maxerr[k] = d; }
            if (q[3] != 255) r->pad_bad++;
        }
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if ((x >= cw || y >= chh) && ((const uint32_t *)dst)[(size_t)y * w + x] != 0xff000000u) { r->pad_bad++; y = h; break; }
        return;
    }
    const uint8_t *duv = dst + (size_t)w * h, *ruv = ref + (size_t)c->w * c->h;
    for (int y = 0; y < chh; y++) for (int x = 0; x < cw; x++) { int d = abs(dst[(size_t)y * w + x] - ref[(size_t)y * c->w + x]); r->sse[0] += (double)d * d; r->cnt[0]++; if (d > r->maxerr[0]) r->maxerr[0] = d; if (dst[(size_t)y * w + x] != 16) black = 0; }
    for (int y = 0; y < chh / 2; y++) for (int x = 0; x < (cw & ~1); x++) {
        int d = abs(duv[(size_t)y * w + x] - ruv[(size_t)y * c->w + x]), k = 1 + (x & 1); r->sse[k] += (double)d * d; r->cnt[k]++; if (d > r->maxerr[k]) r->maxerr[k] = d; if (duv[(size_t)y * w + x] != 128) black = 0; }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if ((x >= cw || y >= chh) && dst[(size_t)y * w + x] != 16) { r->pad_bad++; y = h; break; }
    for (int y = 0; y < h / 2; y++) for (int x = 0; x < w; x++) if ((x >= (cw & ~1) || y >= chh / 2) && duv[(size_t)y * w + x] != 128) { r->pad_bad++; y = h; break; }
    r->black += black;
}
static int run_video(const clip *c, const vopt *o, vres *r) {
    memset(r, 0, sizeof *r);
    vsetup s = {NULL, c, o->rgba, o->avcc}; on_guest(g_setup, &s); mavc *a = s.a;
    int w = o->dw ? o->dw : c->w, h = o->dh ? o->dh : c->h, n = o->nmax ? o->nmax : c->n;
    uint8_t *dst = malloc((size_t)w * h * 4), *ref = malloc((size_t)c->w * c->h * 3 / 2), *au = malloc(4u << 20);
    int64_t *fed = calloc((size_t)c->n, sizeof *fed), *got = calloc((size_t)c->n * 2 + 8, sizeof *got); int nfed = 0, ngot = 0;
    double *lat = calloc((size_t)c->n * 2, sizeof *lat); int nlat = 0, dropped = 0; int64_t last = INT64_MIN;
    mmc_info inf;
    #define ON_PIC(t) do { \
        int k_ = pts_rank(c, t); if ((t) <= last) r->order_bad++; last = (t); got[ngot++] = (t); r->pics++; \
        if (k_ < 0) r->unknown++; else if (o->dmg && k_ >= 6 && k_ < c->key2) {} else if (!ref_pic(c, k_, ref)) r->ref_bad++; else { mavc_mc_info(a, &inf); if (o->expect_black) { vres z = {0}; cmp_pic(c, o, dst, ref, w, h, &z, &inf); r->black += z.black; r->pad_bad += z.pad_bad; } else cmp_pic(c, o, dst, ref, w, h, r, &inf); } \
    } while (0)
    for (int i = 0; i < n; i++) {
        if (o->drop_at && !dropped && i == o->drop_at) {   // a seek: Flush on a guest thread; what came out so far is checked, the rest starts afresh
            dropped = 1; r->pre = ngot; on_guest(g_drop, a); i = o->resume_at; nfed = ngot = 0; last = INT64_MIN;
        }
        const unit *u = &c->u[i]; const uint8_t *p = u->d.p; int len = (int)u->d.n;
        if (o->dmg && i == o->dmg) { memcpy(au, p, (size_t)len); for (int k = len / 2; k < len; k++) au[k] ^= 0xa5; p = au; }
        if (o->lenpfx) {   // Annex B → 4-byte lengths (an MP4 demuxer's units)
            len = 0; for (uint32_t k = 0; k < u->d.n;) { uint32_t e = k + 4; while (e + 4 <= u->d.n && memcmp(u->d.p + e, "\0\0\0\1", 4)) e++; if (e + 4 > u->d.n) e = u->d.n;
                uint32_t nl = e - (k + 4); au[len] = (uint8_t)(nl >> 24); au[len + 1] = (uint8_t)(nl >> 16); au[len + 2] = (uint8_t)(nl >> 8); au[len + 3] = (uint8_t)nl; memcpy(au + len + 4, u->d.p + k + 4, nl); len += 4 + (int)nl; k = e; }
            p = au;
        }
        int64_t t0 = mmc_now_us(), po = 0;
        mavc_decode(a, p, len, u->pts);
        int64_t t1 = mmc_now_us();
        int g = o->no_take ? 0 : mavc_take(a, 0, dst, w, h, &po);
        int64_t t2 = mmc_now_us();
        lat[nlat++] = (double)(t2 - t0) / 1000; if ((t2 - t1) / 1000.0 > r->take_mx) r->take_mx = (t2 - t1) / 1000.0;
        if (i >= o->lat_from && (t2 - t0) / 1000.0 > r->mx_from) r->mx_from = (t2 - t0) / 1000.0;
        fed[nfed++] = u->pts;
        if (g) ON_PIC(po);
    }
    // end of stream as md_pump: the end-of-stream packet's output takes one picture with flush, then one per mavc_queued (each must give one)
    int64_t po = 0;
    if (mavc_take(a, 1, dst, w, h, &po)) ON_PIC(po);
    while (mavc_queued(a) > 0) { if (!mavc_take(a, 1, dst, w, h, &po)) { r->eos_short++; break; } ON_PIC(po); }
    if (mavc_take(a, 1, dst, w, h, &po)) r->extra_after++;
    mavc_mc_info(a, &r->info);
    mavc_free(a);
    qsort(fed, (size_t)nfed, sizeof *fed, cmp64); qsort(got, (size_t)ngot, sizeof *got, cmp64);
    if (nfed != ngot) r->wrong_set = 1; else for (int i = 0; i < nfed; i++) if (fed[i] != got[i]) r->wrong_set = 1;
    if (o->drop_at) r->pics -= r->pre;   // pictures after the drop
    stats(lat, nlat, &r->p50, &r->p95, &r->mx);
    free(dst); free(ref); free(au); free(fed); free(got); free(lat);
    return nfed;
    #undef ON_PIC
}
static const char *cfname(int f) { return f == 19 ? "I420" : f == 21 ? "NV12" : f == 0x7fa30c04 ? "QCOM NV12 (venus)" : f == 0x7f420888 ? "YUV420Flexible" : f == 39 ? "NV12 packed" : "?"; }
static void say_info(const mmc_info *i) {
    say("  decoder %s: colour format %#x (%s), %d×%d, stride %d, slice height %d, crop %d,%d %d×%d, layout from %s, BT.%d %s range; %lld in, %lld out, %d format(s); %d passed over, %d dropped; opened in %.1f ms%s",
        i->codec[0] ? i->codec : "(none)", i->cfmt, cfname(i->cfmt), i->w, i->h, i->stride, i->slice, i->crop[0], i->crop[1], i->crop[2], i->crop[3], i->image_data ? "image-data" : "the colour format",
        i->matrix, i->full ? "full" : "video", (long long)i->nin, (long long)i->nout, i->nfmt, i->skipped, i->stale, i->open_ms, i->failed ? ", FELL BACK" : "");
}
static int video_case(const clip *c, const char *what, const vopt *o) {
    vres r; say("%s %s (%d×%d %s, %d access units)", c->name, what, c->w, c->h, c->prof, o->nmax ? o->nmax : c->n);
    int fed = run_video(c, o, &r);
    say_info(&r.info);
    int f0 = g_fail;
    check(!r.info.failed && r.info.codec[0], "MediaCodec decoded (no fallback)");
    check(r.pics == fed && !r.wrong_set, "%d pictures for %d access units%s, the same presentation times", r.pics, fed, o->drop_at ? " after the drop" : "");
    if (o->drop_at) check(r.pre <= o->drop_at, "%d pictures before the drop (Flush at access unit %d, on from the key frame at %d), each matching its reference", r.pre, o->drop_at, o->resume_at);
    check(!r.order_bad, "presentation order (%d out of order)", r.order_bad);
    check(!r.eos_short && !r.extra_after, "end of stream: every mavc_queued picture taken with flush (%d short), none after (%d)", r.eos_short, r.extra_after);
    check(!r.unknown && !r.ref_bad, "every time is an access unit's (%d unknown, %d references unreadable)", r.unknown, r.ref_bad);
    if (o->rgba) check(r.maxerr[0] <= 2 && r.maxerr[1] <= 2 && r.maxerr[2] <= 2, "RGBA (BT.%d %s) against the reference converted in double precision: largest error R %d G %d B %d (at most 2), PSNR %.1f %.1f %.1f dB",
                       r.info.matrix, r.info.full ? "full" : "video", r.maxerr[0], r.maxerr[1], r.maxerr[2], psnr(r.sse[0], r.cnt[0]), psnr(r.sse[1], r.cnt[1]), psnr(r.sse[2], r.cnt[2]));
    else check(psnr(r.sse[0], r.cnt[0]) >= 50 && psnr(r.sse[1], r.cnt[1]) >= 50 && psnr(r.sse[2], r.cnt[2]) >= 50 && r.maxerr[0] <= 4 && r.maxerr[1] <= 4 && r.maxerr[2] <= 4,
               "NV12 against ffmpeg: PSNR Y %.1f U %.1f V %.1f dB (at least 50), largest error %d %d %d (at most 4)", psnr(r.sse[0], r.cnt[0]), psnr(r.sse[1], r.cnt[1]), psnr(r.sse[2], r.cnt[2]), r.maxerr[0], r.maxerr[1], r.maxerr[2]);
    check(!r.info.skipped && !r.info.stale, "a clean stream: no access unit passed over (%d), no picture dropped (%d) by the output times", r.info.skipped, r.info.stale);
    check(r.info.ps_sent == (o->drop_at ? 1 : 0), "parameter sets sent in band %d time(s) (expected %d: %s)", r.info.ps_sent, o->drop_at ? 1 : 0, o->drop_at ? "once, after the flush" : "never, csd-0 / csd-1 at configure");
    if (o->dw || o->dh) check(!r.pad_bad, "output %d×%d for a %d×%d picture: %s (%d pictures wrong)", o->dw, o->dh, c->w, c->h, o->dw > c->w ? "black padding" : "cropped", r.pad_bad);
    say("  latency per access unit (decode + take): p50 %.2f ms, p95 %.2f ms, max %.2f ms; take alone max %.2f ms", r.p50, r.p95, r.mx, r.take_mx);
    return g_fail == f0;
}

// ---------- AAC ----------
typedef struct { int use_asc, cfg_rate, cfg_ch; int drop_at, resume_at; int dmg; } aopt;   // drop_at: maac_drop (a Flush) before packet drop_at, then on from resume_at; 0: none; dmg: this packet damaged
typedef struct {
    int srate, sch, rate, ch, F;          // maac_settle's rate / channels from Configure's (the format event's), maac_new's, frames per packet
    int nout, pre, size_bad, frames_bad, label_bad, missing, stuck, eos_short, extra_after;   // pre: outputs before the drop; label_bad: an output whose time is not the next packet's; missing: packets without one
    int discarded;                        // packets fed before the drop whose PCM the Flush threw away
    int64_t *lab; int *rank; int16_t *pcm;  // each output's time, its packet's rank in the clip (-1: none), its PCM (F frames)
    double *lat; int nlat; double p50, p95, mx; mmc_info info;
} ares;
static int out_size(const clip *c, int use_asc, int ch) {   // hle_movie.c GetOutputBufferSize for AAC on Android: the frames per packet maac_frames reads from the configuration × channels × 2
    return ch > 0 ? maac_frames(use_asc ? c->asc.p : NULL, use_asc ? (int)c->asc.n : 0) * ch * 2 : PCM_CAP;
}
static void g_adrop(void *x) { maac_drop(x); }
static void ares_free(ares *r) { free(r->lab); free(r->rank); free(r->pcm); free(r->lat); }
static void run_audio(const clip *c, const aopt *o, ares *r) {
    memset(r, 0, sizeof *r);
    const uint8_t *asc = o->use_asc ? c->asc.p : NULL; int an = o->use_asc ? (int)c->asc.n : 0;
    r->srate = o->cfg_rate; r->sch = o->cfg_ch; maac_settle(asc, an, &r->srate, &r->sch);   // h_movie_Configure
    int rate = o->cfg_rate, ch = o->cfg_ch; maac *a = maac_new(asc, an, &rate, &ch);           // md_aac, at the first packet (from Configure's own values here: both must settle alike)
    r->rate = rate; r->ch = ch; maac_info(a, &r->info); int F = r->F = r->info.frames;
    size_t cap = (size_t)c->n + 8; int lim = out_size(c, o->use_asc, ch);
    int16_t *out = malloc(PCM_CAP); r->pcm = calloc(cap * (size_t)F * (size_t)ch, 2); r->lab = calloc(cap, sizeof *r->lab); r->rank = calloc(cap, sizeof *r->rank); r->lat = calloc(cap, sizeof *r->lat);
    int64_t *fed = calloc(cap, sizeof *fed); int nfed = 0, next = 0, dropped = 0; static _Thread_local uint8_t dmgbuf[1 << 16];
    #define EMIT(fr_, po_) do { \
        if ((fr_) != F) r->frames_bad++; if ((fr_) * 2 * ch != lim) r->size_bad++; \
        if (next >= nfed || fed[next] != (po_)) r->label_bad++; next++; \
        if ((size_t)r->nout < cap) { r->lab[r->nout] = (po_); r->rank[r->nout] = pts_rank(c, po_); memcpy(r->pcm + (size_t)r->nout * F * ch, out, (size_t)F * 2 * ch); r->nout++; } \
    } while (0)
    for (int i = 0; i < c->n; i++) {
        if (o->drop_at && !dropped && i == o->drop_at) {   // a seek: Flush on a guest thread; the packets fed and not handed out are forgotten
            dropped = 1; r->pre = r->nout; on_guest(g_adrop, a); i = o->resume_at; r->discarded = nfed - next; nfed = next = 0;
        }
        int64_t po = 0, t0 = mmc_now_us();
        fed[nfed++] = c->u[i].pts;
        const uint8_t *pp = c->u[i].d.p; int pn = (int)c->u[i].d.n;
        if (o->dmg && i == o->dmg) { memcpy(dmgbuf, pp, (size_t)pn); for (int k = 2; k < pn; k++) dmgbuf[k] ^= 0xa5; pp = dmgbuf; }
        int fr = maac_decode(a, pp, pn, c->u[i].pts, 0, out, PCM_CAP, &po);
        if (fr > 0) EMIT(fr, po);
        r->lat[r->nlat++] = (mmc_now_us() - t0) / 1000.0;
        while (maac_ready(a) > 0) { if ((fr = maac_decode(a, NULL, 0, 0, 0, out, PCM_CAP, &po)) <= 0) { r->stuck++; break; } EMIT(fr, po); }   // md_pump: the late ones into free slots
    }
    int64_t po = 0;   // end of stream: md_pump's drain
    while (maac_queued(a) > 0) { int fr = maac_decode(a, NULL, 0, 0, 1, out, PCM_CAP, &po); if (fr <= 0) { r->eos_short++; break; } EMIT(fr, po); }
    if (maac_decode(a, NULL, 0, 0, 1, out, PCM_CAP, &po) > 0) r->extra_after++;
    r->missing += nfed - next;
    #undef EMIT
    maac_info(a, &r->info); maac_free(a); free(out); free(fed);
    double *v = malloc((size_t)(r->nlat ? r->nlat : 1) * sizeof *v); memcpy(v, r->lat, (size_t)r->nlat * sizeof *v); stats(v, r->nlat, &r->p50, &r->p95, &r->mx); free(v);
}
#define MIX(p, i, c) ((c) > 1 ? 0.5 * ((double)(p)[(size_t)(i) * (c)] + (p)[(size_t)(i) * (c) + 1]) : (double)(p)[i])
// PSNR of our frames [i0, i1) (ch channels) against ref[i + lag] (stereo): per channel, or on the downmix; INFINITY when equal, -1 when they do not overlap
static double seg_psnr(const int16_t *x, int ch, size_t i0, size_t i1, const int16_t *ref, size_t nr, int lag, int on_mix) {
    double sse = 0, n = 0;
    for (size_t i = i0; i < i1; i++) {
        long j = (long)i + lag; if (j < 0 || (size_t)j >= nr) continue;
        if (on_mix) { double d = MIX(x, i, ch) - MIX(ref, (size_t)j, 2); sse += d * d; n++; }
        else for (int k = 0; k < 2; k++) { double d = x[i * ch + (k < ch ? k : 0)] - ref[(size_t)j * 2 + k]; sse += d * d; n++; }
    }
    return !n ? -1 : sse > 0 ? 10 * log10(32767.0 * 32767.0 / (sse / n)) : INFINITY;
}
// the outputs placed by their times (rank × F) on a timeline of the whole clip, against the reference
typedef struct { int lag; double corr, all, tail; int lead_x, lead_r, bad, nchk; double worst; size_t nt; int badq[8]; double badp[8]; } acmp;
static void a_compare(const clip *c, const ares *r, int on_mix, int exp_lag, int lag_given, int rank_lo, int rank_mid, int rank_hi, double chunk_min, acmp *m) {
    memset(m, 0, sizeof *m); int F = r->F, ch = r->ch; size_t nr = c->nref / 4; const int16_t *ref = (const int16_t *)c->ref;
    m->nt = (size_t)c->n * F; int16_t *t = calloc(m->nt * (size_t)ch, 2);
    for (int k = 0; k < r->nout; k++) if (r->rank[k] >= 0) memcpy(t + (size_t)r->rank[k] * F * ch, r->pcm + (size_t)k * F * ch, (size_t)F * 2 * ch);
    // the offset: best normalised correlation of the downmixes over frames 8,192..16,383 within ±1,500 frames (the calibrated offsets are under 1,000; a packet's worth of misplacement shows)
    int L = 1500; size_t w0 = 8192, wn = 8192; m->corr = -2;
    if (lag_given) m->lag = exp_lag;
    else for (int l = -L; l <= L; l++) {
        if ((long)w0 + l < 0 || w0 + wn + (size_t)(l > 0 ? l : 0) > nr || w0 + wn > m->nt) continue;
        double sxy = 0, sxx = 0, syy = 0;
        for (size_t i = w0; i < w0 + wn; i++) { double a = MIX(t, i, ch), b = MIX(ref, (size_t)((long)i + l), 2); sxy += a * b; sxx += a * a; syy += b * b; }
        double cc = sxx > 0 && syy > 0 ? sxy / sqrt(sxx * syy) : -1; if (cc > m->corr) { m->corr = cc; m->lag = l; }
    }
    m->all = seg_psnr(t, ch, 0, m->nt, ref, nr, m->lag, on_mix);
    size_t i1 = m->nt; if ((long)nr - m->lag < (long)i1) i1 = (size_t)((long)nr - m->lag);
    m->tail = seg_psnr(t, ch, i1 > 4096 ? i1 - 4096 : 0, i1, ref, nr, m->lag, on_mix);
    m->lead_x = 0; while ((size_t)m->lead_x < m->nt && abs(t[(size_t)m->lead_x * ch]) <= 1 && abs(t[(size_t)m->lead_x * ch + (ch > 1)]) <= 1) m->lead_x++;
    m->lead_r = 0; while ((size_t)m->lead_r < nr && abs(ref[(size_t)m->lead_r * 2]) <= 1 && abs(ref[(size_t)m->lead_r * 2 + 1]) <= 1) m->lead_r++;
    m->worst = INFINITY;   // each packet's PCM where its time puts it: ranks [rank_lo, rank_mid) and [rank_hi, n)
    for (int k = 0; k < r->nout; k++) {
        int q = r->rank[k]; if (q < rank_lo || (q >= rank_mid && q < rank_hi)) continue;
        double p = seg_psnr(r->pcm + (size_t)k * F * ch, ch, 0, (size_t)F, ref + 0, nr, m->lag + q * F, on_mix);
        if (p < 0) continue;
        m->nchk++; if (p < m->worst) m->worst = p; if (p < chunk_min) { if (m->bad < 8) { m->badq[m->bad] = q; m->badp[m->bad] = p; } m->bad++; }
    }
    free(t);
}
static const char *bad_str(const acmp *m) {   // the packets below the floor: rank (PSNR)
    static _Thread_local char b[256]; b[0] = 0;
    for (int i = 0; i < m->bad && i < 8; i++) snprintf(b + strlen(b), sizeof b - strlen(b), "%s%d (%.1f dB)", i ? ", " : "", m->badq[i], m->badp[i]);
    return b;
}
static int lag_ok(int lag, const int *exp, int nexp) { for (int i = 0; i < nexp; i++) if (abs(lag - exp[i]) <= 4) return 1; return 0; }
static const char *lags_str(const int *exp, int nexp) { static char b[64]; b[0] = 0; for (int i = 0; i < nexp; i++) snprintf(b + strlen(b), sizeof b - strlen(b), "%s%d", i ? " or " : "", exp[i]); return b; }
static void say_ainfo(const ares *r) {
    say("  decoder %s: decoder output %d Hz %d ch; host %d Hz %d ch, %d frames per packet; %lld in, %lld out, holds %d back, %d handed out late, %d frames of silence put in, %d packets passed over, %d outputs dropped (%d frames beyond a packet's worth); opened in %.1f ms%s",
        r->info.codec[0] ? r->info.codec : "(none)", r->info.orate, r->info.och, r->info.rate, r->info.ch, r->info.frames, (long long)r->info.nin, (long long)r->info.nout, r->info.lag, r->info.late,
        r->info.padded, r->info.skipped, r->info.stale, r->info.surplus, r->info.open_ms, r->info.failed ? ", FELL BACK" : "");
}
// exp: the offsets calibrated beforehand (ref[i + offset] matches ours[i]), any of which within 4 frames passes
static int audio_case(const clip *c, const char *what, const aopt *o, double min_psnr, int on_mix, const int *exp, int nexp) {
    ares r; say("%s %s (AudioSpecificConfig %s%u bytes, Configure %d Hz %d ch, %d packets)", c->name, what, o->use_asc ? "" : "withheld, ", o->use_asc ? c->asc.n : 0, o->cfg_rate, o->cfg_ch, c->n);
    run_audio(c, o, &r);
    say_ainfo(&r);
    int f0 = g_fail, F = r.F; size_t nr = c->nref / 4;
    check(!r.info.failed && r.info.codec[0], "MediaCodec decoded (no fallback)");
    check(r.srate == c->exp_rate && r.sch == c->exp_ch && r.rate == r.srate && r.ch == r.sch && F == c->exp_frames,
          "the format event announces %d Hz %d ch (maac_settle at Configure, from %d Hz %d ch); the decoder hands out %d Hz %d ch, %d frames per packet (expected %d / %d / %d)",
          r.srate, r.sch, o->cfg_rate, o->cfg_ch, r.rate, r.ch, F, c->exp_rate, c->exp_ch, c->exp_frames);
    double pkt = c->n > 1 ? (double)(c->u[1].pts - c->u[0].pts) / 1e6 : 0, dur = (double)F / r.rate;
    check(!r.info.skipped && !r.info.stale && !r.info.surplus, "a clean stream: no packet passed over (%d), no PCM dropped (%d, %d frames beyond a packet's worth) by the output times", r.info.skipped, r.info.stale, r.info.surplus);
    check(fabs(dur - pkt) < 0.001, "each output lasts its packet's time: %d frames at %d Hz = %.2f ms, packets %.2f ms apart", F, r.rate, dur * 1000, pkt * 1000);
    check(!r.size_bad, "every output exactly the size GetOutputBufferSize advertises (%d bytes: %d frames × %d ch × 2): %d not", out_size(c, o->use_asc, r.ch), out_size(c, o->use_asc, r.ch) / (2 * r.ch), r.ch, r.size_bad);
    int n_exp = o->drop_at ? c->n - o->resume_at : c->n, n_got = r.nout - r.pre;
    check(n_got == n_exp && !r.frames_bad && !r.label_bad && !r.missing && !r.stuck && !r.eos_short && !r.extra_after,
          "%d outputs of %d frames for %d packets%s, each with its own packet's time, in order (%d wrong, %d without an output, %d of another length); end of stream: %d short, %d after",
          n_got, F, n_exp, o->drop_at ? " after the drop" : "", r.label_bad, r.missing, r.frames_bad, r.eos_short, r.extra_after);
    if (!o->drop_at) {
        acmp m; a_compare(c, &r, on_mix, 0, 0, 0, c->n, c->n, min_psnr - 10, &m);
        size_t total = (size_t)r.nout * F;
        check(labs((long)total - (long)nr) <= F, "%zu frames in all, ffmpeg's decode %zu (within one packet, %d)", total, nr, F);
        check(lag_ok(m.lag, exp, nexp) && m.corr >= 0.9, "PCM placed by the output times: offset %d frames against ffmpeg's decode (expected %s, within 4), correlation %.4f", m.lag, lags_str(exp, nexp), m.corr);
        check(abs(m.lead_r - m.lead_x - m.lag) <= 64, "priming: %d silent frames first, ffmpeg's decode %d, offset %d (within 64)", m.lead_x, m.lead_r, m.lag);
        check(m.all >= min_psnr, "PSNR over the whole track %.1f dB %s (at least %.0f)", m.all, on_mix ? "on the downmix" : "per channel", min_psnr);
        check(m.tail >= min_psnr - 10, "PSNR over its last 4,096 frames %.1f dB (at least %.0f)", m.tail, min_psnr - 10);
        check(!m.bad && m.nchk == r.nout, "each packet's PCM where its time puts it: %d of %d below %.0f dB, worst %.1f dB", m.bad, m.nchk, min_psnr - 10, m.worst);
    } else {   // before the drop: the outputs that came out; after it, from 2 packets past the resumption (the decoder starts afresh there; AAC frames overlap)
        acmp m; a_compare(c, &r, on_mix, exp[0], 1, 0, o->drop_at, o->resume_at + 2, min_psnr - 10, &m);
        int lo = c->n; for (int k = r.pre; k < r.nout; k++) if (r.rank[k] >= 0 && r.rank[k] < lo) lo = r.rank[k];
        check(r.pre + r.discarded == o->drop_at && lo == o->resume_at, "%d outputs before the drop at packet %d, %d packets' PCM thrown away by the Flush; the first after it is packet %d's (resumed at %d): nothing from before the Flush",
              r.pre, o->drop_at, r.discarded, lo, o->resume_at);
        check(!m.bad && m.nchk >= r.pre + (c->n - o->resume_at - 2), "each packet's PCM where its time puts it (offset %d), before the drop and from packet %d on: %d of %d below %.0f dB, worst %.1f dB",
              m.lag, o->resume_at + 2, m.bad, m.nchk, min_psnr - 10, m.worst);
    }
    say("  latency per packet: p50 %.2f ms, p95 %.2f ms, max %.2f ms", r.p50, r.p95, r.mx);
    ares_free(&r);
    return g_fail == f0;
}

// ---------- process state ----------
static int nfds(void) { int n = 0; DIR *d = opendir("/proc/self/fd"); if (!d) return -1; while (readdir(d)) n++; closedir(d); return n - 2; }
static long rss_kb(void) { long a = 0, b = 0; FILE *f = fopen("/proc/self/statm", "r"); if (!f) return -1; if (fscanf(f, "%ld %ld", &a, &b) != 2) b = -1; fclose(f); return b * (sysconf(_SC_PAGESIZE) / 1024); }
// memory by kind: the native heap in use (mallinfo) and the resident set as /proc/self/status splits it, in kB
typedef struct { long heap, anon, file, shmem; } memk;
static memk mem_now(void) {
    memk m = {0}; struct mallinfo mi = mallinfo(); m.heap = (long)(mi.uordblks / 1024);
    FILE *f = fopen("/proc/self/status", "r"); if (!f) return m; char l[256];
    while (fgets(l, sizeof l, f)) { sscanf(l, "RssAnon: %ld", &m.anon); sscanf(l, "RssFile: %ld", &m.file); sscanf(l, "RssShmem: %ld", &m.shmem); }
    fclose(f); return m;
}
static const char *mem_diff(memk a, memk b) {
    static char o[160]; snprintf(o, sizeof o, "heap %+ld, anon %+ld, file %+ld, shmem %+ld kB", b.heap - a.heap, b.anon - a.anon, b.file - a.file, b.shmem - a.shmem); return o;
}
// the descriptors by kind (the link's target with numbers taken out), into a table: kinds_diff says what changed
// (an app starts with well over 64 kinds, so the table holds 256)
typedef struct { char k[64]; int n; } fdk; typedef struct { fdk e[256]; int n; } fdkinds;
static void fd_kinds(fdkinds *t) {
    t->n = 0; DIR *d = opendir("/proc/self/fd"); if (!d) return; struct dirent *e; int self = dirfd(d);
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || atoi(e->d_name) == self) continue;
        char p[64], l[256]; snprintf(p, sizeof p, "/proc/self/fd/%s", e->d_name); ssize_t n = readlink(p, l, sizeof l - 1); if (n < 0) continue; l[n] = 0;
        char k[64]; int j = 0; for (ssize_t i = 0; i < n && j < 63; i++) if (!(l[i] >= '0' && l[i] <= '9')) k[j++] = l[i]; k[j] = 0;
        int f = -1; for (int i = 0; i < t->n; i++) if (!strcmp(t->e[i].k, k)) f = i;
        if (f < 0 && t->n < 256) { f = t->n++; snprintf(t->e[f].k, sizeof t->e[f].k, "%s", k); t->e[f].n = 0; }
        if (f >= 0) t->e[f].n++;
    }
    closedir(d);
}
static const char *kinds_diff(const fdkinds *a, const fdkinds *b) {
    static char o[1024]; o[0] = 0;
    for (int i = 0; i < b->n; i++) { int na = 0; for (int j = 0; j < a->n; j++) if (!strcmp(a->e[j].k, b->e[i].k)) na = a->e[j].n; if (b->e[i].n != na) snprintf(o + strlen(o), sizeof o - strlen(o), "%s %s %+d", o[0] ? "," : "", b->e[i].k, b->e[i].n - na); }
    for (int j = 0; j < a->n; j++) { int nb = 0; for (int i = 0; i < b->n; i++) if (!strcmp(a->e[j].k, b->e[i].k)) nb = 1; if (!nb) snprintf(o + strlen(o), sizeof o - strlen(o), "%s %s %+d", o[0] ? "," : "", a->e[j].k, -a->e[j].n); }
    return o[0] ? o : " none";
}
// Theil-Sen slope: the median of the slopes between every two samples (a few outlying samples do not move it)
static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double theil_sen(const double *x, const double *y, int n) {
    double *s = malloc(sizeof *s * (size_t)(n * (n - 1) / 2 + 1)); int m = 0;
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (x[j] != x[i]) s[m++] = (y[j] - y[i]) / (x[j] - x[i]);
    qsort(s, (size_t)m, sizeof *s, dcmp); double r = !m ? 0 : m & 1 ? s[m / 2] : (s[m / 2 - 1] + s[m / 2]) / 2; free(s); return r;
}
// the live counts (movie_mc.h) back to their values at the start: a failed decoder's helper may still be stopping its codec, so this waits up to 5 s for them
static int g_lv0[MMC_LIVE_N];
static void check_live(const char *after) {
    int now[MMC_LIVE_N], same = 0; int64_t t0 = mmc_now_us();
    for (;;) { mmc_live_get(now); same = !memcmp(now, g_lv0, sizeof now); if (same || mmc_now_us() - t0 > 5000000) break; usleep(10000); }
    check(same, "after %s the live counts are back to the start's (after %.2f s): H.264 decoders %d, AAC decoders %d, codecs %d, helper jobs %d, helper threads %d (at the start %d %d %d %d %d)", after,
          (mmc_now_us() - t0) / 1e6, now[MMC_LIVE_MAVC], now[MMC_LIVE_MAAC], now[MMC_LIVE_CODEC], now[MMC_LIVE_JOB], now[MMC_LIVE_THREAD],
          g_lv0[MMC_LIVE_MAVC], g_lv0[MMC_LIVE_MAAC], g_lv0[MMC_LIVE_CODEC], g_lv0[MMC_LIVE_JOB], g_lv0[MMC_LIVE_THREAD]);
}
static long stderr_pos(void) { fflush(stderr); return lseek(2, 0, SEEK_CUR); }
static int reds_since(long pos) {   // [RED] lines written to <out>/stderr.txt since pos
    fflush(stderr); char p[600]; snprintf(p, sizeof p, "%s/stderr.txt", g_out);
    FILE *f = fopen(p, "r"); if (!f) return -1; fseek(f, pos, SEEK_SET);
    char line[1024]; int n = 0; while (fgets(line, sizeof line, f)) if (strstr(line, "[RED]")) n++;
    fclose(f); return n;
}
// up to `cap` 1080p H.264 decoders open at once (each fed its first access unit), then freed: how many MediaCodec gave
static int instances(const clip *c, int cap) {
    mavc *a[16]; int ok = 0; long pos = stderr_pos();
    for (int i = 0; i < cap; i++) {
        vsetup s = {NULL, c, 0, 0}; on_guest(g_setup, &s); a[i] = s.a;
        mavc_decode(a[i], c->u[0].d.p, (int)c->u[0].d.n, c->u[0].pts);
        mmc_info inf; mavc_mc_info(a[i], &inf); if (!inf.failed && inf.codec[0]) ok++;
    }
    for (int i = 0; i < cap; i++) mavc_free(a[i]);
    int reds = reds_since(pos); if (reds) say("  (%d [RED] lines from decoders that could not be made)", reds);
    return ok;
}
// a damaged unit: the decoder may conceal it, pass over it or fail; each way every unit keeps one picture / output at its time, in order. Pixels / PCM away from the damage still match
// (a decoder that fell back: black pictures / silence from there, one [RED])
static void damaged_video(const clip *c, int dmg) {
    long pos = stderr_pos(); vopt o = {.dmg = dmg}; vres r; say("%s, access unit %d damaged (its second half's bytes flipped)", c->name, dmg);
    int fed = run_video(c, &o, &r); say_info(&r.info); int reds = reds_since(pos);
    say("  the decoder %s; %d [RED] line(s)", r.info.failed ? "fell back" : r.info.skipped ? "passed over access units (black pictures in their place)" : "gave a picture for every access unit", reds);
    check(r.pics == fed && !r.wrong_set && !r.order_bad && !r.unknown && !r.eos_short && !r.extra_after, "%d pictures for %d access units, the same times, in order", r.pics, fed);
    if (!r.info.failed) check(psnr(r.sse[0], r.cnt[0]) >= 50 && r.maxerr[0] <= 4 && !reds, "away from the damage (presentation ranks below 6, and from the key frame at %d): PSNR Y %.1f dB, largest error %d; no [RED]",
                              c->key2, psnr(r.sse[0], r.cnt[0]), r.maxerr[0]);
    else check(reds == 1, "fell back with one [RED] line (%d)", reds);
}
static void damaged_audio(const clip *c, int dmg) {
    long pos = stderr_pos(); aopt o = {1, c->rate, c->ch, .dmg = dmg}; ares r; say("%s, packet %d damaged (its bytes from the third on flipped)", c->name, dmg);
    run_audio(c, &o, &r); say_ainfo(&r); int reds = reds_since(pos);
    say("  the decoder %s; %d [RED] line(s)", r.info.failed ? "fell back" : r.info.skipped ? "passed over packets (silence in their place)" : "gave PCM for every packet", reds);
    check(r.nout == c->n && !r.frames_bad && !r.label_bad && !r.missing && !r.stuck && !r.eos_short && !r.extra_after && !r.size_bad,
          "%d outputs of %d frames for %d packets, each with its own packet's time, in order (%d wrong)", r.nout, r.F, c->n, r.label_bad);
    // the decoder's concealment may reach back as far as the packets it holds back, and 2 packets after the damage (seen on the Nubia NX789J: packets 18 to 21 below 50 dB for a damaged 20,
    // with one silent run of 1,024 frames within 18 and 19, the decoder holding 2 back): the rest must match where its time puts it. The reach is the count the decoder holds back, checked on
    // its own and capped at 2, so a larger count (a regression in that heuristic) cannot widen the window unseen; and every packet outside the window must have been compared
    int reach = r.info.lag > 0 ? r.info.lag : 0;
    if (!r.info.failed) check(reach <= 2, "the decoder (%s) holds %d packet(s) back: at most 2, how far before the damage its concealment is allowed to reach", r.info.codec, r.info.lag);
    if (reach > 2) reach = 2;
    int lo = dmg - reach, outside = lo + c->n - (dmg + 3);
    acmp m; a_compare(c, &r, 0, 0, 1, 0, lo, dmg + 3, 50, &m);
    {   // diagnostics: each packet's PSNR around the damage, and the PCM placed by the output times (s16 stereo, <out>/damaged_<clip>.pcm) for a look on the Mac
        char b[512] = ""; size_t nr = c->nref / 4; int F = r.F;
        for (int k = 0; k < r.nout; k++) { int q = r.rank[k]; if (q < dmg - 5 || q > dmg + 5) continue;
            double p = seg_psnr(r.pcm + (size_t)k * F * r.ch, r.ch, 0, (size_t)F, (const int16_t *)c->ref, nr, q * F, 0); snprintf(b + strlen(b), sizeof b - strlen(b), " %d:%.1f", q, p); }
        say("  diag packets around the damage (rank:PSNR dB):%s", b);
        char path[700]; snprintf(path, sizeof path, "%s/damaged_%s.pcm", g_out, c->name); FILE *f = fopen(path, "wb");
        if (f) { int16_t *t = calloc((size_t)c->n * F * r.ch, 2); for (int k = 0; k < r.nout; k++) if (r.rank[k] >= 0) memcpy(t + (size_t)r.rank[k] * F * r.ch, r.pcm + (size_t)k * F * r.ch, (size_t)F * 2 * r.ch); fwrite(t, 2, (size_t)c->n * F * r.ch, f); fclose(f); free(t); }
    }
    if (!r.info.failed) check(!m.bad && !reds && m.nchk == outside, "away from the damage (packets before %d = %d less the %d the decoder holds back, and from %d on): %d of %d packets compared (all %d outside the window), "
                              "%d below 50 dB, worst %.1f dB%s%s; no [RED]", lo, dmg, reach, dmg + 3, m.nchk, c->n, outside, m.bad, m.worst, m.bad ? ": packets " : "", bad_str(&m));
    else {
        int F = r.F, loud = 0; for (int q = 0; q < r.nout; q++) if (r.rank[q] > dmg + 1) for (size_t i = 0; i < (size_t)F * r.ch; i++) if (r.pcm[(size_t)q * F * r.ch + i]) { loud++; break; }
        check(reds == 1 && !loud, "fell back with one [RED] line (%d), silent after it (%d packets not)", reds, loud);
    }
    ares_free(&r);
}
// one create / free cycle as in the stress: 8 access units, or 8 AAC packets with the end of stream; 0 = all of it decoded with the right times
static int cyc_avc(const clip *v, double *open_mx) { vopt o = {.nmax = 8}; vres r; int fed = run_video(v, &o, &r); if (r.info.open_ms > *open_mx) *open_mx = r.info.open_ms; return r.pics != fed || r.info.failed; }
static int cyc_aac(const clip *c, double *open_mx) {
    int rt = c->rate, ch = c->ch; maac *a = maac_new(c->asc.p, (int)c->asc.n, &rt, &ch); int16_t *out = malloc(PCM_CAP); int got = 0, nout = 0, wrong = 0; int64_t po;
    for (int i = 0; i < 8; i++) {
        int fr = maac_decode(a, c->u[i].d.p, (int)c->u[i].d.n, c->u[i].pts, 0, out, PCM_CAP, &po); if (fr > 0) { got += fr; wrong += po != c->u[nout++].pts; }
        while (maac_ready(a) > 0 && (fr = maac_decode(a, NULL, 0, 0, 0, out, PCM_CAP, &po)) > 0) { got += fr; wrong += po != c->u[nout++].pts; }
    }
    for (int fr; maac_queued(a) > 0 && (fr = maac_decode(a, NULL, 0, 0, 1, out, PCM_CAP, &po)) > 0;) { got += fr; wrong += po != c->u[nout++].pts; }
    mmc_info inf; maac_info(a, &inf); if (inf.open_ms > *open_mx) *open_mx = inf.open_ms;
    maac_free(a); free(out);
    return inf.failed || got != 8 * 1024 || nout != 8 || wrong;
}
typedef struct { const clip *c; vopt o; vres r; int fed; const clip *ac; aopt ao; ares ar; } par;
static void *par_video(void *x) { par *p = x; p->fed = run_video(p->c, &p->o, &p->r); return NULL; }
static void *par_audio(void *x) { par *p = x; run_audio(p->ac, &p->ao, &p->ar); return NULL; }

int probe_main(const char *out_dir, int in_app, int argc, char **argv) {
    snprintf(g_out, sizeof g_out, "%s", out_dir);
    const char *dir = argc > 0 ? argv[0] : out_dir;
    char p[700]; snprintf(p, sizeof p, "%s/result.txt", out_dir); g_rep = fopen(p, "w");
    snprintf(p, sizeof p, "%s/stderr.txt", out_dir); if (!freopen(p, "w", stderr)) say("cannot redirect stderr to %s", p);
    setvbuf(stderr, NULL, _IOLBF, 0);
    char ctx[128] = "?"; FILE *f = fopen("/proc/self/attr/current", "r"); if (f) { if (!fgets(ctx, sizeof ctx, f)) strcpy(ctx, "?"); fclose(f); ctx[strcspn(ctx, "\n")] = 0; }
    say("t_movie_mc: %s, SELinux context %s, clips from %s", in_app ? "inside the APK" : "adb executable", ctx, dir);
    // the manifest: video <name> <w> <h> <units> <profile> <second key frame> / audio <name> <Configure rate> <ch> <expected rate> <ch> <frames>
    clip cl[16]; int nc = 0;
    snprintf(p, sizeof p, "%s/manifest.txt", dir); f = fopen(p, "r");
    if (!f) { say("FAIL: no %s", p); return 1; }
    char line[256];
    while (nc < 16 && fgets(line, sizeof line, f)) {
        clip *c = &cl[nc]; memset(c, 0, sizeof *c); char kind[8];
        if (sscanf(line, "%7s", kind) != 1) continue;
        if (!strcmp(kind, "video") && sscanf(line, "%*s %31s %d %d %d %15s %d", c->name, &c->w, &c->h, &c->n, c->prof, &c->key2) == 6) c->kind = 1;
        else if (!strcmp(kind, "audio") && sscanf(line, "%*s %31s %d %d %d %d %d", c->name, &c->rate, &c->ch, &c->exp_rate, &c->exp_ch, &c->exp_frames) == 6) c->kind = 2;
        else continue;
        if (!load(c, dir)) { say("FAIL: clip %s unreadable", c->name); return 1; }
        nc++;
    }
    fclose(f);
    #define CLIP(nm) ({ const clip *c_ = NULL; for (int i_ = 0; i_ < nc; i_++) if (!strcmp(cl[i_].name, nm)) c_ = &cl[i_]; if (!c_) { say("FAIL: no clip " nm); return 1; } c_; })
    const clip *v720 = CLIP("v720"), *v1080 = CLIP("v1080"), *v480 = CLIP("v480"), *v360 = CLIP("v360"), *vbase = CLIP("vbase");
    const clip *alc = CLIP("alc"), *ahe = CLIP("ahe"), *ahex = CLIP("ahex"), *ahe2 = CLIP("ahe2"), *ahe2x = CLIP("ahe2x"), *a960 = CLIP("a960");
    unsetenv("DM_MOVIE_MC_FAIL"); unsetenv("DM_MOVIE_MC_AAC_NOWAIT");
    mmc_live_get(g_lv0);
    const char *cy = getenv("T_MOVIE_MC_CYCLES");
    if (cy && atoi(cy) > 0) {   // only that many create / free cycles (H.264 + AAC), after one of each: for a heap profile of the cycles (malloc_debug's report at exit), nothing else runs
        int n = atoi(cy), bad = 0; double om = 0; bad += cyc_avc(v720, &om) + cyc_aac(alc, &om); memk m0 = mem_now(); int fd0 = nfds();
        for (int i = 0; i < n; i++) { bad += cyc_avc(v720, &om); bad += cyc_aac(alc, &om); }
        memk m1 = mem_now(); say("cycles only: %d cycles after the first, %d bad; then descriptors %d → %d, %s", n, bad, fd0, nfds(), mem_diff(m0, m1));
        check(!bad, "every cycle decoded fully"); check_live("the cycles");
        say("\nt_movie_mc: %d checks, %d failed", g_pass + g_fail, g_fail); if (g_rep) fclose(g_rep);
        return g_fail ? 1 : 0;
    }

    say("\n== H.264: parameter sets from csd-0 / csd-1 only, NV12");
    const clip *vs[] = {v720, v1080, v480, v360, vbase};
    for (int i = 0; i < 5; i++) { vopt o = {0}; video_case(vs[i], "NV12", &o); }
    say("\n== H.264: the avcC record, units with 4-byte lengths, RGBA (mavc_set_rgba)");
    { vopt o = {.rgba = 1, .avcc = 1, .lenpfx = 1}; video_case(v720, "RGBA, avcC", &o); video_case(v360, "RGBA, avcC", &o); }
    { vopt o = {.avcc = 1, .lenpfx = 1}; video_case(v1080, "NV12, avcC", &o); }
    say("\n== H.264: crop and padding (the output larger / smaller than the picture)");
    { vopt o = {.dw = v360->w + 16, .dh = v360->h + 16}; video_case(v360, "NV12 into a larger frame", &o); }
    { vopt o = {.dw = v360->w - 16, .dh = v360->h - 16}; video_case(v360, "NV12 into a smaller frame", &o); }
    { vopt o = {.rgba = 1, .dw = v480->w + 16, .dh = v480->h - 16}; video_case(v480, "RGBA, wider and shorter", &o); }
    say("\n== H.264: mavc_drop mid-stream (a seek), then on from a key frame");
    { vopt o = {.drop_at = 30, .resume_at = v720->key2}; video_case(v720, "drop at 30", &o); }
    { vopt o = {.drop_at = 10, .resume_at = 0}; video_case(vbase, "drop at 10, back to the start (a loop)", &o); }
    say("\n== H.264: no picture taken until the end of stream (every one queued at once)");
    { vopt o = {.no_take = 1}; video_case(v720, "no take until the end", &o); }

    // the offsets calibrated beforehand: AAC-LC 0 (the 960-sample clip: 0 by its reference, ffmpeg's own decode; no other decoder here reads it); HE-AAC at twice the core rate 0, or 962 where the decoder takes out SBR's delay (as AudioToolbox does); halved to the core rate (8 frames of
    // the filter's delay) -8, or 473
    static const int lag_lc[] = {0}, lag_he[] = {0, 962}, lag_half[] = {-8, 473};
    say("\n== AAC");
    { aopt o = {1, alc->rate, alc->ch}; audio_case(alc, "AAC-LC stereo 48 kHz", &o, 60, 0, lag_lc, 1); }
    { aopt o = {0, alc->rate, alc->ch}; audio_case(alc, "AAC-LC without its configuration (made from Configure's rate and channels)", &o, 60, 0, lag_lc, 1); }
    { aopt o = {1, ahe->rate, ahe->ch}; audio_case(ahe, "HE-AAC (afconvert: SBR by extension), Configure as its sample entry", &o, 30, 0, lag_half, 2); }
    { aopt o = {1, 0, 0}; audio_case(ahe, "HE-AAC (afconvert), Configure without rate / channels", &o, 30, 0, lag_half, 2); }
    { aopt o = {1, ahex->rate, ahex->ch}; audio_case(ahex, "HE-AAC, SBR by object type 5", &o, 30, 0, lag_he, 2); }
    { aopt o = {1, ahe2->rate, ahe2->ch}; audio_case(ahe2, "HE-AAC v2 (a mono core with parametric stereo, by extension), Configure at twice the core rate: the format announces the core rate", &o, 30, 1, lag_half, 2); }
    { aopt o = {1, 0, 0}; audio_case(ahe2x, "HE-AAC v2, object type 29, Configure without rate / channels", &o, 30, 1, lag_he, 2); }
    { aopt o = {1, a960->rate, a960->ch}; audio_case(a960, "AAC-LC with 960-sample frames (frameLengthFlag, AudioSpecificConfig 11 94)", &o, 60, 0, lag_lc, 1); }
    say("\n== AAC: no wait for a packet's own PCM (DM_MOVIE_MC_AAC_NOWAIT=1): the decoder's delay shows, the PCM still goes out with its own packet's time");
    setenv("DM_MOVIE_MC_AAC_NOWAIT", "1", 1);
    { aopt o = {1, alc->rate, alc->ch}; audio_case(alc, "AAC-LC, no wait", &o, 60, 0, lag_lc, 1); }
    { aopt o = {1, ahe->rate, ahe->ch}; audio_case(ahe, "HE-AAC by extension, halved, no wait", &o, 30, 0, lag_half, 2); }
    unsetenv("DM_MOVIE_MC_AAC_NOWAIT");
    say("\n== AAC: maac_drop (a Flush) mid-stream on a guest thread, then on from elsewhere");
    { aopt o = {1, alc->rate, alc->ch, 40, 80}; audio_case(alc, "AAC-LC, Flush before packet 40, on from 80", &o, 60, 0, lag_lc, 1); }
    setenv("DM_MOVIE_MC_AAC_NOWAIT", "1", 1);
    { aopt o = {1, alc->rate, alc->ch, 40, 80}; audio_case(alc, "AAC-LC, Flush before packet 40, on from 80, no wait", &o, 60, 0, lag_lc, 1); }
    unsetenv("DM_MOVIE_MC_AAC_NOWAIT");

    say("\n== Damaged units: one picture / output per unit at its time all the same");
    damaged_video(v720, 10);
    damaged_audio(alc, 20);

    say("\n== Stress");
    { vopt o = {0}; vres r; say("1080p latency, NV12 then RGBA (%d access units each)", v1080->n); run_video(v1080, &o, &r);
      say("  NV12: p50 %.2f ms, p95 %.2f ms, max %.2f ms per access unit (decode + take)", r.p50, r.p95, r.mx);
      check(r.p95 < 33.3, "NV12 p95 under a 30 fps frame (33.3 ms)");
      o.rgba = 1; run_video(v1080, &o, &r); say("  RGBA: p50 %.2f ms, p95 %.2f ms, max %.2f ms per access unit (decode + take)", r.p50, r.p95, r.mx);
      check(r.p95 < 33.3, "RGBA p95 under a 30 fps frame (33.3 ms)"); }
    {
        say("Hardware instances, then 250 create / free cycles (H.264 + AAC each), cycles of each alone, then instances again");
        int cap = 8, before = instances(v1080, cap);
        { vopt o = {.nmax = 8}; vres r; run_video(v720, &o, &r); maac *a; int rt = 0, ch = 0; a = maac_new(alc->asc.p, (int)alc->asc.n, &rt, &ch); maac_free(a); }   // warm-up
        // Seen on the Nubia NX789J: the first 50 cycles added 176 to 188 /dev/ashmem descriptors; over cycles 75 to 250 the count then stayed within a band of at most 16 (8 to 16 by run, 7 runs:
        // 5 sampled every 25 cycles, 2 every 5). The native heap rose 0.47 to 0.52 kB per cycle there (5 runs), steadily. The live counts (check_live) say every decoder, codec, helper job and helper
        // thread of ours is gone after the cycles, so the rise is none of those; what it is has not been attributed (a malloc_debug pass of the cycles alone: 83 bytes per cycle left allocated at exit, every one with the system's Codec2 client in its backtrace).
        // The descriptors and the native heap are checked by their slope over cycles 75 to 250 (Theil-Sen, a sample every 5 cycles): at most 0.045 descriptors and 1.2 kB per cycle. On the
        // series sampled every 25 cycles the descriptor slopes were -0.033 to +0.029 per cycle and the heap's 0.49 to 0.52 kB; with a leak of one descriptor per 12.5 cycles (0.08), or of 0.72 kB
        // per cycle, added to them every run fails. Sampled every 5 cycles (2 runs): descriptors +0.000 per cycle, heap 0.47 and 0.50 kB per cycle.
        // The resident set is reported, not checked: over cycles 75 to 250 it stayed within about 0.3 MB of the count after 50 in the exe (3 runs), and rose 1.7 to 8.9 MB in the APK (4 runs)
        // while the heap rose about 0.1 MB (3 of those runs), which is not explained. (rss0 / m0 are taken after the first instances(): the change after 50 cycles includes what those used)
        static fdkinds k0, k50, k, k2; enum { NS = 36 }; double sx[NS], sfd[NS], sheap[NS]; int ns = 0;
        int fd0 = nfds(); long rss0 = rss_kb(); memk m0 = mem_now(); fd_kinds(&k0); int bad = 0; double open_mx = 0; int64_t t0 = mmc_now_us();
        for (int k = 0; k < 50; k++) { bad += cyc_avc(v720, &open_mx); bad += cyc_aac(alc, &open_mx); }
        double secs = (mmc_now_us() - t0) / 1e6; int fd50 = nfds(); long rss50 = rss_kb(); memk m50 = mem_now(); fd_kinds(&k50);
        say("  after 50 cycles: descriptors %d → %d (%s), resident %+ld kB (%s)", fd0, fd50, kinds_diff(&k0, &k50), rss50 - rss0, mem_diff(m0, m50));
        char b[600] = "", bm[1200] = ""; long rssmax = 0; int fdlo = 1 << 30, fdhi = 0;
        for (int i = 1; i <= 200; i++) {
            bad += cyc_avc(v720, &open_mx); bad += cyc_aac(alc, &open_mx);
            if (i >= 25 && i % 5 == 0 && ns < NS) {
                int f = nfds(); memk mk = mem_now(); sx[ns] = 50 + i; sfd[ns] = f; sheap[ns] = (double)mk.heap; ns++; if (f < fdlo) fdlo = f; if (f > fdhi) fdhi = f;
                if (i % 25 == 0) {
                    long m = rss_kb(); if (m > rssmax) rssmax = m;
                    snprintf(b + strlen(b), sizeof b - strlen(b), " %d:%d/%+ldkB", 50 + i, f, m - rss0);
                    snprintf(bm + strlen(bm), sizeof bm - strlen(bm), " %d:%+ld/%+ld/%+ld/%+ld", 50 + i, mk.heap - m50.heap, mk.anon - m50.anon, mk.file - m50.file, mk.shmem - m50.shmem);
                }
            }
        }
        say("  then every 25 cycles (descriptors / resident against the start):%s", b);
        say("  memory against after 50 (heap / anon / file / shmem kB):%s", bm);
        double fd_slope = theil_sen(sx, sfd, ns), heap_slope = theil_sen(sx, sheap, ns);
        check(!bad, "250 cycles of 8 access units + 8 packets, each decoded fully (8 pictures; 8 × 1,024 frames with their packets' times) (%d bad); the first 50 in %.2f s; slowest open %.1f ms", bad, secs, open_mx);
        check(ns == NS && fd_slope <= 0.045, "descriptors over cycles 75 to 250 (%d samples, %d to %d; %d after 50): slope %+.3f per cycle (at most 0.045)", ns, fdlo, fdhi, fd50, fd_slope);
        check(ns == NS && heap_slope <= 1.2, "native heap in use over cycles 75 to 250: slope %.2f kB per cycle (at most 1.2), %+.0f kB from cycle 75 to 250", heap_slope, sheap[ns - 1] - sheap[0]);
        say("  resident memory (reported, not checked): at most %+ld kB over cycles 75 to 250 against the count after 50", rssmax - rss50);
        { fd_kinds(&k); int f = nfds(); memk ma = mem_now(); double om = 0; int bb = 0; for (int i = 0; i < 20; i++) bb += cyc_avc(v720, &om); fd_kinds(&k2);
          say("  diag then 20 H.264 cycles%s: descriptors %d → %d (%s), %s", bb ? " (some bad)" : "", f, nfds(), kinds_diff(&k, &k2), mem_diff(ma, mem_now())); }
        { fd_kinds(&k); int f = nfds(); memk ma = mem_now(); double om = 0; int bb = 0; for (int i = 0; i < 20; i++) bb += cyc_aac(alc, &om); fd_kinds(&k2);
          say("  diag then 20 AAC cycles%s: descriptors %d → %d (%s), %s", bb ? " (some bad)" : "", f, nfds(), kinds_diff(&k, &k2), mem_diff(ma, mem_now())); }
        int after = instances(v1080, cap);
        check(before >= 2 && after == before, "1080p H.264 decoders open at once: %d of %d before the cycles, %d after", before, cap, after);
        check_live("the cycles and the instances");
    }
    {
        say("Decoders at once: 1080p and 720p H.264 and an AAC track, each on its own thread");
        par a = {.c = v1080}, b = {.c = v720}, au = {.ac = alc, .ao = {1, alc->rate, alc->ch}};
        pthread_t ta, tb, tc; pthread_create(&ta, NULL, par_video, &a); pthread_create(&tb, NULL, par_video, &b); pthread_create(&tc, NULL, par_audio, &au);
        pthread_join(ta, NULL); pthread_join(tb, NULL); pthread_join(tc, NULL);
        for (int k = 0; k < 2; k++) { par *x = k ? &b : &a;
            check(!x->r.info.failed && x->r.pics == x->fed && !x->r.wrong_set && !x->r.order_bad && psnr(x->r.sse[0], x->r.cnt[0]) >= 50 && x->r.maxerr[0] <= 4 && !x->r.info.skipped && !x->r.info.stale,
                  "%s: %d pictures of %d in order, PSNR Y %.1f dB, p95 %.2f ms; passed over %d, dropped %d", x->c->name, x->r.pics, x->fed, psnr(x->r.sse[0], x->r.cnt[0]), x->r.p95, x->r.info.skipped, x->r.info.stale); }
        acmp m; a_compare(alc, &au.ar, 0, 0, 1, 0, alc->n, alc->n, 50, &m);
        check(!au.ar.info.failed && au.ar.nout == alc->n && !au.ar.frames_bad && !au.ar.label_bad && !m.bad && !au.ar.info.skipped && !au.ar.info.stale,
              "alc: %d outputs for %d packets with their times (%d wrong), each packet's PCM matching (worst %.1f dB), p95 %.2f ms; passed over %d, dropped %d",
              au.ar.nout, alc->n, au.ar.label_bad, m.worst, au.ar.p95, au.ar.info.skipped, au.ar.info.stale);
        ares_free(&au.ar);
        check_live("the decoders at once");
    }

    say("\n== Fallbacks (DM_MOVIE_MC_FAIL)");
    {
        long pos = stderr_pos(); setenv("DM_MOVIE_MC_FAIL", "avc", 1);
        vopt o = {.expect_black = 1}; vres r; int fed = run_video(v720, &o, &r); unsetenv("DM_MOVIE_MC_FAIL");
        say("avc: no decoder can be made"); say_info(&r.info);
        check(r.info.failed && r.pics == fed && !r.wrong_set && !r.order_bad && r.black == r.pics && !r.eos_short, "%d black pictures for %d access units, in order, at their times", r.black, fed);
        check(reds_since(pos) == 1, "one [RED] line (%d)", reds_since(pos));
        check(r.mx_from < 5, "every call returns at once: slowest %.2f ms (under 5)", r.mx_from);
    }
    {
        long pos = stderr_pos(); setenv("DM_MOVIE_MC_FAIL", "avc@20", 1);
        vopt o = {.lat_from = 19}; vres r; int fed = run_video(v720, &o, &r); unsetenv("DM_MOVIE_MC_FAIL");
        say("avc@20: the decoder fails at the 20th access unit"); say_info(&r.info);
        check(r.info.failed && r.pics == fed && !r.wrong_set && !r.order_bad && !r.eos_short, "%d pictures for %d access units (decoded, then black), in order, at their times", r.pics, fed);
        check(reds_since(pos) == 1, "one [RED] line (%d)", reds_since(pos));
        check(r.mx_from < 5, "the failing call and those after it return at once (the codec stopped and deleted on a helper thread): slowest %.2f ms (under 5)", r.mx_from);
    }
    {
        long pos = stderr_pos(); setenv("DM_MOVIE_MC_FAIL", "avc@20", 1);
        vopt o = {.no_take = 1}; vres r; int fed = run_video(v720, &o, &r); unsetenv("DM_MOVIE_MC_FAIL");
        say("avc@20, no picture taken until the end of stream: every access unit in the decoder or queued keeps its time"); say_info(&r.info);
        check(r.info.failed && r.pics == fed && !r.wrong_set && !r.order_bad && !r.eos_short, "%d pictures for %d access units, in order, at their times", r.pics, fed);
        check(reds_since(pos) == 1, "one [RED] line (%d)", reds_since(pos));
    }
    {
        long pos = stderr_pos(); setenv("DM_MOVIE_MC_FAIL", "avc@20", 1);
        vopt o = {.rgba = 1, .avcc = 1}; vres r; int fed = run_video(vbase, &o, &r); unsetenv("DM_MOVIE_MC_FAIL");
        say("avc@20, Baseline, RGBA"); check(r.info.failed && r.pics == fed && !r.wrong_set && !r.order_bad && !r.eos_short, "%d pictures for %d access units, in order", r.pics, fed);
        check(reds_since(pos) == 1, "one [RED] line (%d)", reds_since(pos));
    }
    for (int k = 0; k < 2; k++) {
        long pos = stderr_pos(); setenv("DM_MOVIE_MC_FAIL", k ? "aac@20" : "aac", 1);
        aopt o = {1, ahe->rate, ahe->ch}; ares r; run_audio(ahe, &o, &r); unsetenv("DM_MOVIE_MC_FAIL");
        say("%s: %s", k ? "aac@20" : "aac", k ? "the decoder fails at the 20th packet" : "no decoder can be made"); say_ainfo(&r);
        // silence from the 20th packet on; before it, the packets whose PCM had come (all but those the decoder still held) match the reference
        int F = r.F, loud = 0; for (int q = 0; q < r.nout; q++) if (r.rank[q] >= (k ? 19 : 0)) for (size_t i = 0; i < (size_t)F * r.ch; i++) if (r.pcm[(size_t)q * F * r.ch + i]) { loud++; break; }
        check(r.info.failed && r.rate == ahe->exp_rate && r.ch == ahe->exp_ch && r.nout == ahe->n && !r.frames_bad && !r.label_bad && !r.missing && !r.eos_short && !loud,
              "%d outputs of %d frames for %d packets at %d Hz %d ch with their times (%d wrong), silent from packet %d (%d not)", r.nout, F, ahe->n, r.rate, r.ch, r.label_bad, k ? 20 : 1, loud);
        if (k) {   // the offset from frames 8,192..16,383 (packets 9 to 16, before the failure)
            int keep = 19 - r.info.lag - 1; if (keep < 0) keep = 0; acmp m; a_compare(ahe, &r, 0, 0, 0, 0, keep, ahe->n, 20, &m);
            check(lag_ok(m.lag, lag_half, 2) && !m.bad && m.nchk == keep, "packets 1..%d (before the failure, not held by the decoder) match the reference at offset %d (expected %s): %d of %d below 20 dB, worst %.1f dB",
                  keep, m.lag, lags_str(lag_half, 2), m.bad, m.nchk, m.worst);
        }
        check(reds_since(pos) == 1, "one [RED] line (%d)", reds_since(pos));
        double mx = 0; for (int i = k ? 19 : 0; i < r.nlat; i++) if (r.lat[i] > mx) mx = r.lat[i];
        check(mx < 5, "%s return at once: slowest %.2f ms (under 5)", k ? "the failing call and those after it" : "every call", mx);
        ares_free(&r);
    }
    check_live("the fallbacks");
    say("\nt_movie_mc: %d checks, %d failed", g_pass + g_fail, g_fail);
    say(g_fail ? "t_movie_mc: FAILED" : "t_movie_mc: all passed");
    if (g_rep) fclose(g_rep);
    return g_fail ? 1 : 0;
}
