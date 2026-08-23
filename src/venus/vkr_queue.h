/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_QUEUE_H
#define VKR_QUEUE_H

#include "vkr_common.h"

struct vkr_queue_sync {
   VkFence fence;
   bool device_lost;

   uint32_t flags;
   uint32_t ring_idx;
   uint64_t fence_id;

   struct list_head head;
};

#ifdef __OHOS__
#define VKR_WINEHUA_SHADOW_UPLOAD_SLOT_COUNT 8

struct vkr_shadow_upload_slot {
   VkCommandPool pool;
   VkCommandBuffer command;
   VkFence fence;
   bool in_flight;
   uint64_t retire_value;
};
#endif

struct vkr_queue {
   struct vkr_object base;

   struct vkr_context *context;
   struct vkr_device *device;

   VkDeviceQueueCreateFlags flags;
   uint32_t family;
   uint32_t index;

   /* only used when client driver uses multiple timelines */
   uint32_t ring_idx;

   /* Ensure host access to VkQueue being externally synchronized between renderer main
    * thread and ring thread.
    */
   mtx_t vk_mutex;

#ifdef __OHOS__
   /* A private transfer command runs immediately before a guest submit when
    * mapped Host memory is not reliably visible to the Maleoon GPU. */
   mtx_t shadow_upload_mutex;
   struct vkr_shadow_upload_slot
      shadow_upload_slots[VKR_WINEHUA_SHADOW_UPLOAD_SLOT_COUNT];
   uint32_t shadow_upload_slot;
   VkSemaphore shadow_upload_timeline;
   uint64_t shadow_upload_next_value;
   bool shadow_upload_inline;
   bool shadow_upload_prepared;
   uint64_t shadow_upload_bytes;
   uint32_t shadow_upload_updates;
   uint32_t shadow_upload_ranges;
   uint32_t shadow_upload_buffers;
   uint32_t shadow_upload_uniform_buffers;
   uint32_t shadow_upload_storage_buffers;
   bool winehua_perf_summary;
   /* Diagnostics only: the production path leaves this at zero. */
   uint32_t winehua_perf_sample_interval;
   /* A sampled present-to-present interval.  It is armed by the private
    * present path while holding vk_mutex, so QueueSubmit can account an
    * entire rendered frame without adding clocks to production frames. */
   uint32_t winehua_frame_timeline_interval;
   bool winehua_frame_timeline_active;
   uint32_t winehua_frame_timeline_serial;
   uint64_t winehua_frame_submit_count;
   uint64_t winehua_frame_submit_infos;
   uint64_t winehua_frame_command_buffers;
   uint64_t winehua_frame_wait_semaphores;
   uint64_t winehua_frame_signal_semaphores;
   uint64_t winehua_frame_shadow_bytes;
   uint64_t winehua_frame_upload_bytes;
   uint64_t winehua_frame_upload_ranges;
   uint64_t winehua_frame_prepare_us;
   uint64_t winehua_frame_sync_us;
   uint64_t winehua_frame_upload_us;
   uint64_t winehua_frame_lock_us;
   uint64_t winehua_frame_driver_us;
   uint64_t winehua_frame_total_us;
   atomic_uint_fast64_t winehua_perf_last_submit_end_ns;
   atomic_uint_fast64_t winehua_perf_last_sample_end_ns;
   atomic_uint_fast64_t winehua_perf_last_sample_submit_id;
   uint64_t shadow_upload_wait_us;
   uint64_t shadow_upload_reset_begin_us;
   uint64_t shadow_upload_dirty_scan_us;
   uint64_t shadow_upload_buffer_record_us;
   uint64_t shadow_upload_uncovered_scan_us;
   uint64_t shadow_upload_end_us;
#endif

   /* Submitted fences are added to sync_thread.syncs first. With required
    * VKR_RENDERER_THREAD_SYNC and VKR_RENDERER_ASYNC_FENCE_CB in render server, the sync
    * thread calls vkWaitForFences and retires signaled fences in order.
    */
   struct {
      mtx_t mutex;
      cnd_t cond;
      struct list_head syncs;
      thrd_t thread;
      bool join;
   } sync_thread;
};
VKR_DEFINE_OBJECT_CAST(queue, VK_OBJECT_TYPE_QUEUE, VkQueue)

struct vkr_fence {
   struct vkr_object base;
};
VKR_DEFINE_OBJECT_CAST(fence, VK_OBJECT_TYPE_FENCE, VkFence)

struct vkr_semaphore {
   struct vkr_object base;
};
VKR_DEFINE_OBJECT_CAST(semaphore, VK_OBJECT_TYPE_SEMAPHORE, VkSemaphore)

struct vkr_event {
   struct vkr_object base;
};
VKR_DEFINE_OBJECT_CAST(event, VK_OBJECT_TYPE_EVENT, VkEvent)

void
vkr_context_init_queue_dispatch(struct vkr_context *ctx);

void
vkr_context_init_fence_dispatch(struct vkr_context *ctx);

void
vkr_context_init_semaphore_dispatch(struct vkr_context *ctx);

void
vkr_context_init_event_dispatch(struct vkr_context *ctx);

struct vkr_queue *
vkr_queue_create(struct vkr_context *ctx,
                 struct vkr_device *dev,
                 VkDeviceQueueCreateFlags flags,
                 uint32_t family,
                 uint32_t index,
                 VkQueue handle);

void
vkr_queue_destroy(struct vkr_context *ctx, struct vkr_queue *queue);

#ifdef __OHOS__
uint64_t
vkr_winehua_queue_submit_generation(void);

/* Called with queue->vk_mutex held by the private present implementation.
 * It reports the completed sampled interval and arms the following one. */
void
vkr_winehua_queue_frame_timeline_present(struct vkr_queue *queue,
                                         uint32_t present_serial);
#endif

bool
vkr_queue_sync_submit(struct vkr_queue *queue,
                      uint32_t flags,
                      uint32_t ring_idx,
                      uint64_t fence_id);

#endif /* VKR_QUEUE_H */
