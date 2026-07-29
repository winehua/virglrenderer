/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_queue.h"

#include "venus-protocol/vn_protocol_renderer_queue.h"

#include "vkr_context.h"
#include "vkr_command_buffer.h"
#include "vkr_device_memory.h"
#include "vkr_physical_device.h"
#include "vkr_queue_gen.h"

#ifdef __OHOS__
#include <stdlib.h>
#include <time.h>

static atomic_uint_fast64_t vkr_ohos_queue_submit_count;
static atomic_uint_fast64_t vkr_ohos_queue_submit_slow_count;
static atomic_uint_fast64_t vkr_ohos_shadow_queue_wait_count;
static atomic_uint_fast64_t vkr_ohos_fence_status_count;
static atomic_uint_fast64_t vkr_ohos_fence_status_total_us;
static atomic_uint_fast64_t vkr_ohos_fence_status_success_count;
static atomic_uint_fast64_t vkr_ohos_fence_status_not_ready_count;
static atomic_uint_fast64_t vkr_ohos_fence_wait_count;
static atomic_uint_fast64_t vkr_ohos_fence_wait_total_us;
static atomic_uint_fast64_t vkr_ohos_fence_wait_max_us;
static atomic_uint_fast64_t vkr_ohos_perf_submit_infos;
static atomic_uint_fast64_t vkr_ohos_perf_shadow_scanned;
static atomic_uint_fast64_t vkr_ohos_perf_shadow_copies;
static atomic_uint_fast64_t vkr_ohos_perf_shadow_bytes;
static atomic_uint_fast64_t vkr_ohos_perf_prepare_total_us;
static atomic_uint_fast64_t vkr_ohos_perf_prepare_max_us;
static atomic_uint_fast64_t vkr_ohos_perf_prepare_phase_count;
static atomic_uint_fast64_t vkr_ohos_perf_prepare_wait_total_us;
static atomic_uint_fast64_t vkr_ohos_perf_prepare_wait_max_us;
static atomic_uint_fast64_t vkr_ohos_perf_prepare_reset_total_us;
static atomic_uint_fast64_t vkr_ohos_perf_prepare_reset_max_us;
static atomic_uint_fast64_t vkr_ohos_perf_prepare_dirty_total_us;
static atomic_uint_fast64_t vkr_ohos_perf_prepare_dirty_max_us;
static atomic_uint_fast64_t vkr_ohos_perf_prepare_buffer_total_us;
static atomic_uint_fast64_t vkr_ohos_perf_prepare_buffer_max_us;
static atomic_uint_fast64_t vkr_ohos_perf_prepare_uncovered_total_us;
static atomic_uint_fast64_t vkr_ohos_perf_prepare_uncovered_max_us;
static atomic_uint_fast64_t vkr_ohos_perf_prepare_end_total_us;
static atomic_uint_fast64_t vkr_ohos_perf_prepare_end_max_us;
static atomic_uint_fast64_t vkr_ohos_perf_sync_total_us;
static atomic_uint_fast64_t vkr_ohos_perf_sync_max_us;
static atomic_uint_fast64_t vkr_ohos_perf_lock_total_us;
static atomic_uint_fast64_t vkr_ohos_perf_lock_max_us;
static atomic_uint_fast64_t vkr_ohos_perf_upload_submit_count;
static atomic_uint_fast64_t vkr_ohos_perf_upload_buffers;
static atomic_uint_fast64_t vkr_ohos_perf_upload_uniform_buffers;
static atomic_uint_fast64_t vkr_ohos_perf_upload_storage_buffers;
static atomic_uint_fast64_t vkr_ohos_perf_upload_ranges;
static atomic_uint_fast64_t vkr_ohos_perf_upload_updates;
static atomic_uint_fast64_t vkr_ohos_perf_upload_bytes;
static atomic_uint_fast64_t vkr_ohos_perf_upload_skipped_bytes;
static atomic_uint_fast64_t vkr_ohos_perf_upload_skipped_copies;
static atomic_uint_fast64_t vkr_ohos_perf_upload_total_us;
static atomic_uint_fast64_t vkr_ohos_perf_upload_max_us;
static atomic_uint_fast64_t vkr_ohos_perf_driver_total_us;
static atomic_uint_fast64_t vkr_ohos_perf_driver_max_us;
static atomic_uint_fast64_t vkr_ohos_perf_total_us;
static atomic_uint_fast64_t vkr_ohos_perf_total_max_us;
static atomic_uint_fast64_t vkr_ohos_perf_submit_gap_total_us;
static atomic_uint_fast64_t vkr_ohos_perf_submit_gap_max_us;

uint64_t
vkr_winehua_queue_submit_generation(void)
{
   return atomic_load_explicit(&vkr_ohos_queue_submit_count,
                               memory_order_acquire);
}

static bool
vkr_ohos_gpu_upload_inline_enabled(void)
{
   const char *value = os_get_option("VKR_WINEHUA_GPU_UPLOAD_INLINE");
   return value && value[0] == '1' && !value[1];
}

static bool
vkr_ohos_cached_option_enabled(const char *name, atomic_int *cached)
{
   int enabled = atomic_load_explicit(cached, memory_order_relaxed);
   if (enabled < 0) {
      const char *value = os_get_option(name);
      enabled = value && value[0] == '1' && !value[1];
      atomic_store_explicit(cached, enabled, memory_order_relaxed);
   }
   return enabled != 0;
}

static bool
vkr_ohos_perf_trace_enabled(void)
{
   static atomic_int cached = ATOMIC_VAR_INIT(-1);
   return vkr_ohos_cached_option_enabled("VKR_WINEHUA_SHADOW_TRACE", &cached);
}

static bool
vkr_ohos_perf_summary_enabled(void)
{
   static atomic_int cached = ATOMIC_VAR_INIT(-1);
   return vkr_ohos_cached_option_enabled("VKR_WINEHUA_PERF_SUMMARY", &cached);
}

/* The full summary instruments every submit.  This separate mode samples one
 * submit in a fixed interval, keeping all other submissions on the production
 * path without extra clocks or aggregate accounting. */
static uint32_t
vkr_ohos_perf_sample_interval(void)
{
   static atomic_int cached = ATOMIC_VAR_INIT(-1);
   int interval = atomic_load_explicit(&cached, memory_order_relaxed);
   if (interval < 0) {
      const char *value = os_get_option("VKR_WINEHUA_PERF_SAMPLE_INTERVAL");
      char *end = NULL;
      const unsigned long parsed = value ? strtoul(value, &end, 10) : 0;
      interval = value && end && !*end && parsed > 0 && parsed <= 1000000
         ? (int)parsed : 0;
      atomic_store_explicit(&cached, interval, memory_order_relaxed);
   }
   return (uint32_t)interval;
}

static uint32_t
vkr_ohos_frame_timeline_interval(void)
{
   static atomic_int cached = ATOMIC_VAR_INIT(-1);
   int interval = atomic_load_explicit(&cached, memory_order_relaxed);
   if (interval < 0) {
      const char *value = os_get_option("VKR_WINEHUA_FRAME_TIMELINE_INTERVAL");
      char *end = NULL;
      const unsigned long parsed = value ? strtoul(value, &end, 10) : 0;
      interval = value && end && !*end && parsed > 0 && parsed <= 1000000
         ? (int)parsed : 0;
      atomic_store_explicit(&cached, interval, memory_order_relaxed);
   }
   return (uint32_t)interval;
}

static void
vkr_ohos_frame_timeline_reset(struct vkr_queue *queue, uint32_t serial)
{
   queue->winehua_frame_timeline_serial = serial;
   queue->winehua_frame_submit_count = 0;
   queue->winehua_frame_submit_infos = 0;
   queue->winehua_frame_command_buffers = 0;
   queue->winehua_frame_wait_semaphores = 0;
   queue->winehua_frame_signal_semaphores = 0;
   queue->winehua_frame_shadow_bytes = 0;
   queue->winehua_frame_upload_bytes = 0;
   queue->winehua_frame_upload_ranges = 0;
   queue->winehua_frame_prepare_us = 0;
   queue->winehua_frame_sync_us = 0;
   queue->winehua_frame_upload_us = 0;
   queue->winehua_frame_lock_us = 0;
   queue->winehua_frame_driver_us = 0;
   queue->winehua_frame_total_us = 0;
}

static void
vkr_ohos_frame_timeline_account(struct vkr_queue *queue,
                                 const struct vn_command_vkQueueSubmit *args,
                                 const struct vkr_shadow_sync_stats *shadow_stats,
                                 uint64_t upload_bytes,
                                 uint32_t upload_ranges,
                                 uint64_t prepare_us,
                                 uint64_t sync_us,
                                 uint64_t upload_us,
                                 uint64_t lock_us,
                                 uint64_t driver_us,
                                 uint64_t total_us)
{
   if (!queue->winehua_frame_timeline_active)
      return;

   uint64_t command_buffers = 0;
   uint64_t waits = 0;
   uint64_t signals = 0;
   for (uint32_t i = 0; i < args->submitCount; i++) {
      command_buffers += args->pSubmits[i].commandBufferCount;
      waits += args->pSubmits[i].waitSemaphoreCount;
      signals += args->pSubmits[i].signalSemaphoreCount;
   }

   queue->winehua_frame_submit_count++;
   queue->winehua_frame_submit_infos += args->submitCount;
   queue->winehua_frame_command_buffers += command_buffers;
   queue->winehua_frame_wait_semaphores += waits;
   queue->winehua_frame_signal_semaphores += signals;
   queue->winehua_frame_shadow_bytes += shadow_stats->bytes;
   queue->winehua_frame_upload_bytes += upload_bytes;
   queue->winehua_frame_upload_ranges += upload_ranges;
   queue->winehua_frame_prepare_us += prepare_us;
   queue->winehua_frame_sync_us += sync_us;
   queue->winehua_frame_upload_us += upload_us;
   queue->winehua_frame_lock_us += lock_us;
   queue->winehua_frame_driver_us += driver_us;
   queue->winehua_frame_total_us += total_us;
}

