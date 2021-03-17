#define KL_LOG KL_PMAP
#include <sys/klog.h>
#include <sys/mimiker.h>
#include <sys/libkern.h>
#include <sys/pool.h>
#include <aarch64/armreg.h>
#include <aarch64/pte.h>
#include <aarch64/tlb.h>
#include <aarch64/pmap.h>
#include <sys/pcpu.h>
#include <sys/pmap.h>
#include <sys/mutex.h>
#include <sys/sched.h>
#include <sys/vm_physmem.h>
#include <bitstring.h>
#include <sys/errno.h>

typedef struct pmap {
  mtx_t mtx;                      /* protects all fields in this structure */
  asid_t asid;                    /* address space identifier */
  paddr_t pde;                    /* directory page table physical address */
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

#define PA_MASK 0xfffffffff000
#define ADDR_MASK 0x8ffffffff000
#define DMAP_BASE 0xffffff8000000000 /* last 512GB */
#define PHYS_TO_DMAP(x) ((intptr_t)(x) + DMAP_BASE)

/*
 * This table describes which access bits need to be set in page table entry
 * for successful memory translation by MMU. Other configurations causes memory
 * fault - see aarch64/trap.c.
 *
 * +--------------+----+------+----+----+
 * |    access    | AF | USER | RO | XN |
 * +==============+====+======+====+====+
 * | user read    | 1  | 1    | *  | *  |
 * +--------------+----+------+----+----+
 * | user write   | 1  | 1    | 0  | *  |
 * +--------------+----+------+----+----+
 * | user exec    | 1  | 1    | *  | 0  |
 * +--------------+----+------+----+----+
 * | kernel read  | 1  | *    | *  | *  |
 * +--------------+----+------+----+----+
 * | kernel write | 1  | *    | 0  | *  |
 * +--------------+----+------+----+----+
 * | kernel exec  | 1  | *    | *  | 0  |
 * +--------------+----+------+----+----+
 */

static const pte_t pte_common = L3_PAGE | ATTR_SH_IS;
static const pte_t pte_noexec = ATTR_XN | ATTR_SW_NOEXEC;

static const pte_t vm_prot_map[] = {
  [VM_PROT_NONE] = pte_noexec | pte_common,
  [VM_PROT_READ] =
    ATTR_AP_RO | ATTR_SW_READ | ATTR_AF | pte_noexec | pte_common,
  [VM_PROT_WRITE] =
    ATTR_AP_RW | ATTR_SW_WRITE | ATTR_AF | pte_noexec | pte_common,
  [VM_PROT_READ | VM_PROT_WRITE] = ATTR_AP_RW | ATTR_SW_READ | ATTR_SW_WRITE |
                                   ATTR_AF | pte_noexec | pte_common,
  [VM_PROT_EXEC] = ATTR_AF | pte_common,
  [VM_PROT_READ | VM_PROT_EXEC] =
    ATTR_AP_RO | ATTR_SW_READ | ATTR_AF | pte_common,
  [VM_PROT_WRITE | VM_PROT_EXEC] =
    ATTR_AP_RW | ATTR_SW_WRITE | ATTR_AF | pte_common,
  [VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC] =
    ATTR_AP_RW | ATTR_SW_READ | ATTR_SW_WRITE | ATTR_AF | pte_common,
};

static pmap_t kernel_pmap;
paddr_t _kernel_pmap_pde;
static bitstr_t asid_used[bitstr_size(MAX_ASID)] = {0};
static SPIN_DEFINE(asid_lock, 0);

/* this lock is used to protect the vm_page::pv_list field */
/* the order of acquiring locks is as follows: firstly pv_list_lock and then
 * pmap_t::mtx */
static MTX_DEFINE(pv_list_lock, 0);

#define PTE_FRAME_ADDR(pte) ((pte)&PA_MASK)
#define PAGE_OFFSET(x) ((x) & (PAGESIZE - 1))
#define PG_DMAP_ADDR(pg) ((void *)((intptr_t)(pg)->paddr + DMAP_BASE))

/*
 * Helper functions.
 */
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
  TAILQ_INSERT_TAIL(&pg->pv_list, pv, page_link);
  TAILQ_INSERT_TAIL(&pmap->pv_list, pv, pmap_link);
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

static pte_t *pmap_lookup_pte(pmap_t *pmap, vaddr_t va) {
  pde_t *pdep;
  paddr_t pa = pmap->pde;

  /* Level 0 */
  pdep = (pde_t *)PHYS_TO_DMAP(pa) + L0_INDEX(va);
  if (!(pa = PTE_FRAME_ADDR(*pdep)))
    return NULL;

  /* Level 1 */
  pdep = (pde_t *)PHYS_TO_DMAP(pa) + L1_INDEX(va);
  if (!(pa = PTE_FRAME_ADDR(*pdep)))
    return NULL;

  /* Level 2 */
  pdep = (pde_t *)PHYS_TO_DMAP(pa) + L2_INDEX(va);
  if (!(pa = PTE_FRAME_ADDR(*pdep)))
    return NULL;

  /* Level 3 */
  return (pde_t *)PHYS_TO_DMAP(pa) + L3_INDEX(va);
}

static paddr_t pmap_alloc_pde(pmap_t *pmap, vaddr_t vaddr) {
  vm_page_t *pg = pmap_pagealloc();

  TAILQ_INSERT_TAIL(&pmap->pte_pages, pg, pageq);

  klog("Page table for 0x%016lx allocated at 0x%016lx", vaddr, pg->paddr);

  return pg->paddr;
}

static pte_t make_pte(paddr_t pa, pte_t prot, unsigned flags) {
  pte_t pte = pa | prot;
  unsigned cacheflags = flags & PMAP_CACHE_MASK;
  if (cacheflags == PMAP_NOCACHE)
    return pte | ATTR_IDX(ATTR_NORMAL_MEM_NC);
  if (cacheflags == PMAP_WRITE_THROUGH)
    return pte | ATTR_IDX(ATTR_NORMAL_MEM_WT);
  return pte | ATTR_IDX(ATTR_NORMAL_MEM_WB);
}

static void pmap_write_pte(pmap_t *pmap, pte_t *ptep, pte_t pte, vaddr_t va) {
  if (pmap != pmap_kernel())
    pte |= ATTR_AP_USER;
  *ptep = pte;
  tlb_invalidate(va, pmap->asid);
}

/*
 * Return pointer to entry of va in level 3 of page table. Allocate space if
 * needed.
 */

static pte_t *pmap_ensure_pte(pmap_t *pmap, vaddr_t va) {
  pde_t *pdep;
  paddr_t pa = pmap->pde;

  /* Level 0 */
  pdep = (pde_t *)PHYS_TO_DMAP(pa) + L0_INDEX(va);
  if (!(pa = PTE_FRAME_ADDR(*pdep))) {
    pa = pmap_alloc_pde(pmap, va);
    *pdep = pa | L0_TABLE;
  }

  /* Level 1 */
  pdep = (pde_t *)PHYS_TO_DMAP(pa) + L1_INDEX(va);
  if (!(pa = PTE_FRAME_ADDR(*pdep))) {
    pa = pmap_alloc_pde(pmap, va);
    *pdep = pa | L1_TABLE;
  }

  /* Level 2 */
  pdep = (pde_t *)PHYS_TO_DMAP(pa) + L2_INDEX(va);
  if (!(pa = PTE_FRAME_ADDR(*pdep))) {
    pa = pmap_alloc_pde(pmap, va);
    *pdep = pa | L2_TABLE;
  }

  /* Level 3 */
  return (pde_t *)PHYS_TO_DMAP(pa) + L3_INDEX(va);
}

void pmap_activate(pmap_t *umap) {
  SCOPED_NO_PREEMPTION();

  PCPU_SET(curpmap, umap);

  uint64_t tcr = READ_SPECIALREG(TCR_EL1);

  if (umap == NULL) {
    WRITE_SPECIALREG(TCR_EL1, tcr | TCR_EPD0);
  } else {
    uint64_t ttbr0 = ((uint64_t)umap->asid << ASID_SHIFT) | umap->pde;
    WRITE_SPECIALREG(TTBR0_EL1, ttbr0);
    WRITE_SPECIALREG(TCR_EL1, tcr & ~TCR_EPD0);
  }
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

  pte_t pte = make_pte(pa, vm_prot_map[prot], flags);

  WITH_MTX_LOCK (&pmap->mtx) {
    pte_t *ptep = pmap_ensure_pte(pmap, va);
    pmap_write_pte(pmap, ptep, pte, va);
  }
}

void pmap_kremove(vaddr_t va, size_t size) {
  pmap_t *pmap = pmap_kernel();

  assert(page_aligned_p(va) && page_aligned_p(size));
  assert(pmap_contains_p(pmap, va, va + size));

  klog("%s: remove unmanaged mapping for %p - %p range", __func__, va,
       va + size - 1);

  WITH_MTX_LOCK (&pmap->mtx) {
    for (size_t off = 0; off < size; off += PAGESIZE) {
      pte_t *ptep = pmap_lookup_pte(pmap, va);
      assert(ptep != NULL);
      pmap_write_pte(pmap, ptep, 0, va);
    }
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

  pte_t *ptep = pmap_lookup_pte(pmap, va);
  if (ptep == NULL)
    return false;
  paddr_t pa = PTE_FRAME_ADDR(*ptep);
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

  klog("Enter virtual mapping %p for frame %p", va, pa);

  bool kern_mapping = (pmap == pmap_kernel());

  /* Mark user pages as non-referenced & non-modified. */
  pte_t mask = kern_mapping ? 0UL : (ATTR_AF);
  pte_t pte = make_pte(pa, vm_prot_map[prot] & ~mask, flags);

  WITH_MTX_LOCK (&pv_list_lock) {
    WITH_MTX_LOCK (&pmap->mtx) {
      pv_entry_t *pv = pv_find(pmap, va, pg);
      if (pv == NULL)
        pv_add(pmap, va, pg);
      if (kern_mapping)
        pg->flags |= PG_MODIFIED | PG_REFERENCED;
      else
        pg->flags &= ~(PG_MODIFIED | PG_REFERENCED);
      pte_t *ptep = pmap_ensure_pte(pmap, va);
      pmap_write_pte(pmap, ptep, pte, va);
    }
  }
}

void pmap_remove(pmap_t *pmap, vaddr_t start, vaddr_t end) {
  assert(page_aligned_p(start) && page_aligned_p(end) && start < end);
  assert(pmap_contains_p(pmap, start, end));

  klog("Remove page mapping for address range %p-%p", start, end);

  WITH_MTX_LOCK (&pv_list_lock) {
    WITH_MTX_LOCK (&pmap->mtx) {
      for (vaddr_t va = start; va < end; va += PAGESIZE) {
        pte_t *ptep = pmap_lookup_pte(pmap, va);
        if (ptep == NULL)
          continue;
        paddr_t pa = PTE_FRAME_ADDR(*ptep);
        if (pa == 0)
          continue;
        vm_page_t *pg = vm_page_find(pa);
        pv_remove(pmap, va, pg);
        pmap_write_pte(pmap, ptep, 0, va);
      }
    }
  }
}

void pmap_protect_nolock(pmap_t *pmap, vaddr_t start, vaddr_t end,
                         vm_prot_t prot) {
  assert(page_aligned_p(start) && page_aligned_p(end) && start < end);
  assert(pmap_contains_p(pmap, start, end));

  klog("Change protection bits to %x for address range %p-%p", prot, start,
       end);

  for (vaddr_t va = start; va < end; va += PAGESIZE) {
    pte_t *ptep = pmap_lookup_pte(pmap, va);
    if (ptep == NULL)
      continue;
    pte_t pte = vm_prot_map[prot] | (*ptep & (~ATTR_AP_MASK & ~ATTR_XN));
    pmap_write_pte(pmap, ptep, pte, va);
  }
}

void pmap_protect(pmap_t *pmap, vaddr_t start, vaddr_t end, vm_prot_t prot) {
  SCOPED_MTX_LOCK(&pmap->mtx);
  pmap_protect_nolock(pmap, start, end, prot);
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
    vaddr_t va = pv->va;
    WITH_MTX_LOCK (&pmap->mtx) {
      TAILQ_REMOVE(&pg->pv_list, pv, page_link);
      TAILQ_REMOVE(&pmap->pv_list, pv, pmap_link);
      pte_t *ptep = pmap_lookup_pte(pmap, va);
      assert(ptep != NULL);
      pmap_write_pte(pmap, ptep, 0, va);
    }
    pool_free(P_PV, pv);
  }
}

void pmap_zero_page(vm_page_t *pg) {
  bzero(PG_DMAP_ADDR(pg), PAGESIZE);
}

void pmap_copy_page(vm_page_t *src, vm_page_t *dst) {
  memcpy(PG_DMAP_ADDR(dst), PG_DMAP_ADDR(src), PAGESIZE);
}

static void pmap_modify_flags(vm_page_t *pg, pte_t set, pte_t clr) {
  SCOPED_MTX_LOCK(&pv_list_lock);
  pv_entry_t *pv;
  TAILQ_FOREACH (pv, &pg->pv_list, page_link) {
    pmap_t *pmap = pv->pmap;
    vaddr_t va = pv->va;
    WITH_MTX_LOCK (&pmap->mtx) {
      pte_t *ptep = pmap_lookup_pte(pmap, va);
      assert(ptep != NULL);
      pte_t pte = *ptep;
      pte |= set;
      pte &= ~clr;
      *ptep = pte;
      tlb_invalidate(va, pmap->asid);
    }
  }
}

bool pmap_clear_referenced(vm_page_t *pg) {
  bool prev = pmap_is_referenced(pg);
  pg->flags &= ~PG_REFERENCED;
  pmap_modify_flags(pg, 0, ATTR_AF);
  return prev;
}

bool pmap_clear_modified(vm_page_t *pg) {
  bool prev = pmap_is_modified(pg);
  pg->flags &= ~PG_MODIFIED;
  pmap_modify_flags(pg, ATTR_AP_RO, 0);
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
  pmap_modify_flags(pg, ATTR_AF, 0);
}

void pmap_set_modified(vm_page_t *pg) {
  pg->flags |= PG_MODIFIED;
  pmap_modify_flags(pg, 0, ATTR_AP_RO);
}

int pmap_emulate_bits(pmap_t *pmap, vaddr_t va, vm_prot_t prot) {
  paddr_t pa;

  WITH_MTX_LOCK (&pmap->mtx) {
    if (!pmap_extract_nolock(pmap, va, &pa))
      return EFAULT;

    pte_t pte = *pmap_lookup_pte(pmap, va);

    if ((prot & VM_PROT_READ) && !(pte & ATTR_SW_READ))
      return EACCES;

    if ((prot & VM_PROT_WRITE) && !(pte & ATTR_SW_WRITE))
      return EACCES;

    if ((prot & VM_PROT_EXEC) && (pte & ATTR_SW_NOEXEC))
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
  TAILQ_INSERT_TAIL(&pmap->pte_pages, pg, pageq);
  pmap->pde = pg->paddr;
  klog("Page directory table allocated at %p", pmap->pde);

  return pmap;
}

void pmap_delete(pmap_t *pmap) {
  assert(pmap != pmap_kernel());

  while (!TAILQ_EMPTY(&pmap->pv_list)) {
    pv_entry_t *pv = TAILQ_FIRST(&pmap->pv_list);
    vm_page_t *pg;
    paddr_t pa;
    pmap_extract_nolock(pmap, pv->va, &pa);
    pg = vm_page_find(pa);
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

  free_asid(pmap->asid);
  pool_free(P_PMAP, pmap);
}

void pmap_page_protect(vm_page_t *pg, vaddr_t start, vaddr_t end,
                       vm_prot_t prot) {
  SCOPED_MTX_LOCK(pv_list_lock);
  pv_entry_t *pv;
  TAILQ_FOREACH (pv, &pg->pv_list, page_link) {
    if (start <= pv->va && pv->va < end)
      pmap_protect_nolock(pv->pmap, max(start, pv->va),
                          min(pv->va + PAGESIZE, end), prot);
  }
}
