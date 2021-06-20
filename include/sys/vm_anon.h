#ifndef _SYS_UVM_ANON_H_
#define _SYS_UVM_ANON_H_

#ifndef _KERNEL
#error "Do not use this header file outside kernel code!"
#endif

#include <sys/mutex.h>
#include <sys/vm.h>

typedef struct vm_anon vm_anon_t;

/*
 * Field markings and the corresponding locks:
 *  (@) vm_anon:an_lock
 */

struct vm_anon {
  mtx_t an_lock;
  int an_ref;         /* (@) Reference counter */
  vm_page_t *an_page; /* (@) Pointer to holded page */
};

/* Allocate new anon. */
vm_anon_t *vm_anon_alloc(void);
/* Acquire anon->an_lock. */
void vm_anon_lock(vm_anon_t *anon);
/* Release anon->an_lock. */
void vm_anon_unlock(vm_anon_t *anon);

/* Increase anon's reference counter.
 *
 * Must be called with anon:an_lock held. */
void vm_anon_hold(vm_anon_t *anon);

/* Decrease anon's reference counter and destroy anon if it reached 0.
 *
 * Must be called with anon:an_lock held.
 * Releases anon:an_lock. */
void vm_anon_drop(vm_anon_t *anon);
/* Create new anon with copy of page from given one. */
vm_anon_t *vm_anon_copy(vm_anon_t *anon);
/* Create new anon with copy of given page. */
vm_anon_t *vm_anon_copy_page(vm_page_t *page);

#endif /* !_SYS_UVM_ANON_H_ */
