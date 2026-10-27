// Descriptor delivery for the draws' one set (nvn_vk.c "Descriptors"), chosen once at start: push descriptors (VK_KHR_push_descriptor, the set lives in the command buffer) where the
// device has them, else sets allocated from per-submission-slot descriptor pools and bound (Mali's proprietary driver, r40p0 on the Mali-G57, has no push descriptors: without this nothing
// was drawn there). Both deliver the same writes; only how they reach the command buffer differs, so the rest of the backend does not know which one runs.
// DM_VK_PUSH=0 takes the pools on a device that has push descriptors (the fallback, tested where it is not the default); =1 asks for push descriptors and says in red when the device lacks
// them (the pools then). One line at start says which one runs and why.
// Pools: each submission slot has a chain of pools, reset as a whole when the slot begins (mtl_sub_begin: its last submission has completed, so none of its sets is still read). A pool that
// runs out (VK_ERROR_OUT_OF_POOL_MEMORY / FRAGMENTED_POOL) gets a successor in the chain, kept for later submissions of that slot. Submit thread only (hle_nvn.c serializes submissions).
// vkResetDescriptorPool is loaded here (vkGetDeviceProcAddr), not added to vkn.h's table: this file is the only user
#include "vkn.h"
#include "vk_desc.h"
#include <stdlib.h>
#include <string.h>

#define VKD_SLOTS 4                    // nvn_vk.c's NSUB: a slot index is taken modulo this
#define VKD_SETS 1024                  // sets per pool; a frame of Stardew Valley draws about 12
#define VKD_PER_SET 16                 // descriptors of each type per set the pool is sized for (a pair has at most 2 * VKN_MAXB; the chain grows past it)
typedef struct { VkDescriptorPool p[64]; int n, cur; } vkd_chain;
static vkd_chain g_ch[VKD_SLOTS];
static int g_mode = -1, g_slot;        // g_mode: VKD_PUSH or VKD_POOL once vkd_init ran (-1 before)
static PFN_vkResetDescriptorPool g_reset;
static _Atomic uint64_t g_sets, g_pools, g_fail, g_alloc_ns, g_upd_ns, g_ndesc;   // CPU time in vkAllocateDescriptorSets / vkUpdateDescriptorSets + bind, descriptors written
uint64_t now_ns(void);

static VkDescriptorPool pool_make(void) {
    VkDescriptorPoolSize sz[6] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VKD_SETS * VKD_PER_SET}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VKD_SETS * VKD_PER_SET}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VKD_SETS * VKD_PER_SET}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VKD_SETS * VKD_PER_SET},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VKD_SETS * 64}, {VK_DESCRIPTOR_TYPE_SAMPLER, VKD_SETS * 64}};   // the dynamic handle arrays (nvn_vk.c draw_dyn): 32 each
    VkDescriptorPoolCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, VKD_SETS, 6, sz};
    VkDescriptorPool p = VK_NULL_HANDLE;
    if (vk.vkCreateDescriptorPool(vd.dev, &ci, NULL, &p)) return VK_NULL_HANDLE;
    atomic_fetch_add(&g_pools, 1); return p;
}

int vkd_init(void) {
    const char *e = getenv("DM_VK_PUSH"); int want = e && *e ? atoi(e) : -1;
    if (want == 0) { g_mode = VKD_POOL; VLOG("descriptors: per-slot descriptor pools (DM_VK_PUSH=0; the device %s push descriptors)", vd.push ? "has" : "has no"); }
    else if (vd.push && vk.vkCmdPushDescriptorSetKHR) { g_mode = VKD_PUSH; VLOG("descriptors: push descriptors (VK_KHR_push_descriptor, up to %u)", vd.max_push); }
    else {
        if (want == 1) VLOG("[RED] DM_VK_PUSH=1, but the device has no VK_KHR_push_descriptor: per-slot descriptor pools instead");
        g_mode = VKD_POOL; VLOG("descriptors: per-slot descriptor pools (the device has no VK_KHR_push_descriptor)");
    }
    if (g_mode == VKD_PUSH) return 1;
    g_reset = (PFN_vkResetDescriptorPool)vk.vkGetDeviceProcAddr(vd.dev, "vkResetDescriptorPool");
    if (!g_reset) { VLOG("[RED] no vkResetDescriptorPool: nothing is drawn"); g_mode = -1; return 0; }
    for (int s = 0; s < VKD_SLOTS; s++) {
        if (!(g_ch[s].p[0] = pool_make())) { VLOG("[RED] the descriptor pools cannot be made: nothing is drawn"); g_mode = -1; return 0; }
        g_ch[s].n = 1;
    }
    return 1;
}
int vkd_mode(void) { return g_mode; }
uint32_t vkd_layout_flags(void) { return g_mode == VKD_PUSH ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0; }
uint32_t vkd_max_bindings(void) { return g_mode == VKD_PUSH ? vd.max_push : UINT32_MAX; }   // pools: the per-stage device limits hold (the shaders' bindings are few)

