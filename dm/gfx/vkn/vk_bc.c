// BC-compressed textures (NVN 0x41-0x50: DXT1/3/5 = BC1-3, RGTC = BC4/5, BPTC = BC7 and BC6H). A device with textureCompressionBC samples them as they are (the Vulkan BC formats,
// uploaded block for block). Mali GPUs (the RG 476H's G57) have none: there each is decoded on the CPU as it is uploaded (bcdec, dm/third_party/bcdec) into RGBA8 (sRGB for the sRGB
// formats), R8 / RG8 (BC4 / BC5, signed where they are) or RGBA16F (BC6H). Paper Mario: The Thousand-Year Door's materials are BC: without them every material texture read zeros and its
// alpha-tested G-buffer pass wrote nothing. DM_VK_BCDEC=0: no decode (such textures have no host image, as before)
#include "vkn.h"
#include "nvn_fmt_vk.h"
#include <stdlib.h>
#include <string.h>
#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#include "../../third_party/bcdec/bcdec.h"

typedef struct { uint16_t nvn; VkFormat native, decoded; uint8_t bpp, block; } bc_fmt;   // bpp: bytes per decoded pixel; block: bytes per 4x4 block
static const bc_fmt B[] = {
    {0x41, VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, 4, 8}, {0x42, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, 4, 8},
    {0x43, VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, 4, 16}, {0x44, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, 4, 16},
    {0x45, VK_FORMAT_BC1_RGB_SRGB_BLOCK, VK_FORMAT_R8G8B8A8_SRGB, 4, 8}, {0x46, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, VK_FORMAT_R8G8B8A8_SRGB, 4, 8},
    {0x47, VK_FORMAT_BC2_SRGB_BLOCK, VK_FORMAT_R8G8B8A8_SRGB, 4, 16}, {0x48, VK_FORMAT_BC3_SRGB_BLOCK, VK_FORMAT_R8G8B8A8_SRGB, 4, 16},
    {0x49, VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_R8_UNORM, 1, 8}, {0x4a, VK_FORMAT_BC4_SNORM_BLOCK, VK_FORMAT_R8_SNORM, 1, 8},
    {0x4b, VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_R8G8_UNORM, 2, 16}, {0x4c, VK_FORMAT_BC5_SNORM_BLOCK, VK_FORMAT_R8G8_SNORM, 2, 16},
    {0x4d, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM, 4, 16}, {0x4e, VK_FORMAT_BC7_SRGB_BLOCK, VK_FORMAT_R8G8B8A8_SRGB, 4, 16},
    {0x4f, VK_FORMAT_BC6H_SFLOAT_BLOCK, VK_FORMAT_R16G16B16A16_SFLOAT, 8, 16}, {0x50, VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_R16G16B16A16_SFLOAT, 8, 16},
};
static const bc_fmt *bc_of(int nf) { for (size_t i = 0; i < sizeof B / sizeof *B; i++) if (B[i].nvn == nf) return &B[i]; return NULL; }
static int decode_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("DM_VK_BCDEC"); on = !(e && *e == '0'); } return on; }

// The format entry of a BC format (nvn_fmt_vk's fallback): the native format where the device has BC, else the decoded one; NULL when it is no BC format, or the decode is off
const nvk_fmt *vkn_bc_nvk(int nf) {
    static nvk_fmt E[sizeof B / sizeof *B]; static int said;
    const bc_fmt *b = bc_of(nf); if (!b) return NULL;
    if (!vd.bc_native && !decode_on()) return NULL;
    if (!said) { said = 1; VLOG("BC textures: %s", vd.bc_native ? "sampled natively (textureCompressionBC)" : "decoded on the CPU at upload (the device has no textureCompressionBC; DM_VK_BCDEC=0 turns it off)"); }
    nvk_fmt *e = &E[b - B]; e->nvn = (uint16_t)nf; e->flags = NVK_UP; e->f[0] = vd.bc_native ? b->native : b->decoded; e->f[1] = e->f[2] = 0; e->lin = 0;
    return e;
}
// 1 = the texture's format is decoded on the CPU; *bpp its decoded bytes per pixel
int vkn_bc_decoded(int nf, int *bpp) {
    const bc_fmt *b = bc_of(nf);
    if (!b || vd.bc_native || !decode_on()) return 0;
    if (bpp) *bpp = b->bpp;
    return 1;
}
static void bc_block(int nf, const uint8_t *b, uint8_t *o, int op) {   // one block → 4x4 pixels, op bytes per output row
    switch (nf) {
    case 0x41: case 0x42: case 0x45: case 0x46: bcdec_bc1(b, o, op); break;
    case 0x43: case 0x47: bcdec_bc2(b, o, op); break;
    case 0x44: case 0x48: bcdec_bc3(b, o, op); break;
    case 0x49: bcdec_bc4(b, o, op, 0); break;
    case 0x4a: bcdec_bc4(b, o, op, 1); break;
    case 0x4b: bcdec_bc5(b, o, op, 0); break;
    case 0x4c: bcdec_bc5(b, o, op, 1); break;
    case 0x4d: case 0x4e: bcdec_bc7(b, o, op); break;
    case 0x4f: case 0x50: {   // bcdec gives RGB half floats: alpha 1.0
        uint16_t rgb[16 * 3]; bcdec_bc6h_half(b, rgb, 4 * 3, nf == 0x4f);
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { uint16_t *d = (uint16_t *)(void *)(o + y * op) + x * 4; const uint16_t *s = rgb + (y * 4 + x) * 3; d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 0x3c00; }
        break; }
    }
}
// Decode a w x h level: src block rows pitch bytes apart, dst rows dpitch bytes apart. 0 = done
int vkn_bc_decode(int nf, const uint8_t *src, size_t pitch, uint32_t w, uint32_t h, uint8_t *dst, size_t dpitch) {
    const bc_fmt *f = bc_of(nf); if (!f) return -1;
    uint8_t tmp[4 * 4 * 8];
    for (uint32_t by = 0; by * 4 < h; by++) for (uint32_t bx = 0; bx * 4 < w; bx++) {
        const uint8_t *b = src + (size_t)by * pitch + (size_t)bx * f->block;
        uint32_t cw = w - bx * 4 < 4 ? w - bx * 4 : 4, ch = h - by * 4 < 4 ? h - by * 4 : 4;
        uint8_t *o = dst + (size_t)by * 4 * dpitch + (size_t)bx * 4 * f->bpp;
        if (cw == 4 && ch == 4) { bc_block(nf, b, o, (int)dpitch); continue; }
        bc_block(nf, b, tmp, 4 * f->bpp);   // a block at the edge: decoded whole, its part copied
        for (uint32_t y = 0; y < ch; y++) memcpy(o + y * dpitch, tmp + y * 4 * f->bpp, (size_t)cw * f->bpp);
    }
    return 0;
}
