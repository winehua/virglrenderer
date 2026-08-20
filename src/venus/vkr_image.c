/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_image.h"

#include "vkr_device.h"
#include "vkr_image_gen.h"
#include "vkr_physical_device.h"
#include "vkr_render_pass.h"

#include <errno.h>
#include <string.h>

static bool
vkr_winehua_option_enabled(const char *name)
{
   const char *value = os_get_option(name);
   return value && !strcmp(value, "1");
}

static bool
vkr_winehua_remap_bgra_array_to_rgba(const struct vkr_image *image,
                                     VkFormat format)
{
#ifdef __OHOS__
   return vkr_winehua_option_enabled("VKR_WINEHUA_BGRA_ARRAY_RGBA") &&
      image && image->image_type == VK_IMAGE_TYPE_2D &&
      image->array_layers > 1 &&
      (image->usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
      image->format == VK_FORMAT_B8G8R8A8_SRGB &&
      format == VK_FORMAT_B8G8R8A8_SRGB;
#else
   (void)image;
   (void)format;
   return false;
#endif
}

#ifdef __OHOS__
static void
vkr_image_bind_host(struct vkr_image *image, VkImage host)
{
   image->base.handle.image = host;
}

static VkImageView
vkr_image_view_for_image(struct vkr_image_view *view,
                         VkImage host_image)
{
   if (!view || !view->image || !host_image || !view->winehua_create_info_valid) {
      vkr_log("WineHuaScanout: view recreate skipped viewId=%" PRIu64
              " valid=%d host=0x%" PRIxPTR,
              view ? view->base.id : 0,
              view ? (int)view->winehua_create_info_valid : 0,
              (uintptr_t)host_image);
      return VK_NULL_HANDLE;
   }
   struct vkr_device *dev = view->image->device;
   if (!dev)
      return VK_NULL_HANDLE;

   if (!view->winehua_private_view)
      view->winehua_private_view = view->base.handle.image_view;
   if (view->image->winehua_private_image &&
       host_image == view->image->winehua_private_image)
      return view->winehua_private_view;
   if (host_image == view->winehua_bound_image && view->base.handle.image_view)
      return view->base.handle.image_view;

   for (uint32_t i = 0; i < view->winehua_scanout_view_count; i++) {
      if (view->winehua_scanout_views[i].image == host_image)
         return view->winehua_scanout_views[i].view;
   }
   if (view->winehua_scanout_view_count >= VKR_WINEHUA_SCANOUT_CACHE) {
      vkr_log("WineHuaScanout: view cache full viewId=%" PRIu64, view->base.id);
      return VK_NULL_HANDLE;
   }

   VkImageViewCreateInfo info = view->winehua_create_info;
   info.pNext = NULL;
   info.image = host_image;
   VkImageView created = VK_NULL_HANDLE;
   const VkResult view_result =
      dev->proc_table.CreateImageView(dev->base.handle.device, &info, NULL, &created);
   if (view_result != VK_SUCCESS || !created) {
      vkr_log("WineHuaScanout: CreateImageView failed result=%d format=%u "
              "viewType=%u image=0x%" PRIxPTR,
              (int)view_result, info.format, info.viewType, (uintptr_t)host_image);
      return VK_NULL_HANDLE;
   }

   uint32_t slot = view->winehua_scanout_view_count++;
   view->winehua_scanout_views[slot].image = host_image;
   view->winehua_scanout_views[slot].view = created;
   return created;
}

static VkFramebuffer
vkr_framebuffer_for_views(struct vkr_framebuffer *fb, struct vkr_device *dev)
{
   if (!fb || !dev || !fb->winehua_create_info_valid)
      return VK_NULL_HANDLE;

   VkImageView views[8];
   for (uint32_t i = 0; i < fb->winehua_attachment_count; i++) {
      struct vkr_image_view *view = fb->winehua_attachments[i];
      views[i] = view ? view->base.handle.image_view : VK_NULL_HANDLE;
      if (!views[i])
         return VK_NULL_HANDLE;
   }
   for (uint32_t i = 0; i < fb->winehua_scanout_fb_count; i++) {
      if (!memcmp(fb->winehua_scanout_fbs[i].views, views,
                  fb->winehua_attachment_count * sizeof(views[0])))
         return fb->winehua_scanout_fbs[i].fb;
   }
   if (fb->winehua_scanout_fb_count >= 8)
      return VK_NULL_HANDLE;

   VkFramebufferCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = fb->winehua_render_pass,
      .attachmentCount = fb->winehua_attachment_count,
      .pAttachments = views,
      .width = fb->winehua_width,
      .height = fb->winehua_height,
      .layers = fb->winehua_layers,
   };
   VkFramebuffer created = VK_NULL_HANDLE;
   const VkResult fb_result =
      dev->proc_table.CreateFramebuffer(dev->base.handle.device, &info, NULL, &created);
   if (fb_result != VK_SUCCESS || !created) {
      vkr_log("WineHuaScanout: CreateFramebuffer failed result=%d attachments=%u "
              "size=%ux%u",
              (int)fb_result, fb->winehua_attachment_count, fb->winehua_width,
              fb->winehua_height);
      return VK_NULL_HANDLE;
   }

   uint32_t slot = fb->winehua_scanout_fb_count++;
   memcpy(fb->winehua_scanout_fbs[slot].views, views, sizeof(views));
   fb->winehua_scanout_fbs[slot].fb = created;
   return created;
}

