#define KL_LOG KL_PMAP
#include <sys/klog.h>
#include <sys/mimiker.h>
#include <sys/libkern.h>
#include <sys/pool.h>
#include <mips/mips.h>
#include <mips/tlb.h>
#include <mips/pmap.h>
#include <sys/pcpu.h>
#include <sys/pmap.h>
#include <sys/mutex.h>
#include <sys/sched.h>
#include <sys/vm_physmem.h>
#include <bitstring.h>
#include <errno.h>
#include <sys/kasan.h>

typedef struct pmap {
  mtx_t mtx;                      /* protects all fields in this structure */
  asid_t asid;                    /* address space identifier */
  pde_t *pde;                     /* directory page table (kseg0) */
  vm_pagelist_t pte_pages;        /* pages we allocate in page table */
  TAILQ_HEAD(, pv_entry) pv_list; /* all pages mapped by this physical map */
} pmap_t;

typedef struct pv_entry {
  TAILQ_ENTRY(pv_entry) pmap_link; /* link on pmap::pv_list */
  TAILQ_ENTRY(pv_entry) page_link; /* link on vm_page::pv_list */
  pmap_t *pmap;                    /* page is mapped in this pmap */
  vaddr_t va;                      /* under this address */
} pv_entry_t;

static POOL_DEFINE(P_PMAP, "pmap", sizeof(pmap_t));
static POOL_DEFINE(P_PV, "pv_entry", sizeof(pv_entry_t));

static const pte_t vm_prot_map[] = {
  [VM_PROT_NONE] = PTE_SW_NOEXEC,
  [VM_PROT_READ] = PTE_VALID | PTE_SW_READ | PTE_SW_NOEXEC,
  [VM_PROT_WRITE] = PTE_VALID | PTE_DIRTY | PTE_SW_WRITE | PTE_SW_NOEXEC,
  [VM_PROT_READ | VM_PROT_WRITE] =
    PTE_VALID | PTE_DIRTY | PTE_SW_READ | PTE_SW_WRITE | PTE_SW_NOEXEC,
  [VM_PROT_EXEC] = PTE_VALID,
  [VM_PROT_READ | VM_PROT_EXEC] = PTE_VALID | PTE_SW_READ,
  [VM_PROT_WRITE | VM_PROT_EXEC] = PTE_VALID | PTE_DIRTY | PTE_SW_WRITE,
  [VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC] =
    PTE_VALID | PTE_DIRTY | PTE_SW_READ | PTE_SW_WRITE,
};

static pmap_t kernel_pmap;
pde_t *_kernel_pmap_pde;
static bitstr_t asid_used[bitstr_size(MAX_ASID)] = {0};
static SPIN_DEFINE(asid_lock, 0);

/* this lock is used to protect the vm_page::pv_list field */
/* the order of acquiring locks is as follows: firstly pv_list_lock and then
 * pmap_t::mtx */
static MTX_DEFINE(pv_list_lock, 0);

#define PDE_OF(pmap, vaddr) ((pmap)->pde[PDE_INDEX(vaddr)])
#define PT_BASE(pde) ((pte_t *)(((pde) >> PTE_PFN_SHIFT) << PTE_INDEX_SHIFT))
#define PTE_OF(pde, vaddr) (PT_BASE(pde)[PTE_INDEX(vaddr)])

#define PTE_FRAME_ADDR(pte) (PTE_PFN_OF(pte) * PAGESIZE)
#define PAGE_OFFSET(x) ((x) & (PAGESIZE - 1))
#define PG_KSEG0_ADDR(pg) (void *)(MIPS_PHYS_TO_KSEG0((pg)->paddr))

/*
 * Helper functions.
 */

static inline bool is_valid_pde(pde_t pde) {
  return pde & PDE_VALID;
}

static inline pte_t empty_pte(pmap_t *pmap) {
  return (pmap == pmap_kernel()) ? PTE_GLOBAL : 0;
}

static bool user_addr_p(vaddr_t addr) {
  return (addr >= PMAP_USER_BEGIN) && (addr < PMAP_USER_END);
}

