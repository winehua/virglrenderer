#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vtest_server.h"
#include "vtest.h"

extern void virgl_renderer_winehua_set_color_remap(uint32_t src_tex, uint32_t dst_tex);
extern int virgl_renderer_winehua_set_scanout_backing(uint32_t res_handle, uint32_t gl_id,
                                                      void *egl_image);
extern int virgl_renderer_winehua_clear_scanout_backing(uint32_t res_handle);
extern int virgl_renderer_winehua_scanout_last_write(uint32_t res_handle, uint32_t *dst_gl,
                                                     uint32_t *full_cover, const char **op);
extern int virgl_renderer_winehua_scanout_generation(uint32_t res_handle,
                                                     uint64_t *requested,
                                                     uint64_t *applied,
                                                     uint32_t *draw_gl);

#ifdef ENABLE_VENUS
#include "venus/vkr_renderer.h"
extern void virgl_renderer_set_winehua_vk_present_callback(
   vkr_renderer_winehua_present_callback_type callback,
   void *user_data);
extern void virgl_renderer_set_winehua_vk_device_release_callback(
   vkr_renderer_winehua_device_release_callback_type callback,
   void *user_data);
extern int virgl_renderer_winehua_vk_set_scanout_backing(uint32_t ctx_id,
                                                         uint64_t scanout_image);
extern int virgl_renderer_winehua_vk_clear_scanout_backing(uint32_t ctx_id);
#endif

__attribute__((visibility("default"))) int winehua_vtest_main(int argc, char **argv);
__attribute__((visibility("default"))) void winehua_vtest_reset_stop_request(void);
__attribute__((visibility("default"))) int winehua_vtest_request_stop(void);
__attribute__((visibility("default"))) void winehua_vtest_set_present_callback(
   vtest_winehua_present_callback callback, void *user_data);
__attribute__((visibility("default"))) void winehua_vtest_set_color_remap(
   uint32_t src_tex, uint32_t dst_tex);
__attribute__((visibility("default"))) int winehua_vtest_set_scanout_backing(
   uint32_t res_handle, uint32_t gl_id, void *egl_image);
__attribute__((visibility("default"))) int winehua_vtest_clear_scanout_backing(
   uint32_t res_handle);
__attribute__((visibility("default"))) int winehua_vtest_scanout_last_write(
   uint32_t res_handle, uint32_t *dst_gl, uint32_t *full_cover, const char **op);
__attribute__((visibility("default"))) int winehua_vtest_scanout_generation(
   uint32_t res_handle, uint64_t *requested, uint64_t *applied, uint32_t *draw_gl);
#ifdef ENABLE_VENUS
__attribute__((visibility("default"))) void winehua_vtest_set_vulkan_present_callback(
   vkr_renderer_winehua_present_callback_type callback, void *user_data);
__attribute__((visibility("default"))) void winehua_vtest_set_vulkan_device_release_callback(
   vkr_renderer_winehua_device_release_callback_type callback, void *user_data);
__attribute__((visibility("default"))) int winehua_vtest_set_vk_scanout_backing(
   uint32_t ctx_id, uint64_t scanout_image);
__attribute__((visibility("default"))) int winehua_vtest_clear_vk_scanout_backing(
   uint32_t ctx_id);
#endif

int winehua_vtest_main(int argc, char **argv)
{
   return vtest_main(argc, argv);
}

void winehua_vtest_reset_stop_request(void)
{
   vtest_server_reset_stop_request();
}

int winehua_vtest_request_stop(void)
{
   return vtest_server_request_stop();
}

void winehua_vtest_set_present_callback(
   vtest_winehua_present_callback callback, void *user_data)
{
   vtest_set_winehua_present_callback(callback, user_data);
}

void winehua_vtest_set_color_remap(uint32_t src_tex, uint32_t dst_tex)
{
   virgl_renderer_winehua_set_color_remap(src_tex, dst_tex);
}

int winehua_vtest_set_scanout_backing(uint32_t res_handle, uint32_t gl_id, void *egl_image)
{
   return virgl_renderer_winehua_set_scanout_backing(res_handle, gl_id, egl_image);
}

int winehua_vtest_clear_scanout_backing(uint32_t res_handle)
{
   return virgl_renderer_winehua_clear_scanout_backing(res_handle);
}

int winehua_vtest_scanout_last_write(uint32_t res_handle, uint32_t *dst_gl,
                                    uint32_t *full_cover, const char **op)
{
   return virgl_renderer_winehua_scanout_last_write(res_handle, dst_gl, full_cover, op);
}

int winehua_vtest_scanout_generation(uint32_t res_handle, uint64_t *requested,
                                     uint64_t *applied, uint32_t *draw_gl)
{
   return virgl_renderer_winehua_scanout_generation(res_handle, requested,
                                                    applied, draw_gl);
}

#ifdef ENABLE_VENUS
void winehua_vtest_set_vulkan_present_callback(
   vkr_renderer_winehua_present_callback_type callback, void *user_data)
{
   virgl_renderer_set_winehua_vk_present_callback(callback, user_data);
}

void winehua_vtest_set_vulkan_device_release_callback(
   vkr_renderer_winehua_device_release_callback_type callback, void *user_data)
{
   virgl_renderer_set_winehua_vk_device_release_callback(callback, user_data);
}

int winehua_vtest_set_vk_scanout_backing(uint32_t ctx_id, uint64_t scanout_image)
{
   return virgl_renderer_winehua_vk_set_scanout_backing(ctx_id, scanout_image);
}

int winehua_vtest_clear_vk_scanout_backing(uint32_t ctx_id)
{
   return virgl_renderer_winehua_vk_clear_scanout_backing(ctx_id);
}
#endif
