/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_DEVICE_MEMORY_H
#define VKR_DEVICE_MEMORY_H

#include "vkr_common.h"

struct gbm_bo;
struct vkr_mtl_shm;

struct vkr_device_memory {
   struct vkr_object base;

   struct vkr_device *device;

   bool might_export;

   uint32_t property_flags;
   uint32_t valid_fd_types;

   /* gbm bo backing non-external mappable memory */
   struct gbm_bo *gbm_bo;

   /* udmabuf backing non-external mappable memory */
   int udmabuf_fd;

   /* Metal buffer backed by POSIX shared memory */
   struct vkr_mtl_shm *mtl_shm;

   uint64_t allocation_size;
   uint32_t memory_type_index;

   bool exported;

#ifdef __OHOS__
   /* Compatibility path for Host-visible Maleoon memory when Linux external
    * memory fd export is unavailable. */
   int shadow_fd;
   void *shadow_map;
   void *host_map;
   uint64_t shadow_size;
   uint32_t shadow_sync_count;
   uint32_t shadow_remote_flush_count;
   uint32_t shadow_guest_write_depth;
   uint32_t shadow_remote_invalidate_count;
   bool shadow_remote_active;
   bool shadow_host_dirty;
   bool shadow_initial_sync_done;
   VkDeviceSize shadow_dirty_offset;
   VkDeviceSize shadow_dirty_size;
   uint64_t shadow_pending_copy_bytes;
   uint32_t shadow_pending_copy_count;
#endif
};
VKR_DEFINE_OBJECT_CAST(device_memory, VK_OBJECT_TYPE_DEVICE_MEMORY, VkDeviceMemory)

void
vkr_context_init_device_memory_dispatch(struct vkr_context *ctx);

void
vkr_device_memory_release(struct vkr_device_memory *mem);

bool
vkr_device_memory_export_blob(struct vkr_device_memory *mem,
                              uint64_t blob_size,
                              uint32_t blob_flags,
                              struct virgl_context_blob *out_blob);

void
vkr_device_memory_sync_shadows_to_host(struct vkr_context *ctx);

void
vkr_device_memory_sync_shadows_from_host(struct vkr_context *ctx);

#endif /* VKR_DEVICE_MEMORY_H */