static bool kern_addr_p(vaddr_t addr) {
  return (addr >= PMAP_KERNEL_BEGIN) && (addr < PMAP_KERNEL_END);
}

inline vaddr_t pmap_start(pmap_t *pmap) {
  return pmap->asid ? PMAP_USER_BEGIN : PMAP_KERNEL_BEGIN;
}

inline vaddr_t pmap_end(pmap_t *pmap) {
  return pmap->asid ? PMAP_USER_END : PMAP_KERNEL_END;
}

inline bool pmap_address_p(pmap_t *pmap, vaddr_t va) {
  return pmap_start(pmap) <= va && va < pmap_end(pmap);
}

inline bool pmap_contains_p(pmap_t *pmap, vaddr_t start, vaddr_t end) {
  return pmap_start(pmap) <= start && end <= pmap_end(pmap);
}

inline pmap_t *pmap_kernel(void) {
  return &kernel_pmap;
}

inline pmap_t *pmap_user(void) {
  return PCPU_GET(curpmap);
}

pmap_t *pmap_lookup(vaddr_t addr) {
  if (kern_addr_p(addr))
    return pmap_kernel();
  if (user_addr_p(addr))
    return pmap_user();
  return NULL;
}

/*
 * Address space identifiers management.
 */

static asid_t alloc_asid(void) {
  int free = 0;
  WITH_SPIN_LOCK (&asid_lock) {
    bit_ffc(asid_used, MAX_ASID, &free);
    if (free < 0)
      panic("Out of asids!");
    bit_set(asid_used, free);
  }
  klog("alloc_asid() = %d", free);
  return free;
}

static void free_asid(asid_t asid) {
  klog("free_asid(%d)", asid);
  SCOPED_SPIN_LOCK(&asid_lock);
  bit_clear(asid_used, (unsigned)asid);
  tlb_invalidate_asid(asid);
}

/*
 * Physical-to-virtual entries are managed for all pageable mappings.
 */

static void pv_add(pmap_t *pmap, vaddr_t va, vm_page_t *pg) {
  assert(mtx_owned(&pv_list_lock));
  pv_entry_t *pv = pool_alloc(P_PV, M_ZERO);
  pv->pmap = pmap;
  pv->va = va;
  //klog("pv_add: pv %p va %p pg %p pmap %p (%d)", pv, va, pg, pmap, pmap->asid);
  //TAILQ_INSERT_TAIL(&pg->pv_list, pv, page_link);
  pv->page_link.tqe_next = TAILQ_END(head);
  pv->page_link.tqe_prev = (&pg->pv_list)->tqh_last;
  *(&pg->pv_list)->tqh_last = pv;
  (&pg->pv_list)->tqh_last = &(pv->page_link.tqe_next);
  // TAILQ_INSERT_TAIL(&pmap->pv_list, pv, pmap_link);
  pv->pmap_link.tqe_next = TAILQ_END(&pmap->pv_list);
  pv->pmap_link.tqe_prev = (&pmap->pv_list)->tqh_last;
  *(&pmap->pv_list)->tqh_last = pv;
  (&pmap->pv_list)->tqh_last = &(pv->pmap_link.tqe_next);
}

static pv_entry_t *pv_find(pmap_t *pmap, vaddr_t va, vm_page_t *pg) {
  assert(mtx_owned(&pv_list_lock));
  pv_entry_t *pv;
  TAILQ_FOREACH (pv, &pg->pv_list, page_link) {
    if (pv->pmap == pmap && pv->va == va)
      return pv;
  }
  return NULL;
}

static void pv_remove(pmap_t *pmap, vaddr_t va, vm_page_t *pg) {
  assert(mtx_owned(&pv_list_lock));
  pv_entry_t *pv = pv_find(pmap, va, pg);
  assert(pv != NULL);
  //klog("Removing pv_entry %p", pv);
  TAILQ_REMOVE(&pg->pv_list, pv, page_link);
  TAILQ_REMOVE(&pmap->pv_list, pv, pmap_link);
  pool_free(P_PV, pv);
}

