#define KL_LOG KL_VFS
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/libkern.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/file.h>
#include <sys/vfs.h>
#include <sys/vnode.h>
#include <sys/stat.h>

/* TODO: We probably need some fancier allocation, since eventually we should
 * start recycling vnodes */
static KMALLOC_DEFINE(M_VFS, "vfs");

/* The list of all installed filesystem types */
vfsconf_list_t vfsconf_list = TAILQ_HEAD_INITIALIZER(vfsconf_list);
MTX_DEFINE(vfsconf_list_mtx, 0);

/* The list of all mounts mounted */
typedef TAILQ_HEAD(, mount) mount_list_t;
static mount_list_t mount_list = TAILQ_HEAD_INITIALIZER(mount_list);
static MTX_DEFINE(mount_list_mtx, 0);

/* Default vfs operations */
static vfs_root_t vfs_default_root;
static vfs_statvfs_t vfs_default_statvfs;
static vfs_vget_t vfs_default_vget;
static vfs_init_t vfs_default_init;

/* Global root vnodes */
vnode_t *vfs_root_vnode;

static int vfs_root_vnode_lookup(vnode_t *vdir, componentname_t *cn,
                                 vnode_t **res) {
  if (componentname_equal(cn, "..") || componentname_equal(cn, ".")) {
    vnode_hold(vfs_root_vnode);
    *res = vfs_root_vnode;
    return 0;
  }

  return ENOENT;
}

static vnodeops_t vfs_root_ops = {.v_lookup = vfs_root_vnode_lookup};

static int vfs_register(vfsconf_t *vfc);

void init_vfs(void) {
  vnodeops_init(&vfs_root_ops);

  vfs_root_vnode = vnode_new(V_DIR, &vfs_root_ops, NULL);

  /* Initialize available filesystem types. */
  SET_DECLARE(vfsconf, vfsconf_t);
  vfsconf_t **ptr;
  SET_FOREACH (ptr, vfsconf)
    vfs_register(*ptr);
}

vfsconf_t *vfs_get_by_name(const char *name) {
  SCOPED_MTX_LOCK(&vfsconf_list_mtx);

  vfsconf_t *vfc;
  TAILQ_FOREACH (vfc, &vfsconf_list, vfc_list)
    if (!strcmp(name, vfc->vfc_name))
      return vfc;
  return NULL;
}

/* Register a file system type */
static int vfs_register(vfsconf_t *vfc) {
  /* Check if this file system type was already registered */
  if (vfs_get_by_name(vfc->vfc_name))
    return EEXIST;

  WITH_MTX_LOCK (&vfsconf_list_mtx)
    TAILQ_INSERT_TAIL(&vfsconf_list, vfc, vfc_list);

  vfc->vfc_mountcnt = 0;

  /* Ensure the filesystem provides obligatory operations */
  assert(vfc->vfc_vfsops != NULL);
  assert(vfc->vfc_vfsops->vfs_mount != NULL);

  /* Use defaults for other operations, if not provided. */
  if (vfc->vfc_vfsops->vfs_root == NULL)
    vfc->vfc_vfsops->vfs_root = vfs_default_root;
  if (vfc->vfc_vfsops->vfs_statvfs == NULL)
    vfc->vfc_vfsops->vfs_statvfs = vfs_default_statvfs;
  if (vfc->vfc_vfsops->vfs_vget == NULL)
    vfc->vfc_vfsops->vfs_vget = vfs_default_vget;
  if (vfc->vfc_vfsops->vfs_init == NULL)
    vfc->vfc_vfsops->vfs_init = vfs_default_init;

  /* Call init function for this vfs... */
  vfc->vfc_vfsops->vfs_init(vfc);

  return 0;
}

static int vfs_default_root(mount_t *m, vnode_t **v) {
  return ENOTSUP;
}

static int vfs_default_statvfs(mount_t *m, statvfs_t *sb) {
  return ENOTSUP;
}

static int vfs_default_vget(mount_t *m, ino_t ino, vnode_t **v) {
  return ENOTSUP;
}

static int vfs_default_init(vfsconf_t *vfc) {
  return 0;
}

mount_t *vfs_mount_alloc(vnode_t *v, vfsconf_t *vfc) {
  mount_t *m = kmalloc(M_VFS, sizeof(mount_t), M_ZERO);

  m->mnt_vfc = vfc;
  m->mnt_vfsops = vfc->vfc_vfsops;
  vfc->vfc_mountcnt++; /* TODO: vfc_mtx? */
  m->mnt_data = NULL;

  m->mnt_vnodecovered = v;

  m->mnt_refcnt = 0;
  mtx_init(&m->mnt_mtx, 0);

  return m;
}

int vfs_domount(vfsconf_t *vfc, vnode_t *vdst, vnode_t *vsrc) {
  int error;

  /* Start by checking whether this vnode can be used for mounting */
  if (vdst->v_type != V_DIR)
    return ENOTDIR;
  if (is_mountpoint(vdst))
    return EBUSY;

  /* TODO: Mark the vnode is in-progress of mounting? See VI_MOUNT in FreeBSD */

  mount_t *m = vfs_mount_alloc(vdst, vfc);

  /* Mount the filesystem. */
  if ((error = VFS_MOUNT(m, vsrc)))
    return error;

  vdst->v_mountedhere = m;

  WITH_MTX_LOCK (&mount_list_mtx)
    TAILQ_INSERT_TAIL(&mount_list, m, mnt_list);

  /* Note that we do not need to ask the new mount for the root vnode! That
     V_DIR vnode which is at the mount point stays in place. The root vnode is
     requested during path lookup process, once we encounter a directory that
     has v_mountedhere set. */

  /* TODO: 'mountcheckdirs', which checks each process in the system, and
     updates their dirs to reflect on the mounted file system... */

  return 0;
}

/* If `*vp` is a root of filesystem that has been mounted,
 * then find vnode of the mount point. */
void vfs_maybe_ascend(vnode_t **vp) {
  vnode_t *v_covered;
  vnode_t *v = *vp;
  while (vnode_is_mounted(v)) {
    v_covered = v->v_mount->mnt_vnodecovered;
    vnode_get(v_covered);
    vnode_put(v);
    v = v_covered;
  }

  *vp = v;
}

/* If `*vp` is a mountpoint, then descend into the root of mounted filesys. */
int vfs_maybe_descend(vnode_t **vp) {
  vnode_t *v_mntpt;
  vnode_t *v = *vp;
  while (is_mountpoint(v)) {
    int error = VFS_ROOT(v->v_mountedhere, &v_mntpt);
    vnode_put(v);
    if (error)
      return error;
    v = v_mntpt;
    /* No need to ref this vnode, VFS_ROOT already did it for us. */
    vnode_lock(v);
    *vp = v;
  }
  return 0;
}
