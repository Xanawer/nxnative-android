// AAC decoding for the movie decoder (hle_movie.c md_aac) on Android's MediaCodec, where the Mac and the iPhone use AudioToolbox.
// Adapted from nx2apk's dm/android/aconv_mc.c (github.com/ghostyshell/nx2apk; MIT, Copyright (c) 2026 nx2apk contributors; licence text in dm/third_party/nx2apk/LICENSE), which wrapped MediaCodec
// as an Apple AudioConverter; md_aac calls this directly. The AudioSpecificConfig (Configure's csd-0) is MediaCodec's csd-0; without one an AAC-LC configuration is made from Configure's rate and
// channel count, as the Apple path does.
// What goes out is settled from the configuration alone (maac_settle: hle_movie.c's Configure, before the format event announces it; maac_new again, to the same values). hle_movie.c's
// GetOutputBufferSize promises 1,024 samples per channel and packet, 2,048 when the configuration's object type is SBR (5) or parametric stereo (29): each packet gives that many (960 / 1,920 with
// 960-sample frames), and the rate is the one that makes them last the packet's time: the core rate, twice it with SBR by object type. HE-AAC that signals SBR only in the configuration's
// extension (0x2b7, what Apple's encoders write) or in band decodes at twice the core rate; that PCM is halved (a half-band filter, 8 frames of delay) so 1,024 samples fit the client's buffers.
// Channels: Configure's count, else the configuration's (2 with parametric stereo); the decoder's are mapped onto them.
// The decoder runs in another process and may give a packet's PCM a call or more later. Each packet's PCM (the next `frames` frames of what the decoder gave) goes out with that packet's time,
// in packet order: a packet waits (bounded) for its own; one that does not come in time goes out later (maac_ready: hle_movie.c's md_pump hands it out into the next free slot), and at the
// end of stream the decoder gives up what it holds (maac_decode with flush). A decoder that holds packets back is learnt from one timed-out wait and not waited for again.
// The decoder is fed regular times of its own (packet k at k packets' duration) and its output times say which packet each buffer's PCM is: a packet it passed over (a damaged one it skipped)
// gets silence of its length in its place, so the later packets' PCM stays with their times; PCM whose time is behind the ones already placed is dropped, and so is more than one packet's worth
// for one time (an output repeated or too long); outputs with such a surplus 3 in a row fail the decoder (pcm_place). Buffers carrying a packet's PCM in pieces, or fewer frames (a decoder
// removing its own delay at the start), keep the stream as it comes.
// Flush (maac_drop, a seek): the PCM, the times and the filter's history are forgotten at once, the decoder's own flush is done at the next packet.
// Bounded waits: making the decoder, its flush and its stop / delete run on a helper thread with a deadline (movie_mc.c); a failed decoder is not waited for.
// Fallback: no decoder, or one that fails, gives silence of each packet's length (those it held too), with one [RED]. DM_MOVIE_MC_FAIL=aac (creation) or aac@N (the N-th packet) forces it;
// DM_MOVIE_MC_AAC_NOWAIT=1 skips the wait for a packet's own PCM, so the decoder's delay shows (both for dm/android/tests/t_movie_mc.c)
#include "../../host/dm.h"
#include "movie_mc.h"
#include <math.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#define OUT_WAIT_US 10000      // one wait for a packet's PCM, at most 3 per packet
#define IN_WAIT_US 10000       // one wait for a free input buffer
#define IN_WAIT_MAX_US 100000  // then the decoder counts as stuck
#define EOS_WAIT_US 500000     // end of stream: what the decoder still holds within 0.5 s
#define OPEN_WAIT_US 2000000   // create + configure + start
#define FLUSH_WAIT_US 500000   // a flush (a seek)
#define CLOSE_WAIT_US 500000   // stop + delete of a decoder that worked; a failed one is not waited for
#define PEND_MAX 64            // packets fed whose PCM is not handed out, at most (then the decoder counts as stuck)
#define HB_N 31                // half-band filter taps
static const int k_rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};

