/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_RENDER_PASS_H
#define VKR_RENDER_PASS_H

#include "vkr_common.h"

struct vkr_render_pass {
   struct vkr_object base;
};
VKR_DEFINE_OBJECT_CAST(render_pass, VK_OBJECT_TYPE_RENDER_PASS, VkRenderPass)

struct vkr_image_view;

struct vkr_framebuffer {
   struct vkr_object base;
#ifdef __OHOS__
   bool winehua_create_info_valid;
   uint32_t winehua_attachment_count;
   struct vkr_image_view *winehua_attachments[8];
   VkRenderPass winehua_render_pass;
   uint32_t winehua_width;
   uint32_t winehua_height;
   uint32_t winehua_layers;
   VkFramebuffer winehua_private_fb;
   struct {
      VkImageView views[8];
      VkFramebuffer fb;
   } winehua_scanout_fbs[8];
   uint32_t winehua_scanout_fb_count;
#endif
};
VKR_DEFINE_OBJECT_CAST(framebuffer, VK_OBJECT_TYPE_FRAMEBUFFER, VkFramebuffer)

void
vkr_context_init_render_pass_dispatch(struct vkr_context *ctx);

void
vkr_context_init_framebuffer_dispatch(struct vkr_context *ctx);

#endif /* VKR_RENDER_PASS_H */
