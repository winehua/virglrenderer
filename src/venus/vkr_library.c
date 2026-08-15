/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_common.h"

#include <dlfcn.h>

#include "vkr_library.h"

void
vkr_library_preload_icd(void)
{
#ifdef ENABLE_VULKAN_PRELOAD
   struct vulkan_library lib = { 0 };

   if (!vkr_library_load(&lib))
      return;

   /* Get vkGetInstanceProcAddr from libvulkan */
   PFN_vkGetInstanceProcAddr get_proc_addr = lib.GetInstanceProcAddr;

   PFN_vkEnumerateInstanceExtensionProperties enumerate_inst_ext_props =
      (PFN_vkEnumerateInstanceExtensionProperties)get_proc_addr(
         VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
   if (enumerate_inst_ext_props) {
      /* this makes the Vulkan loader loads ICDs */
      uint32_t unused_count;
      enumerate_inst_ext_props(NULL, &unused_count, NULL);
   }

   vkr_library_unload(&lib);
#endif
}

#if defined(ENABLE_VULKAN_DLOAD)

bool
vkr_library_load(struct vulkan_library *lib)
{
   if (lib->handle)
      return true;

#ifdef __APPLE__
   lib->handle = dlopen("libvulkan.dylib", RTLD_NOW | RTLD_LOCAL);
   if (lib->handle == NULL)
      lib->handle = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
   if (lib->handle == NULL)
      lib->handle = dlopen("libMoltenVK.dylib", RTLD_NOW | RTLD_LOCAL);
#else
   /* WineHua arm64/OHOS: app bundle el1 顶层放有 guest Vulkan Loader
    * (libvulkan.so.1), 名字搜索会先命中它而不是宿主驱动 → host vkCreateInstance
    * 返回 INCOMPATIBLE_DRIVER. 宿主系统 Vulkan 是 /system/lib64/libvulkan.so
    * (无版本号, 海思 Maleoon), 用绝对路径直连绕过名字搜索/遮蔽.
    * 非 OHOS 平台该路径不存在 → dlopen 返回 NULL, 自然 fallback 到名字搜索. */
   lib->handle = dlopen("/system/lib64/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
   if (lib->handle == NULL)
      lib->handle = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   if (lib->handle == NULL)
      lib->handle = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
#endif
   if (lib->handle == NULL) {
      vkr_log("failed to open libvulkan: %s", dlerror());
      return false;
   }

   /* Clear any existing error */
   dlerror();

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
   /* ISO C forbids conversion of object pointer to function pointer type */
   lib->GetInstanceProcAddr =
      (PFN_vkGetInstanceProcAddr)dlsym(lib->handle, "vkGetInstanceProcAddr");
#pragma GCC diagnostic pop

   char *error = dlerror();
   if (error != NULL) {
      vkr_log("dlerror: %s", error);
      goto fail;
   }

   if (lib->GetInstanceProcAddr == NULL) {
      vkr_log("failed to load vkGetInstanceProcAddr: %s", dlerror());
      goto fail;
   }

   return true;

fail:
   dlclose(lib->handle);
   lib->handle = NULL;
   return false;
}

void
vkr_library_unload(struct vulkan_library *lib)
{
   if (lib->handle) {
      dlclose(lib->handle);
      lib->GetInstanceProcAddr = NULL;
      lib->handle = NULL;
   }
}

#endif /* ENABLE_VULKAN_DLOAD */

bool
vkr_library_has_portability_enumeration(
   PFN_vkEnumerateInstanceExtensionProperties enum_inst_ext_props)
{
   uint32_t property_count = 0;
   VkExtensionProperties *properties;
   bool has_portability_enumeration = false;

   VkResult ret = enum_inst_ext_props(NULL, &property_count, NULL);
   if (ret != VK_SUCCESS)
      return false;

   properties = calloc(property_count, sizeof(*properties));
   if (!properties)
      return false;

   ret = enum_inst_ext_props(NULL, &property_count, properties);
   if (ret != VK_SUCCESS) {
      free(properties);
      return false;
   }

   for (uint32_t i = 0; i < property_count; i++) {
      if (!strcmp(properties[i].extensionName,
                  VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
         has_portability_enumeration = true;
         break;
      }
   }
   free(properties);
   return has_portability_enumeration;
}
