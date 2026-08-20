/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_render_pass.h"

#include "vkr_render_pass_gen.h"
#include "vkr_image.h"

#include <stdatomic.h>

static bool
vkr_winehua_render_trace_enabled(void)
{
   static int enabled = -1;
   if (enabled < 0) {
      const char *value = os_get_option("WINEHUA_VKR_TRACE_CAPTURE");
      enabled = value && value[0] == '1';
   }
   return enabled != 0;
}

static bool
vkr_winehua_render_trace_allow(void)
{
   static atomic_uint emitted = ATOMIC_VAR_INIT(0);
   const unsigned index =
      atomic_fetch_add_explicit(&emitted, 1, memory_order_relaxed);
   return index < 4096u;
}

static void
vkr_winehua_log_render_pass_create(const VkRenderPassCreateInfo *info,
                                   uint64_t render_pass_id)
{
   if (!info || !vkr_winehua_render_trace_enabled() ||
       !vkr_winehua_render_trace_allow())
      return;

   vkr_log("WineHuaRender: create-pass id=%" PRIu64 " attachments=%u subpasses=%u",
           render_pass_id, info->attachmentCount, info->subpassCount);
   for (uint32_t i = 0; i < info->attachmentCount; i++) {
      const VkAttachmentDescription *a = &info->pAttachments[i];
      vkr_log("WineHuaRender: pass-attachment pass=%" PRIu64 " index=%u"
              " format=%u samples=%u load=%u store=%u stencilLoad=%u"
              " stencilStore=%u initial=%u final=%u",
              render_pass_id, i, a->format, a->samples, a->loadOp, a->storeOp,
              a->stencilLoadOp, a->stencilStoreOp, a->initialLayout,
              a->finalLayout);
   }
   for (uint32_t i = 0; i < info->subpassCount; i++) {
      const VkSubpassDescription *s = &info->pSubpasses[i];
      vkr_log("WineHuaRender: pass-subpass pass=%" PRIu64 " index=%u"
              " bindPoint=%u input=%u color=%u resolve=%u depth=%s preserve=%u",
              render_pass_id, i, s->pipelineBindPoint, s->inputAttachmentCount,
              s->colorAttachmentCount, s->pResolveAttachments ? 1u : 0u,
              s->pDepthStencilAttachment ? "yes" : "no", s->preserveAttachmentCount);
      for (uint32_t j = 0; j < s->colorAttachmentCount; j++) {
         const VkAttachmentReference *r = &s->pColorAttachments[j];
         vkr_log("WineHuaRender: pass-color-ref pass=%" PRIu64 " subpass=%u"
                 " index=%u attachment=%u layout=%u",
                 render_pass_id, i, j, r->attachment, r->layout);
      }
      if (s->pDepthStencilAttachment) {
         vkr_log("WineHuaRender: pass-depth-ref pass=%" PRIu64 " subpass=%u"
                 " attachment=%u layout=%u",
                 render_pass_id, i, s->pDepthStencilAttachment->attachment,
                 s->pDepthStencilAttachment->layout);
      }
   }
}

struct vkr_winehua_framebuffer_attachment {
   uint64_t view_id;
   uintptr_t host_view;
   uint64_t image_id;
   uintptr_t host_image;
   VkFormat format;
   VkExtent3D extent;
   uint32_t mip_levels;
   uint32_t array_layers;
};

static void
vkr_winehua_log_framebuffer_create(
   const struct vkr_framebuffer *framebuffer,
   uint64_t render_pass_id,
   uintptr_t host_render_pass,
   uint32_t attachment_count,
   uint32_t width,
   uint32_t height,
   uint32_t layers,
   const struct vkr_winehua_framebuffer_attachment *attachments)
{
   if (!framebuffer || !vkr_winehua_render_trace_enabled() ||
       !vkr_winehua_render_trace_allow())
      return;

   vkr_log("WineHuaRender: create-framebuffer id=%" PRIu64
           " renderPass=%" PRIu64 " hostRenderPass=0x%" PRIxPTR
           " attachments=%u extent=%ux%u layers=%u",
           framebuffer->base.id, render_pass_id, host_render_pass,
           attachment_count, width, height, layers);
   for (uint32_t i = 0; attachments && i < attachment_count; i++) {
      const struct vkr_winehua_framebuffer_attachment *a = &attachments[i];
      vkr_log("WineHuaRender: framebuffer-attachment fb=%" PRIu64 " index=%u"
              " view=%" PRIu64 " hostView=0x%" PRIxPTR
              " image=%" PRIu64 " hostImage=0x%" PRIxPTR
              " format=%u extent=%ux%ux%u mips=%u layers=%u",
              framebuffer->base.id, i, a->view_id, a->host_view,
              a->image_id, a->host_image, a->format,
              a->extent.width, a->extent.height, a->extent.depth,
              a->mip_levels, a->array_layers);
   }
}