/*
 * Routines for accessing page table entries.
 */

static vm_page_t *pmap_pagealloc(void) {
  vm_page_t *pg = vm_page_alloc(1);
  pmap_zero_page(pg);
  return pg;
}

/* Add PT to PD so kernel can handle access to @vaddr. */
static pde_t pmap_add_pde(pmap_t *pmap, vaddr_t vaddr) {
  assert(!is_valid_pde(PDE_OF(pmap, vaddr)));

  vm_page_t *pg = pmap_pagealloc();
  pde_t pde = PTE_PFN((vaddr_t)PG_KSEG0_ADDR(pg)) | PTE_KERNEL;
  PDE_OF(pmap, vaddr) = pde;

  pte_t *pte = &PTE_OF(pde, vaddr & PDE_INDEX_MASK);
  tlb_invalidate(PTE_VPN2((vaddr_t)pte) | PTE_ASID(pmap->asid));

  TAILQ_INSERT_TAIL(&pmap->pte_pages, pg, pageq);
  klog("Page table for %08lx allocated at %08lx", vaddr & PDE_INDEX_MASK, pte);

  /* Must initialize to PTE_GLOBAL, look at comment in update_wired_pde! */
  for (int i = 0; i < PT_ENTRIES; i++)
    pte[i] = PTE_GLOBAL;

  return pde;
}

/*! \brief Reads the PTE mapping virtual address \a vaddr. */
static pte_t pmap_pte_read(pmap_t *pmap, vaddr_t vaddr) {
  pde_t pde = PDE_OF(pmap, vaddr);
  if (!is_valid_pde(pde))
    return 0;
  return PTE_OF(pde, vaddr);
}

/*! \brief Writes \a pte as the new PTE mapping virtual address \a vaddr. */
static void pmap_pte_write(pmap_t *pmap, vaddr_t vaddr, pte_t pte,
                           unsigned flags) {
  unsigned cacheflags = flags & PMAP_CACHE_MASK;

  if (cacheflags == PMAP_NOCACHE)
    pte |= PTE_CACHE_UNCACHED;
  else if (cacheflags == PMAP_WRITE_THROUGH)
    pte |= PTE_CACHE_WRITE_THROUGH;
  else
    pte |= PTE_CACHE_WRITE_BACK;

  pde_t pde = PDE_OF(pmap, vaddr);
  if (!is_valid_pde(pde))
    pde = pmap_add_pde(pmap, vaddr);
  PTE_OF(pde, vaddr) = pte;
  tlb_invalidate(PTE_VPN2(vaddr) | PTE_ASID(pmap->asid));
}

/*
 * User physical map switching routines.
 */

static void update_wired_pde(pmap_t *umap) {
  pmap_t *kmap = pmap_kernel();

  /* Both ENTRYLO0 and ENTRYLO1 must have G bit set for address translation
   * to skip ASID check. */
  tlbentry_t e = {.hi = PTE_VPN2(UPD_BASE),
                  .lo0 = PTE_GLOBAL,
                  .lo1 = PTE_PFN(MIPS_KSEG0_TO_PHYS(kmap->pde)) | PTE_KERNEL};

  if (umap)
    e.lo0 = PTE_PFN(MIPS_KSEG0_TO_PHYS(umap->pde)) | PTE_KERNEL;

  tlb_write(0, &e);
}

void pmap_activate(pmap_t *umap) {
  SCOPED_NO_PREEMPTION();

  PCPU_SET(curpmap, umap);
  update_wired_pde(umap);

  /* Set ASID for current process */
  mips32_setentryhi(umap ? umap->asid : 0);
}

/*
 * Wired memory interface.
 */

void pmap_kenter(vaddr_t va, paddr_t pa, vm_prot_t prot, unsigned flags) {
  pmap_t *pmap = pmap_kernel();

  assert(page_aligned_p(pa) && page_aligned_p(va));
  assert(pmap_address_p(pmap, va));
  assert(pa != 0);

  klog("Enter unmanaged mapping from %p to %p", va, pa);

  pte_t pte = vm_prot_map[prot] | PTE_GLOBAL;

  WITH_MTX_LOCK (&pmap->mtx)
    pmap_pte_write(pmap, va, PTE_PFN(pa) | pte, flags);
}

