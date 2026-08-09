/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_command_buffer.h"

#include "vkr_command_buffer_gen.h"
#include "vkr_buffer.h"
#include "vkr_descriptor_set.h"
#include "vkr_device_memory.h"
#include "vkr_image.h"
#include "vkr_pipeline.h"
#include "vkr_render_pass.h"

#include <stdatomic.h>
#include <stdlib.h>

#ifdef __clang__
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#endif

#define VKR_CMD_CALL(cmd_name, args, ...)                                                \
   do {                                                                                  \
      struct vkr_command_buffer *_cmd =                                                  \
         vkr_command_buffer_from_handle(args->commandBuffer);                            \
      struct vn_device_proc_table *_vk = &_cmd->device->proc_table;                      \
                                                                                         \
      vn_replace_vk##cmd_name##_args_handle(args);                                       \
      _vk->cmd_name(args->commandBuffer, ##__VA_ARGS__);                                 \
   } while (0)

#define VKR_WINEHUA_CAPTURE_TRACE_LIMIT 512u
#define VKR_WINEHUA_VIEWPORT_TRACE_LIMIT 4096u
#ifdef __OHOS__
#define VKR_WINEHUA_UBO_BOUND_TRACE_LIMIT 50000u
#endif

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

static unsigned
vkr_winehua_capture_trace_limit(void)
{
   static unsigned limit;
   if (!limit) {
      const char *value = os_get_option("WINEHUA_VKR_TRACE_CAPTURE_LIMIT");
      char *end = NULL;
      const unsigned parsed = value ? (unsigned)strtoul(value, &end, 10) : 0;
      limit = parsed && end && !*end && parsed <= 1000000u ? parsed :
         VKR_WINEHUA_CAPTURE_TRACE_LIMIT;
   }
   return limit;
}

static bool
vkr_winehua_capture_trace_allow(void)
{
   static atomic_uint emitted = ATOMIC_VAR_INIT(0);
   const unsigned limit = vkr_winehua_capture_trace_limit();
   const unsigned index =
      atomic_fetch_add_explicit(&emitted, 1, memory_order_relaxed);

   if (index < limit)
      return true;
   if (index == limit)
      vkr_log("WineHuaCapture: command capture limit reached; further records suppressed");
   return false;
}

static bool
vkr_winehua_viewport_trace_enabled(void)
{
   const char *value = os_get_option("WINEHUA_VKR_TRACE_PIPELINE");
   return value && value[0] == '1';
}

static bool
vkr_winehua_viewport_trace_allow(void)
{
   static atomic_uint emitted = ATOMIC_VAR_INIT(0);
   const unsigned index =
      atomic_fetch_add_explicit(&emitted, 1, memory_order_relaxed);

   if (index < VKR_WINEHUA_VIEWPORT_TRACE_LIMIT)
      return true;
   if (index == VKR_WINEHUA_VIEWPORT_TRACE_LIMIT)
      vkr_log("WineHuaViewportHost: trace limit reached; further records suppressed");
   return false;
}

#ifdef __OHOS__
static bool
vkr_winehua_ubo_identity_trace_enabled(void)
{
   const char *value = os_get_option("WINEHUA_VKR_TRACE_UBO_IDENTITY");
   return value && value[0] && !(value[0] == '0' && !value[1]);
}

static bool
vkr_winehua_ubo_bound_trace_allow(void)
{
   static atomic_uint emitted = ATOMIC_VAR_INIT(0);
   const unsigned index =
      atomic_fetch_add_explicit(&emitted, 1, memory_order_relaxed);
   if (index < VKR_WINEHUA_UBO_BOUND_TRACE_LIMIT)
      return true;
   if (index == VKR_WINEHUA_UBO_BOUND_TRACE_LIMIT)
      vkr_log("WineHuaUboHost: phase=bound-descriptor trace limit reached");
   return false;
}

static void
vkr_winehua_log_bound_ubo(struct vkr_command_buffer *cmd,
                          struct vkr_descriptor_set *set)
{
   if (!cmd || !set)
      return;

   for (uint32_t i = 0; i < ARRAY_SIZE(set->winehua_ubo_bindings); i++) {
      struct vkr_winehua_ubo_binding *state =
         &set->winehua_ubo_bindings[i];
      struct vkr_buffer *buffer = state->buffer;
      struct vkr_device_memory *mem = buffer ? buffer->bound_memory : NULL;
      if (!state->valid || !buffer || !mem ||
          (state->size != 48 && state->size != 64 &&
           state->size != 1536) ||
          state->offset > UINT64_MAX - buffer->bound_memory_offset)
         continue;

      vkr_winehua_buffer_add_ubo_watch_locked(
         buffer, state->binding, state->offset, state->size);
      state->last_bound_mapping_sequence = state->mapping_sequence;
      if (!vkr_winehua_ubo_bound_trace_allow())
         continue;

      vkr_log("WineHuaUboHost: phase=bound-descriptor"
              " cmdId=%" PRIu64 " hostCmd=0x%" PRIxPTR
              " setId=%" PRIu64 " hostSet=0x%" PRIxPTR
              " mappingSequence=%" PRIu64
              " binding=%u arrayElement=%u type=%u"
              " bufferId=%" PRIu64 " hostBuffer=0x%" PRIxPTR
              " memoryId=%" PRIu64 " hostMemory=0x%" PRIxPTR
              " bufferMemoryOffset=%" PRIu64
              " descriptorOffset=%" PRIu64
              " absoluteOffset=%" PRIu64 " bytes=%" PRIu64,
              (uint64_t)cmd->base.id,
              (uintptr_t)cmd->base.handle.command_buffer,
              (uint64_t)set->base.id,
              (uintptr_t)set->base.handle.descriptor_set,
              state->mapping_sequence, state->binding, state->array_element,
              state->descriptor_type, (uint64_t)buffer->base.id,
              (uintptr_t)buffer->base.handle.buffer,
              (uint64_t)mem->base.id,
              (uintptr_t)mem->base.handle.device_memory,
              (uint64_t)buffer->bound_memory_offset,
              (uint64_t)state->offset,
              (uint64_t)(buffer->bound_memory_offset + state->offset),
              (uint64_t)state->size);
   }
}
#endif

#ifdef __OHOS__
static uint32_t
vkr_winehua_array_trace_bpp(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SRGB:
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
      return 4;
   default:
      return 0;
   }
}

/* Inspect the staging bytes used by a Texture2DArray upload. This is
 * diagnostic-only: it never changes the command or its source. */
static void
vkr_winehua_log_array_layer_stats(const struct vkr_buffer *buffer,
                                  const struct vkr_image *image,
                                  const VkBufferImageCopy *region,
                                  uint32_t layer,
                                  const char *source_name,
                                  const uint8_t *source,
                                  VkDeviceSize source_size)
{
   if (!buffer || !image || !region || !source || !source_size ||
       !source_name || !vkr_winehua_capture_trace_allow())
      return;

   const uint32_t bpp = vkr_winehua_array_trace_bpp(image->format);
   if (!bpp || !region->imageExtent.width || !region->imageExtent.height)
      return;

   const uint64_t row_length = region->bufferRowLength
      ? region->bufferRowLength : region->imageExtent.width;
   const uint64_t image_height = region->bufferImageHeight
      ? region->bufferImageHeight : region->imageExtent.height;
   const uint64_t row_bytes = (uint64_t)region->imageExtent.width * bpp;
   const uint64_t row_pitch = row_length * bpp;
   const uint64_t layer_stride = row_pitch * image_height *
      MAX2(region->imageExtent.depth, 1u);
   if (row_bytes > row_pitch || !row_pitch || !layer_stride)
      return;

   if (buffer->bound_memory_offset > source_size)
      return;
   const uint64_t relative = (uint64_t)buffer->bound_memory_offset +
      (uint64_t)region->bufferOffset + (uint64_t)layer * layer_stride;
   if (relative > source_size || row_pitch > source_size - relative)
      return;

   uint32_t hash = 2166136261u;
   uint64_t rgb_nonzero = 0;
   uint64_t alpha_zero = 0;
   uint64_t alpha_255 = 0;
   uint64_t alpha_other = 0;
   for (uint32_t y = 0; y < region->imageExtent.height; y++) {
      const uint64_t row_offset = relative + (uint64_t)y * row_pitch;
      if (row_offset > source_size || row_bytes > source_size - row_offset)
         return;
      const uint8_t *row = source + row_offset;
      for (uint32_t x = 0; x < region->imageExtent.width; x++) {
         const uint8_t *pixel = row + (size_t)x * bpp;
         for (uint32_t c = 0; c < bpp; c++) {
            hash ^= pixel[c];
            hash *= 16777619u;
         }
         if (pixel[0] || pixel[1] || pixel[2])
            rgb_nonzero++;
         if (pixel[3] == 0)
            alpha_zero++;
         else if (pixel[3] == 255)
            alpha_255++;
         else
            alpha_other++;
      }
   }

   vkr_log("WineHuaArrayUpload: imageId=%" PRIu64
           " bufferId=%" PRIu64 " memoryId=%" PRIu64
           " format=%u mip=%u layer=%u layers=%u"
           " extent=%ux%ux%u rowLength=%" PRIu64
           " imageHeight=%" PRIu64 " bufferOffset=%" PRIu64
           " bindOffset=%" PRIu64 " absoluteOffset=%" PRIu64
           " source=%s fnv=0x%08x rgbNonzero=%" PRIu64
           " alpha0=%" PRIu64 " alpha255=%" PRIu64
           " alphaOther=%" PRIu64,
           (uint64_t)image->base.id, (uint64_t)buffer->base.id,
           buffer->bound_memory ? (uint64_t)buffer->bound_memory->base.id : 0,
           image->format, region->imageSubresource.mipLevel,
           region->imageSubresource.baseArrayLayer + layer,
           region->imageSubresource.layerCount,
           region->imageExtent.width, region->imageExtent.height,
           region->imageExtent.depth, row_length, image_height,
           (uint64_t)region->bufferOffset,
           (uint64_t)buffer->bound_memory_offset, relative,
           source_name, hash, rgb_nonzero, alpha_zero,
           alpha_255, alpha_other);
}
#endif

static void
vkr_winehua_log_buffer_binding(const char *kind,
                                const struct vkr_command_buffer *cmd,
                                uint32_t binding,
                                const struct vkr_buffer *buffer,
                                VkDeviceSize offset)
{
   if (!vkr_winehua_capture_trace_enabled() ||
       !vkr_winehua_capture_trace_allow())
      return;

   const struct vkr_device_memory *mem = buffer ? buffer->bound_memory : NULL;
   vkr_log("WineHuaCapture: %s cmdId=%" PRIu64 " hostCmd=0x%" PRIxPTR
           " binding=%u bufferId=%" PRIu64 " hostBuffer=0x%" PRIxPTR
           " memoryId=%" PRIu64 " memoryOffset=%" PRIu64 " bindOffset=%" PRIu64,
           kind, cmd ? cmd->base.id : 0,
           cmd ? (uintptr_t)cmd->base.handle.command_buffer : 0, binding,
           buffer ? buffer->base.id : 0,
           buffer ? (uintptr_t)buffer->base.handle.buffer : 0,
           mem ? mem->base.id : 0,
           buffer ? (uint64_t)buffer->bound_memory_offset : 0,
           (uint64_t)offset);
}

