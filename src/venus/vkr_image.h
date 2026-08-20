/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_IMAGE_H
#define VKR_IMAGE_H

#include "vkr_common.h"

#define VKR_WINEHUA_SCANOUT_CACHE 8

struct vkr_image {
   struct vkr_object base;

   struct vkr_device *device;
   VkFormat format;
   VkExtent3D extent;
   VkImageUsageFlags usage;
   VkImageType image_type;
   uint32_t mip_levels;
   uint32_t array_layers;
   VkSampleCountFlagBits samples;
   VkImageTiling tiling;
#ifdef __OHOS__
   /* Guest-owned host image. Scanout rotation swaps base.handle.image to a
    * NativeBuffer image owned by NativeWindowVkTarget; never vkDestroy that. */
   VkImage winehua_private_image;
   VkImage winehua_last_write_image;
   uint32_t winehua_last_write_full_cover;
   const char *winehua_last_write_op;
   uint64_t winehua_scanout_generation;
   /* Host-decoded writes to this image. Present compares against the last
    * epoch it already flushed: unchanged epoch + new serial = STALE_SOURCE
    * (typical ROUNDTRIP_ONLY race). */
   uint64_t winehua_write_epoch;
   uint64_t winehua_last_present_epoch;
   uint32_t winehua_last_present_serial;
#endif
};
VKR_DEFINE_OBJECT_CAST(image, VK_OBJECT_TYPE_IMAGE, VkImage)

struct vkr_image_view {
   struct vkr_object base;

   /* Retain the source object identity for opt-in Guest->Host descriptor
    * tracing. The Vulkan driver handle alone cannot prove which Guest image
    * supplied a view after handle replacement. */
   struct vkr_image *image;
#ifdef __OHOS__
   VkImageViewCreateInfo winehua_create_info;
   bool winehua_create_info_valid;
   VkImage winehua_bound_image;
   VkImageView winehua_private_view;
   struct {
      VkImage image;
      VkImageView view;
   } winehua_scanout_views[VKR_WINEHUA_SCANOUT_CACHE];
   uint32_t winehua_scanout_view_count;
#endif
};
VKR_DEFINE_OBJECT_CAST(image_view, VK_OBJECT_TYPE_IMAGE_VIEW, VkImageView)

struct vkr_sampler {
   struct vkr_object base;
};
VKR_DEFINE_OBJECT_CAST(sampler, VK_OBJECT_TYPE_SAMPLER, VkSampler)

struct vkr_sampler_ycbcr_conversion {
   struct vkr_object base;
};
VKR_DEFINE_OBJECT_CAST(sampler_ycbcr_conversion,
                       VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION,
                       VkSamplerYcbcrConversion)

void
vkr_context_init_image_dispatch(struct vkr_context *ctx);

void
vkr_context_init_image_view_dispatch(struct vkr_context *ctx);

void
vkr_context_init_sampler_dispatch(struct vkr_context *ctx);

void
vkr_context_init_sampler_ycbcr_conversion_dispatch(struct vkr_context *ctx);

#ifdef __OHOS__
int
vkr_image_set_scanout_backing(struct vkr_image *image, VkImage scanout);

int
vkr_image_clear_scanout_backing(struct vkr_image *image);

void
vkr_image_prepare_destroy(struct vkr_image *image);

void
vkr_image_view_prepare_destroy(struct vkr_image_view *view);
#endif

#endif /* VKR_IMAGE_H */
