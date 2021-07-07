/*
 * Standard VGA driver
 */

#include <dev/pci.h>
#include <sys/libkern.h>
#include <sys/errno.h>
#include <sys/fb.h>
#include <sys/devfs.h>
#include <sys/devclass.h>
#include <sys/vnode.h>
#include <stdatomic.h>

typedef struct fb_color fb_color_t;
typedef struct fb_palette fb_palette_t;
typedef struct fb_info fb_info_t;

#define FB_SIZE(fbi) ((fbi)->width * (fbi)->height * ((fbi)->bpp / 8))

#define VGA_PALETTE_SIZE 256

typedef struct stdvga_state {
  resource_t *mem;
  resource_t *io;

  atomic_int usecnt;
  fb_info_t fb_info;
} stdvga_state_t;

/* Detailed information about VGA registers is available at
   http://www.osdever.net/FreeVGA/vga/vga.htm */

#define VGA_PALETTE_ADDR 0x3C8
#define VGA_PALETTE_DATA 0x3C9
#define VGA_AR_ADDR 0x3C0
#define VGA_AR_PAS 0x20 /* Palette Address Source bit */

/* Bochs VBE. Simplifies VGA graphics mode configuration a great deal. Some
   documentation is available at http://wiki.osdev.org/Bochs_VBE_Extensions */
#define VBE_DISPI_INDEX_XRES 0x01
#define VBE_DISPI_INDEX_YRES 0x02
#define VBE_DISPI_INDEX_BPP 0x03
#define VBE_DISPI_INDEX_ENABLE 0x04
#define VBE_DISPI_ENABLED 0x01 /* VBE Enabled bit */

/* Offsets for accessing ioports via PCI BAR1 (MMIO) */
#define VGA_MMIO_OFFSET (0x400 - 0x3c0)
#define VBE_MMIO_OFFSET 0x500

/* The general overview of the QEMU std vga device is available at
   https://github.com/qemu/qemu/blob/master/docs/specs/standard-vga.txt */
#define QEMU_STDVGA_VENDOR_ID 0x1234
#define QEMU_STDVGA_DEVICE_ID 0x1111

static void stdvga_io_write(stdvga_state_t *vga, uint16_t reg, uint8_t value) {
  bus_write_1(vga->io, reg + VGA_MMIO_OFFSET, value);
}

static uint8_t __unused stdvga_io_read(stdvga_state_t *vga, uint16_t reg) {
  return bus_read_1(vga->io, reg + VGA_MMIO_OFFSET);
}

static void stdvga_vbe_write(stdvga_state_t *vga, uint16_t reg,
                             uint16_t value) {
  /* <<1 shift enables access to 16-bit registers. */
  bus_write_2(vga->io, (reg << 1) + VBE_MMIO_OFFSET, value);
}

static uint16_t stdvga_vbe_read(stdvga_state_t *vga, uint16_t reg) {
  return bus_read_2(vga->io, (reg << 1) + VBE_MMIO_OFFSET);
}

static uint16_t stdvga_vbe_set(stdvga_state_t *vga, uint16_t reg,
                               uint16_t mask) {
  uint16_t oldval = stdvga_vbe_read(vga, reg);
  stdvga_vbe_write(vga, reg, oldval | mask);
  return oldval;
}

static void stdvga_set_color(stdvga_state_t *vga, uint8_t index, uint8_t r,
                             uint8_t g, uint8_t b) {
  stdvga_io_write(vga, VGA_PALETTE_ADDR, index);
  stdvga_io_write(vga, VGA_PALETTE_DATA, r >> 2);
  stdvga_io_write(vga, VGA_PALETTE_DATA, g >> 2);
  stdvga_io_write(vga, VGA_PALETTE_DATA, b >> 2);
}

static int stdvga_set_palette(stdvga_state_t *vga, fb_palette_t *pal) {
  if (pal->len == 0 || pal->len > VGA_PALETTE_SIZE)
    return EINVAL;

  fb_color_t *colors = kmalloc(M_DEV, pal->len * sizeof(fb_color_t), 0);
  if (!colors)
    return ENOMEM;

  int error;
  if ((error = copyin(pal->colors, colors, pal->len * sizeof(fb_color_t))))
    goto fail;

  for (uint32_t i = 0; i < pal->len; i++)
    stdvga_set_color(vga, i, colors[i].r, colors[i].g, colors[i].b);

fail:
  kfree(M_DEV, colors);
  return error;
}

