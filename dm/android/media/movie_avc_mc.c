// H.264 (AVC) decoding for the movie decoder (hle_movie.c) on Android's MediaCodec (the phone's hardware decoder): hle_movie_avc.h, which hle_movie_avc.c implements on VideoToolbox for Apple.
// Adapted from nx2apk's dm/android/movie_avc_mc.c (github.com/ghostyshell/nx2apk; MIT, Copyright (c) 2026 nx2apk contributors; licence text in dm/third_party/nx2apk/LICENSE).
// The SDK's demuxer hands over one access unit per packet with Annex B start codes and SPS / PPS in Configure's csd-0 / csd-1, MediaCodec's own input convention (an MP4 demuxer gives the avcC box,
// and units with 4-byte lengths, which go in as Annex B). Synchronous use, byte-buffer output (no surface): mavc_decode queues the access unit and copies the pictures that are ready, cropped, into
// a queue sorted by presentation time; MediaCodec gives them in presentation order, so none is held back. mavc_take converts the oldest into the caller's NV12 or RGBA frame. With flush, the first
// call sends end of stream and collects every picture left in the decoder, so mavc_queued counts all that remain (hle_movie.c's end-of-stream drain takes that many).
// Threads: hle_movie.c calls mavc_new / param_sets / avcc / set_rgba / drop / queued on guest threads (guest stacks) and decode / take / free on the decoder's host thread, under its decode lock.
// Only the latter call MediaCodec: a drop (Flush) frees the queue and leaves the codec's flush to the next decode or take, which also sends the parameter sets in band again.
// The output layout is read only where it can be proven: the format's "image-data" (MediaImage2: each plane's offset and steps) describing 8-bit 4:2:0 planar or semi-planar planes, else the
// standard planar / semi-planar colour formats (19, 20, 21, 39) with the stride and slice height (Qualcomm pads the stride to 128 and the slice height to 32); then the crop rectangle. A vendor or
// flexible format without such image-data, or a layout that does not fit the buffer, gives black pictures with one [RED] naming what the decoder sent.
// Bounded waits: the codec calls that wait on its process (create / configure / start, flush, stop / delete) run on a helper thread with a deadline (movie_mc.c); a failed decoder is given to
// that thread to stop and delete, so the fallback is immediate. At most NFLY access units are in the decoder at once, so each has its time kept for a black picture should the decoder fail.
// Fallback: when no decoder can be created, configured or fed, or it fails mid-stream, each access unit gives a black picture at its time (those in the decoder too), with one [RED], as the port
// did before: the movie's clock keeps running. DM_MOVIE_MC_FAIL=avc (creation) or avc@N (the N-th access unit) forces it (dm/android/tests/t_movie_mc.c)
#include "../../host/dm.h"
#include "../../host/hle_movie_avc.h"
#include "movie_mc.h"
#include <arm_neon.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#define NFLY 64                    // access units in the decoder at most (the phone's decoder takes 10 inputs and holds about 12 pictures)
#define IN_WAIT_US 10000           // one wait for a free input buffer; outputs are collected between waits
#define IN_WAIT_MAX_US 200000      // then the decoder counts as stuck
#define EOS_WAIT_MAX_US 1000000    // end of stream: the last pictures within 1 s
#define OPEN_WAIT_US 2000000       // create + configure + start (about 40 ms seen, 80 ms the first in a process)
#define FLUSH_WAIT_US 500000       // a flush (a seek)
#define CLOSE_WAIT_US 500000       // stop + delete of a decoder that worked; a failed one is not waited for
#define AVC_MAX_INPUT (2u << 20)   // input buffer size asked for (a 1080p access unit is far smaller)
enum { CF_I420 = 19, CF_I420P = 20, CF_NV12 = 21, CF_NV12P = 39 };

typedef struct { uint8_t *nv12; int w, h; int64_t pts; int16_t matrix; uint8_t full; } pic;   // NV12, both planes with stride (w + 1) & ~1; nv12 NULL: a black picture; its format's colour
struct mavc {
    uint8_t sps[512], pps[512]; int nsps, npps;
    int ps_new, ps_sent;         // ps_new: the stored parameter sets go in band before the next access unit (they changed after the codec was made, or it was flushed); ps_sent: times they did
    int rgba, depth, nau, red, red_ps, red_lay, failed, fail_at;
    AMediaCodec *mc; char name[64]; double open_ms;
    int flush_due, eos_sent;
    int nfmt, cfmt, w, h, stride, slice, crop[4], matrix, full, image_data, lay_ok, skipped, stale;
    uint32_t idm[26]; int has_id;                      // the last "image-data" as sent (for the [RED] line when it is not read)
    struct { int64_t off; int col, row; } pl[3];       // Y, U, V: first sample's byte offset, column and row steps
    pic *q; int nq, qcap;                              // pictures waiting to be taken, by presentation time (grows: every access unit in the decoder can come out at the end of stream)
    int64_t fly[NFLY]; int nfly;
    uint8_t *au; size_t aucap;
    uint8_t *spare[4]; size_t sparecap[4]; int nspare;   // picture buffers kept for reuse
    int64_t nin, nout;
};

mavc *mavc_new(void) { mavc *a = calloc(1, sizeof *a); if (!a) die("movie: AVC decoder allocation failed"); mmc_live_add(MMC_LIVE_MAVC, 1); a->depth = 4; a->fail_at = -2; return a; }
int mavc_queued(const mavc *a) { return a->nq; }
void mavc_set_rgba(mavc *a) { a->rgba = 1; }