static void
vkr_winehua_log_image_copy(const char *kind,
                           const struct vkr_command_buffer *cmd,
                           const struct vkr_buffer *buffer,
                           const struct vkr_image *image,
                           VkImageLayout layout,
                           uint32_t region_count,
                           const VkBufferImageCopy *regions)
{
   if (!vkr_winehua_capture_trace_enabled())
      return;

   uint64_t memory_id = 0;
   uintptr_t host_memory = 0;
   VkDeviceSize bind_offset = 0;
   VkDeviceSize buffer_size = 0;
   VkBufferUsageFlags guest_usage = 0;
   VkBufferUsageFlags host_usage = 0;
#ifdef __OHOS__
   const struct vkr_device_memory *mem = buffer ? buffer->bound_memory : NULL;
   memory_id = mem ? mem->base.id : 0;
   host_memory = mem ? (uintptr_t)mem->base.handle.device_memory : 0;
   bind_offset = buffer ? buffer->bound_memory_offset : 0;
   buffer_size = buffer ? buffer->size : 0;
   guest_usage = buffer ? buffer->guest_usage : 0;
   host_usage = buffer ? buffer->host_usage : 0;
#endif

   for (uint32_t i = 0; i < region_count; i++) {
      if (!vkr_winehua_capture_trace_allow())
         break;
      const VkBufferImageCopy *region = &regions[i];
      vkr_log("WineHuaCapture: %s cmdId=%" PRIu64
              " hostCmd=0x%" PRIxPTR " bufferId=%" PRIu64
              " hostBuffer=0x%" PRIxPTR " memoryId=%" PRIu64
              " hostMemory=0x%" PRIxPTR " bindOffset=%" PRIu64
              " bufferSize=%" PRIu64 " guestUsage=0x%x hostUsage=0x%x"
              " imageId=%" PRIu64
              " hostImage=0x%" PRIxPTR " format=%u usage=0x%x layout=%u"
              " region=%u bufferOffset=%" PRIu64 " rowLength=%u imageHeight=%u"
              " aspect=0x%x mip=%u baseLayer=%u layers=%u"
              " imageOffset=%d,%d,%d extent=%u,%u,%u",
              kind, cmd ? cmd->base.id : 0,
              cmd ? (uintptr_t)cmd->base.handle.command_buffer : 0,
              buffer ? buffer->base.id : 0,
              buffer ? (uintptr_t)buffer->base.handle.buffer : 0,
              memory_id, host_memory, (uint64_t)bind_offset,
              (uint64_t)buffer_size, guest_usage, host_usage,
              image ? image->base.id : 0,
              image ? (uintptr_t)image->base.handle.image : 0,
              image ? image->format : VK_FORMAT_UNDEFINED,
              image ? image->usage : 0, layout, i,
              (uint64_t)region->bufferOffset, region->bufferRowLength,
              region->bufferImageHeight,
              region->imageSubresource.aspectMask,
              region->imageSubresource.mipLevel,
              region->imageSubresource.baseArrayLayer,
              region->imageSubresource.layerCount,
              region->imageOffset.x, region->imageOffset.y, region->imageOffset.z,
              region->imageExtent.width, region->imageExtent.height,
              region->imageExtent.depth);
#ifdef __OHOS__
      if (image && buffer && mem && image->array_layers > 1 &&
          region->imageSubresource.layerCount &&
          region->imageSubresource.baseArrayLayer < image->array_layers) {
         const uint32_t available_layers = image->array_layers -
            region->imageSubresource.baseArrayLayer;
         const uint32_t layer_count = MIN2(
            region->imageSubresource.layerCount, available_layers);
         const uint8_t *shadow_source = NULL;
         const char *shadow_name = "shadow";
         VkDeviceSize shadow_size = 0;
         if (mem->shadow_host_copy_deferred && mem->shadow_upload_snapshot) {
            shadow_source = mem->shadow_upload_snapshot;
            shadow_name = "snapshot";
            shadow_size = MIN2(mem->shadow_size, mem->allocation_size);
         } else if (mem->shadow_map) {
            shadow_source = mem->shadow_map;
            shadow_size = MIN2(mem->shadow_size, mem->allocation_size);
         }
         const uint8_t *host_source = mem->host_map;
         const VkDeviceSize host_size = mem->host_map ? mem->allocation_size : 0;
         vkr_log("WineHuaArrayUploadState: imageId=%" PRIu64
                 " bufferId=%" PRIu64 " memoryId=%" PRIu64
                 " mip=%u baseLayer=%u layers=%u imageLayers=%u"
                 " shadow=%u snapshot=%u host=%u deferred=%u"
                 " shadowSize=%" PRIu64 " allocationSize=%" PRIu64,
                 (uint64_t)image->base.id, (uint64_t)buffer->base.id,
                 (uint64_t)mem->base.id,
                 region->imageSubresource.mipLevel,
                 region->imageSubresource.baseArrayLayer,
                 region->imageSubresource.layerCount, image->array_layers,
                 mem->shadow_map != NULL,
                 mem->shadow_upload_snapshot != NULL,
                 mem->host_map != NULL,
                 mem->shadow_host_copy_deferred,
                 (uint64_t)mem->shadow_size,
                 (uint64_t)mem->allocation_size);
         for (uint32_t layer = 0; layer < layer_count; layer++) {
            vkr_winehua_log_array_layer_stats(
               buffer, image, region, layer, shadow_name,
               shadow_source, shadow_size);
            if (host_source && host_source != shadow_source)
               vkr_winehua_log_array_layer_stats(
                  buffer, image, region, layer, "host",
                  host_source, host_size);
         }
      }
#endif
   }
}

static void
vkr_winehua_log_buffer_copy(const struct vkr_command_buffer *cmd,
                             const struct vkr_buffer *src,
                             const struct vkr_buffer *dst,
                             uint32_t region_count,
                             const VkBufferCopy *regions)
{
   if (!vkr_winehua_capture_trace_enabled())
      return;

   const struct vkr_device_memory *src_mem = src ? src->bound_memory : NULL;
   const struct vkr_device_memory *dst_mem = dst ? dst->bound_memory : NULL;
   for (uint32_t i = 0; i < region_count; i++) {
      if (!vkr_winehua_capture_trace_allow())
         break;
      const VkBufferCopy *region = &regions[i];
      vkr_log("WineHuaCapture: copy-buffer cmdId=%" PRIu64
              " hostCmd=0x%" PRIxPTR " srcBufferId=%" PRIu64
              " hostSrcBuffer=0x%" PRIxPTR " srcMemoryId=%" PRIu64
              " srcHostMemory=0x%" PRIxPTR " srcBindOffset=%" PRIu64
              " dstBufferId=%" PRIu64 " hostDstBuffer=0x%" PRIxPTR
              " dstMemoryId=%" PRIu64 " dstHostMemory=0x%" PRIxPTR
              " dstBindOffset=%" PRIu64 " region=%u srcOffset=%" PRIu64
              " dstOffset=%" PRIu64 " size=%" PRIu64,
              cmd ? cmd->base.id : 0,
              cmd ? (uintptr_t)cmd->base.handle.command_buffer : 0,
              src ? src->base.id : 0,
              src ? (uintptr_t)src->base.handle.buffer : 0,
              src_mem ? src_mem->base.id : 0,
              src_mem ? (uintptr_t)src_mem->base.handle.device_memory : 0,
              src ? (uint64_t)src->bound_memory_offset : 0,
              dst ? dst->base.id : 0,
              dst ? (uintptr_t)dst->base.handle.buffer : 0,
              dst_mem ? dst_mem->base.id : 0,
              dst_mem ? (uintptr_t)dst_mem->base.handle.device_memory : 0,
              dst ? (uint64_t)dst->bound_memory_offset : 0,
              i, (uint64_t)region->srcOffset,
              (uint64_t)region->dstOffset, (uint64_t)region->size);
   }
}

static void
vkr_dispatch_vkCreateCommandPool(struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkCreateCommandPool *args)
{
   struct vkr_command_pool *pool = vkr_command_pool_create_and_add(dispatch->data, args);
   if (!pool)
      return;

   list_inithead(&pool->command_buffers);
}

static void
vkr_dispatch_vkDestroyCommandPool(struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkDestroyCommandPool *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_command_pool *pool = vkr_command_pool_from_handle(args->commandPool);

   if (!pool)
      return;

   vkr_command_pool_release(ctx, pool);
   vkr_command_pool_destroy_and_remove(ctx, args);
}

static void
vkr_dispatch_vkResetCommandPool(UNUSED struct vn_dispatch_context *dispatch,
                                struct vn_command_vkResetCommandPool *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkResetCommandPool_args_handle(args);
   args->ret = vk->ResetCommandPool(args->device, args->commandPool, args->flags);
}

static void
vkr_dispatch_vkTrimCommandPool(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkTrimCommandPool *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkTrimCommandPool_args_handle(args);
   vk->TrimCommandPool(args->device, args->commandPool, args->flags);
}

static void
vkr_dispatch_vkAllocateCommandBuffers(struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkAllocateCommandBuffers *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vkr_command_pool *pool =
      vkr_command_pool_from_handle(args->pAllocateInfo->commandPool);
   struct object_array arr;

   if (!pool) {
      vkr_context_set_fatal(ctx);
      return;
   }

   if (vkr_command_buffer_create_array(ctx, args, &arr) != VK_SUCCESS)
      return;

   vkr_command_buffer_add_array(ctx, dev, pool, &arr);
}

static void
vkr_dispatch_vkFreeCommandBuffers(struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkFreeCommandBuffers *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct list_head free_list;

   /* args->pCommandBuffers is marked noautovalidity="true" */
   if (args->commandBufferCount && !args->pCommandBuffers) {
      vkr_context_set_fatal(ctx);
      return;
   }

   vkr_command_buffer_destroy_driver_handles(ctx, args, &free_list);
   vkr_context_remove_objects(ctx, &free_list);
}

static void
vkr_dispatch_vkResetCommandBuffer(UNUSED struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkResetCommandBuffer *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkResetCommandBuffer_args_handle(args);
   args->ret = vk->ResetCommandBuffer(args->commandBuffer, args->flags);
}

static void
vkr_dispatch_vkBeginCommandBuffer(UNUSED struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkBeginCommandBuffer *args)
{
   TRACE_FUNC();
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkBeginCommandBuffer_args_handle(args);
   args->ret = vk->BeginCommandBuffer(args->commandBuffer, args->pBeginInfo);
}

static void
vkr_dispatch_vkEndCommandBuffer(UNUSED struct vn_dispatch_context *dispatch,
                                struct vn_command_vkEndCommandBuffer *args)
{
   TRACE_FUNC();
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkEndCommandBuffer_args_handle(args);
   args->ret = vk->EndCommandBuffer(args->commandBuffer);
}

static void
vkr_dispatch_vkCmdBindPipeline(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdBindPipeline *args)
{
   if (vkr_winehua_capture_trace_enabled() && vkr_winehua_capture_trace_allow()) {
      struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
      const struct vkr_pipeline *pipeline = vkr_pipeline_from_handle(args->pipeline);
      vkr_log("WineHuaCapture: bind-pipeline cmdId=%" PRIu64 " hostCmd=0x%" PRIxPTR
              " bindPoint=%u pipelineId=%" PRIu64 " hostPipeline=0x%" PRIxPTR,
              cmd ? cmd->base.id : 0,
              cmd ? (uintptr_t)cmd->base.handle.command_buffer : 0,
              args->pipelineBindPoint, pipeline ? pipeline->base.id : 0,
              pipeline ? (uintptr_t)pipeline->base.handle.pipeline : 0);
   }
   VKR_CMD_CALL(CmdBindPipeline, args, args->pipelineBindPoint, args->pipeline);
}

static void
vkr_dispatch_vkCmdSetViewport(UNUSED struct vn_dispatch_context *dispatch,
                              struct vn_command_vkCmdSetViewport *args)
{
   VKR_CMD_CALL(CmdSetViewport, args, args->firstViewport, args->viewportCount,
                args->pViewports);
}

static void
vkr_dispatch_vkCmdSetScissor(UNUSED struct vn_dispatch_context *dispatch,
                             struct vn_command_vkCmdSetScissor *args)
{
   VKR_CMD_CALL(CmdSetScissor, args, args->firstScissor, args->scissorCount,
                args->pScissors);
}

static void
vkr_dispatch_vkCmdSetLineWidth(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdSetLineWidth *args)
{
   VKR_CMD_CALL(CmdSetLineWidth, args, args->lineWidth);
}

static void
vkr_dispatch_vkCmdSetDepthBias(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdSetDepthBias *args)
{
   VKR_CMD_CALL(CmdSetDepthBias, args, args->depthBiasConstantFactor,
                args->depthBiasClamp, args->depthBiasSlopeFactor);
}

static void
vkr_dispatch_vkCmdSetBlendConstants(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCmdSetBlendConstants *args)
{
   VKR_CMD_CALL(CmdSetBlendConstants, args, args->blendConstants);
}

static void
vkr_dispatch_vkCmdSetDepthBounds(UNUSED struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkCmdSetDepthBounds *args)
{
   VKR_CMD_CALL(CmdSetDepthBounds, args, args->minDepthBounds, args->maxDepthBounds);
}

static void
vkr_dispatch_vkCmdSetStencilCompareMask(UNUSED struct vn_dispatch_context *dispatch,
                                        struct vn_command_vkCmdSetStencilCompareMask *args)
{
   VKR_CMD_CALL(CmdSetStencilCompareMask, args, args->faceMask, args->compareMask);
}

static void
vkr_dispatch_vkCmdSetStencilWriteMask(UNUSED struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkCmdSetStencilWriteMask *args)
{
   VKR_CMD_CALL(CmdSetStencilWriteMask, args, args->faceMask, args->writeMask);
}

static void
vkr_dispatch_vkCmdSetStencilReference(UNUSED struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkCmdSetStencilReference *args)
{
   VKR_CMD_CALL(CmdSetStencilReference, args, args->faceMask, args->reference);
}

static void
vkr_dispatch_vkCmdBindDescriptorSets(struct vn_dispatch_context *dispatch,
                                     struct vn_command_vkCmdBindDescriptorSets *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);

#ifdef __OHOS__
   if (vkr_winehua_ubo_identity_trace_enabled()) {
      struct vkr_context *ctx = dispatch->data;
      mtx_lock(&ctx->object_mutex);
      for (uint32_t i = 0; i < args->descriptorSetCount; i++) {
         struct vkr_descriptor_set *set =
            vkr_descriptor_set_from_handle(args->pDescriptorSets[i]);
         vkr_winehua_log_bound_ubo(cmd, set);
      }
      mtx_unlock(&ctx->object_mutex);
   }
#else
   (void)dispatch;
