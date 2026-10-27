// Descriptor delivery for the draws (vk_desc.c): push descriptors where the device has them, else per-submission-slot descriptor pools (DM_VK_PUSH overrides)
#pragma once
#include "vkn.h"
enum { VKD_PUSH = 1, VKD_POOL = 2 };
int vkd_init(void);                    // after the device exists: chooses and says the mode; 0 = neither works (nothing can be drawn)
int vkd_mode(void);                    // VKD_PUSH, VKD_POOL, or -1 before / after a failed vkd_init
uint32_t vkd_layout_flags(void);       // VkDescriptorSetLayoutCreateFlags for the draws' set layouts
uint32_t vkd_max_bindings(void);       // bindings a pair's one set may have
void vkd_slot_begin(int slot);         // a submission slot begins (its last submission has completed)
int vkd_write(VkCommandBuffer cb, VkPipelineLayout pl, VkDescriptorSetLayout dsl, uint32_t nw, VkWriteDescriptorSet *wr);
int vkd_write_bp(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout pl, VkDescriptorSetLayout dsl, uint32_t nw, VkWriteDescriptorSet *wr);   // the same at a bind point (compute)
void vkd_report(char *o, size_t n);