static int
vkr_image_recreate_views_and_framebuffers(struct vkr_image *image, VkImage host_image)
{
   struct vkr_device *dev = image->device;
   if (!dev)
      return -ENODEV;

   mtx_lock(&dev->object_mutex);
   list_for_each_entry (struct vkr_object, obj, &dev->objects, track_head) {
      if (obj->type != VK_OBJECT_TYPE_IMAGE_VIEW)
         continue;
      struct vkr_image_view *view = (struct vkr_image_view *)obj;
      if (view->image != image)
         continue;
      VkImageView next = vkr_image_view_for_image(view, host_image);
      if (!next) {
         mtx_unlock(&dev->object_mutex);
         return -EIO;
      }
      view->base.handle.image_view = next;
      view->winehua_bound_image = host_image;
   }
   list_for_each_entry (struct vkr_object, obj, &dev->objects, track_head) {
      if (obj->type != VK_OBJECT_TYPE_FRAMEBUFFER)
         continue;
      struct vkr_framebuffer *fb = (struct vkr_framebuffer *)obj;
      if (!fb->winehua_create_info_valid)
         continue;
      bool uses_image = false;
      for (uint32_t i = 0; i < fb->winehua_attachment_count; i++) {
         struct vkr_image_view *view = fb->winehua_attachments[i];
         if (view && view->image == image) {
            uses_image = true;
            break;
         }
      }
      if (!uses_image)
         continue;
      VkFramebuffer next = vkr_framebuffer_for_views(fb, dev);
      if (!next) {
         mtx_unlock(&dev->object_mutex);
         return -EIO;
      }
      fb->base.handle.framebuffer = next;
   }
   mtx_unlock(&dev->object_mutex);
   return 0;
}