#endif

   if (vkr_winehua_capture_trace_enabled()) {
      for (uint32_t i = 0; i < args->descriptorSetCount; i++) {
         const struct vkr_descriptor_set *set =
            vkr_descriptor_set_from_handle(args->pDescriptorSets[i]);
         if (!vkr_winehua_capture_trace_allow())
            break;
         vkr_log("WineHuaCapture: bind-descriptor cmdId=%" PRIu64
                 " hostCmd=0x%" PRIxPTR " firstSet=%u setIndex=%u setId=%" PRIu64
                 " hostSet=0x%" PRIxPTR " bindPoint=%u dynamicOffsets=%u",
                 cmd ? cmd->base.id : 0,
                 cmd ? (uintptr_t)cmd->base.handle.command_buffer : 0,
                 args->firstSet, i, set ? set->base.id : 0,
                 set ? (uintptr_t)set->base.handle.descriptor_set : 0,
                 args->pipelineBindPoint, args->dynamicOffsetCount);
      }
      for (uint32_t i = 0; i < args->dynamicOffsetCount; i++) {
         if (!vkr_winehua_capture_trace_allow())
            break;
         vkr_log("WineHuaFrameAssoc: dynamic-offset cmdId=%" PRIu64
                 " hostCmd=0x%" PRIxPTR " index=%u value=%u",
                 cmd ? cmd->base.id : 0,
                 cmd ? (uintptr_t)cmd->base.handle.command_buffer : 0,
                 i, args->pDynamicOffsets[i]);
      }
   }

   vn_replace_vkCmdBindDescriptorSets_args_handle(args);
   cmd->device->proc_table.CmdBindDescriptorSets(
      args->commandBuffer, args->pipelineBindPoint, args->layout, args->firstSet,
      args->descriptorSetCount, args->pDescriptorSets, args->dynamicOffsetCount,
      args->pDynamicOffsets);
}

static void
vkr_dispatch_vkCmdBindIndexBuffer(UNUSED struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkCmdBindIndexBuffer *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vkr_buffer *buffer = vkr_buffer_from_handle(args->buffer);
   vkr_winehua_log_buffer_binding("bind-index", cmd, 0, buffer, args->offset);
   VKR_CMD_CALL(CmdBindIndexBuffer, args, args->buffer, args->offset, args->indexType);
}

static void
vkr_dispatch_vkCmdBindVertexBuffers(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCmdBindVertexBuffers *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   if (vkr_winehua_capture_trace_enabled()) {
      for (uint32_t i = 0; i < args->bindingCount; i++)
         vkr_winehua_log_buffer_binding("bind-vertex", cmd,
                                         args->firstBinding + i,
                                         vkr_buffer_from_handle(args->pBuffers[i]),
                                         args->pOffsets[i]);
   }
   VKR_CMD_CALL(CmdBindVertexBuffers, args, args->firstBinding, args->bindingCount,
                args->pBuffers, args->pOffsets);
}

static void
vkr_dispatch_vkCmdDraw(UNUSED struct vn_dispatch_context *dispatch,
                       struct vn_command_vkCmdDraw *args)
{
   if (vkr_winehua_capture_trace_enabled() && vkr_winehua_capture_trace_allow()) {
      struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
      vkr_log("WineHuaCapture: draw cmdId=%" PRIu64 " hostCmd=0x%" PRIxPTR
              " vertices=%u instances=%u firstVertex=%u firstInstance=%u",
              cmd ? cmd->base.id : 0,
              cmd ? (uintptr_t)cmd->base.handle.command_buffer : 0,
              args->vertexCount, args->instanceCount, args->firstVertex,
              args->firstInstance);
   }
   VKR_CMD_CALL(CmdDraw, args, args->vertexCount, args->instanceCount, args->firstVertex,
                args->firstInstance);
}

static void
vkr_dispatch_vkCmdDrawIndexed(UNUSED struct vn_dispatch_context *dispatch,
                              struct vn_command_vkCmdDrawIndexed *args)
{
   if (vkr_winehua_capture_trace_enabled() && vkr_winehua_capture_trace_allow()) {
      struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
      vkr_log("WineHuaCapture: draw-indexed cmdId=%" PRIu64 " hostCmd=0x%" PRIxPTR
              " indices=%u instances=%u firstIndex=%u vertexOffset=%d firstInstance=%u",
              cmd ? cmd->base.id : 0,
              cmd ? (uintptr_t)cmd->base.handle.command_buffer : 0,
              args->indexCount, args->instanceCount, args->firstIndex,
              args->vertexOffset, args->firstInstance);
   }
   VKR_CMD_CALL(CmdDrawIndexed, args, args->indexCount, args->instanceCount,
                args->firstIndex, args->vertexOffset, args->firstInstance);
}

static void
vkr_dispatch_vkCmdDrawIndirect(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdDrawIndirect *args)
{
   VKR_CMD_CALL(CmdDrawIndirect, args, args->buffer, args->offset, args->drawCount,
                args->stride);
}

static void
vkr_dispatch_vkCmdDrawIndexedIndirect(UNUSED struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkCmdDrawIndexedIndirect *args)
{
   VKR_CMD_CALL(CmdDrawIndexedIndirect, args, args->buffer, args->offset, args->drawCount,
                args->stride);
}

static void
vkr_dispatch_vkCmdDispatch(UNUSED struct vn_dispatch_context *dispatch,
                           struct vn_command_vkCmdDispatch *args)
{
   VKR_CMD_CALL(CmdDispatch, args, args->groupCountX, args->groupCountY,
                args->groupCountZ);
}

static void
vkr_dispatch_vkCmdDispatchIndirect(UNUSED struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkCmdDispatchIndirect *args)
{
   VKR_CMD_CALL(CmdDispatchIndirect, args, args->buffer, args->offset);
}

static void
vkr_dispatch_vkCmdCopyBuffer(UNUSED struct vn_dispatch_context *dispatch,
                             struct vn_command_vkCmdCopyBuffer *args)
{
   vkr_winehua_log_buffer_copy(
      vkr_command_buffer_from_handle(args->commandBuffer),
      vkr_buffer_from_handle(args->srcBuffer),
      vkr_buffer_from_handle(args->dstBuffer),
      args->regionCount, args->pRegions);
   VKR_CMD_CALL(CmdCopyBuffer, args, args->srcBuffer, args->dstBuffer, args->regionCount,
                args->pRegions);
}

static void
vkr_dispatch_vkCmdCopyBuffer2(UNUSED struct vn_dispatch_context *dispatch,
                              struct vn_command_vkCmdCopyBuffer2 *args)
{
   VKR_CMD_CALL(CmdCopyBuffer2, args, args->pCopyBufferInfo);
}

static void
vkr_dispatch_vkCmdCopyImage(UNUSED struct vn_dispatch_context *dispatch,
                            struct vn_command_vkCmdCopyImage *args)
{
   VKR_CMD_CALL(CmdCopyImage, args, args->srcImage, args->srcImageLayout, args->dstImage,
                args->dstImageLayout, args->regionCount, args->pRegions);
}

static void
vkr_dispatch_vkCmdCopyImage2(UNUSED struct vn_dispatch_context *dispatch,
                             struct vn_command_vkCmdCopyImage2 *args)
{
   VKR_CMD_CALL(CmdCopyImage2, args, args->pCopyImageInfo);
}

static void
vkr_dispatch_vkCmdBlitImage(UNUSED struct vn_dispatch_context *dispatch,
                            struct vn_command_vkCmdBlitImage *args)
{
   VKR_CMD_CALL(CmdBlitImage, args, args->srcImage, args->srcImageLayout, args->dstImage,
                args->dstImageLayout, args->regionCount, args->pRegions, args->filter);
}

static void
vkr_dispatch_vkCmdBlitImage2(UNUSED struct vn_dispatch_context *dispatch,
                             struct vn_command_vkCmdBlitImage2 *args)
{
   VKR_CMD_CALL(CmdBlitImage2, args, args->pBlitImageInfo);
}

static void
vkr_dispatch_vkCmdCopyBufferToImage(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCmdCopyBufferToImage *args)
{
   vkr_winehua_log_image_copy(
      "copy-buffer-to-image",
      vkr_command_buffer_from_handle(args->commandBuffer),
      vkr_buffer_from_handle(args->srcBuffer),
      vkr_image_from_handle(args->dstImage), args->dstImageLayout,
      args->regionCount, args->pRegions);
   VKR_CMD_CALL(CmdCopyBufferToImage, args, args->srcBuffer, args->dstImage,
                args->dstImageLayout, args->regionCount, args->pRegions);
}

static void
vkr_dispatch_vkCmdCopyBufferToImage2(UNUSED struct vn_dispatch_context *dispatch,
                                     struct vn_command_vkCmdCopyBufferToImage2 *args)
{
#ifdef __OHOS__
   /* Modern DXVK uses the Vulkan 1.3 copy-commands2 entry point. Keep the
    * upload inspection identical to the legacy entry point so array texture
    * alpha can be diagnosed without changing the submitted command. */
   const VkCopyBufferToImageInfo2 *info = args->pCopyBufferToImageInfo;
   if (info && info->regionCount && info->pRegions) {
      const struct vkr_command_buffer *cmd =
         vkr_command_buffer_from_handle(args->commandBuffer);
      const struct vkr_buffer *buffer = vkr_buffer_from_handle(info->srcBuffer);
      const struct vkr_image *image = vkr_image_from_handle(info->dstImage);
      for (uint32_t i = 0; i < info->regionCount; i++) {
         const VkBufferImageCopy2 *src = &info->pRegions[i];
         VkBufferImageCopy region = {
            .bufferOffset = src->bufferOffset,
            .bufferRowLength = src->bufferRowLength,
            .bufferImageHeight = src->bufferImageHeight,
            .imageSubresource = src->imageSubresource,
            .imageOffset = src->imageOffset,
            .imageExtent = src->imageExtent,
         };
         vkr_winehua_log_image_copy(
            "copy-buffer-to-image2", cmd, buffer, image,
            info->dstImageLayout, 1, &region);
      }
   }
#endif
   VKR_CMD_CALL(CmdCopyBufferToImage2, args, args->pCopyBufferToImageInfo);
}

static void
vkr_dispatch_vkCmdCopyImageToBuffer(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCmdCopyImageToBuffer *args)
{
   vkr_winehua_log_image_copy(
      "copy-image-to-buffer",
      vkr_command_buffer_from_handle(args->commandBuffer),
      vkr_buffer_from_handle(args->dstBuffer),
      vkr_image_from_handle(args->srcImage), args->srcImageLayout,
      args->regionCount, args->pRegions);
   VKR_CMD_CALL(CmdCopyImageToBuffer, args, args->srcImage, args->srcImageLayout,
                args->dstBuffer, args->regionCount, args->pRegions);
}

static void
vkr_dispatch_vkCmdCopyImageToBuffer2(UNUSED struct vn_dispatch_context *dispatch,
                                     struct vn_command_vkCmdCopyImageToBuffer2 *args)
{
   VKR_CMD_CALL(CmdCopyImageToBuffer2, args, args->pCopyImageToBufferInfo);
}

static void
vkr_dispatch_vkCmdUpdateBuffer(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdUpdateBuffer *args)
{
   VKR_CMD_CALL(CmdUpdateBuffer, args, args->dstBuffer, args->dstOffset, args->dataSize,
                args->pData);
}

static void
vkr_dispatch_vkCmdFillBuffer(UNUSED struct vn_dispatch_context *dispatch,
                             struct vn_command_vkCmdFillBuffer *args)
{
   VKR_CMD_CALL(CmdFillBuffer, args, args->dstBuffer, args->dstOffset, args->size,
                args->data);
}

static void
vkr_dispatch_vkCmdClearColorImage(UNUSED struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkCmdClearColorImage *args)
{
   VKR_CMD_CALL(CmdClearColorImage, args, args->image, args->imageLayout, args->pColor,
                args->rangeCount, args->pRanges);
}

static void
vkr_dispatch_vkCmdClearDepthStencilImage(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdClearDepthStencilImage *args)
{
   VKR_CMD_CALL(CmdClearDepthStencilImage, args, args->image, args->imageLayout,
                args->pDepthStencil, args->rangeCount, args->pRanges);
}

static void
vkr_dispatch_vkCmdClearAttachments(UNUSED struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkCmdClearAttachments *args)
{
   VKR_CMD_CALL(CmdClearAttachments, args, args->attachmentCount, args->pAttachments,
                args->rectCount, args->pRects);
}

static void
vkr_dispatch_vkCmdResolveImage(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdResolveImage *args)
{
   VKR_CMD_CALL(CmdResolveImage, args, args->srcImage, args->srcImageLayout,
                args->dstImage, args->dstImageLayout, args->regionCount, args->pRegions);
}

static void
vkr_dispatch_vkCmdResolveImage2(UNUSED struct vn_dispatch_context *dispatch,
                                struct vn_command_vkCmdResolveImage2 *args)
{
   VKR_CMD_CALL(CmdResolveImage2, args, args->pResolveImageInfo);
}

