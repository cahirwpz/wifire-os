#ifndef _SYS_MEMDEV_H_
#define _SYS_MEMDEV_H_

#include <sys/types.h>
#include <sys/device.h>

typedef int (*readblock_t)(device_t *, uint32_t, unsigned char *, uint32_t);
typedef int (*writeblock_t)(device_t *, uint32_t, const uint8_t *, uint32_t);

typedef struct blockdev_methods {
  readblock_t read;
  writeblock_t write;
} blockdev_methods_t;

#define BLOCK_MEM_METHODS(dev)                                                 \
  (*(bus_methods_t *)(dev)->driver->interfaces[DIF_BUS])

/* No dispatching so far, until calling semantics change */

#endif