void
vkr_winehua_queue_frame_timeline_present(struct vkr_queue *queue,
                                         uint32_t present_serial)
{
   if (!queue || !queue->winehua_frame_timeline_interval)
      return;

   /* vkr_renderer owns queue->vk_mutex for the duration of this call. */
   if (queue->winehua_frame_timeline_active) {
      vkr_log("WineHuaFrameTimeline: layer=host serial=%u "
              "queue_submits=%" PRIu64 " submit_infos=%" PRIu64
              " command_buffers=%" PRIu64 " waits=%" PRIu64
              " signals=%" PRIu64 " shadow_bytes=%" PRIu64
              " upload_bytes=%" PRIu64 " upload_ranges=%" PRIu64
              " prepare_us=%" PRIu64 " sync_us=%" PRIu64
              " upload_us=%" PRIu64 " lock_us=%" PRIu64
              " driver_us=%" PRIu64 " total_cpu_us=%" PRIu64,
              queue->winehua_frame_timeline_serial,
              queue->winehua_frame_submit_count,
              queue->winehua_frame_submit_infos,
              queue->winehua_frame_command_buffers,
              queue->winehua_frame_wait_semaphores,
              queue->winehua_frame_signal_semaphores,
              queue->winehua_frame_shadow_bytes,
              queue->winehua_frame_upload_bytes,
              queue->winehua_frame_upload_ranges,
              queue->winehua_frame_prepare_us,
              queue->winehua_frame_sync_us,
              queue->winehua_frame_upload_us,
              queue->winehua_frame_lock_us,
              queue->winehua_frame_driver_us,
              queue->winehua_frame_total_us);
   }

   const uint32_t next_serial = present_serial + 1;
   queue->winehua_frame_timeline_active =
      !(next_serial % queue->winehua_frame_timeline_interval);
   if (queue->winehua_frame_timeline_active)
      vkr_ohos_frame_timeline_reset(queue, next_serial);
}

static bool
vkr_ohos_perf_sample_now(uint64_t id)
{
   const uint32_t interval = vkr_ohos_perf_sample_interval();
   return interval && !(id % interval);
}

static bool
vkr_ohos_frame_assoc_trace_enabled(void)
{
   static atomic_int cached = ATOMIC_VAR_INIT(-1);
   return vkr_ohos_cached_option_enabled("WINEHUA_VKR_TRACE_CAPTURE", &cached);
}

static void
vkr_ohos_atomic_max(atomic_uint_fast64_t *value, uint64_t candidate)
{
   uint_fast64_t current = atomic_load_explicit(value, memory_order_relaxed);
   while (current < candidate &&
          !atomic_compare_exchange_weak_explicit(value, &current, candidate,
                                                 memory_order_relaxed,
                                                 memory_order_relaxed))
      ;
}

static uint64_t
vkr_ohos_queue_now_ns(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts))
      return 0;
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void
vkr_ohos_perf_record_prepare_phases(const struct vkr_queue *queue)
{
   const uint64_t count = atomic_fetch_add_explicit(
      &vkr_ohos_perf_prepare_phase_count, 1, memory_order_relaxed) + 1;
   atomic_fetch_add_explicit(&vkr_ohos_perf_prepare_wait_total_us,
                             queue->shadow_upload_wait_us,
                             memory_order_relaxed);
   atomic_fetch_add_explicit(&vkr_ohos_perf_prepare_reset_total_us,
                             queue->shadow_upload_reset_begin_us,
                             memory_order_relaxed);
   atomic_fetch_add_explicit(&vkr_ohos_perf_prepare_dirty_total_us,
                             queue->shadow_upload_dirty_scan_us,
                             memory_order_relaxed);
   atomic_fetch_add_explicit(&vkr_ohos_perf_prepare_buffer_total_us,
                             queue->shadow_upload_buffer_record_us,
                             memory_order_relaxed);
   atomic_fetch_add_explicit(&vkr_ohos_perf_prepare_uncovered_total_us,
                             queue->shadow_upload_uncovered_scan_us,
                             memory_order_relaxed);
   atomic_fetch_add_explicit(&vkr_ohos_perf_prepare_end_total_us,
                             queue->shadow_upload_end_us,
                             memory_order_relaxed);
   vkr_ohos_atomic_max(&vkr_ohos_perf_prepare_wait_max_us,
                       queue->shadow_upload_wait_us);
   vkr_ohos_atomic_max(&vkr_ohos_perf_prepare_reset_max_us,
                       queue->shadow_upload_reset_begin_us);
   vkr_ohos_atomic_max(&vkr_ohos_perf_prepare_dirty_max_us,
                       queue->shadow_upload_dirty_scan_us);
   vkr_ohos_atomic_max(&vkr_ohos_perf_prepare_buffer_max_us,
                       queue->shadow_upload_buffer_record_us);
   vkr_ohos_atomic_max(&vkr_ohos_perf_prepare_uncovered_max_us,
                       queue->shadow_upload_uncovered_scan_us);
   vkr_ohos_atomic_max(&vkr_ohos_perf_prepare_end_max_us,
                       queue->shadow_upload_end_us);

   if (count <= 8 || !(count % 600))
      vkr_log("WineHuaPerfPrepare: calls=%" PRIu64
              " wait_us=%" PRIuFAST64 "/%" PRIuFAST64
              " reset_begin_us=%" PRIuFAST64 "/%" PRIuFAST64
              " dirty_scan_us=%" PRIuFAST64 "/%" PRIuFAST64
              " buffer_record_us=%" PRIuFAST64 "/%" PRIuFAST64
              " uncovered_scan_us=%" PRIuFAST64 "/%" PRIuFAST64
              " end_us=%" PRIuFAST64 "/%" PRIuFAST64,
              count,
              atomic_load_explicit(&vkr_ohos_perf_prepare_wait_total_us,
                                   memory_order_relaxed),
              atomic_load_explicit(&vkr_ohos_perf_prepare_wait_max_us,
                                   memory_order_relaxed),
              atomic_load_explicit(&vkr_ohos_perf_prepare_reset_total_us,
                                   memory_order_relaxed),
              atomic_load_explicit(&vkr_ohos_perf_prepare_reset_max_us,
                                   memory_order_relaxed),
              atomic_load_explicit(&vkr_ohos_perf_prepare_dirty_total_us,
                                   memory_order_relaxed),
              atomic_load_explicit(&vkr_ohos_perf_prepare_dirty_max_us,
                                   memory_order_relaxed),
              atomic_load_explicit(&vkr_ohos_perf_prepare_buffer_total_us,
                                   memory_order_relaxed),
              atomic_load_explicit(&vkr_ohos_perf_prepare_buffer_max_us,
                                   memory_order_relaxed),
              atomic_load_explicit(&vkr_ohos_perf_prepare_uncovered_total_us,
                                   memory_order_relaxed),
              atomic_load_explicit(&vkr_ohos_perf_prepare_uncovered_max_us,
                                   memory_order_relaxed),
              atomic_load_explicit(&vkr_ohos_perf_prepare_end_total_us,
                                   memory_order_relaxed),
              atomic_load_explicit(&vkr_ohos_perf_prepare_end_max_us,
                                   memory_order_relaxed));
}

/* Maleoon can transiently return VK_ERROR_OUT_OF_HOST_MEMORY from a fence
 * status query even though the preceding queue submit succeeded and a repeat
 * query completes normally.  A one-shot error is fatal to Venus because it
 * poisons the ring.  Retry only this documented allocation result a small,
 * bounded number of times; all other errors, including DEVICE_LOST, remain
 * immediately visible to the guest. */
static VkResult
vkr_ohos_get_fence_status(struct vn_device_proc_table *vk,
                          VkDevice device,
                          VkFence fence,
                          uint32_t *retry_count,
                          bool *used_wait_fallback)
{
   VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
   uint32_t retry;

   *used_wait_fallback = false;

   for (retry = 0; retry < 4; retry++) {
      result = vk->GetFenceStatus(device, fence);
      if (result != VK_ERROR_OUT_OF_HOST_MEMORY)
         break;
      thrd_yield();
   }

   *retry_count = retry;

   /* A tight retry loop is insufficient once Maleoon's GetFenceStatus path
    * enters its persistent transient-OOM state.  Query the same fence once
    * through the non-blocking Vulkan wait path.  timeout=0 cannot stall the
    * renderer worker and is semantically equivalent to GetFenceStatus:
    * VK_TIMEOUT maps to VK_NOT_READY, while real device/allocation errors
    * remain visible to Venus. */
   if (result == VK_ERROR_OUT_OF_HOST_MEMORY) {
      const VkResult wait_result =
         vk->WaitForFences(device, 1, &fence, VK_TRUE, 0);
      *used_wait_fallback = true;
      result = wait_result == VK_TIMEOUT ? VK_NOT_READY : wait_result;
   }

   return result;
}
#endif