static void
vkr_dispatch_vkCmdSetEvent(UNUSED struct vn_dispatch_context *dispatch,
                           struct vn_command_vkCmdSetEvent *args)
{
   VKR_CMD_CALL(CmdSetEvent, args, args->event, args->stageMask);
}

static void
vkr_dispatch_vkCmdResetEvent(UNUSED struct vn_dispatch_context *dispatch,
                             struct vn_command_vkCmdResetEvent *args)
{
   VKR_CMD_CALL(CmdResetEvent, args, args->event, args->stageMask);
}

static void
vkr_dispatch_vkCmdWaitEvents(UNUSED struct vn_dispatch_context *dispatch,
                             struct vn_command_vkCmdWaitEvents *args)
{
   VKR_CMD_CALL(CmdWaitEvents, args, args->eventCount, args->pEvents, args->srcStageMask,
                args->dstStageMask, args->memoryBarrierCount, args->pMemoryBarriers,
                args->bufferMemoryBarrierCount, args->pBufferMemoryBarriers,
                args->imageMemoryBarrierCount, args->pImageMemoryBarriers);
}

static void
vkr_dispatch_vkCmdPipelineBarrier(struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkCmdPipelineBarrier *args)
{
   if (vkr_winehua_capture_trace_enabled()) {
      struct vkr_command_buffer *cmd =
         vkr_command_buffer_from_handle(args->commandBuffer);
      for (uint32_t i = 0; i < args->imageMemoryBarrierCount; i++) {
         const VkImageMemoryBarrier *barrier = &args->pImageMemoryBarriers[i];
         const struct vkr_image *image = vkr_image_from_handle(barrier->image);

         /* DXVK's private-present source images transition from color output
          * to GENERAL before the Wine Vulkan present bridge consumes them.
          * Keep this narrow identity record independent from the broad command
          * capture limit so a long Heaven run still joins frame -> command ->
          * source image -> present without changing Vulkan behavior. */
         if (image &&
             (image->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) ==
                (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
             barrier->oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
             barrier->newLayout == VK_IMAGE_LAYOUT_GENERAL) {
            const struct vkr_context *ctx = dispatch->data;
            vkr_log("WineHuaFrameAssoc: source-transition ctx=%u guestCmd=0x%" PRIxPTR
                    " cmdId=%" PRIu64 " hostCmd=0x%" PRIxPTR
                    " imageId=%" PRIu64 " hostImage=0x%" PRIxPTR
                    " size=%ux%u format=%u oldLayout=%u newLayout=%u",
                    ctx ? ctx->ctx_id : 0, (uintptr_t)args->commandBuffer,
                    cmd ? cmd->base.id : 0,
                    cmd ? (uintptr_t)cmd->base.handle.command_buffer : 0,
                    image->base.id, (uintptr_t)image->base.handle.image,
                    image->extent.width, image->extent.height, image->format,
                    barrier->oldLayout, barrier->newLayout);
         }

         if (!vkr_winehua_capture_trace_allow())
            continue;
         vkr_log("WineHuaCapture: image-barrier cmdId=%" PRIu64
                 " hostCmd=0x%" PRIxPTR " imageId=%" PRIu64
                 " hostImage=0x%" PRIxPTR " srcStage=0x%x dstStage=0x%x"
                 " srcAccess=0x%x dstAccess=0x%x oldLayout=%u newLayout=%u"
                 " aspect=0x%x baseMip=%u levels=%u baseLayer=%u layers=%u",
                 cmd ? cmd->base.id : 0,
                 cmd ? (uintptr_t)cmd->base.handle.command_buffer : 0,
                 image ? image->base.id : 0,
                 image ? (uintptr_t)image->base.handle.image : 0,
                 args->srcStageMask, args->dstStageMask,
                 barrier->srcAccessMask, barrier->dstAccessMask,
                 barrier->oldLayout, barrier->newLayout,
                 barrier->subresourceRange.aspectMask,
                 barrier->subresourceRange.baseMipLevel,
                 barrier->subresourceRange.levelCount,
                 barrier->subresourceRange.baseArrayLayer,
                 barrier->subresourceRange.layerCount);
      }
   }
   VKR_CMD_CALL(CmdPipelineBarrier, args, args->srcStageMask, args->dstStageMask,
                args->dependencyFlags, args->memoryBarrierCount, args->pMemoryBarriers,
                args->bufferMemoryBarrierCount, args->pBufferMemoryBarriers,
                args->imageMemoryBarrierCount, args->pImageMemoryBarriers);
}

static void
vkr_dispatch_vkCmdBeginQuery(UNUSED struct vn_dispatch_context *dispatch,
                             struct vn_command_vkCmdBeginQuery *args)
{
   VKR_CMD_CALL(CmdBeginQuery, args, args->queryPool, args->query, args->flags);
}

static void
vkr_dispatch_vkCmdEndQuery(UNUSED struct vn_dispatch_context *dispatch,
                           struct vn_command_vkCmdEndQuery *args)
{
   VKR_CMD_CALL(CmdEndQuery, args, args->queryPool, args->query);
}

static void
vkr_dispatch_vkCmdResetQueryPool(UNUSED struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkCmdResetQueryPool *args)
{
   VKR_CMD_CALL(CmdResetQueryPool, args, args->queryPool, args->firstQuery,
                args->queryCount);
}

static void
vkr_dispatch_vkCmdWriteTimestamp(UNUSED struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkCmdWriteTimestamp *args)
{
   VKR_CMD_CALL(CmdWriteTimestamp, args, args->pipelineStage, args->queryPool,
                args->query);
}

static void
vkr_dispatch_vkCmdCopyQueryPoolResults(UNUSED struct vn_dispatch_context *dispatch,
                                       struct vn_command_vkCmdCopyQueryPoolResults *args)
{
   VKR_CMD_CALL(CmdCopyQueryPoolResults, args, args->queryPool, args->firstQuery,
                args->queryCount, args->dstBuffer, args->dstOffset, args->stride,
                args->flags);
}

static void
vkr_dispatch_vkCmdPushConstants(UNUSED struct vn_dispatch_context *dispatch,
                                struct vn_command_vkCmdPushConstants *args)
{
   VKR_CMD_CALL(CmdPushConstants, args, args->layout, args->stageFlags, args->offset,
                args->size, args->pValues);
}

static void
vkr_dispatch_vkCmdBeginRenderPass(UNUSED struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkCmdBeginRenderPass *args)
{
   if (vkr_winehua_capture_trace_enabled() && vkr_winehua_capture_trace_allow()) {
      struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
      const VkRenderPassBeginInfo *begin = args->pRenderPassBegin;
      const struct vkr_render_pass *pass =
         begin ? vkr_render_pass_from_handle(begin->renderPass) : NULL;
      const struct vkr_framebuffer *fb =
         begin ? vkr_framebuffer_from_handle(begin->framebuffer) : NULL;
      vkr_log("WineHuaCapture: begin-render-pass cmdId=%" PRIu64
              " hostCmd=0x%" PRIxPTR " passId=%" PRIu64 " hostPass=0x%" PRIxPTR
              " framebufferId=%" PRIu64 " hostFramebuffer=0x%" PRIxPTR
              " area=%d,%d %ux%u clearValues=%u contents=%u",
              cmd ? cmd->base.id : 0,
              cmd ? (uintptr_t)cmd->base.handle.command_buffer : 0,
              pass ? pass->base.id : 0,
              pass ? (uintptr_t)pass->base.handle.render_pass : 0,
              fb ? fb->base.id : 0,
              fb ? (uintptr_t)fb->base.handle.framebuffer : 0,
              begin ? begin->renderArea.offset.x : 0,
              begin ? begin->renderArea.offset.y : 0,
              begin ? begin->renderArea.extent.width : 0,
              begin ? begin->renderArea.extent.height : 0,
              begin ? begin->clearValueCount : 0, args->contents);
   }
   VKR_CMD_CALL(CmdBeginRenderPass, args, args->pRenderPassBegin, args->contents);
}

static void
vkr_dispatch_vkCmdNextSubpass(UNUSED struct vn_dispatch_context *dispatch,
                              struct vn_command_vkCmdNextSubpass *args)
{
   VKR_CMD_CALL(CmdNextSubpass, args, args->contents);
}

static void
vkr_dispatch_vkCmdEndRenderPass(UNUSED struct vn_dispatch_context *dispatch,
                                struct vn_command_vkCmdEndRenderPass *args)
{
   if (vkr_winehua_capture_trace_enabled() && vkr_winehua_capture_trace_allow()) {
      struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
      vkr_log("WineHuaCapture: end-render-pass cmdId=%" PRIu64 " hostCmd=0x%" PRIxPTR,
              cmd ? cmd->base.id : 0,
              cmd ? (uintptr_t)cmd->base.handle.command_buffer : 0);
   }
   VKR_CMD_CALL(CmdEndRenderPass, args);
}

static void
vkr_dispatch_vkCmdExecuteCommands(UNUSED struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkCmdExecuteCommands *args)
{
   VKR_CMD_CALL(CmdExecuteCommands, args, args->commandBufferCount,
                args->pCommandBuffers);
}

static void
vkr_dispatch_vkCmdSetDeviceMask(UNUSED struct vn_dispatch_context *dispatch,
                                struct vn_command_vkCmdSetDeviceMask *args)
{
   VKR_CMD_CALL(CmdSetDeviceMask, args, args->deviceMask);
}

static void
vkr_dispatch_vkCmdDispatchBase(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdDispatchBase *args)
{
   VKR_CMD_CALL(CmdDispatchBase, args, args->baseGroupX, args->baseGroupY,
                args->baseGroupZ, args->groupCountX, args->groupCountY,
                args->groupCountZ);
}

static void
vkr_dispatch_vkCmdBeginRenderPass2(UNUSED struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkCmdBeginRenderPass2 *args)
{
   if (vkr_winehua_capture_trace_enabled() && vkr_winehua_capture_trace_allow()) {
      struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
      const VkRenderPassBeginInfo *begin = args->pRenderPassBegin;
      const struct vkr_render_pass *pass =
         begin ? vkr_render_pass_from_handle(begin->renderPass) : NULL;
      const struct vkr_framebuffer *fb =
         begin ? vkr_framebuffer_from_handle(begin->framebuffer) : NULL;
      vkr_log("WineHuaCapture: begin-render-pass2 cmdId=%" PRIu64
              " hostCmd=0x%" PRIxPTR " passId=%" PRIu64 " hostPass=0x%" PRIxPTR
              " framebufferId=%" PRIu64 " hostFramebuffer=0x%" PRIxPTR
              " area=%d,%d %ux%u clearValues=%u",
              cmd ? cmd->base.id : 0,
              cmd ? (uintptr_t)cmd->base.handle.command_buffer : 0,
              pass ? pass->base.id : 0,
              pass ? (uintptr_t)pass->base.handle.render_pass : 0,
              fb ? fb->base.id : 0,
              fb ? (uintptr_t)fb->base.handle.framebuffer : 0,
              begin ? begin->renderArea.offset.x : 0,
              begin ? begin->renderArea.offset.y : 0,
              begin ? begin->renderArea.extent.width : 0,
              begin ? begin->renderArea.extent.height : 0,
              begin ? begin->clearValueCount : 0);
   }
   VKR_CMD_CALL(CmdBeginRenderPass2, args, args->pRenderPassBegin,
                args->pSubpassBeginInfo);
}

static void
vkr_dispatch_vkCmdNextSubpass2(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdNextSubpass2 *args)
{
   VKR_CMD_CALL(CmdNextSubpass2, args, args->pSubpassBeginInfo, args->pSubpassEndInfo);
}

static void
vkr_dispatch_vkCmdEndRenderPass2(UNUSED struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkCmdEndRenderPass2 *args)
{
   VKR_CMD_CALL(CmdEndRenderPass2, args, args->pSubpassEndInfo);
}

static void
vkr_dispatch_vkCmdDrawIndirectCount(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCmdDrawIndirectCount *args)
{
   VKR_CMD_CALL(CmdDrawIndirectCount, args, args->buffer, args->offset, args->countBuffer,
                args->countBufferOffset, args->maxDrawCount, args->stride);
}

static void
vkr_dispatch_vkCmdDrawIndexedIndirectCount(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdDrawIndexedIndirectCount *args)
{
   VKR_CMD_CALL(CmdDrawIndexedIndirectCount, args, args->buffer, args->offset,
                args->countBuffer, args->countBufferOffset, args->maxDrawCount,
                args->stride);
}

static void
vkr_dispatch_vkCmdSetLineStipple(UNUSED struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkCmdSetLineStipple *args)
{
   VKR_CMD_CALL(CmdSetLineStipple, args, args->lineStippleFactor,
                args->lineStipplePattern);
}

static void
vkr_dispatch_vkCmdBindTransformFeedbackBuffersEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdBindTransformFeedbackBuffersEXT *args)
{
   VKR_CMD_CALL(CmdBindTransformFeedbackBuffersEXT, args, args->firstBinding,
                args->bindingCount, args->pBuffers, args->pOffsets, args->pSizes);
}

