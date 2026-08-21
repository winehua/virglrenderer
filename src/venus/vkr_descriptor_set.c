/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_descriptor_set.h"

#include "vkr_buffer.h"
#include "vkr_descriptor_set_gen.h"
#include "vkr_device_memory.h"
#include "vkr_image.h"
#include "vkr_queue.h"

#include <stdatomic.h>

#define VKR_WINEHUA_SAMPLE_TRACE_LIMIT 32768u
#define VKR_WINEHUA_CAPTURE_TRACE_LIMIT 512u

static bool
vkr_winehua_sample_trace_enabled(void)
{
   static int enabled = -1;
   if (enabled < 0) {
      const char *value = os_get_option("WINEHUA_VKR_TRACE_SAMPLED");
      enabled = value && value[0] == '1';
   }
   return enabled != 0;
}

static bool
vkr_winehua_sample_trace_allow(void)
{
   static atomic_uint emitted = ATOMIC_VAR_INIT(0);
   const unsigned index =
      atomic_fetch_add_explicit(&emitted, 1, memory_order_relaxed);

   if (index < VKR_WINEHUA_SAMPLE_TRACE_LIMIT)
      return true;
   if (index == VKR_WINEHUA_SAMPLE_TRACE_LIMIT)
      vkr_log("WineHuaSampled: host descriptor trace limit reached; "
              "further records suppressed");
   return false;
}

/* Detailed capture is deliberately separate from the descriptor trace.  It
 * is enabled only by WineHua's shadow-trace diagnostic profile and bounded so
 * a real game cannot turn the host log into an unbounded performance hazard. */
static bool
vkr_winehua_capture_trace_enabled(void)
{
   static int enabled = -1;
   if (enabled < 0) {
      const char *value = os_get_option("WINEHUA_VKR_TRACE_CAPTURE");
      enabled = value && value[0] == '1';
   }
   return enabled != 0;
}

static bool
vkr_winehua_capture_trace_allow(void)
{
   static atomic_uint emitted = ATOMIC_VAR_INIT(0);
   const unsigned index =
      atomic_fetch_add_explicit(&emitted, 1, memory_order_relaxed);

   if (index < VKR_WINEHUA_CAPTURE_TRACE_LIMIT)
      return true;
   if (index == VKR_WINEHUA_CAPTURE_TRACE_LIMIT)
      vkr_log("WineHuaCapture: host capture limit reached; further records suppressed");
   return false;
}

#ifdef __OHOS__
#define VKR_WINEHUA_UBO_DESCRIPTOR_TRACE_LIMIT 200000u

static bool
vkr_winehua_ubo_identity_trace_enabled(void)
{
   const char *value = os_get_option("WINEHUA_VKR_TRACE_UBO_IDENTITY");
   return value && value[0] && !(value[0] == '0' && !value[1]);
}

static bool
vkr_winehua_ubo_identity_trace_verbose(void)
{
   const char *value = os_get_option("WINEHUA_VKR_TRACE_UBO_IDENTITY");
   return value && value[0] == '1' && !value[1];
}

static bool
vkr_winehua_ubo_identity_trace_allow(void)
{
   static atomic_uint emitted = ATOMIC_VAR_INIT(0);
   const unsigned index =
      atomic_fetch_add_explicit(&emitted, 1, memory_order_relaxed);
   if (index < VKR_WINEHUA_UBO_DESCRIPTOR_TRACE_LIMIT)
      return true;
   if (index == VKR_WINEHUA_UBO_DESCRIPTOR_TRACE_LIMIT)
      vkr_log("WineHuaUboHost: phase=descriptor trace limit reached");
   return false;
}

static bool
vkr_winehua_descriptor_update_serialize_enabled(void)
{
   static atomic_int enabled = ATOMIC_VAR_INIT(-1);
   int value = atomic_load_explicit(&enabled, memory_order_relaxed);
   if (value < 0) {
      const char *option = os_get_option("VKR_WINEHUA_DESCRIPTOR_UPDATE_SERIALIZE");
      value = option && option[0] == '1' && !option[1];
      atomic_store_explicit(&enabled, value, memory_order_relaxed);
   }
   return value != 0;
}

