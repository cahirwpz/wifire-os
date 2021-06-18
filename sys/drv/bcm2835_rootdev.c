#define KL_LOG KL_DEV
#include <sys/klog.h>
#include <sys/bus.h>
#include <sys/devclass.h>
#include <aarch64/armreg.h>
#include <dev/bcm2835reg.h>
#include <sys/kmem.h>
#include <sys/pmap.h>

/*
 * located at BCM2836_ARM_LOCAL_BASE
 * 32 local interrupts -- one per CPU but now we only support 1 CPU
 *
 * located at BCM2835_ARMICU_BASE
 * accessed by BCM2835_PERIPHERALS_BASE NOT by BCM2835_PERIPHERALS_BASE_BUS
 * 32 GPU0 interrupts -- 0x204 offset from address; 32 in table
 * 32 GPU1 interrupts -- 0x208 offset from address; 64 in table
 * 32 base interrupts -- 0x200 offset from address; 96 in table
 */

#define NIRQ (BCM2835_NIRQ + BCM2836_NIRQ)

typedef struct rootdev {
  rman_t rm;
  rman_t irq_rm;
  intr_event_t *intr_event[NIRQ];
} rootdev_t;

/* BCM2836_ARM_LOCAL_BASE mapped into VA (4KiB). */
static vaddr_t rootdev_local_handle;
/* BCM2835_PERIPHERALS_BUS_TO_PHYS(BCM2835_ARM_BASE) mapped into VA (4KiB). */
static vaddr_t rootdev_arm_base;

/* Use pre mapped memory. */
static int rootdev_bs_map(bus_addr_t addr, bus_size_t size,
                          bus_space_handle_t *handle_p) {
  if (addr >= BCM2836_ARM_LOCAL_BASE &&
      addr < BCM2836_ARM_LOCAL_BASE + BCM2836_ARM_LOCAL_SIZE) {
    *handle_p = rootdev_local_handle;
    return 0;
  }
  if (addr >= BCM2835_PERIPHERALS_BUS_TO_PHYS(BCM2835_ARM_BASE) &&
      addr <
        BCM2835_PERIPHERALS_BUS_TO_PHYS(BCM2835_ARM_BASE) + BCM2835_ARM_SIZE) {
    *handle_p = rootdev_arm_base;
    return 0;
  }
  return generic_bs_map(addr, size, handle_p);
}

/* clang-format off */
static bus_space_t *rootdev_bus_space = &(bus_space_t){
  .bs_map = rootdev_bs_map,
  .bs_read_1 = generic_bs_read_1,
  .bs_read_2 = generic_bs_read_2,
  .bs_read_4 = generic_bs_read_4,
  .bs_write_1 = generic_bs_write_1,
  .bs_write_2 = generic_bs_write_2,
  .bs_write_4 = generic_bs_write_4,
  .bs_read_region_1 = generic_bs_read_region_1,
  .bs_read_region_2 = generic_bs_read_region_2,
  .bs_read_region_4 = generic_bs_read_region_4,
  .bs_write_region_1 = generic_bs_write_region_1,
  .bs_write_region_2 = generic_bs_write_region_2,
  .bs_write_region_4 = generic_bs_write_region_4,
};
/* clang-format on */

static void enable_local_irq(int irq) {
  assert(irq < BCM2836_INT_NLOCAL);
  uint32_t reg = bus_space_read_4(rootdev_bus_space, rootdev_local_handle,
                                  BCM2836_LOCAL_TIMER_IRQ_CONTROLN(0));
  bus_space_write_4(rootdev_bus_space, rootdev_local_handle,
                    BCM2836_LOCAL_TIMER_IRQ_CONTROLN(0), reg | (1 << irq));
}

static void disable_local_irq(int irq) {
  assert(irq < BCM2836_INT_NLOCAL);
  uint32_t reg = bus_space_read_4(rootdev_bus_space, rootdev_local_handle,
                                  BCM2836_LOCAL_TIMER_IRQ_CONTROLN(0));
  bus_space_write_4(rootdev_bus_space, rootdev_local_handle,
                    BCM2836_LOCAL_TIMER_IRQ_CONTROLN(0), reg & (~(1 << irq)));
}

static void enable_gpu_irq(int irq, bus_size_t offset) {
  uint32_t reg = bus_space_read_4(rootdev_bus_space, rootdev_arm_base,
                                  BCM2835_ARMICU_OFFSET + offset);
  bus_space_write_4(rootdev_bus_space, rootdev_arm_base,
                    BCM2835_ARMICU_OFFSET + offset, reg | (1 << irq));
}