int
vkr_image_set_scanout_backing(struct vkr_image *image, VkImage scanout)
{
   if (!image || !scanout)
      return -EINVAL;
   if (image->samples != VK_SAMPLE_COUNT_1_BIT)
      return -ENOTSUP;
   if (!image->winehua_private_image)
      image->winehua_private_image = image->base.handle.image;
   if (!image->winehua_private_image)
      return -EINVAL;
   if (image->base.handle.image == scanout) {
      image->winehua_scanout_generation++;
      return vkr_image_recreate_views_and_framebuffers(image, scanout);
   }

   const VkImage previous = image->base.handle.image;
   vkr_image_bind_host(image, scanout);
   const int ret = vkr_image_recreate_views_and_framebuffers(image, scanout);
   if (ret) {
      vkr_image_bind_host(image, previous);
      vkr_image_recreate_views_and_framebuffers(image, previous);
      vkr_log("WineHuaScanout: apply failed imageId=%" PRIu64
              " scanout=0x%" PRIxPTR " ret=%d",
              image->base.id, (uintptr_t)scanout, ret);
      return ret;
   }
   image->winehua_scanout_generation++;
   if (image->winehua_scanout_generation <= 8 ||
       image->winehua_scanout_generation % 120 == 0) {
      vkr_log("WineHuaScanout: apply imageId=%" PRIu64 " private=0x%" PRIxPTR
              " scanout=0x%" PRIxPTR " gen=%" PRIu64,
              image->base.id, (uintptr_t)image->winehua_private_image,
              (uintptr_t)scanout, image->winehua_scanout_generation);
   }
   return 0;
}

int
vkr_image_clear_scanout_backing(struct vkr_image *image)
{
   if (!image)
      return -EINVAL;
   if (!image->winehua_private_image ||
       image->base.handle.image == image->winehua_private_image)
      return 0;

   struct vkr_device *dev = image->device;
   const VkImage private_image = image->winehua_private_image;
   vkr_image_bind_host(image, private_image);
   if (!dev)
      return 0;

   mtx_lock(&dev->object_mutex);
   list_for_each_entry (struct vkr_object, obj, &dev->objects, track_head) {
      if (obj->type != VK_OBJECT_TYPE_IMAGE_VIEW)
         continue;
      struct vkr_image_view *view = (struct vkr_image_view *)obj;
      if (view->image != image)
         continue;
      if (view->winehua_private_view)
         view->base.handle.image_view = view->winehua_private_view;
      view->winehua_bound_image = private_image;
      for (uint32_t i = 0; i < view->winehua_scanout_view_count; i++) {
         VkImageView extra = view->winehua_scanout_views[i].view;
         if (extra && extra != view->winehua_private_view)
            dev->proc_table.DestroyImageView(dev->base.handle.device, extra, NULL);
         view->winehua_scanout_views[i].image = VK_NULL_HANDLE;
         view->winehua_scanout_views[i].view = VK_NULL_HANDLE;
      }
      view->winehua_scanout_view_count = 0;
   }
   list_for_each_entry (struct vkr_object, obj, &dev->objects, track_head) {
      if (obj->type != VK_OBJECT_TYPE_FRAMEBUFFER)
         continue;
      struct vkr_framebuffer *fb = (struct vkr_framebuffer *)obj;
      if (!fb->winehua_create_info_valid)
         continue;
      bool uses_image = false;
      for (uint32_t i = 0; i < fb->winehua_attachment_count; i++) {
         struct vkr_image_view *view = fb->winehua_attachments[i];
         if (view && view->image == image) {
            uses_image = true;
            break;
         }
      }
      if (!uses_image)
         continue;
      if (fb->winehua_private_fb)
         fb->base.handle.framebuffer = fb->winehua_private_fb;
      for (uint32_t i = 0; i < fb->winehua_scanout_fb_count; i++) {
         VkFramebuffer extra = fb->winehua_scanout_fbs[i].fb;
         if (extra && extra != fb->winehua_private_fb)
            dev->proc_table.DestroyFramebuffer(dev->base.handle.device, extra, NULL);
         memset(&fb->winehua_scanout_fbs[i], 0, sizeof(fb->winehua_scanout_fbs[i]));
      }
      fb->winehua_scanout_fb_count = 0;
   }
   mtx_unlock(&dev->object_mutex);
   vkr_log("WineHuaScanout: clear imageId=%" PRIu64 " private=0x%" PRIxPTR,
           image->base.id, (uintptr_t)private_image);
   return 0;
}