static int stdvga_set_fbinfo(stdvga_state_t *vga, fb_info_t *fb_info) {
  /* Impose some reasonable resolution limit. */
  if (fb_info->width > 640 || fb_info->height > 480)
    return EINVAL;

  if (fb_info->bpp != 8 && fb_info->bpp != 16 && fb_info->bpp != 24)
    return EINVAL;

  memcpy(&vga->fb_info, fb_info, sizeof(fb_info_t));

  /* Apply resolution & bits per pixel. */
  stdvga_vbe_write(vga, VBE_DISPI_INDEX_XRES, vga->fb_info.width);
  stdvga_vbe_write(vga, VBE_DISPI_INDEX_YRES, vga->fb_info.height);
  stdvga_vbe_write(vga, VBE_DISPI_INDEX_BPP, vga->fb_info.bpp);
  return 0;
}

static int stdvga_open(devnode_t *dev, file_t *fp, int oflags) {
  stdvga_state_t *vga = dev->data;
  int error;

  /* Disallow opening the file more than once. */
  int expected = 0;
  if (!atomic_compare_exchange_strong(&vga->usecnt, &expected, 1))
    return EBUSY;

  if ((error = stdvga_set_fbinfo(vga, &vga->fb_info))) {
    atomic_store(&vga->usecnt, 0);
    return error;
  }

  return 0;
}

static int stdvga_close(devnode_t *dev, file_t *fp) {
  stdvga_state_t *vga = dev->data;
  atomic_store(&vga->usecnt, 0);
  return 0;
}

static int stdvga_write(devnode_t *dev, uio_t *uio) {
  stdvga_state_t *vga = dev->data;
  size_t size = FB_SIZE(&vga->fb_info);

  return uiomove_frombuf((void *)vga->mem->r_bus_handle, size, uio);
}

static int stdvga_ioctl(devnode_t *dev, u_long cmd, void *data, int fflags) {
  stdvga_state_t *vga = dev->data;

  if (cmd == FBIOCGET_FBINFO) {
    memcpy(data, &vga->fb_info, sizeof(fb_info_t));
    return 0;
  }
  if (cmd == FBIOCSET_FBINFO)
    return stdvga_set_fbinfo(vga, data);
  if (cmd == FBIOCSET_PALETTE)
    return stdvga_set_palette(vga, data);
  return EINVAL;
}

static devops_t stdvga_devops = {
  .d_type = DT_SEEKABLE,
  .d_open = stdvga_open,
  .d_close = stdvga_close,
  .d_write = stdvga_write,
  .d_ioctl = stdvga_ioctl,
};

static int stdvga_probe(device_t *dev) {
  pci_device_t *pcid = pci_device_of(dev);

  if (!pci_device_match(pcid, QEMU_STDVGA_VENDOR_ID, QEMU_STDVGA_DEVICE_ID))
    return 0;

  if (!(pcid->bar[0].flags & RF_PREFETCHABLE))
    return 0;

  return 1;
}

static int stdvga_attach(device_t *dev) {
  stdvga_state_t *vga = dev->state;

  vga->mem = device_take_memory(dev, 0, RF_ACTIVE | RF_PREFETCHABLE);
  vga->io = device_take_memory(dev, 2, RF_ACTIVE);

  assert(vga->mem != NULL);
  assert(vga->io != NULL);

  vga->usecnt = 0;

  /* Enable palette access */
  stdvga_io_write(vga, VGA_AR_ADDR, VGA_AR_PAS);

  /* Configure initial videomode. */
  vga->fb_info = (fb_info_t){.width = 320, .height = 200, .bpp = 8};

  /* Enable VBE. */
  stdvga_vbe_set(vga, VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED);

  /* Install /dev/vga device file. */
  devfs_makedev_new(NULL, "vga", &stdvga_devops, vga, NULL);

  return 0;
}

static driver_t stdvga = {
  .desc = "Bochs VGA driver",
  .size = sizeof(stdvga_state_t),
  .pass = SECOND_PASS,
  .probe = stdvga_probe,
  .attach = stdvga_attach,
};

DEVCLASS_ENTRY(pci, stdvga);
