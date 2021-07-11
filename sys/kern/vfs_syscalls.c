#define KL_LOG KL_VFS
#include <sys/klog.h>
#include <sys/filedesc.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/vfs.h>
#include <sys/vnode.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <sys/libkern.h>
#include <sys/statvfs.h>
#include <sys/cred.h>

static int vfs_nameresolveat(proc_t *p, int fdat, vnrstate_t *vs) {
  file_t *f;
  int error;

  if (fdat != AT_FDCWD) {
    if ((error = fdtab_get_file(p->p_fdtable, fdat, FF_READ, &f)))
      return error;
    vs->vs_atdir = f->f_vnode;
  }
  error = vfs_nameresolve(vs);
  if (fdat != AT_FDCWD)
    file_drop(f);

  return error;
}

static int vfs_namelookupat(proc_t *p, int fdat, uint32_t flags,
                            const char *path, vnode_t **vp) {
  vnrstate_t vs;
  int error;

  if ((error = vnrstate_init(&vs, VNR_LOOKUP, flags, path, &p->p_cred)))
    return error;

  error = vfs_nameresolveat(p, fdat, &vs);
  *vp = vs.vs_vp;

  vnrstate_destroy(&vs);
  return error;
}

static int vfs_truncate(vnode_t *v, size_t len, cred_t *cred) {
  vattr_t va;
  vattr_null(&va);
  va.va_size = len;
  return VOP_SETATTR(v, &va, cred);
}

/* This function cleans O_CREAT in flags when file is not being created. */
static int vfs_create(proc_t *p, int fdat, char *pathname, int *flags, int mode,
                      vnode_t **vp) {
  vnrstate_t vs;
  int error;

  if ((error =
         vnrstate_init(&vs, VNR_CREATE, VNR_FOLLOW, pathname, &p->p_cred)))
    return error;

  if ((error = vfs_nameresolveat(p, fdat, &vs)))
    goto fail;

  if (vs.vs_vp == NULL) {
    if ((error = VOP_ACCESS(vs.vs_dvp, VWRITE, &p->p_cred))) {
      vnode_put(vs.vs_dvp);
      goto fail;
    }
    vattr_t va, dva;
    if ((error = VOP_GETATTR(vs.vs_dvp, &dva))) {
      vnode_put(vs.vs_dvp);
      goto fail;
    }

    vattr_null(&va);
    va.va_mode = S_IFREG | (mode & ALLPERMS);
    va.va_uid = p->p_cred.cr_euid;
    va.va_gid = dva.va_mode & S_ISGID ? dva.va_gid : p->p_cred.cr_egid;
    error = VOP_CREATE(vs.vs_dvp, &vs.vs_lastcn, &va, &vs.vs_vp);
    vnode_put(vs.vs_dvp);
  } else {
    if (vs.vs_vp == vs.vs_dvp)
      vnode_drop(vs.vs_dvp);
    else
      vnode_put(vs.vs_dvp);

    if (*flags & O_EXCL) {
      vnode_drop(vs.vs_vp);
      error = EEXIST;
    }
    *flags &= ~O_CREAT;
  }
  *vp = vs.vs_vp;

fail:
  vnrstate_destroy(&vs);
  return error;
}

static int vfs_check_open(vnode_t *v, int flags, cred_t *cred) {
  mode_t accmode = 0;
  switch (flags & O_ACCMODE) {
    case O_RDONLY:
      accmode = VREAD;
      break;
    case O_WRONLY:
      accmode = VWRITE;
      break;
    case O_RDWR:
      accmode = VREAD | VWRITE;
      break;
  }
  if (flags & O_TRUNC)
    accmode |= VWRITE;

  return VOP_ACCESS(v, accmode, cred);
}

static int vfs_open(proc_t *p, file_t *f, int fdat, char *pathname, int flags,
                    int mode) {
  vnode_t *v;
  int error;

  if (flags & O_CREAT) {
    /* XXX O_CREAT can be cleaned here */
    if ((error = vfs_create(p, fdat, pathname, &flags, mode, &v)))
      return error;
  } else {
    if ((error = vfs_namelookupat(p, fdat, VNR_FOLLOW, pathname, &v)))
      return error;
  }

  if (!(flags & O_CREAT))
    if ((error = vfs_check_open(v, flags, &p->p_cred)))
      return error;

  if (flags & O_TRUNC)
    error = vfs_truncate(v, 0, &p->p_cred);

  if (!error)
    error = VOP_OPEN(v, flags, f);

  /* Drop our reference to v. We received it from vfs_namelookup, but we no
     longer need it - file f keeps its own reference to v after open. */
  vnode_drop(v);
  return error;
}

