#ifndef _SYS_POOL_H_
#define _SYS_POOL_H_

#include <sys/cdefs.h>
#include <sys/linker_set.h>
#include <sys/kmem_flags.h>

/*! \file pool.h
 *
 * Pooled allocator manages fixed-size object. Implementation is based on idea
 * of the slab allocator, however object caching facility is not available
 * (though the actual implementation is capable of supporting it).
 *
 * Pooled allocator idea is loosely based on NetBSD's pool(9).
 */

typedef struct pool pool_t;

/*! \brief Called during kernel initialization. */
void init_pool(void);

/* Pool input parameters. */
typedef struct pool_args {
  const char *desc;
  size_t size;
  size_t alignment;
} pool_args_t;

/*! \brief Creates a pool of objects of given size. */
pool_t *pool_create(pool_args_t *args);

/*! \brief Adds a slab of one page to the pool.
 *
 * The page may be bigger than the standard page. Argument @size has to be a
 * multiple of PAGESIZE.
 *
 * \note Use only during memory system bootstrap!
 */
void pool_add_page(pool_t *pool, void *page, size_t size);

/*! \brief Frees all memory associated with the pool
 *
 * \warning Do not call this function on pool with live objects! */
void pool_destroy(pool_t *pool);

/*! \brief Allocate an object from the pool.
 *
 * \note The pool may grow in page size units. */
void *pool_alloc(pool_t *pool, kmem_flags_t flags) __warn_unused;

/*! \brief Release an object that belongs to the pool. */
void pool_free(pool_t *pool, void *ptr);

/*! \brief Define a pool that will be initialized during system startup. */
#define POOL_DEFINE(NAME, ...)                                                 \
  struct pool *NAME;                                                           \
  static void __ctor_##NAME(void) {                                            \
    NAME = pool_create(&(pool_args_t){__VA_ARGS__});                           \
  }                                                                            \
  SET_ENTRY(pool_ctor_table, __ctor_##NAME);

#endif /* !_SYS_POOL_H_ */