static void
vkr_winehua_wait_descriptor_update_queues(struct vkr_device *dev)
{
   if (!dev || !vkr_winehua_descriptor_update_serialize_enabled())
      return;

   const uint64_t submit_generation =
      vkr_winehua_queue_submit_generation();
   if (!submit_generation ||
       atomic_load_explicit(
          &dev->winehua_descriptor_wait_submit_generation,
          memory_order_acquire) == submit_generation)
      return;

   static atomic_uint_fast64_t wait_count = ATOMIC_VAR_INIT(0);
   uint32_t queue_count = 0;
   VkResult first_error = VK_SUCCESS;
   list_for_each_entry (struct vkr_queue, queue, &dev->queues, base.track_head) {
      mtx_lock(&queue->vk_mutex);
      const VkResult result = dev->proc_table.QueueWaitIdle(queue->base.handle.queue);
      mtx_unlock(&queue->vk_mutex);
      queue_count++;
      if (first_error == VK_SUCCESS && result != VK_SUCCESS)
         first_error = result;
   }

   const uint64_t count = atomic_fetch_add_explicit(
      &wait_count, 1, memory_order_relaxed) + 1;
   atomic_store_explicit(&dev->winehua_descriptor_wait_submit_generation,
                         submit_generation, memory_order_release);
   if (count <= 8 || !(count % 120) || first_error != VK_SUCCESS)
      vkr_log("WineHua descriptor update queue wait count=%" PRIu64
              " submit_generation=%" PRIu64 " queues=%u result=%d",
              count, submit_generation, queue_count, first_error);
}

#endif

static bool
vkr_winehua_image_descriptor(VkDescriptorType type)
{
   switch (type) {
   case VK_DESCRIPTOR_TYPE_SAMPLER:
   case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
   case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
   case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
   case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
      return true;
   default:
      return false;
   }
}

static bool
vkr_winehua_buffer_descriptor(VkDescriptorType type)
{
   switch (type) {
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
      return true;
   default:
      return false;
   }
}

#ifdef __OHOS__
#define VKR_WINEHUA_BUFFER_HASH_LIMIT 4096u
#define VKR_WINEHUA_BUFFER_PREVIEW_BYTES 64u
#define VKR_WINEHUA_FNV_OFFSET UINT64_C(1469598103934665603)
#define VKR_WINEHUA_FNV_PRIME UINT64_C(1099511628211)

struct vkr_winehua_buffer_hashes {
   VkDeviceSize absolute_offset;
   size_t byte_count;
   uint64_t shadow_hash;
   uint64_t host_hash;
   int offset_valid;
   int equal;
};

static uint64_t
vkr_winehua_fnv1a64(const void *data, size_t size)
{
   const uint8_t *bytes = data;
   uint64_t hash = VKR_WINEHUA_FNV_OFFSET;

   for (size_t i = 0; i < size; i++) {
      hash ^= bytes[i];
      hash *= VKR_WINEHUA_FNV_PRIME;
   }
   return hash;
}

static struct vkr_winehua_buffer_hashes
vkr_winehua_hash_buffer_descriptor(const struct vkr_buffer *buffer,
                                    const VkDescriptorBufferInfo *info)
{
   struct vkr_winehua_buffer_hashes hashes = {
      .equal = -1,
   };
   const struct vkr_device_memory *mem = buffer ? buffer->bound_memory : NULL;
   if (!mem || info->offset > UINT64_MAX - buffer->bound_memory_offset)
      return hashes;

   hashes.offset_valid = 1;
   hashes.absolute_offset = buffer->bound_memory_offset + info->offset;
   if (!mem->shadow_map || !mem->host_map ||
       hashes.absolute_offset >= mem->allocation_size ||
       hashes.absolute_offset >= mem->shadow_size)
      return hashes;

   VkDeviceSize available = MIN2(mem->allocation_size - hashes.absolute_offset,
                                 mem->shadow_size - hashes.absolute_offset);
   if (info->range != VK_WHOLE_SIZE)
      available = MIN2(available, info->range);
   available = MIN2(available, (VkDeviceSize)VKR_WINEHUA_BUFFER_HASH_LIMIT);
   if (!available)
      return hashes;

   hashes.byte_count = (size_t)available;
   const uint8_t *shadow = (const uint8_t *)mem->shadow_map + hashes.absolute_offset;
   const uint8_t *host = (const uint8_t *)mem->host_map + hashes.absolute_offset;
   hashes.shadow_hash = vkr_winehua_fnv1a64(shadow, hashes.byte_count);
   hashes.host_hash = vkr_winehua_fnv1a64(host, hashes.byte_count);
   hashes.equal = memcmp(shadow, host, hashes.byte_count) == 0;
   return hashes;
}