typedef struct { int aot, core_rate, cc, sbr, ps, explicit_sbr, flen; } ascfg;   // the AudioSpecificConfig as read (or made); flen 960 or 1,024
struct maac {
    AMediaCodec *mc; char name[64]; int failed, red, red_end, fail_at, nowait; double open_ms;
    ascfg c;
    int rate, ch, frames;           // what goes out: rate, channels, frames per packet
    int orate, och, nfmt;           // the decoder's output format
    int flush_due, eos_sent, eos_done;
    int64_t nin, nout, npk; int lag, flight, late, padded, overrun;   // flight: packets in the decoder without an output buffer back (since the last flush)
    int64_t seq, cur_k; int skipped, stale, red_map;   // seq: packets fed (the k-th goes in at time k × the packet's duration); cur_k: the packet whose PCM came last
    size_t kfr;                     // the decoder's frames placed for packet cur_k (one packet's worth at most, kper)
    int kx, xrun, xfail, surplus;   // kx: an output for packet cur_k had frames beyond that (dropped, counted in surplus); xrun: outputs with such a surplus in a row (a packet without one ends
                                    // the run); xfail: xrun reached 3, sticky (maac_decode fails the decoder: overrun, end_stream)
    int64_t pend[PEND_MAX]; int npend;   // times of the packets fed whose PCM is not handed out, oldest first
    int16_t *fifo; size_t nfifo, fcap;   // frames at rate / ch, the oldest packet's first
    float *hx; size_t nhx, hcap;    // half-band decimator: input frames not consumed yet (the filter's history first)
    double rpos; float *rprev;      // other rate ratios: linear interpolation (position in the input, last input frame)
    float *tmp; size_t tcap;
};

// ---------- the AudioSpecificConfig (ISO 14496-3 1.6.2.1, with GASpecificConfig and the 0x2b7 / 0x548 sync extensions) ----------
typedef struct { const uint8_t *p; int n, bit; } bitr;
static unsigned br_u(bitr *b, int k) { unsigned v = 0; while (k--) { if (b->bit >> 3 >= b->n) { b->bit = b->n * 8 + 1; return 0; } v = v << 1 | (b->p[b->bit >> 3] >> (7 - (b->bit & 7)) & 1); b->bit++; } return v; }
static int br_left(const bitr *b) { return b->n * 8 - b->bit; }
static int br_aot(bitr *b) { int t = (int)br_u(b, 5); return t == 31 ? 32 + (int)br_u(b, 6) : t; }
static int br_rate(bitr *b) { unsigned i = br_u(b, 4); return i == 15 ? (int)br_u(b, 24) : i < 13 ? k_rates[i] : 0; }
static int asc_read(ascfg *a, const uint8_t *p, int n) {   // 0 if unreadable
    bitr b = {p, n, 0}; a->flen = 1024;
    int t = br_aot(&b); a->core_rate = br_rate(&b); a->cc = (int)br_u(&b, 4);
    if (t == 5 || t == 29) { a->explicit_sbr = a->sbr = 1; a->ps = t == 29; br_rate(&b); t = br_aot(&b); if (t == 22) br_u(&b, 4); }
    a->aot = t;
    if (br_left(&b) < 0 || a->core_rate <= 0) return 0;
    if (t == 1 || t == 2 || t == 3 || t == 4 || t == 6 || t == 7 || t == 17 || (t >= 19 && t <= 23)) {   // GASpecificConfig
        a->flen = br_u(&b, 1) ? 960 : 1024;
        if (br_u(&b, 1)) br_u(&b, 14);
        int ext = (int)br_u(&b, 1);
        if (!a->cc) return 1;   // a program config element follows: no extension read past it
        if (t == 6 || t == 20) br_u(&b, 3);
        if (ext) { if (t == 22) br_u(&b, 16); if (t == 17 || t == 19 || t == 20 || t == 23) br_u(&b, 3); br_u(&b, 1); }
    }
    if (!a->explicit_sbr && br_left(&b) >= 16 && br_u(&b, 11) == 0x2b7 && br_aot(&b) == 5 && br_u(&b, 1)) {   // backward-compatible SBR signalling
        a->sbr = 1; br_rate(&b);
        if (br_left(&b) >= 12 && br_u(&b, 11) == 0x548) a->ps = (int)br_u(&b, 1);
    }
    return br_left(&b) >= 0;
}
// the configuration the decoder gets (read, or AAC-LC from *rate / *ch: 2 channels at 48 kHz without them) and what goes out: *rate / *ch set, frames per packet; 1 = it was read
static int settle(ascfg *c, const uint8_t *asc, int an, int *rate, int *ch, int *frames) {
    memset(c, 0, sizeof *c); c->flen = 1024;
    int have = an >= 2 && asc && asc_read(c, asc, an);
    if (!have) { memset(c, 0, sizeof *c); c->aot = 2; c->flen = 1024; c->core_rate = *rate > 0 ? *rate : 48000; c->cc = *ch > 0 && *ch <= 6 ? *ch : *ch == 8 ? 7 : 2; }
    int k = *ch > 0 ? *ch : c->cc >= 1 && c->cc <= 6 ? (c->cc == 1 && c->ps ? 2 : c->cc) : c->cc == 7 ? 8 : 2;
    *frames = c->flen * (c->explicit_sbr ? 2 : 1); *rate = c->core_rate * (c->explicit_sbr ? 2 : 1); *ch = k > 8 ? 8 : k;
    return have;
}
void maac_settle(const uint8_t *asc, int an, int *rate, int *ch) {
    ascfg c; int fr, r0 = *rate; settle(&c, asc, an, rate, ch, &fr);
    if (r0 > 0 && r0 != *rate) LOG("[movie] AAC: Configure's %d Hz is not the rate of the PCM (%d Hz: %d samples per %d-sample packet at %d Hz), announced from here on", r0, *rate, fr, c.flen, c.core_rate);
}

