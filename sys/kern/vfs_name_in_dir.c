#include <sys/klog.h>
#include <sys/dirent.h>
#include <sys/vnode.h>
#include <sys/vm_map.h>
#include <sys/malloc.h>
#include <sys/libkern.h>
#include <sys/vfs.h>

int vfs_name_in_dir(vnode_t *dir, vnode_t *node, char *bufp, size_t *buflen) {
  int error = 0;
  char *buf = kmalloc(M_TEMP, PATH_MAX, 0);

  vattr_t va;
  VOP_GETATTR(node, &va);

  int offset = 0;
  uio_t uio = UIO_SINGLE_KERNEL(UIO_READ, offset, buf, PATH_MAX);
  VOP_READDIR(dir, &uio, NULL);

  while (uio.uio_offset != offset) {
    int nread = uio.uio_offset - offset;
    for (dirent_t *dir = (dirent_t *)buf; (char *)dir < buf + nread;
         dir = (dirent_t *)((char *)dir + dir->d_reclen)) {
      if (dir->d_fileno == va.va_ino) {
        size_t len = strlen(dir->d_name);
        if (*buflen < len) {
          error = ENAMETOOLONG;
        } else {
          *buflen -= len;
          memcpy(bufp + *buflen, dir->d_name, len);
        }
        goto end;
      }

      if (dir->d_reclen == 0) {
        panic("Failed to find child node in parent directory");
      }
    }

    offset = uio.uio_offset;
    uio = UIO_SINGLE_KERNEL(UIO_READ, offset, buf, PATH_MAX);
    VOP_READDIR(dir, &uio, NULL);
  }
  panic("Failed to find child node in parent directory");
end:
  kfree(M_TEMP, buf);
  return error;
}
