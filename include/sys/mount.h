#ifndef _SYS_MOUNT_H_
#define _SYS_MOUNT_H_

#include <sys/cdefs.h>
#include <sys/syslimits.h>

/*
 * Flags for various system call interfaces.
 */
#define MNT_WAIT 1   /* synchronously wait for I/O to complete */
#define MNT_NOWAIT 2 /* start all I/O, but do not wait for it */

/*
 * Mount flags.
 */
#define MNT_RDONLY 0x00000001 /* read only filesystem */
#define MNT_IGNORE 0x00100000 /* don't show entry in df */

/* Maximum length of a filesystem type name */
#define MFSNAMELEN 32

#ifdef _KERNEL

#include <sys/queue.h>
#include <sys/mutex.h>
#include <sys/refcnt.h>

#define VFSCONF_NAME_MAX MFSNAMELEN

/* Forward declarations */
typedef struct mount mount_t;
typedef struct vnode vnode_t;
typedef struct vfsconf vfsconf_t;
typedef struct statvfs statvfs_t;

/* VFS operations */
typedef int vfs_mount_t(mount_t *m, vnode_t *vsrc);
typedef int vfs_root_t(mount_t *m, vnode_t **vp);
typedef int vfs_statvfs_t(mount_t *m, statvfs_t *sb);
typedef int vfs_vget_t(mount_t *m, ino_t ino, vnode_t **vp);
typedef int vfs_init_t(vfsconf_t *vfc);

typedef struct vfsops {
  vfs_mount_t *vfs_mount;
  vfs_root_t *vfs_root;
  vfs_statvfs_t *vfs_statvfs;
  vfs_vget_t *vfs_vget;
  vfs_init_t *vfs_init;
} vfsops_t;

/* Description of a filesystem type. There is one instance of this struct per
 * each filesystem type in the kernel (e.g. tmpfs, devfs). */
typedef struct vfsconf {
  char vfc_name[VFSCONF_NAME_MAX]; /* Filesystem type name */
  vfsops_t *vfc_vfsops;            /* Filesystem operations */
  int vfc_mountcnt; /* Number of mounted filesystems of this type */
  TAILQ_ENTRY(vfsconf) vfc_list; /* Entry on the list of vfsconfs */
} vfsconf_t;

/* The list of all installed filesystem types */
typedef TAILQ_HEAD(, vfsconf) vfsconf_list_t;
extern vfsconf_list_t vfsconf_list;
extern mtx_t vfsconf_list_mtx;

/* This structure represents a mount point: a particular instance of a file
   system mounted somewhere in the file tree. */
typedef struct mount {
  TAILQ_ENTRY(mount) mnt_list;    /* Entry on the mounts list */
  TAILQ_HEAD(, vnode) mnt_vnodes; /* List of vnodes on this mount point */

  vfsops_t *mnt_vfsops;      /* Filesystem operations */
  vfsconf_t *mnt_vfc;        /* Link to filesystem info */
  vnode_t *mnt_vnodecovered; /* The vnode covered by this mount */

  refcnt_t mnt_refcnt; /* Reference count */
  mtx_t mnt_mtx;

  void *mnt_data; /* Filesystem-specific arbitrary data */
} mount_t;

static inline int VFS_MOUNT(mount_t *m, vnode_t *vsrc) {
  return m->mnt_vfsops->vfs_mount(m, vsrc);
}

static inline int VFS_ROOT(mount_t *m, vnode_t **vp) {
  return m->mnt_vfsops->vfs_root(m, vp);
}

static inline int VFS_STATVFS(mount_t *m, statvfs_t *sb) {
  return m->mnt_vfsops->vfs_statvfs(m, sb);
}

static inline int VFS_VGET(mount_t *m, ino_t ino, vnode_t **vp) {
  return m->mnt_vfsops->vfs_vget(m, ino, vp);
}

/* This is the / node.*/
extern vnode_t *vfs_root_vnode;

/* Look up a file system type by name. */
vfsconf_t *vfs_get_by_name(const char *name);

/* Allocates and initializes a new mount struct, using filesystem vfc, covering
 * vnode v. Does not modify v. Does not insert new mount onto the all mounts
 * list. */
mount_t *vfs_mount_alloc(vnode_t *v, vfsconf_t *vfc);

/* Mount a new instance of the filesystem vfc at the vnode vdst. Does not
 * support remounting. Use vsrc as a source for fs data if not NULL.
 *TODO: Additional filesystem-specific arguments. */
int vfs_domount(vfsconf_t *vfc, vnode_t *vdst, vnode_t *vsrc);

#else /* !_KERNEL */
#include <sys/cdefs.h>
#include <sys/statvfs.h>

__BEGIN_DECLS
int unmount(const char *, int);
int mount(const char *, const char *, int, void *, size_t);
__END_DECLS

#endif /* _KERNEL */

#endif /* !_SYS_MOUNT_H_ */
