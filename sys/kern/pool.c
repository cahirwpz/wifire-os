#define KL_LOG KL_KMEM
#include <sys/libkern.h>
#include <sys/queue.h>
#include <sys/mimiker.h>
#include <sys/klog.h>
#include <sys/mutex.h>
#include <sys/linker_set.h>
#include <sys/sched.h>
#include <sys/malloc.h>
#include <sys/pool.h>
#include <sys/kmem.h>
#include <sys/vm.h>
#include <machine/vm_param.h>
#include <bitstring.h>
#include <sys/kasan.h>

#define PI_ALIGNMENT sizeof(uint64_t)

#define POOL_DEBUG 0

#if defined(POOL_DEBUG) && POOL_DEBUG > 0
#define debug(...) klog(__VA_ARGS__)
#else
#define debug(...)
#endif

typedef LIST_HEAD(, slab) slab_list_t;

typedef struct pool {
  TAILQ_ENTRY(pool) pp_link;
  mtx_t pp_mtx;
  const char *pp_desc;
  slab_list_t pp_empty_slabs;
  slab_list_t pp_full_slabs;
  slab_list_t pp_part_slabs; /* partially allocated slabs */
  size_t pp_itemsize;        /* size of item */
  size_t pp_alignment;       /* alignment of allocated items */
#if KASAN
  size_t pp_redzone; /* size of redzone after each item */
  quar_t pp_quarantine;
#endif
  /* statistics */
  size_t pp_npages;   /* number of allocated pages (in bytes) */
  size_t pp_nused;    /* number of used items in all slabs */
  size_t pp_nmaxused; /* peak number of used items in all slabs */
  size_t pp_ntotal;   /* total number of items in all slabs */
} pool_t;

static TAILQ_HEAD(, pool) pool_list = TAILQ_HEAD_INITIALIZER(pool_list);
static MTX_DEFINE(pool_list_lock, 0);
static KMALLOC_DEFINE(M_POOL, "pool allocators");

typedef struct slab {
  LIST_ENTRY(slab) ph_link; /* pool slab list */
  uint16_t ph_nused;        /* # of items in use */
  uint16_t ph_ntotal;       /* total number of items */
  size_t ph_size;           /* size of memory allocated for the slab */
  size_t ph_itemsize;       /* total size of item (with header and redzone) */
  void *ph_items;           /* ptr to array of items after bitmap */
  bitstr_t ph_bitmap[0];
} slab_t;

static void *slab_item_at(slab_t *slab, unsigned i) {
  return slab->ph_items + i * slab->ph_itemsize;
}

static void add_slab(pool_t *pool, slab_t *slab, size_t slabsize) {
  assert(mtx_owned(&pool->pp_mtx));
  assert(is_aligned(slab, PAGESIZE));
  assert(is_aligned(slabsize, PAGESIZE));

  klog("add slab at %p to '%s' pool", slab, pool->pp_desc);

  slab->ph_size = slabsize;
  slab->ph_itemsize = pool->pp_itemsize;
#if KASAN
  slab->ph_itemsize += pool->pp_redzone;
#endif /* !KASAN */

  /*
   * Now we need to calculate maximum possible number of items of given `size`
   * in slab that occupies one page, taking into account space taken by:
   *  - items: ntotal * itemsize,
   *  - slab + bitmap: sizeof(slab_t) + bitstr_size(ntotal)
   * With:
   *  - usable = slabsize - sizeof(slab_t)
   * ... inequation looks as follow:
   * (1) ntotal * itemsize + (ntotal + 7) / 8 <= usable
   * (2) ntotal * 8 * itemsize + ntotal + 7 <= usable * 8
   * (3) ntotal * (8 * itemsize + 1) <= usable * 8 - 7
   * (4) ntotal <= (usable * 8 - 7) / (8 * itemsize + 1)
   */
  size_t usable = slabsize - sizeof(slab_t);
  slab->ph_ntotal = (usable * 8 - 7) / (8 * slab->ph_itemsize + 1);
  slab->ph_nused = 0;

  assert(slab->ph_ntotal > 0);

  size_t header = sizeof(slab_t) + bitstr_size(slab->ph_ntotal);
  slab->ph_items = (void *)slab + align(header, pool->pp_alignment);

  bzero(slab->ph_bitmap, bitstr_size(slab->ph_ntotal));

  LIST_INSERT_HEAD(&pool->pp_empty_slabs, slab, ph_link);

  pool->pp_ntotal += slab->ph_ntotal;
  pool->pp_npages += slabsize;

  for (size_t i = 0; i < slabsize; i += PAGESIZE) {
    vm_page_t *pg = kva_find_page((vaddr_t)slab + i);
    assert(pg != NULL);
    pg->slab = slab;
  }
}

