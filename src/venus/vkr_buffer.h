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
   /* Diagnostic ownership only. Vulkan requires bound memory to outlive the
    * buffer, so this non-owning pointer is valid for the buffer lifetime. */
   struct vkr_device_memory *bound_memory;
   VkDeviceSize bound_memory_offset;
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
