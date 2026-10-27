// dm/android/media's decoders (the worktree's sources, unchanged) against the scripted fake MediaCodec (mock_ndk.c), on the Mac: the error paths a real decoder does not take on demand.
// Driven as hle_movie.c / t_movie_mc drive them. Checks per case: one output per unit, each with its own time, in order, and the content each should hold (the unit's value, or silence / black).
#include "movie_mc.h"
#include "hle_movie_avc.h"
#include "mock.h"
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

static int g_fail, g_pass;
static int check(int ok, const char *fmt, ...) {
    va_list v; va_start(v, fmt); printf("  %s ", ok ? "ok  " : "FAIL"); vprintf(fmt, v); printf("\n"); va_end(v);
    if (ok) g_pass++; else g_fail++; return ok;
}
static long reds_pos;
static int reds(void) {   // [RED] lines in stderr.log since mark()
    fflush(stderr); FILE *f = fopen("stderr.log", "r"); if (!f) return -1; fseek(f, reds_pos, SEEK_SET);
    char l[2048]; int n = 0; while (fgets(l, sizeof l, f)) if (strstr(l, "[RED]")) n++; fclose(f); return n;
}
static void mark(void) { fflush(stderr); FILE *f = fopen("stderr.log", "r"); if (f) { fseek(f, 0, SEEK_END); reds_pos = ftell(f); fclose(f); } }
static void reset_mk(void) { memset(&mk, 0, sizeof mk); mk.F_dec = 1024; mk.orate = 48000; mk.och = 2; mk.over_id = mk.tiny_id = mk.swap_id = mk.dup_id = mk.late_id = mk.err_at = -1; mk.vw = 64; mk.vh = 32; mk.vdepth = 2; }

