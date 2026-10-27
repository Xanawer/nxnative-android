// A scripted fake of MediaCodec (the NDK API subset in inc/media), for running dm/android/media's decoders on the Mac: synchronous, in process.
// AAC: a packet's first two bytes are its id; its PCM is mk.F_dec frames of mk.och channels at the value aval(id), with the input's time (or as mk scripts: held back, passed over, swapped,
// split, too long, a sub-frame piece, an error, a slow flush, a format change). H.264: the VCL NAL's two bytes after its header are the id; its picture is mk.vw × mk.vh NV12 with Y = vval(id), output in
// presentation order behind a reorder window of mk.vdepth (or passed over / swapped / doubled).
#include "inc/media/NdkMediaCodec.h"
#include "mock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

mock_cfg mk;
int mk_live;   // codecs not deleted
void *mock_dlopen(const char *p, int m) { (void)p; (void)m; return (void *)1; }
static bool started(void) { return true; }
void *mock_dlsym(void *h, const char *n) { (void)h; return !strcmp(n, "ABinderProcess_isThreadPoolStarted") ? (void *)started : NULL; }

// ---------- AMediaFormat ----------
typedef struct { char k[48]; int type; int32_t i[4]; void *b; size_t n; } kv;
struct AMediaFormat { kv e[32]; int n; };
AMediaFormat *AMediaFormat_new(void) { return calloc(1, sizeof(AMediaFormat)); }
media_status_t AMediaFormat_delete(AMediaFormat *f) { for (int i = 0; i < f->n; i++) free(f->e[i].b); free(f); return AMEDIA_OK; }
static kv *slot(AMediaFormat *f, const char *k) { for (int i = 0; i < f->n; i++) if (!strcmp(f->e[i].k, k)) return &f->e[i]; kv *e = &f->e[f->n++]; memset(e, 0, sizeof *e); snprintf(e->k, sizeof e->k, "%s", k); return e; }
static kv *find(AMediaFormat *f, const char *k, int type) { for (int i = 0; i < f->n; i++) if (!strcmp(f->e[i].k, k) && f->e[i].type == type) return &f->e[i]; return NULL; }
void AMediaFormat_setInt32(AMediaFormat *f, const char *k, int32_t v) { kv *e = slot(f, k); e->type = 1; e->i[0] = v; }
void AMediaFormat_setString(AMediaFormat *f, const char *k, const char *v) { kv *e = slot(f, k); e->type = 2; free(e->b); e->b = strdup(v); }
void AMediaFormat_setBuffer(AMediaFormat *f, const char *k, const void *d, size_t n) { kv *e = slot(f, k); e->type = 3; free(e->b); e->b = malloc(n ? n : 1); memcpy(e->b, d, n); e->n = n; }
void AMediaFormat_setRect(AMediaFormat *f, const char *k, int32_t l, int32_t t, int32_t r, int32_t b) { kv *e = slot(f, k); e->type = 4; e->i[0] = l; e->i[1] = t; e->i[2] = r; e->i[3] = b; }
bool AMediaFormat_getInt32(AMediaFormat *f, const char *k, int32_t *v) { kv *e = find(f, k, 1); if (e) *v = e->i[0]; return e; }
bool AMediaFormat_getBuffer(AMediaFormat *f, const char *k, void **d, size_t *n) { kv *e = find(f, k, 3); if (e) { *d = e->b; *n = e->n; } return e; }
bool AMediaFormat_getRect(AMediaFormat *f, const char *k, int32_t *l, int32_t *t, int32_t *r, int32_t *b) { kv *e = find(f, k, 4); if (e) { *l = e->i[0]; *t = e->i[1]; *r = e->i[2]; *b = e->i[3]; } return e; }

