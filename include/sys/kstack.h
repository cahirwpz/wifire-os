#ifndef _SYS_KSTACK_H_
#define _SYS_KSTACK_H_

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/klog.h>
#include <sys/kmem.h>
#include <machine/abi.h>
#include <machine/vm_param.h>

/*! \brief Thread stack structure. */
typedef struct kstack {
  void *stk_base;  /*!< stack base */
  void *stk_ptr;   /*!< current stack pointer */
  size_t stk_size; /*!< stack length */
} kstack_t;

#define KSTACK_INIT(base, size)                                                \
  { .stk_base = (base), .stk_ptr = &(base)[(size)], .stk_size = (size) }

/*! \brief Reset stack so it's at the bottom */
static inline void kstack_reset(kstack_t *stk) {
  /* Stack grows down, so bottom has the highest address. */
  stk->stk_ptr = stk->stk_base + stk->stk_size;
}

/*! \brief Initialize stack structure. */
static inline void kstack_init(kstack_t *stk) {
  vaddr_t va = kva_alloc(2 * PAGESIZE);
  assert(va);
  vaddr_t base = va + PAGESIZE;
  /* Assume stack grows down. */
  kva_map(base, PAGESIZE, 0);
  /* Leave guard page unmapped. */
  stk->stk_base = (void *)base;
  stk->stk_size = PAGESIZE;
  kstack_reset(stk);
}

static inline void kstack_destroy(kstack_t *stk) {
  kva_unmap((vaddr_t)stk->stk_base, PAGESIZE);
  kva_free((vaddr_t)stk->stk_base - PAGESIZE, 2 * PAGESIZE);
}

/*! \brief Allocate n bytes on stack. */
static inline void *kstack_alloc(kstack_t *stk, size_t n) {
  stk->stk_ptr -= n;
  return stk->stk_ptr;
}

#define kstack_alloc_s(sp, type) kstack_alloc(sp, sizeof(type))

/*! \brief Align stack pointer. */
static inline void kstack_align(kstack_t *stk, size_t alignment) {
  stk->stk_ptr = (void *)((intptr_t)stk->stk_ptr & -(intptr_t)alignment);
}

/*! \brief Align stack pointer to STACK_ALIGN and make it a new bottom. */
static inline void kstack_fix_bottom(kstack_t *stk) {
  kstack_align(stk, STACK_ALIGN);
  stk->stk_size = stk->stk_ptr - stk->stk_base;
}

#endif /* !_SYS_KSTACK_H_ */