static uint8_t *pic_alloc(mavc *a, size_t n) {
    for (int i = 0; i < a->nspare; i++) if (a->sparecap[i] >= n) { uint8_t *p = a->spare[i]; a->spare[i] = a->spare[--a->nspare]; a->sparecap[i] = a->sparecap[a->nspare]; return p; }
    uint8_t *p = malloc(n); if (!p) die("movie: AVC picture allocation failed"); return p;
}
static void pic_release(mavc *a, pic *p) {
    if (!p->nv12) return;
    size_t n = (size_t)((p->w + 1) & ~1) * (p->h + (p->h + 1) / 2);
    if (a->nspare < 4) { a->spare[a->nspare] = p->nv12; a->sparecap[a->nspare++] = n; } else free(p->nv12);
    p->nv12 = NULL;
}
static void enqueue(mavc *a, uint8_t *nv12, int w, int h, int64_t pts) {
    if (a->nq == a->qcap) { a->qcap = a->qcap ? a->qcap * 2 : 16; a->q = realloc(a->q, (size_t)a->qcap * sizeof *a->q); if (!a->q) die("movie: AVC picture queue allocation failed"); }
    int k = a->nq++; while (k > 0 && a->q[k - 1].pts > pts) { a->q[k] = a->q[k - 1]; k--; }
    a->q[k] = (pic){nv12, w, h, pts, (int16_t)a->matrix, (uint8_t)a->full};
}
static void fly_add(mavc *a, int64_t pts) { if (a->nfly < NFLY) a->fly[a->nfly++] = pts; }   // mavc_decode keeps fewer than NFLY in the decoder
// a picture at pts came out (nv12 NULL: one that cannot be read, black): its access unit leaves the decoder. The decoder gives presentation order, so a unit still in it with an earlier time,
// whether fed before this one or after it (a B-frame fed after the P-frame it precedes), will not come before it: it passed over that unit (a damaged one it skipped), which gets a black picture
// at its time now, ahead of this one in the queue (mavc_take hands out the queue's head at once, so a black picture made later would come out of order). On a clean stream no unit with an
// earlier time is still in the decoder when a picture comes, so nothing changes there.
// A picture whose time is not in the decoder (passed over before, or never fed) replaces its black picture if that is still queued, else it is dropped: one picture per access unit
static void pic_out(mavc *a, uint8_t *nv12, int w, int h, int64_t pts) {
    int at = -1; for (int i = 0; i < a->nfly; i++) if (a->fly[i] == pts) { at = i; break; }
    if (at < 0) {
        for (int k = 0; k < a->nq; k++) if (a->q[k].pts == pts && !a->q[k].nv12 && nv12) { a->q[k] = (pic){nv12, w, h, pts, (int16_t)a->matrix, (uint8_t)a->full}; return; }
        if (!a->stale++) LOG("[movie] AVC (%s): a picture at %lld µs, no access unit's in the decoder: dropped (first case)", a->name, (long long)pts);
        pic tmp = {nv12, w, h, pts, 0, 0}; pic_release(a, &tmp);
        return;
    }
    int n = 0, gone = 0;
    for (int i = 0; i < a->nfly; i++) {
        if (i == at) continue;
        if (a->fly[i] < pts) { enqueue(a, NULL, 0, 0, a->fly[i]); gone++; } else a->fly[n++] = a->fly[i];
    }
    a->nfly = n;
    if (gone && !a->skipped) LOG("[movie] AVC (%s): the decoder passed over %d access unit(s) before the picture at %lld µs: black pictures at their times (first case)", a->name, gone, (long long)pts);
    a->skipped += gone;
    enqueue(a, nv12, w, h, pts);
}
// the access units in the decoder come out black at their times
static void fly_black(mavc *a) { for (int i = 0; i < a->nfly; i++) enqueue(a, NULL, 0, 0, a->fly[i]); a->nfly = 0; }

static void close_codec(mavc *a, int64_t wait_us) { if (a->mc) { mmc_close(a->mc, wait_us); a->mc = NULL; } a->eos_sent = a->flush_due = 0; }
// a failed decoder goes to a helper thread to be stopped and deleted (movie_mc.c): stop() on a decoder that stopped answering took 3 s
static void fail(mavc *a, const char *why, int st) {
    if (!a->red++) LOG("[RED] movie: %s (%d): H.264 falls back to black pictures (%s), the movie keeps its clock", why, st, a->name[0] ? a->name : "no decoder");
    a->failed = 1; fly_black(a); close_codec(a, 0);
}
void mavc_drop(mavc *a) { for (int i = 0; i < a->nq; i++) pic_release(a, &a->q[i]); a->nq = 0; a->nfly = 0; if (a->mc) a->flush_due = 1; }
void mavc_free(mavc *a) {
    if (!a) return;
    close_codec(a, CLOSE_WAIT_US); mavc_drop(a);
    for (int i = 0; i < a->nspare; i++) free(a->spare[i]);
    free(a->q); free(a->au); free(a); mmc_live_add(MMC_LIVE_MAVC, -1);
}
void mavc_mc_info(const mavc *a, mmc_info *o) {
    memset(o, 0, sizeof *o); snprintf(o->codec, sizeof o->codec, "%s", a->name); o->failed = a->failed; o->open_ms = a->open_ms;
    o->nfmt = a->nfmt; o->cfmt = a->cfmt; o->w = a->w; o->h = a->h; o->stride = a->stride; o->slice = a->slice; memcpy(o->crop, a->crop, sizeof o->crop);
    o->matrix = a->matrix; o->full = a->full; o->image_data = a->image_data; o->ps_sent = a->ps_sent; o->nin = a->nin; o->nout = a->nout; o->skipped = a->skipped; o->stale = a->stale;
}

