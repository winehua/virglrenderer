/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_pipeline.h"

#include "vkr_pipeline_gen.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

static bool
vkr_winehua_option_enabled(const char *name)
{
   const char *value = os_get_option(name);
   return value && !strcmp(value, "1");
}

static uint32_t
vkr_winehua_fnv1a32(const void *data, size_t size)
{
   const uint8_t *bytes = data;
   uint32_t hash = 2166136261u;

   for (size_t i = 0; i < size; i++) {
      hash ^= bytes[i];
      hash *= 16777619u;
   }
   return hash;
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
   struct vkr_shader_module *obj;
   const bool trace = vkr_winehua_option_enabled("WINEHUA_VKR_TRACE_PIPELINE");

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

   const VkShaderModuleCreateInfo *effective = args->pCreateInfo;
   const uint32_t word_count = (uint32_t)(effective->codeSize / sizeof(uint32_t));
   const uint32_t code_hash = vkr_winehua_fnv1a32(effective->pCode,
                                                   effective->codeSize);
   const uint32_t first_word = word_count ? effective->pCode[0] : 0;
   const uint32_t last_word = word_count ? effective->pCode[word_count - 1] : 0;

   obj = vkr_shader_module_create_and_add(dispatch->data, args);
   if (obj) {
      obj->winehua_code_hash = code_hash;
      obj->winehua_code_size = (uint32_t)effective->codeSize;
      obj->winehua_first_word = first_word;
      obj->winehua_last_word = last_word;
   }
   if (trace) {
      vkr_log("WineHuaShader: create result=%d objectId=%" PRIu64
              " hostModule=0x%" PRIxPTR " codeSize=%zu fnv1a32=0x%08x"
              " first=0x%08x last=0x%08x frozen=%u",
              args->ret, obj ? obj->base.id : 0,
              obj ? (uintptr_t)obj->base.handle.shader_module : 0,
              effective->codeSize, code_hash, first_word, last_word,
              frozen ? 1u : 0u);
   }
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
   const bool trace = vkr_winehua_option_enabled("WINEHUA_VKR_TRACE_PIPELINE");

   if (trace) {
      for (uint32_t i = 0; i < args->createInfoCount; i++) {
         const VkGraphicsPipelineCreateInfo *info = &args->pCreateInfos[i];
         vkr_log("WineHuaPipeline: create[%u] stages=%u layout=%llu renderPass=%llu subpass=%u vertexInput=%p inputAssembly=%p viewport=%p raster=%p multisample=%p blend=%p dynamic=%p",
                 i, info->stageCount,
                 (unsigned long long)(uintptr_t)info->layout,
                 (unsigned long long)(uintptr_t)info->renderPass,
                 info->subpass, (const void *)info->pVertexInputState,
                 (const void *)info->pInputAssemblyState,
                 (const void *)info->pViewportState,
                 (const void *)info->pRasterizationState,
                 (const void *)info->pMultisampleState,
                 (const void *)info->pColorBlendState,
                 (const void *)info->pDynamicState);
         for (const VkBaseInStructure *next =
                 (const VkBaseInStructure *)info->pNext;
              next; next = next->pNext) {
            vkr_log("WineHuaPipeline: create[%u].pNext sType=%u ptr=%p",
                    i, next->sType, (const void *)next);
            if (next->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
               const VkPipelineRenderingCreateInfo *rendering =
                  (const VkPipelineRenderingCreateInfo *)next;
               vkr_log("WineHuaPipeline: create[%u].rendering viewMask=0x%x colors=%u depthFormat=%u stencilFormat=%u",
                       i, rendering->viewMask,
                       rendering->colorAttachmentCount,
                       rendering->depthAttachmentFormat,
                       rendering->stencilAttachmentFormat);
               if (rendering->colorAttachmentCount &&
                   !rendering->pColorAttachmentFormats) {
                  vkr_log("WineHuaPipeline: create[%u].rendering colorFormats=null", i);
               } else {
                  for (uint32_t j = 0;
                       j < rendering->colorAttachmentCount; j++) {
                     vkr_log("WineHuaPipeline: create[%u].rendering colorFormat[%u]=%u",
                             i, j, rendering->pColorAttachmentFormats[j]);
                  }
               }
            }
         }
         if (info->pRasterizationState) {
            const VkPipelineRasterizationStateCreateInfo *raster =
               info->pRasterizationState;
            vkr_log("WineHuaPipeline: create[%u].raster depthClamp=%u discard=%u polygon=%u cull=0x%x front=%u depthBias=%u lineWidth=%g",
                    i, raster->depthClampEnable,
                    raster->rasterizerDiscardEnable, raster->polygonMode,
                    raster->cullMode, raster->frontFace,
                    raster->depthBiasEnable, raster->lineWidth);
         }
         if (info->pDepthStencilState) {
            const VkPipelineDepthStencilStateCreateInfo *depth =
               info->pDepthStencilState;
            vkr_log("WineHuaPipeline: create[%u].depth test=%u write=%u compare=%u bounds=%u stencil=%u",
                    i, depth->depthTestEnable, depth->depthWriteEnable,
                    depth->depthCompareOp, depth->depthBoundsTestEnable,
                    depth->stencilTestEnable);
         }
         if (info->pColorBlendState) {
            const VkPipelineColorBlendStateCreateInfo *blend =
               info->pColorBlendState;
            vkr_log("WineHuaPipeline: create[%u].blend flags=0x%x logicEnable=%u logicOp=%u attachmentCount=%u constants=%g,%g,%g,%g",
                    i, blend->flags, blend->logicOpEnable, blend->logicOp,
                    blend->attachmentCount, blend->blendConstants[0],
                    blend->blendConstants[1], blend->blendConstants[2],
                    blend->blendConstants[3]);
            if (blend->attachmentCount && !blend->pAttachments) {
               vkr_log("WineHuaPipeline: create[%u].blend attachments=null", i);
            } else {
               for (uint32_t j = 0; j < blend->attachmentCount; j++) {
                  const VkPipelineColorBlendAttachmentState *attachment =
                     &blend->pAttachments[j];
                  const bool dual_src =
                     (attachment->srcColorBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR &&
                      attachment->srcColorBlendFactor <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA) ||
                     (attachment->dstColorBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR &&
                      attachment->dstColorBlendFactor <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA) ||
                     (attachment->srcAlphaBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR &&
                      attachment->srcAlphaBlendFactor <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA) ||
                     (attachment->dstAlphaBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR &&
                      attachment->dstAlphaBlendFactor <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA);
                  vkr_log("WineHuaPipeline: create[%u].blend[%u] enable=%u color=%u,%u,%u alpha=%u,%u,%u writeMask=0x%x dualSrc=%u",
                          i, j, attachment->blendEnable,
                          attachment->srcColorBlendFactor,
                          attachment->dstColorBlendFactor,
                          attachment->colorBlendOp,
                          attachment->srcAlphaBlendFactor,
                          attachment->dstAlphaBlendFactor,
                          attachment->alphaBlendOp,
                          attachment->colorWriteMask, dual_src ? 1u : 0u);
               }
            }
         }
         if (info->pDynamicState) {
            const VkPipelineDynamicStateCreateInfo *dynamic = info->pDynamicState;
            vkr_log("WineHuaPipeline: create[%u].dynamic flags=0x%x count=%u",
                    i, dynamic->flags, dynamic->dynamicStateCount);
            if (dynamic->dynamicStateCount && !dynamic->pDynamicStates) {
               vkr_log("WineHuaPipeline: create[%u].dynamic states=null", i);
            } else {
               for (uint32_t j = 0; j < dynamic->dynamicStateCount; j++)
                  vkr_log("WineHuaPipeline: create[%u].dynamic[%u]=%u",
                          i, j, dynamic->pDynamicStates[j]);
            }
         }
         for (uint32_t j = 0; j < info->stageCount; j++) {
            const struct vkr_shader_module *shader =
               vkr_shader_module_from_handle(info->pStages[j].module);
            const VkSpecializationInfo *spec =
               info->pStages[j].pSpecializationInfo;
            vkr_log("WineHuaPipeline: create[%u].stage[%u] flags=0x%x"
                    " objectId=%" PRIu64 " hostModule=0x%" PRIxPTR
                    " codeSize=%u fnv1a32=0x%08x first=0x%08x last=0x%08x"
                    " entry=%s spec=%p",
                    i, j, info->pStages[j].stage,
                    shader ? shader->base.id : 0,
                    shader ? (uintptr_t)shader->base.handle.shader_module : 0,
                    shader ? shader->winehua_code_size : 0,
                    shader ? shader->winehua_code_hash : 0,
                    shader ? shader->winehua_first_word : 0,
                    shader ? shader->winehua_last_word : 0,
                    info->pStages[j].pName ? info->pStages[j].pName : "",
                    (const void *)spec);
            if (!spec) {
               vkr_log("WineHuaPipeline: create[%u].stage[%u] specialization=null",
                       i, j);
               continue;
            }
            vkr_log("WineHuaPipeline: create[%u].stage[%u] specialization mapEntryCount=%u dataSize=%zu pData=%p",
                    i, j, spec->mapEntryCount, spec->dataSize, spec->pData);
            if (spec->mapEntryCount && !spec->pMapEntries) {
               vkr_log("WineHuaPipeline: create[%u].stage[%u] specialization mapEntries=null",
                       i, j);
               continue;
            }
            for (uint32_t k = 0; k < spec->mapEntryCount; k++) {
               const VkSpecializationMapEntry *entry = &spec->pMapEntries[k];
               uint64_t value = 0;
               const bool valid = spec->pData && entry->size <= sizeof(value) &&
                  entry->offset <= spec->dataSize &&
                  entry->size <= spec->dataSize - entry->offset;
               if (valid)
                  memcpy(&value, (const uint8_t *)spec->pData + entry->offset,
                         entry->size);
               vkr_log("WineHuaPipeline: create[%u].stage[%u] specialization[%u] constantID=%u offset=%u size=%zu value=0x%016" PRIx64 " valid=%u",
                       i, j, k, entry->constantID, entry->offset, entry->size,
                       value, valid ? 1u : 0u);
            }
         }
      }
   }

   if (vkr_graphics_pipeline_create_array(ctx, args, &arr) < VK_SUCCESS) {
      if (trace)
         vkr_log("WineHuaPipeline: vkCreateGraphicsPipelines returned %d",
                 args->ret);
      return;
   }

   if (trace)
      vkr_log("WineHuaPipeline: vkCreateGraphicsPipelines returned %d",
              args->ret);

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
