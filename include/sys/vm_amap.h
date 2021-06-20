#ifndef _SYS_UVM_AMAP_H_
#define _SYS_UVM_AMAP_H_

#ifndef _KERNEL
#error "Do not use this header file outside kernel code!"
#endif

#include <sys/mutex.h>

typedef struct vm_anon vm_anon_t;
typedef struct vm_aref vm_aref_t;
typedef struct vm_amap vm_amap_t;

struct vm_aref {
  int ar_pageoff;     /* page offset into amap we start */
  vm_amap_t *ar_amap; /* pointer to amap */
};

/*
 * The upper layer of UVM’s two-layer mapping scheme.
 * A vm_amap describes an area of anonymous memory.
 *
 * Field markings and the corresponding locks:
 *  (@) vm_amap::am_lock
 */
struct vm_amap {
  mtx_t am_lock;
  int am_ref;     /* (@) reference counter */
  int am_nslot;   /* (@) number of allocated slots */
  int am_nused;   /* (@) number of used slots */
  int *am_slot;   /* (@) stack of used anons - refers to am_anon & am_bckptr */
  int *am_bckptr; /* (@) slots of used anons - refers to am_slot */
  vm_anon_t **am_anon; /* (@) anons in that map */
};

/*
 * amap interface
 */

/* Allocate a new amap. */
vm_amap_t *vm_amap_alloc(void);
/* Acquire amap->am_lock. */
void vm_amap_lock(vm_amap_t *amap);
/* Release amap->am_lock. */
void vm_amap_unlock(vm_amap_t *amap);

/* Increase reference counter.
 *
 * Must be called with amap:am_lock held. */
void vm_amap_hold(vm_amap_t *amap);

/* Decrement reference counter and destroy amap if it has reached 0.
 *
 * Must be called with amap:am_lock held.
 * Releases amap:am_lock. */
void vm_amap_drop(vm_amap_t *amap);
/* Lookup an anon at offset in amap. */
vm_anon_t *vm_amap_lookup(vm_aref_t *aref, vaddr_t offset);
/* Add an annon at offset in amap. */
void vm_amap_add(vm_aref_t *aref, vm_anon_t *anon, vaddr_t offset);
/* Remove an annon from an amap. */
void vm_amap_remove(vm_aref_t *aref, vaddr_t offset);

/* Returns aref to second part of amap. */
vm_aref_t vm_amap_split(vm_aref_t *aref, vaddr_t offset);

/* Copy part of amap descirbed with aref and ending at given addres.
 * If there is no amap to copy, returns newly created empty amap. */
vm_aref_t vm_amap_copy(vm_aref_t *aref, vaddr_t end);

/* TODO: ppref */

/* TODO: aref interface */

#endif /* !_SYS_UVM_AMAP_H_ */