void *pool_alloc(pool_t *pool, kmem_flags_t flags) {
  void *ptr;

  debug("pool_alloc: pool=%p", pool);

  WITH_MTX_LOCK (&pool->pp_mtx) {
    slab_t *slab;

    if (!(slab = LIST_FIRST(&pool->pp_part_slabs))) {
      if (!(slab = LIST_FIRST(&pool->pp_empty_slabs))) {
        slab = kmem_alloc(PAGESIZE, flags);
        assert(slab != NULL);
        add_slab(pool, slab, PAGESIZE);
      }
      /* We're going to allocate from empty slab
       * -> move it to the list of non-empty slabs. */
      assert(slab->ph_nused == 0);
      LIST_REMOVE(slab, ph_link);
      LIST_INSERT_HEAD(&pool->pp_part_slabs, slab, ph_link);
    }

    assert(slab->ph_nused < slab->ph_ntotal);
    int i = 0;
    bit_ffc(slab->ph_bitmap, slab->ph_ntotal, &i);
    bit_set(slab->ph_bitmap, i);
    ptr = slab_item_at(slab, i);
    debug("slab_alloc: allocated item %p at slab %p, index %d", ptr, slab, i);

    if (++slab->ph_nused == slab->ph_ntotal) {
      /* We've allocated last item from non-empty slab
       * -> move it to the list of full slabs. */
      LIST_REMOVE(slab, ph_link);
      LIST_INSERT_HEAD(&pool->pp_full_slabs, slab, ph_link);
    }

    pool->pp_nused++;
    pool->pp_nmaxused = max(pool->pp_nmaxused, pool->pp_nused);
  }

  /* Create redzone after the item. */
  kasan_mark(ptr, pool->pp_itemsize, pool->pp_itemsize + pool->pp_redzone,
             KASAN_CODE_POOL_OVERFLOW);
  if (flags & M_ZERO)
    bzero(ptr, pool->pp_itemsize);

  return ptr;
}

/* TODO: destroy empty slabs when their number reaches a certain threshold
 * (maybe leave one). */
static void _pool_free(pool_t *pool, void *ptr) {
  assert(mtx_owned(&pool->pp_mtx));

  debug("pool_free: pool = %p, ptr = %p", pool, ptr);

  vm_page_t *pg = kva_find_page((vaddr_t)ptr);
  assert(pg != NULL);
  slab_t *slab = pg->slab;

  unsigned index =
    ((intptr_t)ptr - (intptr_t)slab->ph_items) / slab->ph_itemsize;
  bitstr_t *bitmap = slab->ph_bitmap;

  if (!bit_test(bitmap, index))
    panic("Double free detected in '%s' pool at %p!", pool->pp_desc, ptr);
  bit_clear(bitmap, index);

  if (slab->ph_nused == slab->ph_ntotal) {
    LIST_REMOVE(slab, ph_link);
    LIST_INSERT_HEAD(&pool->pp_part_slabs, slab, ph_link);
  }

  if (--slab->ph_nused == 0) {
    LIST_REMOVE(slab, ph_link);
    LIST_INSERT_HEAD(&pool->pp_empty_slabs, slab, ph_link);
  }

  pool->pp_nused--;

  debug("pool_free: freed item %p at slab %p, index %d", ptr, slab, index);
}

