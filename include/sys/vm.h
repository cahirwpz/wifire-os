#ifndef _SYS_VM_H_
#define _SYS_VM_H_

#include <sys/types.h>
#include <sys/newqueue.h>
#include <machine/vm_param.h>

#ifdef _KERNEL

typedef struct mtx mtx_t;

#define page_aligned_p(addr) is_aligned((addr), PAGESIZE)

/* Real kernel end in kernel virtual address space. */
extern atomic_vaddr_t vm_kernel_end;
extern mtx_t vm_kernel_end_lock;

typedef enum {
  PG_ALLOCATED = 0x01,  /* page has been allocated */
  PG_MANAGED = 0x02,    /* a page is on a freeq */
  PG_REFERENCED = 0x04, /* page has been accessed since last check */
  PG_MODIFIED = 0x08,   /* page has been modified since last check */
} __packed pg_flags_t;

typedef enum {
  VM_PROT_NONE = 0,
  VM_PROT_READ = 1,  /* can read page */
  VM_PROT_WRITE = 2, /* can write page */
  VM_PROT_EXEC = 4   /* can execute page */
} vm_prot_t;

#define VM_PROT_MASK (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC)

typedef enum {
  VM_FILE = 0x0000,    /* map from file (default) */
  VM_ANON = 0x1000,    /* allocated from memory */
  VM_STACK = 0x2000,   /* region grows down, like a stack */
  VM_SHARED = 0x0001,  /* share changes */
  VM_PRIVATE = 0x0002, /* changes are private */
  VM_FIXED = 0x0004,   /* map addr must be exactly as requested */
} vm_flags_t;

typedef struct vm_page vm_page_t;
typedef TAILQ_HEAD(vm_pagelist, vm_page) vm_pagelist_t;

typedef struct pv_entry pv_entry_t;
typedef struct vm_object vm_object_t;
typedef struct slab slab_t;
typedef uintptr_t vm_offset_t;

/* Field marking and corresponding locks:
 * (@) pv_list_lock (in pmap.c)
 * (P) physmem_lock (in vm_physmem.c)
 * (O) vm_object::vo_lock */

struct vm_page {
  union {
    TAILQ_ENTRY(vm_page) freeq;    /* (P) list of free pages for buddy system */
    TAILQ_ENTRY(vm_page) pageq;    /* used to group allocated pages */
    TAILQ_ENTRY(vm_page) objpages; /* (O) list of pages in vm_object */
    slab_t *slab; /* active when page is used by pool allocator */
  };
  TAILQ_HEAD(, pv_entry) pv_list; /* (@) where this page is mapped? */
  vm_object_t *object;            /* (O) object owning that page */
  vm_offset_t offset;             /* (O) offset to page in vm_object */
  paddr_t paddr;                  /* (P) physical address of page */
  pg_flags_t flags;               /* (P) page flags (used by physmem as well) */
  uint32_t size;                  /* (P) size of page in PAGESIZE units */
};

int do_mmap(vaddr_t *addr_p, size_t length, int u_prot, int u_flags);
int do_munmap(vaddr_t addr, size_t length);

#endif /* !_KERNEL */

#endif /* !_SYS_VM_H_ */