static void
vkr_dispatch_vkCreateRenderPass(struct vn_dispatch_context *dispatch,
                                struct vn_command_vkCreateRenderPass *args)
{
   if (!vkr_winehua_render_trace_enabled()) {
      vkr_render_pass_create_and_add(dispatch->data, args);
      return;
   }

   const VkRenderPassCreateInfo create_info = *args->pCreateInfo;
   const struct vkr_render_pass *pass =
      vkr_render_pass_create_and_add(dispatch->data, args);
   vkr_winehua_log_render_pass_create(&create_info, pass ? pass->base.id : 0);
}

static void
vkr_dispatch_vkCreateRenderPass2(struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkCreateRenderPass2 *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   struct vkr_render_pass *pass = vkr_context_alloc_object(
      ctx, sizeof(*pass), VK_OBJECT_TYPE_RENDER_PASS, args->pRenderPass);
   if (!pass) {
      args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;
      return;
   }

   vn_replace_vkCreateRenderPass2_args_handle(args);
   args->ret = vk->CreateRenderPass2(args->device, args->pCreateInfo, NULL,
                                     &pass->base.handle.render_pass);
   if (args->ret != VK_SUCCESS) {
      free(pass);
      return;
   }

   vkr_device_add_object(ctx, dev, &pass->base);
   if (vkr_winehua_render_trace_enabled() && args->pCreateInfo) {
      vkr_log("WineHuaRender: create-pass2 id=%" PRIu64 " attachments=%u subpasses=%u",
              pass->base.id, args->pCreateInfo->attachmentCount,
              args->pCreateInfo->subpassCount);
      for (uint32_t i = 0; i < args->pCreateInfo->attachmentCount; i++) {
         const VkAttachmentDescription2 *a = &args->pCreateInfo->pAttachments[i];
         vkr_log("WineHuaRender: pass2-attachment pass=%" PRIu64
                 " index=%u format=%u samples=%u load=%u store=%u"
                 " stencilLoad=%u stencilStore=%u initial=%u final=%u",
                 pass->base.id, i, a->format, a->samples, a->loadOp, a->storeOp,
                 a->stencilLoadOp, a->stencilStoreOp, a->initialLayout,
                 a->finalLayout);
      }
   }
}

