#include <sys/vnode.h>
#include <sys/devfs.h>
#include <sys/errno.h>
#include <sys/libkern.h>
#include <dev/vga.h>
#include <sys/fb.h>
#include <stdatomic.h>

static int vga_open(vnode_t *v, int mode, file_t *fp) {
  int error;
  vga_device_t *vga = devfs_node_data(v);

  /* Disallow opening the file more than once. */
  int expected = 0;
  if (!atomic_compare_exchange_strong(&vga->vg_usecnt, &expected, 1))
    return EMFILE;

  /* On error, decrease the use count. */
  if ((error = vnode_open_generic(v, mode, fp)))
    atomic_store(&vga->vg_usecnt, 0);

  return error;
}

static int vga_close(vnode_t *v, file_t *fp) {
  vga_device_t *vga = devfs_node_data(v);
  atomic_store(&vga->vg_usecnt, 0);
  return 0;
}

static int vga_write(vnode_t *v, uio_t *uio, int ioflag) {
  return vga_fb_write(devfs_node_data(v), uio);
}

static int vga_ioctl_set_palette(vga_device_t *vga,
                                 fb_palette_t *user_palette) {
  int error;
  size_t len = user_palette->len;
  fb_palette_t *palette = fb_palette_create(len);

  if ((error = copyin(user_palette->red, palette->red, len)))
    goto end;
  if ((error = copyin(user_palette->green, palette->green, len)))
    goto end;
  if ((error = copyin(user_palette->blue, palette->blue, len)))
    goto end;

  error = vga_palette_write(vga, palette);

end:
  fb_palette_destroy(palette);

  return error;
}

static int vga_ioctl(vnode_t *v, u_long cmd, void *data) {
  vga_device_t *vga = devfs_node_data(v);

  switch (cmd) {
    case FBIOCGET_FBINFO:
      return vga_get_fbinfo(vga, data);
    case FBIOCSET_FBINFO:
      return vga_set_fbinfo(vga, data);
    case FBIOCSET_PALETTE:
      return vga_ioctl_set_palette(vga, data);
  }

  return EINVAL;
}

static vnodeops_t vga_vnodeops = {
  .v_open = vga_open,
  .v_close = vga_close,
  .v_write = vga_write,
  .v_ioctl = vga_ioctl,
};

void dev_vga_install(vga_device_t *vga) {
  devfs_makedev(NULL, "vga", &vga_vnodeops, vga, NULL);
}
