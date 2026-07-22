/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_pipeline.h"

#include "vkr_pipeline_gen.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

static bool
vkr_winehua_option_enabled(const char *name)
{
   const char *value = os_get_option(name);
   return value && !strcmp(value, "1");
}

/* Maleoon's Vulkan compiler mis-handles DXVK's binding-presence
 * OpSpecConstantTrue values when they flow through generated vector selects.
 * Keep this diagnostic workaround opt-in: bake boolean spec constants into
 * ordinary OpConstantTrue/False instructions before the host driver sees the
 * module. Non-boolean specialization constants and their decorations remain
 * untouched. */
static bool
vkr_freeze_bool_spec_constants(const VkShaderModuleCreateInfo *src,
                               VkShaderModuleCreateInfo *dst,
                               uint32_t **owned_code)
{
   const uint32_t *code = src ? src->pCode : NULL;
   uint32_t word_count, bound, offset, new_count;
   uint8_t *bool_ids = NULL;
   uint32_t *copy = NULL;

   if (!src || !code || src->codeSize < 20 || (src->codeSize & 3) ||
       code[0] != 0x07230203u)
      return false;
   word_count = (uint32_t)(src->codeSize / sizeof(uint32_t));
   bound = code[3];
   if (!bound || bound > 65536u) return false;
   bool_ids = calloc(bound, sizeof(*bool_ids));
   if (!bool_ids) return false;
   offset = 5;
   while (offset < word_count) {
      uint32_t inst = code[offset];
      uint16_t words = (uint16_t)(inst >> 16);
      uint16_t opcode = (uint16_t)(inst & 0xffffu);
      if (!words || offset + words > word_count) goto fail;
      if ((opcode == 48 || opcode == 49) && words >= 3 && code[offset + 1] < bound)
         bool_ids[code[offset + 2]] = 1;
      offset += words;
   }
   new_count = 5;
   offset = 5;
   while (offset < word_count) {
      uint32_t inst = code[offset];
      uint16_t words = (uint16_t)(inst >> 16);
      uint16_t opcode = (uint16_t)(inst & 0xffffu);
      if (opcode == 71 && words >= 4 && code[offset + 2] == 1 &&
          code[offset + 1] < bound && bool_ids[code[offset + 1]]) {
         offset += words;
         continue;
      }
      new_count += words;
      offset += words;
   }
   if (new_count == word_count) {
      free(bool_ids);
      return false;
   }
   copy = malloc((size_t)new_count * sizeof(*copy));
   if (!copy) goto fail;
   memcpy(copy, code, 5 * sizeof(*copy));
   new_count = 5;
   offset = 5;
   while (offset < word_count) {
      uint32_t inst = code[offset];
      uint16_t words = (uint16_t)(inst >> 16);
      uint16_t opcode = (uint16_t)(inst & 0xffffu);
      if (opcode == 71 && words >= 4 && code[offset + 2] == 1 &&
          code[offset + 1] < bound && bool_ids[code[offset + 1]]) {
         offset += words;
         continue;
      }
      memcpy(copy + new_count, code + offset, (size_t)words * sizeof(*copy));
      if (opcode == 48)
         copy[new_count] = (uint32_t)((uint32_t)words << 16) | 41u;
      else if (opcode == 49)
         copy[new_count] = (uint32_t)((uint32_t)words << 16) | 42u;
      new_count += words;
      offset += words;
   }
   *dst = *src;
   dst->codeSize = (size_t)new_count * sizeof(*copy);
   dst->pCode = copy;
   *owned_code = copy;
   free(bool_ids);
   return true;
fail:
   free(copy);
   free(bool_ids);
   return false;
}

