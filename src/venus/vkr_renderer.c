/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_common.h"

#include <stdio.h>

#include "venus-protocol/vn_protocol_renderer_info.h"
#include "virtgpu_drm.h"
#include "venus_hw.h"

#include "vkr_context.h"
#include "vkr_device.h"
#include "vkr_image.h"
#include "vkr_instance.h"
#include "vkr_physical_device.h"
#include "vkr_queue.h"

#include <sched.h>
#include <time.h>

struct vkr_renderer_state {
   const struct vkr_renderer_callbacks *cbs;

   /* track the vkr_context */
   mtx_t context_mutex;
   struct list_head contexts;
};

struct vkr_renderer_state vkr_state;
static struct vkr_context *vkr_context_cache[64];

static void
vkr_winehua_stage(const char *stage)
{
   const char *path = getenv("WINEHUA_VIRGL_LOG_PATH");
   if (!path || !path[0])
      return;
   FILE *file = fopen(path, "a");
   if (!file)
      return;
   fprintf(file, "[vkr-present] %s\\n", stage);
   fflush(file);
   fclose(file);
}

static bool
vkr_winehua_trylock(mtx_t *mutex, const char *busy_stage)
{
   /* Queue submit and the present command are serviced by different Venus
    * workers.  Returning EAGAIN on the first object-mutex collision drops the
    * frame before it ever reaches the OHNativeWindow and leaves a white
    * swapchain.  Wait briefly for the in-flight command to retire, but keep a
    * hard bound so a genuinely dead context cannot hang the render server. */
   for (unsigned i = 0; i < 200; i++) {
      if (mtx_trylock(mutex) == thrd_success)
         return true;
      const struct timespec delay = {0, 1000000};
      nanosleep(&delay, NULL);
   }
   vkr_winehua_stage(busy_stage);
   return false;
}

static vkr_renderer_winehua_present_callback_type
   vkr_winehua_present_callback;
static void *vkr_winehua_present_callback_data;

size_t
vkr_get_capset(void *capset, uint32_t flags)
{
   struct virgl_renderer_capset_venus *c = capset;
   if (c) {
      memset(c, 0, sizeof(*c));
      c->wire_format_version = vn_info_wire_format_version();
      c->vk_xml_version = vn_info_vk_xml_version();
      c->vk_ext_command_serialization_spec_version =
         vkr_extension_get_spec_version("VK_EXT_command_serialization");
      c->vk_mesa_venus_protocol_spec_version =
         vkr_extension_get_spec_version("VK_MESA_venus_protocol");
      /* After https://gitlab.freedesktop.org/virgl/virglrenderer/-/merge_requests/688,
       * this flag is used to indicate render server config.
       */
      c->supports_blob_id_0 = true;

      uint32_t ext_mask[VN_INFO_EXTENSION_MAX_NUMBER / 32 + 1] = { 0 };
      vn_info_extension_mask_init(ext_mask);

      static_assert(sizeof(ext_mask) <= sizeof(c->vk_extension_mask1),
                    "Time to extend venus capset with vk_extension_mask2");
      memcpy(c->vk_extension_mask1, ext_mask, sizeof(ext_mask));

      /* set bit 0 to enable the extension mask(s) */
      assert(!(c->vk_extension_mask1[0] & 0x1u));
      c->vk_extension_mask1[0] |= 0x1u;

      c->allow_vk_wait_syncs = 1;
      c->supports_multiple_timelines = 1;

      c->use_guest_vram = (bool)(flags & VIRGL_RENDERER_USE_GUEST_VRAM);
   }

   return sizeof(*c);
}

bool
vkr_renderer_init(uint32_t flags, const struct vkr_renderer_callbacks *cbs)
{
   TRACE_INIT();
   TRACE_FUNC();

   static const uint32_t required_flags =
      VKR_RENDERER_THREAD_SYNC | VKR_RENDERER_ASYNC_FENCE_CB;
   if ((flags & required_flags) != required_flags)
      return false;

   vkr_debug_init();

   if (cbs->debug_logger)
      virgl_log_set_handler(cbs->debug_logger, NULL, NULL);

   vkr_state.cbs = cbs;
   mtx_init(&vkr_state.context_mutex, mtx_plain);
   list_inithead(&vkr_state.contexts);

   return true;
}