// ---------- parameter sets ----------
static void keep_ps(mavc *a, const uint8_t *nal, size_t n) {   // a parameter set NAL unit (without start code)
    int t = nal[0] & 31; uint8_t *dst = t == 7 ? a->sps : a->pps; int *dn = t == 7 ? &a->nsps : &a->npps;
    if (n > sizeof a->sps) { if (!a->red_ps++) LOG("[RED] movie: AVC parameter set of %zu bytes, too long", n); return; }
    if ((size_t)*dn == n && !memcmp(dst, nal, n)) return;
    memcpy(dst, nal, n); *dn = (int)n; if (a->mc) a->ps_new = 1;
    if (t == 7 && n > 1) a->depth = nal[1] == 66 ? 0 : 4;   // profile_idc: Baseline has no B-frames (the black pictures of the fallback wait for this many)
}
// NAL units of p (Annex B start codes, or else 4-byte lengths), each without its prefix (as hle_movie_avc.c)
static size_t next_sc(const uint8_t *p, size_t n, size_t i) { for (; i + 3 <= n; i++) if (!p[i] && !p[i + 1] && p[i + 2] == 1) return i; return n; }
static void each_nal(const uint8_t *p, size_t n, void (*fn)(void *, const uint8_t *, size_t), void *ctx) {
    size_t s = next_sc(p, n, 0);
    if (s < n && s <= 1) {
        while (s < n) {
            size_t b = s + 3, e = next_sc(p, n, b), end = e;
            while (end > b && !p[end - 1]) end--;
            if (end > b) fn(ctx, p + b, end - b);
            s = e;
        }
    } else for (size_t i = 0; i + 4 <= n;) {
        size_t len = (size_t)p[i] << 24 | (size_t)p[i + 1] << 16 | (size_t)p[i + 2] << 8 | p[i + 3]; i += 4;
        if (!len || len > n - i) break;
        fn(ctx, p + i, len); i += len;
    }
}
static void ps_only(void *ctx, const uint8_t *nal, size_t n) { int t = nal[0] & 31; if (t == 7 || t == 8) keep_ps(ctx, nal, n); }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { each_nal(p, n, ps_only, a); }
// AVCDecoderConfigurationRecord: version, profile, compatibility, level, NAL length size - 1, SPS count (low 5 bits) and 16-bit-length SPS, PPS count and 16-bit-length PPS
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) {
    if (n < 7 || p[0] != 1) { if (!a->red_ps++) LOG("[RED] movie: unrecognized avcC record (%zu bytes, version %d)", n, n ? p[0] : -1); return; }
    size_t i = 5;
    for (int set = 0; set < 2 && i < n; set++) {
        int cnt = set ? p[i] : p[i] & 31; i++;
        for (int k = 0; k < cnt && i + 2 <= n; k++) { size_t len = (size_t)p[i] << 8 | p[i + 1]; i += 2; if (len > n - i) return; if (len) keep_ps(a, p + i, len); i += len; }
    }
}
// The picture size from the SPS (H.264 7.3.2.1.1), to configure the decoder with: 0 if it cannot be read
typedef struct { uint8_t b[512]; int n, bit; } rbsp;
static unsigned rb_u(rbsp *r, int k) { unsigned v = 0; while (k--) { if (r->bit >> 3 >= r->n) { r->bit = r->n * 8 + 1; return 0; } v = v << 1 | (r->b[r->bit >> 3] >> (7 - (r->bit & 7)) & 1); r->bit++; } return v; }
static unsigned rb_ue(rbsp *r) { int z = 0; while (!rb_u(r, 1)) { if (++z > 31) return 0; } return z ? (1u << z) - 1 + rb_u(r, z) : 0; }
static int rb_se(rbsp *r) { unsigned v = rb_ue(r); return v & 1 ? (int)((v + 1) / 2) : -(int)(v / 2); }
static int sps_size(const uint8_t *s, int n, int *w, int *h) {
    rbsp r = {.n = 0};
    for (int i = 1, z = 0; i < n && r.n < (int)sizeof r.b; i++) { if (z >= 2 && s[i] == 3) { z = 0; continue; } z = s[i] ? 0 : z + 1; r.b[r.n++] = s[i]; }   // without the emulation prevention bytes
    unsigned prof = rb_u(&r, 8); rb_u(&r, 16); rb_ue(&r);
    unsigned cf = 1;
    if (prof == 100 || prof == 110 || prof == 122 || prof == 244 || prof == 44 || prof == 83 || prof == 86 || prof == 118 || prof == 128 || prof == 138 || prof == 139 || prof == 134 || prof == 135) {
        cf = rb_ue(&r); if (cf == 3) rb_u(&r, 1);
        rb_ue(&r); rb_ue(&r); rb_u(&r, 1);
        if (rb_u(&r, 1)) for (int i = 0; i < (cf != 3 ? 8 : 12); i++) if (rb_u(&r, 1)) {   // scaling lists
            int last = 8, next = 8; for (int j = 0; j < (i < 6 ? 16 : 64) && next; j++) { next = (last + rb_se(&r) + 256) % 256; last = next ? next : last; }
        }
    }
    rb_ue(&r); unsigned poc = rb_ue(&r);
    if (poc == 0) rb_ue(&r);
    else if (poc == 1) { rb_u(&r, 1); rb_se(&r); rb_se(&r); unsigned k = rb_ue(&r); for (unsigned i = 0; i < k && i < 256; i++) rb_se(&r); }
    rb_ue(&r); rb_u(&r, 1);
    unsigned mw = rb_ue(&r) + 1, mh = rb_ue(&r) + 1, fmo = rb_u(&r, 1);
    if (!fmo) rb_u(&r, 1);
    rb_u(&r, 1);
    unsigned cl = 0, cr = 0, ct = 0, cb = 0; if (rb_u(&r, 1)) { cl = rb_ue(&r); cr = rb_ue(&r); ct = rb_ue(&r); cb = rb_ue(&r); }
    if (r.bit > r.n * 8) return 0;
    unsigned ux = cf == 1 || cf == 2 ? 2 : 1, uy = (cf == 1 ? 2 : 1) * (2 - fmo);
    long ww = (long)mw * 16 - ux * (cl + cr), hh = (long)mh * 16 * (2 - fmo) - uy * (ct + cb);
    if (ww <= 0 || hh <= 0 || ww > 8192 || hh > 8192) return 0;
    *w = (int)ww; *h = (int)hh; return 1;
}