static void
vkr_winehua_track_ubo_mappings(struct vkr_context *ctx,
                               uint32_t write_count,
                               const VkWriteDescriptorSet *writes)
{
   if (!vkr_winehua_ubo_identity_trace_enabled())
      return;

   mtx_lock(&ctx->object_mutex);
   for (uint32_t i = 0; i < write_count; i++) {
      const VkWriteDescriptorSet *write = &writes[i];
      if ((write->dstBinding != 0 && write->dstBinding != 3 &&
           write->dstBinding != 4) ||
          !write->pBufferInfo ||
          (write->descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
           write->descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC))
         continue;

      struct vkr_descriptor_set *set =
         vkr_descriptor_set_from_handle(write->dstSet);
      for (uint32_t j = 0; j < write->descriptorCount; j++) {
         const VkDescriptorBufferInfo *info = &write->pBufferInfo[j];
         struct vkr_buffer *buffer = info->buffer
            ? vkr_buffer_from_handle(info->buffer) : NULL;
         struct vkr_device_memory *mem = buffer ? buffer->bound_memory : NULL;
         if (!set || !buffer || !mem ||
             (info->range != 48 && info->range != 64 &&
              info->range != 1536))
            continue;

         const uint32_t binding_index = write->dstBinding == 4 ? 1 : 0;
         const uint32_t array_element = write->dstArrayElement + j;
         struct vkr_winehua_ubo_binding *state =
            &set->winehua_ubo_bindings[binding_index];
         const bool mapping_changed =
            !state->valid || state->buffer_id != buffer->base.id ||
            state->offset != info->offset || state->size != info->range ||
            state->array_element != array_element ||
            state->descriptor_type != write->descriptorType;
         if (mapping_changed) {
            if (state->valid && state->last_bound_mapping_sequence ==
                state->mapping_sequence) {
               vkr_log("WineHuaUboHost: phase=descriptor-remap-after-bind"
                       " submitGeneration=%" PRIu64
                       " setId=%" PRIu64 " binding=%u"
                       " oldMappingSequence=%" PRIu64
                       " newBufferId=%" PRIu64
                       " newDescriptorOffset=%" PRIu64
                       " newDescriptorRange=%" PRIu64,
                       vkr_winehua_queue_submit_generation(),
                       (uint64_t)set->base.id, write->dstBinding,
                       state->mapping_sequence, (uint64_t)buffer->base.id,
                       (uint64_t)info->offset, (uint64_t)info->range);
            }
            state->mapping_sequence++;
         }

         state->buffer = buffer;
         state->buffer_id = buffer->base.id;
         state->offset = info->offset;
         state->size = info->range;
         state->binding = write->dstBinding;
         state->array_element = array_element;
         state->descriptor_type = write->descriptorType;
         state->valid = true;
      }
   }
   mtx_unlock(&ctx->object_mutex);
}