int maac_frames(const uint8_t *asc, int an) { ascfg c; int r = 0, ch = 0, fr; settle(&c, asc, an, &r, &ch, &fr); return fr; }

static void fail(maac *a, const char *why, int st);
static void out_format(maac *a) {
    AMediaFormat *f = AMediaCodec_getOutputFormat(a->mc); if (!f) return;
    int32_t v;
    if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &v) && v > 0) a->orate = v;
    if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &v) && v > 0 && v <= 8) a->och = v;
    AMediaFormat_delete(f);
    if (!a->nfmt++ || a->orate != a->rate || a->och != a->ch)
        LOG("[movie] AAC output (%s): %d Hz %d channels%s%s", a->name, a->orate, a->och, a->orate == a->rate ? "" : a->orate == 2 * a->rate ? ", halved to the track's rate" : ", resampled to the track's rate",
            a->och == a->ch ? "" : ", channels mapped to the track's");
}

maac *maac_new(const uint8_t *asc, int an, int *rate, int *ch) {
    maac *a = calloc(1, sizeof *a); if (!a) die("movie: AAC decoder allocation failed");
    mmc_live_add(MMC_LIVE_MAAC, 1);
    int r0 = *rate, have = settle(&a->c, asc, an, rate, ch, &a->frames);
    if (r0 > 0 && r0 != *rate) LOG("[movie] AAC: Configure's %d Hz is not the rate of the PCM (%d Hz: %d samples per %d-sample packet at %d Hz), reported from here on", r0, *rate, a->frames, a->c.flen, a->c.core_rate);
    a->rate = *rate; a->ch = *ch;
    uint8_t cfg[16]; int cn = 0;
    if (!have) {   // AAC-LC: object type 2, the rate's index (a rate outside the table written out in full, escape index 15), channel configuration, GASpecificConfig: 1,024-sample frames, nothing else
        int fi = 15; for (int i = 0; i < 13; i++) if (k_rates[i] == a->c.core_rate) fi = i;
        uint64_t bits = 2; int nb = 5;
        bits = bits << 4 | (unsigned)fi; nb += 4; if (fi == 15) { bits = bits << 24 | (unsigned)a->c.core_rate; nb += 24; }
        bits = bits << 4 | (unsigned)a->c.cc; nb += 4; bits <<= 3; nb += 3;
        cn = (nb + 7) / 8; bits <<= cn * 8 - nb; for (int i = 0; i < cn; i++) cfg[i] = (uint8_t)(bits >> (8 * (cn - 1 - i)));
    }
    a->orate = a->c.core_rate; a->och = a->c.cc >= 1 && a->c.cc <= 7 ? (a->c.cc == 7 ? 8 : a->c.cc) : 2; a->cur_k = -1;
    const char *nw = getenv("DM_MOVIE_MC_AAC_NOWAIT"); a->nowait = nw && *nw == '1';
    a->fail_at = mmc_fail_at("aac");
    if (a->fail_at == 0) { fail(a, "MediaCodec AAC decoder creation failed (forced, DM_MOVIE_MC_FAIL)", 0); return a; }
    mmc_binder_threads();
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, a->c.core_rate);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, a->och);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_IS_ADTS, 0);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_PCM_ENCODING, 2);   // 16-bit
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_AAC_MAX_OUTPUT_CHANNEL_COUNT, a->ch > 2 ? 8 : 2);   // the decoder's own downmix to stereo when the track has 2 or fewer; mono is made here
    AMediaFormat_setBuffer(f, AMEDIAFORMAT_KEY_CSD_0, have ? asc : cfg, (size_t)(have ? an : cn));
    int st = 0; int64_t t0 = mmc_now_us();
    a->mc = mmc_open("audio/mp4a-latm", f, OPEN_WAIT_US, &st, a->name, sizeof a->name);
    a->open_ms = (mmc_now_us() - t0) / 1000.0;
    if (!a->mc) { fail(a, st == MMC_TIMEOUT ? "MediaCodec AAC decoder not made within 2 s" : st == AMEDIA_ERROR_UNSUPPORTED && !a->name[0] ? "no MediaCodec AAC decoder" : "MediaCodec AAC configure / start failed", st); return a; }
    LOG("[movie] AAC: MediaCodec %s, AudioSpecificConfig %d bytes%s (object type %d%s%s, %d Hz, configuration %d): %d Hz %d channels out, %d samples per packet (opened in %.1f ms)", a->name,
        have ? an : cn, have ? "" : " made here", a->c.aot, a->c.sbr ? (a->c.explicit_sbr ? ", SBR" : ", SBR by extension") : "", a->c.ps ? ", parametric stereo" : "", a->c.core_rate, a->c.cc,
        a->rate, a->ch, a->frames, a->open_ms);
    return a;
}
void maac_free(maac *a) { if (!a) return; mmc_close(a->mc, CLOSE_WAIT_US); free(a->fifo); free(a->hx); free(a->rprev); free(a->tmp); free(a); mmc_live_add(MMC_LIVE_MAAC, -1); }
void maac_info(const maac *a, mmc_info *o) {
    memset(o, 0, sizeof *o); snprintf(o->codec, sizeof o->codec, "%s", a->name); o->failed = a->failed; o->open_ms = a->open_ms;
    o->rate = a->rate; o->ch = a->ch; o->frames = a->frames; o->orate = a->orate; o->och = a->och; o->lag = a->lag; o->late = a->late; o->padded = a->padded;
    o->nin = a->nin; o->nout = a->nout; o->nfmt = a->nfmt; o->skipped = a->skipped; o->stale = a->stale; o->surplus = a->surplus;
}
int maac_ready(const maac *a) { int k = a->frames > 0 ? (int)(a->nfifo / (size_t)a->frames) : 0; return k < a->npend ? k : a->npend; }
int maac_queued(const maac *a) { return a->npend; }

