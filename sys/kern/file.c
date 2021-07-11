#include <sys/file.h>
#include <sys/pool.h>
#include <sys/libkern.h>
#include <sys/errno.h>
#include <sys/mutex.h>
#include <sys/vnode.h>
#include <sys/vfs.h>

static POOL_DEFINE(P_FILE, "file", sizeof(file_t), 16);

file_t *file_alloc(void) {
  file_t *f = pool_alloc(P_FILE, M_ZERO);
  f->f_ops = &badfileops;
  return f;
}

/* May be called after the last reference to a file has been dropped. */
void file_destroy(file_t *f) {
  assert(f->f_count == 0);

  /* Note: If the file failed to open, we shall not close it. In such case its
     fileops are set to badfileops. */
  /* TODO: What if an error happens during close? */
  if (f->f_ops != &badfileops)
    f->f_ops->fo_close(f);

  pool_free(P_FILE, f);
}

void file_hold(file_t *f) {
  refcnt_acquire(&f->f_count);
}

void file_drop(file_t *f) {
  if (refcnt_release(&f->f_count))
    file_destroy(f);
}

int nowrite(file_t *f, uio_t *uio) {
  return EBADF;
}

int noseek(file_t *f, off_t offset, int whence, off_t *newoffp) {
  return ESPIPE;
}

/* Operations on invalid file descriptors */
static int badfo_read(file_t *f, uio_t *uio) {
  return EOPNOTSUPP;
}

static int badfo_write(file_t *f, uio_t *uio) {
  return EOPNOTSUPP;
}

static int badfo_close(file_t *f) {
  return EOPNOTSUPP;
}

static int badfo_stat(file_t *f, stat_t *sb) {
  return EOPNOTSUPP;
}

static int badfo_seek(file_t *f, off_t offset, int whence, off_t *newoffp) {
  return EOPNOTSUPP;
}

static int badfo_ioctl(file_t *f, u_long cmd, void *data) {
  return EOPNOTSUPP;
}

fileops_t badfileops = {
  .fo_read = badfo_read,
  .fo_write = badfo_write,
  .fo_close = badfo_close,
  .fo_stat = badfo_stat,
  .fo_seek = badfo_seek,
  .fo_ioctl = badfo_ioctl,
};
