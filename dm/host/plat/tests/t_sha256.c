// The vendored SHA-256 (plat/sha256.h) against CommonCrypto on the Mac, whole and in uneven pieces; elsewhere against known digests only.
// clang -O1 -Wall -o t_sha256 t_sha256.c && ./t_sha256
#include "../sha256.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

static int fails;
static void hex(const uint8_t *d, char *o) { for (int i = 0; i < 32; i++) sprintf(o + 2 * i, "%02x", d[i]); }
static void check(const uint8_t *msg, size_t n) {
    uint8_t a[32], b[32]; dm_sha256(msg, n, a);
    dm_sha256_ctx c; dm_sha256_init(&c);   // uneven pieces: 1, 2, 3, ... bytes
    for (size_t o = 0, k = 1; o < n; o += k, k++) dm_sha256_update(&c, msg + o, k < n - o ? k : n - o);
    dm_sha256_final(b, &c);
    if (memcmp(a, b, 32)) { printf("FAIL length %zu: pieces differ from whole\n", n); fails++; }
#ifdef __APPLE__
    uint8_t r[32]; CC_SHA256(msg, (CC_LONG)n, r);
    if (memcmp(a, r, 32)) { char x[65], y[65]; hex(a, x); hex(r, y); printf("FAIL length %zu: %s, CommonCrypto %s\n", n, x, y); fails++; }
#endif
}
int main(void) {
    static const struct { const char *msg, *hex; } kv[] = {   // FIPS 180-4 examples
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"}};
    for (int i = 0; i < 3; i++) { uint8_t d[32]; char x[65]; hex(dm_sha256(kv[i].msg, strlen(kv[i].msg), d), x); if (strcmp(x, kv[i].hex)) { printf("FAIL \"%s\": %s\n", kv[i].msg, x); fails++; } }
    size_t big = 1 << 20; uint8_t *m = malloc(big); uint32_t s = 1;
    for (size_t i = 0; i < big; i++) { s = s * 1103515245u + 12345u; m[i] = (uint8_t)(s >> 16); }
    static const size_t lens[] = {0, 1, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 129, 1000, 4096, 1 << 20};
    for (size_t i = 0; i < sizeof lens / sizeof *lens; i++) check(m, lens[i]);
    free(m);
    printf("t_sha256: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