/* Unlocks and decrements reference counter for both vnode and its parent */
static void vnode_put_both(vnode_t *v, vnode_t *dv) {
  if (dv == v)
    vnode_drop(dv);
  else
    vnode_put(dv);
  vnode_put(v);
}

static void vnode_drop_both(vnode_t *v, vnode_t *dv) {
  vnode_put(dv);
  vnode_drop(v);
}

int do_open(proc_t *p, char *pathname, int flags, mode_t mode, int *fdp) {
  return do_openat(p, AT_FDCWD, pathname, flags, mode, fdp);
}

int do_openat(proc_t *p, int fdat, char *pathname, int flags, mode_t mode,
              int *fdp) {
  int error;

  /* Allocate a file structure, but do not install descriptor yet. */
  file_t *f = file_alloc();

  /* According to POSIX, the effect when other than permission bits are set in
   * mode is unspecified. Our implementation honors these.
   * https://pubs.opengroup.org/onlinepubs/9699919799/functions/open.html */
  mode = (mode & ~p->p_cmask) & ALLPERMS;

  /* Try opening file. Fill the file structure. */
  if ((error = vfs_open(p, f, fdat, pathname, flags, mode)))
    goto fail;
  /* Now install the file in descriptor table. */
  if ((error = fdtab_install_file(p->p_fdtable, f, 0, fdp)))
    goto fail;
  /* Set cloexec flag. */
  if (flags & O_CLOEXEC)
    return fd_set_cloexec(p->p_fdtable, *fdp, true);
  return 0;

fail:
  file_destroy(f);
  return error;
}

int do_fstatat(proc_t *p, int fd, char *path, stat_t *sb, int flag) {
  vnode_t *v;
  vattr_t va;
  int error;
  uint32_t vnrflags = 0;

  if (!(flag & AT_SYMLINK_NOFOLLOW))
    vnrflags |= VNR_FOLLOW;

  if ((error = vfs_namelookupat(p, fd, vnrflags, path, &v)))
    return error;

  if (!(error = VOP_GETATTR(v, &va)))
    vattr_convert(&va, sb);

  vnode_drop(v);
  return error;
}

int do_mount(proc_t *p, const char *source, const char *fs, const char *path) {
  vfsconf_t *vfs;
  vnode_t *vdest, *vsource = NULL;
  int error;

  if (!(vfs = vfs_get_by_name(fs)))
    return EINVAL;
  if ((error = vfs_namelookup(path, &vdest, &p->p_cred)))
    return error;
  if (source && (error = vfs_namelookup(source, &vsource, &p->p_cred)))
    return error;

  return vfs_domount(vfs, vdest, vsource);
}

int do_getdents(proc_t *p, int fd, uio_t *uio) {
  file_t *f;
  int error;

  if ((error = fdtab_get_file(p->p_fdtable, fd, FF_READ, &f)))
    return error;

  uio->uio_offset = f->f_offset;
  error = VOP_READDIR(f->f_vnode, uio);
  f->f_offset = uio->uio_offset;
  file_drop(f);
  return error;
}

static int vfs_check_remove(vnode_t *dvp, vnode_t *vp, cred_t *cred) {
  vattr_t dva;
  accmode_t diracc = VWRITE;
  int error;
  if ((error = VOP_GETATTR(dvp, &dva)))
    return error;

  if (dva.va_mode & S_ISTXT)
    if ((error = VOP_ACCESS(vp, VADMIN, cred)))
      diracc |= VADMIN;

  return VOP_ACCESS(dvp, diracc, cred);
}
int do_unlinkat(proc_t *p, int fd, char *path, int flag) {
  vnrstate_t vs;
  int error;

  if ((error = vnrstate_init(&vs, VNR_DELETE, 0, path, &p->p_cred)))
    return error;

  if ((error = vfs_nameresolveat(p, fd, &vs)))
    goto fail;

  if (vs.vs_vp->v_mountedhere != NULL)
    error = EBUSY;
  else if (vs.vs_vp == vs.vs_dvp) /* No rmdir "." please */
    error = EINVAL;
  else if (vs.vs_vp->v_type == V_DIR) {
    if (!(flag & AT_REMOVEDIR))
      error = EPERM;
    else if (!(error = vfs_check_remove(vs.vs_dvp, vs.vs_vp, &p->p_cred)))
      error = VOP_RMDIR(vs.vs_dvp, vs.vs_vp, &vs.vs_lastcn);
  } else {
    if (flag & AT_REMOVEDIR)
      error = ENOTDIR;
    else if (!(error = vfs_check_remove(vs.vs_dvp, vs.vs_vp, &p->p_cred)))
      error = VOP_REMOVE(vs.vs_dvp, vs.vs_vp, &vs.vs_lastcn);
  }

  vnode_put_both(vs.vs_vp, vs.vs_dvp);

fail:
  vnrstate_destroy(&vs);
  return error;
}