static void
vkr_dispatch_vkDestroyRenderPass(struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkDestroyRenderPass *args)
{
   vkr_render_pass_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetRenderAreaGranularity(UNUSED struct vn_dispatch_context *dispatch,
                                        struct vn_command_vkGetRenderAreaGranularity *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetRenderAreaGranularity_args_handle(args);
   vk->GetRenderAreaGranularity(args->device, args->renderPass, args->pGranularity);
}

static void
vkr_dispatch_vkGetRenderingAreaGranularity(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetRenderingAreaGranularity *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetRenderingAreaGranularity_args_handle(args);
   vk->GetRenderingAreaGranularity(args->device, args->pRenderingAreaInfo,
                                   args->pGranularity);
}

static void
vkr_dispatch_vkCreateFramebuffer(struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkCreateFramebuffer *args)
{
   const VkFramebufferCreateInfo *info = args->pCreateInfo;
#ifdef __OHOS__
   struct vkr_image_view *views[8] = { 0 };
   const bool store =
      info &&
      !(info->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) &&
      info->attachmentCount <= 8;
   uint32_t stored = 0;
   VkRenderPass host_pass = VK_NULL_HANDLE;
   if (store) {
      const struct vkr_render_pass *pass =
         vkr_render_pass_from_handle(info->renderPass);
      host_pass = pass ? pass->base.handle.render_pass : VK_NULL_HANDLE;
      stored = info->attachmentCount;
      for (uint32_t i = 0; i < stored; i++)
         views[i] = vkr_image_view_from_handle(info->pAttachments[i]);
   }
#endif
   if (!vkr_winehua_render_trace_enabled()) {
      struct vkr_framebuffer *framebuffer =
         vkr_framebuffer_create_and_add(dispatch->data, args);
#ifdef __OHOS__
      if (framebuffer && store) {
         framebuffer->winehua_create_info_valid = true;
         framebuffer->winehua_attachment_count = stored;
         memcpy(framebuffer->winehua_attachments, views,
                sizeof(framebuffer->winehua_attachments));
         framebuffer->winehua_render_pass = host_pass;
         framebuffer->winehua_width = info->width;
         framebuffer->winehua_height = info->height;
         framebuffer->winehua_layers = info->layers;
         framebuffer->winehua_private_fb = framebuffer->base.handle.framebuffer;
      }
#endif
      return;
   }

   const struct vkr_render_pass *pass = vkr_render_pass_from_handle(info->renderPass);
   const uint64_t render_pass_id = pass ? pass->base.id : 0;
   const uintptr_t host_render_pass =
      pass ? (uintptr_t)pass->base.handle.render_pass : 0;
   const uint32_t attachment_count = info->attachmentCount;
   struct vkr_winehua_framebuffer_attachment *attachments =
      attachment_count ? calloc(attachment_count, sizeof(*attachments)) : NULL;
   if (attachments) {
      for (uint32_t i = 0; i < attachment_count; i++) {
         const struct vkr_image_view *view =
            vkr_image_view_from_handle(info->pAttachments[i]);
         const struct vkr_image *image = view ? view->image : NULL;
         attachments[i].view_id = view ? view->base.id : 0;
         attachments[i].host_view =
            view ? (uintptr_t)view->base.handle.image_view : 0;
         attachments[i].image_id = image ? image->base.id : 0;
         attachments[i].host_image =
            image ? (uintptr_t)image->base.handle.image : 0;
         attachments[i].format = image ? image->format : VK_FORMAT_UNDEFINED;
         attachments[i].extent = image ? image->extent : (VkExtent3D){ 0, 0, 0 };
         attachments[i].mip_levels = image ? image->mip_levels : 0;
         attachments[i].array_layers = image ? image->array_layers : 0;
      }
   }
   const uint32_t width = info->width;
   const uint32_t height = info->height;
   const uint32_t layers = info->layers;
   struct vkr_framebuffer *framebuffer =
      vkr_framebuffer_create_and_add(dispatch->data, args);
#ifdef __OHOS__
   if (framebuffer && store) {
      framebuffer->winehua_create_info_valid = true;
      framebuffer->winehua_attachment_count = stored;
      memcpy(framebuffer->winehua_attachments, views,
             sizeof(framebuffer->winehua_attachments));
      framebuffer->winehua_render_pass = host_pass;
      framebuffer->winehua_width = info->width;
      framebuffer->winehua_height = info->height;
      framebuffer->winehua_layers = info->layers;
      framebuffer->winehua_private_fb = framebuffer->base.handle.framebuffer;
   }
#endif
   vkr_winehua_log_framebuffer_create(framebuffer, render_pass_id,
                                      host_render_pass, attachment_count,
                                      width, height, layers, attachments);
   free(attachments);
}

static void
vkr_dispatch_vkDestroyFramebuffer(struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkDestroyFramebuffer *args)
{
   vkr_framebuffer_destroy_and_remove(dispatch->data, args);
}

void
vkr_context_init_render_pass_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateRenderPass = vkr_dispatch_vkCreateRenderPass;
   dispatch->dispatch_vkCreateRenderPass2 = vkr_dispatch_vkCreateRenderPass2;
   dispatch->dispatch_vkDestroyRenderPass = vkr_dispatch_vkDestroyRenderPass;
   dispatch->dispatch_vkGetRenderAreaGranularity =
      vkr_dispatch_vkGetRenderAreaGranularity;
   dispatch->dispatch_vkGetRenderingAreaGranularity =
      vkr_dispatch_vkGetRenderingAreaGranularity;
}

void
vkr_context_init_framebuffer_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateFramebuffer = vkr_dispatch_vkCreateFramebuffer;
   dispatch->dispatch_vkDestroyFramebuffer = vkr_dispatch_vkDestroyFramebuffer;
}