static struct vkr_queue_sync *
vkr_device_alloc_queue_sync(struct vkr_device *dev,
                            uint32_t fence_flags,
                            uint32_t ring_idx,
                            uint64_t fence_id)
{
   struct vn_device_proc_table *vk = &dev->proc_table;
   struct vkr_queue_sync *sync;

   mtx_lock(&dev->free_sync_mutex);
   if (list_is_empty(&dev->free_syncs)) {
      mtx_unlock(&dev->free_sync_mutex);

      sync = malloc(sizeof(*sync));
      if (!sync)
         return NULL;

      const VkExportFenceCreateInfo export_info = {
         .sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
         .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
      };
      const struct VkFenceCreateInfo create_info = {
         .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
         .pNext = dev->physical_device->KHR_external_fence_fd ? &export_info : NULL,
      };
      VkResult result =
         vk->CreateFence(dev->base.handle.device, &create_info, NULL, &sync->fence);
      if (result != VK_SUCCESS) {
         free(sync);
         vkr_log("failed to create sync fence for fence_id %" PRIu64, fence_id);
         return NULL;
      }
   } else {
      sync = LIST_ENTRY(struct vkr_queue_sync, dev->free_syncs.next, head);
      list_del(&sync->head);
      mtx_unlock(&dev->free_sync_mutex);

      vk->ResetFences(dev->base.handle.device, 1, &sync->fence);
   }

   sync->device_lost = false;
   sync->flags = fence_flags;
   sync->ring_idx = ring_idx;
   sync->fence_id = fence_id;

   return sync;
}

static void
vkr_device_free_queue_sync(struct vkr_device *dev, struct vkr_queue_sync *sync)
{
   mtx_lock(&dev->free_sync_mutex);
   list_addtail(&sync->head, &dev->free_syncs);
   mtx_unlock(&dev->free_sync_mutex);
}

static inline void
vkr_queue_sync_retire(struct vkr_queue *queue, struct vkr_queue_sync *sync)
{
   TRACE_FUNC();
   queue->context->retire_fence(queue->context->ctx_id, sync->ring_idx, sync->fence_id);
   vkr_device_free_queue_sync(queue->device, sync);
}

bool
vkr_queue_sync_submit(struct vkr_queue *queue,
                      uint32_t flags,
                      uint32_t ring_idx,
                      uint64_t fence_id)
{
   TRACE_FUNC();
   struct vkr_device *dev = queue->device;
   struct vn_device_proc_table *vk = &dev->proc_table;

   struct vkr_queue_sync *sync =
      vkr_device_alloc_queue_sync(dev, flags, ring_idx, fence_id);
   if (!sync)
      return false;

   mtx_lock(&queue->vk_mutex);
   VkResult result = vk->QueueSubmit(queue->base.handle.queue, 0, NULL, sync->fence);
   mtx_unlock(&queue->vk_mutex);

   if (result == VK_ERROR_DEVICE_LOST) {
      sync->device_lost = true;
      vkr_log("sync submit hit device lost for fence_id %" PRIu64, fence_id);
   } else if (result != VK_SUCCESS) {
      vkr_device_free_queue_sync(dev, sync);
      vkr_log("sync submit failed (vk ret %d) for fence_id %" PRIu64, result, fence_id);
      return false;
   }

   mtx_lock(&queue->sync_thread.mutex);
   list_addtail(&sync->head, &queue->sync_thread.syncs);
   cnd_signal(&queue->sync_thread.cond);
   mtx_unlock(&queue->sync_thread.mutex);

   return true;
}

static void
vkr_queue_sync_thread_fini(struct vkr_queue *queue)
{
   /* vkDeviceWaitIdle has been called */
   mtx_lock(&queue->sync_thread.mutex);
   queue->sync_thread.join = true;
   cnd_signal(&queue->sync_thread.cond);
   mtx_unlock(&queue->sync_thread.mutex);

   thrd_join(queue->sync_thread.thread, NULL);

   list_for_each_entry_safe (struct vkr_queue_sync, sync, &queue->sync_thread.syncs, head)
      vkr_queue_sync_retire(queue, sync);

   mtx_destroy(&queue->sync_thread.mutex);
   cnd_destroy(&queue->sync_thread.cond);
}

void
vkr_queue_destroy(struct vkr_context *ctx, struct vkr_queue *queue)
{
   vkr_queue_sync_thread_fini(queue);

#ifdef __OHOS__
   struct vn_device_proc_table *vk = &queue->device->proc_table;
   VkDevice device = queue->device->base.handle.device;
   for (uint32_t i = 0; i < VKR_WINEHUA_SHADOW_UPLOAD_SLOT_COUNT; i++) {
      struct vkr_shadow_upload_slot *slot = &queue->shadow_upload_slots[i];
      if (slot->fence)
         vk->DestroyFence(device, slot->fence, NULL);
      if (slot->pool)
         vk->DestroyCommandPool(device, slot->pool, NULL);
   }
   if (queue->shadow_upload_timeline)
      vk->DestroySemaphore(device, queue->shadow_upload_timeline, NULL);
   mtx_destroy(&queue->shadow_upload_mutex);
#endif

   list_del(&queue->base.track_head);

   mtx_destroy(&queue->vk_mutex);

   if (queue->ring_idx > 0)
      ctx->sync_queues[queue->ring_idx] = NULL;

   if (queue->base.id)
      vkr_context_remove_object(ctx, &queue->base);
   else
      free(queue);
}

static int
vkr_queue_thread(void *arg)
{
   struct vkr_queue *queue = arg;
   struct vkr_context *ctx = queue->context;
   struct vkr_device *dev = queue->device;
   struct vn_device_proc_table *vk = &dev->proc_table;
   const uint64_t ns_per_sec = 1000000000llu;
   char thread_name[16];

   snprintf(thread_name, ARRAY_SIZE(thread_name), "vkr-queue-%d", ctx->ctx_id);
   u_thread_setname(thread_name);

   mtx_lock(&queue->sync_thread.mutex);
   while (true) {
      while (list_is_empty(&queue->sync_thread.syncs) && !queue->sync_thread.join)
         cnd_wait(&queue->sync_thread.cond, &queue->sync_thread.mutex);

      if (queue->sync_thread.join)
         break;

      struct vkr_queue_sync *sync =
         list_first_entry(&queue->sync_thread.syncs, struct vkr_queue_sync, head);

      mtx_unlock(&queue->sync_thread.mutex);

      VkResult result;
      if (sync->device_lost) {
         result = VK_ERROR_DEVICE_LOST;
      } else {
         result = vk->WaitForFences(dev->base.handle.device, 1, &sync->fence, true,
                                    ns_per_sec * 3);
      }

      mtx_lock(&queue->sync_thread.mutex);

      if (result == VK_TIMEOUT)
         continue;

      list_del(&sync->head);

      vkr_queue_sync_retire(queue, sync);
   }
   mtx_unlock(&queue->sync_thread.mutex);

   return 0;
}

static int
vkr_queue_sync_thread_init(struct vkr_queue *queue)
{
   STATIC_ASSERT(thrd_success == 0);

   int ret = mtx_init(&queue->sync_thread.mutex, mtx_plain);
   if (ret != thrd_success)
      return ret;

   ret = cnd_init(&queue->sync_thread.cond);
   if (ret != thrd_success)
      goto fail_cnd_init;

   list_inithead(&queue->sync_thread.syncs);

   ret = thrd_create(&queue->sync_thread.thread, vkr_queue_thread, queue);
   if (ret != thrd_success)
      goto fail_thrd_create;

   return 0;

fail_thrd_create:
   mtx_destroy(&queue->sync_thread.mutex);
fail_cnd_init:
   cnd_destroy(&queue->sync_thread.cond);
   return ret;
}

struct vkr_queue *
vkr_queue_create(struct vkr_context *ctx,
                 struct vkr_device *dev,
                 VkDeviceQueueCreateFlags flags,
                 uint32_t family,
                 uint32_t index,
                 VkQueue handle)
{
   /* id is set to 0 until vkr_queue_assign_object_id */
   struct vkr_queue *queue = vkr_object_alloc(sizeof(*queue), VK_OBJECT_TYPE_QUEUE, 0);
   if (!queue)
      return NULL;

   queue->base.handle.queue = handle;

   queue->context = ctx;
   queue->device = dev;
   queue->flags = flags;
   queue->family = family;
   queue->index = index;

   if (mtx_init(&queue->vk_mutex, mtx_plain)) {
      free(queue);
      return NULL;
   }

#ifdef __OHOS__
   if (mtx_init(&queue->shadow_upload_mutex, mtx_plain)) {
      mtx_destroy(&queue->vk_mutex);
      free(queue);
      return NULL;
   }
   for (uint32_t i = 0; i < VKR_WINEHUA_SHADOW_UPLOAD_SLOT_COUNT; i++) {
      queue->shadow_upload_slots[i].pool = VK_NULL_HANDLE;
      queue->shadow_upload_slots[i].command = VK_NULL_HANDLE;
      queue->shadow_upload_slots[i].fence = VK_NULL_HANDLE;
      queue->shadow_upload_slots[i].in_flight = false;
      queue->shadow_upload_slots[i].retire_value = 0;
   }
   queue->shadow_upload_slot = 0;
   queue->shadow_upload_timeline = VK_NULL_HANDLE;
   queue->shadow_upload_next_value = 0;
   queue->shadow_upload_inline = vkr_ohos_gpu_upload_inline_enabled();
   queue->shadow_upload_prepared = false;
   queue->shadow_upload_bytes = 0;
   queue->shadow_upload_updates = 0;
   queue->shadow_upload_ranges = 0;
   queue->shadow_upload_buffers = 0;
   queue->shadow_upload_uniform_buffers = 0;
   queue->shadow_upload_storage_buffers = 0;
   queue->winehua_perf_summary = vkr_ohos_perf_summary_enabled();
   queue->winehua_perf_sample_interval = vkr_ohos_perf_sample_interval();
   queue->winehua_frame_timeline_interval =
      vkr_ohos_frame_timeline_interval();
   queue->winehua_frame_timeline_active = false;
   vkr_ohos_frame_timeline_reset(queue, 0);
   atomic_init(&queue->winehua_perf_last_submit_end_ns, 0);
   atomic_init(&queue->winehua_perf_last_sample_end_ns, 0);
   atomic_init(&queue->winehua_perf_last_sample_submit_id, 0);
   queue->shadow_upload_wait_us = 0;
   queue->shadow_upload_reset_begin_us = 0;
   queue->shadow_upload_dirty_scan_us = 0;
   queue->shadow_upload_buffer_record_us = 0;
   queue->shadow_upload_uncovered_scan_us = 0;
   queue->shadow_upload_end_us = 0;
   if (queue->shadow_upload_inline)
      vkr_log("WineHua shadow GPU upload inline-submit enabled");
#endif

   if (vkr_queue_sync_thread_init(queue)) {
#ifdef __OHOS__
      mtx_destroy(&queue->shadow_upload_mutex);
#endif
      mtx_destroy(&queue->vk_mutex);
      free(queue);
      return NULL;
   }

   list_inithead(&queue->base.track_head);

   return queue;
}

