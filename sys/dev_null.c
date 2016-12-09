#include <vnode.h>
#include <mount.h>
#include <devfs.h>
#include <malloc.h>

vnode_t *dev_null_device;
vnode_t *dev_zero_device;
vm_page_t *zero_page, *junk_page;

static int dev_null_write(vnode_t *t, uio_t *uio) {
  uio->uio_resid = 0;
  return 0;
}

static int dev_null_read(vnode_t *t, uio_t *uio) {
  return 0;
}

static int dev_zero_write(vnode_t *t, uio_t *uio) {
  /* We might just discard the data, but to demonstrate using uiomove for
   * writing, store the data into a junkyard page. */
  int error = 0;
  while (uio->uio_resid && !error) {
    size_t len = uio->uio_resid;
    if (len > PAGESIZE)
      len = PAGESIZE;
    error = uiomove((void *)junk_page->vaddr, len, uio);
  }
  return error;
}

static int dev_zero_read(vnode_t *t, uio_t *uio) {
  int error = 0;
  while (uio->uio_resid && !error) {
    size_t len = uio->uio_resid;
    if (len > PAGESIZE)
      len = PAGESIZE;
    error = uiomove((void *)zero_page->vaddr, len, uio);
  }
  return error;
}

vnodeops_t dev_null_vnodeops = {
  .v_lookup = vnode_op_notsup,
  .v_readdir = vnode_op_notsup,
  .v_open = vnode_op_notsup,
  .v_write = dev_null_write,
  .v_read = dev_null_read,
};

vnodeops_t dev_zero_vnodeops = {
  .v_lookup = vnode_op_notsup,
  .v_readdir = vnode_op_notsup,
  .v_open = vnode_op_notsup,
  .v_write = dev_zero_write,
  .v_read = dev_zero_read,
};

void init_dev_null() {
  zero_page = pm_alloc(1);
  junk_page = pm_alloc(1);

  dev_null_device = vnode_new(V_DEV, &dev_null_vnodeops);
  dev_zero_device = vnode_new(V_DEV, &dev_zero_vnodeops);

  devfs_install("null", dev_null_device);
  devfs_install("zero", dev_zero_device);
}