void
vkr_image_prepare_destroy(struct vkr_image *image)
{
   if (!image)
      return;
   vkr_image_clear_scanout_backing(image);
}

void
vkr_image_view_prepare_destroy(struct vkr_image_view *view)
{
   if (!view || !view->image || !view->image->device)
      return;
   struct vkr_device *dev = view->image->device;
   if (view->winehua_private_view)
      view->base.handle.image_view = view->winehua_private_view;
   for (uint32_t i = 0; i < view->winehua_scanout_view_count; i++) {
      VkImageView extra = view->winehua_scanout_views[i].view;
      if (extra && extra != view->winehua_private_view)
         dev->proc_table.DestroyImageView(dev->base.handle.device, extra, NULL);
      view->winehua_scanout_views[i].view = VK_NULL_HANDLE;
   }
   view->winehua_scanout_view_count = 0;
}
#endif

static void
vkr_dispatch_vkCreateImage(struct vn_dispatch_context *dispatch,
                           struct vn_command_vkCreateImage *args)
{
   /* XXX If VkExternalMemoryImageCreateInfo is chained by the app, all is
    * good.  If it is not chained, we might still bind an external memory to
    * the image, because vkr_dispatch_vkAllocateMemory makes any HOST_VISIBLE
    * memory external.  That is a spec violation.
    *
    * The discussions in vkr_dispatch_vkCreateBuffer are applicable to both
    * buffers and images.  Additionally, drivers usually use
    * VkExternalMemoryImageCreateInfo to pick a well-defined image layout for
    * interoperability with foreign queues.  However, a well-defined layout
    * might not exist for some images.  When it does, it might still require a
    * dedicated allocation or might have a degraded performance.
    *
    * On the other hand, binding an external memory to an image created
    * without VkExternalMemoryImageCreateInfo usually works.  Yes, it will
    * explode if the external memory is accessed by foreign queues due to the
    * lack of a well-defined image layout.  But we never end up in that
    * situation because the app does not consider the memory external.
    */

