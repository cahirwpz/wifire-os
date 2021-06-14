#define KL_LOG KL_FILESYS
#include <sys/klog.h>
#include <sys/devfs.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/errno.h>
#include <sys/libkern.h>
#include <sys/malloc.h>
#include <sys/linker_set.h>
#include <sys/dirent.h>
#include <sys/vfs.h>
#include <sys/queue.h>
#include <sys/stat.h>

static KMALLOC_DEFINE(M_DEVFS, "devfs");

/* default permissions for devfs nodes */
#define DEVFS_FILE_MODE                                                        \
  (S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)

#define DEVFS_DIR_MODE                                                         \
  (S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)

/*
 * Device filesystem node.
 */

#define DEVFS_NAME_MAX 64

typedef TAILQ_HEAD(, devfs_node) devfs_node_list_t;

struct devfs_node {
  char dn_name[DEVFS_NAME_MAX]; /* device file name */
  vnode_t *dn_vnode;            /* corresponding v-node */
  devnode_t dn_device;          /* device-specific data  */

  /* Node attributes (as in vattr). */
  nlink_t dn_nlinks;   /* number of hard links */
  ino_t dn_ino;        /* node identifier */
  timespec_t dn_atime; /* last access time */
  timespec_t dn_mtime; /* last data modification time */
  timespec_t dn_ctime; /* last file status change time */

  /* Directory-specific data. */
  devfs_node_list_t dn_children; /* list of children nodes */

  devfs_node_t *dn_parent;         /* parent devfs node */
  TAILQ_ENTRY(devfs_node) dn_link; /* link on `dn_parent->dn_children` */
};

/*
 * Device filesystem internal state structure
 */
typedef struct devfs_mount {
  mtx_t lock;         /* guards the following fields */
  ino_t next_ino;     /* next node identifier to grant */
  devfs_node_t *root; /* root devfs node of a devfs instance */
} devfs_mount_t;

/* The only mount point for devfs. */
static devfs_mount_t devfs = {
  .lock = MTX_INITIALIZER(devfs.lock, 0),
  .next_ino = 2, /* traditional root i-node number in unix-like systems */
  .root = NULL,
};

/*
 * Devfs internal functions.
 */

static inline devfs_node_t *devfs_node_of(vnode_t *v) {
  return (devfs_node_t *)v->v_data;
}

static devfs_node_t *devfs_find_child(devfs_node_t *parent,
                                      const componentname_t *cn) {
  assert(mtx_owned(&devfs.lock));

  if (parent == NULL)
    parent = devfs.root;

  devfs_node_t *dn;
  TAILQ_FOREACH (dn, &parent->dn_children, dn_link)
    if (componentname_equal(cn, dn->dn_name))
      return dn;

  if (componentname_equal(cn, ".."))
    return (parent->dn_parent != NULL ? parent->dn_parent : parent);
  else if (componentname_equal(cn, "."))
    return parent;

  return NULL;
}

static devfs_node_t *devfs_node_create(const char *name, int mode) {
  devfs_node_t *dn = kmalloc(M_DEVFS, sizeof(devfs_node_t), M_ZERO | M_WAITOK);

  strncpy(dn->dn_name, name, DEVFS_NAME_MAX);
  dn->dn_device.mode = mode;
  dn->dn_nlinks = (mode & S_IFDIR) ? 2 : 1;
  dn->dn_ino = devfs.next_ino++;
  /* UID and GID are set to 0 by `kmalloc`.
   * Note that `dn_size` is initially 0, but the device is
   * free to alter this field. */
  /* TODO: update atime & mtime & ctime */
  return dn;
}

static int devfs_add_entry(devfs_node_t *parent, const char *name, int mode,
                           devfs_node_t **dnp) {
  assert(mtx_owned(&devfs.lock));

  if (parent == NULL)
    parent = devfs.root;
  if (parent->dn_vnode->v_type != V_DIR)
    return ENOTDIR;

  if (devfs_find_child(parent, &COMPONENTNAME(name)))
    return EEXIST;

  devfs_node_t *dn = devfs_node_create(name, mode);
  dn->dn_parent = parent;
  TAILQ_INSERT_TAIL(&parent->dn_children, dn, dn_link);
  if (mode & S_IFDIR)
    parent->dn_nlinks++;
  *dnp = dn;

  return 0;
}

