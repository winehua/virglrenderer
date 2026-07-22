/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_descriptor_set.h"

#include "vkr_descriptor_set_gen.h"
#include "vkr_image.h"

#include <stdatomic.h>

#define VKR_WINEHUA_SAMPLE_TRACE_LIMIT 32768u

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

static void
vkr_winehua_log_guest_descriptor_objects(uint32_t write_count,
                                         const VkWriteDescriptorSet *writes)
{
   if (!vkr_winehua_sample_trace_enabled())
      return;

   for (uint32_t i = 0; i < write_count; i++) {
      const VkWriteDescriptorSet *write = &writes[i];
      struct vkr_descriptor_set *set;
      if (!vkr_winehua_image_descriptor(write->descriptorType) ||
          !write->pImageInfo)
         continue;

      set = vkr_descriptor_set_from_handle(write->dstSet);
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
}

static void
vkr_winehua_log_host_descriptor_handles(uint32_t write_count,
                                        const VkWriteDescriptorSet *writes)
{
   if (!vkr_winehua_sample_trace_enabled())
      return;

   for (uint32_t i = 0; i < write_count; i++) {
      const VkWriteDescriptorSet *write = &writes[i];
      if (!vkr_winehua_image_descriptor(write->descriptorType) ||
          !write->pImageInfo)
         continue;

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
   vkr_descriptor_set_layout_create_and_add(dispatch->data, args);
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
vkr_dispatch_vkUpdateDescriptorSets(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkUpdateDescriptorSets *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

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