// ---------- AAC ----------
#define NPK 40
typedef struct { int n; int64_t lab[4 * NPK]; int16_t v0[4 * NPK], vmid[4 * NPK], vend[4 * NPK]; int uniform[4 * NPK]; int frames_bad, label_bad; double max_ms, fail_ms; mmc_info inf; } aout;
static int64_t gpts(int id) { return 5000000 + (int64_t)id * 21333; }   // the guest's times: not the decoder's
static void g_drop(void *a) { maac_drop(a); }
static void *g_body(void *x) { void **p = x; ((void (*)(void *))p[0])(p[1]); return NULL; }
static void on_guest(void (*fn)(void *), void *arg) { void *p[2] = {(void *)fn, arg}; pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64 << 10); pthread_create(&t, &at, g_body, p); pthread_join(t, NULL); }
// feed packets [0, n) (with a Flush before drop_at, then on from resume_at), take while ready, end of stream while queued
static void run_aac(const uint8_t *asc, int an, int n, int drop_at, int resume_at, aout *o) {
    memset(o, 0, sizeof *o);
    int rate = 48000, ch = 2; maac *a = maac_new(asc, an, &rate, &ch); int F = maac_frames(asc, an);
    int16_t *buf = malloc(8192 * 8 * 2); int64_t fed[4 * NPK]; int nfed = 0, next = 0;
    #define EMIT(fr, po) do { if ((fr) != F) o->frames_bad++; if (next >= nfed || fed[next] != (po)) o->label_bad++; next++; \
        o->lab[o->n] = (po); o->v0[o->n] = buf[0]; o->vmid[o->n] = buf[(F / 2) * ch]; o->vend[o->n] = buf[(F - 1) * ch]; \
        int u = 1; for (int q = 0; q < F * ch; q++) if (buf[q] != buf[0]) { u = 0; break; } o->uniform[o->n++] = u; } while (0)
    for (int i = 0; i < n; i++) {
        if (drop_at && i == drop_at) { on_guest(g_drop, a); i = resume_at; nfed = next = 0; }
        uint8_t p[8] = {(uint8_t)i, (uint8_t)(i >> 8), 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa}; int64_t po = 0;
        fed[nfed++] = gpts(i);
        int64_t t0 = mmc_now_us(); int fr = maac_decode(a, p, 8, gpts(i), 0, buf, 8192 * 8 * 2, &po); double ms = (mmc_now_us() - t0) / 1000.0;
        if (ms > o->max_ms) o->max_ms = ms;
        mmc_info inf; maac_info(a, &inf); if (inf.failed && !o->fail_ms) o->fail_ms = ms;
        if (fr > 0) EMIT(fr, po);
        while (maac_ready(a) > 0 && (fr = maac_decode(a, NULL, 0, 0, 0, buf, 8192 * 8 * 2, &po)) > 0) EMIT(fr, po);
    }
    int64_t po; int fr;
    while (maac_queued(a) > 0 && (fr = maac_decode(a, NULL, 0, 0, 1, buf, 8192 * 8 * 2, &po)) > 0) EMIT(fr, po);
    if (next != nfed) o->label_bad += nfed - next;
    maac_info(a, &o->inf); maac_free(a); free(buf);
    #undef EMIT
}
static const uint8_t ASC_LC[] = {0x11, 0x90}, ASC_960[] = {0x11, 0x94};
// expected per output k (packet id = first + k): exp(id, &v0, &vend); returns how many outputs differ
typedef void (*aexp)(int id, int n, int16_t *v0, int16_t *vend, int *uni);
static void e_plain(int id, int n, int16_t *v0, int16_t *ve, int *u) { (void)n; *v0 = *ve = aval(id); *u = 1; }
static int a_bad(const aout *o, int first, aexp e, int n) {
    int bad = 0;
    for (int k = 0; k < o->n; k++) { int16_t v0, ve; int u; e(first + k, n, &v0, &ve, &u); if (o->v0[k] != v0 || o->vend[k] != ve || (u && !o->uniform[k])) { if (bad < 3) printf("    output %d (packet %d): %d .. %d, expected %d .. %d\n", k, first + k, o->v0[k], o->vend[k], v0, ve); bad++; } }
    return bad;
}
static int s_skip5;
static void e_skip5(int id, int n, int16_t *v0, int16_t *ve, int *u) { (void)n; *v0 = *ve = id == s_skip5 ? 0 : aval(id); *u = 1; }
static void e_short(int id, int n, int16_t *v0, int16_t *ve, int *u) { *v0 = aval(id); *ve = id + 1 < n ? aval(id + 1) : 0; *u = 0; }
static void e_over7(int id, int n, int16_t *v0, int16_t *ve, int *u) { (void)n; *v0 = *ve = id <= 7 ? aval(id) : 0; *u = 1; }
static int s_err;
static void e_err(int id, int n, int16_t *v0, int16_t *ve, int *u) { (void)n; *v0 = *ve = id <= s_err ? aval(id) : 0; *u = 1; }
static int labels_ok(const aout *o, int first, int n) { if (o->n != n - first || o->label_bad || o->frames_bad) return 0; for (int k = 0; k < o->n; k++) if (o->lab[k] != gpts(first + k)) return 0; return 1; }

