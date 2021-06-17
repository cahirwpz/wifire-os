#ifndef _SYS_DEVICE_H_
#define _SYS_DEVICE_H_

#include <sys/queue.h>
#include <sys/malloc.h>
#include <sys/linker_set.h>
#include <sys/rman.h>

typedef struct devclass devclass_t;
typedef struct device device_t;
typedef struct driver driver_t;
typedef TAILQ_HEAD(, device) device_list_t;
typedef SLIST_HEAD(, resource) resource_list_t;

/* Driver that returns the highest value from its probe action
 * will be selected for attach action. */
typedef int (*d_probe_t)(device_t *dev);
/* Returns value from <errno.h> on failure, 0 on success. */
typedef int (*d_attach_t)(device_t *dev);
typedef int (*d_detach_t)(device_t *dev);

/* Update this section if you add any new driver interface */
typedef enum {
  DIF_BUS,
  DIF_PCI_BUS,
  DIF_EMMC,
  DIF_UART,
  DIF_USBHC,
  DIF_USB,
  DIF_COUNT /* this must be the last item */
} drv_if_t;

/* During kernel initialization the device tree is scanned multiple times.
 * Each scan we can detect new devices and attach drivers to existing or new
 * devices. Each driver is assigned a pass number. A driver may only probe and
 * attach to a device if driver's pass number is not greater than
 * `current_pass` counter.
 * Pass description:
 * - FIRST_PASS: devoted for drivers that require the most basic kernel APIs
 *   to be in working state (i.e. memory allocation, resource management,
 *   interrupt management). The main goal of this pass is to initialize enough
 *   drivers to clock subsystem (and thus scheduler & callouts) and console.
 * - SECOND_PASS: during this pass following kernel APIs are available:
 *   callouts, kernel threads, devfs.
 * If extra pass is needed, please add a coresponding description here and
 * explain what kernel APIs are required. */
typedef enum {
  FIRST_PASS,
  SECOND_PASS,
  PASS_COUNT /* this must be the last item */
} drv_pass_t;

struct driver {
  const char *desc;            /* short driver description */
  size_t size;                 /* device->state object size */
  drv_pass_t pass;             /* device tree pass number */
  d_probe_t probe;             /* probe for specific device(s) */
  d_attach_t attach;           /* attach device to system */
  d_detach_t detach;           /* detach device from system */
  void *interfaces[DIF_COUNT]; /* pointers to device methods structures */
};

typedef enum {
  DEV_BUS_NONE,
  DEV_BUS_PCI,
  DEV_BUS_ISA,
  DEV_BUS_EMMC,
  DEV_BUS_USB,
} device_bus_t;

struct device {
  /* Device hierarchy. */
  device_t *parent; /* parent node (bus?) or null (root or pseudo-dev) */
  TAILQ_ENTRY(device) link; /* node on list of siblings */
  device_list_t children;   /* head of children devices */

  /* Device information and state. */
  device_bus_t bus; /* (for children) type of bus we're attached to */
  driver_t *driver;
  devclass_t *devclass; /* (for buses) device class of children */
  int unit;
  void *instance;            /* used by bus driver to store data in children */
  void *state;               /* memory requested by driver for its state */
  resource_list_t resources; /* used by driver, assigned by parent bus */
};

/*! \brief Check whether a device is a bus or a regular device. */
static inline bool device_bus(device_t *dev) {
  return dev->devclass != NULL;
}

device_t *device_alloc(int unit);
device_t *device_add_child(device_t *parent, int unit);
void device_remove_child(device_t *parent, device_t *dev);
int device_probe(device_t *dev);
int device_attach(device_t *dev);
int device_detach(device_t *dev);

/*! \brief Add a resource entry to resource list. */
void device_add_resource(device_t *dev, res_type_t type, int rid,
                         rman_addr_t start, rman_addr_t end, size_t size,
                         rman_flags_t flags);

#define device_add_memory(dev, rid, start, size)                               \
  device_add_resource((dev), RT_MEMORY, (rid), (start), (start) + (size)-1,    \
                      (size), 0)

#define device_add_ioports(dev, rid, start, size)                              \
  device_add_resource((dev), RT_IOPORTS, (rid), (start), (start) + (size)-1,   \
                      (size), 0)

#define device_add_irq(dev, rid, irq)                                          \
  device_add_resource((dev), RT_IRQ, (rid), (irq), (irq), 1, RF_SHAREABLE)

/*! \brief Take a resource which is assigned to device by parent bus. */
resource_t *device_take_resource(device_t *dev, res_type_t type, int rid,
                                 rman_flags_t flags);

#define device_take_memory(dev, rid, flags)                                    \
  device_take_resource((dev), RT_MEMORY, (rid), (flags))

#define device_take_ioports(dev, rid, flags)                                   \
  device_take_resource((dev), RT_IOPORTS, (rid), (flags))

#define device_take_irq(dev, rid, flags)                                       \
  device_take_resource((dev), RT_IRQ, (rid), (flags))

/* A universal memory pool to be used by all drivers. */
KMALLOC_DECLARE(M_DEV);

/* Finds a device that implements a method for given interface */
/* As for now this actually returns a child of the bus, not the bus itself.
 * This is consistent with the current method semantics. Hopefully the
 * signatures will change in a future PR to be more suited for dispatching.
 * Currently the information about the caller is lost as the `dev` argument is
 * not guarenteed to be the caller, it's just a child of the bus. It just so
 * happens that the current scenarios in which we'll need the dispatching
 * don't require to know anything about the caller. Again, this will hopefully
 * change thanks to future extension of method semantics. */
device_t *device_method_provider(device_t *dev, drv_if_t iface,
                                 ptrdiff_t method_offset);

#endif /* !_SYS_DEVICE_H_ */
