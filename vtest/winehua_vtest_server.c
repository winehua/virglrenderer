#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vtest_server.h"
#include "vtest.h"

#ifdef ENABLE_VENUS
#include "venus/vkr_renderer.h"
extern void virgl_renderer_set_winehua_vk_present_callback(
   vkr_renderer_winehua_present_callback_type callback,
   void *user_data);
#endif

__attribute__((visibility("default"))) int winehua_vtest_main(int argc, char **argv);
__attribute__((visibility("default"))) void winehua_vtest_set_present_callback(
   vtest_winehua_present_callback callback, void *user_data);
#ifdef ENABLE_VENUS
__attribute__((visibility("default"))) void winehua_vtest_set_vulkan_present_callback(
   vkr_renderer_winehua_present_callback_type callback, void *user_data);
#endif

int winehua_vtest_main(int argc, char **argv)
{
   return vtest_main(argc, argv);
}

void winehua_vtest_set_present_callback(
   vtest_winehua_present_callback callback, void *user_data)
{
   vtest_set_winehua_present_callback(callback, user_data);
}

#ifdef ENABLE_VENUS
void winehua_vtest_set_vulkan_present_callback(
   vkr_renderer_winehua_present_callback_type callback, void *user_data)
{
   virgl_renderer_set_winehua_vk_present_callback(callback, user_data);
}
#endif
