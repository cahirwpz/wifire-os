#define KL_LOG KL_FILE
#include <sys/klog.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/uio.h>

int do_close(proc_t *p, int fd) {
  return fdtab_close_fd(p->p_fdtable, fd);
}

int do_read(proc_t *p, int fd, uio_t *uio) {
  file_t *f;
  int error;

  if ((error = fdtab_get_file(p->p_fdtable, fd, FF_READ, &f)))
    return error;

  uio->uio_ioflags |= f->f_flags & IO_MASK;
  error = f->f_ops->fo_read(f, uio);
  file_drop(f);
  return error;
}

int do_write(proc_t *p, int fd, uio_t *uio) {
  file_t *f;
  int error;

  if ((error = fdtab_get_file(p->p_fdtable, fd, FF_WRITE, &f)))
    return error;

  uio->uio_ioflags |= f->f_flags & IO_MASK;
  error = f->f_ops->fo_write(f, uio);
  if (error == EPIPE) {
    proc_lock(p);
    sig_kill(p, &DEF_KSI_RAW(SIGPIPE));
    proc_unlock(p);
  }

  file_drop(f);
  return error;
}

int do_lseek(proc_t *p, int fd, off_t offset, int whence, off_t *newoffp) {
  file_t *f;
  int error;

  if ((error = fdtab_get_file(p->p_fdtable, fd, 0, &f)))
    return error;
  error = f->f_ops->fo_seek(f, offset, whence, newoffp);
  file_drop(f);
  return error;
}

int do_fstat(proc_t *p, int fd, stat_t *sb) {
  file_t *f;
  int error;

  if ((error = fdtab_get_file(p->p_fdtable, fd, 0, &f)))
    return error;
  error = f->f_ops->fo_stat(f, sb);
  file_drop(f);
  return error;
}

int do_dup(proc_t *p, int oldfd, int *newfdp) {
  file_t *f;
  int error;

  if ((error = fdtab_get_file(p->p_fdtable, oldfd, 0, &f)))
    return error;

  error = fdtab_install_file(p->p_fdtable, f, 0, newfdp);
  file_drop(f);
  return error;
}

int do_dup2(proc_t *p, int oldfd, int newfd) {
  file_t *f;
  int error;

  if (oldfd == newfd)
    return 0;

  if ((error = fdtab_get_file(p->p_fdtable, oldfd, 0, &f)))
    return error;

  error = fdtab_install_file_at(p->p_fdtable, f, newfd);
  file_drop(f);
  return error;
}

int do_fcntl(proc_t *p, int fd, int cmd, int arg, int *resp) {
  file_t *f;
  int error;

  if ((error = fdtab_get_file(p->p_fdtable, fd, 0, &f)))
    return error;

  bool cloexec = false;
  int flags = 0;
  switch (cmd) {
    case F_DUPFD_CLOEXEC:
      cloexec = true;
      /* FALLTHROUGH */
    case F_DUPFD:
      if ((error = fdtab_install_file(p->p_fdtable, f, arg, resp)))
        break;
      error = fd_set_cloexec(p->p_fdtable, fd, cloexec);
      break;

    case F_SETFD:
      if (arg == FD_CLOEXEC)
        cloexec = true;
      error = fd_set_cloexec(p->p_fdtable, fd, cloexec);
      break;

    case F_GETFD:
      error = fd_get_cloexec(p->p_fdtable, fd, resp);
      break;

    case F_GETFL:
      if (f->f_flags & FF_READ && f->f_flags & FF_WRITE) {
        flags |= O_RDWR;
      } else {
        if (f->f_flags & FF_READ)
          flags |= O_RDONLY;
        if (f->f_flags & FF_WRITE)
          flags |= O_WRONLY;
      }
      if (f->f_flags & IO_APPEND)
        flags |= O_APPEND;
      if (f->f_flags & IO_NONBLOCK)
        flags |= O_NONBLOCK;
      *resp = flags;
      break;

    case F_SETFL:
      if (arg & O_APPEND)
        flags |= IO_APPEND;
      if (arg & O_NONBLOCK)
        flags |= IO_NONBLOCK;
      f->f_flags = flags;
      break;

    default:
      error = EINVAL;
      break;
  }

  file_drop(f);
  return error;
}

int do_ioctl(proc_t *p, int fd, u_long cmd, void *data) {
  file_t *f;
  int error;

  if ((error = fdtab_get_file(p->p_fdtable, fd, 0, &f)))
    return error;
  error = f->f_ops->fo_ioctl(f, cmd, data);
  file_drop(f);
  if (error == EPASSTHROUGH)
    error = ENOTTY;
  return error;
}

int do_umask(proc_t *p, int newmask, int *oldmaskp) {
  *oldmaskp = p->p_cmask;
  p->p_cmask = newmask & ALLPERMS;
  return 0;
}
