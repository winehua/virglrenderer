/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_device_memory.h"

#include <math.h>
#include <string.h>

#ifdef __OHOS__
#include <sys/mman.h>
#include <unistd.h>
#include "util/anon_file.h"
#endif

#include "venus-protocol/vn_protocol_renderer_transport.h"

#include "vkr_device_memory_gen.h"
#include "vkr_metal_helpers.h"
#include "vkr_physical_device.h"

static VkResult
vkr_device_memory_flush_shadow_range(struct vkr_device_memory *mem,
                                     VkDeviceSize offset,
                                     VkDeviceSize size);

static bool
vkr_get_fd_info_from_resource_info(struct vkr_context *ctx,
                                   const VkImportMemoryResourceInfoMESA *res_info,
                                   VkImportMemoryFdInfoKHR *out)
{
   struct vkr_resource *res = vkr_context_get_resource(ctx, res_info->resourceId);
   if (!res) {
      vkr_log("failed to import resource: invalid res_id %u", res_info->resourceId);
      vkr_context_set_fatal(ctx);
      return false;
   }

   VkExternalMemoryHandleTypeFlagBits handle_type;
   switch (res->fd_type) {
   case VIRGL_RESOURCE_FD_DMABUF:
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      break;
   case VIRGL_RESOURCE_FD_OPAQUE:
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      break;
   default:
      return false;
   }

   int fd = os_dupfd_cloexec(res->u.fd);
   if (fd < 0)
      return false;

   *out = (VkImportMemoryFdInfoKHR){
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = res_info->pNext,
      .fd = fd,
      .handleType = handle_type,
   };
   return true;
}

#if defined(HAVE_LINUX_UDMABUF_H) && defined(HAVE_MEMFD_CREATE)
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