static void disable_gpu_irq(int irq, bus_size_t offset) {
  uint32_t reg = bus_space_read_4(rootdev_bus_space, rootdev_arm_base,
                                  BCM2835_ARMICU_OFFSET + offset);
  bus_space_write_4(rootdev_bus_space, rootdev_arm_base,
                    BCM2835_ARMICU_OFFSET + offset, reg & (~(1 << irq)));
}

static void rootdev_enable_irq(intr_event_t *ie) {
  int irq = ie->ie_irq;
  assert(irq < NIRQ);

  if (irq < BCM2836_NIRQ) {
    /* Enable local IRQ. */
    enable_local_irq(irq);
  } else if (irq < BCM2835_INT_GPU1BASE) {
    /* Enable GPU0 IRQ. */
    enable_gpu_irq(irq - BCM2835_INT_GPU0BASE, BCM2835_INTC_IRQ1ENABLE);
  } else if (irq < BCM2835_INT_BASICBASE) {
    /* Enable GPU1 IRQ. */
    enable_gpu_irq(irq - BCM2835_INT_GPU1BASE, BCM2835_INTC_IRQ2ENABLE);
  } else {
    /* Enable base IRQ. */
    enable_gpu_irq(irq - BCM2835_INT_BASICBASE, BCM2835_INTC_IRQBENABLE);
  }
}

static void rootdev_disable_irq(intr_event_t *ie) {
  int irq = ie->ie_irq;
  assert(irq < NIRQ);

  if (irq < BCM2836_NIRQ) {
    /* disable local IRQ. */
    disable_local_irq(irq);
  } else if (irq < BCM2835_INT_GPU1BASE) {
    /* disable GPU0 IRQ. */
    disable_gpu_irq(irq - BCM2835_INT_GPU0BASE, BCM2835_INTC_IRQ1DISABLE);
  } else if (irq < BCM2835_INT_BASICBASE) {
    /* disable GPU1 IRQ. */
    disable_gpu_irq(irq - BCM2835_INT_GPU1BASE, BCM2835_INTC_IRQ2DISABLE);
  } else {
    /* disable base IRQ. */
    disable_gpu_irq(irq - BCM2835_INT_BASICBASE, BCM2835_INTC_IRQBDISABLE);
  }
}

static void rootdev_intr_setup(device_t *dev, resource_t *r,
                               ih_filter_t *filter, ih_service_t *service,
                               void *arg, const char *name) {
  rootdev_t *rd = dev->parent->state;
  int irq = resource_start(r);
  assert(irq < NIRQ);

  if (rd->intr_event[irq] == NULL)
    rd->intr_event[irq] = intr_event_create(dev, irq, rootdev_disable_irq,
                                            rootdev_enable_irq, "???");

  r->r_handler =
    intr_event_add_handler(rd->intr_event[irq], filter, service, arg, name);
}

static void rootdev_intr_teardown(device_t *dev, resource_t *irq) {
  intr_event_remove_handler(irq->r_handler);
}

/* Read 32 bit pending register located at irq_base + offset and run
 * corresponding handlers. */
static void bcm2835_intr_handle(bus_space_handle_t irq_base, bus_size_t offset,
                                intr_event_t **events) {
  uint32_t pending = bus_space_read_4(rootdev_bus_space, irq_base, offset);

  while (pending) {
    int irq = ffs(pending) - 1;
    /* XXX: some pending bits are shared between BASIC and GPU0/1. */
    if (events[irq])
      intr_event_run_handlers(events[irq]);
    pending &= ~(1 << irq);
  }
}

static void rootdev_intr_handler(ctx_t *ctx, device_t *dev, void *arg) {
  assert(dev != NULL);
  rootdev_t *rd = dev->state;

  /* Handle local interrupts. */
  bcm2835_intr_handle(rootdev_local_handle, BCM2836_LOCAL_INTC_IRQPENDINGN(0),
                      &rd->intr_event[BCM2836_INT_BASECPUN(0)]);

  /* Handle GPU0 interrupts. */
  bcm2835_intr_handle(rootdev_arm_base,
                      (BCM2835_ARMICU_OFFSET + BCM2835_INTC_IRQ1PENDING),
                      &rd->intr_event[BCM2835_INT_GPU0BASE]);

  /* Handle GPU1 interrupts. */
  bcm2835_intr_handle(rootdev_arm_base,
                      (BCM2835_ARMICU_OFFSET + BCM2835_INTC_IRQ2PENDING),
                      &rd->intr_event[BCM2835_INT_GPU1BASE]);

  /* Handle base interrupts. */
  bcm2835_intr_handle(rootdev_arm_base,
                      (BCM2835_ARMICU_OFFSET + BCM2835_INTC_IRQBPENDING),
                      &rd->intr_event[BCM2835_INT_BASICBASE]);
}