/* FIXME should be defined as static */
void devfs_free(devfs_node_t *dn) {
  assert(dn->dn_vnode->v_usecnt == 0);
  kfree(M_DEVFS, dn);
}

/*
 * Fileops interface for device nodes.
 */

static int devfs_fop_read(file_t *fp, uio_t *uio) {
  devnode_t *dev = fp->f_data;
  return dev->ops->d_read(dev, uio);
}

static int devfs_fop_write(file_t *fp, uio_t *uio) {
  devnode_t *dev = fp->f_data;
  return dev->ops->d_write(dev, uio);
}

static int devfs_fop_close(file_t *fp) {
  devnode_t *dev = fp->f_data;
  refcnt_release(&dev->refcnt);
  return dev->ops->d_close(dev, fp);
}

static int devfs_fop_seek(file_t *fp, off_t offset, int whence,
                          off_t *newoffp) {
  devnode_t *dev = fp->f_data;
  bool seekable = dev->ops->d_type & DT_SEEKABLE;

  if (!seekable)
    return ESPIPE;

  if (whence == SEEK_CUR) {
    offset += fp->f_offset;
  } else if (whence == SEEK_END) {
    offset += dev->size;
  } else if (whence != SEEK_SET) {
    return EINVAL;
  }

  if (offset < 0) {
    offset = 0;
  } else if ((size_t)offset > dev->size) {
    offset = dev->size;
  }

  *newoffp = fp->f_offset = offset;
  return 0;
}

static int devfs_fop_stat(file_t *fp, stat_t *sb) {
  devnode_t *dev = fp->f_data;
  int error;

  /* Take all attributes from v-node. */
  if ((error = default_vnstat(fp, sb)))
    return error;

  /* If devnode is cloned following fields may differ from the master node. */
  sb->st_mode = dev->mode;
  sb->st_uid = dev->uid;
  sb->st_gid = dev->gid;
  sb->st_size = dev->size;
  return 0;
}

static int devfs_fop_ioctl(file_t *fp, u_long cmd, void *data) {
  devnode_t *dev = fp->f_data;
  return dev->ops->d_ioctl(dev, cmd, data, fp->f_flags);
}

static fileops_t devfs_fileops = {
  .fo_read = devfs_fop_read,
  .fo_write = devfs_fop_write,
  .fo_close = devfs_fop_close,
  .fo_seek = devfs_fop_seek,
  .fo_stat = devfs_fop_stat,
  .fo_ioctl = devfs_fop_ioctl,
};

/*
 * Devfs device v-node operations.
 */

static int devfs_vop_getattr(vnode_t *v, vattr_t *va) {
  devfs_node_t *dn = devfs_node_of(v);

  bzero(va, sizeof(vattr_t));
  va->va_mode = dn->dn_device.mode;
  va->va_uid = dn->dn_device.uid;
  va->va_gid = dn->dn_device.gid;
  va->va_size = dn->dn_device.size;
  va->va_nlink = dn->dn_nlinks;
  va->va_ino = dn->dn_ino;
  va->va_atime = dn->dn_atime;
  va->va_mtime = dn->dn_mtime;
  va->va_ctime = dn->dn_ctime;
  return 0;
}

static int devfs_vop_open(vnode_t *v, int mode, file_t *fp) {
  devfs_node_t *dn = devfs_node_of(v);
  devnode_t *dev = &dn->dn_device;
  int error;

  if ((error = vnode_open_generic(v, mode, fp)))
    return error;

  fp->f_data = dev;
  fp->f_ops = &devfs_fileops;
  fp->f_type = FT_VNODE;
  fp->f_vnode = v;

  if ((error = dev->ops->d_open(dev, fp, mode)))
    return error;

  refcnt_acquire(&dev->refcnt);
  return 0;
}

/* Free a devfs device file after unlinking it. */
static int devfs_vop_reclaim(vnode_t *v) {
  devfs_node_t *dn = devfs_node_of(v);
  devnode_t *dev = &dn->dn_device;
  assert(dev->refcnt == 0);
  devfs_free(dn);
  return 0;
}

static vnodeops_t devfs_dev_vnodeops = {
  .v_open = devfs_vop_open,
  .v_access = vnode_access_generic,
  .v_getattr = devfs_vop_getattr,
  .v_reclaim = devfs_vop_reclaim,
};