static void
vkr_dispatch_vkCreateShaderModule(struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkCreateShaderModule *args)
{
   struct vkr_context *ctx = dispatch->data;
   const VkShaderModuleCreateInfo *original = args->pCreateInfo;
   VkShaderModuleCreateInfo frozen_info;
   uint32_t *frozen_code = NULL;
   bool frozen = false;

   /* Reject invalid codeSize.
    *
    * VkShaderModuleCreateInfo is unique in the Vulkan API (as of 2023-08-22).
    * Except in rare cases, (see the `altlen` attribute in vk.xml), for each
    * typed non-void array in Vulkan, the api specifies the array length as the
    * count of array elements. But VkShaderModuleCreateInfo has a typed array
    * (uint32_t *pCode) whose length (codeSize) is specified in bytes, not as
    * a count of uint32_t.
    *
    * Also, the Vulkan 1.3.261 spec seems confused about the size of `pCode`.
    * The spec says "codeSize is the size, in bytes, of the code pointed to by
    * pCode", and then later says "pCode must be a valid pointer to an array of
    * codeSize/4 uint32_t values".
    *
    * (FWIW, VkShaderCreateInfoEXT learned from this mistake and declared the
    * array to be typeless, `void *pCode`).
    *
    * The venus encoder/decoder believes the array size is `4 * (codeSize / 4)`
    * because the vk.xml says so. For example, if codeSize is 259, then venus
    * encodes/decodes only 256 bytes. But the native driver may try to read all
    * 259 bytes, leading to out-of-bound access. Prevent the oob access by
    * validating codeSize here.
    */
   if (args->pCreateInfo->codeSize % 4 != 0) {
      vkr_context_set_fatal(ctx);
      return;
   }

   if (vkr_winehua_option_enabled("WINEHUA_VKR_FREEZE_BOOL_SPEC")) {
      frozen = vkr_freeze_bool_spec_constants(original, &frozen_info, &frozen_code);
      if (frozen) {
         args->pCreateInfo = &frozen_info;
         vkr_log("WineHuaSampled: froze boolean specialization constants codeSize=%zu->%zu",
                 original->codeSize, frozen_info.codeSize);
      }
   }

   vkr_shader_module_create_and_add(dispatch->data, args);
   args->pCreateInfo = original;
   free(frozen_code);
}

