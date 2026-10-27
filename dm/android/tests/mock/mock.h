#pragma once
#include <stdint.h>
typedef struct {
    int F_dec, orate, och;           // AAC: frames per packet the fake gives, its rate and channels
    int delay;                       // AAC: a packet's PCM comes out once this many more packets went in
    int skip[8], nskip;              // units passed over (no output at all)
    int first_short;                 // AAC: the first output this many frames short (a decoder removing its delay)
    int pts_mode;                    // AAC: 1 = every output time 0 (times not kept)
    int split;                       // AAC: each packet's PCM in two buffers with the same time
    int over_id, over_mul;           // AAC: this packet gives over_mul times its PCM
    int tiny_id;                     // AAC: this packet also gives a 2-byte piece first
    int swap_id;                     // AAC: this packet's PCM comes after the next packet's
    int dup_id;                      // a second output for this unit
    int dup_parts;                   // AAC: that second output in this many buffers (0: one)
    int fmt_at, fmt_och;             // AAC: before the fmt_at-th PCM buffer (from 1) the output format changes to fmt_och channels (that buffer and the later ones have them)
    int late_id;                     // H.264: this picture comes after the next one
    int err_at;                      // AAC: the decoder errs once this packet went in (-1 none)
    int flush_sleep_ms, stop_sleep_ms, create_fail, eos_lost;
    int vw, vh, vdepth;              // H.264: picture size, reorder window
    int flushes;                     // flushes done (counted by the fake)
} mock_cfg;
extern mock_cfg mk;
extern int mk_live;
static inline int16_t aval(int id) { return (int16_t)(100 * (id + 1)); }
static inline uint8_t vval(int id) { return (uint8_t)(40 + (id * 7) % 180); }