static bool MUST_CHECK
vkr_queue_assign_ring_idx(struct vkr_context *ctx,
                          struct vkr_queue *queue,
                          const VkDeviceQueueTimelineInfoMESA *timeline_info)
{
   if (unlikely(!timeline_info)) {
      vkr_log("missing VkDeviceQueueTimelineInfoMESA");
      return false;
   }

   const uint32_t ring_idx = timeline_info->ringIdx;
   if (unlikely(!ring_idx || ring_idx >= ARRAY_SIZE(ctx->sync_queues))) {
      vkr_log("invalid ring_idx %u", ring_idx);
      return false;
   }

   if (unlikely(ctx->sync_queues[ring_idx])) {
      vkr_log("sync_queue is already bound to ring_idx %u", ring_idx);
      return false;
   }

   queue->ring_idx = ring_idx;
   ctx->sync_queues[ring_idx] = queue;
   return true;
}

static void
vkr_queue_assign_object_id(struct vkr_context *ctx,
                           struct vkr_queue *queue,
                           vkr_object_id id)
{
   if (queue->base.id) {
      if (queue->base.id != id)
         vkr_context_set_fatal(ctx);
      return;
   }
   if (!vkr_context_validate_object_id(ctx, id))
      return;

   queue->base.id = id;

   vkr_context_add_object(ctx, &queue->base);
}

static struct vkr_queue *
vkr_device_lookup_queue(struct vkr_device *dev,
                        VkDeviceQueueCreateFlags flags,
                        uint32_t family,
                        uint32_t index)
{
   list_for_each_entry (struct vkr_queue, queue, &dev->queues, base.track_head) {
      if (queue->flags == flags && queue->family == family && queue->index == index)
         return queue;
   }

   return NULL;
}

static void
vkr_dispatch_vkGetDeviceQueue2(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkGetDeviceQueue2 *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_device *dev = vkr_device_from_handle(args->device);

   struct vkr_queue *queue = vkr_device_lookup_queue(dev, args->pQueueInfo->flags,
                                                     args->pQueueInfo->queueFamilyIndex,
                                                     args->pQueueInfo->queueIndex);
   if (!queue) {
      vkr_context_set_fatal(ctx);
      return;
   }

   /* Venus driver implementation is required to retrieve device queue only once to avoid
    * overriding vkr_queue object id assignment.
    */
   if (queue->base.id) {
      vkr_log("invalid to reinitialize vkr_queue");
      vkr_context_set_fatal(ctx);
      return;
   }

   const VkDeviceQueueTimelineInfoMESA *timeline_info = vkr_find_struct(
      args->pQueueInfo->pNext, VK_STRUCTURE_TYPE_DEVICE_QUEUE_TIMELINE_INFO_MESA);
   if (!vkr_queue_assign_ring_idx(ctx, queue, timeline_info)) {
      vkr_context_set_fatal(ctx);
      return;
   }

   const vkr_object_id id =
      vkr_cs_handle_load_id((const void **)args->pQueue, VK_OBJECT_TYPE_QUEUE);
   vkr_queue_assign_object_id(ctx, queue, id);
}

static void
vkr_dispatch_vkGetDeviceQueue(struct vn_dispatch_context *dispatch,
                              UNUSED struct vn_command_vkGetDeviceQueue *args)
{
   /* Must use vkGetDeviceQueue2 for proper device queue initialization. */
   struct vkr_context *ctx = dispatch->data;
   vkr_context_set_fatal(ctx);
   return;
}

#ifdef __OHOS__
#define VKR_WINEHUA_INLINE_MAX_GUEST_SUBMITS 64u

static VkResult
vkr_ohos_queue_submit_inline_upload(struct vkr_queue *queue,
                                    struct vn_device_proc_table *vk,
                                    const struct vn_command_vkQueueSubmit *args)
{
   assert(queue->shadow_upload_prepared);
   assert(args->submitCount <= VKR_WINEHUA_INLINE_MAX_GUEST_SUBMITS);

   const uint32_t slot_index = queue->shadow_upload_slot;
   struct vkr_shadow_upload_slot *slot =
      &queue->shadow_upload_slots[slot_index];
   const uint64_t retire_value = ++queue->shadow_upload_next_value;
   const VkTimelineSemaphoreSubmitInfo timeline_info = {
      .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
      .signalSemaphoreValueCount = 1,
      .pSignalSemaphoreValues = &retire_value,
   };
   const VkSubmitInfo upload_submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &slot->command,
   };
   const VkSubmitInfo retire_submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .pNext = &timeline_info,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &queue->shadow_upload_timeline,
   };
   VkSubmitInfo submits[VKR_WINEHUA_INLINE_MAX_GUEST_SUBMITS + 2];
   submits[0] = upload_submit;
   if (args->submitCount)
      memcpy(&submits[1], args->pSubmits,
             sizeof(*args->pSubmits) * args->submitCount);
   submits[args->submitCount + 1] = retire_submit;

   const VkResult result = vk->QueueSubmit(
      args->queue, args->submitCount + 2, submits, args->fence);
   queue->shadow_upload_prepared = false;
   if (result == VK_SUCCESS) {
      slot->in_flight = true;
      slot->retire_value = retire_value;
      queue->shadow_upload_slot =
         (slot_index + 1) % VKR_WINEHUA_SHADOW_UPLOAD_SLOT_COUNT;
   }
   return result;
}

static VkResult
vkr_ohos_queue_submit2_inline_upload(struct vkr_queue *queue,
                                     struct vn_device_proc_table *vk,
                                     const struct vn_command_vkQueueSubmit2 *args)
{
   assert(queue->shadow_upload_prepared);
   assert(args->submitCount <= VKR_WINEHUA_INLINE_MAX_GUEST_SUBMITS);

   const uint32_t slot_index = queue->shadow_upload_slot;
   struct vkr_shadow_upload_slot *slot =
      &queue->shadow_upload_slots[slot_index];
   const uint64_t retire_value = ++queue->shadow_upload_next_value;
   const VkCommandBufferSubmitInfo upload_command = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = slot->command,
      .deviceMask = 0,
   };
   const VkSemaphoreSubmitInfo retire_signal = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = queue->shadow_upload_timeline,
      .value = retire_value,
      .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      .deviceIndex = 0,
   };
   const VkSubmitInfo2 upload_submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .commandBufferInfoCount = 1,
      .pCommandBufferInfos = &upload_command,
   };
   const VkSubmitInfo2 retire_submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .signalSemaphoreInfoCount = 1,
      .pSignalSemaphoreInfos = &retire_signal,
   };
   VkSubmitInfo2 submits[VKR_WINEHUA_INLINE_MAX_GUEST_SUBMITS + 2];
   submits[0] = upload_submit;
   if (args->submitCount)
      memcpy(&submits[1], args->pSubmits,
             sizeof(*args->pSubmits) * args->submitCount);
   submits[args->submitCount + 1] = retire_submit;

   const VkResult result = vk->QueueSubmit2(
      args->queue, args->submitCount + 2, submits, args->fence);
   queue->shadow_upload_prepared = false;
   if (result == VK_SUCCESS) {
      slot->in_flight = true;
      slot->retire_value = retire_value;
      queue->shadow_upload_slot =
         (slot_index + 1) % VKR_WINEHUA_SHADOW_UPLOAD_SLOT_COUNT;
   }
   return result;
}

static VkResult
vkr_ohos_wait_deferred_shadow_host_copy(struct vkr_context *ctx,
                                           struct vkr_queue *queue,
                                           struct vn_device_proc_table *vk)
{
   const char *serialize_value =
      os_get_option("VKR_WINEHUA_GPU_UPLOAD_SERIALIZE");
   const bool serialize_upload = queue->shadow_upload_prepared &&
      serialize_value && serialize_value[0] == '1' && !serialize_value[1];
   const bool deferred_fallback =
      vkr_device_memory_requires_deferred_host_wait(ctx, queue->device);
   if (!serialize_upload && !deferred_fallback)
      return VK_SUCCESS;

   mtx_lock(&queue->vk_mutex);
   const VkResult result = vk->QueueWaitIdle(queue->base.handle.queue);
   mtx_unlock(&queue->vk_mutex);
   const uint64_t wait_count = atomic_fetch_add_explicit(
      &vkr_ohos_shadow_queue_wait_count, 1, memory_order_relaxed) + 1;
   if (result != VK_SUCCESS || wait_count <= 8 || !(wait_count % 120))
      vkr_log("OHOS shadow queue wait count=%" PRIu64
              " reason=%s result=%d",
              wait_count,
              serialize_upload ? "serialized-upload" : "deferred-host-copy",
              result);
   return result;
}

