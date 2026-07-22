/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_RENDERER_H
#define VKR_RENDERER_H

#include "config.h"

#include <stddef.h>
#include <stdint.h>

#include "virgl_resource.h"
#include "virglrenderer.h"

#define VKR_RENDERER_THREAD_SYNC (1u << 0)
#define VKR_RENDERER_ASYNC_FENCE_CB (1u << 1)

typedef void (*vkr_renderer_retire_fence_callback_type)(uint32_t ctx_id,
                                                        uint32_t ring_idx,
                                                        uint64_t fence_id);

typedef void (*vkr_renderer_winehua_release_queue_callback_type)(
   void *queue_sync_data);

typedef int (*vkr_renderer_winehua_present_callback_type)(
   uint32_t ctx_id,
   uintptr_t instance,
   uintptr_t physical_device,
   uintptr_t device,
   uintptr_t queue,
   uint64_t image,
   uint32_t queue_family,
   uint32_t width,
   uint32_t height,
   uint32_t format,
   uint32_t layout,
   uint32_t client_pid,
   uint32_t surface_id,
   uint32_t serial,
   uint32_t flags,
   uint64_t *next_present_deadline_ns,
   vkr_renderer_winehua_release_queue_callback_type release_queue,
   void *queue_sync_data,
   void *user_data);

struct vkr_renderer_callbacks {
   virgl_log_callback_type debug_logger;
   vkr_renderer_retire_fence_callback_type retire_fence;
};

size_t
vkr_get_capset(void *capset, uint32_t flags);

bool
vkr_renderer_init(uint32_t flags, const struct vkr_renderer_callbacks *cbs);

void
vkr_renderer_fini(void);

void
vkr_renderer_set_winehua_present_callback(
   vkr_renderer_winehua_present_callback_type callback,
   void *user_data);

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
                             uint64_t *next_present_deadline_ns);

bool
vkr_renderer_create_context(uint32_t ctx_id,
                            uint32_t ctx_flags,
                            uint32_t nlen,
                            const char *name);

void
vkr_renderer_destroy_context(uint32_t ctx_id);

bool
vkr_renderer_submit_cmd(uint32_t ctx_id, void *cmd, uint32_t size);

bool
vkr_renderer_submit_fence(uint32_t ctx_id,
                          uint32_t flags,
                          uint64_t ring_idx,
                          uint64_t fence_id);

bool
vkr_renderer_create_resource(uint32_t ctx_id,
                             uint32_t res_id,
                             uint64_t blob_id,
                             uint64_t blob_size,
                             uint32_t blob_flags,
                             enum virgl_resource_fd_type *out_fd_type,
                             int *out_res_fd,
                             uint32_t *out_map_info,
                             struct virgl_resource_vulkan_info *out_vulkan_info);

bool
vkr_renderer_import_resource(uint32_t ctx_id,
                             uint32_t res_id,
                             enum virgl_resource_fd_type fd_type,
                             int fd,
                             uint64_t size);

void
vkr_renderer_destroy_resource(uint32_t ctx_id, uint32_t res_id);

#endif /* VKR_RENDERER_H */