static void
vkr_dispatch_vkDestroyShaderModule(struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkDestroyShaderModule *args)
{
   vkr_shader_module_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkCreatePipelineLayout(struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCreatePipelineLayout *args)
{
   vkr_pipeline_layout_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroyPipelineLayout(struct vn_dispatch_context *dispatch,
                                     struct vn_command_vkDestroyPipelineLayout *args)
{
   vkr_pipeline_layout_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkCreatePipelineCache(struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkCreatePipelineCache *args)
{
   vkr_pipeline_cache_create_and_add(dispatch->data, args);
}

static void
vkr_dispatch_vkDestroyPipelineCache(struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkDestroyPipelineCache *args)
{
   vkr_pipeline_cache_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetPipelineCacheData(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkGetPipelineCacheData *args)
{
   TRACE_FUNC();
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetPipelineCacheData_args_handle(args);
   args->ret = vk->GetPipelineCacheData(args->device, args->pipelineCache,
                                        args->pDataSize, args->pData);
}

static void
vkr_dispatch_vkMergePipelineCaches(UNUSED struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkMergePipelineCaches *args)
{
   TRACE_FUNC();
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkMergePipelineCaches_args_handle(args);
   args->ret = vk->MergePipelineCaches(args->device, args->dstCache, args->srcCacheCount,
                                       args->pSrcCaches);
}

static void
vkr_dispatch_vkCreateGraphicsPipelines(struct vn_dispatch_context *dispatch,
                                       struct vn_command_vkCreateGraphicsPipelines *args)
{
   TRACE_FUNC();
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct object_array arr;

   if (vkr_graphics_pipeline_create_array(ctx, args, &arr) < VK_SUCCESS)
      return;

   vkr_pipeline_add_array(ctx, dev, &arr, args->pPipelines);
}

static void
vkr_dispatch_vkCreateComputePipelines(struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkCreateComputePipelines *args)
{
   TRACE_FUNC();
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct object_array arr;

   if (vkr_winehua_option_enabled("WINEHUA_VKR_TRACE_SAMPLED")) {
      for (uint32_t i = 0; i < args->createInfoCount; i++) {
         const VkSpecializationInfo *spec = args->pCreateInfos[i].stage.pSpecializationInfo;
         if (!spec) {
            vkr_log("WineHuaSampled: compute-pipeline[%u] specialization=null", i);
            continue;
         }
         vkr_log("WineHuaSampled: compute-pipeline[%u] specialization mapCount=%u dataSize=%zu",
                 i, spec->mapEntryCount, spec->dataSize);
         for (uint32_t j = 0; j < spec->mapEntryCount; j++) {
            const VkSpecializationMapEntry *entry = &spec->pMapEntries[j];
            uint32_t value = 0;
            if (entry->offset + sizeof(value) <= spec->dataSize)
               memcpy(&value, (const uint8_t *)spec->pData + entry->offset, sizeof(value));
            vkr_log("WineHuaSampled: compute-pipeline[%u] specialization[%u] id=%u offset=%zu size=%zu value=0x%x",
                    i, j, entry->constantID, entry->offset, entry->size, value);
         }
      }
   }

   if (vkr_compute_pipeline_create_array(ctx, args, &arr) < VK_SUCCESS)
      return;

   vkr_pipeline_add_array(ctx, dev, &arr, args->pPipelines);
}

static void
vkr_dispatch_vkDestroyPipeline(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkDestroyPipeline *args)
{
   vkr_pipeline_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkCreateRayTracingPipelinesKHR(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkCreateRayTracingPipelinesKHR *args)
{
   TRACE_FUNC();
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct object_array arr;

   if (vkr_ray_tracing_pipeline_create_array(ctx, args, &arr) < VK_SUCCESS)
      return;

   vkr_pipeline_add_array(ctx, dev, &arr, args->pPipelines);
}

static void
vkr_dispatch_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR_args_handle(args);
   args->ret = vk->GetRayTracingCaptureReplayShaderGroupHandlesKHR(
      args->device, args->pipeline, args->firstGroup, args->groupCount, args->dataSize,
      args->pData);
}

static void
vkr_dispatch_vkGetRayTracingShaderGroupHandlesKHR(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetRayTracingShaderGroupHandlesKHR *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetRayTracingShaderGroupHandlesKHR_args_handle(args);
   args->ret = vk->GetRayTracingShaderGroupHandlesKHR(args->device, args->pipeline,
                                                      args->firstGroup, args->groupCount,
                                                      args->dataSize, args->pData);
}

static void
vkr_dispatch_vkGetRayTracingShaderGroupStackSizeKHR(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetRayTracingShaderGroupStackSizeKHR *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetRayTracingShaderGroupStackSizeKHR_args_handle(args);
   args->ret = vk->GetRayTracingShaderGroupStackSizeKHR(args->device, args->pipeline,
                                                        args->group, args->groupShader);
}

void
vkr_context_init_shader_module_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateShaderModule = vkr_dispatch_vkCreateShaderModule;
   dispatch->dispatch_vkDestroyShaderModule = vkr_dispatch_vkDestroyShaderModule;
}

void
vkr_context_init_pipeline_layout_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreatePipelineLayout = vkr_dispatch_vkCreatePipelineLayout;
   dispatch->dispatch_vkDestroyPipelineLayout = vkr_dispatch_vkDestroyPipelineLayout;
}

void
vkr_context_init_pipeline_cache_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreatePipelineCache = vkr_dispatch_vkCreatePipelineCache;
   dispatch->dispatch_vkDestroyPipelineCache = vkr_dispatch_vkDestroyPipelineCache;
   dispatch->dispatch_vkGetPipelineCacheData = vkr_dispatch_vkGetPipelineCacheData;
   dispatch->dispatch_vkMergePipelineCaches = vkr_dispatch_vkMergePipelineCaches;
}

void
vkr_context_init_pipeline_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateGraphicsPipelines = vkr_dispatch_vkCreateGraphicsPipelines;
   dispatch->dispatch_vkCreateComputePipelines = vkr_dispatch_vkCreateComputePipelines;
   dispatch->dispatch_vkDestroyPipeline = vkr_dispatch_vkDestroyPipeline;

   /* VK_KHR_ray_tracing_pipeline */
   dispatch->dispatch_vkCreateRayTracingPipelinesKHR =
      vkr_dispatch_vkCreateRayTracingPipelinesKHR;
   dispatch->dispatch_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR =
      vkr_dispatch_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR;
   dispatch->dispatch_vkGetRayTracingShaderGroupHandlesKHR =
      vkr_dispatch_vkGetRayTracingShaderGroupHandlesKHR;
   dispatch->dispatch_vkGetRayTracingShaderGroupStackSizeKHR =
      vkr_dispatch_vkGetRayTracingShaderGroupStackSizeKHR;
}
