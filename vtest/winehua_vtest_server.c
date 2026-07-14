#include "vtest_server.h"
#include "vtest.h"

__attribute__((visibility("default"))) int winehua_vtest_main(int argc, char **argv);
__attribute__((visibility("default"))) void winehua_vtest_set_present_callback(
   vtest_winehua_present_callback callback, void *user_data);

int winehua_vtest_main(int argc, char **argv)
{
   return vtest_main(argc, argv);
}

void winehua_vtest_set_present_callback(
   vtest_winehua_present_callback callback, void *user_data)
{
   vtest_set_winehua_present_callback(callback, user_data);
}