int do_mkdirat(proc_t *p, int fd, char *path, mode_t mode) {
  vnrstate_t vs;
  vattr_t va, dva;
  int error;

  if ((error = vnrstate_init(&vs, VNR_CREATE, VNR_FOLLOW, path, &p->p_cred)))
    return error;

  if ((error = vfs_nameresolveat(p, fd, &vs)))
    goto fail;

  if (vs.vs_vp != NULL) {
    vnode_drop_both(vs.vs_vp, vs.vs_dvp);
    error = EEXIST;
    goto fail;
  }

  if ((error = VOP_ACCESS(vs.vs_dvp, VWRITE, &p->p_cred))) {
    vnode_put(vs.vs_dvp);
    goto fail;
  }

  if ((error = VOP_GETATTR(vs.vs_dvp, &dva))) {
    vnode_put(vs.vs_dvp);
    goto fail;
  }

  memset(&va, 0, sizeof(vattr_t));
  /* We discard all bits but permission bits, since it is
   * implementation-defined.
   * https://pubs.opengroup.org/onlinepubs/9699919799/functions/mkdir.html */
  va.va_mode = S_IFDIR | ((mode & ACCESSPERMS) & ~p->p_cmask);
  va.va_uid = p->p_cred.cr_euid;
  if (dva.va_mode & S_ISGID) {
    /* We propagate set-group-id down */
    va.va_mode |= S_ISGID;
    va.va_gid = dva.va_gid;
  } else {
    va.va_gid = p->p_cred.cr_egid;
  }

  error = VOP_MKDIR(vs.vs_dvp, &vs.vs_lastcn, &va, &vs.vs_vp);
  if (!error)
    vnode_drop(vs.vs_vp);

  vnode_put(vs.vs_dvp);

fail:
  vnrstate_destroy(&vs);
  return error;
}

int do_faccessat(proc_t *p, int fd, char *path, int mode, int flags) {
  int error;
  uint32_t vnrflags = 0;

  if (!(flags & AT_SYMLINK_NOFOLLOW))
    vnrflags |= VNR_FOLLOW;

  /* Check if access mode argument is valid. */
  if (mode & ~(R_OK | W_OK | X_OK))
    return EINVAL;

  vnode_t *v;
  if ((error = vfs_namelookupat(p, fd, vnrflags, path, &v)))
    return error;

  /* TODO handle AT_EACCESS: Use the effective user and group IDs instead of
     the real user and group IDs for checking permission.*/
  error = VOP_ACCESS(v, mode, &p->p_cred);
  vnode_drop(v);
  return error;
}

int do_getcwd(proc_t *p, char *buf, size_t *lastp) {
  assert(*lastp == PATH_MAX);

  vnode_get(p->p_cwd);
  vnode_t *uvp = p->p_cwd;
  vnode_t *lvp = NULL;
  int error = 0;

  /* Last writable position in provided buffer. */
  size_t last = *lastp;

  /* Let's start with terminating NUL. */
  buf[--last] = '\0';

  /* Handle special case for root directory. */
  vfs_maybe_ascend(&uvp);

  if (uvp == vfs_root_vnode) {
    buf[--last] = '/';
    goto end;
  }

  do {
    componentname_t cn = COMPONENTNAME("..");
    if ((error = VOP_LOOKUP(uvp, &cn, &lvp)))
      break;

    if (uvp == lvp) {
      error = ENOENT;
      break;
    }

    if ((error = vfs_name_in_dir(lvp, uvp, buf, &last)))
      break;

    if (last == 0) {
      error = ENAMETOOLONG;
      break;
    }

    buf[--last] = '/'; /* Prepend component separator. */

    vnode_put(uvp);
    vnode_lock(lvp);
    vfs_maybe_ascend(&lvp);
    uvp = lvp;
    lvp = NULL;
  } while (uvp != vfs_root_vnode);

end:
  vnode_put(uvp);
  if (lvp)
    vnode_drop(lvp);
  *lastp = last;
  return error;
}

int do_truncate(proc_t *p, char *path, off_t length) {
  int error;
  vnode_t *vn;

  if ((error = vfs_namelookup(path, &vn, &p->p_cred)))
    return error;
  vnode_lock(vn);
  if (vn->v_type == V_DIR)
    error = EISDIR;
  else if (!(error = VOP_ACCESS(vn, VWRITE, &p->p_cred)))
    error = vfs_truncate(vn, length, &p->p_cred);

  vnode_put(vn);
  return error;
}