static void
vkr_winehua_log_buffer_preview(const struct vkr_descriptor_set *set,
                                const VkWriteDescriptorSet *write,
                                const struct vkr_buffer *buffer,
                                const struct vkr_winehua_buffer_hashes *hashes)
{
   if (!vkr_winehua_capture_trace_enabled() ||
       !vkr_winehua_capture_trace_allow() || !buffer || !buffer->bound_memory ||
       !hashes->byte_count || !hashes->offset_valid)
      return;

   const struct vkr_device_memory *mem = buffer->bound_memory;
   const size_t byte_count = MIN2(hashes->byte_count,
                                  (size_t)VKR_WINEHUA_BUFFER_PREVIEW_BYTES);
   char shadow_hex[VKR_WINEHUA_BUFFER_PREVIEW_BYTES * 2 + 1];
   char host_hex[VKR_WINEHUA_BUFFER_PREVIEW_BYTES * 2 + 1];
   const uint8_t *shadow = (const uint8_t *)mem->shadow_map + hashes->absolute_offset;
   const uint8_t *host = (const uint8_t *)mem->host_map + hashes->absolute_offset;

   for (size_t i = 0; i < byte_count; i++) {
      snprintf(&shadow_hex[i * 2], 3, "%02x", shadow[i]);
      snprintf(&host_hex[i * 2], 3, "%02x", host[i]);
   }
   shadow_hex[byte_count * 2] = '\0';
   host_hex[byte_count * 2] = '\0';

   vkr_log("WineHuaCapture: descriptor-buffer setId=%" PRIu64
           " binding=%u bufferId=%" PRIu64 " memoryId=%" PRIu64
           " absoluteOffset=%" PRIu64 " previewBytes=%zu shadow=%s host=%s",
           set ? set->base.id : 0, write->dstBinding, buffer->base.id,
           mem->base.id, (uint64_t)hashes->absolute_offset, byte_count,
           shadow_hex, host_hex);
}
#endif