static int rootdev_probe(device_t *bus) {
  return 1;
}

static int rootdev_attach(device_t *bus) {
  rootdev_t *rd = bus->state;

  rman_init(&rd->rm, "ARM and BCM2835 space");
  rman_manage_region(&rd->rm, BCM2835_PERIPHERALS_BASE,
                     BCM2835_PERIPHERALS_SIZE);
  rman_manage_region(&rd->rm, BCM2836_ARM_LOCAL_BASE, BCM2836_ARM_LOCAL_SIZE);

  rman_init(&rd->irq_rm, "BCM2835 interrupts");
  rman_manage_region(&rd->irq_rm, 0, NIRQ);

  /* Map BCM2836 shared processor only once. */
  rootdev_local_handle = kmem_map_contig(BCM2836_ARM_LOCAL_BASE,
                                         BCM2836_ARM_LOCAL_SIZE, PMAP_NOCACHE);

  rootdev_arm_base =
    kmem_map_contig(BCM2835_PERIPHERALS_BUS_TO_PHYS(BCM2835_ARM_BASE),
                    BCM2835_ARM_SIZE, PMAP_NOCACHE);

  intr_root_claim(rootdev_intr_handler, bus, NULL);

  device_t *dev;

  /* Create ARM timer device and assign resources to it. */
  dev = device_add_child(bus, 0);
  device_add_irq(dev, 0, BCM2836_INT_CNTPNSIRQ_CPUN(0));

  /* Create PL011 UART device and assign resources to it. */
  dev = device_add_child(bus, 1);
  device_add_memory(dev, 0, BCM2835_PERIPHERALS_BUS_TO_PHYS(BCM2835_UART0_BASE),
                    BCM2835_UART0_SIZE);
  device_add_irq(dev, 0, BCM2835_INT_UART0);

  /* Create DWC OTG device and assign resources to it. */
  dev = device_add_child(bus, 2);
  device_add_memory(dev, 0, BCM2835_PERIPHERALS_BUS_TO_PHYS(BCM2835_USB_BASE),
                    BCM2835_USB_SIZE);
  device_add_irq(dev, 0, BCM2835_INT_USB);

  /* TODO: replace raw resource assignments by parsing FDT file. */

  return bus_generic_probe(bus);
}

static resource_t *rootdev_alloc_resource(device_t *dev, res_type_t type,
                                          int rid, rman_addr_t start,
                                          rman_addr_t end, size_t size,
                                          rman_flags_t flags) {
  rootdev_t *rd = dev->parent->state;
  size_t alignment = 0;
  rman_t *rman = NULL;

  if (type == RT_MEMORY) {
    alignment = PAGESIZE;
    rman = &rd->rm;
  } else if (type == RT_IRQ) {
    rman = &rd->irq_rm;
  } else {
    panic("Resource type not handled!");
  }

  resource_t *r =
    rman_reserve_resource(rman, type, rid, start, end, size, alignment, flags);
  if (!r)
    return NULL;

  if (type == RT_MEMORY) {
    r->r_bus_tag = rootdev_bus_space;
    r->r_bus_handle = resource_start(r);
  }

  if (flags & RF_ACTIVE) {
    if (bus_activate_resource(dev, r)) {
      resource_release(r);
      return NULL;
    }
  }

  return r;
}

static void rootdev_release_resource(device_t *dev, resource_t *r) {
  bus_deactivate_resource(dev, r);
  resource_release(r);
}

static int rootdev_activate_resource(device_t *dev, resource_t *r) {
  if (r->r_type == RT_MEMORY)
    return bus_space_map(r->r_bus_tag, resource_start(r), resource_size(r),
                         &r->r_bus_handle);
  return 0;
}

static void rootdev_deactivate_resource(device_t *dev, resource_t *r) {
  /* TODO: unmap mapped resources. */
}

static bus_methods_t rootdev_bus_if = {
  .intr_setup = rootdev_intr_setup,
  .intr_teardown = rootdev_intr_teardown,
  .alloc_resource = rootdev_alloc_resource,
  .release_resource = rootdev_release_resource,
  .activate_resource = rootdev_activate_resource,
  .deactivate_resource = rootdev_deactivate_resource,
};

driver_t rootdev_driver = {
  .desc = "RPI3 platform root bus driver",
  .size = sizeof(rootdev_t),
  .pass = FIRST_PASS,
  .probe = rootdev_probe,
  .attach = rootdev_attach,
  .interfaces =
    {
      [DIF_BUS] = &rootdev_bus_if,
    },
};

DEVCLASS_CREATE(root);
