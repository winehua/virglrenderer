/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_device_memory.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __OHOS__
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "util/anon_file.h"
#endif

#include "venus-protocol/vn_protocol_renderer_transport.h"

#include "vkr_device_memory_gen.h"
#include "vkr_buffer.h"
#include "vkr_metal_helpers.h"
#include "vkr_physical_device.h"
#include "vkr_queue.h"

static VkResult
vkr_device_memory_flush_shadow_range(struct vkr_device_memory *mem,
                                     VkDeviceSize offset,
                                     VkDeviceSize size);
static VkResult
vkr_device_memory_invalidate_shadow_range(struct vkr_device_memory *mem,
                                          VkDeviceSize offset,
                                          VkDeviceSize size);

#ifdef __OHOS__
static atomic_uint_fast64_t vkr_ohos_shadow_to_host_sync_count;
static atomic_uint_fast64_t vkr_ohos_shadow_from_host_sync_count;
static atomic_uint_fast64_t vkr_ohos_shadow_generation_submit_count;
static atomic_uint_fast64_t vkr_ohos_shadow_generation_flush_count;
static atomic_uint_fast64_t vkr_ohos_shadow_generation_contended_count;
static atomic_uint_fast64_t vkr_ohos_shadow_generation_wait_us;
static atomic_uint vkr_ohos_ubo_flush_trace_count;
static atomic_uint vkr_ohos_ubo_range_trace_count;
static atomic_uint vkr_ohos_ubo_update_trace_count;
static atomic_uint vkr_ohos_ubo_watched_update_trace_count;

#define VKR_WINEHUA_UBO_TRACE_LIMIT 200000u
#define VKR_WINEHUA_FNV64_OFFSET UINT64_C(1469598103934665603)
#define VKR_WINEHUA_FNV64_PRIME UINT64_C(1099511628211)

static uint64_t
vkr_ohos_fnv1a64(const void *data, size_t size);

static bool
vkr_ohos_gate_c_trace_enabled(void)
{
   const char *value = os_get_option("WINEHUA_VKD3D_GATE_C_TRACE");
   return value && value[0] == '1' && !value[1];
}

static uint64_t
vkr_ohos_gate_c_trace_now_us(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts))
      return 0;
   return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void
vkr_ohos_gate_c_trace_memory(const char *phase,
                             const struct vkr_device_memory *mem,
                             VkDeviceSize offset,
                             VkDeviceSize size,
                             bool complete,
                             VkResult result)
{
   if (!vkr_ohos_gate_c_trace_enabled() || !mem)
      return;

   const VkDeviceSize available = mem
      ? MIN2(mem->allocation_size, mem->shadow_size) : 0;
   const VkDeviceSize sample_offset = MIN2(offset, available);
   const VkDeviceSize requested_size = size == VK_WHOLE_SIZE
      ? available - sample_offset : MIN2(size, available - sample_offset);
   const VkDeviceSize sample_size = MIN2(requested_size, 4096u);
   const uint64_t host_hash = mem && mem->host_map && sample_size
      ? vkr_ohos_fnv1a64((const uint8_t *)mem->host_map + sample_offset,
                         (size_t)sample_size)
      : 0;
   const uint64_t shadow_hash = mem && mem->shadow_map && sample_size
      ? vkr_ohos_fnv1a64((const uint8_t *)mem->shadow_map + sample_offset,
                         (size_t)sample_size)
      : 0;

   const char *path = getenv("WINEHUA_VIRGL_LOG_PATH");
   if (!path || !path[0])
      return;

   FILE *file = fopen(path, "a");
   if (!file)
      return;

   fprintf(file,
           "[%" PRIu64 "] [vkd3d-gate-c] phase=%s memory_id=%" PRIu64
           " allocation_size=%" PRIu64 " offset=%" PRIu64
           " size=%" PRIu64 " host_map=%u shadow_map=%u"
           " sample_size=%" PRIu64 " host_hash=%016" PRIx64
           " shadow_hash=%016" PRIx64 " equal=%u complete=%u result=%d\n",
           vkr_ohos_gate_c_trace_now_us(), phase, (uint64_t)mem->base.id,
           (uint64_t)mem->allocation_size, (uint64_t)offset, (uint64_t)size,
           mem->host_map != NULL, mem->shadow_map != NULL,
           (uint64_t)sample_size, host_hash, shadow_hash,
           host_hash == shadow_hash, complete, result);
   fflush(file);
   fclose(file);
}

static bool
vkr_ohos_shadow_trace_enabled(void)
{
   const char *value = os_get_option("VKR_WINEHUA_SHADOW_TRACE");
   return value && value[0] == '1' && !value[1];
}

static bool
vkr_ohos_ubo_identity_trace_enabled(void)
{
   const char *value = os_get_option("WINEHUA_VKR_TRACE_UBO_IDENTITY");
   return value && value[0] && !(value[0] == '0' && !value[1]);
}

static bool
vkr_ohos_ubo_identity_trace_verbose(void)
{
   const char *value = os_get_option("WINEHUA_VKR_TRACE_UBO_IDENTITY");
   return value && value[0] == '1' && !value[1];
}

static bool
vkr_ohos_ubo_identity_trace_allow(atomic_uint *counter, const char *phase)
{
   const unsigned index =
      atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
   if (index < VKR_WINEHUA_UBO_TRACE_LIMIT)
      return true;
   if (index == VKR_WINEHUA_UBO_TRACE_LIMIT)
      vkr_log("WineHuaUboHost: phase=%s trace limit reached", phase);
   return false;
}

static uint64_t
vkr_ohos_fnv1a64(const void *data, size_t size)
{
   const uint8_t *bytes = data;
   uint64_t hash = VKR_WINEHUA_FNV64_OFFSET;
   for (size_t i = 0; i < size; i++) {
      hash ^= bytes[i];
      hash *= VKR_WINEHUA_FNV64_PRIME;
   }
   return hash;
}

static bool
vkr_ohos_shadow_submit_unmap_large_enabled(void)
{
   const char *value = os_get_option("VKR_WINEHUA_SHADOW_SUBMIT_UNMAP_LARGE");
   return value && value[0] == '1' && !value[1];
}

static bool
vkr_ohos_shadow_msync_enabled(void)
{
   const char *value = os_get_option("VKR_WINEHUA_SHADOW_MSYNC");
   return value && value[0] == '1' && !value[1];
}

static bool
vkr_ohos_shadow_upload_wait_enabled(void)
{
   const char *value = os_get_option("VKR_WINEHUA_GPU_UPLOAD_WAIT");
   return value && value[0] == '1' && !value[1];
}

static bool
vkr_ohos_shadow_dirty_list_enabled(void)
{
   const char *value = os_get_option("VKR_WINEHUA_SHADOW_DIRTY_LIST");
   return !(value && value[0] == '0' && !value[1]);
}

static bool
vkr_ohos_shadow_bound_buffer_list_enabled(void)
{
   const char *value = os_get_option("VKR_WINEHUA_BOUND_BUFFER_LIST");
   const bool enabled = value && value[0] == '1' && !value[1];
   if (enabled) {
      static atomic_flag logged = ATOMIC_FLAG_INIT;
      if (!atomic_flag_test_and_set_explicit(&logged, memory_order_relaxed))
         vkr_log("WineHua shadow bound-buffer dirty iteration enabled");
   }
   return enabled;
}

static bool
vkr_ohos_shadow_coverage_sort_enabled(void)
{
   const char *value = os_get_option("VKR_WINEHUA_COVERAGE_SORT");
   const bool enabled = value && value[0] == '1' && !value[1];
   if (enabled) {
      static atomic_flag logged = ATOMIC_FLAG_INIT;
      if (!atomic_flag_test_and_set_explicit(&logged, memory_order_relaxed))
         vkr_log("WineHua shadow coverage sorted-interval A/B enabled");
   }
   return enabled;
}

static bool
vkr_ohos_shadow_defer_host_copy_enabled(const struct vkr_device *dev)
{
   const char *value = os_get_option("VKR_WINEHUA_GPU_UPLOAD_INLINE");
   return vkr_device_memory_gpu_upload_enabled(dev) &&
      value && value[0] == '1' && !value[1];
}

/* A dirty mapped allocation is already copied into Host memory by the
 * remote-flush bridge.  Flushing one VkMappedMemoryRange per allocation adds
 * one Venus/Host call for every dynamic allocation touched by a submit.  Keep
 * the cache-domain transition, but batch ranges belonging to the same device.
 * The caller falls back to the old per-allocation path on overflow, mixed
 * devices, or a failed batch call. */
static bool
vkr_ohos_shadow_batch_flush_enabled(void)
{
   const char *value = os_get_option("VKR_WINEHUA_BATCH_FLUSH");
   return !(value && value[0] == '0' && !value[1]);
}

static bool
vkr_ohos_shadow_merge_ranges_enabled(void)
{
   const char *value = os_get_option("VKR_WINEHUA_SHADOW_MERGE_RANGES");
   return !(value && value[0] == '0' && !value[1]);
}

/* A dirty VkDeviceMemory range can be visible through several VkBuffer
 * aliases.  Updating every alias writes the same backing memory repeatedly.
 * Keep this opt-in until the exact-cover implementation has completed device
 * A/B validation; the normal path remains the conservative per-buffer walk. */
static bool
vkr_ohos_shadow_cover_upload_enabled(void)
{
   const char *value = os_get_option("VKR_WINEHUA_SHADOW_COVER_UPLOAD");
   const bool enabled = value && value[0] == '1' && !value[1];
   if (enabled) {
      static atomic_flag logged = ATOMIC_FLAG_INIT;
      if (!atomic_flag_test_and_set_explicit(&logged, memory_order_relaxed))
         vkr_log("WineHua shadow exact alias-cover upload A/B enabled");
   }
   return enabled;
}

static int
vkr_ohos_shadow_dirty_range_compare(const void *lhs_ptr, const void *rhs_ptr)
{
   const struct vkr_ohos_shadow_dirty_range *lhs = lhs_ptr;
   const struct vkr_ohos_shadow_dirty_range *rhs = rhs_ptr;
   if (lhs->offset < rhs->offset)
      return -1;
   if (lhs->offset > rhs->offset)
      return 1;
   if (lhs->size < rhs->size)
      return -1;
   if (lhs->size > rhs->size)
      return 1;
   return 0;
}

static int
vkr_ohos_shadow_coverage_range_compare(const void *lhs_ptr,
                                       const void *rhs_ptr)
{
   const struct vkr_ohos_shadow_coverage_range *lhs = lhs_ptr;
   const struct vkr_ohos_shadow_coverage_range *rhs = rhs_ptr;
   if (lhs->begin < rhs->begin)
      return -1;
   if (lhs->begin > rhs->begin)
      return 1;
   if (lhs->end < rhs->end)
      return -1;
   if (lhs->end > rhs->end)
      return 1;
   return 0;
}

static void
vkr_ohos_record_shadow_upload_range(struct vn_device_proc_table *vk,
                                    VkCommandBuffer command,
                                    const struct vkr_buffer *buffer,
                                    const struct vkr_device_memory *mem,
                                    uint64_t submit_id,
                                    VkDeviceSize relative_begin,
                                    VkDeviceSize relative_end,
                                    uint32_t *update_count,
                                    uint64_t *upload_bytes)
{
   VkDeviceSize remaining = relative_end - relative_begin;
   VkDeviceSize dst_offset = relative_begin;
   const uint8_t *source = mem->shadow_host_copy_deferred &&
      mem->shadow_upload_snapshot
      ? mem->shadow_upload_snapshot : mem->shadow_map;
   const uint8_t *data = source + buffer->bound_memory_offset +
      relative_begin;

   while (remaining) {
      VkDeviceSize chunk = MIN2(remaining, (VkDeviceSize)65536);
      chunk &= ~(VkDeviceSize)3;
      if (!chunk)
         break;
      vk->CmdUpdateBuffer(command, buffer->base.handle.buffer, dst_offset,
                          chunk, data);
      if (vkr_ohos_ubo_identity_trace_verbose() &&
          vkr_ohos_ubo_identity_trace_allow(
             &vkr_ohos_ubo_update_trace_count, "update")) {
         vkr_log("WineHuaUboHost: phase=update submit=%" PRIu64
                 " uploadCmd=0x%" PRIxPTR " bufferId=%" PRIu64
                 " hostBuffer=0x%" PRIxPTR " memoryId=%" PRIu64
                 " hostMemory=0x%" PRIxPTR " bufferMemoryOffset=%" PRIu64
                 " dstOffset=%" PRIu64 " absoluteOffset=%" PRIu64
                 " bytes=%" PRIu64 " sourceHash=%016" PRIx64,
                 submit_id, (uintptr_t)command, (uint64_t)buffer->base.id,
                 (uintptr_t)buffer->base.handle.buffer,
                 (uint64_t)mem->base.id,
                 (uintptr_t)mem->base.handle.device_memory,
                 (uint64_t)buffer->bound_memory_offset,
                 (uint64_t)dst_offset,
                 (uint64_t)(buffer->bound_memory_offset + dst_offset),
                 (uint64_t)chunk, vkr_ohos_fnv1a64(data, (size_t)chunk));
      }
      if (vkr_ohos_ubo_identity_trace_enabled()) {
         const uint32_t watch_count = atomic_load_explicit(
            &buffer->winehua_ubo_watch_count, memory_order_acquire);
         for (uint32_t i = 0; i < watch_count; i++) {
            struct vkr_winehua_ubo_watch *watch =
               &buffer->winehua_ubo_watches[i];
            const VkDeviceSize watch_offset =
               watch->offset;
            const VkDeviceSize watch_size =
               watch->size;
            if (watch_offset < dst_offset || watch_size > chunk ||
                watch_offset - dst_offset > chunk - watch_size)
               continue;
            const uint8_t *watch_data = data + watch_offset - dst_offset;
            const uint64_t source_hash =
               vkr_ohos_fnv1a64(watch_data, (size_t)watch_size);
            const bool hash_valid = atomic_load_explicit(
               &watch->last_update_hash_valid, memory_order_acquire);
            const uint64_t previous_hash = atomic_load_explicit(
               &watch->last_update_hash, memory_order_relaxed);
            if (hash_valid && previous_hash == source_hash)
               continue;
            atomic_store_explicit(&watch->last_update_hash, source_hash,
                                  memory_order_relaxed);
            atomic_store_explicit(&watch->last_update_hash_valid, true,
                                  memory_order_release);
            if (!vkr_ohos_ubo_identity_trace_allow(
                   &vkr_ohos_ubo_watched_update_trace_count,
                   "watched-update"))
               continue;
            vkr_log("WineHuaUboHost: phase=watched-update submit=%" PRIu64
                    " binding=%u bufferId=%" PRIu64
                    " hostBuffer=0x%" PRIxPTR " memoryId=%" PRIu64
                    " hostMemory=0x%" PRIxPTR
                    " bufferMemoryOffset=%" PRIu64
                    " descriptorOffset=%" PRIu64
                    " absoluteOffset=%" PRIu64 " bytes=%" PRIu64
                    " updateOffset=%" PRIu64 " updateBytes=%" PRIu64
                    " sourceHash=%016" PRIx64,
                    submit_id, watch->binding,
                    (uint64_t)buffer->base.id,
                    (uintptr_t)buffer->base.handle.buffer,
                    (uint64_t)mem->base.id,
                    (uintptr_t)mem->base.handle.device_memory,
                    (uint64_t)buffer->bound_memory_offset,
                    (uint64_t)watch_offset,
                    (uint64_t)(buffer->bound_memory_offset + watch_offset),
                    (uint64_t)watch_size, (uint64_t)dst_offset,
                    (uint64_t)chunk, source_hash);
         }
      }
      (*update_count)++;
      *upload_bytes += chunk;
      remaining -= chunk;
      dst_offset += chunk;
      data += chunk;
   }
}

