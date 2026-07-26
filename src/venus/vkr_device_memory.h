/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_DEVICE_MEMORY_H
#define VKR_DEVICE_MEMORY_H

#include "vkr_common.h"

struct gbm_bo;
struct vkr_mtl_shm;
struct vkr_context;

#ifdef __OHOS__
struct vkr_ohos_shadow_dirty_range {
   VkDeviceSize offset;
   VkDeviceSize size;
};
#endif

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
   struct vkr_context *context;

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
   struct vkr_ohos_shadow_dirty_range *shadow_dirty_ranges;
   uint32_t shadow_dirty_range_count;
   uint32_t shadow_dirty_range_capacity;
   bool shadow_dirty_range_overflow;
   struct list_head shadow_dirty_head;
   bool shadow_dirty_listed;
   struct list_head bound_buffers;
   void *shadow_upload_snapshot;
   bool shadow_host_copy_deferred;
   bool shadow_gpu_upload_covered;
   bool shadow_gpu_upload_full_coverage;
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

bool
vkr_device_memory_gpu_upload_enabled(const struct vkr_device *dev);

bool
vkr_device_memory_requires_deferred_host_wait(struct vkr_context *ctx,
                                              struct vkr_device *dev);

VkResult
vkr_device_memory_prepare_shadow_upload(struct vkr_context *ctx,
                                        struct vkr_queue *queue,
                                        bool perf_timing);

VkResult
vkr_device_memory_submit_shadow_upload(struct vkr_queue *queue);

void
vkr_device_memory_shadow_generation_begin(struct vkr_context *ctx,
                                          bool submit);

void
vkr_device_memory_shadow_generation_end(struct vkr_context *ctx);

void
vkr_device_memory_disable_shadow_upload_coverage(struct vkr_context *ctx);

struct vkr_shadow_sync_stats {
   uint64_t bytes;
   uint32_t copies;
   uint32_t cache_ops;
   uint64_t gpu_upload_skipped_bytes;
   uint32_t gpu_upload_skipped_copies;
   uint32_t scanned;
   uint64_t elapsed_us;
};

struct vkr_shadow_sync_stats
vkr_device_memory_sync_shadows_to_host(struct vkr_context *ctx);

void
vkr_device_memory_sync_shadows_from_host(struct vkr_context *ctx);

#endif /* VKR_DEVICE_MEMORY_H */