#endif

static void
vkr_dispatch_vkQueueSubmit(struct vn_dispatch_context *dispatch,
                           struct vn_command_vkQueueSubmit *args)
{
   TRACE_FUNC();
   struct vkr_queue *queue = vkr_queue_from_handle(args->queue);
   struct vn_device_proc_table *vk = &queue->device->proc_table;

#ifdef __OHOS__
   const uint64_t submit_id =
      atomic_fetch_add_explicit(&vkr_ohos_queue_submit_count, 1, memory_order_relaxed) + 1;
   if (vkr_ohos_frame_assoc_trace_enabled()) {
      for (uint32_t i = 0; i < args->submitCount; i++) {
         const VkSubmitInfo *submit = &args->pSubmits[i];
         for (uint32_t j = 0; j < submit->commandBufferCount; j++) {
            const struct vkr_command_buffer *cmd =
               vkr_command_buffer_from_handle(submit->pCommandBuffers[j]);
            vkr_log("WineHuaFrameAssoc: queue-submit ctx=%u submit=%" PRIu64
                    " queueId=%" PRIu64 " hostQueue=0x%" PRIxPTR
                    " batch=%u cmdIndex=%u guestCmd=0x%" PRIxPTR
                    " cmdId=%" PRIu64
                    " hostCmd=0x%" PRIxPTR,
                    queue->context ? queue->context->ctx_id : 0,
                    submit_id, queue->base.id,
                    (uintptr_t)queue->base.handle.queue, i, j,
                    (uintptr_t)submit->pCommandBuffers[j],
                    cmd ? cmd->base.id : 0,
                    cmd ? (uintptr_t)cmd->base.handle.command_buffer : 0);
         }
      }
   }
#endif