// ---------- PCM: the decoder's channels and rate → the track's, into the FIFO ----------
static void fifo_put(maac *a, const float *x, size_t fr) {   // fr frames of a->ch channels
    if (a->nfifo + fr > a->fcap) { a->fcap = (a->nfifo + fr) * 2; a->fifo = realloc(a->fifo, a->fcap * 2u * (unsigned)a->ch); if (!a->fifo) die("movie: AAC buffer allocation failed"); }
    int16_t *d = a->fifo + a->nfifo * (size_t)a->ch;
    if (x) for (size_t i = 0; i < fr * (size_t)a->ch; i++) { float v = x[i]; d[i] = (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : lrintf(v)); }
    else memset(d, 0, fr * 2u * (unsigned)a->ch);   // silence
    a->nfifo += fr;
}
static float hb_h[HB_N];
static pthread_once_t hb_once = PTHREAD_ONCE_INIT;
static void hb_init(void) {   // windowed sinc, cut at a quarter of the input rate (Blackman window), unity gain
    double s = 0, h[HB_N];
    for (int k = 0; k < HB_N; k++) { double m = k - HB_N / 2, x = m * M_PI / 2; h[k] = (m ? sin(x) / x : 1.0) * (0.42 - 0.5 * cos(2 * M_PI * k / (HB_N - 1)) + 0.08 * cos(4 * M_PI * k / (HB_N - 1))); s += h[k]; }
    for (int k = 0; k < HB_N; k++) hb_h[k] = (float)(h[k] / s);
}
// the filters start from silence: the half-band history is HB_N frames (an odd count: output j is centred on input 2j - 16, a delay of exactly 8 output frames, and every 2 inputs give 1)
static void filters_reset(maac *a) {
    if (a->hx) { memset(a->hx, 0, (size_t)HB_N * (size_t)a->ch * sizeof *a->hx); a->nhx = HB_N; }
    a->rpos = 0; if (a->rprev) memset(a->rprev, 0, (size_t)a->ch * sizeof *a->rprev);
}
static void pcm_in(maac *a, const int16_t *s, size_t fr) {
    if (!fr) return;   // (a buffer shorter than one frame)
    int ch = a->ch, och = a->och;
    if (a->tcap < fr * (size_t)ch) { a->tcap = fr * (size_t)ch; a->tmp = realloc(a->tmp, a->tcap * sizeof *a->tmp); if (!a->tmp) die("movie: AAC buffer allocation failed"); }
    float *x = a->tmp;
    for (size_t i = 0; i < fr; i++) {   // channels: the same count as they are; mono from the first two; a mono decoder output on every channel; else the first ones, the rest silent
        const int16_t *in = s + i * (size_t)och; float *o = x + i * (size_t)ch;
        if (och == ch) for (int k = 0; k < ch; k++) o[k] = in[k];
        else if (ch == 1) o[0] = 0.5f * ((float)in[0] + (float)in[1]);
        else if (och == 1) for (int k = 0; k < ch; k++) o[k] = in[0];
        else for (int k = 0; k < ch; k++) o[k] = k < och ? in[k] : 0;
    }
    if (a->orate == a->rate) { fifo_put(a, x, fr); return; }
    if (a->orate == 2 * a->rate) {   // half-band filter, every second output
        pthread_once(&hb_once, hb_init);
        if (!a->hx) { a->hcap = (HB_N + fr) * (size_t)ch * 2; a->hx = calloc(a->hcap, sizeof *a->hx); if (!a->hx) die("movie: AAC buffer allocation failed"); a->nhx = HB_N; }   // history: silence
        size_t need = (a->nhx + fr) * (size_t)ch;
        if (a->hcap < need) { a->hcap = need * 2; a->hx = realloc(a->hx, a->hcap * sizeof *a->hx); if (!a->hx) die("movie: AAC buffer allocation failed"); }
        memcpy(a->hx + a->nhx * (size_t)ch, x, fr * (size_t)ch * sizeof *x); a->nhx += fr;
        size_t nout = (a->nhx - (HB_N - 1)) / 2;
        float o[8 * 256];
        for (size_t j = 0; j < nout; ) {
            size_t m = nout - j < 256 ? nout - j : 256;
            for (size_t q = 0; q < m; q++) for (int c = 0; c < ch; c++) { float acc = 0; const float *in = a->hx + (2 * (j + q)) * (size_t)ch + c; for (int k = 0; k < HB_N; k++) acc += hb_h[k] * in[(size_t)k * ch]; o[q * ch + c] = acc; }
            fifo_put(a, o, m); j += m;
        }
        size_t used = 2 * nout; memmove(a->hx, a->hx + used * (size_t)ch, (a->nhx - used) * (size_t)ch * sizeof *a->hx); a->nhx -= used;
        return;
    }
    // any other ratio: linear interpolation between consecutive input frames
    if (!a->rprev) { a->rprev = calloc((size_t)ch, sizeof *a->rprev); if (!a->rprev) die("movie: AAC buffer allocation failed"); }
    double step = (double)a->orate / a->rate; float o[8 * 256]; size_t m = 0;
    for (;;) {
        double p = a->rpos; if (p >= (double)fr - 1) break;   // the next output's position from this buffer's frame 0, from -1 (between the last buffer's last frame and frame 0)
        int i0 = (int)floor(p); float f = (float)(p - i0);
        for (int c = 0; c < ch; c++) { float x0 = i0 < 0 ? a->rprev[c] : x[(size_t)i0 * ch + c], x1 = x[(size_t)(i0 + 1) * ch + c]; o[m * ch + c] = x0 + f * (x1 - x0); }
        if (++m == 256) { fifo_put(a, o, m); m = 0; }
        a->rpos += step;
    }
    if (m) fifo_put(a, o, m);
    a->rpos -= (double)fr; memcpy(a->rprev, x + (fr - 1) * (size_t)ch, (size_t)ch * sizeof *x);
}