void pmap_kremove(vaddr_t va, size_t size) {
  pmap_t *pmap = pmap_kernel();

  assert(page_aligned_p(va) && page_aligned_p(size));
  assert(pmap_contains_p(pmap, va, va + size));

  klog("%s: remove unmanaged mapping for %p - %p range", __func__, va,
       va + size - 1);

  WITH_MTX_LOCK (&pmap->mtx) {
    for (size_t off = 0; off < size; off += PAGESIZE)
      pmap_pte_write(pmap, va + off, PTE_GLOBAL, 0);
  }
}

bool pmap_kextract(vaddr_t va, paddr_t *pap) {
  return pmap_extract(pmap_kernel(), va, pap);
}

/*
 * Pageable (user & kernel) memory interface.
 */

static bool pmap_extract_nolock(pmap_t *pmap, vaddr_t va, paddr_t *pap) {
  if (!pmap_address_p(pmap, va))
    return false;

  pte_t pte = pmap_pte_read(pmap, va);
  paddr_t pa = PTE_FRAME_ADDR(pte);
  if (pa == 0)
    return false;

  *pap = pa | PAGE_OFFSET(va);
  return true;
}

void pmap_enter(pmap_t *pmap, vaddr_t va, vm_page_t *pg, vm_prot_t prot,
                unsigned flags) {
  paddr_t pa = pg->paddr;

  assert(page_aligned_p(va));
  assert(pmap_address_p(pmap, va));

  //klog("Enter virtual mapping %p for frame %p (pmap %p (%d) prot: %d)", va, pa, pmap, pmap->asid, prot);

  bool kern_mapping = (pmap == pmap_kernel());

  /* Mark user pages as non-referenced & non-modified. */
  pte_t mask =
    kern_mapping ? (PTE_VALID | PTE_DIRTY | PTE_SW_FLAGS) : PTE_SW_FLAGS;
  pte_t pte = (vm_prot_map[prot] & mask) | empty_pte(pmap);

  WITH_MTX_LOCK (&pv_list_lock) {
    WITH_MTX_LOCK (&pmap->mtx) {
      pv_entry_t *pv = pv_find(pmap, va, pg);
      if (pv == NULL)
        pv_add(pmap, va, pg);
      if (kern_mapping)
        pg->flags |= PG_MODIFIED | PG_REFERENCED;
      else
        pg->flags &= ~(PG_MODIFIED | PG_REFERENCED);
      pmap_pte_write(pmap, va, PTE_PFN(pa) | pte, flags);
    }
  }
}

void pmap_remove(pmap_t *pmap, vaddr_t start, vaddr_t end) {
  assert(page_aligned_p(start) && page_aligned_p(end) && start < end);
  assert(pmap_contains_p(pmap, start, end));

  //klog("Remove page mapping for address range %p-%p", start, end);

  WITH_MTX_LOCK (&pv_list_lock) {
    WITH_MTX_LOCK (&pmap->mtx) {
      for (vaddr_t va = start; va < end; va += PAGESIZE) {
        paddr_t pa;
        if (pmap_extract_nolock(pmap, va, &pa)) {
          vm_page_t *pg = vm_page_find(pa);
          pv_remove(pmap, va, pg);
          pmap_pte_write(pmap, va, empty_pte(pmap), 0);
        }
      }

      /* TODO: Deallocate empty page table fragment by calling pmap_remove_pde.
       */
    }
  }
}

void pmap_protect(pmap_t *pmap, vaddr_t start, vaddr_t end, vm_prot_t prot) {
  assert(page_aligned_p(start) && page_aligned_p(end) && start < end);
  assert(pmap_contains_p(pmap, start, end));

//  klog("Change protection bits to %x for address range %p-%p (pmap %p (%d))", prot, start,
//       end, pmap, pmap->asid);

  WITH_MTX_LOCK (&pmap->mtx) {
    for (vaddr_t va = start; va < end; va += PAGESIZE) {
      pte_t pte = pmap_pte_read(pmap, va);
      if (pte == 0)
        continue;
      pmap_pte_write(pmap, va, (pte & ~PTE_PROT_MASK) | vm_prot_map[prot], 0);
    }
  }
}