   struct vkr_device *dev = vkr_device_from_handle(args->device);
   const VkImageCreateInfo *guest_info = args->pCreateInfo;
   const VkImageCreateInfo create_info = *args->pCreateInfo;
   VkImageCreateInfo host_info = create_info;
#ifdef __OHOS__
   if (create_info.samples == VK_SAMPLE_COUNT_2_BIT) {
      /* Legal CreateImage error; FORMAT_NOT_SUPPORTED is not. */
      args->ret = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      if (args->pImage)
         *args->pImage = VK_NULL_HANDLE;
      return;
   }
#endif
#ifdef __OHOS__
   const bool remap_bgra_array =
      vkr_winehua_option_enabled("VKR_WINEHUA_BGRA_ARRAY_RGBA") &&
      create_info.imageType == VK_IMAGE_TYPE_2D &&
      create_info.arrayLayers > 1 &&
      (create_info.usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
      create_info.format == VK_FORMAT_B8G8R8A8_SRGB;
   if (remap_bgra_array) {
      host_info.format = VK_FORMAT_R8G8B8A8_SRGB;
      args->pCreateInfo = &host_info;
   }
#endif
   struct vkr_image *image = vkr_image_create_and_add(dispatch->data, args);
   args->pCreateInfo = guest_info;
   if (!image)
      return;

   image->device = dev;
   image->format = create_info.format;
   image->extent = create_info.extent;
   image->usage = create_info.usage;
   image->image_type = create_info.imageType;
   image->mip_levels = create_info.mipLevels;
   image->array_layers = create_info.arrayLayers;
   image->samples = create_info.samples;
   image->tiling = create_info.tiling;
   if (vkr_winehua_option_enabled("WINEHUA_VKR_TRACE_SAMPLED")) {
      vkr_log("WineHuaSampled: host-image guestImage=%" PRIu64
              " hostImage=0x%" PRIxPTR " format=%u hostFormat=%u"
              " extent=%ux%ux%u"
              " mips=%u layers=%u usage=0x%x tiling=%u",
              image->base.id, (uintptr_t)image->base.handle.image,
              create_info.format, host_info.format, create_info.extent.width,
              create_info.extent.height, create_info.extent.depth,
              create_info.mipLevels, create_info.arrayLayers,
              create_info.usage, create_info.tiling);
   }
}

static void
vkr_dispatch_vkDestroyImage(struct vn_dispatch_context *dispatch,
                            struct vn_command_vkDestroyImage *args)
{
#ifdef __OHOS__
   struct vkr_image *image = vkr_image_from_handle(args->image);
   vkr_image_prepare_destroy(image);
#endif
   vkr_image_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetImageMemoryRequirements(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageMemoryRequirements_args_handle(args);
   vk->GetImageMemoryRequirements(args->device, args->image, args->pMemoryRequirements);
}

static void
vkr_dispatch_vkGetImageMemoryRequirements2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageMemoryRequirements2 *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageMemoryRequirements2_args_handle(args);
   vk->GetImageMemoryRequirements2(args->device, args->pInfo, args->pMemoryRequirements);
}

static void
vkr_dispatch_vkGetImageSparseMemoryRequirements(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSparseMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageSparseMemoryRequirements_args_handle(args);
   vk->GetImageSparseMemoryRequirements(args->device, args->image,
                                        args->pSparseMemoryRequirementCount,
                                        args->pSparseMemoryRequirements);
}

static void
vkr_dispatch_vkGetImageSparseMemoryRequirements2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSparseMemoryRequirements2 *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageSparseMemoryRequirements2_args_handle(args);
   vk->GetImageSparseMemoryRequirements2(args->device, args->pInfo,
                                         args->pSparseMemoryRequirementCount,
                                         args->pSparseMemoryRequirements);
}

static void
vkr_dispatch_vkBindImageMemory(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkBindImageMemory *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkBindImageMemory_args_handle(args);
   args->ret =
      vk->BindImageMemory(args->device, args->image, args->memory, args->memoryOffset);
}

static void
vkr_dispatch_vkBindImageMemory2(UNUSED struct vn_dispatch_context *dispatch,
                                struct vn_command_vkBindImageMemory2 *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkBindImageMemory2_args_handle(args);
   args->ret = vk->BindImageMemory2(args->device, args->bindInfoCount, args->pBindInfos);
}

static void
vkr_dispatch_vkGetImageSubresourceLayout(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSubresourceLayout *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageSubresourceLayout_args_handle(args);
   vk->GetImageSubresourceLayout(args->device, args->image, args->pSubresource,
                                 args->pLayout);
}

static void
vkr_dispatch_vkGetImageSubresourceLayout2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSubresourceLayout2 *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageSubresourceLayout2_args_handle(args);
   vk->GetImageSubresourceLayout2(args->device, args->image, args->pSubresource,
                                  args->pLayout);
}

static void
vkr_dispatch_vkGetDeviceImageSubresourceLayout(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDeviceImageSubresourceLayout *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDeviceImageSubresourceLayout_args_handle(args);
   vk->GetDeviceImageSubresourceLayout(args->device, args->pInfo, args->pLayout);
}

static void
vkr_dispatch_vkGetImageDrmFormatModifierPropertiesEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageDrmFormatModifierPropertiesEXT *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetImageDrmFormatModifierPropertiesEXT_args_handle(args);
   args->ret = vk->GetImageDrmFormatModifierPropertiesEXT(args->device, args->image,
                                                          args->pProperties);
}