   vn_replace_vkQueueSubmit_args_handle(args);
#ifdef __OHOS__
   const bool perf_trace = vkr_ohos_perf_trace_enabled();
   const bool perf_summary = queue->winehua_perf_summary;
   const bool perf_sample = queue->winehua_perf_sample_interval &&
      !(submit_id % queue->winehua_perf_sample_interval);
   const bool frame_timeline = queue->winehua_frame_timeline_active;
   const bool perf_account = perf_summary || perf_sample || frame_timeline;
   const bool perf_timing = perf_trace || perf_account;
   const uint64_t submit_start_ns = perf_account ? vkr_ohos_queue_now_ns() : 0;
   const uint64_t previous_submit_end_ns = perf_summary
      ? atomic_load_explicit(&queue->winehua_perf_last_submit_end_ns,
                             memory_order_relaxed)
      : 0;
   const uint64_t submit_gap_us = perf_summary && previous_submit_end_ns &&
      submit_start_ns >= previous_submit_end_ns
      ? (submit_start_ns - previous_submit_end_ns) / 1000 : 0;
   const uint64_t prepare_start_ns = submit_start_ns;
   const bool gpu_upload = vkr_device_memory_gpu_upload_enabled(queue->device);
   const bool log_submit = perf_trace &&
      (submit_id <= 8 || !(submit_id % 120));
   if (log_submit)
      vkr_log("OHOS queue submit begin id=%" PRIu64 " submits=%u", submit_id,
              args->submitCount);
   VkResult upload_prepare_result = VK_SUCCESS;
   if (gpu_upload) {
      mtx_lock(&queue->shadow_upload_mutex);
      vkr_device_memory_shadow_generation_begin(dispatch->data, true);
      upload_prepare_result =
         vkr_device_memory_prepare_shadow_upload(dispatch->data, queue,
                                                 perf_account, submit_id);
      if (perf_summary)
         vkr_ohos_perf_record_prepare_phases(queue);
   }
   const uint64_t prepare_end_ns = perf_account ? vkr_ohos_queue_now_ns() : 0;
   if (upload_prepare_result != VK_SUCCESS)
      vkr_device_memory_disable_shadow_upload_coverage(dispatch->data);
   const VkResult deferred_host_wait_result =
      vkr_ohos_wait_deferred_shadow_host_copy(
         dispatch->data, queue, vk);
   if (deferred_host_wait_result != VK_SUCCESS) {
      if (gpu_upload) {
         vkr_device_memory_shadow_generation_end(dispatch->data);
         mtx_unlock(&queue->shadow_upload_mutex);
      }
      args->ret = deferred_host_wait_result;
      return;
   }
#endif
#ifdef __OHOS__
   struct vkr_shadow_sync_stats shadow_stats =
      vkr_device_memory_sync_shadows_to_host(dispatch->data);
   if (gpu_upload)
      vkr_device_memory_shadow_generation_end(dispatch->data);
   const uint64_t sync_end_ns = perf_account ? vkr_ohos_queue_now_ns() : 0;
   const bool upload_prepared = gpu_upload && queue->shadow_upload_prepared;
   const bool inline_upload = upload_prepared && queue->shadow_upload_inline &&
      args->submitCount <= VKR_WINEHUA_INLINE_MAX_GUEST_SUBMITS;
   const uint32_t upload_updates = upload_prepared
      ? queue->shadow_upload_updates : 0;
   const uint32_t upload_ranges = upload_prepared
      ? queue->shadow_upload_ranges : 0;
   const uint32_t upload_buffers = upload_prepared
      ? queue->shadow_upload_buffers : 0;
   const uint32_t upload_uniform_buffers = upload_prepared
      ? queue->shadow_upload_uniform_buffers : 0;
   const uint32_t upload_storage_buffers = upload_prepared
      ? queue->shadow_upload_storage_buffers : 0;
   const uint64_t upload_bytes = upload_prepared
      ? queue->shadow_upload_bytes : 0;
   if (log_submit)
      vkr_log("OHOS queue submit shadows synced id=%" PRIu64, submit_id);
#else
   vkr_device_memory_sync_shadows_to_host(dispatch->data);
#endif

#ifdef __OHOS__
   const uint64_t lock_start_ns = perf_timing ? vkr_ohos_queue_now_ns() : 0;
#endif
   mtx_lock(&queue->vk_mutex);
#ifdef __OHOS__
   const uint64_t lock_acquired_ns = perf_timing ? vkr_ohos_queue_now_ns() : 0;
   VkResult upload_submit_result = upload_prepare_result;
   if (upload_submit_result == VK_SUCCESS && !inline_upload)
      upload_submit_result = vkr_device_memory_submit_shadow_upload(queue);
   if (upload_submit_result != VK_SUCCESS && gpu_upload && !inline_upload) {
      /* A failed private upload must fall back to the original mapped-memory
       * path before the guest submission is allowed to execute. */
      vkr_device_memory_disable_shadow_upload_coverage(dispatch->data);
      const struct vkr_shadow_sync_stats retry_stats =
         vkr_device_memory_sync_shadows_to_host(dispatch->data);
      shadow_stats.bytes += retry_stats.bytes;
      shadow_stats.copies += retry_stats.copies;
      shadow_stats.cache_ops += retry_stats.cache_ops;
      shadow_stats.scanned += retry_stats.scanned;
      shadow_stats.elapsed_us += retry_stats.elapsed_us;
   }
   const uint64_t upload_end_ns = perf_account ? vkr_ohos_queue_now_ns() : 0;
#endif
#ifdef __OHOS__
   if (inline_upload) {
      args->ret = vkr_ohos_queue_submit_inline_upload(queue, vk, args);
      upload_submit_result = args->ret;
   } else {
#endif
      args->ret = vk->QueueSubmit(
         args->queue, args->submitCount, args->pSubmits, args->fence);
#ifdef __OHOS__
   }
#endif
#ifdef __OHOS__
   const uint64_t driver_end_ns = perf_timing ? vkr_ohos_queue_now_ns() : 0;
   if (perf_summary)
      atomic_store_explicit(&queue->winehua_perf_last_submit_end_ns,
                            driver_end_ns, memory_order_relaxed);
   if (frame_timeline) {
      const uint64_t frame_lock_wait_us = lock_acquired_ns >= lock_start_ns
         ? (lock_acquired_ns - lock_start_ns) / 1000 : 0;
      const uint64_t prepare_us = prepare_end_ns >= prepare_start_ns
         ? (prepare_end_ns - prepare_start_ns) / 1000 : 0;
      const uint64_t sync_us = sync_end_ns >= prepare_end_ns
         ? (sync_end_ns - prepare_end_ns) / 1000 : 0;
      const uint64_t upload_us = upload_end_ns >= lock_acquired_ns
         ? (upload_end_ns - lock_acquired_ns) / 1000 : 0;
      const uint64_t app_driver_us = driver_end_ns >= upload_end_ns
         ? (driver_end_ns - upload_end_ns) / 1000 : 0;
      const uint64_t total_us = driver_end_ns >= submit_start_ns
         ? (driver_end_ns - submit_start_ns) / 1000 : 0;
      /* The presenter reports and rearms this state under the same mutex.
       * Account before unlocking so a present cannot publish a partial frame. */
      vkr_ohos_frame_timeline_account(queue, args, &shadow_stats,
                                      upload_bytes, upload_ranges,
                                      prepare_us, sync_us, upload_us,
                                      frame_lock_wait_us, app_driver_us,
                                      total_us);
   }
#endif
   mtx_unlock(&queue->vk_mutex);
#ifdef __OHOS__
   if (gpu_upload)
      mtx_unlock(&queue->shadow_upload_mutex);
   const uint64_t lock_wait_us = perf_timing && lock_acquired_ns >= lock_start_ns
                                    ? (lock_acquired_ns - lock_start_ns) / 1000
                                    : 0;
   const uint64_t driver_us = perf_timing && driver_end_ns >= lock_acquired_ns
                                 ? (driver_end_ns - lock_acquired_ns) / 1000
                                 : 0;
   if (perf_summary) {
      const uint64_t prepare_us = prepare_end_ns >= prepare_start_ns
         ? (prepare_end_ns - prepare_start_ns) / 1000 : 0;
      const uint64_t sync_us = sync_end_ns >= prepare_end_ns
         ? (sync_end_ns - prepare_end_ns) / 1000 : 0;
      const uint64_t upload_us = upload_end_ns >= lock_acquired_ns
         ? (upload_end_ns - lock_acquired_ns) / 1000 : 0;
      const uint64_t app_driver_us = driver_end_ns >= upload_end_ns
         ? (driver_end_ns - upload_end_ns) / 1000 : 0;
      const uint64_t total_us = driver_end_ns >= submit_start_ns
         ? (driver_end_ns - submit_start_ns) / 1000 : 0;

      atomic_fetch_add_explicit(&vkr_ohos_perf_submit_infos,
                                args->submitCount, memory_order_relaxed);
      atomic_fetch_add_explicit(&vkr_ohos_perf_shadow_scanned,
                                shadow_stats.scanned, memory_order_relaxed);
      atomic_fetch_add_explicit(&vkr_ohos_perf_shadow_copies,
                                shadow_stats.copies, memory_order_relaxed);
      atomic_fetch_add_explicit(&vkr_ohos_perf_shadow_bytes,
                                shadow_stats.bytes, memory_order_relaxed);
      atomic_fetch_add_explicit(&vkr_ohos_perf_prepare_total_us,
                                prepare_us, memory_order_relaxed);
      atomic_fetch_add_explicit(&vkr_ohos_perf_sync_total_us,
                                sync_us, memory_order_relaxed);
      atomic_fetch_add_explicit(&vkr_ohos_perf_lock_total_us,
                                lock_wait_us, memory_order_relaxed);
      atomic_fetch_add_explicit(&vkr_ohos_perf_upload_total_us,
                                upload_us, memory_order_relaxed);
      atomic_fetch_add_explicit(&vkr_ohos_perf_driver_total_us,
                                app_driver_us, memory_order_relaxed);
      atomic_fetch_add_explicit(&vkr_ohos_perf_total_us,
                                total_us, memory_order_relaxed);
      atomic_fetch_add_explicit(&vkr_ohos_perf_submit_gap_total_us,
                                submit_gap_us, memory_order_relaxed);
      if (upload_prepared) {
         atomic_fetch_add_explicit(&vkr_ohos_perf_upload_submit_count, 1,
                                   memory_order_relaxed);
         atomic_fetch_add_explicit(&vkr_ohos_perf_upload_buffers,
                                   upload_buffers, memory_order_relaxed);
         atomic_fetch_add_explicit(&vkr_ohos_perf_upload_uniform_buffers,
                                   upload_uniform_buffers,
                                   memory_order_relaxed);
         atomic_fetch_add_explicit(&vkr_ohos_perf_upload_storage_buffers,
                                   upload_storage_buffers,
                                   memory_order_relaxed);
         atomic_fetch_add_explicit(&vkr_ohos_perf_upload_ranges,
                                   upload_ranges, memory_order_relaxed);
         atomic_fetch_add_explicit(&vkr_ohos_perf_upload_updates,
                                   upload_updates, memory_order_relaxed);
         atomic_fetch_add_explicit(&vkr_ohos_perf_upload_bytes,
                                   upload_bytes, memory_order_relaxed);
      }
      atomic_fetch_add_explicit(&vkr_ohos_perf_upload_skipped_bytes,
                                shadow_stats.gpu_upload_skipped_bytes,
                                memory_order_relaxed);
      atomic_fetch_add_explicit(&vkr_ohos_perf_upload_skipped_copies,
                                shadow_stats.gpu_upload_skipped_copies,
                                memory_order_relaxed);
      vkr_ohos_atomic_max(&vkr_ohos_perf_prepare_max_us, prepare_us);
      vkr_ohos_atomic_max(&vkr_ohos_perf_sync_max_us, sync_us);
      vkr_ohos_atomic_max(&vkr_ohos_perf_lock_max_us, lock_wait_us);
      vkr_ohos_atomic_max(&vkr_ohos_perf_upload_max_us, upload_us);
      vkr_ohos_atomic_max(&vkr_ohos_perf_driver_max_us, app_driver_us);
      vkr_ohos_atomic_max(&vkr_ohos_perf_total_max_us, total_us);
      vkr_ohos_atomic_max(&vkr_ohos_perf_submit_gap_max_us, submit_gap_us);

      if (submit_id <= 8 || (perf_summary ? !(submit_id % 60) : !(submit_id % 600))) {
         const uint64_t status_count = atomic_load_explicit(
            &vkr_ohos_fence_status_count, memory_order_relaxed);
         const uint64_t status_total_us = atomic_load_explicit(
            &vkr_ohos_fence_status_total_us, memory_order_relaxed);
         const uint64_t wait_count = atomic_load_explicit(
            &vkr_ohos_fence_wait_count, memory_order_relaxed);
         const uint64_t wait_total_us = atomic_load_explicit(
            &vkr_ohos_fence_wait_total_us, memory_order_relaxed);
         vkr_log("WineHuaPerf: queue calls=%" PRIu64
                 " submit_infos=%" PRIuFAST64
                 " shadow_scanned=%" PRIuFAST64
                 " shadow_copies=%" PRIuFAST64
                 " shadow_bytes=%" PRIuFAST64
                 " prepare_us=%" PRIuFAST64 "/%" PRIuFAST64
                 " sync_us=%" PRIuFAST64 "/%" PRIuFAST64
                 " lock_us=%" PRIuFAST64 "/%" PRIuFAST64
                 " upload_submits=%" PRIuFAST64
                 " upload_buffers=%" PRIuFAST64
                 " upload_uniform_buffers=%" PRIuFAST64
                 " upload_storage_buffers=%" PRIuFAST64
                 " upload_ranges=%" PRIuFAST64
                 " upload_updates=%" PRIuFAST64
                 " upload_bytes=%" PRIuFAST64
                 " upload_skipped_bytes=%" PRIuFAST64
                 " upload_skipped_copies=%" PRIuFAST64
                 " upload_us=%" PRIuFAST64 "/%" PRIuFAST64
                 " driver_us=%" PRIuFAST64 "/%" PRIuFAST64
                 " total_us=%" PRIuFAST64 "/%" PRIuFAST64
                 " submit_gap_us=%" PRIuFAST64 "/%" PRIuFAST64
                 " fence_status=%" PRIu64 "/%" PRIu64
                 " fence_wait=%" PRIu64 "/%" PRIu64 "/%" PRIuFAST64,
                 submit_id,
                 atomic_load_explicit(&vkr_ohos_perf_submit_infos,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_shadow_scanned,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_shadow_copies,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_shadow_bytes,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_prepare_total_us,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_prepare_max_us,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_sync_total_us,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_sync_max_us,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_lock_total_us,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_lock_max_us,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_upload_submit_count,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_upload_buffers,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_upload_uniform_buffers,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_upload_storage_buffers,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_upload_ranges,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_upload_updates,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_upload_bytes,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_upload_skipped_bytes,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_upload_skipped_copies,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_upload_total_us,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_upload_max_us,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_driver_total_us,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_driver_max_us,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_total_us,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_total_max_us,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_submit_gap_total_us,
                                      memory_order_relaxed),
                 atomic_load_explicit(&vkr_ohos_perf_submit_gap_max_us,
                                      memory_order_relaxed),
                 status_count, status_total_us,
                 wait_count, wait_total_us,
                 atomic_load_explicit(&vkr_ohos_fence_wait_max_us,
                                      memory_order_relaxed));
      }
   } else if (perf_sample) {
      const uint64_t prepare_us = prepare_end_ns >= submit_start_ns
         ? (prepare_end_ns - submit_start_ns) / 1000 : 0;
      const uint64_t sync_us = sync_end_ns >= prepare_end_ns
         ? (sync_end_ns - prepare_end_ns) / 1000 : 0;
      const uint64_t upload_us = upload_end_ns >= lock_acquired_ns
         ? (upload_end_ns - lock_acquired_ns) / 1000 : 0;
      const uint64_t app_driver_us = driver_end_ns >= upload_end_ns
         ? (driver_end_ns - upload_end_ns) / 1000 : 0;
      const uint64_t total_us = driver_end_ns >= submit_start_ns
         ? (driver_end_ns - submit_start_ns) / 1000 : 0;
      const uint64_t previous_end_ns = atomic_load_explicit(
         &queue->winehua_perf_last_sample_end_ns, memory_order_relaxed);
      const uint64_t previous_submit_id = atomic_load_explicit(
         &queue->winehua_perf_last_sample_submit_id, memory_order_relaxed);
      const uint64_t window_us = previous_end_ns && driver_end_ns >= previous_end_ns
         ? (driver_end_ns - previous_end_ns) / 1000 : 0;
      const uint64_t window_submits = previous_submit_id
         ? submit_id - previous_submit_id : 0;
      atomic_store_explicit(&queue->winehua_perf_last_sample_end_ns,
                            driver_end_ns, memory_order_relaxed);
      atomic_store_explicit(&queue->winehua_perf_last_sample_submit_id,
                            submit_id, memory_order_relaxed);
      vkr_log("WineHuaPerfSample: submit=%" PRIu64
              " window_submits=%" PRIu64 " window_us=%" PRIu64
              " infos=%u shadow_scanned=%" PRIu64
              " shadow_copies=%" PRIu64 " shadow_bytes=%" PRIu64
              " prepare_us=%" PRIu64 " sync_us=%" PRIu64
              " lock_us=%" PRIu64 " upload_us=%" PRIu64
              " driver_us=%" PRIu64 " total_us=%" PRIu64
              " upload_bytes=%" PRIu64 " result=%d",
              submit_id, window_submits, window_us, args->submitCount,
              shadow_stats.scanned, shadow_stats.copies, shadow_stats.bytes,
              prepare_us, sync_us, lock_wait_us, upload_us, app_driver_us,
              total_us, upload_bytes, args->ret);
   }
   const bool slow_submit = perf_trace &&
      (lock_wait_us >= 1000 || driver_us >= 1000);
   const uint64_t slow_id = slow_submit
                               ? atomic_fetch_add_explicit(
                                    &vkr_ohos_queue_submit_slow_count, 1,
                                    memory_order_relaxed) + 1
                               : 0;
   if (perf_trace && (log_submit ||
       (slow_submit && (slow_id <= 8 || !(slow_id % 60)))))
      vkr_log("OHOS queue submit phases id=%" PRIu64
              " slow=%" PRIu64 " lock_wait_us=%" PRIu64
              " driver_us=%" PRIu64 " upload_result=%d",
              submit_id, slow_id, lock_wait_us, driver_us,
              upload_submit_result);
   if (log_submit || args->ret != VK_SUCCESS)
      vkr_log("OHOS queue submit end id=%" PRIu64 " result=%d", submit_id,
              args->ret);
#endif
}