static void
vkr_winehua_log_guest_descriptor_objects(uint32_t write_count,
                                         const VkWriteDescriptorSet *writes)
{
   if (!vkr_winehua_sample_trace_enabled())
      return;

   for (uint32_t i = 0; i < write_count; i++) {
      const VkWriteDescriptorSet *write = &writes[i];
      struct vkr_descriptor_set *set;
      const bool image_descriptor =
         vkr_winehua_image_descriptor(write->descriptorType) && write->pImageInfo;
      const bool buffer_descriptor =
         vkr_winehua_buffer_descriptor(write->descriptorType) && write->pBufferInfo;
      if (!image_descriptor && !buffer_descriptor)
         continue;

      set = vkr_descriptor_set_from_handle(write->dstSet);
      if (image_descriptor) {
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            if (!vkr_winehua_sample_trace_allow())
               continue;
            const VkDescriptorImageInfo *info = &write->pImageInfo[j];
            struct vkr_image_view *view = info->imageView
               ? vkr_image_view_from_handle(info->imageView) : NULL;
            struct vkr_sampler *sampler = info->sampler
               ? vkr_sampler_from_handle(info->sampler) : NULL;
            struct vkr_image *image = view ? view->image : NULL;

            vkr_log("WineHuaSampled: host-descriptor phase=guest-object "
                    "setId=%" PRIu64 " hostSet=0x%" PRIxPTR " binding=%u "
                    "arrayElement=%u type=%u viewId=%" PRIu64 " "
                    "hostView=0x%" PRIxPTR " imageId=%" PRIu64 " "
                    "hostImage=0x%" PRIxPTR " samplerId=%" PRIu64 " "
                    "hostSampler=0x%" PRIxPTR " layout=%u",
                    set ? set->base.id : 0,
                    set ? (uintptr_t)set->base.handle.descriptor_set : 0,
                    write->dstBinding, write->dstArrayElement + j,
                    write->descriptorType, view ? view->base.id : 0,
                    view ? (uintptr_t)view->base.handle.image_view : 0,
                    image ? image->base.id : 0,
                    image ? (uintptr_t)image->base.handle.image : 0,
                    sampler ? sampler->base.id : 0,
                    sampler ? (uintptr_t)sampler->base.handle.sampler : 0,
                    info->imageLayout);
         }
      }
#ifdef __OHOS__
      if (buffer_descriptor) {
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            const VkDescriptorBufferInfo *info = &write->pBufferInfo[j];
            struct vkr_buffer *buffer = info->buffer
               ? vkr_buffer_from_handle(info->buffer) : NULL;
            struct vkr_device_memory *mem = buffer ? buffer->bound_memory : NULL;
            const struct vkr_winehua_buffer_hashes hashes =
               vkr_winehua_hash_buffer_descriptor(buffer, info);

            if ((write->dstBinding == 3 || write->dstBinding == 4) &&
                vkr_winehua_ubo_identity_trace_verbose() &&
                vkr_winehua_ubo_identity_trace_allow()) {
               vkr_log("WineHuaUboHost: phase=descriptor submitGeneration=%" PRIu64
                       " setId=%" PRIu64 " hostSet=0x%" PRIxPTR
                       " binding=%u bufferId=%" PRIu64
                       " hostBuffer=0x%" PRIxPTR " memoryId=%" PRIu64
                       " hostMemory=0x%" PRIxPTR
                       " descriptorOffset=%" PRIu64
                       " descriptorRange=%" PRIu64
                       " bufferMemoryOffset=%" PRIu64
                       " absoluteOffset=%" PRIu64 " hashBytes=%zu"
                       " shadowHash=%016" PRIx64 " hostHash=%016" PRIx64
                       " hashEqual=%d",
                       vkr_winehua_queue_submit_generation(),
                       set ? set->base.id : 0,
                       set ? (uintptr_t)set->base.handle.descriptor_set : 0,
                       write->dstBinding, buffer ? buffer->base.id : 0,
                       buffer ? (uintptr_t)buffer->base.handle.buffer : 0,
                       mem ? mem->base.id : 0,
                       mem ? (uintptr_t)mem->base.handle.device_memory : 0,
                       (uint64_t)info->offset, (uint64_t)info->range,
                       buffer ? (uint64_t)buffer->bound_memory_offset : 0,
                       (uint64_t)hashes.absolute_offset, hashes.byte_count,
                       hashes.shadow_hash, hashes.host_hash, hashes.equal);
            }

            if (!vkr_winehua_sample_trace_allow())
               continue;

            vkr_winehua_log_buffer_preview(set, write, buffer, &hashes);

            vkr_log("WineHuaSampled: host-descriptor phase=guest-buffer "
                    "setId=%" PRIu64 " hostSet=0x%" PRIxPTR " binding=%u "
                    "arrayElement=%u type=%u bufferId=%" PRIu64 " "
                    "hostBuffer=0x%" PRIxPTR " memoryId=%" PRIu64 " "
                    "hostMemory=0x%" PRIxPTR " descriptorOffset=%" PRIu64 " "
                    "descriptorRange=%" PRIu64 " memoryOffset=%" PRIu64 " "
                    "absoluteOffset=%" PRIu64 " offsetValid=%d hashBytes=%zu "
                    "shadowHash=%016" PRIx64 " hostHash=%016" PRIx64 " "
                    "hashEqual=%d",
                    set ? set->base.id : 0,
                    set ? (uintptr_t)set->base.handle.descriptor_set : 0,
                    write->dstBinding, write->dstArrayElement + j,
                    write->descriptorType, buffer ? buffer->base.id : 0,
                    buffer ? (uintptr_t)buffer->base.handle.buffer : 0,
                    mem ? mem->base.id : 0,
                    mem ? (uintptr_t)mem->base.handle.device_memory : 0,
                    (uint64_t)info->offset, (uint64_t)info->range,
                    buffer ? (uint64_t)buffer->bound_memory_offset : 0,
                    (uint64_t)hashes.absolute_offset, hashes.offset_valid,
                    hashes.byte_count, hashes.shadow_hash, hashes.host_hash,
                    hashes.equal);
         }
      }
#endif
   }
}