static void
vkr_dispatch_vkCreateImageView(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCreateImageView *args)
{
   const VkImageViewCreateInfo *guest_info = args->pCreateInfo;
   const VkImageViewCreateInfo create_info = *args->pCreateInfo;
   struct vkr_image *image =
      vkr_image_from_handle(args->pCreateInfo->image);
   VkImageViewCreateInfo host_info = create_info;
   if (vkr_winehua_remap_bgra_array_to_rgba(image, create_info.format)) {
      host_info.format = VK_FORMAT_R8G8B8A8_SRGB;
      host_info.components.r = VK_COMPONENT_SWIZZLE_B;
      host_info.components.g = VK_COMPONENT_SWIZZLE_G;
      host_info.components.b = VK_COMPONENT_SWIZZLE_R;
      host_info.components.a = VK_COMPONENT_SWIZZLE_A;
      args->pCreateInfo = &host_info;
   }
   struct vkr_image_view *view =
      vkr_image_view_create_and_add(dispatch->data, args);
   args->pCreateInfo = guest_info;

   if (!view)
      return;

   view->image = image;
#ifdef __OHOS__
   view->winehua_create_info = host_info;
   view->winehua_create_info.pNext = NULL;
   view->winehua_create_info_valid = true;
   view->winehua_bound_image = image ? image->base.handle.image : VK_NULL_HANDLE;
   view->winehua_private_view = view->base.handle.image_view;
#endif
   {
      if (vkr_winehua_option_enabled("WINEHUA_VKR_TRACE_SAMPLED")) {
         vkr_log("WineHuaSampled: host-image-view guestView=%" PRIu64 " "
                 "guestImage=%" PRIu64 " hostView=0x%" PRIxPTR " "
                 "hostImage=0x%" PRIxPTR " format=%u hostFormat=%u viewType=%u "
                 "components=%u,%u,%u,%u "
                 "aspect=0x%x mip=%u+%u layer=%u+%u",
                 view->base.id, image ? image->base.id : 0,
                 (uintptr_t)view->base.handle.image_view,
                 image ? (uintptr_t)image->base.handle.image : 0,
                 create_info.format, host_info.format, create_info.viewType,
                 host_info.components.r, host_info.components.g,
                 host_info.components.b, host_info.components.a,
                 create_info.subresourceRange.aspectMask,
                 create_info.subresourceRange.baseMipLevel,
                 create_info.subresourceRange.levelCount,
                 create_info.subresourceRange.baseArrayLayer,
                 create_info.subresourceRange.layerCount);
      }
   }
}

