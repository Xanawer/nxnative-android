// The movie decoders on Android's MediaCodec (docs/android/PLAN.md 1.5): H.264 behind hle_movie_avc.h (movie_avc_mc.c), AAC for hle_movie.c's md_aac (movie_aac_mc.c), and what both share (movie_mc.c).
// Both fall back to what the port did before them (black pictures, silence) when MediaCodec fails; DM_MOVIE_MC_FAIL forces that for the unit test (dm/android/tests/t_movie_mc.c).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// AAC: an AudioSpecificConfig (asc / an, may be empty) and Configure's rate and channel count (0 = not given). maac_settle sets *rate / *ch to those of the PCM a decoder made from them will hand
// out, without making one: hle_movie.c's Configure calls it, so the format event announces them (HE-AAC's rate is not Configure's). maac_new settles them the same way (the same values again) and
// never fails: without a decoder it gives silence. maac_new / decode / free run on the decoder's host thread (MediaCodec calls); maac_settle and maac_drop on guest threads (no MediaCodec call)
typedef struct maac maac;
void maac_settle(const uint8_t *asc, int an, int *rate, int *ch);
maac *maac_new(const uint8_t *asc, int an, int *rate, int *ch);
int maac_frames(const uint8_t *asc, int an);   // frames per packet a decoder made from this configuration hands out (960 or 1,024, twice with SBR by object type): hle_movie.c's GetOutputBufferSize
// Each packet's PCM is one AAC frame at the settled rate (frames per packet: mmc_info.frames), handed out once the decoder gave it, with that packet's time: n > 0 feeds a packet (time pts) and hands
// out the oldest packet's PCM if it is there; n == 0 hands out the next one that is there, and with flush (the end of stream) first gets every one the decoder still holds (silence for any it never
// gives). The decoder's output times say which packet its PCM is: a packet it passes over gets silence in its place, PCM for a time no longer waited for is dropped, and so is more than one
// packet's worth for one time (3 outputs in a row with that: the decoder fails).
// Returns the frames written (at most cap bytes), the packet's time in *pts_out; 0 = none yet
int maac_decode(maac *a, const uint8_t *p, int n, int64_t pts, int flush, int16_t *out, size_t cap, int64_t *pts_out);
int maac_ready(const maac *a);    // packets whose PCM is there to hand out (maac_decode with n == 0 gives them)
int maac_queued(const maac *a);   // packets fed whose PCM is not handed out yet (the end of stream hands out this many)
void maac_drop(maac *a);          // Flush (a seek): what the decoder holds belongs to the old position; its own flush waits for the next maac_decode
void maac_free(maac *a);

// What the decoders did, for the unit test
typedef struct {
    char codec[64];                                // the MediaCodec component, empty when there is none
    int failed;                                    // fell back to black pictures / silence
    double open_ms;                                // how long making, configuring and starting the decoder took
    int nfmt, cfmt, w, h, stride, slice, crop[4];  // H.264: output format changes, the last one's colour format, size, stride, slice height, crop (x, y, w, h)
    int matrix, full, image_data, ps_sent;         // H.264: the RGBA conversion's matrix (601, 709, 2020) and range; whether the plane layout came from "image-data"; times the parameter sets went in band
    int rate, ch, frames, orate, och, lag;         // AAC: the PCM handed out, frames per packet, the decoder's own rate / channels, packets it holds back
    int late, padded;                              // AAC: packets whose PCM came after their own call; frames of silence put in for PCM the decoder never gave
    int skipped, stale;                            // units the decoder passed over (black / silence in their place); outputs for a time no longer waited for (dropped), AAC: or with a surplus
    int surplus;                                   // AAC: the decoder's frames beyond one packet's worth for a packet's time (dropped)
    int64_t nin, nout;                             // units queued, output buffers taken
} mmc_info;
// Live objects, for the unit test: H.264 / AAC decoders made and not freed, MediaCodec codecs created and not deleted, helper job records not freed, helper threads not finished.
// Each is back to its starting value once every decoder is freed and its helpers are done (a failed decoder's helper may still be stopping its codec for a while)
enum { MMC_LIVE_MAVC, MMC_LIVE_MAAC, MMC_LIVE_CODEC, MMC_LIVE_JOB, MMC_LIVE_THREAD, MMC_LIVE_N };
void mmc_live_add(int what, int d);
void mmc_live_get(int out[MMC_LIVE_N]);
struct mavc;
void mavc_mc_info(const struct mavc *a, mmc_info *o);
void maac_info(const maac *a, mmc_info *o);

// ---------- shared (movie_mc.c) ----------
// MediaCodec's decoders answer over binder (the Codec2 HAL calls back into this process): an app has binder threads, an adb executable (dmhost, the tests) has none, and there no picture or
// PCM ever comes and stop() times out. Both decoders call this before making theirs: it starts the binder thread pool once, a no-op in an app
void mmc_binder_threads(void);
// The MediaCodec calls that can wait on the codec's process (create / configure / start, flush, stop / delete) run on a helper thread, the decoder's thread waiting a bounded time: when the time
// runs out the helper keeps the codec and stops and deletes it once the call returns, and the decoder falls back. mmc_open takes f (deleted either way): the started codec, or NULL with the status
// in *st (MMC_TIMEOUT when the time ran out) and the component's name in name. mmc_flush: AMEDIA_OK, an error (the caller still owns the codec), or MMC_TIMEOUT (the codec is no longer the
// caller's). mmc_close: the codec is no longer the caller's; 1 if it was stopped and deleted within wait_us (0: do not wait)
struct AMediaCodec; struct AMediaFormat;
enum { MMC_TIMEOUT = -0x7ffffff0 };
struct AMediaCodec *mmc_open(const char *mime, struct AMediaFormat *f, int64_t wait_us, int *st, char *name, size_t nn);
int mmc_flush(struct AMediaCodec *mc, int64_t wait_us);
int mmc_close(struct AMediaCodec *mc, int64_t wait_us);
static inline int64_t mmc_now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
// DM_MOVIE_MC_FAIL=<what> fails <what>'s decoder creation, <what>@N its N-th unit (counted from 1): -1 none, 0 creation, N
static inline int mmc_fail_at(const char *what) {
    const char *e = getenv("DM_MOVIE_MC_FAIL"); size_t n = strlen(what);
    if (!e || strncmp(e, what, n)) return -1;
    return !e[n] ? 0 : e[n] == '@' && atoi(e + n + 1) > 0 ? atoi(e + n + 1) : -1;
}