static void
vkr_dispatch_vkCmdBeginTransformFeedbackEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdBeginTransformFeedbackEXT *args)
{
   VKR_CMD_CALL(CmdBeginTransformFeedbackEXT, args, args->firstCounterBuffer,
                args->counterBufferCount, args->pCounterBuffers,
                args->pCounterBufferOffsets);
}

static void
vkr_dispatch_vkCmdEndTransformFeedbackEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdEndTransformFeedbackEXT *args)
{
   VKR_CMD_CALL(CmdEndTransformFeedbackEXT, args, args->firstCounterBuffer,
                args->counterBufferCount, args->pCounterBuffers,
                args->pCounterBufferOffsets);
}

static void
vkr_dispatch_vkCmdBeginQueryIndexedEXT(UNUSED struct vn_dispatch_context *dispatch,
                                       struct vn_command_vkCmdBeginQueryIndexedEXT *args)
{
   VKR_CMD_CALL(CmdBeginQueryIndexedEXT, args, args->queryPool, args->query, args->flags,
                args->index);
}

static void
vkr_dispatch_vkCmdEndQueryIndexedEXT(UNUSED struct vn_dispatch_context *dispatch,
                                     struct vn_command_vkCmdEndQueryIndexedEXT *args)
{
   VKR_CMD_CALL(CmdEndQueryIndexedEXT, args, args->queryPool, args->query, args->index);
}

static void
vkr_dispatch_vkCmdDrawIndirectByteCountEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdDrawIndirectByteCountEXT *args)
{
   VKR_CMD_CALL(CmdDrawIndirectByteCountEXT, args, args->instanceCount,
                args->firstInstance, args->counterBuffer, args->counterBufferOffset,
                args->counterOffset, args->vertexStride);
}

static void
vkr_dispatch_vkCmdBindVertexBuffers2(UNUSED struct vn_dispatch_context *dispatch,
                                     struct vn_command_vkCmdBindVertexBuffers2 *args)
{
   VKR_CMD_CALL(CmdBindVertexBuffers2, args, args->firstBinding, args->bindingCount,
                args->pBuffers, args->pOffsets, args->pSizes, args->pStrides);
}

static void
vkr_dispatch_vkCmdBindIndexBuffer2(UNUSED struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkCmdBindIndexBuffer2 *args)
{
   VKR_CMD_CALL(CmdBindIndexBuffer2, args, args->buffer, args->offset, args->size,
                args->indexType);
}

static void
vkr_dispatch_vkCmdSetCullMode(UNUSED struct vn_dispatch_context *dispatch,
                              struct vn_command_vkCmdSetCullMode *args)
{
   VKR_CMD_CALL(CmdSetCullMode, args, args->cullMode);
}

static void
vkr_dispatch_vkCmdSetDepthBoundsTestEnable(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetDepthBoundsTestEnable *args)
{
   VKR_CMD_CALL(CmdSetDepthBoundsTestEnable, args, args->depthBoundsTestEnable);
}

static void
vkr_dispatch_vkCmdSetDepthCompareOp(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCmdSetDepthCompareOp *args)
{
   VKR_CMD_CALL(CmdSetDepthCompareOp, args, args->depthCompareOp);
}

static void
vkr_dispatch_vkCmdSetDepthTestEnable(UNUSED struct vn_dispatch_context *dispatch,
                                     struct vn_command_vkCmdSetDepthTestEnable *args)
{
   VKR_CMD_CALL(CmdSetDepthTestEnable, args, args->depthTestEnable);
}

static void
vkr_dispatch_vkCmdSetDepthWriteEnable(UNUSED struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkCmdSetDepthWriteEnable *args)
{
   VKR_CMD_CALL(CmdSetDepthWriteEnable, args, args->depthWriteEnable);
}

static void
vkr_dispatch_vkCmdSetFrontFace(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdSetFrontFace *args)
{
   VKR_CMD_CALL(CmdSetFrontFace, args, args->frontFace);
}

static void
vkr_dispatch_vkCmdSetPrimitiveTopology(UNUSED struct vn_dispatch_context *dispatch,
                                       struct vn_command_vkCmdSetPrimitiveTopology *args)
{
   VKR_CMD_CALL(CmdSetPrimitiveTopology, args, args->primitiveTopology);
}

static void
vkr_dispatch_vkCmdSetScissorWithCount(UNUSED struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkCmdSetScissorWithCount *args)
{
   if (vkr_winehua_viewport_trace_enabled() && args->pScissors &&
       vkr_winehua_viewport_trace_allow()) {
      const struct vkr_command_buffer *cmd =
         vkr_command_buffer_from_handle(args->commandBuffer);
      for (uint32_t i = 0; i < args->scissorCount; i++) {
         const VkRect2D *scissor = &args->pScissors[i];
         vkr_log("WineHuaViewportHost: type=scissor cmdId=%" PRIu64
                 " count=%u index=%u value=%d,%d,%u,%u",
                 cmd ? cmd->base.id : 0, args->scissorCount, i,
                 scissor->offset.x, scissor->offset.y,
                 scissor->extent.width, scissor->extent.height);
      }
   }
   VKR_CMD_CALL(CmdSetScissorWithCount, args, args->scissorCount, args->pScissors);
}

static void
vkr_dispatch_vkCmdSetStencilOp(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdSetStencilOp *args)
{
   VKR_CMD_CALL(CmdSetStencilOp, args, args->faceMask, args->failOp, args->passOp,
                args->depthFailOp, args->compareOp);
}

static void
vkr_dispatch_vkCmdSetStencilTestEnable(UNUSED struct vn_dispatch_context *dispatch,
                                       struct vn_command_vkCmdSetStencilTestEnable *args)
{
   VKR_CMD_CALL(CmdSetStencilTestEnable, args, args->stencilTestEnable);
}

static void
vkr_dispatch_vkCmdSetViewportWithCount(UNUSED struct vn_dispatch_context *dispatch,
                                       struct vn_command_vkCmdSetViewportWithCount *args)
{
   if (vkr_winehua_viewport_trace_enabled() && args->pViewports &&
       vkr_winehua_viewport_trace_allow()) {
      const struct vkr_command_buffer *cmd =
         vkr_command_buffer_from_handle(args->commandBuffer);
      for (uint32_t i = 0; i < args->viewportCount; i++) {
         const VkViewport *viewport = &args->pViewports[i];
         vkr_log("WineHuaViewportHost: type=viewport cmdId=%" PRIu64
                 " count=%u index=%u value=%g,%g,%g,%g,%g,%g",
                 cmd ? cmd->base.id : 0, args->viewportCount, i,
                 viewport->x, viewport->y,
                 viewport->width, viewport->height,
                 viewport->minDepth, viewport->maxDepth);
      }
   }
   VKR_CMD_CALL(CmdSetViewportWithCount, args, args->viewportCount, args->pViewports);
}

static void
vkr_dispatch_vkCmdSetDepthBiasEnable(UNUSED struct vn_dispatch_context *dispatch,
                                     struct vn_command_vkCmdSetDepthBiasEnable *args)
{
   VKR_CMD_CALL(CmdSetDepthBiasEnable, args, args->depthBiasEnable);
}

static void
vkr_dispatch_vkCmdSetLogicOpEXT(UNUSED struct vn_dispatch_context *dispatch,
                                struct vn_command_vkCmdSetLogicOpEXT *args)
{
   VKR_CMD_CALL(CmdSetLogicOpEXT, args, args->logicOp);
}

static void
vkr_dispatch_vkCmdSetPatchControlPointsEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetPatchControlPointsEXT *args)
{
   VKR_CMD_CALL(CmdSetPatchControlPointsEXT, args, args->patchControlPoints);
}

static void
vkr_dispatch_vkCmdSetPrimitiveRestartEnable(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetPrimitiveRestartEnable *args)
{
   VKR_CMD_CALL(CmdSetPrimitiveRestartEnable, args, args->primitiveRestartEnable);
}

static void
vkr_dispatch_vkCmdSetRasterizerDiscardEnable(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetRasterizerDiscardEnable *args)
{
   VKR_CMD_CALL(CmdSetRasterizerDiscardEnable, args, args->rasterizerDiscardEnable);
}

static void
vkr_dispatch_vkCmdBeginConditionalRenderingEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdBeginConditionalRenderingEXT *args)
{
   VKR_CMD_CALL(CmdBeginConditionalRenderingEXT, args, args->pConditionalRenderingBegin);
}

static void
vkr_dispatch_vkCmdEndConditionalRenderingEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdEndConditionalRenderingEXT *args)
{
   VKR_CMD_CALL(CmdEndConditionalRenderingEXT, args);
}

static void
vkr_dispatch_vkCmdBeginRendering(UNUSED struct vn_dispatch_context *ctx,
                                 struct vn_command_vkCmdBeginRendering *args)
{
   static atomic_uint trace_sequence;
   const char *trace_value = os_get_option("WINEHUA_VKR_TRACE_PIPELINE");

   if (trace_value && trace_value[0] == '1' && args->pRenderingInfo) {
      const unsigned sequence = atomic_fetch_add_explicit(
         &trace_sequence, 1, memory_order_relaxed);
      const VkRenderingInfo *info = args->pRenderingInfo;
      if (sequence < 1024u) {
         vkr_log("WineHuaRendering: begin seq=%u flags=0x%x area=%d,%d,%u,%u layers=%u viewMask=0x%x colors=%u depth=%p stencil=%p",
                 sequence, info->flags, info->renderArea.offset.x,
                 info->renderArea.offset.y, info->renderArea.extent.width,
                 info->renderArea.extent.height, info->layerCount,
                 info->viewMask, info->colorAttachmentCount,
                 (const void *)info->pDepthAttachment,
                 (const void *)info->pStencilAttachment);
         for (uint32_t i = 0; i < info->colorAttachmentCount; i++) {
            const VkRenderingAttachmentInfo *attachment =
               &info->pColorAttachments[i];
            const struct vkr_image_view *view =
               attachment->imageView
                  ? vkr_image_view_from_handle(attachment->imageView) : NULL;
            const struct vkr_image_view *resolve =
               attachment->resolveImageView
                  ? vkr_image_view_from_handle(attachment->resolveImageView) : NULL;
            vkr_log("WineHuaRendering: begin seq=%u color[%u] viewId=%" PRIu64 " hostView=0x%" PRIxPTR " layout=%u resolveMode=0x%x resolveViewId=%" PRIu64 " hostResolveView=0x%" PRIxPTR " resolveLayout=%u load=%u store=%u",
                    sequence, i, view ? view->base.id : 0,
                    view ? (uintptr_t)view->base.handle.image_view : 0,
                    attachment->imageLayout, attachment->resolveMode,
                    resolve ? resolve->base.id : 0,
                    resolve ? (uintptr_t)resolve->base.handle.image_view : 0,
                    attachment->resolveImageLayout, attachment->loadOp,
                    attachment->storeOp);
         }
         if (info->pDepthAttachment) {
            const VkRenderingAttachmentInfo *attachment = info->pDepthAttachment;
            const struct vkr_image_view *view =
               attachment->imageView
                  ? vkr_image_view_from_handle(attachment->imageView) : NULL;
            vkr_log("WineHuaRendering: begin seq=%u depth viewId=%" PRIu64 " hostView=0x%" PRIxPTR " layout=%u resolveMode=0x%x load=%u store=%u",
                    sequence, view ? view->base.id : 0,
                    view ? (uintptr_t)view->base.handle.image_view : 0,
                    attachment->imageLayout, attachment->resolveMode,
                    attachment->loadOp, attachment->storeOp);
         }
         if (info->pStencilAttachment &&
             info->pStencilAttachment != info->pDepthAttachment) {
            const VkRenderingAttachmentInfo *attachment = info->pStencilAttachment;
            const struct vkr_image_view *view =
               attachment->imageView
                  ? vkr_image_view_from_handle(attachment->imageView) : NULL;
            vkr_log("WineHuaRendering: begin seq=%u stencil viewId=%" PRIu64 " hostView=0x%" PRIxPTR " layout=%u resolveMode=0x%x load=%u store=%u",
                    sequence, view ? view->base.id : 0,
                    view ? (uintptr_t)view->base.handle.image_view : 0,
                    attachment->imageLayout, attachment->resolveMode,
                    attachment->loadOp, attachment->storeOp);
         }
      }
   }
   VKR_CMD_CALL(CmdBeginRendering, args, args->pRenderingInfo);
}

static void
vkr_dispatch_vkCmdEndRendering(UNUSED struct vn_dispatch_context *ctx,
                               struct vn_command_vkCmdEndRendering *args)
{
   VKR_CMD_CALL(CmdEndRendering, args);
}

static void
vkr_dispatch_vkCmdPipelineBarrier2(UNUSED struct vn_dispatch_context *ctx,
                                   struct vn_command_vkCmdPipelineBarrier2 *args)
{
   VKR_CMD_CALL(CmdPipelineBarrier2, args, args->pDependencyInfo);
}

