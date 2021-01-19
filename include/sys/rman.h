#ifndef _SYS_RMAN_H_
#define _SYS_RMAN_H_

#include <sys/cdefs.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <machine/bus_defs.h>

#define RMAN_ADDR_MAX UINTPTR_MAX
#define RMAN_SIZE_MAX UINTPTR_MAX

typedef uintptr_t rman_addr_t;
typedef struct rman rman_t;
typedef struct resource resource_t;
typedef struct range range_t;
typedef struct device device_t;
typedef TAILQ_HEAD(range_list, range) range_list_t;

typedef enum {
  RF_RESERVED = 1,
  RF_ACTIVE = 2,
  /* According to PCI specification prefetchable bit is CLEAR when memory mapped
   * range contains locations with read side-effects or locations in which the
   * device does not tolerate write merging. */
  RF_PREFETCHABLE = 4,
} rman_flags_t;

struct range {
  rman_t *rman;            /* resource manager of this resource */
  rman_addr_t start;       /* first physical address of the resource */
  rman_addr_t end;         /* last (inclusive) physical address */
  rman_flags_t flags;      /* or'ed RF_* values */
  TAILQ_ENTRY(range) link; /* link on resource manager list */
};

struct rman {
  mtx_t rm_lock;          /* protects all fields of range manager */
  const char *rm_name;    /* description of the range manager */
  range_list_t rm_ranges; /* ranges managed by this range manager */
};

/* !\brief Reserve an rman range within given rman.
 *
 * Looks up a region of size `count` between `start` and `end` address.
 * Assigned starting address will be aligned to `alignment` which must be
 * power of 2.
 *
 * \returns NULL if could not allocate a range
 */
range_t *rman_reserve_range(rman_t *rm, rman_addr_t start, rman_addr_t end,
                            size_t count, size_t alignment, rman_flags_t flags);

/*! \brief Removes a range from its range manager and releases memory. */
void rman_release_range(range_t *r);

/*! \brief Marks range as ready to be used with bus_space interface. */
void rman_activate_range(range_t *r);

/*! \brief Marks range as deactivated. */
void rman_deactivate_range(range_t *r);

/* !\brief Initializes range manager for further use. */
void rman_init(rman_t *rm, const char *name);

/* !\brief Initializes range manager based on supplied resource. */
void rman_init_from_resource(rman_t *rm, const char *name, resource_t *r);

/* !\brief Adds a new region to be managed by a range manager. */
void rman_manage_region(rman_t *rm, rman_addr_t start, size_t size);

/* !\brief Destroy range manager and free its memory resources. */
void rman_fini(rman_t *rm);

#endif /* !_SYS_RMAN_H_ */
