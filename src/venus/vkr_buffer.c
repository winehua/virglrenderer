/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_buffer.h"

#include "vkr_buffer_gen.h"
#include "vkr_context.h"
#include "vkr_device_memory.h"
#include "vkr_physical_device.h"

#ifdef __OHOS__
struct vkr_winehua_buffer_binding {
   struct vkr_buffer *buffer;
   struct vkr_device_memory *memory;
   VkDeviceSize offset;
};

static void
vkr_winehua_set_buffer_memory(struct vkr_context *ctx,
                              struct vkr_buffer *buffer,
                              struct vkr_device_memory *memory,
                              VkDeviceSize offset)
{
   if (!buffer)
      return;

   mtx_lock(&ctx->object_mutex);
   if (buffer->memory_listed) {
      list_del(&buffer->memory_head);
      list_inithead(&buffer->memory_head);
      buffer->memory_listed = false;
   }
   buffer->bound_memory = memory;
   buffer->bound_memory_offset = offset;
   if (memory) {
      list_addtail(&buffer->memory_head, &memory->bound_buffers);
      buffer->memory_listed = true;
   }
   mtx_unlock(&ctx->object_mutex);
}
#endif

static void
vkr_dispatch_vkCreateBuffer(struct vn_dispatch_context *dispatch,
                            struct vn_command_vkCreateBuffer *args)
{
   /* XXX If VkExternalMemoryBufferCreateInfo is chained by the app, all is
    * good.  If it is not chained, we might still bind an external memory to
    * the buffer, because vkr_dispatch_vkAllocateMemory makes any HOST_VISIBLE
    * memory external.  That is a spec violation.
    *
    * We could unconditionally chain VkExternalMemoryBufferCreateInfo.  Or we
    * could call vkGetPhysicalDeviceExternalBufferProperties and fail
    * vkCreateBuffer if the buffer does not support external memory.  But we
    * would still end up with spec violation either way, while having a higher
    * chance of causing compatibility issues.
    *
    * In practice, drivers usually ignore VkExternalMemoryBufferCreateInfo, or
    * use it to filter out memory types in VkMemoryRequirements that do not
    * support external memory.  Binding an external memory to a buffer created
    * without VkExternalMemoryBufferCreateInfo usually works.
    *
    * To formalize this, we are potentially looking for an extension that
    * supports exporting memories without making them external.  Because they
    * are not external, they can be bound to buffers created without
    * VkExternalMemoryBufferCreateInfo.  And because they are not external, we
    * need something that is not vkGetPhysicalDeviceExternalBufferProperties
    * to determine the exportability.  See
    * vkr_physical_device_init_memory_properties as well.
    */

#ifdef __OHOS__
   const VkBufferCreateInfo *guest_info = args->pCreateInfo;
   VkBufferCreateInfo host_info = *guest_info;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   if (vkr_device_memory_gpu_upload_enabled(dev))
      host_info.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   args->pCreateInfo = &host_info;

   struct vkr_buffer *buffer = vkr_buffer_create_and_add(dispatch->data, args);
   if (buffer) {
      buffer->winehua_ubo_watches = NULL;
      atomic_init(&buffer->winehua_ubo_watch_count, 0);
      buffer->winehua_ubo_watch_overflow = false;
      buffer->bound_memory = NULL;
      buffer->bound_memory_offset = 0;
      buffer->size = guest_info->size;
      buffer->guest_usage = guest_info->usage;
      buffer->host_usage = host_info.usage;
      list_inithead(&buffer->memory_head);
      buffer->memory_listed = false;
   }
#else
   vkr_buffer_create_and_add(dispatch->data, args);
#endif
}

static void
vkr_dispatch_vkDestroyBuffer(struct vn_dispatch_context *dispatch,
                             struct vn_command_vkDestroyBuffer *args)
{
#ifdef __OHOS__
   struct vkr_buffer *buffer = vkr_buffer_from_handle(args->buffer);
   vkr_winehua_set_buffer_memory(dispatch->data, buffer, NULL, 0);
   if (buffer) {
      free(buffer->winehua_ubo_watches);
      buffer->winehua_ubo_watches = NULL;
      atomic_store_explicit(&buffer->winehua_ubo_watch_count, 0,
                            memory_order_release);
   }
#endif
   vkr_buffer_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetBufferMemoryRequirements(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetBufferMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetBufferMemoryRequirements_args_handle(args);
   vk->GetBufferMemoryRequirements(args->device, args->buffer, args->pMemoryRequirements);
}

static void
vkr_dispatch_vkGetBufferMemoryRequirements2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetBufferMemoryRequirements2 *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetBufferMemoryRequirements2_args_handle(args);
   vk->GetBufferMemoryRequirements2(args->device, args->pInfo, args->pMemoryRequirements);
}

