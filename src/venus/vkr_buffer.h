/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_BUFFER_H
#define VKR_BUFFER_H

#include "vkr_common.h"

#ifdef __OHOS__
#include <stdatomic.h>

#define VKR_WINEHUA_UBO_WATCH_COUNT 256u

struct vkr_winehua_ubo_watch {
   VkDeviceSize offset;
   VkDeviceSize size;
   atomic_uint_fast64_t last_update_hash;
   uint32_t binding;
   atomic_bool last_update_hash_valid;
};
#endif

struct vkr_buffer {
   struct vkr_object base;

#ifdef __OHOS__
   struct vkr_winehua_ubo_watch *winehua_ubo_watches;
   atomic_uint winehua_ubo_watch_count;
   bool winehua_ubo_watch_overflow;

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

#ifdef __OHOS__
bool
vkr_winehua_buffer_add_ubo_watch_locked(struct vkr_buffer *buffer,
                                        uint32_t binding,
                                        VkDeviceSize offset,
                                        VkDeviceSize size);
#endif

void
vkr_context_init_buffer_view_dispatch(struct vkr_context *ctx);

#endif /* VKR_BUFFER_H */
