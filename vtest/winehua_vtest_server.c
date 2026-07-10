#include "vtest_server.h"

__attribute__((visibility("default"))) int winehua_vtest_main(int argc, char **argv);

int winehua_vtest_main(int argc, char **argv)
{
   return vtest_main(argc, argv);
}
