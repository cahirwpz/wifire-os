#define KL_LOG KL_VFS

#include <sys/mimiker.h>
#include <sys/klog.h>
#include <sys/vfs.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/malloc.h>
#include <sys/pool.h>
#include <sys/mutex.h>

static POOL_DEFINE(P_VCACHE, "vcache", sizeof(vnode_t));

#define VCACHE_BUCKET_CNT 64
#define VCACHE_INITIAL_FREE_CNT 128

typedef TAILQ_HEAD(vnode_list, vnode) vnode_list_t;

static vnode_list_t vcache_free;
static vnode_list_t vcache_buckets[VCACHE_BUCKET_CNT];
static mtx_t vcache_giant_lock;

typedef size_t vcache_t; 

static vcache_t vcache_hash(mount_t *mp, ino_t ino) {
  return (((vcache_t)mp->mnt_vnodecovered >> 3) + ino) % VCACHE_BUCKET_CNT;
}

/* XXX: this is taken from vfs_vnode.c */
static void vnlock_init(vnlock_t *vl) {
  spin_init(&vl->vl_interlock, 0);
  cv_init(&vl->vl_cv, "vnode sleep cv");
}

static vnode_t *vcache_new_vnode(void) {
  vnode_t *vn = pool_alloc(P_VCACHE, M_ZERO);
  vn->v_type = V_NONE;
  vnlock_init(&vn->v_lock);
  TAILQ_INSERT_TAIL(&vcache_free, vn, v_free);
  return vn;
}

void vfs_vcache_init(void) {
  TAILQ_INIT(&vcache_free);
  for (size_t i = 0; i < VCACHE_BUCKET_CNT; i++)
    TAILQ_INIT(&vcache_buckets[i]);

  for (size_t i = 0; i < VCACHE_INITIAL_FREE_CNT; i++)
   (void)vcache_new_vnode();

  mtx_init(&vcache_giant_lock, 0);
}

/**
 * \brief Get a free vnode from a hashlist.
 * \param mp mountpoint of the inode's filesystem
 * \param ino inode to be used for vnode lookup
 */
vnode_t *vfs_vcache_hashget(mount_t *mp, ino_t ino) {
  vcache_t bucket = vcache_hash(mp, ino);

  SCOPED_MTX_LOCK(&vcache_giant_lock);

  vnode_t *vn = NULL;
  TAILQ_FOREACH(vn, &vcache_buckets[bucket], v_cached) {
    if (vn->v_mount == mp && vn->ino == ino) {
      TAILQ_REMOVE(&vcache_free, vn, v_free);
      TAILQ_REMOVE(&vcache_buckets[bucket], vn, v_cached);
      vnode_hold(vn);
      return vn;
    }
  }

  return NULL;
}

/**
 * \brief Bind a free vnode to a given inode
 * \param vn a vnode from vcache free list
 */
void vfs_vcache_bind(vnode_t *vn) {
  vcache_t bucket = vcache_hash(vn->v_mount, vn->ino);
  
  SCOPED_MTX_LOCK(&vcache_giant_lock);

  TAILQ_INSERT_HEAD(&vcache_buckets[bucket], vn, v_cached);
}

/**
 * \brief Get a new, vcached vnode
 */
vnode_t *vfs_vcache_new_vnode(void) {
  SCOPED_MTX_LOCK(&vcache_giant_lock);

  /* Allocate new vnode if no free vnodes are available */
  if (TAILQ_EMPTY(&vcache_free))
    return vcache_new_vnode();

  /* Get the Least Recently Used vnode */
  vnode_t *vn = TAILQ_FIRST(&vcache_free);
  
  /* Make it the Most Recently Used vnode */
  TAILQ_REMOVE(&vcache_free, vn, v_free);
  TAILQ_INSERT_TAIL(&vcache_free, vn, v_free);

  /* Remove it from its bucket (if present in any) */
  if (!vn->v_mount)
    return vn; /* vnode was not in a any bucket and cannot be hashed */
  vcache_t bucket = vcache_hash(vn->v_mount, vn->ino);
  vnode_t *bucket_node = NULL;
  TAILQ_FOREACH(bucket_node, &vcache_buckets[bucket], v_cached) {
    if (bucket_node == vn) {
      TAILQ_REMOVE(&vcache_buckets[bucket], vn, v_cached);
      break;
    }
  }

  return vn;
}

/**
 * \brief Return a vnode to vcache's free list
 * \param vn 
 */
void vfs_vcache_put(vnode_t *vn) {
  SCOPED_MTX_LOCK(&vcache_giant_lock);

  TAILQ_INSERT_TAIL(&vcache_free, vn, v_free);
  vcache_t bucket = vcache_hash(vn->v_mount, vn->ino);
  TAILQ_INSERT_HEAD(&vcache_buckets[bucket], vn, v_cached);
}