void
vkr_renderer_fini(void)
{
   mtx_lock(&vkr_state.context_mutex);
   list_for_each_entry_safe (struct vkr_context, ctx, &vkr_state.contexts, head)
      vkr_context_destroy(ctx);

   list_inithead(&vkr_state.contexts);
   mtx_unlock(&vkr_state.context_mutex);
   mtx_destroy(&vkr_state.context_mutex);

   vkr_state.cbs = NULL;
}

void
vkr_renderer_set_winehua_present_callback(
   vkr_renderer_winehua_present_callback_type callback,
   void *user_data)
{
   vkr_winehua_present_callback = callback;
   vkr_winehua_present_callback_data = user_data;
}

static struct vkr_context *
vkr_renderer_lookup_context(uint32_t ctx_id)
{
   for (unsigned i = 0; i < 64; i++) {
      struct vkr_context *ctx = vkr_context_cache[i];
      if (ctx && ctx->ctx_id == ctx_id)
         return ctx;
   }
   return NULL;
}

bool
vkr_renderer_create_context(uint32_t ctx_id,
                            uint32_t ctx_flags,
                            uint32_t nlen,
                            const char *name)
{
   TRACE_FUNC();

   assert(ctx_id);
   assert(!(ctx_flags & ~VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK));

   if ((ctx_flags & VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK) !=
       VIRTGPU_DRM_CAPSET_VENUS)
      return false;

   /* duplicate ctx creation between server and vkr is invalid */
   mtx_lock(&vkr_state.context_mutex);
   struct vkr_context *ctx = vkr_renderer_lookup_context(ctx_id);
   if (ctx) {
      mtx_unlock(&vkr_state.context_mutex);
      return false;
   }

   ctx = vkr_context_create(ctx_id, vkr_state.cbs->retire_fence, nlen, name);
   if (!ctx) {
      mtx_unlock(&vkr_state.context_mutex);
      return false;
   }

   list_addtail(&ctx->head, &vkr_state.contexts);
   for (unsigned i = 0; i < 64; i++) {
      if (!vkr_context_cache[i]) {
         vkr_context_cache[i] = ctx;
         break;
      }
   }
   mtx_unlock(&vkr_state.context_mutex);

   return true;
}

void
vkr_renderer_destroy_context(uint32_t ctx_id)
{
   TRACE_FUNC();

   mtx_lock(&vkr_state.context_mutex);
   struct vkr_context *ctx = vkr_renderer_lookup_context(ctx_id);
   if (!ctx) {
      mtx_unlock(&vkr_state.context_mutex);
      return;
   }

   list_del(&ctx->head);
   for (unsigned i = 0; i < 64; i++) {
      if (vkr_context_cache[i] == ctx)
         vkr_context_cache[i] = NULL;
   }
   vkr_context_destroy(ctx);
   mtx_unlock(&vkr_state.context_mutex);
}

