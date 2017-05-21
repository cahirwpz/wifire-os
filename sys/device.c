#define KL_LOG KL_DEV
#include <stdc.h>
#include <klog.h>
#include <device.h>
#include <pci.h>
#include <sysinit.h>

MALLOC_DEFINE(M_DEV, "devices & drivers", 128, 1024);

static device_t *device_alloc() {
  device_t *dev = kmalloc(M_DEV, sizeof(device_t), M_ZERO);
  TAILQ_INIT(&dev->children);
  return dev;
}

device_t *device_add_child(device_t *dev) {
  device_t *child = device_alloc();
  child->parent = dev;
  TAILQ_INSERT_TAIL(&dev->children, child, link);
  return child;
}

/* TODO: this routine should go over all drivers within a suitable class and
 * choose the best driver. For now the user is responsible for setting the
 * driver before calling `device_probe`. */
int device_probe(device_t *dev) {
  assert(dev->driver != NULL);
  d_probe_t probe = dev->driver->probe;
  int found = probe ? probe(dev) : 1;
  if (found)
    dev->state = kmalloc(M_DEV, dev->driver->size, M_ZERO);
  return found;
}

int device_attach(device_t *dev) {
  assert(dev->driver != NULL);
  d_attach_t attach = dev->driver->attach;
  return attach ? attach(dev) : 0;
}

int device_detach(device_t *dev) {
  assert(dev->driver != NULL);
  d_detach_t detach = dev->driver->detach;
  int res = detach ? detach(dev) : 0;
  if (res == 0)
    kfree(M_DEV, dev->state);
  return res;
}

int bus_generic_probe(device_t *bus) {
  device_t *dev;
  SET_DECLARE(driver_table, driver_t);
  klog("Scanning %s for known devices.", bus->driver->desc);
  TAILQ_FOREACH (dev, &bus->children, link) {
    driver_t **drv_p;
    SET_FOREACH(drv_p, driver_table) {
      driver_t *drv = *drv_p;
      dev->driver = drv;
      if (device_probe(dev)) {
        klog("%s detected!", drv->desc);
        device_attach(dev);
        break;
      }
      dev->driver = NULL;
    }
  }
  return 0;
}

extern device_t *rootdev;

void driver_init() {
  device_attach(rootdev);
}

SYSINIT_ADD(driver, driver_init, NODEPS);