static void aac_cases(void) {
    aout o;
    printf("AAC: in order, no delay\n"); reset_mk(); mark(); run_aac(ASC_LC, 2, NPK, 0, 0, &o);
    check(labels_ok(&o, 0, NPK) && !a_bad(&o, 0, e_plain, NPK) && !reds(), "%d outputs, own times, each packet's PCM; no [RED]", o.n);
    printf("AAC: held back 3 packets\n"); reset_mk(); mk.delay = 3; mark(); run_aac(ASC_LC, 2, NPK, 0, 0, &o);
    check(labels_ok(&o, 0, NPK) && !a_bad(&o, 0, e_plain, NPK) && !reds(), "%d outputs, own times, each packet's PCM (%d late, holds %d back); no [RED]", o.n, o.inf.late, o.inf.lag);
    for (int d = 0; d <= 2; d += 2) {
        printf("AAC: packet 5 passed over (no output), held back %d (round-2 #2)\n", d); reset_mk(); mk.delay = d; mk.skip[mk.nskip++] = 5; s_skip5 = 5; mark(); run_aac(ASC_LC, 2, NPK, 0, 0, &o);
        check(labels_ok(&o, 0, NPK) && !a_bad(&o, 0, e_skip5, NPK) && o.inf.skipped == 1, "%d outputs with their times; packet 5 silent, 6 on with their own PCM (passed over %d)", o.n, o.inf.skipped);
    }
    printf("AAC: packet 4's PCM after packet 5's\n"); reset_mk(); mk.swap_id = 4; s_skip5 = 4; mark(); run_aac(ASC_LC, 2, NPK, 0, 0, &o);
    check(labels_ok(&o, 0, NPK) && !a_bad(&o, 0, e_skip5, NPK) && o.inf.skipped == 1 && o.inf.stale == 1, "packet 4 silent (passed over %d), its late PCM dropped (%d), the rest their own", o.inf.skipped, o.inf.stale);
    printf("AAC: each packet's PCM in two buffers with the same time\n"); reset_mk(); mk.split = 1; mark(); run_aac(ASC_LC, 2, NPK, 0, 0, &o);
    check(labels_ok(&o, 0, NPK) && !a_bad(&o, 0, e_plain, NPK) && !o.inf.skipped && !o.inf.stale, "own times, each packet's PCM");
    printf("AAC: the first output 300 frames short (a decoder removing its delay)\n"); reset_mk(); mk.first_short = 300; mark(); run_aac(ASC_LC, 2, NPK, 0, 0, &o);
    check(labels_ok(&o, 0, NPK) && !a_bad(&o, 0, e_short, NPK) && !o.inf.skipped && o.inf.padded == 300 && !reds(), "the stream kept as it comes: no silence inside, 300 frames at the end (%d), passed over %d; no [RED]", o.inf.padded, o.inf.skipped);
    printf("AAC: output times all 0 (not kept), held back 2 (round 4: one time on every output is a surplus for packet 0's time)\n"); reset_mk(); mk.pts_mode = 1; mk.delay = 2; s_err = 0; mark(); run_aac(ASC_LC, 2, NPK, 0, 0, &o);
    check(labels_ok(&o, 0, NPK) && !a_bad(&o, 0, e_err, NPK) && o.inf.failed && o.inf.surplus == 3072 && reds() == 1,
          "packet 0 its own PCM; the next 3 outputs dropped as surplus (%d frames), then the fallback: silence after, own times; one [RED] (%d)", o.inf.surplus, reds());
    printf("AAC: packet 7 gives 4 packets' PCM in one buffer (round-2 #4; round 3: one packet's worth per time)\n"); reset_mk(); mk.over_id = 7; mk.over_mul = 4; mark(); run_aac(ASC_LC, 2, NPK, 0, 0, &o);
    check(labels_ok(&o, 0, NPK) && !a_bad(&o, 0, e_plain, NPK) && !o.inf.failed && o.inf.surplus == 3072 && o.inf.stale == 1 && !reds(),
          "every packet its own PCM, 7's surplus dropped (%d frames, expected 3072; %d output), no fallback; no [RED] (%d)", o.inf.surplus, o.inf.stale, reds());
    for (int d = 0; d <= 2; d += 2) for (int id = 0; id <= 5; id += 5) {
        printf("AAC: packet %d's PCM given twice with its time, held back %d (round-3 #2)\n", id, d); reset_mk(); mk.delay = d; mk.dup_id = id; mark(); run_aac(ASC_LC, 2, NPK, 0, 0, &o);
        check(labels_ok(&o, 0, NPK) && !a_bad(&o, 0, e_plain, NPK) && !o.inf.failed && o.inf.stale == 1 && o.inf.surplus == 1024 && !reds(),
              "%d outputs, every packet its own PCM at its own time: the repeat dropped (%d, %d frames); no [RED]", o.n, o.inf.stale, o.inf.surplus);
    }
    for (int d = 0; d <= 2; d += 2) {
        printf("AAC: 2,048 frames for every packet at the rate the decoder reports (its PCM rate is not that one), held back %d\n", d); reset_mk(); mk.F_dec = 2048; mk.delay = d; s_err = 2; mark(); run_aac(ASC_LC, 2, NPK, 0, 0, &o);
        check(labels_ok(&o, 0, NPK) && !a_bad(&o, 0, e_err, NPK) && o.inf.failed && reds() == 1, "falls back at the third output in a row with a surplus: 0..2 their own PCM, silence after, own times; one [RED] (%d)", reds());
    }
    printf("AAC: 6 packets held until the end of stream, then 2,048 frames for each (round-4 #3)\n"); reset_mk(); mk.F_dec = 2048; mk.delay = 100; s_err = 2; mark(); run_aac(ASC_LC, 2, 6, 0, 0, &o);
    check(labels_ok(&o, 0, 6) && !a_bad(&o, 0, e_err, 6) && o.inf.failed && reds() == 1, "the same fallback all at the end of stream: 0..2 their own PCM, 3..5 silence, own times; one [RED] (%d)", reds());
    for (int d = 0; d <= 2; d += 2) {
        printf("AAC: packet 0's PCM given twice, the second time in two 512-frame buffers, packet 5 passed over, held back %d (round-4 #2)\n", d);
        reset_mk(); mk.delay = d; mk.dup_id = 0; mk.dup_parts = 2; mk.skip[mk.nskip++] = 5; s_skip5 = 5; mark(); run_aac(ASC_LC, 2, NPK, 0, 0, &o);
        check(labels_ok(&o, 0, NPK) && !a_bad(&o, 0, e_skip5, NPK) && !o.inf.failed && o.inf.skipped == 1 && o.inf.stale == 2 && o.inf.surplus == 1024 && !reds(),
              "both halves dropped (%d outputs, %d frames); later packets keep their times: packet 5 silent (passed over %d), the rest their own PCM; no [RED]", o.inf.stale, o.inf.surplus, o.inf.skipped);
    }
    printf("AAC: a 2-byte piece through the other-ratio resampler (decoder at 44.1 kHz) (round-2 #5)\n"); reset_mk(); mk.orate = 44100; mk.F_dec = 941; mk.tiny_id = 0; mark(); run_aac(ASC_LC, 2, 12, 0, 0, &o);
    check(o.n == 12 && !o.label_bad && !o.inf.failed, "12 outputs with their times, no fallback (AddressSanitizer: no read outside the buffer)");
    printf("AAC: 960-sample frames (AudioSpecificConfig 11 94) (round-2 #1)\n"); reset_mk(); mk.F_dec = 960; mark(); run_aac(ASC_960, 2, NPK, 0, 0, &o);
    { int r = 0, c = 0; maac_settle(ASC_960, 2, &r, &c);
      check(maac_frames(ASC_960, 2) == 960 && maac_frames(ASC_LC, 2) == 1024 && r == 48000 && c == 2 && labels_ok(&o, 0, NPK) && !a_bad(&o, 0, e_plain, NPK), "maac_frames 960 (LC 1,024), %d Hz %d ch; %d outputs of 960 frames, own times and PCM", r, c, o.n); }
    printf("AAC: Flush (maac_drop on a guest thread) at packet 10, on from 20, held back 2\n"); reset_mk(); mk.delay = 2; mark(); run_aac(ASC_LC, 2, 30, 10, 20, &o);
    { int pre = 0; while (pre < o.n && o.lab[pre] < gpts(10)) pre++; aout post = o; memmove(post.lab, o.lab + pre, (size_t)(o.n - pre) * sizeof o.lab[0]); memmove(post.v0, o.v0 + pre, (size_t)(o.n - pre) * 2); memmove(post.vend, o.vend + pre, (size_t)(o.n - pre) * 2); memmove(post.uniform, o.uniform + pre, (size_t)(o.n - pre) * sizeof(int)); post.n = o.n - pre;
      check(post.n == 10 && post.lab[0] == gpts(20) && !a_bad(&post, 20, e_plain, 30) && mk.flushes == 1 && !reds(), "%d outputs before, then packets 20..29 with their own PCM (%d); the decoder flushed %d time(s)", pre, post.n, mk.flushes); }
    printf("AAC: a flush that takes 1.5 s (bounded at 0.5 s)\n"); reset_mk(); mk.flush_sleep_ms = 1500; mark(); run_aac(ASC_LC, 2, 30, 10, 20, &o);
    check(o.inf.failed && o.max_ms < 700 && reds() == 1, "falls back: the call waited %.0f ms (under 700); one [RED] (%d)", o.max_ms, reds());
    printf("AAC: a decoder error after packet 12, stop() taking 2 s\n"); reset_mk(); mk.err_at = 12; s_err = 12; mk.stop_sleep_ms = 2000; mark(); run_aac(ASC_LC, 2, NPK, 0, 0, &o);
    check(labels_ok(&o, 0, NPK) && !a_bad(&o, 0, e_err, NPK) && o.inf.failed && o.fail_ms < 50 && reds() == 1, "0..12 their own PCM, silence after, own times; the failing call took %.1f ms (stop on a helper thread); one [RED]", o.fail_ms);
    printf("AAC: no decoder\n"); reset_mk(); mk.create_fail = 1; mark(); run_aac(ASC_LC, 2, NPK, 0, 0, &o);
    { int silent = 1; for (int k = 0; k < o.n; k++) if (o.v0[k] || !o.uniform[k]) silent = 0; check(labels_ok(&o, 0, NPK) && silent && o.inf.failed && reds() == 1 && o.max_ms < 5, "%d silent outputs with their times; one [RED]; slowest call %.2f ms", o.n, o.max_ms); }
    // last of the AAC cases: the code it was written for read outside a buffer here (AddressSanitizer stops the run)
    printf("AAC: a mono decoder repeats packet 5 in two buffers, its format changing to stereo between them, held back 2 (round-4 #1)\n");
    reset_mk(); mk.och = 1; mk.delay = 2; mk.dup_id = 5; mk.dup_parts = 2; mk.fmt_at = 8; mk.fmt_och = 2; mark(); run_aac(ASC_LC, 2, NPK, 0, 0, &o);
    check(labels_ok(&o, 0, NPK) && !a_bad(&o, 0, e_plain, NPK) && !o.inf.failed && o.inf.och == 2 && o.inf.nfmt == 2 && o.inf.stale == 2 && o.inf.surplus == 1024 && !reds(),
          "both halves dropped (%d outputs, %d frames), every packet its own PCM at its own time, %d format(s), %d channel(s) at the end; no [RED] (AddressSanitizer: no read outside a buffer)",
          o.inf.stale, o.inf.surplus, o.inf.nfmt, o.inf.och);
}