int
vkr_renderer_winehua_present(uint32_t ctx_id,
                             uint64_t queue_id,
                             uint64_t image_id,
                             uint32_t width,
                             uint32_t height,
                             uint32_t format,
                             uint32_t layout,
                             uint32_t client_pid,
                             uint32_t surface_id,
                             uint32_t serial,
                             uint32_t flags,
                             uint64_t *next_present_deadline_ns)
{
   vkr_winehua_stage("enter");
   if (next_present_deadline_ns)
      *next_present_deadline_ns = 0;
   if (!ctx_id || !queue_id || !image_id || !width || !height ||
       !client_pid || !surface_id || flags)
      return -EINVAL;

   switch ((VkImageLayout)layout) {
   case VK_IMAGE_LAYOUT_GENERAL:
   case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
   case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
   case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
      break;
   default:
      return -EINVAL;
   }

   vkr_winehua_stage("context-lock");
   if (!vkr_winehua_trylock(&vkr_state.context_mutex, "context-busy")) {
      vkr_winehua_stage("context-busy");
      return -EAGAIN;
   }
   vkr_winehua_stage("context-locked");
   struct vkr_context *ctx = vkr_renderer_lookup_context(ctx_id);
   if (!ctx) {
      vkr_winehua_stage("context-missing");
      mtx_unlock(&vkr_state.context_mutex);
      return -ESRCH;
   }

   vkr_winehua_stage("object-lock");
   if (!vkr_winehua_trylock(&ctx->object_mutex, "object-busy")) {
      vkr_winehua_stage("object-busy");
      mtx_unlock(&vkr_state.context_mutex);
      return -EAGAIN;
   }
   vkr_winehua_stage("object-locked");
   const struct hash_entry *queue_entry =
      _mesa_hash_table_search(ctx->object_table, &queue_id);
   const struct hash_entry *image_entry =
      _mesa_hash_table_search(ctx->object_table, &image_id);
   struct vkr_object *queue_obj = queue_entry ? queue_entry->data : NULL;
   struct vkr_object *image_obj = image_entry ? image_entry->data : NULL;
   if (!queue_obj || queue_obj->type != VK_OBJECT_TYPE_QUEUE ||
       !image_obj || image_obj->type != VK_OBJECT_TYPE_IMAGE) {
      vkr_winehua_stage(!queue_obj ? "queue-missing" :
                        !image_obj ? "image-missing" : "object-type-mismatch");
      mtx_unlock(&ctx->object_mutex);
      mtx_unlock(&vkr_state.context_mutex);
      /* Object commands and the private socket command are asynchronous.  A
       * transient miss must request retry/suboptimal handling, never poison
       * the DXVK device as a permanent object lookup failure. */
      return -EAGAIN;
   }

   struct vkr_queue *queue = (struct vkr_queue *)queue_obj;
   struct vkr_image *image = (struct vkr_image *)image_obj;
   struct vkr_device *dev = queue->device;
   struct vkr_physical_device *physical_dev =
      dev ? dev->physical_device : NULL;
   struct vkr_instance *instance =
      physical_dev ? physical_dev->instance : NULL;
   const bool image_matches =
      image->device == dev && image->image_type == VK_IMAGE_TYPE_2D &&
      image->extent.width == width && image->extent.height == height &&
      image->extent.depth == 1 && image->format == (VkFormat)format &&
      image->mip_levels >= 1 && image->array_layers >= 1 &&
      image->samples == VK_SAMPLE_COUNT_1_BIT &&
      (image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
   if (!dev || !physical_dev || !instance || !image_matches) {
      mtx_unlock(&ctx->object_mutex);
      mtx_unlock(&vkr_state.context_mutex);
      return -EINVAL;
   }

   if (!vkr_winehua_present_callback) {
      mtx_unlock(&ctx->object_mutex);
      mtx_unlock(&vkr_state.context_mutex);
      return -ENOSYS;
   }

   vkr_winehua_stage("queue-lock");
   if (!vkr_winehua_trylock(&queue->vk_mutex, "queue-busy")) {
      mtx_unlock(&ctx->object_mutex);
      mtx_unlock(&vkr_state.context_mutex);
      return -EAGAIN;
   }
   vkr_winehua_stage("queue-locked");

   const uintptr_t instance_handle = (uintptr_t)instance->base.handle.instance;
   const uintptr_t physical_device_handle =
      (uintptr_t)physical_dev->base.handle.physical_device;
   const uintptr_t device_handle = (uintptr_t)dev->base.handle.device;
   const uintptr_t queue_handle = (uintptr_t)queue->base.handle.queue;
   const uint64_t image_handle = (uint64_t)(uintptr_t)image->base.handle.image;
   const uint32_t queue_family = queue->family;
   vkr_winehua_stage("handles-ready");

   /* Keep the queue externally synchronized with renderer QueueSubmit,
    * QueueSubmit2, QueueBindSparse and sync submissions. The callback calls
    * the host Vulkan driver directly and does not re-enter Venus. Object and
    * context locks are released first so unrelated renderer work can proceed
    * while the platform compositor blocks in vkQueuePresentKHR. */
   mtx_unlock(&ctx->object_mutex);
   mtx_unlock(&vkr_state.context_mutex);
   vkr_winehua_stage("object-locks-released");

   const int ret = vkr_winehua_present_callback(
      ctx_id, instance_handle, physical_device_handle, device_handle,
      queue_handle, image_handle, queue_family, width, height, format, layout,
      client_pid, surface_id, serial, flags, next_present_deadline_ns,
      vkr_winehua_present_callback_data);
   mtx_unlock(&queue->vk_mutex);
   vkr_winehua_stage("queue-unlocked");
   return ret;
}

bool
vkr_renderer_submit_cmd(uint32_t ctx_id, void *cmd, uint32_t size)
{
   TRACE_FUNC();

   struct vkr_context *ctx = vkr_renderer_lookup_context(ctx_id);
   if (!ctx)
      return false;

   return vkr_context_submit_cmd(ctx, cmd, size);
}

bool
vkr_renderer_submit_fence(uint32_t ctx_id,
                          uint32_t flags,
                          uint64_t ring_idx,
                          uint64_t fence_id)
{
   TRACE_FUNC();

   struct vkr_context *ctx = vkr_renderer_lookup_context(ctx_id);
   if (!ctx)
      return false;

   assert(vkr_state.cbs->retire_fence);
   return vkr_context_submit_fence(ctx, flags, ring_idx, fence_id);
}

bool
vkr_renderer_create_resource(uint32_t ctx_id,
                             uint32_t res_id,
                             uint64_t blob_id,
                             uint64_t blob_size,
                             uint32_t blob_flags,
                             enum virgl_resource_fd_type *out_fd_type,
                             int *out_res_fd,
                             uint32_t *out_map_info,
                             struct virgl_resource_vulkan_info *out_vulkan_info)
{
   TRACE_FUNC();

   assert(res_id);
   assert(blob_size);

   struct vkr_context *ctx = vkr_renderer_lookup_context(ctx_id);
   if (!ctx)
      return false;

   struct virgl_context_blob blob;
   if (!vkr_context_create_resource(ctx, res_id, blob_id, blob_size, blob_flags, &blob))
      return false;

   assert(blob.type == VIRGL_RESOURCE_FD_SHM || blob.type == VIRGL_RESOURCE_FD_DMABUF ||
          blob.type == VIRGL_RESOURCE_FD_OPAQUE);

   *out_fd_type = blob.type;
   *out_res_fd = blob.u.fd;
   *out_map_info = blob.map_info;

   if (blob.type == VIRGL_RESOURCE_FD_OPAQUE) {
      assert(out_vulkan_info);
      *out_vulkan_info = blob.vulkan_info;
   }

   return true;
}

bool
vkr_renderer_import_resource(uint32_t ctx_id,
                             uint32_t res_id,
                             enum virgl_resource_fd_type fd_type,
                             int fd,
                             uint64_t size)
{
   TRACE_FUNC();

   assert(res_id);
   assert(fd_type == VIRGL_RESOURCE_FD_DMABUF || fd_type == VIRGL_RESOURCE_FD_OPAQUE);
   assert(fd >= 0);
   assert(size);

   struct vkr_context *ctx = vkr_renderer_lookup_context(ctx_id);
   if (!ctx)
      return false;

   return vkr_context_import_resource(ctx, res_id, fd_type, fd, size);
}

void
vkr_renderer_destroy_resource(uint32_t ctx_id, uint32_t res_id)
{
   TRACE_FUNC();

   struct vkr_context *ctx = vkr_renderer_lookup_context(ctx_id);
   if (ctx)
      vkr_context_destroy_resource(ctx, res_id);
}