static void
vkr_dispatch_vkCmdResetEvent2(UNUSED struct vn_dispatch_context *ctx,
                              struct vn_command_vkCmdResetEvent2 *args)
{
   VKR_CMD_CALL(CmdResetEvent2, args, args->event, args->stageMask);
}

static void
vkr_dispatch_vkCmdSetEvent2(UNUSED struct vn_dispatch_context *ctx,
                            struct vn_command_vkCmdSetEvent2 *args)
{
   VKR_CMD_CALL(CmdSetEvent2, args, args->event, args->pDependencyInfo);
}

static void
vkr_dispatch_vkCmdWaitEvents2(UNUSED struct vn_dispatch_context *ctx,
                              struct vn_command_vkCmdWaitEvents2 *args)
{
   VKR_CMD_CALL(CmdWaitEvents2, args, args->eventCount, args->pEvents,
                args->pDependencyInfos);
}

static void
vkr_dispatch_vkCmdWriteTimestamp2(UNUSED struct vn_dispatch_context *ctx,
                                  struct vn_command_vkCmdWriteTimestamp2 *args)
{
   VKR_CMD_CALL(CmdWriteTimestamp2, args, args->stage, args->queryPool, args->query);
}

static void
vkr_dispatch_vkCmdDrawMultiEXT(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdDrawMultiEXT *args)
{
   VKR_CMD_CALL(CmdDrawMultiEXT, args, args->drawCount, args->pVertexInfo,
                args->instanceCount, args->firstInstance, args->stride);
}

static void
vkr_dispatch_vkCmdDrawMultiIndexedEXT(UNUSED struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkCmdDrawMultiIndexedEXT *args)
{
   VKR_CMD_CALL(CmdDrawMultiIndexedEXT, args, args->drawCount, args->pIndexInfo,
                args->instanceCount, args->firstInstance, args->stride,
                args->pVertexOffset);
}

static void
vkr_dispatch_vkCmdPushDescriptorSet(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCmdPushDescriptorSet *args)
{
   VKR_CMD_CALL(CmdPushDescriptorSet, args, args->pipelineBindPoint, args->layout,
                args->set, args->descriptorWriteCount, args->pDescriptorWrites);
}

static void
vkr_dispatch_vkCmdSetColorWriteEnableEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetColorWriteEnableEXT *args)
{
   VKR_CMD_CALL(CmdSetColorWriteEnableEXT, args, args->attachmentCount,
                args->pColorWriteEnables);
}

static void
vkr_dispatch_vkCmdSetVertexInputEXT(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCmdSetVertexInputEXT *args)
{
   VKR_CMD_CALL(CmdSetVertexInputEXT, args, args->vertexBindingDescriptionCount,
                args->pVertexBindingDescriptions, args->vertexAttributeDescriptionCount,
                args->pVertexAttributeDescriptions);
}

static void
vkr_dispatch_vkCmdSetAlphaToCoverageEnableEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetAlphaToCoverageEnableEXT *args)
{
   VKR_CMD_CALL(CmdSetAlphaToCoverageEnableEXT, args, args->alphaToCoverageEnable);
}

static void
vkr_dispatch_vkCmdSetAlphaToOneEnableEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetAlphaToOneEnableEXT *args)
{
   VKR_CMD_CALL(CmdSetAlphaToOneEnableEXT, args, args->alphaToOneEnable);
}

static void
vkr_dispatch_vkCmdSetColorBlendAdvancedEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetColorBlendAdvancedEXT *args)
{
   VKR_CMD_CALL(CmdSetColorBlendAdvancedEXT, args, args->firstAttachment,
                args->attachmentCount, args->pColorBlendAdvanced);
}

static void
vkr_dispatch_vkCmdSetColorBlendEnableEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetColorBlendEnableEXT *args)
{
   VKR_CMD_CALL(CmdSetColorBlendEnableEXT, args, args->firstAttachment,
                args->attachmentCount, args->pColorBlendEnables);
}

static void
vkr_dispatch_vkCmdSetColorBlendEquationEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetColorBlendEquationEXT *args)
{
   static atomic_uint trace_sequence;
   const char *trace_value = os_get_option("WINEHUA_VKR_TRACE_PIPELINE");

   if (trace_value && trace_value[0] == '1' && args->pColorBlendEquations) {
      const unsigned sequence = atomic_fetch_add_explicit(
         &trace_sequence, 1, memory_order_relaxed);
      for (uint32_t i = 0; i < args->attachmentCount; i++) {
         const VkColorBlendEquationEXT *equation = &args->pColorBlendEquations[i];
         const bool dual_src =
            (equation->srcColorBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR &&
             equation->srcColorBlendFactor <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA) ||
            (equation->dstColorBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR &&
             equation->dstColorBlendFactor <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA) ||
            (equation->srcAlphaBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR &&
             equation->srcAlphaBlendFactor <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA) ||
            (equation->dstAlphaBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR &&
             equation->dstAlphaBlendFactor <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA);
         if (sequence < 2048u || dual_src) {
            vkr_log("WineHuaBlendDynamic: seq=%u first=%u attachment=%u color=%u,%u,%u alpha=%u,%u,%u dualSrc=%u",
                    sequence, args->firstAttachment, i,
                    equation->srcColorBlendFactor,
                    equation->dstColorBlendFactor,
                    equation->colorBlendOp,
                    equation->srcAlphaBlendFactor,
                    equation->dstAlphaBlendFactor,
                    equation->alphaBlendOp, dual_src ? 1u : 0u);
         }
      }
   }
   VKR_CMD_CALL(CmdSetColorBlendEquationEXT, args, args->firstAttachment,
                args->attachmentCount, args->pColorBlendEquations);
}

static void
vkr_dispatch_vkCmdSetColorWriteMaskEXT(UNUSED struct vn_dispatch_context *dispatch,
                                       struct vn_command_vkCmdSetColorWriteMaskEXT *args)
{
   VKR_CMD_CALL(CmdSetColorWriteMaskEXT, args, args->firstAttachment,
                args->attachmentCount, args->pColorWriteMasks);
}

static void
vkr_dispatch_vkCmdSetConservativeRasterizationModeEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetConservativeRasterizationModeEXT *args)
{
   VKR_CMD_CALL(CmdSetConservativeRasterizationModeEXT, args,
                args->conservativeRasterizationMode);
}

static void
vkr_dispatch_vkCmdSetDepthClampEnableEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetDepthClampEnableEXT *args)
{
   VKR_CMD_CALL(CmdSetDepthClampEnableEXT, args, args->depthClampEnable);
}

static void
vkr_dispatch_vkCmdSetDepthClipEnableEXT(UNUSED struct vn_dispatch_context *dispatch,
                                        struct vn_command_vkCmdSetDepthClipEnableEXT *args)
{
   VKR_CMD_CALL(CmdSetDepthClipEnableEXT, args, args->depthClipEnable);
}

static void
vkr_dispatch_vkCmdSetDepthClipNegativeOneToOneEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetDepthClipNegativeOneToOneEXT *args)
{
   VKR_CMD_CALL(CmdSetDepthClipNegativeOneToOneEXT, args, args->negativeOneToOne);
}

static void
vkr_dispatch_vkCmdSetExtraPrimitiveOverestimationSizeEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetExtraPrimitiveOverestimationSizeEXT *args)
{
   VKR_CMD_CALL(CmdSetExtraPrimitiveOverestimationSizeEXT, args,
                args->extraPrimitiveOverestimationSize);
}

static void
vkr_dispatch_vkCmdSetLineRasterizationModeEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetLineRasterizationModeEXT *args)
{
   VKR_CMD_CALL(CmdSetLineRasterizationModeEXT, args, args->lineRasterizationMode);
}

static void
vkr_dispatch_vkCmdSetLineStippleEnableEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetLineStippleEnableEXT *args)
{
   VKR_CMD_CALL(CmdSetLineStippleEnableEXT, args, args->stippledLineEnable);
}

static void
vkr_dispatch_vkCmdSetLogicOpEnableEXT(UNUSED struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkCmdSetLogicOpEnableEXT *args)
{
   VKR_CMD_CALL(CmdSetLogicOpEnableEXT, args, args->logicOpEnable);
}

static void
vkr_dispatch_vkCmdSetPolygonModeEXT(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCmdSetPolygonModeEXT *args)
{
   VKR_CMD_CALL(CmdSetPolygonModeEXT, args, args->polygonMode);
}

static void
vkr_dispatch_vkCmdSetProvokingVertexModeEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetProvokingVertexModeEXT *args)
{
   VKR_CMD_CALL(CmdSetProvokingVertexModeEXT, args, args->provokingVertexMode);
}

static void
vkr_dispatch_vkCmdSetRasterizationSamplesEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetRasterizationSamplesEXT *args)
{
   VKR_CMD_CALL(CmdSetRasterizationSamplesEXT, args, args->rasterizationSamples);
}

static void
vkr_dispatch_vkCmdSetRasterizationStreamEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetRasterizationStreamEXT *args)
{
   VKR_CMD_CALL(CmdSetRasterizationStreamEXT, args, args->rasterizationStream);
}

static void
vkr_dispatch_vkCmdSetSampleLocationsEnableEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetSampleLocationsEnableEXT *args)
{
   VKR_CMD_CALL(CmdSetSampleLocationsEnableEXT, args, args->sampleLocationsEnable);
}

static void
vkr_dispatch_vkCmdSetSampleMaskEXT(UNUSED struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkCmdSetSampleMaskEXT *args)
{
   VKR_CMD_CALL(CmdSetSampleMaskEXT, args, args->samples, args->pSampleMask);
}

static void
vkr_dispatch_vkCmdSetTessellationDomainOriginEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetTessellationDomainOriginEXT *args)
{
   VKR_CMD_CALL(CmdSetTessellationDomainOriginEXT, args, args->domainOrigin);
}

static void
vkr_dispatch_vkCmdSetFragmentShadingRateKHR(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetFragmentShadingRateKHR *args)
{
   VKR_CMD_CALL(CmdSetFragmentShadingRateKHR, args, args->pFragmentSize,
                args->combinerOps);
}

static void
vkr_dispatch_vkCmdSetSampleLocationsEXT(UNUSED struct vn_dispatch_context *dispatch,
                                        struct vn_command_vkCmdSetSampleLocationsEXT *args)
{
   VKR_CMD_CALL(CmdSetSampleLocationsEXT, args, args->pSampleLocationsInfo);
}

static void
vkr_dispatch_vkCmdSetRenderingAttachmentLocations(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetRenderingAttachmentLocations *args)
{
   VKR_CMD_CALL(CmdSetRenderingAttachmentLocations, args, args->pLocationInfo);
}

static void
vkr_dispatch_vkCmdSetRenderingInputAttachmentIndices(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetRenderingInputAttachmentIndices *args)
{
   VKR_CMD_CALL(CmdSetRenderingInputAttachmentIndices, args,
                args->pInputAttachmentIndexInfo);
}

static void
vkr_dispatch_vkCmdBindDescriptorSets2(UNUSED struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkCmdBindDescriptorSets2 *args)
{
   VKR_CMD_CALL(CmdBindDescriptorSets2, args, args->pBindDescriptorSetsInfo);
}

static void
vkr_dispatch_vkCmdPushConstants2(UNUSED struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkCmdPushConstants2 *args)
{
   VKR_CMD_CALL(CmdPushConstants2, args, args->pPushConstantsInfo);
}

static void
vkr_dispatch_vkCmdPushDescriptorSet2(UNUSED struct vn_dispatch_context *dispatch,
                                     struct vn_command_vkCmdPushDescriptorSet2 *args)
{
   VKR_CMD_CALL(CmdPushDescriptorSet2, args, args->pPushDescriptorSetInfo);
}

static void
vkr_dispatch_vkCmdBuildAccelerationStructuresIndirectKHR(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdBuildAccelerationStructuresIndirectKHR *args)
{
   VKR_CMD_CALL(CmdBuildAccelerationStructuresIndirectKHR, args, args->infoCount,
                args->pInfos, args->pIndirectDeviceAddresses, args->pIndirectStrides,
                args->ppMaxPrimitiveCounts);
}

static void
vkr_dispatch_vkCmdBuildAccelerationStructuresKHR(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdBuildAccelerationStructuresKHR *args)
{
   VKR_CMD_CALL(CmdBuildAccelerationStructuresKHR, args, args->infoCount, args->pInfos,
                args->ppBuildRangeInfos);
}

static void
vkr_dispatch_vkCmdCopyAccelerationStructureKHR(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdCopyAccelerationStructureKHR *args)
{
   VKR_CMD_CALL(CmdCopyAccelerationStructureKHR, args, args->pInfo);
}

static void
vkr_dispatch_vkCmdCopyAccelerationStructureToMemoryKHR(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdCopyAccelerationStructureToMemoryKHR *args)
{
   VKR_CMD_CALL(CmdCopyAccelerationStructureToMemoryKHR, args, args->pInfo);
}

static void
vkr_dispatch_vkCmdCopyMemoryToAccelerationStructureKHR(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdCopyMemoryToAccelerationStructureKHR *args)
{
   VKR_CMD_CALL(CmdCopyMemoryToAccelerationStructureKHR, args, args->pInfo);
}