static void
vkr_winehua_log_host_descriptor_handles(uint32_t write_count,
                                        const VkWriteDescriptorSet *writes)
{
   if (!vkr_winehua_sample_trace_enabled())
      return;

   for (uint32_t i = 0; i < write_count; i++) {
      const VkWriteDescriptorSet *write = &writes[i];
      const bool image_descriptor =
         vkr_winehua_image_descriptor(write->descriptorType) && write->pImageInfo;
      const bool buffer_descriptor =
         vkr_winehua_buffer_descriptor(write->descriptorType) && write->pBufferInfo;
      if (!image_descriptor && !buffer_descriptor)
         continue;

      if (image_descriptor) {
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            if (!vkr_winehua_sample_trace_allow())
               continue;
            const VkDescriptorImageInfo *info = &write->pImageInfo[j];
            vkr_log("WineHuaSampled: host-descriptor phase=driver-call "
                    "hostSet=0x%" PRIxPTR " binding=%u arrayElement=%u "
                    "type=%u hostView=0x%" PRIxPTR " "
                    "hostSampler=0x%" PRIxPTR " layout=%u",
                    (uintptr_t)write->dstSet, write->dstBinding,
                    write->dstArrayElement + j, write->descriptorType,
                    (uintptr_t)info->imageView, (uintptr_t)info->sampler,
                    info->imageLayout);
         }
      }
      if (buffer_descriptor) {
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            if (!vkr_winehua_sample_trace_allow())
               continue;
            const VkDescriptorBufferInfo *info = &write->pBufferInfo[j];
            vkr_log("WineHuaSampled: host-descriptor phase=driver-buffer "
                    "hostSet=0x%" PRIxPTR " binding=%u arrayElement=%u "
                    "type=%u hostBuffer=0x%" PRIxPTR " offset=%" PRIu64 " "
                    "range=%" PRIu64,
                    (uintptr_t)write->dstSet, write->dstBinding,
                    write->dstArrayElement + j, write->descriptorType,
                    (uintptr_t)info->buffer, (uint64_t)info->offset,
                    (uint64_t)info->range);
         }
      }
   }
}

static void
vkr_dispatch_vkGetDescriptorSetLayoutSupport(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDescriptorSetLayoutSupport *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDescriptorSetLayoutSupport_args_handle(args);
   vk->GetDescriptorSetLayoutSupport(args->device, args->pCreateInfo, args->pSupport);
}

static void
vkr_dispatch_vkCreateDescriptorSetLayout(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkCreateDescriptorSetLayout *args)
{
#ifdef __OHOS__
   const char *gate_c_trace = os_get_option("WINEHUA_VKD3D_GATE_C_TRACE");
   const bool trace = gate_c_trace && gate_c_trace[0] == '1' && !gate_c_trace[1];
   const VkDescriptorSetLayout requested =
      args->pSetLayout ? *args->pSetLayout : VK_NULL_HANDLE;
   const VkDescriptorSetLayoutCreateInfo *info = args->pCreateInfo;

   if (trace)
      vkr_log("WineHuaDescriptorSetLayout: create requested object=%" PRIu64
              " flags=0x%x bindings=%u",
              (uint64_t)(uintptr_t)requested, info ? info->flags : 0,
              info ? info->bindingCount : 0);
#endif
   vkr_descriptor_set_layout_create_and_add(dispatch->data, args);
#ifdef __OHOS__
   if (trace)
      vkr_log("WineHuaDescriptorSetLayout: create result=%d object=%" PRIu64,
              args->ret, (uint64_t)(uintptr_t)requested);
#endif
}