void pool_free(pool_t *pool, void *ptr) {
  SCOPED_MTX_LOCK(&pool->pp_mtx);

  kasan_mark_invalid(ptr, pool->pp_itemsize + pool->pp_redzone,
                     KASAN_CODE_POOL_FREED);
  kasan_quar_additem(&pool->pp_quarantine, pool, ptr);
#if !KASAN
  /* Without KASAN, call regular free method */
  _pool_free(pool, ptr);
#endif /* !KASAN */
}

static void pool_ctor(pool_t *pool) {
  LIST_INIT(&pool->pp_empty_slabs);
  LIST_INIT(&pool->pp_full_slabs);
  LIST_INIT(&pool->pp_part_slabs);
  mtx_init(&pool->pp_mtx, 0);
}

static void destroy_slabs(pool_t *pool, slab_list_t *slabs) {
  slab_t *slab, *next;

  LIST_FOREACH_SAFE (slab, slabs, ph_link, next) {
    klog("destroy_slab: pool = %p, slab = %p", pool, slab);

    pool->pp_ntotal -= slab->ph_ntotal;
    pool->pp_npages -= slab->ph_size;

    LIST_REMOVE(slab, ph_link);

    for (size_t i = 0; i < slab->ph_size; i += PAGESIZE) {
      vm_page_t *pg = kva_find_page((vaddr_t)slab + i);
      assert(pg != NULL);
      assert(pg->slab == slab);
      pg->slab = NULL;
    }

    kmem_free(slab, slab->ph_size);
  }
}

static void pool_dtor(pool_t *pool) {
  destroy_slabs(pool, &pool->pp_empty_slabs);
  destroy_slabs(pool, &pool->pp_part_slabs);
  destroy_slabs(pool, &pool->pp_full_slabs);
  klog("destroyed pool '%s' at %p", pool->pp_desc, pool);
}

static void pool_init(pool_t *pool, pool_args_t *args) {
  const char *desc = args->desc;
  size_t size = args->size;
  size_t alignment = max(args->alignment, PI_ALIGNMENT);
  assert(desc);
  assert(size);
  assert(powerof2(alignment));

  pool_ctor(pool);
  pool->pp_desc = desc;
  pool->pp_alignment = alignment;
#if KASAN
  /* the alignment is within the redzone */
  pool->pp_itemsize = size;
  pool->pp_redzone = align(size + KASAN_POOL_REDZONE_SIZE, alignment) - size;
#else /* !KASAN */
  /* no redzone, we have to align the size itself */
  pool->pp_itemsize = align(size, alignment);
#endif
  kasan_quar_init(&pool->pp_quarantine, (quar_free_t)_pool_free);
  klog("initialized '%s' pool at %p (item size = %d)", pool->pp_desc, pool,
       pool->pp_itemsize);
  WITH_MTX_LOCK (&pool_list_lock)
    TAILQ_INSERT_TAIL(&pool_list, pool, pp_link);
}

void init_pool(void) {
  INVOKE_CTORS(pool_ctor_table);
}

void pool_add_page(pool_t *pool, void *page, size_t size) {
  SCOPED_MTX_LOCK(&pool->pp_mtx);
  add_slab(pool, page, size);
}

pool_t *pool_create(pool_args_t *args) {
  pool_t *pool = kmalloc(M_POOL, sizeof(pool_t), M_ZERO | M_NOWAIT);
  pool_init(pool, args);
  return pool;
}

void pool_destroy(pool_t *pool) {
  WITH_MTX_LOCK (&pool_list_lock)
    TAILQ_REMOVE(&pool_list, pool, pp_link);
  WITH_MTX_LOCK (&pool->pp_mtx)
    /* Lock needed as the quarantine may call _pool_free! */
    kasan_quar_releaseall(&pool->pp_quarantine);
  pool_dtor(pool);
  kfree(M_POOL, pool);
}