// ---------- the codec ----------
static int open_codec(mavc *a) {
    if (!a->nsps || !a->npps) { if (!a->red_ps++) LOG("[RED] movie: AVC has no SPS / PPS yet, black pictures until they come"); return 0; }
    if (a->fail_at == -2) a->fail_at = mmc_fail_at("avc");
    if (a->fail_at == 0) { fail(a, "MediaCodec H.264 decoder creation failed (forced, DM_MOVIE_MC_FAIL)", 0); return 0; }
    mmc_binder_threads();
    int w = 1920, h = 1088, sized = sps_size(a->sps, a->nsps, &w, &h);
    uint8_t csd0[516] = {0, 0, 0, 1}, csd1[516] = {0, 0, 0, 1};
    memcpy(csd0 + 4, a->sps, (size_t)a->nsps); memcpy(csd1 + 4, a->pps, (size_t)a->npps);
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, w); AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, h);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, CF_NV12);   // asked for; the decoder may still choose its own (out_format)
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, (int32_t)AVC_MAX_INPUT);
    AMediaFormat_setBuffer(f, AMEDIAFORMAT_KEY_CSD_0, csd0, (size_t)a->nsps + 4); AMediaFormat_setBuffer(f, AMEDIAFORMAT_KEY_CSD_1, csd1, (size_t)a->npps + 4);
    int st = 0; int64_t t0 = mmc_now_us();
    a->mc = mmc_open("video/avc", f, OPEN_WAIT_US, &st, a->name, sizeof a->name);
    a->open_ms = (mmc_now_us() - t0) / 1000.0;
    if (!a->mc) { fail(a, st == MMC_TIMEOUT ? "MediaCodec H.264 decoder not made within 2 s" : st == AMEDIA_ERROR_UNSUPPORTED && !a->name[0] ? "no MediaCodec H.264 decoder" : "MediaCodec H.264 configure / start failed", st); return 0; }
    LOG("[movie] AVC: MediaCodec %s decoding %d×%d%s, profile %d (opened in %.1f ms)", a->name, w, h, sized ? "" : " (SPS unread, a guess)", a->nsps > 1 ? a->sps[1] : 0, a->open_ms);
    a->ps_new = 0; a->nfmt = 0;
    return 1;
}
// the output format: plane layout, crop, colour; logged once per decoder and on each later change of size or layout. lay_ok: the layout is one this file can read (the header comment)
static void out_format(mavc *a) {
    AMediaFormat *f = AMediaCodec_getOutputFormat(a->mc); if (!f) return;
    int32_t v, w = 0, h = 0, l, t, r, b;
    if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &v)) w = v;
    if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &v)) h = v;
    int stride = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_STRIDE, &v) && v > 0 ? v : w, slice = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &v) && v > 0 ? v : h;
    int cfmt = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, &v) ? v : 0;
    int crop[4] = {0, 0, w, h};
    if (AMediaFormat_getRect(f, AMEDIAFORMAT_KEY_DISPLAY_CROP, &l, &t, &r, &b) && r >= l && b >= t && l >= 0 && t >= 0) { crop[0] = l; crop[1] = t; crop[2] = r - l + 1; crop[3] = b - t + 1; }
    int std = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_STANDARD, &v) ? v : 0, range = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_RANGE, &v) ? v : 0;
    // RGBA: the matrix the stream declares (MediaFormat COLOR_STANDARD_BT709 1, BT601_PAL 2, BT601_NTSC 4, BT2020 6), when undeclared BT.709 from 720 lines up, else BT.601 (as hle_movie.c md_rgba); video range unless full (1)
    a->matrix = std == 1 ? 709 : std == 2 || std == 4 ? 601 : std == 6 ? 2020 : crop[3] >= 720 ? 709 : 601; a->full = range == 1;
    // plane layout: MediaImage2 (media/hardware/VideoAPI.h: type, planes, width, height, bit depth, allocated bit depth, then per plane offset, column step, row step, subsampling x / y);
    // read when it is 8-bit YUV 4:2:0 with luma samples side by side and chroma either planar (two planes, step 1) or semi-planar (step 2, one plane's samples beside the other's)
    void *id = NULL; size_t idn = 0; int img = 0, ok = 0;
    a->has_id = AMediaFormat_getBuffer(f, "image-data", &id, &idn) && id && idn >= sizeof a->idm;
    if (a->has_id) {
        uint32_t *m = a->idm; memcpy(m, id, sizeof a->idm); img = 1;
        #define P(k, i) m[6 + 5 * (k) + (i)]
        int planar = (int32_t)P(1, 1) == 1 && (int32_t)P(2, 1) == 1, semi = (int32_t)P(1, 1) == 2 && (int32_t)P(2, 1) == 2 && P(1, 2) == P(2, 2) && (P(2, 0) == P(1, 0) + 1 || P(1, 0) == P(2, 0) + 1);
        ok = m[0] == 1 && m[1] == 3 && m[4] == 8 && m[5] == 8 && (int32_t)P(0, 1) == 1 && P(0, 3) == 1 && P(0, 4) == 1 && P(1, 3) == 2 && P(1, 4) == 2 && P(2, 3) == 2 && P(2, 4) == 2 && (planar || semi);
        if (ok) for (int k = 0; k < 3; k++) { a->pl[k].off = P(k, 0); a->pl[k].col = (int32_t)P(k, 1); a->pl[k].row = (int32_t)P(k, 2); }
        #undef P
    } else {
        int64_t c = (int64_t)stride * slice;
        if (cfmt == CF_I420 || cfmt == CF_I420P) { ok = 1; a->pl[1].off = c; a->pl[1].col = 1; a->pl[1].row = stride / 2; a->pl[2].off = c + (int64_t)(stride / 2) * ((slice + 1) / 2); a->pl[2].col = 1; a->pl[2].row = stride / 2; }
        else if (cfmt == CF_NV12 || cfmt == CF_NV12P) { ok = 1; a->pl[1].off = c; a->pl[1].col = 2; a->pl[1].row = stride; a->pl[2].off = c + 1; a->pl[2].col = 2; a->pl[2].row = stride; }
        a->pl[0].off = 0; a->pl[0].col = 1; a->pl[0].row = stride;
    }
    int changed = !a->nfmt || w != a->w || h != a->h || stride != a->stride || slice != a->slice || cfmt != a->cfmt || memcmp(crop, a->crop, sizeof crop) || ok != a->lay_ok;
    a->w = w; a->h = h; a->stride = stride; a->slice = slice; a->cfmt = cfmt; memcpy(a->crop, crop, sizeof crop); a->image_data = img; a->lay_ok = ok; a->nfmt++;
    if (changed) LOG("[movie] AVC output (%s): %d×%d, colour format %#x, stride %d, slice height %d, crop %d,%d %d×%d, BT.%d %s range; planes %s Y %lld+%d/%d U %lld+%d/%d V %lld+%d/%d",
                     a->name, w, h, cfmt, stride, slice, crop[0], crop[1], crop[2], crop[3], a->matrix, a->full ? "full" : "video",
                     !ok ? "(not readable)" : img ? "(image-data)" : "(from the colour format)",
                     (long long)a->pl[0].off, a->pl[0].col, a->pl[0].row, (long long)a->pl[1].off, a->pl[1].col, a->pl[1].row, (long long)a->pl[2].off, a->pl[2].col, a->pl[2].row);
    AMediaFormat_delete(f);
}
// last byte (exclusive) a plane's crop window reads: from the plane's first sample, x0 / y0 the window's corner, cols × rows its size
static int64_t plane_end(int64_t off, int col, int row, int x0, int y0, int cols, int rows) { return off + (int64_t)(y0 + rows - 1) * row + (int64_t)(x0 + cols - 1) * col + 1; }
// one decoded picture → a cropped NV12 copy; NULL (a black picture) when the layout is not readable or does not fit the buffer
static uint8_t *copy_pic(mavc *a, const uint8_t *buf, size_t len, int *pw, int *ph) {
    int cx = a->crop[0], cy = a->crop[1], cw = a->crop[2], ch = a->crop[3], sw = (cw + 1) & ~1, cw2 = (cw + 1) / 2, ch2 = (ch + 1) / 2;
    *pw = cw; *ph = ch;
    if (cw <= 0 || ch <= 0) return NULL;
    if (!a->lay_ok) {
        const uint32_t *m = a->idm;
        if (!a->red_lay++) {
            if (a->has_id) LOG("[RED] movie: H.264 decoder %s gives colour format %#x with image-data not read here (type %u, %u planes, %u×%u, %u-bit in %u; Y %u+%d/%d %u×%u U %u+%d/%d %u×%u "
                               "V %u+%d/%d %u×%u): black pictures in its place", a->name, a->cfmt, m[0], m[1], m[2], m[3], m[4], m[5], m[6], (int)m[7], (int)m[8], m[9], m[10],
                               m[11], (int)m[12], (int)m[13], m[14], m[15], m[16], (int)m[17], (int)m[18], m[19], m[20]);
            else LOG("[RED] movie: H.264 decoder %s gives colour format %#x without image-data (stride %d, slice height %d): not a layout read here, black pictures in its place", a->name, a->cfmt, a->stride, a->slice);
        }
        return NULL;
    }
    for (int k = 0; k < 3; k++) {
        int64_t e = k ? plane_end(a->pl[k].off, a->pl[k].col, a->pl[k].row, cx / 2, cy / 2, cw2, ch2) : plane_end(a->pl[0].off, 1, a->pl[0].row, cx, cy, cw, ch);
        if (a->pl[k].col <= 0 || a->pl[k].row <= 0 || a->pl[k].off < 0 || e > (int64_t)len) {
            if (!a->red_lay++) LOG("[RED] movie: H.264 picture layout (plane %d ends at %lld) does not fit the %zu-byte output buffer: black pictures in its place", k, (long long)e, len);
            return NULL;
        }
    }
    uint8_t *d = pic_alloc(a, (size_t)sw * (ch + ch2)), *duv = d + (size_t)sw * ch;
    const uint8_t *y = buf + a->pl[0].off + (int64_t)cy * a->pl[0].row + cx;
    for (int r = 0; r < ch; r++) memcpy(d + (size_t)r * sw, y + (int64_t)r * a->pl[0].row, (size_t)cw);
    const uint8_t *u = buf + a->pl[1].off + (int64_t)(cy / 2) * a->pl[1].row + (int64_t)(cx / 2) * a->pl[1].col, *v = buf + a->pl[2].off + (int64_t)(cy / 2) * a->pl[2].row + (int64_t)(cx / 2) * a->pl[2].col;
    int nv12 = a->pl[1].col == 2 && a->pl[2].col == 2 && v == u + 1 && a->pl[1].row == a->pl[2].row, i420 = a->pl[1].col == 1 && a->pl[2].col == 1;
    for (int r = 0; r < ch2; r++) {
        const uint8_t *ur = u + (int64_t)r * a->pl[1].row, *vr = v + (int64_t)r * a->pl[2].row; uint8_t *o = duv + (size_t)r * sw;
        if (nv12) { memcpy(o, ur, (size_t)cw2 * 2); continue; }
        int x = 0;
        if (i420) for (; x + 16 <= cw2; x += 16) { uint8x16x2_t t = {{vld1q_u8(ur + x), vld1q_u8(vr + x)}}; vst2q_u8(o + 2 * x, t); }
        for (; x < cw2; x++) { o[2 * x] = ur[(int64_t)x * a->pl[1].col]; o[2 * x + 1] = vr[(int64_t)x * a->pl[2].col]; }
    }
    return d;
}
// collect the pictures the decoder has ready, waiting up to wait_us for the first: 1 = the end-of-stream buffer came, 0 = no more for now, -1 = a decoder error (its status in *err)
static int drain(mavc *a, int64_t wait_us, int *err) {
    for (;;) {
        AMediaCodecBufferInfo bi;
        ssize_t i = AMediaCodec_dequeueOutputBuffer(a->mc, &bi, wait_us);
        if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { out_format(a); continue; }
        if (i == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (i == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return 0;
        if (i < 0) { *err = (int)i; return -1; }
        wait_us = 0;
        if (!a->nfmt) out_format(a);
        size_t cap = 0; uint8_t *b = AMediaCodec_getOutputBuffer(a->mc, (size_t)i, &cap);
        int end = (bi.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        if (bi.size > 0 && bi.offset >= 0 && (size_t)bi.offset <= cap) {
            int pw = 0, ph = 0; uint8_t *d = b ? copy_pic(a, b + bi.offset, cap - (size_t)bi.offset, &pw, &ph) : NULL;
            pic_out(a, d, pw, ph, bi.presentationTimeUs); a->nout++;
        }
        AMediaCodec_releaseOutputBuffer(a->mc, (size_t)i, false);
        if (end) return 1;
    }
}
// queue one input buffer (an access unit, or none with the end-of-stream flag), collecting pictures while the decoder has no free input: 0, or -1 with the status in *err
static int feed(mavc *a, const uint8_t *p, size_t n, int64_t pts, uint32_t flags, int *err) {
    for (int64_t t0 = mmc_now_us();;) {
        ssize_t i = AMediaCodec_dequeueInputBuffer(a->mc, IN_WAIT_US);
        if (i >= 0) {
            size_t cap = 0; uint8_t *b = AMediaCodec_getInputBuffer(a->mc, (size_t)i, &cap);
            if (!b || cap < n) { AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, 0, (uint64_t)pts, 0); *err = (int)cap; return -1; }
            if (n) memcpy(b, p, n);
            media_status_t st = AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, n, (uint64_t)pts, flags);
            if (st != AMEDIA_OK) { *err = st; return -1; }
            return 0;
        }
        if (i != AMEDIACODEC_INFO_TRY_AGAIN_LATER) { *err = (int)i; return -1; }
        if (drain(a, 0, err) < 0) return -1;
        if (mmc_now_us() - t0 > IN_WAIT_MAX_US) { *err = 0; return -1; }
    }
}
// a drop (Flush) since the last call: the codec forgets what it holds (bounded: a flush that does not return in time leaves the codec to the helper thread, and the decoder falls back);
// the parameter sets go in band again (MediaCodec may need them after a flush)
static void sync_flush(mavc *a) {
    if (!a->flush_due || !a->mc) return;
    a->flush_due = 0; int st = mmc_flush(a->mc, FLUSH_WAIT_US);
    if (st == MMC_TIMEOUT) { a->mc = NULL; fail(a, "MediaCodec H.264 flush did not return within 0.5 s", 0); return; }
    if (st != AMEDIA_OK) { fail(a, "MediaCodec H.264 flush failed", st); return; }
    a->eos_sent = 0; a->ps_new = 1;
}
// end of stream: every picture still in the decoder comes out (within EOS_WAIT_MAX_US); those it never gives are black at their times, so each access unit has its picture
static void end_stream(mavc *a) {
    int err = 0, r = 0; a->eos_sent = 1;
    if (feed(a, NULL, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM, &err) < 0) { fail(a, "MediaCodec H.264 takes no end of stream", err); return; }
    for (int64_t t0 = mmc_now_us(); r == 0 && mmc_now_us() - t0 < EOS_WAIT_MAX_US;) r = drain(a, 10000, &err);
    if (r < 0) { fail(a, "MediaCodec H.264 error at the end of stream", err); return; }
    if (r == 0) { fail(a, "MediaCodec H.264 end of stream not reached within 1 s", 0); return; }
    if (a->nfly) { if (!a->red++) LOG("[RED] movie: the H.264 decoder gave no picture for %d access units: black in their place", a->nfly); fly_black(a); }
}

typedef struct { mavc *a; size_t len; int vcl; } au_ctx;
static void au_put(mavc *a, size_t *len, const uint8_t *nal, size_t n) {
    if (*len + 4 + n > a->aucap) { a->aucap = (*len + 4 + n) * 2; a->au = realloc(a->au, a->aucap); if (!a->au) die("movie: AVC buffer allocation failed"); }
    uint8_t *o = a->au + *len; o[0] = 0; o[1] = 0; o[2] = 0; o[3] = 1; memcpy(o + 4, nal, n); *len += 4 + n;
}
// the access unit as Annex B with 4-byte start codes; parameter sets in the stream go along (the decoder takes them in band) and are kept for a later decoder
static void au_add(void *ctx, const uint8_t *nal, size_t n) {
    au_ctx *c = ctx; int t = nal[0] & 31;
    if (t == 9) return;   // access unit delimiter
    if (t == 7 || t == 8) { int had = c->a->ps_new; keep_ps(c->a, nal, n); c->a->ps_new = had; }   // in band already
    if (t >= 1 && t <= 5) c->vcl++;
    au_put(c->a, &c->len, nal, n);
}
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) {
    if (n <= 0) return 0;
    // a Flush since the last call, or input after the end of stream without one: the codec starts afresh before this unit is put together, so the parameter sets go in with it
    if (!a->failed && a->mc) { if (a->eos_sent) a->flush_due = 1; sync_flush(a); }
    au_ctx c = {a, 0, 0}; int ps = !a->failed && a->mc && a->ps_new && a->nsps && a->npps;
    if (ps) { au_put(a, &c.len, a->sps, (size_t)a->nsps); au_put(a, &c.len, a->pps, (size_t)a->npps); }
    size_t pre = c.len; each_nal(p, (size_t)n, au_add, &c);
    if (!c.vcl) { if (c.len > pre) a->ps_new = a->mc != NULL; return 0; }   // parameter sets / SEI only: no picture; the sets go in with the next one
    a->nau++;
    if (a->fail_at == -2) a->fail_at = mmc_fail_at("avc");
    if (!a->failed && a->fail_at > 0 && a->nau >= a->fail_at) fail(a, "MediaCodec H.264 decoder failure (forced, DM_MOVIE_MC_FAIL)", a->nau);
    if (a->failed || (!a->mc && !open_codec(a))) { enqueue(a, NULL, 0, 0, pts_us); return -1; }
    int err = 0;
    if (a->nfly >= NFLY) {   // NFLY units in the decoder and no picture: wait for one (bounded), else it counts as stuck
        for (int64_t t0 = mmc_now_us(); a->nfly >= NFLY && mmc_now_us() - t0 < IN_WAIT_MAX_US;) if (drain(a, IN_WAIT_US, &err) < 0) { fail(a, "MediaCodec H.264 decoder error", err); break; }
        if (!a->failed && a->nfly >= NFLY) fail(a, "MediaCodec H.264 decoder holds 64 access units without a picture", 0);
        if (a->failed) { enqueue(a, NULL, 0, 0, pts_us); return -1; }
    }
    if (feed(a, a->au, c.len, pts_us, 0, &err) < 0) { fly_add(a, pts_us); fail(a, "MediaCodec H.264 takes no input", err); return -1; }
    if (ps) { a->ps_new = 0; a->ps_sent++; }
    a->nin++; fly_add(a, pts_us);
    if (drain(a, 0, &err) < 0) { fail(a, "MediaCodec H.264 decoder error", err); return -1; }
    return 0;
}