struct vkr_ohos_shadow_upload_record {
   struct vn_device_proc_table *vk;
   VkCommandBuffer command;
   struct vkr_device *device;
   uint64_t submit_id;
   bool merge_ranges;
   uint32_t update_count;
   uint32_t upload_range_count;
   uint32_t buffer_count;
   uint32_t uniform_buffer_count;
   uint32_t storage_buffer_count;
   uint64_t upload_bytes;
};

struct vkr_ohos_shadow_dirty_summary {
   uint64_t bytes;
   uint32_t allocation_count;
   uint32_t range_count;
   uint32_t range_overflow_count;
};

static void
vkr_ohos_shadow_upload_record_buffer(
   struct vkr_ohos_shadow_upload_record *record,
   struct vkr_buffer *buffer)
{
   if (buffer->winehua_shadow_record_submit_id == record->submit_id)
      return;

   buffer->winehua_shadow_record_submit_id = record->submit_id;
   record->buffer_count++;
   if (buffer->host_usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
      record->uniform_buffer_count++;
   if (buffer->host_usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
      record->storage_buffer_count++;
}

static bool
vkr_ohos_shadow_dirty_ranges_buffer_covered_legacy(
   const struct vkr_device_memory *mem)
{
   const VkDeviceSize allocation_size = MIN2(
      mem->allocation_size, mem->shadow_size);
   for (uint32_t range_index = 0;
        range_index < mem->shadow_dirty_range_count;
        range_index++) {
      const struct vkr_ohos_shadow_dirty_range *range =
         &mem->shadow_dirty_ranges[range_index];
      VkDeviceSize cursor = MIN2(range->offset, allocation_size);
      const VkDeviceSize range_size = MIN2(
         range->size, allocation_size - cursor);
      const VkDeviceSize range_end = cursor + range_size;

      while (cursor < range_end) {
         VkDeviceSize covered_end = cursor;
         list_for_each_entry (struct vkr_buffer, buffer,
                              &mem->bound_buffers, memory_head) {
            if (buffer->bound_memory != mem ||
                !(buffer->host_usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT))
               continue;

            const VkDeviceSize buffer_begin = MIN2(
               buffer->bound_memory_offset, allocation_size);
            const VkDeviceSize buffer_size = MIN2(
               buffer->size, allocation_size - buffer_begin);
            const VkDeviceSize buffer_end = buffer_begin + buffer_size;
            if (buffer_begin <= cursor && buffer_end > covered_end)
               covered_end = MIN2(buffer_end, range_end);
         }
         if (covered_end == cursor)
            return false;
         cursor = covered_end;
      }
   }
   return mem->shadow_dirty_range_count > 0;
}

static bool
vkr_ohos_shadow_dirty_ranges_buffer_covered_sorted(
   struct vkr_device_memory *mem)
{
   const VkDeviceSize allocation_size = MIN2(
      mem->allocation_size, mem->shadow_size);
   bool has_nonempty_dirty_range = false;
   for (uint32_t i = 0; i < mem->shadow_dirty_range_count; i++) {
      const struct vkr_ohos_shadow_dirty_range *dirty =
         &mem->shadow_dirty_ranges[i];
      const VkDeviceSize begin = MIN2(dirty->offset, allocation_size);
      if (MIN2(dirty->size, allocation_size - begin)) {
         has_nonempty_dirty_range = true;
         break;
      }
   }
   if (!has_nonempty_dirty_range)
      return mem->shadow_dirty_range_count > 0;

   if (!mem->shadow_coverage_valid) {
      uint32_t buffer_count = 0;
      list_for_each_entry (struct vkr_buffer, buffer,
                           &mem->bound_buffers, memory_head) {
         if (buffer->bound_memory == mem &&
             (buffer->host_usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT))
            buffer_count++;
      }

      if (!buffer_count)
         return false;

      if (mem->shadow_coverage_range_capacity < buffer_count) {
         struct vkr_ohos_shadow_coverage_range *ranges = realloc(
            mem->shadow_coverage_ranges, sizeof(*ranges) * buffer_count);
         if (!ranges) {
            static atomic_flag logged = ATOMIC_FLAG_INIT;
            if (!atomic_flag_test_and_set_explicit(&logged,
                                                   memory_order_relaxed))
               vkr_log("WineHua shadow coverage scratch allocation failed; "
                       "using legacy coverage scan");
            return vkr_ohos_shadow_dirty_ranges_buffer_covered_legacy(mem);
         }
         mem->shadow_coverage_ranges = ranges;
         mem->shadow_coverage_range_capacity = buffer_count;
      }

      uint32_t range_count = 0;
      list_for_each_entry (struct vkr_buffer, buffer,
                           &mem->bound_buffers, memory_head) {
         if (buffer->bound_memory != mem ||
             !(buffer->host_usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT))
            continue;

         const VkDeviceSize begin = MIN2(
            buffer->bound_memory_offset, allocation_size);
         const VkDeviceSize size = MIN2(
            buffer->size, allocation_size - begin);
         if (!size)
            continue;
         mem->shadow_coverage_ranges[range_count++] =
            (struct vkr_ohos_shadow_coverage_range) {
               .begin = begin,
               .end = begin + size,
            };
      }
      if (!range_count)
         return false;

      if (range_count > 1)
         qsort(mem->shadow_coverage_ranges, range_count,
               sizeof(*mem->shadow_coverage_ranges),
               vkr_ohos_shadow_coverage_range_compare);

      uint32_t merged_count = 0;
      for (uint32_t i = 0; i < range_count; i++) {
         const struct vkr_ohos_shadow_coverage_range current =
            mem->shadow_coverage_ranges[i];
         if (merged_count && current.begin <=
                mem->shadow_coverage_ranges[merged_count - 1].end) {
            mem->shadow_coverage_ranges[merged_count - 1].end = MAX2(
               mem->shadow_coverage_ranges[merged_count - 1].end,
               current.end);
         } else {
            mem->shadow_coverage_ranges[merged_count++] = current;
         }
      }
      mem->shadow_coverage_range_count = merged_count;
      mem->shadow_coverage_valid = true;
   }

   const uint32_t range_count = mem->shadow_coverage_range_count;
   if (!range_count)
      return false;

   uint32_t coverage_index = 0;
   for (uint32_t i = 0; i < mem->shadow_dirty_range_count; i++) {
      const struct vkr_ohos_shadow_dirty_range *dirty =
         &mem->shadow_dirty_ranges[i];
      const VkDeviceSize dirty_begin = MIN2(dirty->offset, allocation_size);
      const VkDeviceSize dirty_size = MIN2(
         dirty->size, allocation_size - dirty_begin);
      const VkDeviceSize dirty_end = dirty_begin + dirty_size;
      if (!dirty_size)
         continue;

      while (coverage_index < range_count &&
             mem->shadow_coverage_ranges[coverage_index].end <= dirty_begin)
         coverage_index++;
      if (coverage_index == range_count ||
          mem->shadow_coverage_ranges[coverage_index].begin > dirty_begin ||
          mem->shadow_coverage_ranges[coverage_index].end < dirty_end)
         return false;
   }
   return mem->shadow_dirty_range_count > 0;
}

static bool
vkr_ohos_shadow_dirty_ranges_buffer_covered(
   struct vkr_device_memory *mem)
{
   if (vkr_ohos_shadow_coverage_sort_enabled())
      return vkr_ohos_shadow_dirty_ranges_buffer_covered_sorted(mem);
   return vkr_ohos_shadow_dirty_ranges_buffer_covered_legacy(mem);
}

void
vkr_device_memory_invalidate_shadow_coverage(struct vkr_device_memory *mem)
{
   if (!mem)
      return;

   mem->shadow_coverage_range_count = 0;
   mem->shadow_coverage_valid = false;
}

static void
vkr_ohos_prepare_shadow_dirty_memory(
   struct vkr_device_memory *mem,
   struct vkr_device *device,
   struct vkr_ohos_shadow_dirty_summary *summary)
{
   if (mem->device != device || !mem->shadow_host_dirty)
      return;

   mem->shadow_gpu_upload_covered = false;
   /* Every precise dirty range must be represented by transfer-dst buffers
    * before the mapped Host copy can be skipped. */
   mem->shadow_gpu_upload_full_coverage = false;
   summary->allocation_count++;
   if (mem->shadow_dirty_range_overflow || !mem->shadow_dirty_range_count) {
      summary->range_overflow_count++;
      return;
   }

   if (mem->shadow_dirty_range_count > 1)
      qsort(mem->shadow_dirty_ranges, mem->shadow_dirty_range_count,
            sizeof(*mem->shadow_dirty_ranges),
            vkr_ohos_shadow_dirty_range_compare);
   summary->range_count += mem->shadow_dirty_range_count;
   for (uint32_t i = 0; i < mem->shadow_dirty_range_count; i++)
      summary->bytes += mem->shadow_dirty_ranges[i].size;
   mem->shadow_gpu_upload_full_coverage =
      vkr_ohos_shadow_dirty_ranges_buffer_covered(mem);
}

static void
vkr_ohos_record_shadow_upload_buffer(
   struct vkr_ohos_shadow_upload_record *record,
   struct vkr_buffer *buffer)
{
   struct vkr_device_memory *mem = buffer->bound_memory;
   if (!mem || mem->device != record->device || !mem->shadow_host_dirty ||
       !mem->shadow_map || !mem->shadow_size ||
       mem->shadow_dirty_range_overflow ||
       !mem->shadow_dirty_range_count ||
       !(buffer->host_usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT))
      return;

   const VkDeviceSize allocation_size = MIN2(
      mem->allocation_size, mem->shadow_size);
   const VkDeviceSize buffer_begin = MIN2(
      buffer->bound_memory_offset, allocation_size);
   const VkDeviceSize buffer_size = MIN2(
      buffer->size, allocation_size - buffer_begin);
   const VkDeviceSize buffer_end = buffer_begin + buffer_size;

   bool pending = false;
   VkDeviceSize pending_begin = 0;
   VkDeviceSize pending_end = 0;
   for (uint32_t range_index = 0;
        range_index < mem->shadow_dirty_range_count;
        range_index++) {
      const struct vkr_ohos_shadow_dirty_range *range =
         &mem->shadow_dirty_ranges[range_index];
      const VkDeviceSize dirty_begin = MIN2(range->offset, allocation_size);
      const VkDeviceSize dirty_size = MIN2(
         range->size, allocation_size - dirty_begin);
      const VkDeviceSize dirty_end = dirty_begin + dirty_size;
      const VkDeviceSize intersection_begin =
         MAX2(dirty_begin, buffer_begin);
      const VkDeviceSize intersection_end = MIN2(dirty_end, buffer_end);
      if (intersection_end <= intersection_begin)
         continue;

      VkDeviceSize relative_begin = intersection_begin - buffer_begin;
      VkDeviceSize relative_end = intersection_end - buffer_begin;
      relative_begin &= ~(VkDeviceSize)3;
      relative_end = MIN2(ALIGN(relative_end, 4), buffer_size);
      relative_end &= ~(VkDeviceSize)3;
      if (relative_end <= relative_begin)
         continue;

      vkr_ohos_shadow_upload_record_buffer(record, buffer);

      if (vkr_ohos_ubo_identity_trace_verbose() &&
          vkr_ohos_ubo_identity_trace_allow(
             &vkr_ohos_ubo_range_trace_count, "upload-range")) {
         const uint8_t *source = mem->shadow_host_copy_deferred &&
            mem->shadow_upload_snapshot ? mem->shadow_upload_snapshot :
            mem->shadow_map;
         const VkDeviceSize absolute_offset =
            buffer->bound_memory_offset + relative_begin;
         const VkDeviceSize byte_count = relative_end - relative_begin;
         vkr_log("WineHuaUboHost: phase=upload-range submit=%" PRIu64
                 " bufferId=%" PRIu64 " hostBuffer=0x%" PRIxPTR
                 " memoryId=%" PRIu64 " hostMemory=0x%" PRIxPTR
                 " bufferMemoryOffset=%" PRIu64 " dstOffset=%" PRIu64
                 " absoluteOffset=%" PRIu64 " bytes=%" PRIu64
                 " sourceHash=%016" PRIx64,
                 record->submit_id, (uint64_t)buffer->base.id,
                 (uintptr_t)buffer->base.handle.buffer,
                 (uint64_t)mem->base.id,
                 (uintptr_t)mem->base.handle.device_memory,
                 (uint64_t)buffer->bound_memory_offset,
                 (uint64_t)relative_begin, (uint64_t)absolute_offset,
                 (uint64_t)byte_count,
                 vkr_ohos_fnv1a64((const uint8_t *)source + absolute_offset,
                                  (size_t)byte_count));
      }

      mem->shadow_gpu_upload_covered = true;
      record->upload_range_count++;
      if (!record->merge_ranges) {
         vkr_ohos_record_shadow_upload_range(
            record->vk, record->command, buffer, mem, record->submit_id,
            relative_begin,
            relative_end, &record->update_count, &record->upload_bytes);
         continue;
      }

      if (!pending) {
         pending_begin = relative_begin;
         pending_end = relative_end;
         pending = true;
      } else if (relative_begin <= pending_end) {
         pending_end = MAX2(pending_end, relative_end);
      } else {
         vkr_ohos_record_shadow_upload_range(
            record->vk, record->command, buffer, mem, record->submit_id,
            pending_begin,
            pending_end, &record->update_count, &record->upload_bytes);
         pending_begin = relative_begin;
         pending_end = relative_end;
      }
   }
   if (pending)
      vkr_ohos_record_shadow_upload_range(
         record->vk, record->command, buffer, mem, record->submit_id,
         pending_begin,
         pending_end, &record->update_count, &record->upload_bytes);
}

/* Record every dirty byte exactly once through a buffer that covers its
 * backing-memory address.  VkBuffer aliases share the same VkDeviceMemory,
 * so this is equivalent to the conservative per-buffer path without its
 * repeated writes.  Callers use it only after the existing coverage proof has
 * established complete TRANSFER_DST coverage. */
static bool
vkr_ohos_record_shadow_upload_memory_cover(
   struct vkr_ohos_shadow_upload_record *record,
   struct vkr_device_memory *mem)
{
   const VkDeviceSize allocation_size = MIN2(
      mem->allocation_size, mem->shadow_size);
   VkDeviceSize prior_dirty_end = 0;
   bool have_prior_dirty_end = false;

   for (uint32_t range_index = 0;
        range_index < mem->shadow_dirty_range_count;
        range_index++) {
      const struct vkr_ohos_shadow_dirty_range *range =
         &mem->shadow_dirty_ranges[range_index];
      VkDeviceSize dirty_begin = MIN2(range->offset, allocation_size);
      const VkDeviceSize dirty_size = MIN2(
         range->size, allocation_size - dirty_begin);
      const VkDeviceSize dirty_end = dirty_begin + dirty_size;
      if (!dirty_size)
         continue;

      /* Dirty ranges are sorted by prepare_shadow_dirty_memory.  Coalesce
       * overlaps here so a repeated flush does not recreate a duplicate
       * command through a different buffer alias. */
      if (have_prior_dirty_end) {
         if (dirty_end <= prior_dirty_end)
            continue;
         dirty_begin = MAX2(dirty_begin, prior_dirty_end);
      }

      VkDeviceSize cursor = dirty_begin;
      while (cursor < dirty_end) {
         struct vkr_buffer *best_buffer = NULL;
         VkDeviceSize best_buffer_begin = 0;
         VkDeviceSize best_buffer_end = cursor;

         list_for_each_entry (struct vkr_buffer, buffer,
                              &mem->bound_buffers, memory_head) {
            if (buffer->bound_memory != mem ||
                !(buffer->host_usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT))
               continue;

            const VkDeviceSize buffer_begin = MIN2(
               buffer->bound_memory_offset, allocation_size);
            const VkDeviceSize buffer_size = MIN2(
               buffer->size, allocation_size - buffer_begin);
            const VkDeviceSize buffer_end = buffer_begin + buffer_size;
            if (buffer_begin <= cursor && buffer_end > best_buffer_end) {
               best_buffer = buffer;
               best_buffer_begin = buffer_begin;
               best_buffer_end = buffer_end;
            }
         }

         if (!best_buffer) {
            vkr_log("WineHua shadow alias-cover unexpectedly uncovered "
                    "memoryId=%" PRIu64 " offset=%" PRIu64,
                    (uint64_t)mem->base.id, (uint64_t)cursor);
            mem->shadow_gpu_upload_covered = false;
            return false;
         }

         const VkDeviceSize segment_end = MIN2(best_buffer_end, dirty_end);
         VkDeviceSize relative_begin = cursor - best_buffer_begin;
         VkDeviceSize relative_end = segment_end - best_buffer_begin;
         const VkDeviceSize buffer_size = best_buffer_end - best_buffer_begin;
         relative_begin &= ~(VkDeviceSize)3;
         relative_end = MIN2(ALIGN(relative_end, 4), buffer_size);
         relative_end &= ~(VkDeviceSize)3;
         if (relative_end > relative_begin) {
            vkr_ohos_shadow_upload_record_buffer(record, best_buffer);
            mem->shadow_gpu_upload_covered = true;
            record->upload_range_count++;
            vkr_ohos_record_shadow_upload_range(
               record->vk, record->command, best_buffer, mem,
               record->submit_id, relative_begin, relative_end,
               &record->update_count, &record->upload_bytes);
         }
         cursor = segment_end;
      }

      prior_dirty_end = dirty_end;
      have_prior_dirty_end = true;
   }

   return true;
}

#define VKR_OHOS_SHADOW_BATCH_MAX_RANGES 256u

struct vkr_ohos_shadow_flush_batch {
   struct vkr_device *device;
   VkMappedMemoryRange ranges[VKR_OHOS_SHADOW_BATCH_MAX_RANGES];
   struct vkr_device_memory *memories[VKR_OHOS_SHADOW_BATCH_MAX_RANGES];
   uint32_t count;
   bool overflow;
};

static bool
vkr_ohos_shadow_flush_batch_add(
   struct vkr_ohos_shadow_flush_batch *batch,
   struct vkr_device_memory *mem)
{
   if (!mem->host_map || !mem->shadow_map || !mem->shadow_size ||
       !mem->shadow_remote_active || !mem->shadow_host_dirty ||
       !mem->shadow_dirty_size ||
       (vkr_device_memory_gpu_upload_enabled(mem->device) &&
        mem->shadow_gpu_upload_full_coverage))
      return true;

   if ((batch->device && batch->device != mem->device) ||
       batch->count >= VKR_OHOS_SHADOW_BATCH_MAX_RANGES) {
      batch->overflow = true;
      return false;
   }

   if (!batch->device)
      batch->device = mem->device;

   batch->memories[batch->count] = mem;
   batch->ranges[batch->count++] = (VkMappedMemoryRange) {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = mem->base.handle.device_memory,
      .offset = mem->shadow_dirty_offset,
      .size = mem->shadow_dirty_size,
   };
   return true;
}

static VkResult
vkr_ohos_shadow_flush_batch_submit(
   const struct vkr_ohos_shadow_flush_batch *batch)
{
   if (!batch->count || batch->overflow || !batch->device)
      return batch->overflow ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;

   struct vn_device_proc_table *vk = &batch->device->proc_table;
   for (uint32_t i = 0; i < batch->count; i++)
      vkr_ohos_gate_c_trace_memory("flush-batch-entry", batch->memories[i],
                                   batch->ranges[i].offset, batch->ranges[i].size,
                                   false, VK_SUCCESS);
   const VkResult result = vk->FlushMappedMemoryRanges(
      batch->device->base.handle.device, batch->count, batch->ranges);
   for (uint32_t i = 0; i < batch->count; i++)
      vkr_ohos_gate_c_trace_memory("flush-batch-result", batch->memories[i],
                                   batch->ranges[i].offset, batch->ranges[i].size,
                                   true, result);
   return result;
}

static void
vkr_ohos_shadow_dirty_list_remove(struct vkr_device_memory *mem)
{
   if (!mem->shadow_dirty_listed)
      return;

   list_del(&mem->shadow_dirty_head);
   list_inithead(&mem->shadow_dirty_head);
   mem->shadow_dirty_listed = false;
}

static void
vkr_ohos_shadow_dirty_list_add(struct vkr_device_memory *mem)
{
   if (!mem->context || mem->shadow_dirty_listed ||
       !vkr_ohos_shadow_dirty_list_enabled())
      return;

   list_addtail(&mem->shadow_dirty_head, &mem->context->shadow_dirty_memories);
   mem->shadow_dirty_listed = true;
}

static uint64_t
vkr_ohos_now_ns(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts))
      return 0;
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static bool
vkr_ohos_shadow_generation_serialize_enabled(void)
{
   const char *value = os_get_option(
      "VKR_WINEHUA_SHADOW_GENERATION_SERIALIZE");
   return value && value[0] == '1' && !value[1];
}

void
vkr_device_memory_shadow_generation_begin(struct vkr_context *ctx,
                                          bool submit)
{
   if (!ctx || !vkr_ohos_shadow_generation_serialize_enabled())
      return;

   const uint64_t start_ns = vkr_ohos_now_ns();
   const int try_result = mtx_trylock(&ctx->shadow_generation_mutex);
   const bool contended = try_result != thrd_success;
   if (contended)
      mtx_lock(&ctx->shadow_generation_mutex);
   const uint64_t end_ns = vkr_ohos_now_ns();
   const uint64_t waited_us = start_ns && end_ns >= start_ns
      ? (end_ns - start_ns) / 1000 : 0;

   atomic_uint_fast64_t *role_counter = submit
      ? &vkr_ohos_shadow_generation_submit_count
      : &vkr_ohos_shadow_generation_flush_count;
   const uint64_t role_count = atomic_fetch_add_explicit(
      role_counter, 1, memory_order_relaxed) + 1;
   const uint64_t total_wait_us = atomic_fetch_add_explicit(
      &vkr_ohos_shadow_generation_wait_us, waited_us,
      memory_order_relaxed) + waited_us;
   const uint64_t contended_count = contended
      ? atomic_fetch_add_explicit(
           &vkr_ohos_shadow_generation_contended_count, 1,
           memory_order_relaxed) + 1
      : atomic_load_explicit(&vkr_ohos_shadow_generation_contended_count,
                             memory_order_relaxed);

   if (role_count <= 8 || !(role_count % 120) ||
       (contended && (contended_count <= 8 || !(contended_count % 60))))
      vkr_log("OHOS shadow generation lock role=%s count=%" PRIu64
              " contended=%u contended_total=%" PRIu64
              " waited_us=%" PRIu64 " wait_total_us=%" PRIu64,
              submit ? "submit" : "flush", role_count, contended,
              contended_count, waited_us, total_wait_us);
}

void
vkr_device_memory_shadow_generation_end(struct vkr_context *ctx)
{
   if (ctx && vkr_ohos_shadow_generation_serialize_enabled())
      mtx_unlock(&ctx->shadow_generation_mutex);
}

static uint32_t
vkr_ohos_fnv1a32(const void *data, size_t size)
{
   const uint8_t *bytes = data;
   uint32_t hash = 2166136261u;
   for (size_t i = 0; i < size; i++) {
      hash ^= bytes[i];
      hash *= 16777619u;
   }
   return hash;
}

#define VKR_OHOS_MAX_SHADOW_DIRTY_RANGES 4096u

static bool
vkr_ohos_record_shadow_dirty_range(struct vkr_device_memory *mem,
                                   VkDeviceSize offset,
                                   VkDeviceSize size)
{
   if (!size)
      return true;
   if (mem->shadow_dirty_range_overflow)
      return false;

   VkDeviceSize begin = offset;
   VkDeviceSize end = offset + size;
   for (uint32_t i = 0; i < mem->shadow_dirty_range_count;) {
      const struct vkr_ohos_shadow_dirty_range *range =
         &mem->shadow_dirty_ranges[i];
      const VkDeviceSize range_begin = range->offset;
      const VkDeviceSize range_end = range->offset + range->size;
      if (end < range_begin || begin > range_end) {
         i++;
         continue;
      }

      begin = MIN2(begin, range_begin);
      end = MAX2(end, range_end);
      mem->shadow_dirty_ranges[i] =
         mem->shadow_dirty_ranges[--mem->shadow_dirty_range_count];
      i = 0;
   }

   if (mem->shadow_dirty_range_count == mem->shadow_dirty_range_capacity) {
      if (mem->shadow_dirty_range_capacity >=
          VKR_OHOS_MAX_SHADOW_DIRTY_RANGES) {
         mem->shadow_dirty_range_overflow = true;
         vkr_log("OHOS shadow precise dirty range overflow guestMemory=%" PRIu64
                 " count=%u",
                 (uint64_t)mem->base.id, mem->shadow_dirty_range_count);
         return false;
      }
      uint32_t new_capacity = mem->shadow_dirty_range_capacity
         ? mem->shadow_dirty_range_capacity * 2 : 16;
      new_capacity = MIN2(new_capacity, VKR_OHOS_MAX_SHADOW_DIRTY_RANGES);
      struct vkr_ohos_shadow_dirty_range *new_ranges = realloc(
         mem->shadow_dirty_ranges, sizeof(*new_ranges) * new_capacity);
      if (!new_ranges) {
         mem->shadow_dirty_range_overflow = true;
         vkr_log("OHOS shadow precise dirty range allocation failed guestMemory=%"
                 PRIu64 " capacity=%u",
                 (uint64_t)mem->base.id, new_capacity);
         return false;
      }
      mem->shadow_dirty_ranges = new_ranges;
      mem->shadow_dirty_range_capacity = new_capacity;
   }

   mem->shadow_dirty_ranges[mem->shadow_dirty_range_count++] =
      (struct vkr_ohos_shadow_dirty_range) {
         .offset = begin,
         .size = end - begin,
      };
   return true;
}
#endif

bool
vkr_device_memory_gpu_upload_enabled(const struct vkr_device *dev)
{
#ifdef __OHOS__
   const char *value = os_get_option("VKR_WINEHUA_GPU_UPLOAD");
   if (value && value[0] == '1' && !value[1])
      return true;
   if (value && value[0] == '0' && !value[1])
      return false;
   return dev && dev->physical_device &&
          dev->physical_device->winehua_shadow_gpu_upload_quirk;
#else
   (void)dev;
   return false;
#endif
}

static bool
vkr_get_fd_info_from_resource_info(struct vkr_context *ctx,
                                   const VkImportMemoryResourceInfoMESA *res_info,
                                   VkImportMemoryFdInfoKHR *out)
{
   struct vkr_resource *res = vkr_context_get_resource(ctx, res_info->resourceId);
   if (!res) {
      vkr_log("failed to import resource: invalid res_id %u", res_info->resourceId);
      vkr_context_set_fatal(ctx);
      return false;
   }

   VkExternalMemoryHandleTypeFlagBits handle_type;
   switch (res->fd_type) {
   case VIRGL_RESOURCE_FD_DMABUF:
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      break;
   case VIRGL_RESOURCE_FD_OPAQUE:
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      break;
   default:
      return false;
   }

   int fd = os_dupfd_cloexec(res->u.fd);
   if (fd < 0)
      return false;

   *out = (VkImportMemoryFdInfoKHR){
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = res_info->pNext,
      .fd = fd,
      .handleType = handle_type,
   };
   return true;
}

#if defined(HAVE_LINUX_UDMABUF_H) && defined(HAVE_MEMFD_CREATE)
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

static VkResult
vkr_udmabuf_get_fd_info_from_allocation_info(struct vkr_physical_device *physical_dev,
                                             const VkMemoryAllocateInfo *alloc_info,
                                             int *out_udmabuf_fd,
                                             VkImportMemoryFdInfoKHR *out_fd_info)
{
   int memfd = -1;
   int udmabuf_fd = -1;
   int fd = -1;

   memfd = memfd_create("vkr-udmabuf", MFD_CLOEXEC | MFD_ALLOW_SEALING);
   if (memfd < 0) {
      vkr_log("memfd_create failed (%s)", strerror(errno));
      goto fail;
   }

   const size_t size = align(alloc_info->allocationSize, getpagesize());
   int ret = ftruncate(memfd, size);
   if (ret) {
      vkr_log("ftruncate failed (%s)", strerror(errno));
      goto fail;
   }

   ret = fcntl(memfd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW);
   if (ret) {
      vkr_log("fcntl F_ADD_SEALS failed (%s)", strerror(errno));
      goto fail;
   }

   const struct udmabuf_create create = {
      .memfd = memfd,
      .flags = UDMABUF_FLAGS_CLOEXEC,
      .size = size,
   };
   udmabuf_fd = ioctl(physical_dev->udmabuf_dev_fd, UDMABUF_CREATE, &create);
   if (udmabuf_fd < 0) {
      vkr_log("ioctl UDMABUF_CREATE failed (%s)", strerror(errno));
      goto fail;
   }

   fd = os_dupfd_cloexec(udmabuf_fd);
   if (fd < 0) {
      vkr_log("os_dupfd_cloexec failed (%s)", strerror(errno));
      goto fail;
   }

   close(memfd);

   *out_udmabuf_fd = udmabuf_fd;
   *out_fd_info = (VkImportMemoryFdInfoKHR){
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = alloc_info->pNext,
      .fd = fd,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };

   return VK_SUCCESS;

fail:
   if (udmabuf_fd >= 0)
      close(udmabuf_fd);
   if (memfd >= 0)
      close(memfd);
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

#else  /* HAVE_LINUX_UDMABUF_H && HAVE_MEMFD_CREATE */

static inline VkResult
vkr_udmabuf_get_fd_info_from_allocation_info(
   UNUSED struct vkr_physical_device *physical_dev,
   UNUSED const VkMemoryAllocateInfo *alloc_info,
   UNUSED int *out_udmabuf_fd,
   UNUSED VkImportMemoryFdInfoKHR *out_fd_info)
{
   vkr_log("udmabuf_allocation is not enabled");
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

#endif /* HAVE_LINUX_UDMABUF_H && HAVE_MEMFD_CREATE */

#ifdef ENABLE_GBM_ALLOCATION
#include <gbm.h>

#define GBM_BO_USE_SW_READ_RARELY (1 << 10)
#define GBM_BO_USE_SW_WRITE_RARELY (1 << 12)

static inline int
vkr_gbm_bo_get_fd(void *gbm_bo)
{
   assert(gbm_bo);

   /* gbm_bo_get_fd returns negative error code on failure */
   return gbm_bo_get_fd(gbm_bo);
}

static inline void
vkr_gbm_bo_destroy(void *gbm_bo)
{
   gbm_bo_destroy(gbm_bo);
}

static VkResult
vkr_gbm_get_fd_info_from_allocation_info(struct vkr_physical_device *physical_dev,
                                         const VkMemoryAllocateInfo *alloc_info,
                                         void **out_gbm_bo,
                                         VkImportMemoryFdInfoKHR *out_fd_info)
{
   const uint32_t flags =
      GBM_BO_USE_LINEAR | GBM_BO_USE_SW_READ_RARELY | GBM_BO_USE_SW_WRITE_RARELY;
   struct gbm_bo *gbm_bo;
   int fd = -1;

   assert(physical_dev->gbm_device);

   /*
    * Reject here for simplicity. Letting VkPhysicalDeviceVulkan11Properties return
    * min(maxMemoryAllocationSize, UINT32_MAX) will affect unmappable scenarios.
    */
   if (alloc_info->allocationSize > UINT32_MAX)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   /* Page alignment is used on all implementations we support. */
   const uint32_t alloc_size = align(alloc_info->allocationSize, getpagesize());
#ifdef MINIGBM
   const uint32_t format = GBM_FORMAT_R8;
   const uint32_t width = alloc_size;
   const uint32_t height = 1;
#else
   /* Mesa gbm has texture size limitations, so we can't rely on R8 here. Instead, we
    * allocate a large enough linear rgba8 buffer.
    */
   const uint32_t format = GBM_FORMAT_ABGR8888;
   const uint8_t pixel_bytes = 4;
   const uint32_t width =
      (uint32_t)ceil(sqrt((alloc_size + pixel_bytes - 1) / pixel_bytes));
   const uint32_t height = width;
#endif /* MINIGBM */

   gbm_bo = gbm_bo_create(physical_dev->gbm_device, width, height, format, flags);
   if (!gbm_bo)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   fd = vkr_gbm_bo_get_fd(gbm_bo);
   if (fd < 0) {
      vkr_gbm_bo_destroy(gbm_bo);
      return fd == -EMFILE ? VK_ERROR_TOO_MANY_OBJECTS : VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   *out_gbm_bo = (void *)gbm_bo;
   *out_fd_info = (VkImportMemoryFdInfoKHR){
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = alloc_info->pNext,
      .fd = fd,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   return VK_SUCCESS;
}

#else

static inline int
vkr_gbm_bo_get_fd(ASSERTED void *gbm_bo)
{
   vkr_log("minigbm_allocation is not enabled");
   assert(!gbm_bo);
   return -1;
}

static inline void
vkr_gbm_bo_destroy(ASSERTED void *gbm_bo)
{
   vkr_log("minigbm_allocation is not enabled");
   assert(!gbm_bo);
}

static inline VkResult
vkr_gbm_get_fd_info_from_allocation_info(UNUSED struct vkr_physical_device *physical_dev,
                                         UNUSED const VkMemoryAllocateInfo *alloc_info,
                                         UNUSED void **out_gbm_bo,
                                         UNUSED VkImportMemoryFdInfoKHR *out_fd_info)
{
   vkr_log("minigbm_allocation is not enabled");
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

#endif /* ENABLE_GBM_ALLOCATION */

static void
vkr_dispatch_vkAllocateMemory(struct vn_dispatch_context *dispatch,
                              struct vn_command_vkAllocateMemory *args)
{
   TRACE_FUNC();
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vkr_physical_device *physical_dev = dev->physical_device;

   VkMemoryAllocateInfo *alloc_info = (VkMemoryAllocateInfo *)args->pAllocateInfo;
   const uint32_t mem_type_index = alloc_info->memoryTypeIndex;
   if (unlikely(mem_type_index >= physical_dev->memory_properties.memoryTypeCount)) {
      args->ret = VK_ERROR_UNKNOWN;
      return;
   }

   /* translate VkImportMemoryResourceInfoMESA into VkImportMemoryFdInfoKHR in place */
   VkImportMemoryFdInfoKHR local_import_info = { .fd = -1 };
   VkImportMemoryResourceInfoMESA *res_info = NULL;
   VkBaseInStructure *prev_of_res_info = vkr_find_prev_struct(
      alloc_info, VK_STRUCTURE_TYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA);
   if (prev_of_res_info) {
      res_info = (VkImportMemoryResourceInfoMESA *)prev_of_res_info->pNext;
      if (!vkr_get_fd_info_from_resource_info(ctx, res_info, &local_import_info)) {
         args->ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
         return;
      }

      prev_of_res_info->pNext = (const struct VkBaseInStructure *)&local_import_info;
   }

   VkExportMemoryAllocateInfo *export_info =
      vkr_find_struct(alloc_info->pNext, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);

   /* track if driver has requested export allocation */
   const bool might_export = export_info && export_info->handleTypes;

   /* XXX Force dma_buf/opaque fd export or gbm bo import until a new extension that
    * supports direct export from host visible memory
    *
    * Most VkImage and VkBuffer are non-external while most VkDeviceMemory are external
    * if allocated with a host visible memory type. We still violate the spec by binding
    * external memory to non-external image or buffer, which needs spec changes with a
    * new extension.
    *
    * Skip forcing external if a valid VkImportMemoryResourceInfoMESA is provided, since
    * the mapping will be directly set up from the existing virgl resource.
    */
   const uint32_t property_flags =
      physical_dev->memory_properties.memoryTypes[mem_type_index].propertyFlags;
   uint32_t valid_fd_types = 0;
   int udmabuf_fd = -1;
   void *gbm_bo = NULL;
   struct vkr_mtl_shm *mtl_shm = NULL;
   VkExportMemoryAllocateInfo local_export_info;
   VkImportMemoryMetalHandleInfoEXT local_metal_import;

   if ((property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !res_info) {
      /* An implementation can support dma_buf import along with opaque fd export/import.
       * If the client driver is using external memory and requesting dma_buf, without
       * dma_buf fd export support, we must use gbm bo import path instead of forcing
       * opaque fd export. e.g. the client driver uses external memory for wsi image.
       */
      const bool no_dma_buf_export =
         !export_info ||
         !(export_info->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
      const bool force_gbm_import = !!physical_dev->gbm_device;
      const bool force_udmabuf_import = physical_dev->udmabuf_dev_fd >= 0;
      if (!(force_gbm_import || force_udmabuf_import) &&
          (physical_dev->is_dma_buf_fd_export_supported ||
           (physical_dev->is_opaque_fd_export_supported && no_dma_buf_export))) {
         const VkExternalMemoryHandleTypeFlagBits handle_type =
            physical_dev->is_dma_buf_fd_export_supported
               ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
               : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
         if (export_info) {
            export_info->handleTypes |= handle_type;
         } else {
            local_export_info = (const VkExportMemoryAllocateInfo){
               .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
               .pNext = alloc_info->pNext,
               .handleTypes = handle_type,
            };
            export_info = &local_export_info;
            alloc_info->pNext = &local_export_info;

            if (handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) {
               /* Guest virtgpu kernel aligns up blob mem size to the page boundary. No
                * matter dma-buf or opaque fd export allocation, the actual allocation
                * in most cases would follow the same padding. For dma-buf, we are able
                * to allocate and validate via lseek below. For opaque fd, Vulkan spec
                * requires to use the same size with allocation time for the mapper,
                * either vkr_allocator or vulkano, to import and populate the mapping.
                * Accessible mapping given by vkMapMemory is normally limited to just the
                * alloc size. When KVM register the mappings to guest pci bar, that
                * involves an invalid chunk towards the end of the page boundary. As a
                * result, any guest side accelerated instructions that rely on the
                * paddings can end up with Illegal instruction error. The most common
                * trigger is via memcpy'ing valid range to the guest side mapped buffe
                * memory, and then, e.g. __memcpy_avx_unaligned_erms  can hit the error.
                */
               alloc_info->allocationSize =
                  align(alloc_info->allocationSize, getpagesize());
            }
         }
      } else if (physical_dev->EXT_external_memory_metal) {
         /* Allocate shm and wrap as a MTLBuffer for import. */
         mtl_shm = vkr_mtl_shm_alloc(dev->mtl_device, alloc_info->allocationSize);
         if (!mtl_shm) {
            args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;
            return;
         }

         local_metal_import = (VkImportMemoryMetalHandleInfoEXT){
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_METAL_HANDLE_INFO_EXT,
            .pNext = alloc_info->pNext,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLBUFFER_BIT_EXT,
            .handle = mtl_shm->mtl_buffer,
         };
         alloc_info->pNext = &local_metal_import;
         alloc_info->allocationSize = mtl_shm->shm_size;
         valid_fd_types = 1 << VIRGL_RESOURCE_FD_SHM;
      } else if (physical_dev->EXT_external_memory_dma_buf) {
         /* Allocate dma_buf externally and force to import. */
         if (export_info) {
            /* Strip export info since valid_fd_types can only be dma_buf here. */
            VkBaseInStructure *prev_of_export_info = vkr_find_prev_struct(
               alloc_info, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);

            prev_of_export_info->pNext = export_info->pNext;
            export_info = NULL;
         }

         if (force_udmabuf_import) {
            /* To be noted, there are 2 limits for udmabuf:
             * - list_limit: udmabuf_create_list->count limit. Default is 1024.
             * - size_limit_mb: Max size of a dmabuf, in megabytes. Default is 64.
             */
            args->ret = vkr_udmabuf_get_fd_info_from_allocation_info(
               physical_dev, alloc_info, &udmabuf_fd, &local_import_info);
         } else {
            args->ret = vkr_gbm_get_fd_info_from_allocation_info(
               physical_dev, alloc_info, &gbm_bo, &local_import_info);
         }
         if (args->ret != VK_SUCCESS)
            return;

         alloc_info->pNext = &local_import_info;
         valid_fd_types = 1 << VIRGL_RESOURCE_FD_DMABUF;
      }
   }

   if (export_info) {
      if (export_info->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)
         valid_fd_types |= 1 << VIRGL_RESOURCE_FD_OPAQUE;
      if (export_info->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
         valid_fd_types |= 1 << VIRGL_RESOURCE_FD_DMABUF;
   }

   struct vkr_device_memory *mem = vkr_device_memory_create_and_add(ctx, args);
   if (!mem) {
      if (local_import_info.fd >= 0)
         close(local_import_info.fd);
      if (gbm_bo)
         vkr_gbm_bo_destroy(gbm_bo);
      vkr_mtl_shm_free(mtl_shm);
      return;
   }

   mem->device = dev;
   mem->might_export = might_export;
   mem->property_flags = property_flags;
   mem->valid_fd_types = valid_fd_types;
   mem->udmabuf_fd = udmabuf_fd;
   mem->gbm_bo = gbm_bo;
   mem->mtl_shm = mtl_shm;
   mem->allocation_size = alloc_info->allocationSize;
   mem->memory_type_index = mem_type_index;
#ifdef __OHOS__
   mem->context = ctx;
   mem->shadow_fd = -1;
   mem->shadow_map = NULL;
   mem->host_map = NULL;
   mem->shadow_size = 0;
   mem->shadow_sync_count = 0;
   mem->shadow_remote_flush_count = 0;
   mem->shadow_guest_write_depth = 0;
   mem->shadow_remote_invalidate_count = 0;
   mem->shadow_remote_active = false;
   mem->shadow_host_dirty = false;
   mem->shadow_initial_sync_done = false;
   mem->shadow_dirty_offset = 0;
   mem->shadow_dirty_size = 0;
   mem->shadow_dirty_ranges = NULL;
   mem->shadow_dirty_range_count = 0;
   mem->shadow_dirty_range_capacity = 0;
   mem->shadow_dirty_range_overflow = false;
   list_inithead(&mem->shadow_dirty_head);
   mem->shadow_dirty_listed = false;
   list_inithead(&mem->bound_buffers);
   mem->shadow_coverage_ranges = NULL;
   mem->shadow_coverage_range_count = 0;
   mem->shadow_coverage_range_capacity = 0;
   mem->shadow_coverage_valid = false;
   mem->shadow_upload_snapshot = NULL;
   mem->shadow_host_copy_deferred = false;
   mem->shadow_gpu_upload_covered = false;
   mem->shadow_gpu_upload_full_coverage = false;
   mem->shadow_pending_copy_bytes = 0;
   mem->shadow_pending_copy_count = 0;
#endif
}

static void
vkr_dispatch_vkFreeMemory(struct vn_dispatch_context *dispatch,
                          struct vn_command_vkFreeMemory *args)
{
   TRACE_FUNC();
   struct vkr_device_memory *mem = vkr_device_memory_from_handle(args->memory);
   if (!mem)
      return;

   vkr_device_memory_release(mem);
   vkr_device_memory_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetDeviceMemoryCommitment(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDeviceMemoryCommitment *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDeviceMemoryCommitment_args_handle(args);
   vk->GetDeviceMemoryCommitment(args->device, args->memory,
                                 args->pCommittedMemoryInBytes);
}

static void
vkr_dispatch_vkGetDeviceMemoryOpaqueCaptureAddress(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDeviceMemoryOpaqueCaptureAddress *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDeviceMemoryOpaqueCaptureAddress_args_handle(args);
   args->ret = vk->GetDeviceMemoryOpaqueCaptureAddress(args->device, args->pInfo);
}

static void
vkr_dispatch_vkFlushMappedMemoryRanges(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkFlushMappedMemoryRanges *args)
{
   struct vkr_context *ctx = dispatch->data;
   args->ret = VK_SUCCESS;

   vkr_device_memory_shadow_generation_begin(ctx, false);
   mtx_lock(&ctx->object_mutex);
   for (uint32_t i = 0; i < args->memoryRangeCount; i++) {
      const VkMappedMemoryRange *range = &args->pMemoryRanges[i];
      struct vkr_device_memory *mem = vkr_device_memory_from_handle(range->memory);
      if (!mem) {
         args->ret = VK_ERROR_MEMORY_MAP_FAILED;
         break;
      }

      args->ret = vkr_device_memory_flush_shadow_range(
         mem, range->offset, range->size);
      if (args->ret != VK_SUCCESS)
         break;
   }
   mtx_unlock(&ctx->object_mutex);
   vkr_device_memory_shadow_generation_end(ctx);
}

static void
vkr_dispatch_vkInvalidateMappedMemoryRanges(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkInvalidateMappedMemoryRanges *args)
{
   struct vkr_context *ctx = dispatch->data;
   args->ret = VK_SUCCESS;
#ifdef __OHOS__
   const char *mode = os_get_option("VKR_WINEHUA_SHADOW_FROM_HOST");
   const bool precise = mode && !strcmp(mode, "precise");
#endif

   mtx_lock(&ctx->object_mutex);
   for (uint32_t i = 0; i < args->memoryRangeCount; i++) {
      struct vkr_device_memory *mem =
         vkr_device_memory_from_handle(args->pMemoryRanges[i].memory);
      if (!mem) {
         args->ret = VK_ERROR_MEMORY_MAP_FAILED;
         break;
      }
#ifdef __OHOS__
      if (precise) {
         args->ret = vkr_device_memory_invalidate_shadow_range(
            mem, args->pMemoryRanges[i].offset,
            args->pMemoryRanges[i].size);
         if (args->ret != VK_SUCCESS)
            break;
      } else {
         /* Legacy WineHua DXVK builds used invalidate as a write-begin
          * marker.  Keep that contract outside the precise profile so old
          * runtime overlays remain bisectable. */
         mem->shadow_guest_write_depth++;
         mem->shadow_remote_active = true;
         const uint32_t count = mem->shadow_remote_invalidate_count++;
         if (vkr_ohos_shadow_trace_enabled() &&
             (count < 8 || !(count % 60)))
            vkr_log("OHOS shadow guest write begin count=%u mem=%p depth=%u",
                    count + 1, mem, mem->shadow_guest_write_depth);
      }
#endif
   }
   mtx_unlock(&ctx->object_mutex);
}

static void
vkr_dispatch_vkGetMemoryResourcePropertiesMESA(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetMemoryResourcePropertiesMESA *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   struct vkr_resource *res = vkr_context_get_resource(ctx, args->resourceId);
   if (!res) {
      vkr_log("failed to query resource props: invalid res_id %u", args->resourceId);
      vkr_context_set_fatal(ctx);
      return;
   }

   if (res->fd_type != VIRGL_RESOURCE_FD_DMABUF) {
      args->ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      return;
   }

   static const VkExternalMemoryHandleTypeFlagBits handle_type =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
   VkMemoryFdPropertiesKHR mem_fd_props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
      .pNext = NULL,
      .memoryTypeBits = 0,
   };
   vn_replace_vkGetMemoryResourcePropertiesMESA_args_handle(args);
   args->ret =
      vk->GetMemoryFdPropertiesKHR(args->device, handle_type, res->u.fd, &mem_fd_props);
   if (args->ret != VK_SUCCESS)
      return;

   args->pMemoryResourceProperties->memoryTypeBits = mem_fd_props.memoryTypeBits;

   VkMemoryResourceAllocationSizePropertiesMESA *alloc_size_props =
      vkr_find_struct(args->pMemoryResourceProperties->pNext,
                      VK_STRUCTURE_TYPE_MEMORY_RESOURCE_ALLOCATION_SIZE_PROPERTIES_MESA);
   if (alloc_size_props)
      alloc_size_props->allocationSize = res->size;
}

void
vkr_context_init_device_memory_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkAllocateMemory = vkr_dispatch_vkAllocateMemory;
   dispatch->dispatch_vkFreeMemory = vkr_dispatch_vkFreeMemory;
   dispatch->dispatch_vkMapMemory = NULL;
   dispatch->dispatch_vkUnmapMemory = NULL;
   dispatch->dispatch_vkFlushMappedMemoryRanges =
      vkr_dispatch_vkFlushMappedMemoryRanges;
   dispatch->dispatch_vkInvalidateMappedMemoryRanges =
      vkr_dispatch_vkInvalidateMappedMemoryRanges;
   dispatch->dispatch_vkGetDeviceMemoryCommitment =
      vkr_dispatch_vkGetDeviceMemoryCommitment;
   dispatch->dispatch_vkGetDeviceMemoryOpaqueCaptureAddress =
      vkr_dispatch_vkGetDeviceMemoryOpaqueCaptureAddress;

   dispatch->dispatch_vkGetMemoryResourcePropertiesMESA =
      vkr_dispatch_vkGetMemoryResourcePropertiesMESA;
}

void
vkr_device_memory_release(struct vkr_device_memory *mem)
{
#ifdef __OHOS__
   if (mem->context) {
      mtx_lock(&mem->context->object_mutex);
      vkr_ohos_shadow_dirty_list_remove(mem);
      list_for_each_entry_safe (struct vkr_buffer, buffer,
                                &mem->bound_buffers, memory_head) {
         list_del(&buffer->memory_head);
         list_inithead(&buffer->memory_head);
         buffer->memory_listed = false;
         buffer->bound_memory = NULL;
         buffer->bound_memory_offset = 0;
      }
      mtx_unlock(&mem->context->object_mutex);
   }

   if (mem->host_map) {
      struct vn_device_proc_table *vk = &mem->device->proc_table;
      vk->UnmapMemory(mem->device->base.handle.device,
                      mem->base.handle.device_memory);
      mem->host_map = NULL;
   }
   if (mem->shadow_map) {
      munmap(mem->shadow_map, mem->shadow_size);
      mem->shadow_map = NULL;
   }
   if (mem->shadow_fd >= 0) {
      close(mem->shadow_fd);
      mem->shadow_fd = -1;
   }
   free(mem->shadow_dirty_ranges);
   mem->shadow_dirty_ranges = NULL;
   free(mem->shadow_coverage_ranges);
   mem->shadow_coverage_ranges = NULL;
   free(mem->shadow_upload_snapshot);
   mem->shadow_upload_snapshot = NULL;
   mem->shadow_host_copy_deferred = false;
   mem->shadow_dirty_range_count = 0;
   mem->shadow_dirty_range_capacity = 0;
   mem->shadow_coverage_range_count = 0;
   mem->shadow_coverage_range_capacity = 0;
   mem->shadow_coverage_valid = false;
#endif
   vkr_mtl_shm_free(mem->mtl_shm);
   if (mem->gbm_bo)
      vkr_gbm_bo_destroy(mem->gbm_bo);
   if (mem->udmabuf_fd >= 0)
      close(mem->udmabuf_fd);
}

bool
vkr_device_memory_export_blob(struct vkr_device_memory *mem,
                              uint64_t blob_size,
                              uint32_t blob_flags,
                              struct virgl_context_blob *out_blob)
{
   TRACE_FUNC();

   /* a memory can only be exported once; we don't want two resources to point
    * to the same storage.
    */
   if (mem->exported) {
      vkr_log("mem has been exported");
      return false;
   }

   uint32_t map_info = VIRGL_RENDERER_MAP_CACHE_NONE;
   if (blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE) {
      const bool visible = mem->property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      const bool coherent = mem->property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      const bool cached = mem->property_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
      if (!visible) {
         vkr_log("mem cannot support mappable blob");
         return false;
      }

      /* XXX guessed */
      map_info = (coherent && cached) ? VIRGL_RENDERER_MAP_CACHE_CACHED
                                      : VIRGL_RENDERER_MAP_CACHE_WC;
   }

   if (mem->mtl_shm && mem->mtl_shm->shm_fd >= 0) {
      mem->exported = true;
      *out_blob = (struct virgl_context_blob){
         .type = VIRGL_RESOURCE_FD_SHM,
         .u.fd = os_dupfd_cloexec(mem->mtl_shm->shm_fd),
         .map_info = map_info,
      };
      return out_blob->u.fd >= 0;
   }

   const bool can_export_dma_buf = mem->valid_fd_types & (1 << VIRGL_RESOURCE_FD_DMABUF);
   const bool can_export_opaque = mem->valid_fd_types & (1 << VIRGL_RESOURCE_FD_OPAQUE);
   enum virgl_resource_fd_type fd_type;
   VkExternalMemoryHandleTypeFlagBits handle_type;
   struct virgl_resource_vulkan_info vulkan_info;
   if (blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_CROSS_DEVICE) {
      if (!can_export_dma_buf) {
         vkr_log("mem cannot export to dma_buf for cross device blob sharing");
         return false;
      }
      fd_type = VIRGL_RESOURCE_FD_DMABUF;
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
   } else if (can_export_dma_buf) {
      /* prefer dmabuf for easier mapping? */
      fd_type = VIRGL_RESOURCE_FD_DMABUF;
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
   } else if (can_export_opaque) {
      /* prefer opaque for performance? */
      fd_type = VIRGL_RESOURCE_FD_OPAQUE;
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

      STATIC_ASSERT(sizeof(vulkan_info.device_uuid) == VK_UUID_SIZE);
      STATIC_ASSERT(sizeof(vulkan_info.driver_uuid) == VK_UUID_SIZE);

      const VkPhysicalDeviceIDProperties *id_props =
         &mem->device->physical_device->id_properties;
      memcpy(vulkan_info.device_uuid, id_props->deviceUUID, VK_UUID_SIZE);
      memcpy(vulkan_info.driver_uuid, id_props->driverUUID, VK_UUID_SIZE);

      vulkan_info.allocation_size = mem->allocation_size;
      vulkan_info.memory_type_index = mem->memory_type_index;
   } else {
#ifdef __OHOS__
      if (blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_CROSS_DEVICE) {
         vkr_log("OHOS shadow memory cannot support cross-device export");
         return false;
      }

      const uint64_t page_size = (uint64_t)getpagesize();
      const uint64_t shadow_size = align(MAX2(blob_size, mem->allocation_size), page_size);
      const bool mappable = blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE;
      if (mappable && !(mem->property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
         vkr_log("OHOS shadow requested for non-host-visible memory");
         return false;
      }

      int shadow_fd = os_create_anonymous_file(shadow_size, "vkr-ohos-shadow");
      if (shadow_fd < 0) {
         vkr_log("failed to allocate OHOS shadow fd: %s", strerror(errno));
         return false;
      }
      void *shadow_map = mmap(NULL, shadow_size, PROT_READ | PROT_WRITE,
                              MAP_SHARED, shadow_fd, 0);
      if (shadow_map == MAP_FAILED) {
         vkr_log("failed to map OHOS shadow fd: %s", strerror(errno));
         close(shadow_fd);
         return false;
      }

      struct vn_device_proc_table *vk = &mem->device->proc_table;
      void *host_map = NULL;
      if (mappable) {
         VkResult result = vk->MapMemory(mem->device->base.handle.device,
                                         mem->base.handle.device_memory,
                                         0, VK_WHOLE_SIZE, 0, &host_map);
         if (result != VK_SUCCESS) {
            vkr_log("failed to map OHOS Host Vulkan memory (%d)", result);
            munmap(shadow_map, shadow_size);
            close(shadow_fd);
            return false;
         }
      }

      int exported_fd = os_dupfd_cloexec(shadow_fd);
      if (exported_fd < 0) {
         if (host_map) {
            vk->UnmapMemory(mem->device->base.handle.device,
                            mem->base.handle.device_memory);
         }
         munmap(shadow_map, shadow_size);
         close(shadow_fd);
         return false;
      }

      mem->shadow_fd = shadow_fd;
      mem->shadow_map = shadow_map;
      mem->host_map = host_map;
      mem->shadow_size = shadow_size;
      if (host_map) {
         const bool coherent =
            mem->property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
         if (!coherent) {
            const VkMappedMemoryRange range = {
               .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
               .memory = mem->base.handle.device_memory,
               .offset = 0,
               .size = VK_WHOLE_SIZE,
            };
            vk->InvalidateMappedMemoryRanges(mem->device->base.handle.device, 1, &range);
         }
         const size_t copy_size =
            (size_t)MIN2(mem->shadow_size, mem->allocation_size);
         memcpy(mem->shadow_map, mem->host_map, copy_size);

      }
      mem->exported = true;
      *out_blob = (struct virgl_context_blob){
         .type = VIRGL_RESOURCE_FD_SHM,
         .u.fd = exported_fd,
         .map_info = mappable ? map_info : VIRGL_RENDERER_MAP_CACHE_NONE,
      };
      vkr_log("using OHOS shadow memory size=%" PRIu64
              " allocation=%" PRIu64 " mem=%p guest_memory=%" PRIu64
              " flags=0x%x mappable=%d coherent=%d",
              shadow_size, mem->allocation_size, mem,
              (uint64_t)mem->base.id, mem->property_flags, mappable,
              !!(mem->property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
      return true;
#else
      vkr_log("mem is not exportable");
      return false;
#endif
   }

   int fd;
   if (mem->udmabuf_fd >= 0) {
      fd = os_dupfd_cloexec(mem->udmabuf_fd);
      if (fd < 0) {
         vkr_log("mem udmabuf fd dup failed (%s)", strerror(errno));
         return false;
      }
   } else if (mem->gbm_bo) {
      assert(handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
      assert(can_export_dma_buf && !can_export_opaque);

      fd = vkr_gbm_bo_get_fd(mem->gbm_bo);
      if (fd < 0) {
         vkr_log("mem gbm bo export failed (ret %d)", fd);
         return false;
      }
   } else {
      struct vn_device_proc_table *vk = &mem->device->proc_table;
      const VkMemoryGetFdInfoKHR fd_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
         .memory = mem->base.handle.device_memory,
         .handleType = handle_type,
      };
      VkResult ret = vk->GetMemoryFdKHR(mem->device->base.handle.device, &fd_info, &fd);
      if (ret != VK_SUCCESS) {
         vkr_log("mem fd export failed (vk ret %d)", ret);
         return false;
      }
   }

   if (fd_type == VIRGL_RESOURCE_FD_DMABUF) {
      const off_t dma_buf_size = lseek(fd, 0, SEEK_END);
      if (dma_buf_size < 0 || (uint64_t)dma_buf_size < blob_size) {
         vkr_log("mem dma_buf_size %lld < blob_size %" PRIu64, (long long)dma_buf_size,
                 blob_size);
         close(fd);
         return false;
      }
   }

   mem->exported = true;

   *out_blob = (struct virgl_context_blob){
      .type = fd_type,
      .u.fd = fd,
      .map_info = map_info,
      .vulkan_info = vulkan_info,
   };

   return true;
}

static struct vkr_shadow_sync_stats
vkr_device_memory_sync_shadow(struct vkr_device_memory *mem, bool to_host,
                              bool host_flush_prepared)
{
   struct vkr_shadow_sync_stats stats = { 0 };
#ifdef __OHOS__
   if (!mem->host_map || !mem->shadow_map || !mem->shadow_size)
      return stats;

   struct vn_device_proc_table *vk = &mem->device->proc_table;
   const VkMappedMemoryRange range = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = mem->base.handle.device_memory,
      .offset = 0,
      .size = VK_WHOLE_SIZE,
   };
   const bool coherent = mem->property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   const size_t copy_size = (size_t)MIN2(mem->shadow_size, mem->allocation_size);
   const char *to_host_mode = os_get_option("VKR_WINEHUA_SHADOW_TO_HOST");
   const bool explicit_to_host =
      to_host_mode && (!strcmp(to_host_mode, "explicit") ||
                       !strcmp(to_host_mode, "explicit-only"));

   /* CmdUpdateBuffer has already copied this exact mapped range into the
    * Host buffer.  Do not repeat the same bytes through vkMapMemory.  The
    * dirty state is retired here so later submits do not rescan it. */
   if (to_host && mem->shadow_remote_active && mem->shadow_host_dirty &&
       vkr_device_memory_gpu_upload_enabled(mem->device) &&
       mem->shadow_gpu_upload_full_coverage) {
      stats.gpu_upload_skipped_bytes = mem->shadow_pending_copy_bytes;
      stats.gpu_upload_skipped_copies = mem->shadow_pending_copy_count;
      if (vkr_ohos_shadow_trace_enabled())
         vkr_log("OHOS shadow host sync skipped gpu-upload-covered guestMemory=%" PRIu64
                 " bytes=%" PRIu64 " copies=%u",
                 (uint64_t)mem->base.id, stats.gpu_upload_skipped_bytes,
                 stats.gpu_upload_skipped_copies);
      mem->shadow_pending_copy_bytes = 0;
      mem->shadow_pending_copy_count = 0;
      mem->shadow_host_dirty = false;
      mem->shadow_dirty_range_count = 0;
      mem->shadow_dirty_range_overflow = false;
      mem->shadow_host_copy_deferred = false;
      mem->shadow_gpu_upload_full_coverage = false;
      vkr_ohos_shadow_dirty_list_remove(mem);
      return stats;
   }

   if (to_host && mem->shadow_remote_active) {
      if (!mem->shadow_host_dirty) {
         vkr_ohos_shadow_dirty_list_remove(mem);
         return stats;
      }

      const bool had_deferred_copy = mem->shadow_host_copy_deferred &&
         mem->shadow_upload_snapshot;

      if (vkr_ohos_shadow_trace_enabled()) {
         const VkDeviceSize available = MIN2(mem->shadow_size,
                                             mem->allocation_size);
         const VkDeviceSize trace_offset = MIN2(mem->shadow_dirty_offset,
                                                available);
         const VkDeviceSize trace_size = MIN2(mem->shadow_dirty_size,
                                              available - trace_offset);
         const uint8_t *shadow = mem->shadow_map;
         const uint8_t *host = mem->host_map;
         const uint32_t shadow_hash = vkr_ohos_fnv1a32(
            shadow + trace_offset, (size_t)trace_size);
         const uint32_t host_hash = vkr_ohos_fnv1a32(
            host + trace_offset, (size_t)trace_size);
         vkr_log("OHOS shadow submit-input guestMemory=%" PRIu64
                 " hostMemory=0x%" PRIxPTR " offset=%" PRIu64
                 " size=%" PRIu64 " shadowFnv=0x%08x hostFnv=0x%08x"
                 " equal=%u",
                 (uint64_t)mem->base.id,
                 (uintptr_t)mem->base.handle.device_memory,
                 (uint64_t)trace_offset, (uint64_t)trace_size,
                 shadow_hash, host_hash, shadow_hash == host_hash);
      }

      if (mem->shadow_host_copy_deferred &&
          mem->shadow_upload_snapshot) {
         if (!mem->shadow_dirty_range_overflow &&
             mem->shadow_dirty_range_count) {
            for (uint32_t i = 0; i < mem->shadow_dirty_range_count; i++) {
               const struct vkr_ohos_shadow_dirty_range *copy_range =
                  &mem->shadow_dirty_ranges[i];
               memcpy((uint8_t *)mem->host_map + copy_range->offset,
                      (const uint8_t *)mem->shadow_upload_snapshot +
                         copy_range->offset,
                      (size_t)copy_range->size);
            }
         } else {
            memcpy((uint8_t *)mem->host_map + mem->shadow_dirty_offset,
                   (const uint8_t *)mem->shadow_upload_snapshot +
                      mem->shadow_dirty_offset,
                   (size_t)mem->shadow_dirty_size);
         }
         mem->shadow_host_copy_deferred = false;
      }

      const VkMappedMemoryRange dirty_range = {
         .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
         .memory = mem->base.handle.device_memory,
         .offset = mem->shadow_dirty_offset,
         .size = mem->shadow_dirty_size,
      };
      if (vkr_ohos_shadow_msync_enabled()) {
         const uintptr_t page_size = (uintptr_t)getpagesize();
         const uintptr_t begin =
            (uintptr_t)mem->host_map + (uintptr_t)mem->shadow_dirty_offset;
         const uintptr_t end = begin + (uintptr_t)mem->shadow_dirty_size;
         const uintptr_t page_begin = begin & ~(page_size - 1u);
         const uintptr_t page_end = (end + page_size - 1u) & ~(page_size - 1u);
         errno = 0;
         const int msync_result = msync((void *)page_begin,
                                        page_end - page_begin, MS_SYNC);
         vkr_log("OHOS shadow dirty msync guestMemory=%" PRIu64
                 " offset=%" PRIu64 " size=%" PRIu64
                 " pageBytes=%" PRIuPTR " result=%d errno=%d",
                 (uint64_t)mem->base.id,
                 (uint64_t)mem->shadow_dirty_offset,
                 (uint64_t)mem->shadow_dirty_size,
                 page_end - page_begin, msync_result,
                 msync_result ? errno : 0);
      }
      if (!host_flush_prepared)
         vkr_ohos_gate_c_trace_memory("flush-entry", mem,
                                      mem->shadow_dirty_offset,
                                      mem->shadow_dirty_size, false,
                                      VK_SUCCESS);
      VkResult dirty_result = host_flush_prepared ? VK_SUCCESS :
         vk->FlushMappedMemoryRanges(mem->device->base.handle.device, 1,
                                     &dirty_range);
      stats.result = dirty_result;
      if (!host_flush_prepared)
         vkr_ohos_gate_c_trace_memory("flush-result", mem,
                                      mem->shadow_dirty_offset,
                                      mem->shadow_dirty_size, true,
                                      dirty_result);
      if (dirty_result != VK_SUCCESS)
         vkr_log("OHOS shadow dirty flush failed result=%d offset=%" PRIu64
                 " size=%" PRIu64,
                 dirty_result, (uint64_t)mem->shadow_dirty_offset,
                 (uint64_t)mem->shadow_dirty_size);
      if (vkr_ohos_shadow_trace_enabled()) {
         const VkDeviceSize available = MIN2(mem->shadow_size,
                                             mem->allocation_size);
         const VkDeviceSize trace_offset = MIN2(mem->shadow_dirty_offset,
                                                available);
         const VkDeviceSize trace_size = MIN2(mem->shadow_dirty_size,
                                              available - trace_offset);
         const uint8_t *expected = had_deferred_copy
            ? mem->shadow_upload_snapshot : mem->shadow_map;
         const uint8_t *host = mem->host_map;
         const uint32_t expected_hash = vkr_ohos_fnv1a32(
            expected + trace_offset, (size_t)trace_size);
         const uint32_t host_hash = vkr_ohos_fnv1a32(
            host + trace_offset, (size_t)trace_size);
         vkr_log("OHOS shadow submit-output guestMemory=%" PRIu64
                 " hostMemory=0x%" PRIxPTR " offset=%" PRIu64
                 " size=%" PRIu64 " expectedFnv=0x%08x hostFnv=0x%08x"
                 " equal=%u source=%s flushResult=%d",
                 (uint64_t)mem->base.id,
                 (uintptr_t)mem->base.handle.device_memory,
                 (uint64_t)trace_offset, (uint64_t)trace_size,
                 expected_hash, host_hash, expected_hash == host_hash,
                 had_deferred_copy ? "snapshot" : "shadow", dirty_result);
      }
      if (vkr_ohos_shadow_submit_unmap_large_enabled() &&
          mem->allocation_size >= 1024u * 1024u) {
         void *old_host_map = mem->host_map;
         vk->UnmapMemory(mem->device->base.handle.device,
                         mem->base.handle.device_memory);
         mem->host_map = NULL;
         vkr_log("OHOS shadow submit unmap-large guestMemory=%" PRIu64
                 " hostMemory=0x%" PRIxPTR " bytes=%" PRIu64
                 " oldMap=%p",
                 (uint64_t)mem->base.id,
                 (uintptr_t)mem->base.handle.device_memory,
                 (uint64_t)mem->shadow_dirty_size,
                 old_host_map);
      }
      stats.bytes = mem->shadow_pending_copy_bytes;
      stats.copies = mem->shadow_pending_copy_count;
      stats.cache_ops = 1;
      mem->shadow_pending_copy_bytes = 0;
      mem->shadow_pending_copy_count = 0;
      mem->shadow_host_dirty = false;
      mem->shadow_dirty_range_count = 0;
      mem->shadow_dirty_range_overflow = false;
      mem->shadow_host_copy_deferred = false;
      mem->shadow_gpu_upload_full_coverage = false;
      vkr_ohos_shadow_dirty_list_remove(mem);
      return stats;
   }

   if (to_host && explicit_to_host) {
      if (mem->shadow_initial_sync_done)
         return stats;

      memcpy(mem->host_map, mem->shadow_map, copy_size);
      vkr_ohos_gate_c_trace_memory("flush-entry", mem, 0, copy_size, false,
                                   VK_SUCCESS);
      VkResult result =
         vk->FlushMappedMemoryRanges(mem->device->base.handle.device, 1, &range);
      vkr_ohos_gate_c_trace_memory("flush-result", mem, 0, copy_size, true,
                                   result);
      if (result != VK_SUCCESS)
         vkr_log("OHOS shadow initial flush failed result=%d coherent=%d size=%zu",
                 result, coherent, copy_size);
      stats.bytes = copy_size;
      stats.copies = copy_size ? 1 : 0;
      stats.cache_ops = 1;
      mem->shadow_initial_sync_done = true;
      mem->shadow_sync_count++;
      if (vkr_ohos_shadow_trace_enabled())
         vkr_log("OHOS shadow explicit initial guest_memory=%" PRIu64
                 " bytes=%zu result=%d",
                 (uint64_t)mem->base.id, copy_size, result);
      return stats;
   }

   if (to_host) {
      size_t first_diff = copy_size;
      if (vkr_ohos_shadow_trace_enabled() && mem->shadow_sync_count < 16) {
         const uint8_t *shadow = mem->shadow_map;
         const uint8_t *host = mem->host_map;
         const size_t page_size = 4096;
         for (size_t offset = 0; offset < copy_size; offset += page_size) {
            const size_t chunk = MIN2(page_size, copy_size - offset);
            if (memcmp(shadow + offset, host + offset, chunk)) {
               size_t byte = 0;
               while (byte < chunk && shadow[offset + byte] == host[offset + byte])
                  byte++;
               first_diff = offset + byte;
               break;
            }
         }
         if (first_diff < copy_size) {
            const uint8_t *shadow = mem->shadow_map;
            const uint8_t *host = mem->host_map;
            const size_t available = MIN2((size_t)16, copy_size - first_diff);
            uint64_t shadow_word0 = 0, shadow_word1 = 0;
            uint64_t host_word0 = 0, host_word1 = 0;
            memcpy(&shadow_word0, shadow + first_diff, MIN2((size_t)8, available));
            memcpy(&host_word0, host + first_diff, MIN2((size_t)8, available));
            if (available > 8) {
               memcpy(&shadow_word1, shadow + first_diff + 8, available - 8);
               memcpy(&host_word1, host + first_diff + 8, available - 8);
            }
            vkr_log("OHOS shadow diff sync=%u mem=%p offset=%zu "
                    "shadow=%016" PRIx64 "%016" PRIx64
                    " host=%016" PRIx64 "%016" PRIx64,
                    mem->shadow_sync_count, mem, first_diff,
                    shadow_word0, shadow_word1, host_word0, host_word1);
         }
      }
      memcpy(mem->host_map, mem->shadow_map, copy_size);
      stats.bytes = copy_size;
      stats.copies = copy_size ? 1 : 0;
      /* Maleoon advertises some host-visible heaps as coherent, but mapped
       * writes made through the OHOS shadow bridge are not always visible to
       * shader reads without an explicit cache-domain transition.  Flushing a
       * coherent range is valid Vulkan and is a no-op on conformant coherent
       * implementations, so force it for the OHOS compatibility path. */
      vkr_ohos_gate_c_trace_memory("flush-entry", mem, 0, copy_size, false,
                                   VK_SUCCESS);
      VkResult result =
         vk->FlushMappedMemoryRanges(mem->device->base.handle.device, 1, &range);
      vkr_ohos_gate_c_trace_memory("flush-result", mem, 0, copy_size, true,
                                   result);
      if (result != VK_SUCCESS)
         vkr_log("OHOS shadow flush failed result=%d coherent=%d size=%zu",
                 result, coherent, copy_size);
      stats.cache_ops = 1;
      mem->shadow_initial_sync_done = true;
      mem->shadow_sync_count++;
   } else {
      if (mem->shadow_guest_write_depth)
         return stats;
      vkr_ohos_gate_c_trace_memory("invalidate-entry", mem, 0, copy_size,
                                   false, VK_SUCCESS);
      VkResult result =
         vk->InvalidateMappedMemoryRanges(mem->device->base.handle.device, 1, &range);
      vkr_ohos_gate_c_trace_memory("invalidate-result", mem, 0, copy_size,
                                   true, result);
      if (result != VK_SUCCESS)
         vkr_log("OHOS shadow invalidate failed result=%d coherent=%d size=%zu",
                 result, coherent, copy_size);
      memcpy(mem->shadow_map, mem->host_map, copy_size);
      if (mem->shadow_upload_snapshot)
         memcpy(mem->shadow_upload_snapshot, mem->host_map, copy_size);
      stats.bytes = copy_size;
      stats.copies = copy_size ? 1 : 0;
      stats.cache_ops = 1;
      if (mem->shadow_remote_active) {
         mem->shadow_host_dirty = false;
         mem->shadow_dirty_range_count = 0;
         mem->shadow_dirty_range_overflow = false;
         vkr_ohos_shadow_dirty_list_remove(mem);
      }
   }
#else
   (void)mem;
   (void)to_host;
#endif
   return stats;
}

static VkResult
vkr_device_memory_flush_shadow_range(struct vkr_device_memory *mem,
                                     VkDeviceSize offset,
                                     VkDeviceSize size)
{
#ifdef __OHOS__
   if (!mem->host_map || !mem->shadow_map || !mem->shadow_size) {
      if (mem->shadow_guest_write_depth)
         mem->shadow_guest_write_depth--;
      return VK_SUCCESS;
   }

   if (offset > mem->allocation_size)
      return VK_ERROR_MEMORY_MAP_FAILED;

   const VkDeviceSize available = MIN2(
      mem->allocation_size - offset,
      mem->shadow_size > offset ? mem->shadow_size - offset : 0);
   const VkDeviceSize copy_size = size == VK_WHOLE_SIZE
      ? available
      : MIN2(size, available);
   const bool gate_c = vkr_ohos_gate_c_trace_enabled();

   vkr_ohos_gate_c_trace_memory("flush-request", mem, offset, copy_size,
                                false, VK_SUCCESS);

   bool deferred_copy = false;
   if (copy_size && !gate_c &&
       vkr_ohos_shadow_defer_host_copy_enabled(mem->device)) {
      if (!mem->shadow_upload_snapshot) {
         const size_t snapshot_size = (size_t)MIN2(
            mem->shadow_size, mem->allocation_size);
         mem->shadow_upload_snapshot = malloc(snapshot_size);
         if (mem->shadow_upload_snapshot)
            memcpy(mem->shadow_upload_snapshot, mem->host_map,
                   snapshot_size);
         else
            vkr_log("OHOS shadow upload snapshot allocation failed bytes=%" PRIu64,
                    mem->shadow_size);
      }
      if (mem->shadow_upload_snapshot) {
         memcpy((uint8_t *)mem->shadow_upload_snapshot + offset,
                (const uint8_t *)mem->shadow_map + offset,
                (size_t)copy_size);
         mem->shadow_host_copy_deferred = true;
         deferred_copy = true;
      }
   }
   if (!deferred_copy)
      memcpy((uint8_t *)mem->host_map + offset,
             (const uint8_t *)mem->shadow_map + offset,
             (size_t)copy_size);
   if (vkr_ohos_ubo_identity_trace_verbose() &&
       (copy_size == 48 || copy_size == 1536) &&
       vkr_ohos_ubo_identity_trace_allow(
          &vkr_ohos_ubo_flush_trace_count, "flush")) {
      const uint8_t *source = deferred_copy && mem->shadow_upload_snapshot
         ? mem->shadow_upload_snapshot : mem->shadow_map;
      vkr_log("WineHuaUboHost: phase=flush submitGeneration=%" PRIu64
              " memoryId=%" PRIu64 " hostMemory=0x%" PRIxPTR
              " absoluteOffset=%" PRIu64 " bytes=%" PRIu64
              " sourceHash=%016" PRIx64 " deferred=%u",
              vkr_winehua_queue_submit_generation(),
              (uint64_t)mem->base.id,
              (uintptr_t)mem->base.handle.device_memory,
              (uint64_t)offset, (uint64_t)copy_size,
              vkr_ohos_fnv1a64(source + offset, (size_t)copy_size),
              deferred_copy);
   }

   /* VKD3D's explicit map flush is a synchronous API boundary.  The regular
    * shadow path batches writes at queue submit, but this isolated Gate C
    * path must execute the requested range before replying to the caller. */
   if (gate_c && !deferred_copy && copy_size) {
      struct vn_device_proc_table *vk = &mem->device->proc_table;
      const VkMappedMemoryRange range = {
         .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
         .memory = mem->base.handle.device_memory,
         .offset = offset,
         .size = size == VK_WHOLE_SIZE ? VK_WHOLE_SIZE : copy_size,
      };
      vkr_ohos_gate_c_trace_memory("flush-direct-entry", mem, offset,
                                   copy_size, false, VK_SUCCESS);
      const VkResult result = vk->FlushMappedMemoryRanges(
         mem->device->base.handle.device, 1, &range);
      vkr_ohos_gate_c_trace_memory("flush-direct-result", mem, offset,
                                   copy_size, true, result);
      if (mem->shadow_guest_write_depth)
         mem->shadow_guest_write_depth--;
      if (result == VK_SUCCESS)
         mem->shadow_remote_active = true;
      return result;
   }

   vkr_ohos_record_shadow_dirty_range(mem, offset, copy_size);
   mem->shadow_pending_copy_bytes += copy_size;
   if (copy_size)
      mem->shadow_pending_copy_count++;

   mem->shadow_remote_active = true;
   if (!mem->shadow_host_dirty) {
      mem->shadow_dirty_offset = offset;
      mem->shadow_dirty_size = copy_size;
      mem->shadow_host_dirty = true;
   } else {
      const VkDeviceSize dirtyBegin = MIN2(mem->shadow_dirty_offset, offset);
      const VkDeviceSize dirtyEnd = MAX2(
         mem->shadow_dirty_offset + mem->shadow_dirty_size,
         offset + copy_size);
      mem->shadow_dirty_offset = dirtyBegin;
      mem->shadow_dirty_size = dirtyEnd - dirtyBegin;
   }
   vkr_ohos_shadow_dirty_list_add(mem);

   if (mem->shadow_guest_write_depth)
      mem->shadow_guest_write_depth--;

   const uint32_t flush_count = mem->shadow_remote_flush_count++;
   if (vkr_ohos_shadow_trace_enabled() &&
       (flush_count < 8 || !(flush_count % 60)))
      vkr_log("OHOS shadow remote flush count=%u mem=%p offset=%" PRIu64
              " size=%" PRIu64 " result=0",
              flush_count + 1, mem, (uint64_t)offset,
              (uint64_t)copy_size);

   vkr_ohos_gate_c_trace_memory("flush-staged", mem, offset, copy_size,
                                true, VK_SUCCESS);

   /* The queue-submit path performs the actual Host Vulkan cache-domain
    * flush once per submit.  Doing it here for every 64-byte slice is
    * correct but disproportionately expensive on Maleoon. */
   return VK_SUCCESS;
#else
   (void)mem;
   (void)offset;
   (void)size;
   return VK_SUCCESS;
#endif
}

static VkResult
vkr_device_memory_invalidate_shadow_range(struct vkr_device_memory *mem,
                                           VkDeviceSize offset,
                                           VkDeviceSize size)
{
#ifdef __OHOS__
   if (!mem->host_map || !mem->shadow_map || !mem->shadow_size)
      return VK_SUCCESS;

   if (offset > mem->allocation_size)
      return VK_ERROR_MEMORY_MAP_FAILED;

   const VkDeviceSize available = MIN2(
      mem->allocation_size - offset,
      mem->shadow_size > offset ? mem->shadow_size - offset : 0);
   const VkDeviceSize copy_size = size == VK_WHOLE_SIZE
      ? available
      : MIN2(size, available);
   if (!copy_size)
      return VK_SUCCESS;

   /* Inline GPU upload keeps explicit Guest flushes in a snapshot until the
    * next queue submit.  An invalidate is a conflicting Host access boundary:
    * retire all earlier Guest writes before Host memory is read back, or the
    * stale Host mapping would overwrite the shared shadow. */
   if (mem->shadow_host_dirty) {
      const struct vkr_shadow_sync_stats pending =
         vkr_device_memory_sync_shadow(mem, true, false);
      if (pending.result != VK_SUCCESS)
         return pending.result;

      if (vkr_ohos_shadow_trace_enabled())
         vkr_log("OHOS shadow invalidate resolved pending guest writes "
                 "guestMemory=%" PRIu64 " copies=%u bytes=%" PRIu64,
                 (uint64_t)mem->base.id, pending.copies, pending.bytes);
   }

   vkr_ohos_gate_c_trace_memory("invalidate-entry", mem, offset, copy_size,
                                false, VK_SUCCESS);

   struct vn_device_proc_table *vk = &mem->device->proc_table;
   const VkMappedMemoryRange range = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = mem->base.handle.device_memory,
      .offset = offset,
      .size = size == VK_WHOLE_SIZE ? VK_WHOLE_SIZE : copy_size,
   };
   VkResult result = vk->InvalidateMappedMemoryRanges(
      mem->device->base.handle.device, 1, &range);
   if (result == VK_SUCCESS) {
      const uint32_t host_hash = vkr_ohos_shadow_trace_enabled()
         ? vkr_ohos_fnv1a32((const uint8_t *)mem->host_map + offset,
                            (size_t)copy_size)
         : 0;
      memcpy((uint8_t *)mem->shadow_map + offset,
             (const uint8_t *)mem->host_map + offset,
             (size_t)copy_size);
      if (mem->shadow_upload_snapshot)
         memcpy((uint8_t *)mem->shadow_upload_snapshot + offset,
                (const uint8_t *)mem->host_map + offset,
                (size_t)copy_size);
      mem->shadow_remote_active = true;
      if (vkr_ohos_shadow_trace_enabled()) {
         const uint32_t shadow_hash = vkr_ohos_fnv1a32(
            (const uint8_t *)mem->shadow_map + offset, (size_t)copy_size);
         vkr_log("OHOS shadow invalidate-output guestMemory=%" PRIu64
                 " hostMemory=0x%" PRIxPTR " offset=%" PRIu64
                 " size=%" PRIu64 " hostFnv=0x%08x shadowFnv=0x%08x"
                 " equal=%u",
                 (uint64_t)mem->base.id,
                 (uintptr_t)mem->base.handle.device_memory,
                 (uint64_t)offset, (uint64_t)copy_size,
                 host_hash, shadow_hash, host_hash == shadow_hash);
      }
   }

   const uint32_t count = mem->shadow_remote_invalidate_count++;
   if ((vkr_ohos_shadow_trace_enabled() &&
        (count < 8 || !(count % 60))) || result != VK_SUCCESS)
      vkr_log("OHOS shadow remote invalidate count=%u mem=%p offset=%" PRIu64
              " size=%" PRIu64 " result=%d",
              count + 1, mem, (uint64_t)offset,
              (uint64_t)copy_size, result);
   vkr_ohos_gate_c_trace_memory("invalidate-result", mem, offset, copy_size,
                                true, result);
   return result;
#else
   (void)mem;
   (void)offset;
   (void)size;
   return VK_SUCCESS;
#endif
}

#ifdef __OHOS__
static VkResult
vkr_device_memory_init_shadow_upload(struct vkr_queue *queue,
                                     struct vkr_shadow_upload_slot *slot)
{
   struct vkr_device *dev = queue->device;
   struct vn_device_proc_table *vk = &dev->proc_table;
   VkDevice device = dev->base.handle.device;

   if (queue->shadow_upload_inline && !queue->shadow_upload_timeline) {
      if (!vk->WaitSemaphores)
         return VK_ERROR_FEATURE_NOT_PRESENT;

      const VkSemaphoreTypeCreateInfo type_info = {
         .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
         .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
         .initialValue = 0,
      };
      const VkSemaphoreCreateInfo semaphore_info = {
         .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
         .pNext = &type_info,
      };
      VkResult result = vk->CreateSemaphore(
         device, &semaphore_info, NULL, &queue->shadow_upload_timeline);
      if (result != VK_SUCCESS)
         return result;
   }

   if (!slot->pool) {
      const VkCommandPoolCreateInfo pool_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
         .queueFamilyIndex = queue->family,
      };
      VkResult result = vk->CreateCommandPool(
         device, &pool_info, NULL, &slot->pool);
      if (result != VK_SUCCESS)
         return result;
   }

   if (!slot->command) {
      const VkCommandBufferAllocateInfo alloc_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = slot->pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      };
      VkResult result = vk->AllocateCommandBuffers(
         device, &alloc_info, &slot->command);
      if (result != VK_SUCCESS)
         return result;
   }

   if (!slot->fence) {
      const VkFenceCreateInfo fence_info = {
         .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      };
      VkResult result = vk->CreateFence(
         device, &fence_info, NULL, &slot->fence);
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}
#endif

VkResult
vkr_device_memory_prepare_shadow_upload(struct vkr_context *ctx,
                                        struct vkr_queue *queue,
                                        bool perf_timing,
                                        uint64_t submit_id)
{
#ifdef __OHOS__
   queue->shadow_upload_prepared = false;
   queue->shadow_upload_bytes = 0;
   queue->shadow_upload_updates = 0;
   queue->shadow_upload_ranges = 0;
   queue->shadow_upload_buffers = 0;
   queue->shadow_upload_uniform_buffers = 0;
   queue->shadow_upload_storage_buffers = 0;
   if (perf_timing) {
      queue->shadow_upload_wait_us = 0;
      queue->shadow_upload_reset_begin_us = 0;
      queue->shadow_upload_dirty_scan_us = 0;
      queue->shadow_upload_buffer_record_us = 0;
      queue->shadow_upload_uncovered_scan_us = 0;
      queue->shadow_upload_end_us = 0;
   }
   if (!vkr_device_memory_gpu_upload_enabled(queue->device))
      return VK_SUCCESS;

   struct vkr_device *dev = queue->device;
   const bool use_dirty_list = vkr_ohos_shadow_dirty_list_enabled();
   if (use_dirty_list) {
      bool has_dirty = false;
      mtx_lock(&ctx->object_mutex);
      list_for_each_entry (struct vkr_device_memory, mem,
                           &ctx->shadow_dirty_memories,
                           shadow_dirty_head) {
         if (mem->device == dev && mem->shadow_host_dirty) {
            has_dirty = true;
            break;
         }
      }
      mtx_unlock(&ctx->object_mutex);
      if (!has_dirty)
         return VK_SUCCESS;
   }

   struct vn_device_proc_table *vk = &dev->proc_table;
   VkDevice device = dev->base.handle.device;
   struct vkr_shadow_upload_slot *slot =
      &queue->shadow_upload_slots[queue->shadow_upload_slot];
   VkResult result = vkr_device_memory_init_shadow_upload(queue, slot);
   if (result != VK_SUCCESS) {
      vkr_log("OHOS shadow GPU upload init failed result=%d", result);
      return result;
   }

   uint64_t phase_start_ns = perf_timing ? vkr_ohos_now_ns() : 0;
   if (slot->in_flight) {
      if (queue->shadow_upload_inline && slot->retire_value) {
         const VkSemaphoreWaitInfo wait_info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &queue->shadow_upload_timeline,
            .pValues = &slot->retire_value,
         };
         result = vk->WaitSemaphores(device, &wait_info, 3000000000ull);
      } else {
         result = vk->WaitForFences(device, 1, &slot->fence,
                                    true, 3000000000ull);
      }
      const uint64_t wait_end_ns = perf_timing ? vkr_ohos_now_ns() : 0;
      if (perf_timing) {
         queue->shadow_upload_wait_us = wait_end_ns >= phase_start_ns
            ? (wait_end_ns - phase_start_ns) / 1000 : 0;
         phase_start_ns = wait_end_ns;
      }
      if (result != VK_SUCCESS) {
         vkr_log("OHOS shadow GPU upload wait failed result=%d", result);
         return result;
      }
      slot->in_flight = false;
      if (!queue->shadow_upload_inline || !slot->retire_value) {
         result = vk->ResetFences(device, 1, &slot->fence);
         if (result != VK_SUCCESS) {
            vkr_log("OHOS shadow GPU upload fence reset failed result=%d", result);
            return result;
         }
      }
      slot->retire_value = 0;
   }

   result = vk->ResetCommandPool(device, slot->pool, 0);
   if (result != VK_SUCCESS) {
      vkr_log("OHOS shadow GPU upload pool reset failed result=%d", result);
      return result;
   }

   const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   result = vk->BeginCommandBuffer(slot->command, &begin_info);
   if (result != VK_SUCCESS) {
      vkr_log("OHOS shadow GPU upload begin failed result=%d", result);
      return result;
   }
   uint64_t phase_end_ns = perf_timing ? vkr_ohos_now_ns() : 0;
   if (perf_timing) {
      queue->shadow_upload_reset_begin_us = phase_end_ns >= phase_start_ns
         ? (phase_end_ns - phase_start_ns) / 1000 : 0;
      phase_start_ns = phase_end_ns;
   }

   struct vkr_ohos_shadow_dirty_summary dirty = { 0 };
   uint32_t uncovered_allocation_count = 0;
   mtx_lock(&ctx->object_mutex);
   if (use_dirty_list) {
      list_for_each_entry (struct vkr_device_memory, mem,
                           &ctx->shadow_dirty_memories,
                           shadow_dirty_head) {
         vkr_ohos_prepare_shadow_dirty_memory(mem, dev, &dirty);
      }
   } else {
      hash_table_foreach (ctx->object_table, entry) {
         struct vkr_object *obj = entry->data;
         if (obj->type == VK_OBJECT_TYPE_DEVICE_MEMORY)
            vkr_ohos_prepare_shadow_dirty_memory(
               (struct vkr_device_memory *)obj, dev, &dirty);
      }
   }

   if (dirty.allocation_count) {
      const VkMemoryBarrier barrier = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT |
                          VK_ACCESS_MEMORY_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      };
      /* The upload command is inserted between Guest submissions on the same
       * queue.  Order prior GPU reads before overwriting a reused dynamic
       * buffer; submission order alone does not resolve the memory hazard. */
      vk->CmdPipelineBarrier(slot->command,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &barrier, 0, NULL, 0, NULL);
   }
   phase_end_ns = perf_timing ? vkr_ohos_now_ns() : 0;
   if (perf_timing) {
      queue->shadow_upload_dirty_scan_us = phase_end_ns >= phase_start_ns
         ? (phase_end_ns - phase_start_ns) / 1000 : 0;
      phase_start_ns = phase_end_ns;
   }

   struct vkr_ohos_shadow_upload_record record = {
      .vk = vk,
      .command = slot->command,
      .device = dev,
      .submit_id = submit_id,
      .merge_ranges = vkr_ohos_shadow_merge_ranges_enabled(),
   };
   const bool cover_upload = use_dirty_list &&
      vkr_ohos_shadow_cover_upload_enabled();
   if (cover_upload) {
      list_for_each_entry (struct vkr_device_memory, mem,
                           &ctx->shadow_dirty_memories,
                           shadow_dirty_head) {
         if (mem->device != dev || !mem->shadow_host_dirty)
            continue;

         if (mem->shadow_gpu_upload_full_coverage &&
             vkr_ohos_record_shadow_upload_memory_cover(&record, mem))
            continue;

         /* The coverage proof is deliberately a hard prerequisite.  An
          * unexpected gap falls through to the established exact per-buffer
          * path, after which the normal mapped-memory fallback handles any
          * still-uncovered bytes. */
         list_for_each_entry (struct vkr_buffer, buffer,
                              &mem->bound_buffers, memory_head) {
            vkr_ohos_record_shadow_upload_buffer(&record, buffer);
         }
      }
   } else if (use_dirty_list && vkr_ohos_shadow_bound_buffer_list_enabled()) {
      list_for_each_entry (struct vkr_device_memory, mem,
                           &ctx->shadow_dirty_memories,
                           shadow_dirty_head) {
         if (mem->device != dev || !mem->shadow_host_dirty)
            continue;
         list_for_each_entry (struct vkr_buffer, buffer,
                              &mem->bound_buffers, memory_head) {
            vkr_ohos_record_shadow_upload_buffer(&record, buffer);
         }
      }
   } else {
      hash_table_foreach (ctx->object_table, entry) {
         struct vkr_object *obj = entry->data;
         if (obj->type == VK_OBJECT_TYPE_BUFFER)
            vkr_ohos_record_shadow_upload_buffer(
               &record, (struct vkr_buffer *)obj);
      }
   }
   phase_end_ns = perf_timing ? vkr_ohos_now_ns() : 0;
   if (perf_timing) {
      queue->shadow_upload_buffer_record_us = phase_end_ns >= phase_start_ns
         ? (phase_end_ns - phase_start_ns) / 1000 : 0;
      phase_start_ns = phase_end_ns;
   }

   if (use_dirty_list) {
      list_for_each_entry (struct vkr_device_memory, mem,
                           &ctx->shadow_dirty_memories,
                           shadow_dirty_head) {
         if (mem->device == dev && mem->shadow_host_dirty &&
             mem->shadow_gpu_upload_full_coverage &&
             !mem->shadow_gpu_upload_covered)
            mem->shadow_gpu_upload_full_coverage = false;
         if (mem->device == dev && mem->shadow_host_dirty &&
             !mem->shadow_gpu_upload_covered)
            uncovered_allocation_count++;
      }
   } else {
      hash_table_foreach (ctx->object_table, entry) {
         struct vkr_object *obj = entry->data;
         if (obj->type != VK_OBJECT_TYPE_DEVICE_MEMORY)
            continue;
         struct vkr_device_memory *mem = (struct vkr_device_memory *)obj;
         if (mem->device == dev && mem->shadow_host_dirty &&
             mem->shadow_gpu_upload_full_coverage &&
             !mem->shadow_gpu_upload_covered)
            mem->shadow_gpu_upload_full_coverage = false;
         if (mem->device == dev && mem->shadow_host_dirty &&
             !mem->shadow_gpu_upload_covered)
            uncovered_allocation_count++;
      }
   }
   mtx_unlock(&ctx->object_mutex);
   phase_end_ns = perf_timing ? vkr_ohos_now_ns() : 0;
   if (perf_timing) {
      queue->shadow_upload_uncovered_scan_us = phase_end_ns >= phase_start_ns
         ? (phase_end_ns - phase_start_ns) / 1000 : 0;
      phase_start_ns = phase_end_ns;
   }

   if (record.update_count) {
      const VkMemoryBarrier barrier = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT |
                          VK_ACCESS_MEMORY_WRITE_BIT,
      };
      vk->CmdPipelineBarrier(slot->command,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 1, &barrier, 0, NULL, 0, NULL);
   }

   result = vk->EndCommandBuffer(slot->command);
   phase_end_ns = perf_timing ? vkr_ohos_now_ns() : 0;
   if (perf_timing)
      queue->shadow_upload_end_us = phase_end_ns >= phase_start_ns
         ? (phase_end_ns - phase_start_ns) / 1000 : 0;
   if (result != VK_SUCCESS) {
      vkr_log("OHOS shadow GPU upload end failed result=%d", result);
      return result;
   }

   queue->shadow_upload_bytes = record.upload_bytes;
   queue->shadow_upload_updates = record.update_count;
   queue->shadow_upload_ranges = record.upload_range_count;
   queue->shadow_upload_buffers = record.buffer_count;
   queue->shadow_upload_uniform_buffers = record.uniform_buffer_count;
   queue->shadow_upload_storage_buffers = record.storage_buffer_count;
   queue->shadow_upload_prepared = record.update_count > 0;
   if (vkr_ohos_shadow_trace_enabled() && dirty.allocation_count)
      vkr_log("OHOS shadow GPU upload prepared ranges=%u updates=%u bytes=%" PRIu64
              " dirty_allocations=%u dirty_ranges=%u dirty_bytes=%" PRIu64
              " uncovered_allocations=%u overflow_allocations=%u",
              record.upload_range_count, record.update_count,
              record.upload_bytes, dirty.allocation_count, dirty.range_count,
              dirty.bytes, uncovered_allocation_count,
              dirty.range_overflow_count);
   return VK_SUCCESS;
#else
   (void)ctx;
   (void)queue;
   (void)perf_timing;
   (void)submit_id;
   return VK_SUCCESS;
#endif
}

VkResult
vkr_device_memory_submit_shadow_upload(struct vkr_queue *queue)
{
#ifdef __OHOS__
   if (!queue->shadow_upload_prepared)
      return VK_SUCCESS;

   struct vn_device_proc_table *vk = &queue->device->proc_table;
   const uint32_t slot_index = queue->shadow_upload_slot;
   struct vkr_shadow_upload_slot *slot =
      &queue->shadow_upload_slots[slot_index];
   const VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &slot->command,
   };
   VkResult result = vk->QueueSubmit(queue->base.handle.queue, 1,
                                     &submit_info,
                                     slot->fence);
   queue->shadow_upload_prepared = false;
   if (result == VK_SUCCESS) {
      slot->in_flight = true;
      queue->shadow_upload_slot =
         (slot_index + 1) % VKR_WINEHUA_SHADOW_UPLOAD_SLOT_COUNT;
      if (vkr_ohos_shadow_upload_wait_enabled()) {
         const uint64_t wait_start_ns = vkr_ohos_now_ns();
         result = vk->WaitForFences(queue->device->base.handle.device, 1,
                                    &slot->fence, true,
                                    3000000000ull);
         vkr_log("OHOS shadow GPU upload diagnostic wait result=%d elapsed_us=%"
                 PRIu64 " updates=%u bytes=%" PRIu64,
                 result, (vkr_ohos_now_ns() - wait_start_ns) / 1000,
                 queue->shadow_upload_updates, queue->shadow_upload_bytes);
      }
   }
   if (vkr_ohos_shadow_trace_enabled() || result != VK_SUCCESS)
      vkr_log("OHOS shadow GPU upload submit result=%d updates=%u bytes=%" PRIu64,
              result, queue->shadow_upload_updates,
              queue->shadow_upload_bytes);
   return result;
#else
   (void)queue;
   return VK_SUCCESS;
#endif
}

void
vkr_device_memory_disable_shadow_upload_coverage(struct vkr_context *ctx)
{
#ifdef __OHOS__
   if (!ctx)
      return;
   mtx_lock(&ctx->object_mutex);
   hash_table_foreach (ctx->object_table, entry) {
      struct vkr_object *obj = entry->data;
      if (obj->type != VK_OBJECT_TYPE_DEVICE_MEMORY)
         continue;
      struct vkr_device_memory *mem = (struct vkr_device_memory *)obj;
      if (mem->shadow_host_dirty)
         mem->shadow_gpu_upload_full_coverage = false;
   }
   mtx_unlock(&ctx->object_mutex);
#else
   (void)ctx;
#endif
}

bool
vkr_device_memory_requires_deferred_host_wait(struct vkr_context *ctx,
                                              struct vkr_device *dev)
{
#ifdef __OHOS__
   if (!ctx || !dev)
      return false;

   bool required = false;
   mtx_lock(&ctx->object_mutex);
   if (vkr_ohos_shadow_dirty_list_enabled()) {
      list_for_each_entry (struct vkr_device_memory, mem,
                           &ctx->shadow_dirty_memories,
                           shadow_dirty_head) {
         if (mem->device == dev && mem->shadow_host_dirty &&
             mem->shadow_host_copy_deferred &&
             !mem->shadow_gpu_upload_full_coverage) {
            required = true;
            break;
         }
      }
   } else {
      hash_table_foreach (ctx->object_table, entry) {
         struct vkr_object *obj = entry->data;
         if (obj->type != VK_OBJECT_TYPE_DEVICE_MEMORY)
            continue;
         struct vkr_device_memory *mem =
            (struct vkr_device_memory *)obj;
         if (mem->device == dev && mem->shadow_host_dirty &&
             mem->shadow_host_copy_deferred &&
             !mem->shadow_gpu_upload_full_coverage) {
            required = true;
            break;
         }
      }
   }
   mtx_unlock(&ctx->object_mutex);
   return required;
#else
   (void)ctx;
   (void)dev;
   return false;
#endif
}

static struct vkr_shadow_sync_stats
vkr_device_memory_sync_shadows(struct vkr_context *ctx, bool to_host)
{
   struct vkr_shadow_sync_stats total = { 0 };
#ifdef __OHOS__
   const uint64_t start_ns = vkr_ohos_now_ns();
   mtx_lock(&ctx->object_mutex);
   const char *to_host_mode = os_get_option("VKR_WINEHUA_SHADOW_TO_HOST");
   const bool explicit_to_host =
      to_host_mode && (!strcmp(to_host_mode, "explicit") ||
                       !strcmp(to_host_mode, "explicit-only"));
   const bool use_dirty_list = to_host && !explicit_to_host &&
      vkr_ohos_shadow_dirty_list_enabled();
   struct vkr_ohos_shadow_flush_batch flush_batch = { 0 };
   bool host_flush_prepared = false;

   const char *inline_upload =
      os_get_option("VKR_WINEHUA_GPU_UPLOAD_INLINE");
   const bool defer_host_copy = inline_upload &&
      inline_upload[0] == '1' && !inline_upload[1];
   if (to_host && vkr_ohos_shadow_batch_flush_enabled() &&
       !defer_host_copy) {
      if (use_dirty_list) {
         list_for_each_entry (struct vkr_device_memory, mem,
                              &ctx->shadow_dirty_memories,
                              shadow_dirty_head) {
            if (!vkr_ohos_shadow_flush_batch_add(&flush_batch, mem))
               break;
         }
      } else {
         hash_table_foreach (ctx->object_table, entry) {
            struct vkr_object *obj = entry->data;
            if (obj->type != VK_OBJECT_TYPE_DEVICE_MEMORY)
               continue;
            if (!vkr_ohos_shadow_flush_batch_add(
                   &flush_batch, (struct vkr_device_memory *)obj))
               break;
         }
      }

      if (!flush_batch.overflow && flush_batch.count) {
         const VkResult batch_result =
            vkr_ohos_shadow_flush_batch_submit(&flush_batch);
         if (batch_result == VK_SUCCESS) {
            host_flush_prepared = true;
            total.cache_ops++;
         } else {
            vkr_log("OHOS shadow batch flush failed result=%d ranges=%u; "
                    "falling back to per-allocation flush",
                    batch_result, flush_batch.count);
         }
      }
   }

   if (use_dirty_list) {
      list_for_each_entry_safe (struct vkr_device_memory, mem,
                                &ctx->shadow_dirty_memories,
                                shadow_dirty_head) {
         const struct vkr_shadow_sync_stats stats =
            vkr_device_memory_sync_shadow(mem, to_host,
                                          host_flush_prepared);
         total.scanned++;
         total.bytes += stats.bytes;
         total.copies += stats.copies;
         total.cache_ops += stats.cache_ops;
         total.gpu_upload_skipped_bytes += stats.gpu_upload_skipped_bytes;
         total.gpu_upload_skipped_copies += stats.gpu_upload_skipped_copies;
      }
   } else {
      hash_table_foreach (ctx->object_table, entry) {
         struct vkr_object *obj = entry->data;
         if (obj->type == VK_OBJECT_TYPE_DEVICE_MEMORY) {
            const struct vkr_shadow_sync_stats stats =
               vkr_device_memory_sync_shadow((struct vkr_device_memory *)obj,
                                             to_host, host_flush_prepared);
            total.scanned++;
            total.bytes += stats.bytes;
            total.copies += stats.copies;
            total.cache_ops += stats.cache_ops;
            total.gpu_upload_skipped_bytes += stats.gpu_upload_skipped_bytes;
            total.gpu_upload_skipped_copies += stats.gpu_upload_skipped_copies;
         }
      }
   }
   mtx_unlock(&ctx->object_mutex);

   atomic_uint_fast64_t *counter = to_host
      ? &vkr_ohos_shadow_to_host_sync_count
      : &vkr_ohos_shadow_from_host_sync_count;
   const uint64_t call_id =
      atomic_fetch_add_explicit(counter, 1, memory_order_relaxed) + 1;
   const uint64_t end_ns = vkr_ohos_now_ns();
   const uint64_t elapsed_us = start_ns && end_ns >= start_ns
      ? (end_ns - start_ns) / 1000 : 0;
   total.elapsed_us = elapsed_us;
   if (vkr_ohos_shadow_trace_enabled() &&
       (call_id <= 8 || !(call_id % 120) || elapsed_us >= 20000)) {
      vkr_log("OHOS shadow sync direction=%s call=%" PRIu64
              " scanned=%u copies=%u bytes=%" PRIu64
              " cache_ops=%u elapsed_us=%" PRIu64,
              to_host ? "to-host" : "from-host", call_id, total.scanned,
              total.copies, total.bytes, total.cache_ops, elapsed_us);
   }
#else
   (void)ctx;
   (void)to_host;
#endif
   return total;
}

struct vkr_shadow_sync_stats
vkr_device_memory_sync_shadows_to_host(struct vkr_context *ctx)
{
   return vkr_device_memory_sync_shadows(ctx, true);
}

void
vkr_device_memory_sync_shadows_from_host(struct vkr_context *ctx)
{
#ifdef __OHOS__
   const char *mode = os_get_option("VKR_WINEHUA_SHADOW_FROM_HOST");
   if (mode && (!strcmp(mode, "none") || !strcmp(mode, "disabled") ||
                !strcmp(mode, "precise"))) {
      const uint64_t call_id =
         atomic_fetch_add_explicit(&vkr_ohos_shadow_from_host_sync_count, 1,
                                   memory_order_relaxed) + 1;
      if (vkr_ohos_shadow_trace_enabled() &&
          (call_id <= 8 || !(call_id % 120)))
         vkr_log("OHOS shadow sync direction=from-host call=%" PRIu64
                 " skipped=1 mode=%s",
                 call_id, mode);
      return;
   }
#endif
   vkr_device_memory_sync_shadows(ctx, false);
}