bool pmap_extract(pmap_t *pmap, vaddr_t va, paddr_t *pap) {
  SCOPED_MTX_LOCK(&pmap->mtx);
  return pmap_extract_nolock(pmap, va, pap);
}

void pmap_page_remove(vm_page_t *pg) {
  SCOPED_MTX_LOCK(&pv_list_lock);
  while (!TAILQ_EMPTY(&pg->pv_list)) {
    pv_entry_t *pv = TAILQ_FIRST(&pg->pv_list);
    pmap_t *pmap = pv->pmap;
    WITH_MTX_LOCK (&pmap->mtx) {
      vaddr_t va = pv->va;
      TAILQ_REMOVE(&pg->pv_list, pv, page_link);
      TAILQ_REMOVE(&pmap->pv_list, pv, pmap_link);
      pmap_pte_write(pmap, va, empty_pte(pmap), 0);
    }
    pool_free(P_PV, pv);
  }
}

void pmap_zero_page(vm_page_t *pg) {
  bzero(PG_KSEG0_ADDR(pg), PAGESIZE);
}

void pmap_copy_page(vm_page_t *src, vm_page_t *dst) {
  memcpy(PG_KSEG0_ADDR(dst), PG_KSEG0_ADDR(src), PAGESIZE);
}

static void pmap_modify_flags(vm_page_t *pg, pte_t set, pte_t clr) {
  SCOPED_MTX_LOCK(&pv_list_lock);
  pv_entry_t *pv;
  TAILQ_FOREACH (pv, &pg->pv_list, page_link) {
    pmap_t *pmap = pv->pmap;
    vaddr_t va = pv->va;
    WITH_MTX_LOCK (&pmap->mtx) {
      pde_t pde = PDE_OF(pmap, va);
      assert(is_valid_pde(pde));
      pte_t pte = PTE_OF(pde, va);
      pte |= set;
      pte &= ~clr;
      PTE_OF(pde, va) = pte;
      tlb_invalidate(PTE_VPN2(va) | PTE_ASID(pmap->asid));
    }
  }
}

bool pmap_clear_referenced(vm_page_t *pg) {
  bool prev = pmap_is_referenced(pg);
  pg->flags &= ~PG_REFERENCED;
  pmap_modify_flags(pg, 0, PTE_VALID);
  return prev;
}

bool pmap_clear_modified(vm_page_t *pg) {
  bool prev = pmap_is_modified(pg);
  pg->flags &= ~PG_MODIFIED;
  pmap_modify_flags(pg, 0, PTE_DIRTY);
  return prev;
}

bool pmap_is_referenced(vm_page_t *pg) {
  return pg->flags & PG_REFERENCED;
}

bool pmap_is_modified(vm_page_t *pg) {
  return pg->flags & PG_MODIFIED;
}

void pmap_set_referenced(vm_page_t *pg) {
  pg->flags |= PG_REFERENCED;
  pmap_modify_flags(pg, PTE_VALID, 0);
}

void pmap_set_modified(vm_page_t *pg) {
  pg->flags |= PG_MODIFIED;
  pmap_modify_flags(pg, PTE_DIRTY, 0);
}