static void
vkr_dispatch_vkDestroyDescriptorSetLayout(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkDestroyDescriptorSetLayout *args)
{
   vkr_descriptor_set_layout_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkCreateDescriptorPool(struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCreateDescriptorPool *args)
{
   struct vkr_descriptor_pool *pool =
      vkr_descriptor_pool_create_and_add(dispatch->data, args);
   if (!pool)
      return;

   pool->flags = args->pCreateInfo->flags;

   list_inithead(&pool->descriptor_sets);
}

static void
vkr_dispatch_vkDestroyDescriptorPool(struct vn_dispatch_context *dispatch,
                                     struct vn_command_vkDestroyDescriptorPool *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_descriptor_pool *pool =
      vkr_descriptor_pool_from_handle(args->descriptorPool);

   if (!pool)
      return;

   vkr_descriptor_pool_release(ctx, pool);
   vkr_descriptor_pool_destroy_and_remove(ctx, args);
}

static void
vkr_dispatch_vkResetDescriptorPool(struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkResetDescriptorPool *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   struct vkr_context *ctx = dispatch->data;

   struct vkr_descriptor_pool *pool =
      vkr_descriptor_pool_from_handle(args->descriptorPool);
   if (!pool) {
      vkr_context_set_fatal(ctx);
      return;
   }

   vn_replace_vkResetDescriptorPool_args_handle(args);
   args->ret = vk->ResetDescriptorPool(args->device, args->descriptorPool, args->flags);

   vkr_descriptor_pool_release(ctx, pool);
   list_inithead(&pool->descriptor_sets);
}

static void
vkr_dispatch_vkAllocateDescriptorSets(struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkAllocateDescriptorSets *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vkr_descriptor_pool *pool =
      vkr_descriptor_pool_from_handle(args->pAllocateInfo->descriptorPool);
   struct object_array arr;
   VkResult result;

   if (!pool) {
      vkr_context_set_fatal(ctx);
      return;
   }

   result = vkr_descriptor_set_create_array(ctx, args, &arr);
   if (result != VK_SUCCESS) {
      if (!(pool->flags & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT))
         vkr_log("Warning: vkAllocateDescriptorSets failed(%u)", result);
      return;
   }

   vkr_descriptor_set_add_array(ctx, dev, pool, &arr);
}

static void
vkr_dispatch_vkFreeDescriptorSets(struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkFreeDescriptorSets *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct list_head free_list;

   /* args->pDescriptorSets is marked noautovalidity="true" */
   if (args->descriptorSetCount && !args->pDescriptorSets) {
      vkr_context_set_fatal(ctx);
      return;
   }

   vkr_descriptor_set_destroy_driver_handles(ctx, args, &free_list);
   vkr_context_remove_objects(ctx, &free_list);

   args->ret = VK_SUCCESS;
}

static void
vkr_dispatch_vkUpdateDescriptorSets(struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkUpdateDescriptorSets *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

#ifdef __OHOS__
   vkr_winehua_wait_descriptor_update_queues(dev);
   vkr_winehua_track_ubo_mappings(dispatch->data,
                                  args->descriptorWriteCount,
                                  args->pDescriptorWrites);
#endif
   vkr_winehua_log_guest_descriptor_objects(args->descriptorWriteCount,
                                             args->pDescriptorWrites);
   vn_replace_vkUpdateDescriptorSets_args_handle(args);
   vkr_winehua_log_host_descriptor_handles(args->descriptorWriteCount,
                                            args->pDescriptorWrites);
   vk->UpdateDescriptorSets(args->device, args->descriptorWriteCount,
                            args->pDescriptorWrites, args->descriptorCopyCount,
                            args->pDescriptorCopies);
}

static void
vkr_dispatch_vkCreateDescriptorUpdateTemplate(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkCreateDescriptorUpdateTemplate *args)
{
   vkr_descriptor_update_template_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroyDescriptorUpdateTemplate(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkDestroyDescriptorUpdateTemplate *args)
{
   vkr_descriptor_update_template_destroy_and_remove(dispatch->data, args);
}

void
vkr_context_init_descriptor_set_layout_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkGetDescriptorSetLayoutSupport =
      vkr_dispatch_vkGetDescriptorSetLayoutSupport;
   dispatch->dispatch_vkCreateDescriptorSetLayout =
      vkr_dispatch_vkCreateDescriptorSetLayout;
   dispatch->dispatch_vkDestroyDescriptorSetLayout =
      vkr_dispatch_vkDestroyDescriptorSetLayout;
}

void
vkr_context_init_descriptor_pool_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateDescriptorPool = vkr_dispatch_vkCreateDescriptorPool;
   dispatch->dispatch_vkDestroyDescriptorPool = vkr_dispatch_vkDestroyDescriptorPool;
   dispatch->dispatch_vkResetDescriptorPool = vkr_dispatch_vkResetDescriptorPool;
}

void
vkr_context_init_descriptor_set_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkAllocateDescriptorSets = vkr_dispatch_vkAllocateDescriptorSets;
   dispatch->dispatch_vkFreeDescriptorSets = vkr_dispatch_vkFreeDescriptorSets;
   dispatch->dispatch_vkUpdateDescriptorSets = vkr_dispatch_vkUpdateDescriptorSets;
}

void
vkr_context_init_descriptor_update_template_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateDescriptorUpdateTemplate =
      vkr_dispatch_vkCreateDescriptorUpdateTemplate;
   dispatch->dispatch_vkDestroyDescriptorUpdateTemplate =
      vkr_dispatch_vkDestroyDescriptorUpdateTemplate;
   dispatch->dispatch_vkUpdateDescriptorSetWithTemplate = NULL;
}