// ---------- pictures out ----------
// NV12 → RGBA, by the matrix and range of the picture's format, 16-bit fixed point (as hle_movie.c md_rgba); 16 pixels at a time
typedef struct { int y, rv, gu, gv, bu, y0; } yuvk;
static yuvk yuv_k(int matrix, int full) {
    double kr = matrix == 709 ? 0.2126 : matrix == 2020 ? 0.2627 : 0.299, kb = matrix == 709 ? 0.0722 : matrix == 2020 ? 0.0593 : 0.114, kg = 1 - kr - kb;
    double ky = full ? 1.0 : 255.0 / 219.0, kc = full ? 1.0 : 255.0 / 224.0;
    double rv = 2 * (1 - kr), bu = 2 * (1 - kb), gu = bu * kb / kg, gv = rv * kr / kg;
    return (yuvk){(int)(ky * 65536 + 0.5), (int)(rv * kc * 65536 + 0.5), (int)(gu * kc * 65536 + 0.5), (int)(gv * kc * 65536 + 0.5), (int)(bu * kc * 65536 + 0.5), full ? 0 : 16};
}
static inline uint8_t cl8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static void row_rgba(const uint8_t *ys, const uint8_t *uvs, uint8_t *o, int cw, yuvk k) {
    int x = 0;
    for (; x + 16 <= cw; x += 16) {
        uint8x16_t y8 = vld1q_u8(ys + x); uint8x8x2_t uv = vld2_u8(uvs + x);
        int16x8_t u16 = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(uv.val[0])), vdupq_n_s16(128)), v16 = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(uv.val[1])), vdupq_n_s16(128));
        int16x8_t yl = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(y8))), vdupq_n_s16((int16_t)k.y0)), yh = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(y8))), vdupq_n_s16((int16_t)k.y0));
        int32x4_t L[4] = {vmulq_n_s32(vmovl_s16(vget_low_s16(yl)), k.y), vmulq_n_s32(vmovl_s16(vget_high_s16(yl)), k.y), vmulq_n_s32(vmovl_s16(vget_low_s16(yh)), k.y), vmulq_n_s32(vmovl_s16(vget_high_s16(yh)), k.y)};
        int32x4_t u0 = vmovl_s16(vget_low_s16(u16)), u1 = vmovl_s16(vget_high_s16(u16)), v0 = vmovl_s16(vget_low_s16(v16)), v1 = vmovl_s16(vget_high_s16(v16));
        int32x4_t cr[2] = {vmulq_n_s32(v0, k.rv), vmulq_n_s32(v1, k.rv)}, cb[2] = {vmulq_n_s32(u0, k.bu), vmulq_n_s32(u1, k.bu)};
        int32x4_t cg[2] = {vaddq_s32(vmulq_n_s32(u0, k.gu), vmulq_n_s32(v0, k.gv)), vaddq_s32(vmulq_n_s32(u1, k.gu), vmulq_n_s32(v1, k.gv))};
        uint16x4_t R[4], G[4], B[4]; const int32x4_t half = vdupq_n_s32(32768);
        for (int q = 0; q < 4; q++) {   // pixels 4q..4q+3 share chroma samples 2q, 2q+1
            int32x4_t r = q & 1 ? vzip2q_s32(cr[q >> 1], cr[q >> 1]) : vzip1q_s32(cr[q >> 1], cr[q >> 1]), g = q & 1 ? vzip2q_s32(cg[q >> 1], cg[q >> 1]) : vzip1q_s32(cg[q >> 1], cg[q >> 1]),
                      b = q & 1 ? vzip2q_s32(cb[q >> 1], cb[q >> 1]) : vzip1q_s32(cb[q >> 1], cb[q >> 1]), l = vaddq_s32(L[q], half);
            R[q] = vqmovun_s32(vshrq_n_s32(vaddq_s32(l, r), 16)); G[q] = vqmovun_s32(vshrq_n_s32(vsubq_s32(l, g), 16)); B[q] = vqmovun_s32(vshrq_n_s32(vaddq_s32(l, b), 16));
        }
        uint8x16x4_t px = {{vcombine_u8(vqmovn_u16(vcombine_u16(R[0], R[1])), vqmovn_u16(vcombine_u16(R[2], R[3]))), vcombine_u8(vqmovn_u16(vcombine_u16(G[0], G[1])), vqmovn_u16(vcombine_u16(G[2], G[3]))),
                            vcombine_u8(vqmovn_u16(vcombine_u16(B[0], B[1])), vqmovn_u16(vcombine_u16(B[2], B[3]))), vdupq_n_u8(255)}};
        vst4q_u8(o + 4 * x, px);
    }
    for (; x < cw; x++) {
        int l = (ys[x] - k.y0) * k.y + 32768, u = uvs[x & ~1] - 128, v = uvs[(x & ~1) + 1] - 128;
        o[4 * x] = cl8((l + k.rv * v) >> 16); o[4 * x + 1] = cl8((l - k.gu * u - k.gv * v) >> 16); o[4 * x + 2] = cl8((l + k.bu * u) >> 16); o[4 * x + 3] = 255;
    }
}
int mavc_take(mavc *a, int flush, uint8_t *dst, int w, int h, int64_t *pts_us) {
    if (a->mc) {
        int err = 0;
        sync_flush(a);
        if (a->mc && flush && !a->eos_sent) end_stream(a);
        else if (a->mc && !a->eos_sent && drain(a, 0, &err) < 0) fail(a, "MediaCodec H.264 decoder error", err);
    }
    // black pictures of the fallback wait for those before them in presentation order (as the port's stub did, `depth` for a stream with B-frames); the decoder's own come in that order
    if (a->nq <= (a->failed && !flush ? a->depth : 0)) return 0;
    pic p = a->q[0]; memmove(a->q, a->q + 1, (size_t)--a->nq * sizeof a->q[0]);
    if (pts_us) *pts_us = p.pts;
    int cw = p.w < w ? p.w : w, chh = p.h < h ? p.h : h, sw = (p.w + 1) & ~1;
    const uint8_t *s = p.nv12, *suv = s ? s + (size_t)sw * p.h : NULL;
    if (a->rgba) {
        if (!s || cw < w || chh < h) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u;   // opaque black
        yuvk k = yuv_k(p.matrix ? p.matrix : 601, p.full);
        if (s) for (int r = 0; r < chh; r++) row_rgba(s + (size_t)r * sw, suv + (size_t)(r / 2) * sw, dst + (size_t)r * w * 4, cw, k);
    } else {
        if (!s || cw < w || chh < h) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }   // NV12 black (video range)
        if (s) {
            for (int r = 0; r < chh; r++) memcpy(dst + (size_t)r * w, s + (size_t)r * sw, (size_t)cw);
            size_t uvb = (size_t)((cw + 1) & ~1); if (uvb > (size_t)w) uvb = (size_t)w;   // interleaved U V: one byte per luma column
            for (int r = 0; r < (chh + 1) / 2 && r < h / 2; r++) memcpy(dst + (size_t)w * h + (size_t)r * w, suv + (size_t)r * sw, uvb);
        }
    }
    pic_release(a, &p);
    return 1;
}