int pmap_emulate_bits(pmap_t *pmap, vaddr_t va, vm_prot_t prot) {
  paddr_t pa;

  WITH_MTX_LOCK (&pmap->mtx) {
    if (!pmap_extract_nolock(pmap, va, &pa))
      return EFAULT;

    pte_t pte = pmap_pte_read(pmap, va);

    if ((prot & VM_PROT_READ) && !(pte & PTE_SW_READ))
      return EACCES;

    if ((prot & VM_PROT_WRITE) && !(pte & PTE_SW_WRITE))
      return EACCES;

    if ((prot & VM_PROT_EXEC) && (pte & PTE_SW_NOEXEC))
      return EACCES;
  }

  vm_page_t *pg = vm_page_find(pa);
  assert(pg != NULL);

  WITH_MTX_LOCK (&pv_list_lock) {
    /* Kernel non-pageable memory? */
    if (TAILQ_EMPTY(&pg->pv_list))
      return EINVAL;
  }

  pmap_set_referenced(pg);
  if (prot & VM_PROT_WRITE)
    pmap_set_modified(pg);

  return 0;
}

/*
 * Physical map management routines.
 */

static void pmap_setup(pmap_t *pmap) {
  pmap->asid = alloc_asid();
  mtx_init(&pmap->mtx, 0);
  TAILQ_INIT(&pmap->pte_pages);
  TAILQ_INIT(&pmap->pv_list);
}

void init_pmap(void) {
  pmap_setup(&kernel_pmap);
  kernel_pmap.pde = _kernel_pmap_pde;
}

pmap_t *pmap_new(void) {
  pmap_t *pmap = pool_alloc(P_PMAP, M_ZERO);
  pmap_setup(pmap);

  vm_page_t *pg = pmap_pagealloc();
  pmap->pde = PG_KSEG0_ADDR(pg);
  klog("Page directory table allocated at %p", pmap->pde);

  return pmap;
}

void pmap_delete(pmap_t *pmap) {
  assert(pmap != pmap_kernel());
  klog("pmap_delete: pmap %p (%d)", pmap, pmap->asid);
  while (!TAILQ_EMPTY(&pmap->pv_list)) {
    pv_entry_t *pv = TAILQ_FIRST(&pmap->pv_list);
    vm_page_t *pg;
    paddr_t pa;
    pmap_extract_nolock(pmap, pv->va, &pa);
    pg = vm_page_find(pa);
    klog("Removing pv %p for pg %p (pa %p, va %p)", pv, pg, pg->paddr, pv->va);
    WITH_MTX_LOCK (&pv_list_lock)
      TAILQ_REMOVE(&pg->pv_list, pv, page_link);
    TAILQ_REMOVE(&pmap->pv_list, pv, pmap_link);
    pool_free(P_PV, pv);
  }

  while (!TAILQ_EMPTY(&pmap->pte_pages)) {
    vm_page_t *pg = TAILQ_FIRST(&pmap->pte_pages);
    TAILQ_REMOVE(&pmap->pte_pages, pg, pageq);
    vm_page_free(pg);
  }

  vm_page_t *pg = vm_page_find(MIPS_KSEG0_TO_PHYS(pmap->pde));
  vm_page_free(pg);
  free_asid(pmap->asid);
  pool_free(P_PMAP, pmap);
}

#define L1_SPACE_SIZE (PAGESIZE * PAGESIZE / sizeof(pte_t))

/*
 * Increase usable kernel virtual address space to at least maxkvaddr.
 * Allocate page table (level 1) if needed.
 */
void pmap_growkernel(vaddr_t maxkvaddr) {
  assert(mtx_owned(&vm_kernel_end_lock));
  assert(maxkvaddr > vm_kernel_end);

  pmap_t *pmap = pmap_kernel();
  vaddr_t va;

  maxkvaddr = roundup2(maxkvaddr, L1_SPACE_SIZE);

  WITH_MTX_LOCK (&pmap->mtx) {
    for (va = vm_kernel_end; va < maxkvaddr; va += L1_SPACE_SIZE) {
      if (!is_valid_pde(PDE_OF(pmap, va)))
        pmap_add_pde(pmap, va);
    }
  }

  /*
   * kasan_grow calls pmap_kenter which acquires pmap->mtx.
   * But we are under vm_kernel_end_lock from kmem so it's safe to call
   * kasan_grow.
   */
  kasan_grow(maxkvaddr);

  vm_kernel_end = maxkvaddr;
}

int pmap_get_asid(pmap_t *pmap) {
  return (int)pmap->asid;
}