static void
vkr_dispatch_vkCmdWriteAccelerationStructuresPropertiesKHR(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdWriteAccelerationStructuresPropertiesKHR *args)
{
   VKR_CMD_CALL(CmdWriteAccelerationStructuresPropertiesKHR, args,
                args->accelerationStructureCount, args->pAccelerationStructures,
                args->queryType, args->queryPool, args->firstQuery);
}

static void
vkr_dispatch_vkCmdSetRayTracingPipelineStackSizeKHR(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetRayTracingPipelineStackSizeKHR *args)
{
   VKR_CMD_CALL(CmdSetRayTracingPipelineStackSizeKHR, args, args->pipelineStackSize);
}

static void
vkr_dispatch_vkCmdTraceRaysIndirectKHR(UNUSED struct vn_dispatch_context *dispatch,
                                       struct vn_command_vkCmdTraceRaysIndirectKHR *args)
{
   VKR_CMD_CALL(CmdTraceRaysIndirectKHR, args, args->pRaygenShaderBindingTable,
                args->pMissShaderBindingTable, args->pHitShaderBindingTable,
                args->pCallableShaderBindingTable, args->indirectDeviceAddress);
}

static void
vkr_dispatch_vkCmdTraceRaysKHR(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdTraceRaysKHR *args)
{
   VKR_CMD_CALL(CmdTraceRaysKHR, args, args->pRaygenShaderBindingTable,
                args->pMissShaderBindingTable, args->pHitShaderBindingTable,
                args->pCallableShaderBindingTable, args->width, args->height,
                args->depth);
}

static void
vkr_dispatch_vkCmdTraceRaysIndirect2KHR(UNUSED struct vn_dispatch_context *dispatch,
                                        struct vn_command_vkCmdTraceRaysIndirect2KHR *args)
{
   VKR_CMD_CALL(CmdTraceRaysIndirect2KHR, args, args->indirectDeviceAddress);
}

static void
vkr_dispatch_vkCmdSetDepthBias2EXT(UNUSED struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkCmdSetDepthBias2EXT *args)
{
   VKR_CMD_CALL(CmdSetDepthBias2EXT, args, args->pDepthBiasInfo);
}

static void
vkr_dispatch_vkCmdSetDepthClampRangeEXT(UNUSED struct vn_dispatch_context *dispatch,
                                        struct vn_command_vkCmdSetDepthClampRangeEXT *args)
{
   VKR_CMD_CALL(CmdSetDepthClampRangeEXT, args, args->depthClampMode,
                args->pDepthClampRange);
}

static void
vkr_dispatch_vkCmdSetAttachmentFeedbackLoopEnableEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetAttachmentFeedbackLoopEnableEXT *args)
{
   VKR_CMD_CALL(CmdSetAttachmentFeedbackLoopEnableEXT, args, args->aspectMask);
}

static void
vkr_dispatch_vkCmdDrawMeshTasksEXT(UNUSED struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkCmdDrawMeshTasksEXT *args)
{
   VKR_CMD_CALL(CmdDrawMeshTasksEXT, args, args->groupCountX, args->groupCountY,
                args->groupCountZ);
}

static void
vkr_dispatch_vkCmdDrawMeshTasksIndirectEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdDrawMeshTasksIndirectEXT *args)
{
   VKR_CMD_CALL(CmdDrawMeshTasksIndirectEXT, args, args->buffer, args->offset,
                args->drawCount, args->stride);
}

static void
vkr_dispatch_vkCmdDrawMeshTasksIndirectCountEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdDrawMeshTasksIndirectCountEXT *args)
{
   VKR_CMD_CALL(CmdDrawMeshTasksIndirectCountEXT, args, args->buffer, args->offset,
                args->countBuffer, args->countBufferOffset, args->maxDrawCount,
                args->stride);
}

static void
vkr_dispatch_vkCmdBindSamplerHeapEXT(UNUSED struct vn_dispatch_context *dispatch,
                                     struct vn_command_vkCmdBindSamplerHeapEXT *args)
{
   VKR_CMD_CALL(CmdBindSamplerHeapEXT, args, args->pBindInfo);
}

static void
vkr_dispatch_vkCmdBindResourceHeapEXT(UNUSED struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkCmdBindResourceHeapEXT *args)
{
   VKR_CMD_CALL(CmdBindResourceHeapEXT, args, args->pBindInfo);
}

static void
vkr_dispatch_vkCmdPushDataEXT(UNUSED struct vn_dispatch_context *dispatch,
                              struct vn_command_vkCmdPushDataEXT *args)
{
   VKR_CMD_CALL(CmdPushDataEXT, args, args->pPushDataInfo);
}

void
vkr_context_init_command_pool_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateCommandPool = vkr_dispatch_vkCreateCommandPool;
   dispatch->dispatch_vkDestroyCommandPool = vkr_dispatch_vkDestroyCommandPool;
   dispatch->dispatch_vkResetCommandPool = vkr_dispatch_vkResetCommandPool;
   dispatch->dispatch_vkTrimCommandPool = vkr_dispatch_vkTrimCommandPool;
}