static void
vkr_dispatch_vkDestroyImageView(struct vn_dispatch_context *dispatch,
                                struct vn_command_vkDestroyImageView *args)
{
#ifdef __OHOS__
   struct vkr_image_view *view = vkr_image_view_from_handle(args->imageView);
   vkr_image_view_prepare_destroy(view);
#endif
   vkr_image_view_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkCreateSampler(struct vn_dispatch_context *dispatch,
                             struct vn_command_vkCreateSampler *args)
{
   const VkSamplerCreateInfo create_info = *args->pCreateInfo;
   struct vkr_sampler *sampler =
      vkr_sampler_create_and_add(dispatch->data, args);
   if (vkr_winehua_option_enabled("WINEHUA_VKR_TRACE_SAMPLED")) {
      vkr_log("WineHuaSampled: host-sampler guestSampler=%" PRIu64 " "
              "hostSampler=0x%" PRIxPTR " compareEnable=%u compareOp=%u "
              "minFilter=%u magFilter=%u mipmapMode=%u "
              "address=%u,%u,%u lod=%f..%f border=%u result=%d",
              sampler ? sampler->base.id : 0,
              sampler ? (uintptr_t)sampler->base.handle.sampler : 0,
              create_info.compareEnable, create_info.compareOp,
              create_info.minFilter, create_info.magFilter,
              create_info.mipmapMode, create_info.addressModeU,
              create_info.addressModeV, create_info.addressModeW,
              create_info.minLod, create_info.maxLod,
              create_info.borderColor, args->ret);
   }
}

static void
vkr_dispatch_vkDestroySampler(struct vn_dispatch_context *dispatch,
                              struct vn_command_vkDestroySampler *args)
{
   vkr_sampler_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkCreateSamplerYcbcrConversion(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkCreateSamplerYcbcrConversion *args)
{
   vkr_sampler_ycbcr_conversion_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroySamplerYcbcrConversion(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkDestroySamplerYcbcrConversion *args)
{
   vkr_sampler_ycbcr_conversion_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetDeviceImageMemoryRequirements(
   UNUSED struct vn_dispatch_context *ctx,
   struct vn_command_vkGetDeviceImageMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDeviceImageMemoryRequirements_args_handle(args);
   vk->GetDeviceImageMemoryRequirements(args->device, args->pInfo,
                                        args->pMemoryRequirements);
}

static void
vkr_dispatch_vkGetDeviceImageSparseMemoryRequirements(
   UNUSED struct vn_dispatch_context *ctx,
   struct vn_command_vkGetDeviceImageSparseMemoryRequirements *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDeviceImageSparseMemoryRequirements_args_handle(args);
   vk->GetDeviceImageSparseMemoryRequirements(args->device, args->pInfo,
                                              args->pSparseMemoryRequirementCount,
                                              args->pSparseMemoryRequirements);
}

void
vkr_context_init_image_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateImage = vkr_dispatch_vkCreateImage;
   dispatch->dispatch_vkDestroyImage = vkr_dispatch_vkDestroyImage;
   dispatch->dispatch_vkGetImageMemoryRequirements =
      vkr_dispatch_vkGetImageMemoryRequirements;
   dispatch->dispatch_vkGetImageMemoryRequirements2 =
      vkr_dispatch_vkGetImageMemoryRequirements2;
   dispatch->dispatch_vkGetImageSparseMemoryRequirements =
      vkr_dispatch_vkGetImageSparseMemoryRequirements;
   dispatch->dispatch_vkGetImageSparseMemoryRequirements2 =
      vkr_dispatch_vkGetImageSparseMemoryRequirements2;
   dispatch->dispatch_vkBindImageMemory = vkr_dispatch_vkBindImageMemory;
   dispatch->dispatch_vkBindImageMemory2 = vkr_dispatch_vkBindImageMemory2;
   dispatch->dispatch_vkGetImageSubresourceLayout =
      vkr_dispatch_vkGetImageSubresourceLayout;
   dispatch->dispatch_vkGetImageSubresourceLayout2 =
      vkr_dispatch_vkGetImageSubresourceLayout2;
   dispatch->dispatch_vkGetDeviceImageSubresourceLayout =
      vkr_dispatch_vkGetDeviceImageSubresourceLayout;

   dispatch->dispatch_vkGetImageDrmFormatModifierPropertiesEXT =
      vkr_dispatch_vkGetImageDrmFormatModifierPropertiesEXT;
   dispatch->dispatch_vkGetDeviceImageMemoryRequirements =
      vkr_dispatch_vkGetDeviceImageMemoryRequirements;
   dispatch->dispatch_vkGetDeviceImageSparseMemoryRequirements =
      vkr_dispatch_vkGetDeviceImageSparseMemoryRequirements;
}

void
vkr_context_init_image_view_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateImageView = vkr_dispatch_vkCreateImageView;
   dispatch->dispatch_vkDestroyImageView = vkr_dispatch_vkDestroyImageView;
}

void
vkr_context_init_sampler_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateSampler = vkr_dispatch_vkCreateSampler;
   dispatch->dispatch_vkDestroySampler = vkr_dispatch_vkDestroySampler;
}

void
vkr_context_init_sampler_ycbcr_conversion_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateSamplerYcbcrConversion =
      vkr_dispatch_vkCreateSamplerYcbcrConversion;
   dispatch->dispatch_vkDestroySamplerYcbcrConversion =
      vkr_dispatch_vkDestroySamplerYcbcrConversion;
}