/*
 * Devfs directory v-node operations.
 */

static int devfs_vop_lookup(vnode_t *dv, componentname_t *cn, vnode_t **vp) {
  SCOPED_MTX_LOCK(&devfs.lock);

  if (dv->v_type != V_DIR)
    return ENOTDIR;

  devfs_node_t *dn = devfs_find_child(devfs_node_of(dv), cn);
  if (!dn)
    return ENOENT;
  *vp = dn->dn_vnode;
  vnode_hold(*vp);
  return 0;
}

static void *devfs_dirent_next(vnode_t *v, void *it) {
  assert(it != NULL);
  if (it == DIRENT_DOT)
    return DIRENT_DOTDOT;
  if (it == DIRENT_DOTDOT)
    return TAILQ_FIRST(&devfs_node_of(v)->dn_children);
  return TAILQ_NEXT((devfs_node_t *)it, dn_link);
}

static size_t devfs_dirent_namlen(vnode_t *v, void *it) {
  assert(it != NULL);
  if (it == DIRENT_DOT)
    return 1;
  if (it == DIRENT_DOTDOT)
    return 2;
  return strlen(((devfs_node_t *)it)->dn_name);
}

static void devfs_to_dirent(vnode_t *v, void *it, dirent_t *dir) {
  assert(it != NULL);
  devfs_node_t *node;
  const char *name;
  if (it == DIRENT_DOT) {
    node = devfs_node_of(v);
    name = ".";
  } else if (it == DIRENT_DOTDOT) {
    node = devfs_node_of(v)->dn_parent;
    name = "..";
  } else {
    node = (devfs_node_t *)it;
    name = node->dn_name;
  }
  dir->d_fileno = node->dn_ino;
  dir->d_type = vt2dt(node->dn_vnode->v_type);
  memcpy(dir->d_name, name, dir->d_namlen + 1);
}

static readdir_ops_t devfs_readdir_ops = {
  .next = devfs_dirent_next,
  .namlen_of = devfs_dirent_namlen,
  .convert = devfs_to_dirent,
};

static int devfs_vop_readdir(vnode_t *v, uio_t *uio) {
  return readdir_generic(v, uio, &devfs_readdir_ops);
}

static vnodeops_t devfs_dir_vnodeops = {
  .v_lookup = devfs_vop_lookup,
  .v_readdir = devfs_vop_readdir,
  .v_getattr = devfs_vop_getattr,
  .v_reclaim = devfs_vop_reclaim,
  .v_open = vnode_open_generic,
};

/*
 * Devfs vfsops interface implementation.
 */

/* We are using a single vnode for each devfs_node
 * instead of allocating a new one each time, to simplify things. */
static int devfs_mount(mount_t *m, vnode_t *vsrc) {
  devfs.root->dn_vnode->v_mount = m;
  m->mnt_data = &devfs;
  return 0;
}

static int devfs_root(mount_t *m, vnode_t **vp) {
  *vp = devfs.root->dn_vnode;
  vnode_hold(*vp);
  return 0;
}

static int devfs_init(vfsconf_t *vfc) {
  vnodeops_init(&devfs_dir_vnodeops);
  vnodeops_init(&devfs_dev_vnodeops);

  devfs.root = devfs_node_create("", DEVFS_DIR_MODE);
  devfs_node_t *dn = devfs.root;

  dn->dn_vnode = vnode_new(V_DIR, &devfs_dir_vnodeops, dn);
  dn->dn_parent = dn;
  TAILQ_INIT(&dn->dn_children);

  /* Prepare some initial devices */
  typedef void devfs_init_func_t(void);
  SET_DECLARE(devfs_init, devfs_init_func_t);
  devfs_init_func_t **ptr;
  SET_FOREACH (ptr, devfs_init)
    (**ptr)();
  return 0;
}

static vfsops_t devfs_vfsops = {
  .vfs_mount = devfs_mount,
  .vfs_root = devfs_root,
  .vfs_init = devfs_init,
};

static vfsconf_t devfs_conf = {
  .vfc_name = "devfs",
  .vfc_vfsops = &devfs_vfsops,
};

SET_ENTRY(vfsconf, devfs_conf);

