// SHA-256 (FIPS 180-4), portable C, for the platforms without CommonCrypto (plat.h maps the CC_SHA256 names to it on Linux).
// Written after Brad Conte's public-domain sha256.c (https://github.com/B-Con/crypto-algorithms); this file is public domain as well.
// Checked against CommonCrypto on the Mac at message lengths 0, 1, 55, 56, 63, 64, 65, 119, 1000 and 1 MB, and fed in uneven pieces (plat/tests/t_sha256.c)
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct { uint32_t h[8]; uint64_t bits; uint8_t buf[64]; uint32_t n; } dm_sha256_ctx;

#define DM_SHA_ROR(x, k) (((x) >> (k)) | ((x) << (32 - (k))))
static inline void dm_sha256_block(dm_sha256_ctx *c, const uint8_t *p) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64], a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = DM_SHA_ROR(w[i - 15], 7) ^ DM_SHA_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = DM_SHA_ROR(w[i - 2], 17) ^ DM_SHA_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (DM_SHA_ROR(e, 6) ^ DM_SHA_ROR(e, 11) ^ DM_SHA_ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (DM_SHA_ROR(a, 2) ^ DM_SHA_ROR(a, 13) ^ DM_SHA_ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}
#undef DM_SHA_ROR
static inline int dm_sha256_init(dm_sha256_ctx *c) {
    static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, H0, sizeof H0); c->bits = 0; c->n = 0; return 1;
}
static inline int dm_sha256_update(dm_sha256_ctx *c, const void *data, size_t len) {
    const uint8_t *p = data; c->bits += (uint64_t)len * 8;
    if (c->n) { size_t k = 64 - c->n < len ? 64 - c->n : len; memcpy(c->buf + c->n, p, k); c->n += (uint32_t)k; p += k; len -= k; if (c->n < 64) return 1; dm_sha256_block(c, c->buf); c->n = 0; }
    for (; len >= 64; p += 64, len -= 64) dm_sha256_block(c, p);
    memcpy(c->buf, p, len); c->n = (uint32_t)len; return 1;
}
static inline int dm_sha256_final(uint8_t out[32], dm_sha256_ctx *c) {   // CommonCrypto's argument order: digest first
    uint64_t bits = c->bits; uint8_t pad = 0x80, z[64] = {0}, len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    dm_sha256_update(c, &pad, 1);
    dm_sha256_update(c, z, c->n <= 56 ? 56 - c->n : 120 - c->n);
    dm_sha256_update(c, len, 8);
    for (int i = 0; i < 8; i++) { out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16); out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i]; }
    return 1;
}
static inline uint8_t *dm_sha256(const void *data, size_t len, uint8_t out[32]) { dm_sha256_ctx c; dm_sha256_init(&c); dm_sha256_update(&c, data, len); dm_sha256_final(out, &c); return out; }