int do_ftruncate(proc_t *p, int fd, off_t length) {
  int error;
  file_t *f;

  if ((error = fdtab_get_file(p->p_fdtable, fd, FF_WRITE, &f)))
    return error;

  vnode_t *vn = f->f_vnode;
  vnode_lock(vn);
  if (vn->v_type == V_DIR)
    error = EINVAL;
  else
    error = vfs_truncate(vn, length, &p->p_cred);

  vnode_unlock(vn);
  file_drop(f);
  return error;
}

ssize_t do_readlinkat(proc_t *p, int fd, char *path, uio_t *uio) {
  vnode_t *v;
  int error;

  if ((error = vfs_namelookupat(p, fd, 0, path, &v)))
    return error;

  if (v->v_type != V_LNK)
    error = EINVAL;
  else if (!(error = VOP_ACCESS(v, VREAD, &p->p_cred)))
    error = VOP_READLINK(v, uio);

  vnode_drop(v);
  return error;
}

int do_chdir(proc_t *p, const char *path) {
  vnode_t *cwd;
  int error;

  if ((error = vfs_namelookup(path, &cwd, &p->p_cred)))
    return error;

  if (cwd->v_type != V_DIR) {
    /* drop our reference to cwd that was set by vfs_namelookup - we don't
     * longer need it */
    vnode_drop(cwd);
    return ENOTDIR;
  }

  vnode_drop(p->p_cwd);
  p->p_cwd = cwd;
  return 0;
}

int do_fchdir(proc_t *p, int fd) {
  file_t *f;
  int error;

  if ((error = fdtab_get_file(p->p_fdtable, fd, FF_READ, &f)))
    return error;

  vnode_t *v = f->f_vnode;
  if (v->v_type == V_DIR) {
    vnode_t *old_cwd = p->p_cwd;
    vnode_hold(v);
    p->p_cwd = v;
    vnode_drop(old_cwd);
  } else {
    error = ENOTDIR;
  }

  file_drop(f);
  return error;
}

int do_symlinkat(proc_t *p, char *target, int newdirfd, char *linkpath) {
  vnrstate_t vs;
  vattr_t va;
  int error;

  if ((error = vnrstate_init(&vs, VNR_CREATE, 0, linkpath, &p->p_cred)))
    return error;

  if ((error = vfs_nameresolveat(p, newdirfd, &vs)))
    goto fail;

  if (vs.vs_vp != NULL) {
    vnode_drop_both(vs.vs_vp, vs.vs_dvp);

    error = EEXIST;
    goto fail;
  }

  memset(&va, 0, sizeof(vattr_t));
  va.va_mode = S_IFLNK | (ACCESSPERMS & ~p->p_cmask);
  va.va_uid = p->p_cred.cr_ruid;
  va.va_gid = p->p_cred.cr_rgid;

  error = VOP_SYMLINK(vs.vs_dvp, &vs.vs_lastcn, &va, target, &vs.vs_vp);
  if (!error)
    vnode_drop(vs.vs_vp);
  vnode_put(vs.vs_dvp);

fail:
  vnrstate_destroy(&vs);
  return error;
}

int do_linkat(proc_t *p, int fd, char *path, int linkfd, char *linkpath,
              int flags) {
  vnrstate_t vs;
  vnode_t *target_vn;
  int error;
  uint32_t vnrflags = 0;

  if (flags & AT_SYMLINK_FOLLOW)
    vnrflags |= VNR_FOLLOW;

  if ((error = vfs_namelookupat(p, fd, vnrflags, path, &target_vn)))
    return error;

  vnode_lock(target_vn);

  if (target_vn->v_type == V_DIR) {
    error = EPERM;
    goto fail1;
  }

  if ((error =
         vnrstate_init(&vs, VNR_CREATE, VNR_FOLLOW, linkpath, &p->p_cred)))
    goto fail1;

  if ((error = vfs_nameresolveat(p, linkfd, &vs)))
    goto fail2;

  if (vs.vs_vp != NULL) {
    vnode_drop_both(vs.vs_vp, vs.vs_dvp);
    error = EEXIST;
    goto fail2;
  }

  if (vs.vs_dvp->v_mount != target_vn->v_mount)
    error = EXDEV;
  else
    error = VOP_LINK(vs.vs_dvp, target_vn, &vs.vs_lastcn);

  vnode_put(vs.vs_dvp);

fail2:
  vnrstate_destroy(&vs);
fail1:
  vnode_put(target_vn);
  return error;
}

