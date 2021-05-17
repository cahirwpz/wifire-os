#include <sys/bus.h>
#include <sys/devclass.h>

driver_t rootdev_driver;

DEVCLASS_CREATE(root);

static driver_t stub;
DEVCLASS_ENTRY(root, stub);
