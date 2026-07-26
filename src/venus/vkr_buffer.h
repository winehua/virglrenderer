/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_BUFFER_H
#define VKR_BUFFER_H

#include "vkr_common.h"

struct vkr_buffer {
   struct vkr_object base;

#ifdef __OHOS__
   /* Vulkan requires bound memory to outlive the buffer, so this non-owning
    * pointer is valid for the buffer lifetime.  Size and usage are retained
    * for the Maleoon shadow-memory GPU upload compatibility path. */
   struct vkr_device_memory *bound_memory;
   VkDeviceSize bound_memory_offset;
   VkDeviceSize size;
   VkBufferUsageFlags guest_usage;
   VkBufferUsageFlags host_usage;
   struct list_head memory_head;
   bool memory_listed;
#endif
};
VKR_DEFINE_OBJECT_CAST(buffer, VK_OBJECT_TYPE_BUFFER, VkBuffer)

struct vkr_buffer_view {
   struct vkr_object base;
};
VKR_DEFINE_OBJECT_CAST(buffer_view, VK_OBJECT_TYPE_BUFFER_VIEW, VkBufferView)

void
vkr_context_init_buffer_dispatch(struct vkr_context *ctx);

void
vkr_context_init_buffer_view_dispatch(struct vkr_context *ctx);

#endif /* VKR_BUFFER_H */
