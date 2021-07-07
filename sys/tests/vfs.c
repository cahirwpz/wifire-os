#include <sys/mount.h>
#include <sys/libkern.h>
#include <sys/vfs.h>
#include <sys/vnode.h>
#include <sys/errno.h>
#include <sys/vm_map.h>
#include <sys/ktest.h>
#include <sys/proc.h>
#include <sys/cred.h>

static bool fsname_of(vnode_t *v, const char *fsname) {
  return strncmp(v->v_mount->mnt_vfc->vfc_name, fsname, strlen(fsname)) == 0;
}

static int test_vfs(void) {
  vnode_t *v;
  int error;
  cred_t *cred = cred_self();

  error = vfs_namelookup("/dev/SPAM", &v, cred);
  assert(error == ENOENT);
  error = vfs_namelookup("/", &v, cred);
  assert(error == 0);
  assert(fsname_of(v, "initrd"));
  vnode_drop(v);
  error = vfs_namelookup("/dev////", &v, cred);
  assert(error == 0);
  assert(fsname_of(v, "devfs"));
  vnode_drop(v);

  error = vfs_namelookup("/dev", &v, cred);
  assert(error == 0 && !is_mountpoint(v));
  vnode_drop(v);

  vnode_t *dev_null, *dev_zero;
  error = vfs_namelookup("/dev/null", &dev_null, cred);
  assert(error == 0);
  vnode_drop(dev_null);
  error = vfs_namelookup("/dev/zero", &dev_zero, cred);
  assert(error == 0);
  vnode_drop(dev_zero);

  assert(dev_zero->v_usecnt == 1);
  /* Ask for the same vnode multiple times and check for correct v_usecnt. */
  error = vfs_namelookup("/dev/zero", &dev_zero, cred);
  assert(error == 0);
  error = vfs_namelookup("/dev/zero", &dev_zero, cred);
  assert(error == 0);
  error = vfs_namelookup("/dev/zero", &dev_zero, cred);
  assert(error == 0);
  assert(dev_zero->v_usecnt == 4);
  vnode_drop(dev_zero);
  vnode_drop(dev_zero);
  vnode_drop(dev_zero);
  assert(dev_zero->v_usecnt == 1);

  return KTEST_SUCCESS;
}

KTEST_ADD(vfs, test_vfs, 0);
