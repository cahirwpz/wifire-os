#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/devfs.h>
#include <sys/errno.h>
#include <sys/uio.h>
#include <sys/console.h>
#include <sys/linker_set.h>

#define UART_BUF_MAX 100

static int dev_cons_write(devnode_t *dev, uio_t *uio) {
  char buffer[UART_BUF_MAX];
  size_t n = uio->uio_resid;
  int res = uiomove(buffer, UART_BUF_MAX - 1, uio);
  if (res)
    return res;
  size_t moved = n - uio->uio_resid;
  cn_write(buffer, moved);
  return 0;
}

static int dev_cons_read(devnode_t *dev, uio_t *uio) {
  char buffer[UART_BUF_MAX];
  unsigned curr = 0;
  while (curr < UART_BUF_MAX && curr < uio->uio_resid) {
    buffer[curr] = cn_getc();
    if (buffer[curr++] == '\n')
      break;
  }
  uio->uio_offset = 0; /* This device does not support offsets. */
  return uiomove_frombuf(buffer, curr, uio);
}

static devops_t dev_cons_ops = {
  .d_type = DT_OTHER,
  .d_read = dev_cons_read,
  .d_write = dev_cons_write,
};

static void init_dev_cons(void) {
  devfs_makedev_new(NULL, "cons", &dev_cons_ops, NULL, NULL);
}

SET_ENTRY(devfs_init, init_dev_cons);