static int vfs_change_mode(vnode_t *v, mode_t mode, cred_t *cred) {
  vattr_t va;
  vattr_null(&va);
  va.va_mode = mode & ALLPERMS;
  return VOP_SETATTR(v, &va, cred);
}

static int vfs_change_owner(vnode_t *v, uid_t uid, gid_t gid, cred_t *cred) {
  vattr_t va;
  vattr_null(&va);
  va.va_uid = uid;
  va.va_gid = gid;
  return VOP_SETATTR(v, &va, cred);
}

int do_fchmod(proc_t *p, int fd, mode_t mode) {
  int error;
  file_t *f;

  if ((error = fdtab_get_file(p->p_fdtable, fd, FF_WRITE, &f)))
    return error;

  vnode_t *vn = f->f_vnode;
  vnode_lock(vn);
  error = vfs_change_mode(vn, mode, &p->p_cred);
  vnode_unlock(vn);
  file_drop(f);
  return error;
}

int do_fchmodat(proc_t *p, int fd, char *path, mode_t mode, int flag) {
  int error;
  uint32_t vnrflags = 0;

  if (!(flag & AT_SYMLINK_NOFOLLOW))
    vnrflags |= VNR_FOLLOW;

  vnode_t *v;
  if ((error = vfs_namelookupat(p, fd, vnrflags, path, &v)))
    return error;
  vnode_lock(v);
  error = vfs_change_mode(v, mode, &p->p_cred);
  vnode_put(v);
  return error;
}

int do_fchown(proc_t *p, int fd, uid_t uid, gid_t gid) {
  int error;
  file_t *f;

  if ((error = fdtab_get_file(p->p_fdtable, fd, FF_WRITE, &f)))
    return error;

  vnode_t *vn = f->f_vnode;
  vnode_lock(vn);
  error = vfs_change_owner(vn, uid, gid, &p->p_cred);
  vnode_unlock(vn);
  file_drop(f);
  return error;
}

int do_fchownat(proc_t *p, int fd, char *path, uid_t uid, gid_t gid, int flag) {
  int error;
  uint32_t vnrflags = 0;

  if (!(flag & AT_SYMLINK_NOFOLLOW))
    vnrflags |= VNR_FOLLOW;

  vnode_t *v;
  if ((error = vfs_namelookupat(p, fd, vnrflags, path, &v)))
    return error;
  vnode_lock(v);
  error = vfs_change_owner(v, uid, gid, &p->p_cred);
  vnode_put(v);
  return error;
}

int do_statvfs(proc_t *p, char *path, statvfs_t *buf) {
  vnode_t *v;
  int error;

  if ((error = vfs_namelookup(path, &v, &p->p_cred)))
    return error;

  memset(buf, 0, sizeof(*buf));
  error = VFS_STATVFS(v->v_mount, buf);
  vnode_drop(v);

  return error;
}

int do_fstatvfs(proc_t *p, int fd, statvfs_t *buf) {
  file_t *f;
  int error;

  if ((error = fdtab_get_file(p->p_fdtable, fd, 0, &f)))
    return error;
  memset(buf, 0, sizeof(*buf));
  error = VFS_STATVFS(f->f_vnode->v_mount, buf);
  file_drop(f);

  return error;
}

static int vfs_utimens(vnode_t *v, timespec_t times[2], cred_t *cred) {
  vattr_t va;
  vattr_null(&va);

  if (times == NULL) {
    va.va_atime = va.va_mtime = nanotime();
    va.va_flags = VA_UTIMES_NULL;
  } else {
    va.va_atime = times[0];
    va.va_mtime = times[1];
  }

  return VOP_SETATTR(v, &va, cred);
}

int do_futimens(proc_t *p, int fd, timespec_t *times) {
  int error;
  file_t *f;

  if ((error = fdtab_get_file(p->p_fdtable, fd, FF_WRITE, &f)))
    return error;

  vnode_t *vn = f->f_vnode;
  vnode_lock(vn);
  error = vfs_utimens(vn, times, &p->p_cred);
  vnode_unlock(vn);
  file_drop(f);
  return error;
}

int do_utimensat(proc_t *p, int fd, char *path, timespec_t *times, int flag) {
  int error;
  uint32_t vnrflags = 0;

  if (!(flag & AT_SYMLINK_NOFOLLOW))
    vnrflags |= VNR_FOLLOW;

  vnode_t *v;
  if ((error = vfs_namelookupat(p, fd, vnrflags, path, &v)))
    return error;
  vnode_lock(v);
  error = vfs_utimens(v, times, &p->p_cred);
  vnode_put(v);
  return error;
}
