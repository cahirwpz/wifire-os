#include <vnode.h>
#include <mount.h>
#include <devfs.h>
#include <errno.h>
#include <uio.h>
#include <stdc.h>
#include <vga.h>

static int framebuffer_write(vnode_t *v, uio_t *uio) {
  return vga_fb_write((vga_device_t *)v->v_data, uio);
}

static vnodeops_t framebuffer_vnodeops = {.v_open = vnode_open_generic,
                                      .v_write = framebuffer_write};

static int palette_write(vnode_t *v, uio_t *uio) {
  return vga_palette_write((vga_device_t *)v->v_data, uio);
}

static vnodeops_t palette_vnodeops = {.v_open = vnode_open_generic,
                                  .v_write = palette_write};

#define RES_CTRL_BUFFER_SIZE 16

static int videomode_write(vnode_t *v, uio_t *uio) {
  vga_device_t *vga = (vga_device_t *)v->v_data;
  uio->uio_offset = 0; /* This file does not support offsets. */
  unsigned xres, yres, bpp;
  int error = vga_get_videomode(vga, &xres, &yres, &bpp);
  if (error)
    return error;
  char buffer[RES_CTRL_BUFFER_SIZE];
  error = uiomove_frombuf(buffer, RES_CTRL_BUFFER_SIZE, uio);
  if (error)
    return error;
  /* Not specifying BPP leaves it at current value. */
  int matches = sscanf(buffer, "%d %d %d", &xres, &yres, &bpp);
  if (matches < 2)
    return -EINVAL;
  error = vga_set_videomode(vga, xres, yres, bpp);
  if (error)
    return error;
  return 0;
}

static int videomode_read(vnode_t *v, uio_t *uio) {
  vga_device_t *vga = (vga_device_t *)v->v_data;
  unsigned xres, yres, bpp;
  int error = vga_get_videomode(vga, &xres, &yres, &bpp);
  if (error)
    return error;
  char buffer[RES_CTRL_BUFFER_SIZE];
  error = snprintf(buffer, RES_CTRL_BUFFER_SIZE, "%d %d %d", xres, yres, bpp);
  if (error < 0)
    return error;
  if (error >= RES_CTRL_BUFFER_SIZE)
    return -EINVAL;
  error = uiomove_frombuf(buffer, RES_CTRL_BUFFER_SIZE, uio);
  if (error)
    return error;
  return 0;
}

static vnodeops_t videomode_vnodeops = {
  .v_open = vnode_open_generic,
  .v_read = videomode_read,
  .v_write = videomode_write};

void dev_vga_install(vga_device_t *vga) {
  devfs_node_t *vga_root;

  /* Only a single vga device may be installed at /dev/vga. */
  if (devfs_makedir(NULL, "vga", &vga_root))
    return;

  vnodeops_init(&framebuffer_vnodeops);
  devfs_makedev(vga_root, "fb", &framebuffer_vnodeops, vga);
  vnodeops_init(&palette_vnodeops);
  devfs_makedev(vga_root, "palette", &palette_vnodeops, vga);
  vnodeops_init(&videomode_vnodeops);
  devfs_makedev(vga_root, "videomode", &videomode_vnodeops, vga);
}