// ---------- H.264 ----------
#define NV 31   // 1 + whole groups of 3 (P then two B): presentation indices 0..30
static int disp_of(int k) { if (!k) return 0; int g = (k - 1) / 3, r = (k - 1) % 3; return r == 0 ? 3 * g + 3 : 3 * g + r; }   // decoding order → presentation index: 0, 3, 1, 2, 6, 4, 5, ...
static int64_t vpts(int id) { return 1000 + (int64_t)id * 33333; }
typedef struct { int n; int64_t pts[2 * NV]; uint8_t y[2 * NV]; int order_bad, q[2 * NV]; mmc_info inf; } vout;   // q[k]: pictures queued after the k-th unit is fed (and taken, if one is)
static void run_avc(int n, int no_take, vout *o) {
    memset(o, 0, sizeof *o);
    mavc *a = mavc_new();
    static const uint8_t sps[] = {0, 0, 0, 1, 0x67, 0x42, 0, 0x1e, 0xab, 0xcd}, pps[] = {0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80};
    mavc_param_sets(a, sps, sizeof sps); mavc_param_sets(a, pps, sizeof pps);
    int w = mk.vw, h = mk.vh; uint8_t *dst = malloc((size_t)w * h * 3 / 2); int64_t po, last = INT64_MIN;
    #define ON() do { if (po <= last) o->order_bad++; last = po; o->pts[o->n] = po; o->y[o->n++] = dst[0]; } while (0)
    for (int k = 0; k < n; k++) {
        int id = disp_of(k); uint8_t au[] = {0, 0, 0, 1, (uint8_t)(id ? 0x41 : 0x65), (uint8_t)id, (uint8_t)(id >> 8), 0x80, 0x80};
        mavc_decode(a, au, sizeof au, vpts(id));
        if (!no_take && mavc_take(a, 0, dst, w, h, &po)) ON();
        o->q[k] = mavc_queued(a);
    }
    if (mavc_take(a, 1, dst, w, h, &po)) ON();
    while (mavc_queued(a) > 0) { if (!mavc_take(a, 1, dst, w, h, &po)) break; ON(); }
    mavc_mc_info(a, &o->inf); mavc_free(a); free(dst);
    #undef ON
}
static int v_bad(const vout *o, int n, int black_id) {   // every presentation index once, in order, Y its value (16 for black_id)
    if (o->n != n || o->order_bad) return 1;
    for (int k = 0; k < n; k++) { int id = (int)((o->pts[k] - 1000) / 33333); if (o->pts[k] != vpts(k) || o->y[k] != (id == black_id ? 16 : vval(id))) { printf("    picture %d: time %lld Y %d\n", k, (long long)o->pts[k], o->y[k]); return 1; } }
    return 0;
}
static void avc_cases(void) {
    vout o;
    for (int nt = 0; nt < 2; nt++) {
        const char *how = nt ? ", none taken until the end" : "";
        printf("H.264: reorder window 2%s\n", how); reset_mk(); mark(); run_avc(NV, nt, &o);
        check(!v_bad(&o, NV, -1) && !o.inf.skipped && !reds(), "%d pictures, every time once, in order, each its own", o.n); int q_clean[2 * NV]; memcpy(q_clean, o.q, sizeof q_clean);
        printf("H.264: access unit with presentation index 4 passed over%s (round-2 #3)\n", how); reset_mk(); mk.skip[mk.nskip++] = 4; mark(); run_avc(NV, nt, &o);
        check(!v_bad(&o, NV, 4) && o.inf.skipped == 1, "%d pictures: 4 black at its time, the rest their own (passed over %d)", o.n, o.inf.skipped);
        printf("H.264: P picture 3 passed over%s\n", how); reset_mk(); mk.skip[mk.nskip++] = 3; mark(); run_avc(NV, nt, &o);
        check(!v_bad(&o, NV, 3) && o.inf.skipped == 1, "%d pictures: 3 black at its time (passed over %d)", o.n, o.inf.skipped);
        printf("H.264: B picture 2 passed over (decoding order 0 3 1 2: fed after P picture 3)%s (round-3 #1)\n", how); reset_mk(); mk.skip[mk.nskip++] = 2; mark(); run_avc(NV, nt, &o);
        check(!v_bad(&o, NV, 2) && o.inf.skipped == 1, "%d pictures in presentation order (%d out of order): 2 black at its time, before 3 (passed over %d)", o.n, o.order_bad, o.inf.skipped);
        if (nt) {   // none taken: the queue is in time order either way, so this checks when 2's black picture is queued: with 3's picture (s3: the unit after which a clean stream has 0..3 queued)
            int s3 = 0; while (s3 < NV && q_clean[s3] < 4) s3++;
            int k = s3; while (k < NV && o.q[k] == q_clean[k]) k++;
            if (k == NV) check(s3 < NV, "2's black picture queued with 3's picture: from unit %d on (3's picture queued) as many pictures queued as on a clean stream", s3);
            else check(0, "2's black picture queued with 3's picture: from unit %d on (3's picture queued) as many pictures queued as on a clean stream (at unit %d: %d, clean %d)", s3, k, o.q[k], q_clean[k]);
        }
        printf("H.264: picture 7 comes after picture 8%s\n", how); reset_mk(); mk.late_id = 7; mark(); run_avc(NV, nt, &o);
        check(!v_bad(&o, NV, -1) && !o.inf.stale, "%d pictures, each its own: 7's black stand-in replaced by its picture (dropped %d)", o.n, o.inf.stale);
        printf("H.264: picture 10 given twice%s\n", how); reset_mk(); mk.dup_id = 10; mark(); run_avc(NV, nt, &o);
        check(!v_bad(&o, NV, -1) && o.inf.stale == 1, "%d pictures, the second 10 dropped (%d)", o.n, o.inf.stale);
    }
}

int main(void) {
    if (!freopen("stderr.log", "w", stderr)) return 2;
    setvbuf(stderr, NULL, _IOLBF, 0);
    aac_cases(); avc_cases();
    sleep(3);   // helper threads still stopping codecs (the slow flush / stop cases) finish
    check(mk_live == 0, "every fake codec deleted (%d left)", mk_live);
    { int lv[MMC_LIVE_N]; mmc_live_get(lv);
      check(!lv[MMC_LIVE_MAVC] && !lv[MMC_LIVE_MAAC] && !lv[MMC_LIVE_CODEC] && !lv[MMC_LIVE_JOB] && !lv[MMC_LIVE_THREAD], "live counters back to 0: H.264 %d, AAC %d, codecs %d, jobs %d, helper threads %d",
            lv[MMC_LIVE_MAVC], lv[MMC_LIVE_MAAC], lv[MMC_LIVE_CODEC], lv[MMC_LIVE_JOB], lv[MMC_LIVE_THREAD]); }
    printf("t_mock: %d checks, %d failed\n", g_pass + g_fail, g_fail);
    return g_fail != 0;
}