static VkResult
vkr_udmabuf_get_fd_info_from_allocation_info(struct vkr_physical_device *physical_dev,
                                             const VkMemoryAllocateInfo *alloc_info,
                                             int *out_udmabuf_fd,
                                             VkImportMemoryFdInfoKHR *out_fd_info)
{
   int memfd = -1;
   int udmabuf_fd = -1;
   int fd = -1;

   memfd = memfd_create("vkr-udmabuf", MFD_CLOEXEC | MFD_ALLOW_SEALING);
   if (memfd < 0) {
      vkr_log("memfd_create failed (%s)", strerror(errno));
      goto fail;
   }

   const size_t size = align(alloc_info->allocationSize, getpagesize());
   int ret = ftruncate(memfd, size);
   if (ret) {
      vkr_log("ftruncate failed (%s)", strerror(errno));
      goto fail;
   }

   ret = fcntl(memfd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW);
   if (ret) {
      vkr_log("fcntl F_ADD_SEALS failed (%s)", strerror(errno));
      goto fail;
   }

   const struct udmabuf_create create = {
      .memfd = memfd,
      .flags = UDMABUF_FLAGS_CLOEXEC,
      .size = size,
   };
   udmabuf_fd = ioctl(physical_dev->udmabuf_dev_fd, UDMABUF_CREATE, &create);
   if (udmabuf_fd < 0) {
      vkr_log("ioctl UDMABUF_CREATE failed (%s)", strerror(errno));
      goto fail;
   }

   fd = os_dupfd_cloexec(udmabuf_fd);
   if (fd < 0) {
      vkr_log("os_dupfd_cloexec failed (%s)", strerror(errno));
      goto fail;
   }

   close(memfd);

   *out_udmabuf_fd = udmabuf_fd;
   *out_fd_info = (VkImportMemoryFdInfoKHR){
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = alloc_info->pNext,
      .fd = fd,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };

   return VK_SUCCESS;

fail:
   if (udmabuf_fd >= 0)
      close(udmabuf_fd);
   if (memfd >= 0)
      close(memfd);
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

#else  /* HAVE_LINUX_UDMABUF_H && HAVE_MEMFD_CREATE */

static inline VkResult
vkr_udmabuf_get_fd_info_from_allocation_info(
   UNUSED struct vkr_physical_device *physical_dev,
   UNUSED const VkMemoryAllocateInfo *alloc_info,
   UNUSED int *out_udmabuf_fd,
   UNUSED VkImportMemoryFdInfoKHR *out_fd_info)
{
   vkr_log("udmabuf_allocation is not enabled");
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

#endif /* HAVE_LINUX_UDMABUF_H && HAVE_MEMFD_CREATE */

#ifdef ENABLE_GBM_ALLOCATION
#include <gbm.h>

#define GBM_BO_USE_SW_READ_RARELY (1 << 10)
#define GBM_BO_USE_SW_WRITE_RARELY (1 << 12)

static inline int
vkr_gbm_bo_get_fd(void *gbm_bo)
{
   assert(gbm_bo);

   /* gbm_bo_get_fd returns negative error code on failure */
   return gbm_bo_get_fd(gbm_bo);
}

static inline void
vkr_gbm_bo_destroy(void *gbm_bo)
{
   gbm_bo_destroy(gbm_bo);
}

static VkResult
vkr_gbm_get_fd_info_from_allocation_info(struct vkr_physical_device *physical_dev,
                                         const VkMemoryAllocateInfo *alloc_info,
                                         void **out_gbm_bo,
                                         VkImportMemoryFdInfoKHR *out_fd_info)
{
   const uint32_t flags =
      GBM_BO_USE_LINEAR | GBM_BO_USE_SW_READ_RARELY | GBM_BO_USE_SW_WRITE_RARELY;
   struct gbm_bo *gbm_bo;
   int fd = -1;

   assert(physical_dev->gbm_device);

   /*
    * Reject here for simplicity. Letting VkPhysicalDeviceVulkan11Properties return
    * min(maxMemoryAllocationSize, UINT32_MAX) will affect unmappable scenarios.
    */
   if (alloc_info->allocationSize > UINT32_MAX)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   /* Page alignment is used on all implementations we support. */
   const uint32_t alloc_size = align(alloc_info->allocationSize, getpagesize());
#ifdef MINIGBM
   const uint32_t format = GBM_FORMAT_R8;
   const uint32_t width = alloc_size;
   const uint32_t height = 1;
#else
   /* Mesa gbm has texture size limitations, so we can't rely on R8 here. Instead, we
    * allocate a large enough linear rgba8 buffer.
    */
   const uint32_t format = GBM_FORMAT_ABGR8888;
   const uint8_t pixel_bytes = 4;
   const uint32_t width =
      (uint32_t)ceil(sqrt((alloc_size + pixel_bytes - 1) / pixel_bytes));
   const uint32_t height = width;
#endif /* MINIGBM */

   gbm_bo = gbm_bo_create(physical_dev->gbm_device, width, height, format, flags);
   if (!gbm_bo)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   fd = vkr_gbm_bo_get_fd(gbm_bo);
   if (fd < 0) {
      vkr_gbm_bo_destroy(gbm_bo);
      return fd == -EMFILE ? VK_ERROR_TOO_MANY_OBJECTS : VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   *out_gbm_bo = (void *)gbm_bo;
   *out_fd_info = (VkImportMemoryFdInfoKHR){
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = alloc_info->pNext,
      .fd = fd,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   return VK_SUCCESS;
}

#else

static inline int
vkr_gbm_bo_get_fd(ASSERTED void *gbm_bo)
{
   vkr_log("minigbm_allocation is not enabled");
   assert(!gbm_bo);
   return -1;
}

static inline void
vkr_gbm_bo_destroy(ASSERTED void *gbm_bo)
{
   vkr_log("minigbm_allocation is not enabled");
   assert(!gbm_bo);
}

static inline VkResult
vkr_gbm_get_fd_info_from_allocation_info(UNUSED struct vkr_physical_device *physical_dev,
                                         UNUSED const VkMemoryAllocateInfo *alloc_info,
                                         UNUSED void **out_gbm_bo,
                                         UNUSED VkImportMemoryFdInfoKHR *out_fd_info)
{
   vkr_log("minigbm_allocation is not enabled");
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

#endif /* ENABLE_GBM_ALLOCATION */

static void
vkr_dispatch_vkAllocateMemory(struct vn_dispatch_context *dispatch,
                              struct vn_command_vkAllocateMemory *args)
{
   TRACE_FUNC();
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vkr_physical_device *physical_dev = dev->physical_device;

   VkMemoryAllocateInfo *alloc_info = (VkMemoryAllocateInfo *)args->pAllocateInfo;
   const uint32_t mem_type_index = alloc_info->memoryTypeIndex;
   if (unlikely(mem_type_index >= physical_dev->memory_properties.memoryTypeCount)) {
      args->ret = VK_ERROR_UNKNOWN;
      return;
   }

   /* translate VkImportMemoryResourceInfoMESA into VkImportMemoryFdInfoKHR in place */
   VkImportMemoryFdInfoKHR local_import_info = { .fd = -1 };
   VkImportMemoryResourceInfoMESA *res_info = NULL;
   VkBaseInStructure *prev_of_res_info = vkr_find_prev_struct(
      alloc_info, VK_STRUCTURE_TYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA);
   if (prev_of_res_info) {
      res_info = (VkImportMemoryResourceInfoMESA *)prev_of_res_info->pNext;
      if (!vkr_get_fd_info_from_resource_info(ctx, res_info, &local_import_info)) {
         args->ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
         return;
      }

      prev_of_res_info->pNext = (const struct VkBaseInStructure *)&local_import_info;
   }

   VkExportMemoryAllocateInfo *export_info =
      vkr_find_struct(alloc_info->pNext, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);

   /* track if driver has requested export allocation */
   const bool might_export = export_info && export_info->handleTypes;

   /* XXX Force dma_buf/opaque fd export or gbm bo import until a new extension that
    * supports direct export from host visible memory
    *
    * Most VkImage and VkBuffer are non-external while most VkDeviceMemory are external
    * if allocated with a host visible memory type. We still violate the spec by binding
    * external memory to non-external image or buffer, which needs spec changes with a
    * new extension.
    *
    * Skip forcing external if a valid VkImportMemoryResourceInfoMESA is provided, since
    * the mapping will be directly set up from the existing virgl resource.
    */
   const uint32_t property_flags =
      physical_dev->memory_properties.memoryTypes[mem_type_index].propertyFlags;
   uint32_t valid_fd_types = 0;
   int udmabuf_fd = -1;
   void *gbm_bo = NULL;
   struct vkr_mtl_shm *mtl_shm = NULL;
   VkExportMemoryAllocateInfo local_export_info;
   VkImportMemoryMetalHandleInfoEXT local_metal_import;

   if ((property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !res_info) {
      /* An implementation can support dma_buf import along with opaque fd export/import.
       * If the client driver is using external memory and requesting dma_buf, without
       * dma_buf fd export support, we must use gbm bo import path instead of forcing
       * opaque fd export. e.g. the client driver uses external memory for wsi image.
       */
      const bool no_dma_buf_export =
         !export_info ||
         !(export_info->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
      const bool force_gbm_import = !!physical_dev->gbm_device;
      const bool force_udmabuf_import = physical_dev->udmabuf_dev_fd >= 0;
      if (!(force_gbm_import || force_udmabuf_import) &&
          (physical_dev->is_dma_buf_fd_export_supported ||
           (physical_dev->is_opaque_fd_export_supported && no_dma_buf_export))) {
         const VkExternalMemoryHandleTypeFlagBits handle_type =
            physical_dev->is_dma_buf_fd_export_supported
               ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
               : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
         if (export_info) {
            export_info->handleTypes |= handle_type;
         } else {
            local_export_info = (const VkExportMemoryAllocateInfo){
               .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
               .pNext = alloc_info->pNext,
               .handleTypes = handle_type,
            };
            export_info = &local_export_info;
            alloc_info->pNext = &local_export_info;

            if (handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) {
               /* Guest virtgpu kernel aligns up blob mem size to the page boundary. No
                * matter dma-buf or opaque fd export allocation, the actual allocation
                * in most cases would follow the same padding. For dma-buf, we are able
                * to allocate and validate via lseek below. For opaque fd, Vulkan spec
                * requires to use the same size with allocation time for the mapper,
                * either vkr_allocator or vulkano, to import and populate the mapping.
                * Accessible mapping given by vkMapMemory is normally limited to just the
                * alloc size. When KVM register the mappings to guest pci bar, that
                * involves an invalid chunk towards the end of the page boundary. As a
                * result, any guest side accelerated instructions that rely on the
                * paddings can end up with Illegal instruction error. The most common
                * trigger is via memcpy'ing valid range to the guest side mapped buffer
                * memory, and then, e.g. __memcpy_avx_unaligned_erms  can hit the error.
                */
               alloc_info->allocationSize =
                  align(alloc_info->allocationSize, getpagesize());
            }
         }
      } else if (physical_dev->EXT_external_memory_metal) {
         /* Allocate shm and wrap as a MTLBuffer for import. */
         mtl_shm = vkr_mtl_shm_alloc(dev->mtl_device, alloc_info->allocationSize);
         if (!mtl_shm) {
            args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;
            return;
         }

         local_metal_import = (VkImportMemoryMetalHandleInfoEXT){
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_METAL_HANDLE_INFO_EXT,
            .pNext = alloc_info->pNext,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLBUFFER_BIT_EXT,
            .handle = mtl_shm->mtl_buffer,
         };
         alloc_info->pNext = &local_metal_import;
         alloc_info->allocationSize = mtl_shm->shm_size;
         valid_fd_types = 1 << VIRGL_RESOURCE_FD_SHM;
      } else if (physical_dev->EXT_external_memory_dma_buf) {
         /* Allocate dma_buf externally and force to import. */
         if (export_info) {
            /* Strip export info since valid_fd_types can only be dma_buf here. */
            VkBaseInStructure *prev_of_export_info = vkr_find_prev_struct(
               alloc_info, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);

            prev_of_export_info->pNext = export_info->pNext;
            export_info = NULL;
         }

         if (force_udmabuf_import) {
            /* To be noted, there are 2 limits for udmabuf:
             * - list_limit: udmabuf_create_list->count limit. Default is 1024.
             * - size_limit_mb: Max size of a dmabuf, in megabytes. Default is 64.
             */
            args->ret = vkr_udmabuf_get_fd_info_from_allocation_info(
               physical_dev, alloc_info, &udmabuf_fd, &local_import_info);
         } else {
            args->ret = vkr_gbm_get_fd_info_from_allocation_info(
               physical_dev, alloc_info, &gbm_bo, &local_import_info);
         }
         if (args->ret != VK_SUCCESS)
            return;

         alloc_info->pNext = &local_import_info;
         valid_fd_types = 1 << VIRGL_RESOURCE_FD_DMABUF;
      }
   }

   if (export_info) {
      if (export_info->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)
         valid_fd_types |= 1 << VIRGL_RESOURCE_FD_OPAQUE;
      if (export_info->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
         valid_fd_types |= 1 << VIRGL_RESOURCE_FD_DMABUF;
   }

   struct vkr_device_memory *mem = vkr_device_memory_create_and_add(ctx, args);
   if (!mem) {
      if (local_import_info.fd >= 0)
         close(local_import_info.fd);
      if (gbm_bo)
         vkr_gbm_bo_destroy(gbm_bo);
      vkr_mtl_shm_free(mtl_shm);
      return;
   }

   mem->device = dev;
   mem->might_export = might_export;
   mem->property_flags = property_flags;
   mem->valid_fd_types = valid_fd_types;
   mem->udmabuf_fd = udmabuf_fd;
   mem->gbm_bo = gbm_bo;
   mem->mtl_shm = mtl_shm;
   mem->allocation_size = alloc_info->allocationSize;
   mem->memory_type_index = mem_type_index;
#ifdef __OHOS__
   mem->shadow_fd = -1;
   mem->shadow_map = NULL;
   mem->host_map = NULL;
   mem->shadow_size = 0;
   mem->shadow_sync_count = 0;
   mem->shadow_remote_flush_count = 0;
   mem->shadow_guest_write_depth = 0;
   mem->shadow_remote_invalidate_count = 0;
   mem->shadow_remote_active = false;
   mem->shadow_host_dirty = false;
   mem->shadow_dirty_offset = 0;
   mem->shadow_dirty_size = 0;
#endif
}

static void
vkr_dispatch_vkFreeMemory(struct vn_dispatch_context *dispatch,
                          struct vn_command_vkFreeMemory *args)
{
   TRACE_FUNC();
   struct vkr_device_memory *mem = vkr_device_memory_from_handle(args->memory);
   if (!mem)
      return;

   vkr_device_memory_release(mem);
   vkr_device_memory_destroy_and_remove(dispatch->data, args);
}

static void
vkr_dispatch_vkGetDeviceMemoryCommitment(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDeviceMemoryCommitment *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDeviceMemoryCommitment_args_handle(args);
   vk->GetDeviceMemoryCommitment(args->device, args->memory,
                                 args->pCommittedMemoryInBytes);
}

static void
vkr_dispatch_vkGetDeviceMemoryOpaqueCaptureAddress(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDeviceMemoryOpaqueCaptureAddress *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDeviceMemoryOpaqueCaptureAddress_args_handle(args);
   args->ret = vk->GetDeviceMemoryOpaqueCaptureAddress(args->device, args->pInfo);
}

static void
vkr_dispatch_vkFlushMappedMemoryRanges(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkFlushMappedMemoryRanges *args)
{
   struct vkr_context *ctx = dispatch->data;
   args->ret = VK_SUCCESS;

   mtx_lock(&ctx->object_mutex);
   for (uint32_t i = 0; i < args->memoryRangeCount; i++) {
      const VkMappedMemoryRange *range = &args->pMemoryRanges[i];
      struct vkr_device_memory *mem = vkr_device_memory_from_handle(range->memory);
      if (!mem) {
         args->ret = VK_ERROR_MEMORY_MAP_FAILED;
         break;
      }

      args->ret = vkr_device_memory_flush_shadow_range(
         mem, range->offset, range->size);
      if (args->ret != VK_SUCCESS)
         break;
   }
   mtx_unlock(&ctx->object_mutex);
}

static void
vkr_dispatch_vkInvalidateMappedMemoryRanges(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkInvalidateMappedMemoryRanges *args)
{
   struct vkr_context *ctx = dispatch->data;
   args->ret = VK_SUCCESS;

   mtx_lock(&ctx->object_mutex);
   for (uint32_t i = 0; i < args->memoryRangeCount; i++) {
      struct vkr_device_memory *mem =
         vkr_device_memory_from_handle(args->pMemoryRanges[i].memory);
      if (!mem) {
         args->ret = VK_ERROR_MEMORY_MAP_FAILED;
         break;
      }
#ifdef __OHOS__
      mem->shadow_guest_write_depth++;
      mem->shadow_remote_active = true;
      const uint32_t count = mem->shadow_remote_invalidate_count++;
      if (count < 8 || !(count % 60))
         vkr_log("OHOS shadow guest write begin count=%u mem=%p depth=%u",
                 count + 1, mem, mem->shadow_guest_write_depth);
#endif
   }
   mtx_unlock(&ctx->object_mutex);
}

static void
vkr_dispatch_vkGetMemoryResourcePropertiesMESA(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetMemoryResourcePropertiesMESA *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   struct vkr_resource *res = vkr_context_get_resource(ctx, args->resourceId);
   if (!res) {
      vkr_log("failed to query resource props: invalid res_id %u", args->resourceId);
      vkr_context_set_fatal(ctx);
      return;
   }

   if (res->fd_type != VIRGL_RESOURCE_FD_DMABUF) {
      args->ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      return;
   }

   static const VkExternalMemoryHandleTypeFlagBits handle_type =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
   VkMemoryFdPropertiesKHR mem_fd_props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
      .pNext = NULL,
      .memoryTypeBits = 0,
   };
   vn_replace_vkGetMemoryResourcePropertiesMESA_args_handle(args);
   args->ret =
      vk->GetMemoryFdPropertiesKHR(args->device, handle_type, res->u.fd, &mem_fd_props);
   if (args->ret != VK_SUCCESS)
      return;

   args->pMemoryResourceProperties->memoryTypeBits = mem_fd_props.memoryTypeBits;

   VkMemoryResourceAllocationSizePropertiesMESA *alloc_size_props =
      vkr_find_struct(args->pMemoryResourceProperties->pNext,
                      VK_STRUCTURE_TYPE_MEMORY_RESOURCE_ALLOCATION_SIZE_PROPERTIES_MESA);
   if (alloc_size_props)
      alloc_size_props->allocationSize = res->size;
}

void
vkr_context_init_device_memory_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkAllocateMemory = vkr_dispatch_vkAllocateMemory;
   dispatch->dispatch_vkFreeMemory = vkr_dispatch_vkFreeMemory;
   dispatch->dispatch_vkMapMemory = NULL;
   dispatch->dispatch_vkUnmapMemory = NULL;
   dispatch->dispatch_vkFlushMappedMemoryRanges =
      vkr_dispatch_vkFlushMappedMemoryRanges;
   dispatch->dispatch_vkInvalidateMappedMemoryRanges =
      vkr_dispatch_vkInvalidateMappedMemoryRanges;
   dispatch->dispatch_vkGetDeviceMemoryCommitment =
      vkr_dispatch_vkGetDeviceMemoryCommitment;
   dispatch->dispatch_vkGetDeviceMemoryOpaqueCaptureAddress =
      vkr_dispatch_vkGetDeviceMemoryOpaqueCaptureAddress;

   dispatch->dispatch_vkGetMemoryResourcePropertiesMESA =
      vkr_dispatch_vkGetMemoryResourcePropertiesMESA;
}

void
vkr_device_memory_release(struct vkr_device_memory *mem)
{
#ifdef __OHOS__
   if (mem->host_map) {
      struct vn_device_proc_table *vk = &mem->device->proc_table;
      vk->UnmapMemory(mem->device->base.handle.device,
                      mem->base.handle.device_memory);
      mem->host_map = NULL;
   }
   if (mem->shadow_map) {
      munmap(mem->shadow_map, mem->shadow_size);
      mem->shadow_map = NULL;
   }
   if (mem->shadow_fd >= 0) {
      close(mem->shadow_fd);
      mem->shadow_fd = -1;
   }
#endif
   vkr_mtl_shm_free(mem->mtl_shm);
   if (mem->gbm_bo)
      vkr_gbm_bo_destroy(mem->gbm_bo);
   if (mem->udmabuf_fd >= 0)
      close(mem->udmabuf_fd);
}

bool
vkr_device_memory_export_blob(struct vkr_device_memory *mem,
                              uint64_t blob_size,
                              uint32_t blob_flags,
                              struct virgl_context_blob *out_blob)
{
   TRACE_FUNC();

   /* a memory can only be exported once; we don't want two resources to point
    * to the same storage.
    */
   if (mem->exported) {
      vkr_log("mem has been exported");
      return false;
   }

   uint32_t map_info = VIRGL_RENDERER_MAP_CACHE_NONE;
   if (blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE) {
      const bool visible = mem->property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      const bool coherent = mem->property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      const bool cached = mem->property_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
      if (!visible) {
         vkr_log("mem cannot support mappable blob");
         return false;
      }

      /* XXX guessed */
      map_info = (coherent && cached) ? VIRGL_RENDERER_MAP_CACHE_CACHED
                                      : VIRGL_RENDERER_MAP_CACHE_WC;
   }

   if (mem->mtl_shm && mem->mtl_shm->shm_fd >= 0) {
      mem->exported = true;
      *out_blob = (struct virgl_context_blob){
         .type = VIRGL_RESOURCE_FD_SHM,
         .u.fd = os_dupfd_cloexec(mem->mtl_shm->shm_fd),
         .map_info = map_info,
      };
      return out_blob->u.fd >= 0;
   }

   const bool can_export_dma_buf = mem->valid_fd_types & (1 << VIRGL_RESOURCE_FD_DMABUF);
   const bool can_export_opaque = mem->valid_fd_types & (1 << VIRGL_RESOURCE_FD_OPAQUE);
   enum virgl_resource_fd_type fd_type;
   VkExternalMemoryHandleTypeFlagBits handle_type;
   struct virgl_resource_vulkan_info vulkan_info;
   if (blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_CROSS_DEVICE) {
      if (!can_export_dma_buf) {
         vkr_log("mem cannot export to dma_buf for cross device blob sharing");
         return false;
      }
      fd_type = VIRGL_RESOURCE_FD_DMABUF;
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
   } else if (can_export_dma_buf) {
      /* prefer dmabuf for easier mapping? */
      fd_type = VIRGL_RESOURCE_FD_DMABUF;
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
   } else if (can_export_opaque) {
      /* prefer opaque for performance? */
      fd_type = VIRGL_RESOURCE_FD_OPAQUE;
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

      STATIC_ASSERT(sizeof(vulkan_info.device_uuid) == VK_UUID_SIZE);
      STATIC_ASSERT(sizeof(vulkan_info.driver_uuid) == VK_UUID_SIZE);

      const VkPhysicalDeviceIDProperties *id_props =
         &mem->device->physical_device->id_properties;
      memcpy(vulkan_info.device_uuid, id_props->deviceUUID, VK_UUID_SIZE);
      memcpy(vulkan_info.driver_uuid, id_props->driverUUID, VK_UUID_SIZE);

      vulkan_info.allocation_size = mem->allocation_size;
      vulkan_info.memory_type_index = mem->memory_type_index;
   } else {
#ifdef __OHOS__
      if (blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_CROSS_DEVICE) {
         vkr_log("OHOS shadow memory cannot support cross-device export");
         return false;
      }

      const uint64_t page_size = (uint64_t)getpagesize();
      const uint64_t shadow_size = align(MAX2(blob_size, mem->allocation_size), page_size);
      const bool mappable = blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE;
      if (mappable && !(mem->property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
         vkr_log("OHOS shadow requested for non-host-visible memory");
         return false;
      }

      int shadow_fd = os_create_anonymous_file(shadow_size, "vkr-ohos-shadow");
      if (shadow_fd < 0) {
         vkr_log("failed to allocate OHOS shadow fd: %s", strerror(errno));
         return false;
      }
      void *shadow_map = mmap(NULL, shadow_size, PROT_READ | PROT_WRITE,
                              MAP_SHARED, shadow_fd, 0);
      if (shadow_map == MAP_FAILED) {
         vkr_log("failed to map OHOS shadow fd: %s", strerror(errno));
         close(shadow_fd);
         return false;
      }

      struct vn_device_proc_table *vk = &mem->device->proc_table;
      void *host_map = NULL;
      if (mappable) {
         VkResult result = vk->MapMemory(mem->device->base.handle.device,
                                         mem->base.handle.device_memory,
                                         0, VK_WHOLE_SIZE, 0, &host_map);
         if (result != VK_SUCCESS) {
            vkr_log("failed to map OHOS Host Vulkan memory (%d)", result);
            munmap(shadow_map, shadow_size);
            close(shadow_fd);
            return false;
         }
      }

      int exported_fd = os_dupfd_cloexec(shadow_fd);
      if (exported_fd < 0) {
         if (host_map) {
            vk->UnmapMemory(mem->device->base.handle.device,
                            mem->base.handle.device_memory);
         }
         munmap(shadow_map, shadow_size);
         close(shadow_fd);
         return false;
      }

      mem->shadow_fd = shadow_fd;
      mem->shadow_map = shadow_map;
      mem->host_map = host_map;
      mem->shadow_size = shadow_size;
      if (host_map) {
         const bool coherent =
            mem->property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
         if (!coherent) {
            const VkMappedMemoryRange range = {
               .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
               .memory = mem->base.handle.device_memory,
               .offset = 0,
               .size = VK_WHOLE_SIZE,
            };
            vk->InvalidateMappedMemoryRanges(mem->device->base.handle.device, 1, &range);
         }
         const size_t copy_size =
            (size_t)MIN2(mem->shadow_size, mem->allocation_size);
         memcpy(mem->shadow_map, mem->host_map, copy_size);

      }
      mem->exported = true;
      *out_blob = (struct virgl_context_blob){
         .type = VIRGL_RESOURCE_FD_SHM,
         .u.fd = exported_fd,
         .map_info = mappable ? map_info : VIRGL_RENDERER_MAP_CACHE_NONE,
      };
      vkr_log("using OHOS shadow memory size=%" PRIu64
              " allocation=%" PRIu64 " flags=0x%x mappable=%d coherent=%d",
              shadow_size, mem->allocation_size, mem->property_flags, mappable,
              !!(mem->property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
      return true;
#else
      vkr_log("mem is not exportable");
      return false;
#endif
   }

   int fd;
   if (mem->udmabuf_fd >= 0) {
      fd = os_dupfd_cloexec(mem->udmabuf_fd);
      if (fd < 0) {
         vkr_log("mem udmabuf fd dup failed (%s)", strerror(errno));
         return false;
      }
   } else if (mem->gbm_bo) {
      assert(handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
      assert(can_export_dma_buf && !can_export_opaque);

      fd = vkr_gbm_bo_get_fd(mem->gbm_bo);
      if (fd < 0) {
         vkr_log("mem gbm bo export failed (ret %d)", fd);
         return false;
      }
   } else {
      struct vn_device_proc_table *vk = &mem->device->proc_table;
      const VkMemoryGetFdInfoKHR fd_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
         .memory = mem->base.handle.device_memory,
         .handleType = handle_type,
      };
      VkResult ret = vk->GetMemoryFdKHR(mem->device->base.handle.device, &fd_info, &fd);
      if (ret != VK_SUCCESS) {
         vkr_log("mem fd export failed (vk ret %d)", ret);
         return false;
      }
   }

   if (fd_type == VIRGL_RESOURCE_FD_DMABUF) {
      const off_t dma_buf_size = lseek(fd, 0, SEEK_END);
      if (dma_buf_size < 0 || (uint64_t)dma_buf_size < blob_size) {
         vkr_log("mem dma_buf_size %lld < blob_size %" PRIu64, (long long)dma_buf_size,
                 blob_size);
         close(fd);
         return false;
      }
   }

   mem->exported = true;

   *out_blob = (struct virgl_context_blob){
      .type = fd_type,
      .u.fd = fd,
      .map_info = map_info,
      .vulkan_info = vulkan_info,
   };

   return true;
}

static void
vkr_device_memory_sync_shadow(struct vkr_device_memory *mem, bool to_host)
{
#ifdef __OHOS__
   if (!mem->host_map || !mem->shadow_map || !mem->shadow_size)
      return;

   struct vn_device_proc_table *vk = &mem->device->proc_table;
   const VkMappedMemoryRange range = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = mem->base.handle.device_memory,
      .offset = 0,
      .size = VK_WHOLE_SIZE,
   };
   const bool coherent = mem->property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   const size_t copy_size = (size_t)MIN2(mem->shadow_size, mem->allocation_size);

   if (to_host && mem->shadow_remote_active) {
      if (!mem->shadow_host_dirty)
         return;

      const VkMappedMemoryRange dirty_range = {
         .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
         .memory = mem->base.handle.device_memory,
         .offset = mem->shadow_dirty_offset,
         .size = mem->shadow_dirty_size,
      };
      VkResult dirty_result = vk->FlushMappedMemoryRanges(
         mem->device->base.handle.device, 1, &dirty_range);
      if (dirty_result != VK_SUCCESS)
         vkr_log("OHOS shadow dirty flush failed result=%d offset=%" PRIu64
                 " size=%" PRIu64,
                 dirty_result, (uint64_t)mem->shadow_dirty_offset,
                 (uint64_t)mem->shadow_dirty_size);
      mem->shadow_host_dirty = false;
      return;
   }

   if (to_host) {
      size_t first_diff = copy_size;
      if (mem->shadow_sync_count < 16) {
         const uint8_t *shadow = mem->shadow_map;
         const uint8_t *host = mem->host_map;
         const size_t page_size = 4096;
         for (size_t offset = 0; offset < copy_size; offset += page_size) {
            const size_t chunk = MIN2(page_size, copy_size - offset);
            if (memcmp(shadow + offset, host + offset, chunk)) {
               size_t byte = 0;
               while (byte < chunk && shadow[offset + byte] == host[offset + byte])
                  byte++;
               first_diff = offset + byte;
               break;
            }
         }
         if (first_diff < copy_size) {
            const uint8_t *shadow = mem->shadow_map;
            const uint8_t *host = mem->host_map;
            const size_t available = MIN2((size_t)16, copy_size - first_diff);
            uint64_t shadow_word0 = 0, shadow_word1 = 0;
            uint64_t host_word0 = 0, host_word1 = 0;
            memcpy(&shadow_word0, shadow + first_diff, MIN2((size_t)8, available));
            memcpy(&host_word0, host + first_diff, MIN2((size_t)8, available));
            if (available > 8) {
               memcpy(&shadow_word1, shadow + first_diff + 8, available - 8);
               memcpy(&host_word1, host + first_diff + 8, available - 8);
            }
            vkr_log("OHOS shadow diff sync=%u mem=%p offset=%zu "
                    "shadow=%016" PRIx64 "%016" PRIx64
                    " host=%016" PRIx64 "%016" PRIx64,
                    mem->shadow_sync_count, mem, first_diff,
                    shadow_word0, shadow_word1, host_word0, host_word1);
         }
      }
      memcpy(mem->host_map, mem->shadow_map, copy_size);
      /* Maleoon advertises some host-visible heaps as coherent, but mapped
       * writes made through the OHOS shadow bridge are not always visible to
       * shader reads without an explicit cache-domain transition.  Flushing a
       * coherent range is valid Vulkan and is a no-op on conformant coherent
       * implementations, so force it for the OHOS compatibility path. */
      VkResult result =
         vk->FlushMappedMemoryRanges(mem->device->base.handle.device, 1, &range);
      if (result != VK_SUCCESS)
         vkr_log("OHOS shadow flush failed result=%d coherent=%d size=%zu",
                 result, coherent, copy_size);
      mem->shadow_sync_count++;
   } else {
      if (mem->shadow_guest_write_depth)
         return;
      VkResult result =
         vk->InvalidateMappedMemoryRanges(mem->device->base.handle.device, 1, &range);
      if (result != VK_SUCCESS)
         vkr_log("OHOS shadow invalidate failed result=%d coherent=%d size=%zu",
                 result, coherent, copy_size);
      memcpy(mem->shadow_map, mem->host_map, copy_size);
      if (mem->shadow_remote_active)
         mem->shadow_host_dirty = false;
   }
#else
   (void)mem;
   (void)to_host;
#endif
}

static VkResult
vkr_device_memory_flush_shadow_range(struct vkr_device_memory *mem,
                                     VkDeviceSize offset,
                                     VkDeviceSize size)
{
#ifdef __OHOS__
   if (!mem->host_map || !mem->shadow_map || !mem->shadow_size) {
      if (mem->shadow_guest_write_depth)
         mem->shadow_guest_write_depth--;
      return VK_SUCCESS;
   }

   if (offset > mem->allocation_size)
      return VK_ERROR_MEMORY_MAP_FAILED;

   const VkDeviceSize available = mem->allocation_size - offset;
   const VkDeviceSize copy_size = size == VK_WHOLE_SIZE
      ? available
      : MIN2(size, available);

   memcpy((uint8_t *)mem->host_map + offset,
          (const uint8_t *)mem->shadow_map + offset,
          (size_t)copy_size);

   mem->shadow_remote_active = true;
   if (!mem->shadow_host_dirty) {
      mem->shadow_dirty_offset = offset;
      mem->shadow_dirty_size = copy_size;
      mem->shadow_host_dirty = true;
   } else {
      const VkDeviceSize dirtyBegin = MIN2(mem->shadow_dirty_offset, offset);
      const VkDeviceSize dirtyEnd = MAX2(
         mem->shadow_dirty_offset + mem->shadow_dirty_size,
         offset + copy_size);
      mem->shadow_dirty_offset = dirtyBegin;
      mem->shadow_dirty_size = dirtyEnd - dirtyBegin;
   }

   if (mem->shadow_guest_write_depth)
      mem->shadow_guest_write_depth--;

   const uint32_t flush_count = mem->shadow_remote_flush_count++;
   if (flush_count < 8 || !(flush_count % 60))
      vkr_log("OHOS shadow remote flush count=%u mem=%p offset=%" PRIu64
              " size=%" PRIu64 " result=0",
              flush_count + 1, mem, (uint64_t)offset,
              (uint64_t)copy_size);

   /* The queue-submit path performs the actual Host Vulkan cache-domain
    * flush once per submit.  Doing it here for every 64-byte slice is
    * correct but disproportionately expensive on Maleoon. */
   return VK_SUCCESS;
#else
   (void)mem;
   (void)offset;
   (void)size;
   return VK_SUCCESS;
#endif
}
static void
vkr_device_memory_sync_shadows(struct vkr_context *ctx, bool to_host)
{
#ifdef __OHOS__
   mtx_lock(&ctx->object_mutex);
   hash_table_foreach (ctx->object_table, entry) {
      struct vkr_object *obj = entry->data;
      if (obj->type == VK_OBJECT_TYPE_DEVICE_MEMORY)
         vkr_device_memory_sync_shadow((struct vkr_device_memory *)obj, to_host);
   }
   mtx_unlock(&ctx->object_mutex);
#else
   (void)ctx;
   (void)to_host;
#endif
}

void
vkr_device_memory_sync_shadows_to_host(struct vkr_context *ctx)
{
   vkr_device_memory_sync_shadows(ctx, true);
}

void
vkr_device_memory_sync_shadows_from_host(struct vkr_context *ctx)
{
   vkr_device_memory_sync_shadows(ctx, false);
}
