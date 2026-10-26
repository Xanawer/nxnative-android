// Image atomics on a buffer image (SHIN MEGAMI TENSEI V's compute shaders; Unreal's cp_i_cb2_4C_r32ui_atomic): the image is declared as a buffer (nvn_refl.m, a texture_buffer
// img binding), so the admission (imga_extent) takes a bound buffer texture, and an unbound one gets a writable buffer dummy (img_binding) whose extent the unbound atomics' own scratch holds
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

int main(void) { @autoreleasepool {
    mtl_init();
    nvn_refl r = {0}; char err[256] = "";
    NSDictionary *j = @{@"route": @"B", @"bindings": @[
        @{@"kind": @"img", @"reg": @2, @"arg_id": @0, @"msl_name": @"cp_i_cb2_4C_r32ui", @"msl_type": @"texture_buffer<uint, access::read_write>"},
        @{@"kind": @"img", @"reg": @2, @"arg_id": @1, @"msl_name": @"cp_i_cb2_4C_r32ui_atomic", @"msl_type": @"device atomic_uint*"},
        @{@"kind": @"img", @"reg": @3, @"arg_id": @2, @"msl_name": @"cp_i_cb3_4C_r32ui", @"msl_type": @"texture2d<uint, access::read_write>"},
        @{@"kind": @"img", @"reg": @3, @"arg_id": @3, @"msl_name": @"cp_i_cb3_4C_r32ui_atomic", @"msl_type": @"device atomic_uint*"}]};
    CHECK(!nvn_refl_from_json(j, &r, err, sizeof err), "reflection converts");
    CHECK(r.nb == 4 && r.b[0].kind == NVN_RB_IMG && r.b[1].kind == NVN_RB_IMGA && r.b[2].kind == NVN_RB_IMG && r.b[3].kind == NVN_RB_IMGA, "two image + atomics pairs");
    CHECK(r.b[0].dtt == (uint8_t)MTLTextureTypeTextureBuffer && r.b[0].dpf == NVN_DPF_UINT && r.b[0].rw, "a buffer image is declared as a writable uint buffer");
    CHECK(r.b[2].dtt == (uint8_t)MTLTextureType2D, "a 2D image is still declared as 2D");

    // A bound R32Uint buffer texture of 256 texels: admitted against the buffer image, with its whole extent; refused against the 2D one
    id<MTLBuffer> mb = [g_dev newBufferWithLength:256 * 4 options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:256 resourceOptions:mb.resourceOptions
                                                                                    usage:MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite];
    id<MTLTexture> bt = [mb newTextureWithDescriptor:d offset:0 bytesPerRow:256 * 4];
    uint64_t need = 0;
    CHECK(bt && imga_extent(bt, &r.b[0], &need) && need == 256 * 4, "a bound buffer texture is admitted with its 1,024 bytes");
    CHECK(!imga_extent(bt, &r.b[2], &need), "and refused against a 2D image");

    // Unbound: a writable buffer dummy (not dummy_img's 1D one, whose cache index the buffer type falls on), admitted at one texel, which the unbound storage holds
    os_unfair_lock_lock(&g_rmu);
    uint64_t rid = 0, rid2 = 0; int real = 1, real2 = 1;
    id<MTLTexture> t = img_binding(0, &r.b[0], &rid, &real), t2 = img_binding(0, &r.b[2], &rid2, &real2);
    id<MTLTexture> one = dummy_img(MTLTextureType1D, NVN_DPF_UINT);
    os_unfair_lock_unlock(&g_rmu);
    CHECK(t && !real && t.textureType == MTLTextureTypeTextureBuffer && (t.usage & MTLTextureUsageShaderWrite) && rid == t.gpuResourceID._impl, "an unbound buffer image gets a writable buffer dummy");
    CHECK(imga_extent(t, &r.b[0], &need) && need == 4 && nvn_imga_capacity(need, dummy_atom_buf().length, 0), "admitted at 4 bytes, which the unbound atomics' scratch holds");
    CHECK(dummy_atom_buf() != dummy_buf() && dummy_atom_buf().gpuAddress, "unbound atomics have a scratch of their own, not the buffer unbound buffers read as 0");
    CHECK(t2 && !real2 && t2.textureType == MTLTextureType2D && rid2 == t2.gpuResourceID._impl && imga_extent(t2, &r.b[2], &need) && need == 4, "an unbound 2D image still gets a 2D dummy");
    CHECK(one && one.textureType == MTLTextureType1D && one != t, "the 1D dummy is unchanged");
    printf("t_imga_tbuf: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