void
vkr_context_init_command_buffer_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkAllocateCommandBuffers = vkr_dispatch_vkAllocateCommandBuffers;
   dispatch->dispatch_vkFreeCommandBuffers = vkr_dispatch_vkFreeCommandBuffers;
   dispatch->dispatch_vkResetCommandBuffer = vkr_dispatch_vkResetCommandBuffer;
   dispatch->dispatch_vkBeginCommandBuffer = vkr_dispatch_vkBeginCommandBuffer;
   dispatch->dispatch_vkEndCommandBuffer = vkr_dispatch_vkEndCommandBuffer;

   dispatch->dispatch_vkCmdBindPipeline = vkr_dispatch_vkCmdBindPipeline;
   dispatch->dispatch_vkCmdSetViewport = vkr_dispatch_vkCmdSetViewport;
   dispatch->dispatch_vkCmdSetScissor = vkr_dispatch_vkCmdSetScissor;
   dispatch->dispatch_vkCmdSetLineWidth = vkr_dispatch_vkCmdSetLineWidth;
   dispatch->dispatch_vkCmdSetDepthBias = vkr_dispatch_vkCmdSetDepthBias;
   dispatch->dispatch_vkCmdSetBlendConstants = vkr_dispatch_vkCmdSetBlendConstants;
   dispatch->dispatch_vkCmdSetDepthBounds = vkr_dispatch_vkCmdSetDepthBounds;
   dispatch->dispatch_vkCmdSetStencilCompareMask =
      vkr_dispatch_vkCmdSetStencilCompareMask;
   dispatch->dispatch_vkCmdSetStencilWriteMask = vkr_dispatch_vkCmdSetStencilWriteMask;
   dispatch->dispatch_vkCmdSetStencilReference = vkr_dispatch_vkCmdSetStencilReference;
   dispatch->dispatch_vkCmdBindDescriptorSets = vkr_dispatch_vkCmdBindDescriptorSets;
   dispatch->dispatch_vkCmdBindIndexBuffer = vkr_dispatch_vkCmdBindIndexBuffer;
   dispatch->dispatch_vkCmdBindVertexBuffers = vkr_dispatch_vkCmdBindVertexBuffers;
   dispatch->dispatch_vkCmdDraw = vkr_dispatch_vkCmdDraw;
   dispatch->dispatch_vkCmdDrawIndexed = vkr_dispatch_vkCmdDrawIndexed;
   dispatch->dispatch_vkCmdDrawIndirect = vkr_dispatch_vkCmdDrawIndirect;
   dispatch->dispatch_vkCmdDrawIndexedIndirect = vkr_dispatch_vkCmdDrawIndexedIndirect;
   dispatch->dispatch_vkCmdDispatch = vkr_dispatch_vkCmdDispatch;
   dispatch->dispatch_vkCmdDispatchIndirect = vkr_dispatch_vkCmdDispatchIndirect;
   dispatch->dispatch_vkCmdCopyBuffer = vkr_dispatch_vkCmdCopyBuffer;
   dispatch->dispatch_vkCmdCopyBuffer2 = vkr_dispatch_vkCmdCopyBuffer2;
   dispatch->dispatch_vkCmdCopyImage = vkr_dispatch_vkCmdCopyImage;
   dispatch->dispatch_vkCmdCopyImage2 = vkr_dispatch_vkCmdCopyImage2;
   dispatch->dispatch_vkCmdBlitImage = vkr_dispatch_vkCmdBlitImage;
   dispatch->dispatch_vkCmdBlitImage2 = vkr_dispatch_vkCmdBlitImage2;
   dispatch->dispatch_vkCmdCopyBufferToImage = vkr_dispatch_vkCmdCopyBufferToImage;
   dispatch->dispatch_vkCmdCopyBufferToImage2 = vkr_dispatch_vkCmdCopyBufferToImage2;
   dispatch->dispatch_vkCmdCopyImageToBuffer = vkr_dispatch_vkCmdCopyImageToBuffer;
   dispatch->dispatch_vkCmdCopyImageToBuffer2 = vkr_dispatch_vkCmdCopyImageToBuffer2;
   dispatch->dispatch_vkCmdUpdateBuffer = vkr_dispatch_vkCmdUpdateBuffer;
   dispatch->dispatch_vkCmdFillBuffer = vkr_dispatch_vkCmdFillBuffer;
   dispatch->dispatch_vkCmdClearColorImage = vkr_dispatch_vkCmdClearColorImage;
   dispatch->dispatch_vkCmdClearDepthStencilImage =
      vkr_dispatch_vkCmdClearDepthStencilImage;
   dispatch->dispatch_vkCmdClearAttachments = vkr_dispatch_vkCmdClearAttachments;
   dispatch->dispatch_vkCmdResolveImage = vkr_dispatch_vkCmdResolveImage;
   dispatch->dispatch_vkCmdResolveImage2 = vkr_dispatch_vkCmdResolveImage2;
   dispatch->dispatch_vkCmdSetEvent = vkr_dispatch_vkCmdSetEvent;
   dispatch->dispatch_vkCmdResetEvent = vkr_dispatch_vkCmdResetEvent;
   dispatch->dispatch_vkCmdWaitEvents = vkr_dispatch_vkCmdWaitEvents;
   dispatch->dispatch_vkCmdPipelineBarrier = vkr_dispatch_vkCmdPipelineBarrier;
   dispatch->dispatch_vkCmdBeginQuery = vkr_dispatch_vkCmdBeginQuery;
   dispatch->dispatch_vkCmdEndQuery = vkr_dispatch_vkCmdEndQuery;
   dispatch->dispatch_vkCmdResetQueryPool = vkr_dispatch_vkCmdResetQueryPool;
   dispatch->dispatch_vkCmdWriteTimestamp = vkr_dispatch_vkCmdWriteTimestamp;
   dispatch->dispatch_vkCmdCopyQueryPoolResults = vkr_dispatch_vkCmdCopyQueryPoolResults;
   dispatch->dispatch_vkCmdPushConstants = vkr_dispatch_vkCmdPushConstants;
   dispatch->dispatch_vkCmdBeginRenderPass = vkr_dispatch_vkCmdBeginRenderPass;
   dispatch->dispatch_vkCmdNextSubpass = vkr_dispatch_vkCmdNextSubpass;
   dispatch->dispatch_vkCmdEndRenderPass = vkr_dispatch_vkCmdEndRenderPass;
   dispatch->dispatch_vkCmdExecuteCommands = vkr_dispatch_vkCmdExecuteCommands;
   dispatch->dispatch_vkCmdSetDeviceMask = vkr_dispatch_vkCmdSetDeviceMask;
   dispatch->dispatch_vkCmdDispatchBase = vkr_dispatch_vkCmdDispatchBase;
   dispatch->dispatch_vkCmdBeginRenderPass2 = vkr_dispatch_vkCmdBeginRenderPass2;
   dispatch->dispatch_vkCmdNextSubpass2 = vkr_dispatch_vkCmdNextSubpass2;
   dispatch->dispatch_vkCmdEndRenderPass2 = vkr_dispatch_vkCmdEndRenderPass2;
   dispatch->dispatch_vkCmdDrawIndirectCount = vkr_dispatch_vkCmdDrawIndirectCount;
   dispatch->dispatch_vkCmdDrawIndexedIndirectCount =
      vkr_dispatch_vkCmdDrawIndexedIndirectCount;

   dispatch->dispatch_vkCmdSetLineStipple = vkr_dispatch_vkCmdSetLineStipple;

   dispatch->dispatch_vkCmdBindTransformFeedbackBuffersEXT =
      vkr_dispatch_vkCmdBindTransformFeedbackBuffersEXT;
   dispatch->dispatch_vkCmdBeginTransformFeedbackEXT =
      vkr_dispatch_vkCmdBeginTransformFeedbackEXT;
   dispatch->dispatch_vkCmdEndTransformFeedbackEXT =
      vkr_dispatch_vkCmdEndTransformFeedbackEXT;
   dispatch->dispatch_vkCmdBeginQueryIndexedEXT = vkr_dispatch_vkCmdBeginQueryIndexedEXT;
   dispatch->dispatch_vkCmdEndQueryIndexedEXT = vkr_dispatch_vkCmdEndQueryIndexedEXT;
   dispatch->dispatch_vkCmdDrawIndirectByteCountEXT =
      vkr_dispatch_vkCmdDrawIndirectByteCountEXT;

   dispatch->dispatch_vkCmdBindVertexBuffers2 = vkr_dispatch_vkCmdBindVertexBuffers2;
   dispatch->dispatch_vkCmdSetCullMode = vkr_dispatch_vkCmdSetCullMode;
   dispatch->dispatch_vkCmdSetDepthBoundsTestEnable =
      vkr_dispatch_vkCmdSetDepthBoundsTestEnable;
   dispatch->dispatch_vkCmdSetDepthCompareOp = vkr_dispatch_vkCmdSetDepthCompareOp;
   dispatch->dispatch_vkCmdSetDepthTestEnable = vkr_dispatch_vkCmdSetDepthTestEnable;
   dispatch->dispatch_vkCmdSetDepthWriteEnable = vkr_dispatch_vkCmdSetDepthWriteEnable;
   dispatch->dispatch_vkCmdSetFrontFace = vkr_dispatch_vkCmdSetFrontFace;
   dispatch->dispatch_vkCmdSetPrimitiveTopology = vkr_dispatch_vkCmdSetPrimitiveTopology;
   dispatch->dispatch_vkCmdSetScissorWithCount = vkr_dispatch_vkCmdSetScissorWithCount;
   dispatch->dispatch_vkCmdSetStencilOp = vkr_dispatch_vkCmdSetStencilOp;
   dispatch->dispatch_vkCmdSetStencilTestEnable = vkr_dispatch_vkCmdSetStencilTestEnable;
   dispatch->dispatch_vkCmdSetViewportWithCount = vkr_dispatch_vkCmdSetViewportWithCount;

   dispatch->dispatch_vkCmdBindIndexBuffer2 = vkr_dispatch_vkCmdBindIndexBuffer2;

   /* VK_KHR_dynamic_rendering */
   dispatch->dispatch_vkCmdBeginRendering = vkr_dispatch_vkCmdBeginRendering;
   dispatch->dispatch_vkCmdEndRendering = vkr_dispatch_vkCmdEndRendering;

   /* VK_KHR_synchronization2 */
   dispatch->dispatch_vkCmdPipelineBarrier2 = vkr_dispatch_vkCmdPipelineBarrier2;
   dispatch->dispatch_vkCmdResetEvent2 = vkr_dispatch_vkCmdResetEvent2;
   dispatch->dispatch_vkCmdSetEvent2 = vkr_dispatch_vkCmdSetEvent2;
   dispatch->dispatch_vkCmdWaitEvents2 = vkr_dispatch_vkCmdWaitEvents2;
   dispatch->dispatch_vkCmdWriteTimestamp2 = vkr_dispatch_vkCmdWriteTimestamp2;

   /* VK_EXT_extended_dynamic_state2 */
   dispatch->dispatch_vkCmdSetRasterizerDiscardEnable =
      vkr_dispatch_vkCmdSetRasterizerDiscardEnable;
   dispatch->dispatch_vkCmdSetPrimitiveRestartEnable =
      vkr_dispatch_vkCmdSetPrimitiveRestartEnable;
   dispatch->dispatch_vkCmdSetPatchControlPointsEXT =
      vkr_dispatch_vkCmdSetPatchControlPointsEXT;
   dispatch->dispatch_vkCmdSetLogicOpEXT = vkr_dispatch_vkCmdSetLogicOpEXT;
   dispatch->dispatch_vkCmdSetDepthBiasEnable = vkr_dispatch_vkCmdSetDepthBiasEnable;

   /* VK_EXT_conditional_rendering */
   dispatch->dispatch_vkCmdBeginConditionalRenderingEXT =
      vkr_dispatch_vkCmdBeginConditionalRenderingEXT;
   dispatch->dispatch_vkCmdEndConditionalRenderingEXT =
      vkr_dispatch_vkCmdEndConditionalRenderingEXT;

   /* VK_EXT_multi_draw */
   dispatch->dispatch_vkCmdDrawMultiEXT = vkr_dispatch_vkCmdDrawMultiEXT;
   dispatch->dispatch_vkCmdDrawMultiIndexedEXT = vkr_dispatch_vkCmdDrawMultiIndexedEXT;

   /* VK_KHR_push_descriptor */
   dispatch->dispatch_vkCmdPushDescriptorSet = vkr_dispatch_vkCmdPushDescriptorSet;
   dispatch->dispatch_vkCmdPushDescriptorSetWithTemplate = NULL;

   /* VK_EXT_color_write_enable */
   dispatch->dispatch_vkCmdSetColorWriteEnableEXT =
      vkr_dispatch_vkCmdSetColorWriteEnableEXT;

   /* VK_EXT_vertex_input_dynamic_state */
   dispatch->dispatch_vkCmdSetVertexInputEXT = vkr_dispatch_vkCmdSetVertexInputEXT;

   /* VK_EXT_extended_dynamic_state3 */
   dispatch->dispatch_vkCmdSetAlphaToCoverageEnableEXT =
      vkr_dispatch_vkCmdSetAlphaToCoverageEnableEXT;
   dispatch->dispatch_vkCmdSetAlphaToOneEnableEXT =
      vkr_dispatch_vkCmdSetAlphaToOneEnableEXT;
   dispatch->dispatch_vkCmdSetColorBlendAdvancedEXT =
      vkr_dispatch_vkCmdSetColorBlendAdvancedEXT;
   dispatch->dispatch_vkCmdSetColorBlendEnableEXT =
      vkr_dispatch_vkCmdSetColorBlendEnableEXT;
   dispatch->dispatch_vkCmdSetColorBlendEquationEXT =
      vkr_dispatch_vkCmdSetColorBlendEquationEXT;
   dispatch->dispatch_vkCmdSetColorWriteMaskEXT = vkr_dispatch_vkCmdSetColorWriteMaskEXT;
   dispatch->dispatch_vkCmdSetConservativeRasterizationModeEXT =
      vkr_dispatch_vkCmdSetConservativeRasterizationModeEXT;
   dispatch->dispatch_vkCmdSetDepthClampEnableEXT =
      vkr_dispatch_vkCmdSetDepthClampEnableEXT;
   dispatch->dispatch_vkCmdSetDepthClipEnableEXT =
      vkr_dispatch_vkCmdSetDepthClipEnableEXT;
   dispatch->dispatch_vkCmdSetDepthClipNegativeOneToOneEXT =
      vkr_dispatch_vkCmdSetDepthClipNegativeOneToOneEXT;
   dispatch->dispatch_vkCmdSetExtraPrimitiveOverestimationSizeEXT =
      vkr_dispatch_vkCmdSetExtraPrimitiveOverestimationSizeEXT;
   dispatch->dispatch_vkCmdSetLineRasterizationModeEXT =
      vkr_dispatch_vkCmdSetLineRasterizationModeEXT;
   dispatch->dispatch_vkCmdSetLineStippleEnableEXT =
      vkr_dispatch_vkCmdSetLineStippleEnableEXT;
   dispatch->dispatch_vkCmdSetLogicOpEnableEXT = vkr_dispatch_vkCmdSetLogicOpEnableEXT;
   dispatch->dispatch_vkCmdSetPolygonModeEXT = vkr_dispatch_vkCmdSetPolygonModeEXT;
   dispatch->dispatch_vkCmdSetProvokingVertexModeEXT =
      vkr_dispatch_vkCmdSetProvokingVertexModeEXT;
   dispatch->dispatch_vkCmdSetRasterizationSamplesEXT =
      vkr_dispatch_vkCmdSetRasterizationSamplesEXT;
   dispatch->dispatch_vkCmdSetRasterizationStreamEXT =
      vkr_dispatch_vkCmdSetRasterizationStreamEXT;
   dispatch->dispatch_vkCmdSetSampleLocationsEnableEXT =
      vkr_dispatch_vkCmdSetSampleLocationsEnableEXT;
   dispatch->dispatch_vkCmdSetSampleMaskEXT = vkr_dispatch_vkCmdSetSampleMaskEXT;
   dispatch->dispatch_vkCmdSetTessellationDomainOriginEXT =
      vkr_dispatch_vkCmdSetTessellationDomainOriginEXT;

   /* VK_KHR_fragment_shading_rate */
   dispatch->dispatch_vkCmdSetFragmentShadingRateKHR =
      vkr_dispatch_vkCmdSetFragmentShadingRateKHR;

   /* VK_EXT_sample_locations */
   dispatch->dispatch_vkCmdSetSampleLocationsEXT =
      vkr_dispatch_vkCmdSetSampleLocationsEXT;

   /* VK_KHR_dynamic_rendering_local_read */
   dispatch->dispatch_vkCmdSetRenderingAttachmentLocations =
      vkr_dispatch_vkCmdSetRenderingAttachmentLocations;
   dispatch->dispatch_vkCmdSetRenderingInputAttachmentIndices =
      vkr_dispatch_vkCmdSetRenderingInputAttachmentIndices;

   /* VK_KHR_maintenance6 */
   dispatch->dispatch_vkCmdBindDescriptorSets2 = vkr_dispatch_vkCmdBindDescriptorSets2;
   dispatch->dispatch_vkCmdPushConstants2 = vkr_dispatch_vkCmdPushConstants2;
   dispatch->dispatch_vkCmdPushDescriptorSet2 = vkr_dispatch_vkCmdPushDescriptorSet2;
   dispatch->dispatch_vkCmdPushDescriptorSetWithTemplate2 = NULL;

   /* VK_KHR_acceleration_structure */
   dispatch->dispatch_vkCmdBuildAccelerationStructuresIndirectKHR =
      vkr_dispatch_vkCmdBuildAccelerationStructuresIndirectKHR;
   dispatch->dispatch_vkCmdBuildAccelerationStructuresKHR =
      vkr_dispatch_vkCmdBuildAccelerationStructuresKHR;
   dispatch->dispatch_vkCmdCopyAccelerationStructureKHR =
      vkr_dispatch_vkCmdCopyAccelerationStructureKHR;
   dispatch->dispatch_vkCmdCopyAccelerationStructureToMemoryKHR =
      vkr_dispatch_vkCmdCopyAccelerationStructureToMemoryKHR;
   dispatch->dispatch_vkCmdCopyMemoryToAccelerationStructureKHR =
      vkr_dispatch_vkCmdCopyMemoryToAccelerationStructureKHR;
   dispatch->dispatch_vkCmdWriteAccelerationStructuresPropertiesKHR =
      vkr_dispatch_vkCmdWriteAccelerationStructuresPropertiesKHR;

   /* VK_KHR_ray_tracing_pipeline */
   dispatch->dispatch_vkCmdSetRayTracingPipelineStackSizeKHR =
      vkr_dispatch_vkCmdSetRayTracingPipelineStackSizeKHR;
   dispatch->dispatch_vkCmdTraceRaysIndirectKHR = vkr_dispatch_vkCmdTraceRaysIndirectKHR;
   dispatch->dispatch_vkCmdTraceRaysKHR = vkr_dispatch_vkCmdTraceRaysKHR;

   /* VK_KHR_ray_tracing_maintenance1 */
   dispatch->dispatch_vkCmdTraceRaysIndirect2KHR =
      vkr_dispatch_vkCmdTraceRaysIndirect2KHR;

   /* VK_EXT_depth_bias_control */
   dispatch->dispatch_vkCmdSetDepthBias2EXT = vkr_dispatch_vkCmdSetDepthBias2EXT;

   /* VK_EXT_depth_clamp_control */
   dispatch->dispatch_vkCmdSetDepthClampRangeEXT =
      vkr_dispatch_vkCmdSetDepthClampRangeEXT;

   /* VK_EXT_attachment_feedback_loop_dynamic_state */
   dispatch->dispatch_vkCmdSetAttachmentFeedbackLoopEnableEXT =
      vkr_dispatch_vkCmdSetAttachmentFeedbackLoopEnableEXT;

   /* VK_EXT_mesh_shader */
   dispatch->dispatch_vkCmdDrawMeshTasksEXT = vkr_dispatch_vkCmdDrawMeshTasksEXT;
   dispatch->dispatch_vkCmdDrawMeshTasksIndirectEXT =
      vkr_dispatch_vkCmdDrawMeshTasksIndirectEXT;
   dispatch->dispatch_vkCmdDrawMeshTasksIndirectCountEXT =
      vkr_dispatch_vkCmdDrawMeshTasksIndirectCountEXT;

   /* VK_EXT_descriptor_heap */
   dispatch->dispatch_vkCmdBindSamplerHeapEXT = vkr_dispatch_vkCmdBindSamplerHeapEXT;
   dispatch->dispatch_vkCmdBindResourceHeapEXT = vkr_dispatch_vkCmdBindResourceHeapEXT;
   dispatch->dispatch_vkCmdPushDataEXT = vkr_dispatch_vkCmdPushDataEXT;
}
