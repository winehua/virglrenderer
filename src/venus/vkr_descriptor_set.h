/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_DESCRIPTOR_SET_H
#define VKR_DESCRIPTOR_SET_H

#include "vkr_common.h"

#include "vkr_context.h"

#ifdef __OHOS__
struct vkr_buffer;

struct vkr_winehua_ubo_binding {
   struct vkr_buffer *buffer;
   uint64_t buffer_id;
   uint64_t mapping_sequence;
   uint64_t last_bound_mapping_sequence;
   VkDeviceSize offset;
   VkDeviceSize size;
   uint32_t binding;
   uint32_t array_element;
   VkDescriptorType descriptor_type;
   bool valid;
};
#endif

struct vkr_descriptor_set_layout {
   struct vkr_object base;
};
VKR_DEFINE_OBJECT_CAST(descriptor_set_layout,
                       VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
                       VkDescriptorSetLayout)

struct vkr_descriptor_pool {
   struct vkr_object base;

   VkDescriptorPoolCreateFlags flags;

   struct list_head descriptor_sets;
};
VKR_DEFINE_OBJECT_CAST(descriptor_pool, VK_OBJECT_TYPE_DESCRIPTOR_POOL, VkDescriptorPool)

struct vkr_descriptor_set {
   struct vkr_object base;

   struct vkr_device *device;
#ifdef __OHOS__
   struct vkr_winehua_ubo_binding winehua_ubo_bindings[2];
#endif
};
VKR_DEFINE_OBJECT_CAST(descriptor_set, VK_OBJECT_TYPE_DESCRIPTOR_SET, VkDescriptorSet)

struct vkr_descriptor_update_template {
   struct vkr_object base;
};
VKR_DEFINE_OBJECT_CAST(descriptor_update_template,
                       VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE,
                       VkDescriptorUpdateTemplate)

void
vkr_context_init_descriptor_set_layout_dispatch(struct vkr_context *ctx);

void
vkr_context_init_descriptor_pool_dispatch(struct vkr_context *ctx);

void
vkr_context_init_descriptor_set_dispatch(struct vkr_context *ctx);

void
vkr_context_init_descriptor_update_template_dispatch(struct vkr_context *ctx);

static inline void
vkr_descriptor_pool_release(struct vkr_context *ctx, struct vkr_descriptor_pool *pool)
{
   vkr_context_remove_objects(ctx, &pool->descriptor_sets);
}

#endif /* VKR_DESCRIPTOR_SET_H */