// ---------- the codec ----------
typedef struct { uint8_t *d; size_t n; int64_t pts; uint32_t flags; int id, fmt_before; long due; } ob;   // fmt_before: a format change is given just before it
struct AMediaCodec {
    int video, fmt_sent, err, nq_in, n_out, npcm, och_out;   // npcm: PCM buffers made; och_out: the channels getOutputFormat reports
    uint8_t in[4][1 << 16];
    ob held[512]; int nheld;    // decoded, not given yet
    ob ready[512]; int nready;  // given out in this order
    ob given[16]; int busy[16];
    ob swap_wait; int has_swap; ob late_wait; int has_late, late_armed;
};
static int is_skip(int id) { for (int i = 0; i < mk.nskip; i++) if (mk.skip[i] == id) return 1; return 0; }
static void put_ready(AMediaCodec *c, ob o) { if (c->nready < 512) c->ready[c->nready++] = o; else free(o.d); }
static ob pcm(AMediaCodec *c, int id, int64_t pts, size_t fr, int16_t v) {
    int n = ++c->npcm, och = mk.fmt_at && n >= mk.fmt_at ? mk.fmt_och : mk.och;
    ob o = {0}; o.n = fr * 2 * (size_t)och; o.d = malloc(o.n ? o.n : 1); o.pts = mk.pts_mode == 1 ? 0 : pts; o.id = id; o.fmt_before = mk.fmt_at && n == mk.fmt_at;
    for (size_t i = 0; i < fr * (size_t)och; i++) ((int16_t *)o.d)[i] = v;
    return o;
}
static ob picture(int id, int64_t pts) {
    ob o = {0}; size_t ys = (size_t)mk.vw * mk.vh; o.n = ys * 3 / 2; o.d = malloc(o.n); o.pts = pts; o.id = id;
    memset(o.d, vval(id), ys); memset(o.d + ys, 128, ys / 2);
    return o;
}
AMediaCodec *AMediaCodec_createDecoderByType(const char *mime) {
    if (mk.create_fail) return NULL;
    AMediaCodec *c = calloc(1, sizeof *c); c->video = !strcmp(mime, "video/avc"); c->och_out = mk.och; mk_live++; return c;
}
media_status_t AMediaCodec_getName(AMediaCodec *c, char **out) { *out = strdup(c->video ? "mock.avc.decoder" : "mock.aac.decoder"); return AMEDIA_OK; }
void AMediaCodec_releaseName(AMediaCodec *c, char *n) { (void)c; free(n); }
media_status_t AMediaCodec_configure(AMediaCodec *c, const AMediaFormat *f, void *s, void *cr, uint32_t fl) { (void)c; (void)f; (void)s; (void)cr; (void)fl; return AMEDIA_OK; }
media_status_t AMediaCodec_start(AMediaCodec *c) { (void)c; return AMEDIA_OK; }
media_status_t AMediaCodec_stop(AMediaCodec *c) { (void)c; if (mk.stop_sleep_ms) usleep((useconds_t)mk.stop_sleep_ms * 1000); return AMEDIA_OK; }
static void clear(AMediaCodec *c) {
    for (int i = 0; i < c->nheld; i++) free(c->held[i].d);
    for (int i = 0; i < c->nready; i++) free(c->ready[i].d);
    if (c->has_swap) free(c->swap_wait.d);
    if (c->has_late) free(c->late_wait.d);
    c->nheld = c->nready = c->has_swap = c->has_late = c->late_armed = 0;
}
media_status_t AMediaCodec_delete(AMediaCodec *c) { clear(c); for (int i = 0; i < 16; i++) if (c->busy[i]) free(c->given[i].d); free(c); mk_live--; return AMEDIA_OK; }
media_status_t AMediaCodec_flush(AMediaCodec *c) { if (mk.flush_sleep_ms) usleep((useconds_t)mk.flush_sleep_ms * 1000); clear(c); mk.flushes++; return AMEDIA_OK; }
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec *c, int64_t t) { (void)c; (void)t; return 0; }
uint8_t *AMediaCodec_getInputBuffer(AMediaCodec *c, size_t i, size_t *n) { *n = sizeof c->in[0]; return c->in[i]; }
// pictures in presentation order behind the reorder window: the smallest time leaves once more than vdepth are held (all at the end of stream)
static void vrelease(AMediaCodec *c, int all) {
    while (c->nheld > (all ? 0 : mk.vdepth)) {
        int m = 0; for (int i = 1; i < c->nheld; i++) if (c->held[i].pts < c->held[m].pts) m = i;
        ob o = c->held[m]; c->held[m] = c->held[--c->nheld];
        if (o.id == mk.late_id && !c->has_late) { c->late_wait = o; c->has_late = 1; continue; }   // held back past the next picture
        put_ready(c, o);
        if (o.id == mk.dup_id) { ob d = o; d.d = malloc(o.n); memcpy(d.d, o.d, o.n); put_ready(c, d); }
        if (c->has_late && o.id != mk.late_id) { put_ready(c, c->late_wait); c->has_late = 0; }
    }
    if (all && c->has_late) { put_ready(c, c->late_wait); c->has_late = 0; }
}
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec *c, size_t i, off_t off, size_t n, uint64_t t, uint32_t flags) {
    (void)off;
    const uint8_t *p = c->in[i];
    if (n && c->video) {
        int id = -1;
        for (size_t k = 0; k + 6 < n; k++) if (!p[k] && !p[k + 1] && !p[k + 2] && p[k + 3] == 1 && ((p[k + 4] & 31) == 1 || (p[k + 4] & 31) == 5)) { id = p[k + 5] | p[k + 6] << 8; break; }
        if (id >= 0 && !is_skip(id)) { c->held[c->nheld++] = picture(id, (int64_t)t); vrelease(c, 0); }
    } else if (n) {
        int id = p[0] | p[1] << 8; c->nq_in++;
        if (mk.err_at >= 0 && id == mk.err_at) c->err = 1;
        if (!is_skip(id)) {
            size_t fr = (size_t)mk.F_dec;
            if (!c->n_out++ && mk.first_short) fr -= (size_t)mk.first_short;
            if (id == mk.over_id) fr *= (size_t)mk.over_mul;
            if (id == mk.tiny_id) { ob o = pcm(c, id, (int64_t)t, 0, 0); o.n = 2; o.due = c->nq_in + mk.delay; c->held[c->nheld++] = o; }   // a piece shorter than one frame
            int parts = mk.split ? 2 : 1;
            for (int q = 0; q < parts; q++) { ob o = pcm(c, id, (int64_t)t, fr / (size_t)parts, aval(id)); o.due = c->nq_in + mk.delay; c->held[c->nheld++] = o; }
            if (id == mk.dup_id) { int dp = mk.dup_parts > 1 ? mk.dup_parts : 1; for (int q = 0; q < dp; q++) { ob o = pcm(c, id, (int64_t)t, fr / (size_t)dp, aval(id)); o.due = c->nq_in + mk.delay; c->held[c->nheld++] = o; } }
        }
        // what is due leaves in order; swap_id's PCM waits for the next packet's
        int k = 0;
        for (int j = 0; j < c->nheld; j++) {
            ob o = c->held[j];
            if (o.due > c->nq_in) { c->held[k++] = o; continue; }
            if (o.id == mk.swap_id && !c->has_swap) { c->swap_wait = o; c->has_swap = 1; continue; }
            put_ready(c, o);
            if (c->has_swap && o.id != mk.swap_id) { put_ready(c, c->swap_wait); c->has_swap = 0; }
        }
        c->nheld = k;
    }
    if (flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
        if (c->video) vrelease(c, 1);
        else { for (int j = 0; j < c->nheld; j++) put_ready(c, c->held[j]); c->nheld = 0; if (c->has_swap) { put_ready(c, c->swap_wait); c->has_swap = 0; } }
        if (!mk.eos_lost) { ob e = {0}; e.d = malloc(1); e.pts = (int64_t)t; e.flags = AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM; put_ready(c, e); }
    }
    return AMEDIA_OK;
}
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec *c, AMediaCodecBufferInfo *info, int64_t t) {
    if (!c->fmt_sent) { c->fmt_sent = 1; return AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED; }
    if (c->err && !c->nready) return AMEDIA_ERROR_UNKNOWN;
    if (c->nready && c->ready[0].fmt_before) { c->ready[0].fmt_before = 0; c->och_out = mk.fmt_och; return AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED; }
    if (!c->nready) { if (t > 0) usleep((useconds_t)(t < 10000 ? t : 10000)); return AMEDIACODEC_INFO_TRY_AGAIN_LATER; }
    int s = -1; for (int i = 0; i < 16; i++) if (!c->busy[i]) { s = i; break; }
    if (s < 0) return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    c->given[s] = c->ready[0]; c->busy[s] = 1; memmove(c->ready, c->ready + 1, (size_t)--c->nready * sizeof c->ready[0]);
    info->offset = 0; info->size = (int32_t)(c->given[s].flags ? 0 : c->given[s].n); info->presentationTimeUs = c->given[s].pts; info->flags = c->given[s].flags;
    return s;
}
uint8_t *AMediaCodec_getOutputBuffer(AMediaCodec *c, size_t i, size_t *n) { *n = c->given[i].n; return c->given[i].d; }
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec *c, size_t i, bool r) { (void)r; free(c->given[i].d); c->busy[i] = 0; return AMEDIA_OK; }
AMediaFormat *AMediaCodec_getOutputFormat(AMediaCodec *c) {
    AMediaFormat *f = AMediaFormat_new();
    if (c->video) {
        AMediaFormat_setInt32(f, "width", mk.vw); AMediaFormat_setInt32(f, "height", mk.vh); AMediaFormat_setInt32(f, "stride", mk.vw); AMediaFormat_setInt32(f, "slice-height", mk.vh);
        AMediaFormat_setInt32(f, "color-format", 21); AMediaFormat_setRect(f, "crop", 0, 0, mk.vw - 1, mk.vh - 1);
    } else { AMediaFormat_setInt32(f, "sample-rate", mk.orate); AMediaFormat_setInt32(f, "channel-count", c->och_out); }
    return f;
}