// ---------- the codec ----------
// a failed decoder goes to a helper thread to be stopped and deleted; every packet waiting for its PCM gets silence for what did not come
static void pad_pending(maac *a) {
    size_t want = (size_t)a->npend * (size_t)a->frames;
    if (a->nfifo < want) { a->padded += (int)(want - a->nfifo); fifo_put(a, NULL, want - a->nfifo); }
}
static void fail(maac *a, const char *why, int st) {
    if (!a->red++) LOG("[RED] movie: %s (%d): the AAC track plays silence (%d channels %d Hz, %s)", why, st, a->ch, a->rate, a->name[0] ? a->name : "no decoder");
    a->failed = 1; mmc_close(a->mc, 0); a->mc = NULL;
    size_t want = (size_t)a->npend * (size_t)a->frames; if (a->nfifo > want) a->nfifo = want;   // PCM beyond the packets waiting (a decoder giving more than its packets) is not played
    pad_pending(a);
}
// the decoder's output time → the packet (its feed time, k × the packet's duration, rounded to the nearest)
static int64_t pkt_of(const maac *a, int64_t t) { int64_t d = (int64_t)a->frames * 1000000; return t < 0 ? -1 : (t * a->rate + d / 2) / d; }
static int64_t pkt_time(const maac *a, int64_t k) { return k * a->frames * 1000000 / a->rate; }
// silence for k whole packets, put through the same path as the decoder's PCM (at its rate and channels: the filters stay continuous)
static void pcm_silence(maac *a, int64_t k) {
    static const int16_t z[8 * 1024];
    size_t fr = (size_t)((k * a->frames * (int64_t)a->orate + a->rate / 2) / a->rate);
    for (size_t m; fr; fr -= m) { m = fr < 1024 ? fr : 1024; pcm_in(a, z, m); }
}
// one packet's worth of the decoder's frames (at its rate: pcm_in turns them into `frames` at the track's), rounded up
static size_t kper(const maac *a) { return (size_t)(((int64_t)a->frames * a->orate + a->rate - 1) / a->rate); }
// packet k (after cur_k) starts: with gap, packets passed over get silence first. If packet cur_k's PCM came without a surplus, a run of outputs with one ends
static void pkt_start(maac *a, int64_t k, int gap) {
    if (!a->kx) a->xrun = 0;
    a->kx = 0;
    if (gap && k > a->cur_k + 1) {
        int64_t g = k - a->cur_k - 1; a->skipped += (int)g;
        static _Atomic int said; if (!said++) LOG("[movie] AAC (%s): the decoder passed over %lld packet(s) before packet %lld (a damaged one?): silence in their place (first case)", a->name, (long long)g, (long long)k);
        pcm_silence(a, g);
    }
    a->flight -= (int)(k - a->cur_k); if (a->flight < 0) a->flight = 0; a->cur_k = k; a->kfr = 0;
}
static void place(maac *a, const int16_t *s, size_t fr) { pcm_in(a, s, fr); a->kfr += fr; }
// in order (the decoder's times say nothing): packet cur_k filled up to one packet's worth, then the next packet fed; beyond the last one fed it is dropped
static void place_in_order(maac *a, const int16_t *s, size_t fr) {
    while (fr) {
        size_t per = kper(a);
        if (a->cur_k < 0 || a->kfr >= per) {
            if (a->cur_k + 1 >= a->seq) { a->surplus += (int)fr; a->stale++; return; }
            pkt_start(a, a->cur_k + 1, 0);
        }
        size_t m = per - a->kfr < fr ? per - a->kfr : fr;
        place(a, s, m); s += m * (size_t)a->och; fr -= m;
    }
}
// one output buffer's PCM (fr of the decoder's frames), placed by its time: a later time than the last one's starts that packet (packets passed over get silence first), the same time adds a
// piece, up to one packet's worth (kper); a time behind is dropped, and so are the frames beyond one packet's worth for a time (an output repeated or too long: counted in surplus, the output
// in stale). Outputs with such a surplus 3 in a row (each counts, a packet whose PCM came without one ends the run) mean the decoder's PCM is not what it reports (a rate it does not report,
// times it does not advance): xfail is set there, nothing more is placed, and maac_decode fails the decoder. A time never fed (the decoder does not keep the times) goes on in order
static void pcm_place(maac *a, const int16_t *s, size_t fr, int64_t t) {
    if (a->xfail) return;
    int64_t k = pkt_of(a, t);
    if (k >= a->seq || k < 0) {
        if (!a->red_map++) LOG("[movie] AAC (%s): output time %lld µs is no packet's (%lld fed): PCM placed in order", a->name, (long long)t, (long long)a->seq);
        place_in_order(a, s, fr); return;
    }
    if (k < a->cur_k) {
        a->stale++; static _Atomic int said; if (!said++) LOG("[movie] AAC (%s): PCM for packet %lld after packet %lld's: dropped (first case)", a->name, (long long)k, (long long)a->cur_k);
        return;
    }
    if (k > a->cur_k) pkt_start(a, k, 1);
    size_t per = kper(a), m = a->kfr < per ? per - a->kfr : 0; if (m > fr) m = fr;
    place(a, s, m); fr -= m;
    if (!fr) return;
    a->surplus += (int)fr; a->stale++; a->kx = 1;
    static _Atomic int said; if (!said++) LOG("[movie] AAC (%s): more than one packet's PCM for packet %lld's time: %zu frames dropped (first case)", a->name, (long long)a->cur_k, fr);
    if (++a->xrun >= 3) a->xfail = 1;
}
// one output buffer within wait_us: 1 = a buffer or a format change, 0 = none, -1 = a decoder error (*err)
static int drain_one(maac *a, int64_t wait_us, int *err) {
    AMediaCodecBufferInfo bi;
    ssize_t o = AMediaCodec_dequeueOutputBuffer(a->mc, &bi, wait_us);
    if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { out_format(a); return 1; }
    if (o == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) return 1;
    if (o == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return 0;
    if (o < 0) { *err = (int)o; return -1; }
    if (!a->nfmt) out_format(a);
    size_t sz = 0; uint8_t *b = AMediaCodec_getOutputBuffer(a->mc, (size_t)o, &sz);
    if (b && bi.size > 0 && bi.offset >= 0 && (size_t)bi.offset + (size_t)bi.size <= sz) pcm_place(a, (const int16_t *)(b + bi.offset), (size_t)bi.size / (2u * (unsigned)a->och), bi.presentationTimeUs);
    AMediaCodec_releaseOutputBuffer(a->mc, (size_t)o, false);
    if (bi.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) a->eos_done = 1;
    else a->nout++;
    return 1;
}
static int drain_all(maac *a, int *err) { int r; while ((r = drain_one(a, 0, err)) > 0) {} return r; }
static int feed(maac *a, const uint8_t *p, int n, uint32_t flags, int *err) {
    for (int64_t t0 = mmc_now_us();;) {
        ssize_t i = AMediaCodec_dequeueInputBuffer(a->mc, IN_WAIT_US);
        if (i >= 0) {
            size_t cap = 0; uint8_t *b = AMediaCodec_getInputBuffer(a->mc, (size_t)i, &cap);
            uint64_t t = (uint64_t)pkt_time(a, a->seq);   // the decoder's own times: regular, whatever the packets carry
            if (!b || cap < (size_t)n) { AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, 0, t, 0); *err = (int)cap; return -1; }
            if (n) memcpy(b, p, (size_t)n);
            media_status_t st = AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, (size_t)n, t, flags);
            if (st != AMEDIA_OK) { *err = st; return -1; }
            if (n) { a->seq++; a->nin++; a->flight++; }
            return 0;
        }
        if (i != AMEDIACODEC_INFO_TRY_AGAIN_LATER) { *err = (int)i; return -1; }
        if (drain_all(a, err) < 0) return -1;
        if (mmc_now_us() - t0 > IN_WAIT_MAX_US) { *err = 0; return -1; }
    }
}
// a Flush since the last call: the decoder forgets what it holds (bounded: a flush that does not return in time leaves the codec to the helper thread, and the track plays silence)
static void sync_flush(maac *a) {
    if (!a->flush_due || !a->mc) return;
    a->flush_due = 0; int st = mmc_flush(a->mc, FLUSH_WAIT_US);
    if (st == MMC_TIMEOUT) { a->mc = NULL; fail(a, "MediaCodec AAC flush did not return within 0.5 s", 0); return; }
    if (st != AMEDIA_OK) { fail(a, "MediaCodec AAC flush failed", st); return; }
    a->eos_sent = a->eos_done = 0; a->flight = 0; a->cur_k = a->seq - 1; a->kfr = SIZE_MAX / 2; a->kx = a->xrun = 0;   // the last packet before counts as complete
}
void maac_drop(maac *a) {
    a->npend = 0; a->nfifo = 0; a->lag = 0; filters_reset(a);
    if (a->mc) a->flush_due = 1;
}
// the decoder gives more than its packets (a rate it does not report), which breaks the packets' times: 3 outputs in a row with more than one packet's worth for their time (xfail, set where
// it happens: however the outputs come in batches), or PCM in the FIFO beyond the packets waiting for it. (At the end of stream the decoder's own delay may come out after the last packet's
// PCM, and is dropped)
static int overrun(maac *a) { return a->xfail || a->nfifo > ((size_t)a->npend + 2) * (size_t)a->frames; }
// end of stream: the decoder gives up what it holds (within EOS_WAIT_US); PCM beyond the packets is dropped, packets without theirs get silence
static void end_stream(maac *a) {
    int err = 0;
    if (!a->eos_sent) { if (feed(a, NULL, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM, &err) < 0) { fail(a, "MediaCodec AAC takes no end of stream", err); return; } a->eos_sent = 1; }
    for (int64_t t0 = mmc_now_us(); !a->eos_done && mmc_now_us() - t0 < EOS_WAIT_US;) if (drain_one(a, 10000, &err) < 0) { fail(a, "MediaCodec AAC error at the end of stream", err); return; }
    if (a->xfail) { fail(a, "MediaCodec AAC decoder gives more PCM than its packets", a->surplus); return; }
    size_t want = (size_t)a->npend * (size_t)a->frames;
    if (a->nfifo > want) a->nfifo = want;
    else if (a->nfifo < want) {
        size_t miss = want - a->nfifo;
        if (!a->red_end++) {
            if (miss > (size_t)a->frames) LOG("[RED] movie: the AAC decoder (%s) gave %zu frames fewer than its packets by the end of stream%s: silence in their place", a->name, miss, a->eos_done ? "" : " (no end of stream within 0.5 s)");
            else LOG("[movie] AAC (%s): %zu frames fewer than the packets at the end of stream (the decoder's delay), silence in their place", a->name, miss);
        }
        pad_pending(a);
    }
    a->eos_done = 1;
}
// the oldest packet's PCM into out, with its time
static int take_one(maac *a, int16_t *out, int64_t *pts_out) {
    int F = a->frames;
    *pts_out = a->pend[0]; memmove(a->pend, a->pend + 1, (size_t)--a->npend * sizeof a->pend[0]);
    memcpy(out, a->fifo, (size_t)F * 2u * (unsigned)a->ch);
    memmove(a->fifo, a->fifo + (size_t)F * a->ch, (a->nfifo - (size_t)F) * 2u * (unsigned)a->ch); a->nfifo -= (size_t)F;
    return F;
}
int maac_decode(maac *a, const uint8_t *p, int n, int64_t pts, int flush, int16_t *out, size_t cap, int64_t *pts_out) {
    int F = a->frames, err = 0, r;
    if ((size_t)F * 2u * (unsigned)a->ch > cap) { if (!a->red++) LOG("[RED] movie: AAC packet of %d frames × %d channels exceeds the %zu-byte output", F, a->ch, cap); return 0; }
    if (!a->failed && a->mc) { if (n > 0 && a->eos_sent) a->flush_due = 1; sync_flush(a); }   // a Flush since the last call, or a packet after the end of stream without one: start afresh
    if (n > 0) {
        a->npk++;
        if (!a->failed && a->fail_at > 0 && a->npk >= a->fail_at) fail(a, "MediaCodec AAC decoder failure (forced, DM_MOVIE_MC_FAIL)", (int)a->npk);
        if (a->npend == PEND_MAX) {   // no room for this packet's time: the decoder is stuck (every packet waiting got silence), the oldest goes out now and this one's silence waits its turn
            if (!a->failed) fail(a, "MediaCodec AAC decoder holds 64 packets without their PCM", 0);
            take_one(a, out, pts_out); a->late++;
            a->pend[a->npend++] = pts; pad_pending(a);
            return F;
        }
        a->pend[a->npend++] = pts;
        if (!a->failed) {
            if (drain_all(a, &err) < 0) fail(a, "MediaCodec AAC decoder error", err);
            else if (a->flight < a->lag) a->lag = a->flight;   // it caught up: a wait that ran out was a hiccup, not packets held back
            if (!a->failed && feed(a, p, n, 0, &err) < 0) fail(a, "MediaCodec AAC takes no input", err);
            if (!a->failed) {
                r = drain_all(a, &err);
                for (int64_t t0 = mmc_now_us(); r >= 0 && !a->nowait && a->nfifo < (size_t)F && a->flight > a->lag && mmc_now_us() - t0 < 3 * OUT_WAIT_US;)
                    if ((r = drain_one(a, OUT_WAIT_US, &err)) == 0) {   // nothing came: the decoder holds this many packets back, not waited for again while it does
                        a->lag = a->flight; static _Atomic int said; if (said++ < 4) LOG("[movie] AAC (%s): the decoder holds %d packets back", a->name, a->lag); break;
                    }
                if (r < 0) fail(a, "MediaCodec AAC decoder error", err);
                else if (overrun(a)) fail(a, "MediaCodec AAC decoder gives more PCM than its packets", (int)a->nfifo);
            }
        }
        if (a->failed) pad_pending(a);
    } else if (flush) {
        if (!a->failed && !a->eos_done) end_stream(a);
        if (a->failed) pad_pending(a);
    } else if (!a->failed) {
        if (drain_all(a, &err) < 0) fail(a, "MediaCodec AAC decoder error", err);
        else if (overrun(a)) fail(a, "MediaCodec AAC decoder gives more PCM than its packets", (int)a->nfifo);
    }
    // the oldest packet's PCM, when all of it is there
    if (!a->npend || a->nfifo < (size_t)F) return 0;
    take_one(a, out, pts_out);
    if (n <= 0 || *pts_out != pts) a->late++;
    return F;
}
