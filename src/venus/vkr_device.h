/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_DEVICE_H
#define VKR_DEVICE_H

#include "vkr_common.h"

#include "venus-protocol/vn_protocol_renderer_util.h"

#include "vkr_context.h"

#ifdef __OHOS__
struct vkr_winehua_perf_totals {
   uint64_t queue_submit_count;
   uint64_t submit_infos;
   uint64_t shadow_scanned;
   uint64_t shadow_copies;
   uint64_t shadow_bytes;
   uint64_t prepare_total_us;
   uint64_t prepare_phase_count;
   uint64_t prepare_wait_total_us;
   uint64_t prepare_reset_total_us;
   uint64_t prepare_dirty_total_us;
   uint64_t prepare_buffer_total_us;
   uint64_t prepare_uncovered_total_us;
   uint64_t prepare_end_total_us;
   uint64_t sync_total_us;
   uint64_t lock_total_us;
   uint64_t upload_submit_count;
   uint64_t upload_buffers;
   uint64_t upload_uniform_buffers;
   uint64_t upload_storage_buffers;
   uint64_t upload_ranges;
   uint64_t upload_updates;
   uint64_t upload_bytes;
   uint64_t upload_skipped_bytes;
   uint64_t upload_skipped_copies;
   uint64_t upload_total_us;
   uint64_t driver_total_us;
   uint64_t total_us;
   uint64_t submit_gap_total_us;
   uint64_t fence_status_count;
   uint64_t fence_status_total_us;
   uint64_t fence_status_success_count;
   uint64_t fence_status_not_ready_count;
   uint64_t fence_wait_count;
   uint64_t fence_wait_total_us;
   uint64_t context_fence_submit_count;
   uint64_t context_fence_wait_count;
};
#endif

struct vkr_device {
   struct vkr_object base;

   struct vkr_physical_device *physical_device;

   struct vn_device_proc_table proc_table;

   struct list_head queues;

   mtx_t free_sync_mutex;
   struct list_head free_syncs;

   mtx_t object_mutex;
   struct list_head objects;

#ifdef __OHOS__
   /* Diagnostics only: process-wide counters are snapshotted at device
    * creation and reported as a per-device delta at destruction. */
   struct vkr_winehua_perf_totals winehua_perf_begin;
   bool winehua_perf_boundary_enabled;

   /* Diagnostic descriptor serialization waits at most once for each Host
    * queue-submit generation. Waiting before every descriptor write makes a
    * real game effectively single-step and can mask the ordering failure the
    * A/B is intended to isolate. */
   atomic_uint_fast64_t winehua_descriptor_wait_submit_generation;
#endif

   void *mtl_device;
};
VKR_DEFINE_OBJECT_CAST(device, VK_OBJECT_TYPE_DEVICE, VkDevice)

void
vkr_context_init_device_dispatch(struct vkr_context *ctx);

void
vkr_device_destroy(struct vkr_context *ctx, struct vkr_device *dev, bool destroy_vk);

static inline bool
vkr_device_should_track_object(const struct vkr_object *obj)
{
   assert(vkr_is_recognized_object_type(obj->type));

   switch (obj->type) {
   case VK_OBJECT_TYPE_INSTANCE:        /* non-device objects */
   case VK_OBJECT_TYPE_PHYSICAL_DEVICE: /* non-device objects */
   case VK_OBJECT_TYPE_DEVICE:          /* device itself */
   case VK_OBJECT_TYPE_QUEUE:           /* not tracked as device objects */
   case VK_OBJECT_TYPE_COMMAND_BUFFER:  /* pool objects */
   case VK_OBJECT_TYPE_DESCRIPTOR_SET:  /* pool objects */
      return false;
   default:
      return true;
   }
}

static inline void
vkr_device_add_object(struct vkr_context *ctx,
                      struct vkr_device *dev,
                      struct vkr_object *obj)
{
   vkr_context_add_object(ctx, obj);

   assert(vkr_device_should_track_object(obj));

   mtx_lock(&dev->object_mutex);
   list_add(&obj->track_head, &dev->objects);
   mtx_unlock(&dev->object_mutex);
}

static inline void
vkr_device_remove_object(struct vkr_context *ctx,
                         UNUSED struct vkr_device *dev,
                         struct vkr_object *obj)
{
   assert(vkr_device_should_track_object(obj));

   mtx_lock(&dev->object_mutex);
   list_del(&obj->track_head);
   mtx_unlock(&dev->object_mutex);

   /* this frees obj */
   vkr_context_remove_object(ctx, obj);
}

#endif /* VKR_DEVICE_H */