static void
vkr_dispatch_vkBindBufferMemory(struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkBindBufferMemory *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;
#ifdef __OHOS__
   struct vkr_buffer *buffer = vkr_buffer_from_handle(args->buffer);
   struct vkr_device_memory *memory = vkr_device_memory_from_handle(args->memory);
   const VkDeviceSize memory_offset = args->memoryOffset;
#endif

   vn_replace_vkBindBufferMemory_args_handle(args);
   args->ret =
      vk->BindBufferMemory(args->device, args->buffer, args->memory, args->memoryOffset);
#ifdef __OHOS__
   if (args->ret == VK_SUCCESS && buffer) {
      vkr_winehua_set_buffer_memory(
         dispatch->data, buffer, memory, memory_offset);
   }
#endif
}

static void
vkr_dispatch_vkBindBufferMemory2(struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkBindBufferMemory2 *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;
#ifdef __OHOS__
   STACK_ARRAY(struct vkr_winehua_buffer_binding, bindings, args->bindInfoCount);
   if (bindings) {
      for (uint32_t i = 0; i < args->bindInfoCount; i++) {
         const VkBindBufferMemoryInfo *info = &args->pBindInfos[i];
         bindings[i].buffer = vkr_buffer_from_handle(info->buffer);
         bindings[i].memory = vkr_device_memory_from_handle(info->memory);
         bindings[i].offset = info->memoryOffset;
      }
   }
#endif

   vn_replace_vkBindBufferMemory2_args_handle(args);
   args->ret = vk->BindBufferMemory2(args->device, args->bindInfoCount, args->pBindInfos);
#ifdef __OHOS__
   if (args->ret == VK_SUCCESS && bindings) {
      for (uint32_t i = 0; i < args->bindInfoCount; i++) {
         if (bindings[i].buffer) {
            vkr_winehua_set_buffer_memory(
               dispatch->data, bindings[i].buffer, bindings[i].memory,
               bindings[i].offset);
         }
      }
   }
   STACK_ARRAY_FINISH(bindings);
#endif
}

static void
vkr_dispatch_vkGetBufferOpaqueCaptureAddress(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetBufferOpaqueCaptureAddress *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetBufferOpaqueCaptureAddress_args_handle(args);
   args->ret = vk->GetBufferOpaqueCaptureAddress(args->device, args->pInfo);
}

static void
vkr_dispatch_vkGetBufferDeviceAddress(UNUSED struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkGetBufferDeviceAddress *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetBufferDeviceAddress_args_handle(args);
   args->ret = vk->GetBufferDeviceAddress(args->device, args->pInfo);
}

static void
vkr_dispatch_vkCreateBufferView(struct vn_dispatch_context *dispatch,
                                struct vn_command_vkCreateBufferView *args)
{
   vkr_buffer_view_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroyBufferView(struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkDestroyBufferView *args)
{
   vkr_buffer_view_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetDeviceBufferMemoryRequirements(
   UNUSED struct vn_dispatch_context *ctx,
   struct vn_command_vkGetDeviceBufferMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDeviceBufferMemoryRequirements_args_handle(args);
   vk->GetDeviceBufferMemoryRequirements(args->device, args->pInfo,
                                         args->pMemoryRequirements);
}

void
vkr_context_init_buffer_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateBuffer = vkr_dispatch_vkCreateBuffer;
   dispatch->dispatch_vkDestroyBuffer = vkr_dispatch_vkDestroyBuffer;
   dispatch->dispatch_vkGetBufferMemoryRequirements =
      vkr_dispatch_vkGetBufferMemoryRequirements;
   dispatch->dispatch_vkGetBufferMemoryRequirements2 =
      vkr_dispatch_vkGetBufferMemoryRequirements2;
   dispatch->dispatch_vkBindBufferMemory = vkr_dispatch_vkBindBufferMemory;
   dispatch->dispatch_vkBindBufferMemory2 = vkr_dispatch_vkBindBufferMemory2;
   dispatch->dispatch_vkGetBufferOpaqueCaptureAddress =
      vkr_dispatch_vkGetBufferOpaqueCaptureAddress;
   dispatch->dispatch_vkGetBufferDeviceAddress = vkr_dispatch_vkGetBufferDeviceAddress;
   dispatch->dispatch_vkGetDeviceBufferMemoryRequirements =
      vkr_dispatch_vkGetDeviceBufferMemoryRequirements;
}

void
vkr_context_init_buffer_view_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateBufferView = vkr_dispatch_vkCreateBufferView;
   dispatch->dispatch_vkDestroyBufferView = vkr_dispatch_vkDestroyBufferView;
}