static void
vkr_dispatch_vkQueueBindSparse(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkQueueBindSparse *args)
{
   TRACE_FUNC();
   struct vkr_queue *queue = vkr_queue_from_handle(args->queue);
   struct vn_device_proc_table *vk = &queue->device->proc_table;

   vn_replace_vkQueueBindSparse_args_handle(args);
#ifdef __OHOS__
   const VkResult deferred_host_wait_result =
      vkr_ohos_wait_deferred_shadow_host_copy(
         dispatch->data, queue, vk);
   if (deferred_host_wait_result != VK_SUCCESS) {
      args->ret = deferred_host_wait_result;
      return;
   }
#endif
   vkr_device_memory_sync_shadows_to_host(dispatch->data);

   mtx_lock(&queue->vk_mutex);
   args->ret =
      vk->QueueBindSparse(args->queue, args->bindInfoCount, args->pBindInfo, args->fence);
   mtx_unlock(&queue->vk_mutex);
}

static void
vkr_dispatch_vkQueueWaitIdle(struct vn_dispatch_context *dispatch,
                             UNUSED struct vn_command_vkQueueWaitIdle *args)
{
   struct vkr_context *ctx = dispatch->data;
   /* no blocking call */
   vkr_context_set_fatal(ctx);
}

static void
vkr_dispatch_vkQueueSubmit2(struct vn_dispatch_context *dispatch,
                            struct vn_command_vkQueueSubmit2 *args)
{
   TRACE_FUNC();
   struct vkr_queue *queue = vkr_queue_from_handle(args->queue);
   struct vn_device_proc_table *vk = &queue->device->proc_table;

   vn_replace_vkQueueSubmit2_args_handle(args);
#ifdef __OHOS__
   const uint64_t submit_id =
      atomic_fetch_add_explicit(&vkr_ohos_queue_submit_count, 1,
                                memory_order_relaxed) + 1;
   const bool gpu_upload = vkr_device_memory_gpu_upload_enabled(queue->device);
   const bool perf_summary = queue->winehua_perf_summary;
   VkResult upload_prepare_result = VK_SUCCESS;
   if (gpu_upload) {
      mtx_lock(&queue->shadow_upload_mutex);
      upload_prepare_result =
         vkr_device_memory_prepare_shadow_upload(dispatch->data, queue,
                                                 perf_summary, submit_id);
      if (perf_summary)
         vkr_ohos_perf_record_prepare_phases(queue);
   }
   if (upload_prepare_result != VK_SUCCESS)
      vkr_device_memory_disable_shadow_upload_coverage(dispatch->data);
   const VkResult deferred_host_wait_result =
      vkr_ohos_wait_deferred_shadow_host_copy(
         dispatch->data, queue, vk);
   if (deferred_host_wait_result != VK_SUCCESS) {
      if (gpu_upload)
         mtx_unlock(&queue->shadow_upload_mutex);
      args->ret = deferred_host_wait_result;
      return;
   }
#endif
   vkr_device_memory_sync_shadows_to_host(dispatch->data);
#ifdef __OHOS__
   const bool upload_prepared = gpu_upload && queue->shadow_upload_prepared;
   const bool inline_upload = upload_prepared && queue->shadow_upload_inline &&
      args->submitCount <= VKR_WINEHUA_INLINE_MAX_GUEST_SUBMITS;
#endif

   mtx_lock(&queue->vk_mutex);
#ifdef __OHOS__
   VkResult upload_submit_result = upload_prepare_result;
   if (upload_submit_result == VK_SUCCESS && !inline_upload)
      upload_submit_result = vkr_device_memory_submit_shadow_upload(queue);
   if (upload_submit_result != VK_SUCCESS && gpu_upload && !inline_upload) {
      vkr_device_memory_disable_shadow_upload_coverage(dispatch->data);
      vkr_device_memory_sync_shadows_to_host(dispatch->data);
   }
#endif
#ifdef __OHOS__
   if (inline_upload) {
      args->ret = vkr_ohos_queue_submit2_inline_upload(queue, vk, args);
      upload_submit_result = args->ret;
   } else {
#endif
      args->ret = vk->QueueSubmit2(
         args->queue, args->submitCount, args->pSubmits, args->fence);
#ifdef __OHOS__
   }
#endif
   mtx_unlock(&queue->vk_mutex);
#ifdef __OHOS__
   if (gpu_upload)
      mtx_unlock(&queue->shadow_upload_mutex);
#endif
}