void vkd_slot_begin(int slot) {   // mtl_sub_begin, after the slot's last submission completed and before anything is recorded
    g_slot = slot % VKD_SLOTS;
    if (g_mode != VKD_POOL) return;
    vkd_chain *c = &g_ch[g_slot];
    for (int i = 0; i < c->n; i++) g_reset(vd.dev, c->p[i], 0);
    c->cur = 0;
}

// The draw's writes (dstSet ignored by push descriptors, filled here for a set) into the command buffer, for layout pl whose one set has layout dsl. 0 = not delivered (said once, counted)
int vkd_write(VkCommandBuffer cb, VkPipelineLayout pl, VkDescriptorSetLayout dsl, uint32_t nw, VkWriteDescriptorSet *wr) { return vkd_write_bp(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, dsl, nw, wr); }
int vkd_write_bp(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout pl, VkDescriptorSetLayout dsl, uint32_t nw, VkWriteDescriptorSet *wr) {
    if (g_mode == VKD_PUSH) { vk.vkCmdPushDescriptorSetKHR(cb, bp, pl, 0, nw, wr); return 1; }
    if (g_mode != VKD_POOL) return 0;
    vkd_chain *c = &g_ch[g_slot]; VkDescriptorSet set = VK_NULL_HANDLE;
    uint64_t t0 = now_ns();
    for (;;) {
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, c->p[c->cur], 1, &dsl};
        VkResult r = vk.vkAllocateDescriptorSets(vd.dev, &ai, &set);
        if (!r) break;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) { VRED_ONCE("vkAllocateDescriptorSets: VkResult %d: the draw is skipped", r); atomic_fetch_add(&g_fail, 1); return 0; }
        if (c->cur + 1 < c->n) { c->cur++; continue; }
        if (c->n == (int)(sizeof c->p / sizeof c->p[0]) || !(c->p[c->n] = pool_make())) { VRED_ONCE("the descriptor pool chain cannot grow: the draw is skipped"); atomic_fetch_add(&g_fail, 1); return 0; }
        c->cur = c->n++;
    }
    uint64_t t1 = now_ns(); atomic_fetch_add(&g_alloc_ns, t1 - t0);
    for (uint32_t i = 0; i < nw; i++) { wr[i].dstSet = set; atomic_fetch_add(&g_ndesc, wr[i].descriptorCount); }
    vk.vkUpdateDescriptorSets(vd.dev, nw, wr, 0, NULL);
    vk.vkCmdBindDescriptorSets(cb, bp, pl, 0, 1, &set, 0, NULL);
    atomic_fetch_add(&g_upd_ns, now_ns() - t1);
    atomic_fetch_add(&g_sets, 1); return 1;
}

void vkd_report(char *o, size_t n) {   // for the draws' periodic line
    if (g_mode == VKD_POOL) snprintf(o, n, "descriptor sets from pools %llu (pools %llu, failed %llu; %.1f descriptors a set, allocate %.2f + update and bind %.2f us a set)", (unsigned long long)atomic_load(&g_sets), (unsigned long long)atomic_load(&g_pools), (unsigned long long)atomic_load(&g_fail),
        g_sets ? (double)g_ndesc / (double)g_sets : 0.0, g_sets ? g_alloc_ns / 1e3 / (double)g_sets : 0.0, g_sets ? g_upd_ns / 1e3 / (double)g_sets : 0.0);
    else snprintf(o, n, "descriptors %s", g_mode == VKD_PUSH ? "pushed" : "off");
}