/*
 * Devfs kernel interface for device drivers.
 */

static int dev_noopen(devnode_t *dev, file_t *fp, int oflags) {
  return 0;
}

static int dev_noclose(devnode_t *dev, file_t *fp) {
  return 0;
}

static int dev_noread(devnode_t *dev, uio_t *uio) {
  return EOPNOTSUPP;
}

static int dev_nowrite(devnode_t *dev, uio_t *uio) {
  return EOPNOTSUPP;
}

static int dev_noioctl(devnode_t *dev, u_long cmd, void *data, int flags) {
  return EOPNOTSUPP;
}

static int _devfs_makedev(devfs_node_t *parent, const char *name, void *data,
                          devfs_node_t **dn_p) {
  int error;
  devfs_node_t *dn;
  if ((error = devfs_add_entry(parent, name, DEVFS_FILE_MODE, &dn)))
    return error;

  dn->dn_device.data = data;
  dn->dn_vnode = vnode_new(V_DEV, &devfs_dev_vnodeops, dn);
  *dn_p = dn;
  return 0;
}

int devfs_makedev_new(devfs_node_t *parent, const char *name, devops_t *devops,
                      void *data, devnode_t **dev_p) {
  devfs_node_t *dn;
  int error;

  WITH_MTX_LOCK (&devfs.lock) {
    if ((error = _devfs_makedev(parent, name, data, &dn)))
      return error;

    if (devops->d_open == NULL)
      devops->d_open = dev_noopen;
    if (devops->d_close == NULL)
      devops->d_close = dev_noclose;
    if (devops->d_read == NULL)
      devops->d_read = dev_noread;
    if (devops->d_write == NULL)
      devops->d_write = dev_nowrite;
    if (devops->d_ioctl == NULL)
      devops->d_ioctl = dev_noioctl;

    dn->dn_device.ops = devops;
  }

  if (dev_p)
    *dev_p = &dn->dn_device;

  klog("devfs: registered '%s' device", name);
  return 0;
}

/* TODO: remove the following function after rewriting all drivers. */
static void devfs_add_default_vops(vnodeops_t *vops) {
  if (vops->v_open == NULL)
    vops->v_open = vnode_open_generic;

  if (vops->v_access == NULL)
    vops->v_access = vnode_access_generic;

  if (vops->v_getattr == NULL)
    vops->v_getattr = devfs_vop_getattr;
}

/* TODO: remove the following function after rewriting all drivers. */
int devfs_makedev(devfs_node_t *parent, const char *name, vnodeops_t *vops,
                  void *data, vnode_t **vp) {
  devfs_node_t *dn;
  int error;

  WITH_MTX_LOCK (&devfs.lock) {
    if ((error = _devfs_makedev(parent, name, data, &dn)))
      return error;

    devfs_add_default_vops(vops);
    vnodeops_init(vops);

    vnode_t *v = dn->dn_vnode;
    v->v_ops = vops;

    if (vp) {
      vnode_hold(v);
      *vp = v;
    }
  }

  return 0;
}

void *devfs_node_data(vnode_t *v) {
  return devfs_node_of(v)->dn_device.data;
}

int devfs_makedir(devfs_node_t *parent, const char *name,
                  devfs_node_t **dir_p) {
  SCOPED_MTX_LOCK(&devfs.lock);

  devfs_node_t *dn;
  int error = devfs_add_entry(parent, name, DEVFS_DIR_MODE, &dn);
  if (error)
    return error;

  dn->dn_vnode = vnode_new(V_DIR, &devfs_dir_vnodeops, dn);
  TAILQ_INIT(&dn->dn_children);
  *dir_p = dn;
  return 0;
}

int devfs_unlink(devfs_node_t *dn) {
  SCOPED_MTX_LOCK(&devfs.lock);

  devfs_node_t *parent = dn->dn_parent;
  assert(parent != NULL);

  /* Only allow removal of empty directories. */
  if (dn->dn_vnode->v_type == V_DIR && !TAILQ_EMPTY(&dn->dn_children))
    return ENOTEMPTY;

  TAILQ_REMOVE(&parent->dn_children, dn, dn_link);
  if (dn->dn_device.mode & S_IFDIR)
    parent->dn_nlinks--;
  vnode_drop(dn->dn_vnode);
  return 0;
}