static void
vkr_dispatch_vkCreateFence(struct vn_dispatch_context *dispatch,
                           struct vn_command_vkCreateFence *args)
{
   vkr_fence_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroyFence(struct vn_dispatch_context *dispatch,
                            struct vn_command_vkDestroyFence *args)
{
   vkr_fence_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkResetFences(UNUSED struct vn_dispatch_context *dispatch,
                           struct vn_command_vkResetFences *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkResetFences_args_handle(args);
   args->ret = vk->ResetFences(args->device, args->fenceCount, args->pFences);
}

static void
vkr_dispatch_vkGetFenceStatus(struct vn_dispatch_context *dispatch,
                              struct vn_command_vkGetFenceStatus *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetFenceStatus_args_handle(args);
#ifdef __OHOS__
   uint32_t retry_count = 0;
   bool used_wait_fallback = false;
   const bool perf_trace = vkr_ohos_perf_trace_enabled();
   const bool perf_summary = vkr_ohos_perf_summary_enabled();
   const uint64_t status_id = atomic_fetch_add_explicit(
      &vkr_ohos_fence_status_count, 1, memory_order_relaxed) + 1;
   const bool perf_sample = vkr_ohos_perf_sample_now(status_id);
   const bool perf_timing = perf_trace || perf_summary || perf_sample;
   const uint64_t start_ns = perf_timing ? vkr_ohos_queue_now_ns() : 0;
   args->ret = vkr_ohos_get_fence_status(vk, args->device, args->fence,
                                         &retry_count, &used_wait_fallback);
   const uint64_t end_ns = perf_timing ? vkr_ohos_queue_now_ns() : 0;
#else
   args->ret = vk->GetFenceStatus(args->device, args->fence);
#endif
#ifdef __OHOS__
   const uint64_t elapsed_us = start_ns && end_ns >= start_ns
      ? (end_ns - start_ns) / 1000 : 0;
   uint64_t total_us = 0;
   if (perf_timing) {
      total_us = atomic_fetch_add_explicit(
         &vkr_ohos_fence_status_total_us, elapsed_us,
         memory_order_relaxed) + elapsed_us;
      if (args->ret == VK_SUCCESS)
         atomic_fetch_add_explicit(&vkr_ohos_fence_status_success_count, 1,
                                   memory_order_relaxed);
      else if (args->ret == VK_NOT_READY)
         atomic_fetch_add_explicit(&vkr_ohos_fence_status_not_ready_count, 1,
                                   memory_order_relaxed);
   }
   if (retry_count)
      vkr_log("OHOS fence status transient OOM count=%" PRIu64
              " retries=%u wait0=%u final=%d", status_id, retry_count,
              used_wait_fallback, args->ret);
   if (perf_trace && (status_id <= 8 || !(status_id % 256))) {
      const uint64_t success_count = atomic_load_explicit(
         &vkr_ohos_fence_status_success_count, memory_order_relaxed);
      const uint64_t not_ready_count = atomic_load_explicit(
         &vkr_ohos_fence_status_not_ready_count, memory_order_relaxed);
      vkr_log("OHOS fence status count=%" PRIu64
              " success=%" PRIu64 " not_ready=%" PRIu64
              " result=%d elapsed_us=%" PRIu64
              " average_us=%" PRIu64,
              status_id, success_count, not_ready_count, args->ret,
              elapsed_us, status_id ? total_us / status_id : 0);
   } else if (perf_sample) {
      vkr_log("WineHuaPerfFenceSample: type=status id=%" PRIu64
              " result=%d elapsed_us=%" PRIu64,
              status_id, args->ret, elapsed_us);
   } else if (args->ret < 0)
      vkr_log("OHOS fence status count=%" PRIu64 " result=%d",
              status_id, args->ret);
#endif
   if (args->ret == VK_SUCCESS)
      vkr_device_memory_sync_shadows_from_host(dispatch->data);
}

static void
vkr_dispatch_vkWaitForFences(struct vn_dispatch_context *dispatch,
                             struct vn_command_vkWaitForFences *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkWaitForFences_args_handle(args);
#ifdef __OHOS__
   const bool perf_trace = vkr_ohos_perf_trace_enabled();
   const bool perf_summary = vkr_ohos_perf_summary_enabled();
   const uint64_t wait_id =
      atomic_fetch_add_explicit(&vkr_ohos_fence_wait_count, 1,
                                memory_order_relaxed) + 1;
   const bool perf_sample = vkr_ohos_perf_sample_now(wait_id);
   const bool perf_timing = perf_trace || perf_summary || perf_sample;
   const uint64_t start_ns = perf_timing ? vkr_ohos_queue_now_ns() : 0;
#endif
   args->ret = vk->WaitForFences(args->device, args->fenceCount, args->pFences,
                                 args->waitAll, args->timeout);
#ifdef __OHOS__
   const uint64_t end_ns = perf_timing ? vkr_ohos_queue_now_ns() : 0;
   const uint64_t elapsed_us = start_ns && end_ns >= start_ns
      ? (end_ns - start_ns) / 1000 : 0;
   if (perf_timing) {
      atomic_fetch_add_explicit(&vkr_ohos_fence_wait_total_us, elapsed_us,
                                memory_order_relaxed);
      vkr_ohos_atomic_max(&vkr_ohos_fence_wait_max_us, elapsed_us);
   }
   if (perf_trace && (wait_id <= 8 || !(wait_id % 120)))
      vkr_log("OHOS fence wait count=%" PRIu64
              " fences=%u wait_all=%u timeout_ns=%" PRIu64
              " result=%d elapsed_us=%" PRIu64,
              wait_id, args->fenceCount, args->waitAll, args->timeout,
              args->ret, elapsed_us);
   else if (perf_sample)
      vkr_log("WineHuaPerfFenceSample: type=wait id=%" PRIu64
              " fences=%u wait_all=%u timeout_ns=%" PRIu64
              " result=%d elapsed_us=%" PRIu64,
              wait_id, args->fenceCount, args->waitAll, args->timeout,
              args->ret, elapsed_us);
   else if (args->ret < 0)
      vkr_log("OHOS fence wait count=%" PRIu64
              " fences=%u wait_all=%u timeout_ns=%" PRIu64 " result=%d",
              wait_id, args->fenceCount, args->waitAll, args->timeout,
              args->ret);
#endif
   if (args->ret == VK_SUCCESS)
      vkr_device_memory_sync_shadows_from_host(dispatch->data);
}

static void
vkr_dispatch_vkResetFenceResourceMESA(struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkResetFenceResourceMESA *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;
   int fd = -1;

   vn_replace_vkResetFenceResourceMESA_args_handle(args);

   const VkFenceGetFdInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR,
      .fence = args->fence,
      .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
   };
   VkResult result = vk->GetFenceFdKHR(args->device, &info, &fd);
   if (result != VK_SUCCESS) {
      vkr_context_set_fatal(ctx);
      return;
   }

   if (fd >= 0)
      close(fd);
}

static void
vkr_dispatch_vkCreateSemaphore(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCreateSemaphore *args)
{
   vkr_semaphore_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroySemaphore(struct vn_dispatch_context *dispatch,
                                struct vn_command_vkDestroySemaphore *args)
{
   vkr_semaphore_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetSemaphoreCounterValue(UNUSED struct vn_dispatch_context *dispatch,
                                        struct vn_command_vkGetSemaphoreCounterValue *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetSemaphoreCounterValue_args_handle(args);
   args->ret = vk->GetSemaphoreCounterValue(args->device, args->semaphore, args->pValue);
}

static void
vkr_dispatch_vkWaitSemaphores(UNUSED struct vn_dispatch_context *dispatch,
                              struct vn_command_vkWaitSemaphores *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkWaitSemaphores_args_handle(args);
   args->ret = vk->WaitSemaphores(args->device, args->pWaitInfo, args->timeout);
}

static void
vkr_dispatch_vkSignalSemaphore(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkSignalSemaphore *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkSignalSemaphore_args_handle(args);
   args->ret = vk->SignalSemaphore(args->device, args->pSignalInfo);
}

static void
vkr_dispatch_vkWaitSemaphoreResourceMESA(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkWaitSemaphoreResourceMESA *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;
   int fd = -1;

   vn_replace_vkWaitSemaphoreResourceMESA_args_handle(args);

   const VkSemaphoreGetFdInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
      .semaphore = args->semaphore,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
   };
   VkResult result = vk->GetSemaphoreFdKHR(args->device, &info, &fd);
   if (result != VK_SUCCESS) {
      vkr_context_set_fatal(ctx);
      return;
   }

   if (fd >= 0)
      close(fd);
}

static void
vkr_dispatch_vkImportSemaphoreResourceMESA(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkImportSemaphoreResourceMESA *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkImportSemaphoreResourceMESA_args_handle(args);

   const VkImportSemaphoreResourceInfoMESA *res_info = args->pImportSemaphoreResourceInfo;

   /* resourceId 0 is for importing a signaled payload to sync_fd fence */
   assert(!res_info->resourceId);

   const VkImportSemaphoreFdInfoKHR import_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
      .semaphore = res_info->semaphore,
      .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
      .fd = -1,
   };
   if (vk->ImportSemaphoreFdKHR(args->device, &import_info) != VK_SUCCESS)
      vkr_context_set_fatal(ctx);
}

static void
vkr_dispatch_vkCreateEvent(struct vn_dispatch_context *dispatch,
                           struct vn_command_vkCreateEvent *args)
{
   vkr_event_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroyEvent(struct vn_dispatch_context *dispatch,
                            struct vn_command_vkDestroyEvent *args)
{
   vkr_event_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetEventStatus(UNUSED struct vn_dispatch_context *dispatch,
                              struct vn_command_vkGetEventStatus *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetEventStatus_args_handle(args);
   args->ret = vk->GetEventStatus(args->device, args->event);
}

static void
vkr_dispatch_vkSetEvent(UNUSED struct vn_dispatch_context *dispatch,
                        struct vn_command_vkSetEvent *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkSetEvent_args_handle(args);
   args->ret = vk->SetEvent(args->device, args->event);
}

static void
vkr_dispatch_vkResetEvent(UNUSED struct vn_dispatch_context *dispatch,
                          struct vn_command_vkResetEvent *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkResetEvent_args_handle(args);
   args->ret = vk->ResetEvent(args->device, args->event);
}

void
vkr_context_init_queue_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkGetDeviceQueue = vkr_dispatch_vkGetDeviceQueue;
   dispatch->dispatch_vkGetDeviceQueue2 = vkr_dispatch_vkGetDeviceQueue2;
   dispatch->dispatch_vkQueueSubmit = vkr_dispatch_vkQueueSubmit;
   dispatch->dispatch_vkQueueBindSparse = vkr_dispatch_vkQueueBindSparse;
   dispatch->dispatch_vkQueueWaitIdle = vkr_dispatch_vkQueueWaitIdle;

   /* VK_KHR_synchronization2 */
   dispatch->dispatch_vkQueueSubmit2 = vkr_dispatch_vkQueueSubmit2;
}

void
vkr_context_init_fence_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateFence = vkr_dispatch_vkCreateFence;
   dispatch->dispatch_vkDestroyFence = vkr_dispatch_vkDestroyFence;
   dispatch->dispatch_vkResetFences = vkr_dispatch_vkResetFences;
   dispatch->dispatch_vkGetFenceStatus = vkr_dispatch_vkGetFenceStatus;
   dispatch->dispatch_vkWaitForFences = vkr_dispatch_vkWaitForFences;

   dispatch->dispatch_vkResetFenceResourceMESA = vkr_dispatch_vkResetFenceResourceMESA;
}

void
vkr_context_init_semaphore_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateSemaphore = vkr_dispatch_vkCreateSemaphore;
   dispatch->dispatch_vkDestroySemaphore = vkr_dispatch_vkDestroySemaphore;
   dispatch->dispatch_vkGetSemaphoreCounterValue =
      vkr_dispatch_vkGetSemaphoreCounterValue;
   dispatch->dispatch_vkWaitSemaphores = vkr_dispatch_vkWaitSemaphores;
   dispatch->dispatch_vkSignalSemaphore = vkr_dispatch_vkSignalSemaphore;

   dispatch->dispatch_vkWaitSemaphoreResourceMESA =
      vkr_dispatch_vkWaitSemaphoreResourceMESA;
   dispatch->dispatch_vkImportSemaphoreResourceMESA =
      vkr_dispatch_vkImportSemaphoreResourceMESA;
}

void
vkr_context_init_event_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateEvent = vkr_dispatch_vkCreateEvent;
   dispatch->dispatch_vkDestroyEvent = vkr_dispatch_vkDestroyEvent;
   dispatch->dispatch_vkGetEventStatus = vkr_dispatch_vkGetEventStatus;
   dispatch->dispatch_vkSetEvent = vkr_dispatch_vkSetEvent;
   dispatch->dispatch_vkResetEvent = vkr_dispatch_vkResetEvent;
}
