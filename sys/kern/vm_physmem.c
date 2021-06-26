#define KL_LOG KL_PHYSMEM
#include <sys/klog.h>
#include <sys/mimiker.h>
#include <sys/libkern.h>
#include <sys/errno.h>
#include <sys/mutex.h>
#include <sys/pmap.h>
#include <sys/vm_physmem.h>
#include <sys/kasan.h>

#define FREELIST(page) (&freelist[log2((page)->size)])
#define PAGECOUNT(page) (pagecount[log2((page)->size)])

#define PG_SIZE(pg) ((pg)->size * PAGESIZE)
#define PG_START(pg) ((pg)->paddr)
#define PG_END(pg) ((pg)->paddr + PG_SIZE(pg))

#define PM_NQUEUES 16U

typedef struct vm_physseg {
  TAILQ_ENTRY(vm_physseg) seglink;
  paddr_t start;
  paddr_t end;
  size_t npages;
  bool used; /* all memory in this segment must be marked as used */
  vm_page_t *pages;
} vm_physseg_t;

static TAILQ_HEAD(, vm_physseg) seglist = TAILQ_HEAD_INITIALIZER(seglist);
static vm_pagelist_t freelist[PM_NQUEUES];
static size_t pagecount[PM_NQUEUES];
static MTX_DEFINE(physmem_lock, LK_RECURSIVE);

void _vm_physseg_plug(paddr_t start, paddr_t end, bool used) {
  assert(page_aligned_p(start) && page_aligned_p(end) && start < end);

  static vm_physseg_t freeseg[VM_PHYSSEG_NMAX];
  static unsigned freeseg_last = 0;

  SCOPED_MTX_LOCK(&physmem_lock);

  assert(freeseg_last < VM_PHYSSEG_NMAX - 1);

  vm_physseg_t *seg = &freeseg[freeseg_last++];

  seg->start = start;
  seg->end = end;
  seg->npages = (end - start) / PAGESIZE;
  seg->used = used;

  TAILQ_INSERT_TAIL(&seglist, seg, seglink);
}

static bool vm_boot_done = false;
atomic_vaddr_t vm_kernel_end = (vaddr_t)__kernel_end;
MTX_DEFINE(vm_kernel_end_lock, 0);

static void *vm_boot_alloc(size_t n) {
  assert(!vm_boot_done);
  assert(page_aligned_p(vm_kernel_end));

  n = roundup(n, PAGESIZE);

  void *begin = (void *)vm_kernel_end;
  void *end = begin + n;
#if KASAN
  /* We're not ready to call kasan_grow() yet, so this function could
   * potentially make vm_kernel_end go past _kernel_sanitized_end, which could
   * lead to KASAN referencing unmapped addresses in the shadow map, causing
   * a panic. Make sure that doesn't happen.
   * If this assertion fails, more shadow memory must be allocated when
   * initializing KASAN.*/
  assert((vaddr_t)end <= _kasan_sanitized_end);
#endif

  vm_physseg_t *seg = TAILQ_FIRST(&seglist);

  while (seg && (seg->used || seg->npages * PAGESIZE < n))
    seg = TAILQ_NEXT(seg, seglink);

  assert(seg != NULL);

  for (void *va = begin; va < end; va += PAGESIZE) {
    paddr_t pa = seg->start;
    seg->start += PAGESIZE;
    if (--seg->npages == 0) {
      assert(seg->start == seg->end);
      TAILQ_REMOVE(&seglist, seg, seglink);
      /* The segment should service the whole request. */
      seg = NULL;
    }

    pmap_kenter((vaddr_t)va, pa, VM_PROT_READ | VM_PROT_WRITE, 0);
  }

  vm_kernel_end = (vaddr_t)end;

  return begin;
}

static void vm_boot_finish(void) {
  vm_boot_done = true;
}

static inline size_t pg_compute_size(paddr_t pa) {
  /* Take the highest number of the form 2^k * PAGESIZE
   * that divides the physical address of a page. */
  return 1 << min(PM_NQUEUES - 1, ctz(pa / PAGESIZE));
}

void init_vm_page(void) {
  vm_physseg_t *seg;

  for (unsigned i = 0; i < PM_NQUEUES; i++)
    TAILQ_INIT(&freelist[i]);

  /* Allocate contiguous array of vm_page_t to cover all physical memory. */
  size_t npages = 0;
  TAILQ_FOREACH (seg, &seglist, seglink) {
    assert(seg->npages);
    npages += seg->npages;
  }

  vm_page_t *pages = vm_boot_alloc(npages * sizeof(vm_page_t));
  bzero(pages, npages * sizeof(vm_page_t));

  TAILQ_FOREACH (seg, &seglist, seglink) {
    /* Configure all pages in the segment. */
    for (unsigned i = 0; i < seg->npages; i++) {
      vm_page_t *page = &pages[i];
      paddr_t pa = seg->start + i * PAGESIZE;
      size_t size = pg_compute_size(pa);
      if (pa + size * PAGESIZE > seg->end) {
        /* Here:
         *
         *    pa    = 2^(k+1) * A + 2^k
         * seg->end = 2^(k+1) * A + 2^k + B
         *
         * Let's just restrict to B (this ensures that we won't get beyond the
         * segment while page->size * PAGESIZE still divides page->paddr). */
        size = pg_compute_size(seg->end ^ pa);
      }
      page->paddr = pa;
      page->size = size;
      page->flags = seg->used ? PG_ALLOCATED : 0;
      TAILQ_INIT(&page->pv_list);
    }

    /* Insert pages into free lists of corresponding size. */
    if (!seg->used) {
      for (unsigned i = 0; i < seg->npages;) {
        vm_page_t *page = &pages[i];
        TAILQ_INSERT_TAIL(FREELIST(page), page, freeq);
        PAGECOUNT(page)++;
        page->flags |= PG_MANAGED;
        i += page->size;
      }
    }

    seg->pages = pages;
    pages += seg->npages;
  }

  vm_boot_finish();
}

/* Takes two pages which are buddies, and merges them. */
static vm_page_t *pm_merge_buddies(vm_page_t *pg1, vm_page_t *pg2) {
  assert(pg1->size == pg2->size);

  if (pg1 > pg2)
    swap(pg1, pg2);

  assert(pg1 + pg1->size == pg2);

  pg1->size *= 2;
  return pg1;
}

static vm_page_t *pm_find_buddy(vm_physseg_t *seg, vm_page_t *pg) {
  vm_page_t *buddy = pg;

  assert(powerof2(pg->size));

  /* When page address is divisible by (2 * size) then:
   * look at right buddy, otherwise look at left buddy. */
  if (pg->paddr & (pg->size * PAGESIZE))
    buddy -= pg->size;
  else
    buddy += pg->size;

  intptr_t index = buddy - seg->pages;

  if (index < 0 || index >= (intptr_t)seg->npages)
    return NULL;

  if (buddy->size != pg->size)
    return NULL;

  if (!(buddy->flags & PG_MANAGED))
    return NULL;

  assert(!(buddy->flags & PG_ALLOCATED));

  return buddy;
}

static void pm_split_page(size_t fl) {
  vm_page_t *page = TAILQ_FIRST(&freelist[fl]);
  assert(page != NULL);

  /* It works, because every page is a member of pages! */
  size_t size = page->size / 2;
  vm_page_t *buddy = page + size;

  assert(size);
  assert(!(buddy->flags & (PG_ALLOCATED | PG_MANAGED)));

  TAILQ_REMOVE(&freelist[fl], page, freeq);
  pagecount[fl]--;

  page->size = size;
  buddy->size = size;
  fl--;

  TAILQ_INSERT_HEAD(&freelist[fl], page, freeq);
  TAILQ_INSERT_HEAD(&freelist[fl], buddy, freeq);
  pagecount[fl] += 2;
  buddy->flags |= PG_MANAGED;
}

static vm_page_t *pm_take_page(size_t fl) {
  vm_page_t *page = TAILQ_FIRST(&freelist[fl]);
  assert(page != NULL);
  klog("%s: allocated %lx of size %ld", __func__, page->paddr, page->size);
  TAILQ_REMOVE(&freelist[fl], page, freeq);
  pagecount[fl]--;
  page->flags &= ~PG_MANAGED;
  for (unsigned j = 0; j < page->size; j++)
    page[j].flags |= PG_ALLOCATED;
  return page;
}

vm_page_t *vm_page_alloc(size_t npages) {
  assert((npages > 0) && powerof2(npages));

  SCOPED_MTX_LOCK(&physmem_lock);

  size_t n = log2(npages);
  size_t fl = n;

  /* Lowest non-empty queue of size higher or equal to log2(npages). */
  while (fl < PM_NQUEUES && TAILQ_EMPTY(&freelist[fl]))
    fl++;

  if (fl >= PM_NQUEUES)
    return NULL;

  for (; fl > n; fl--)
    pm_split_page(fl);

  return pm_take_page(fl);
}

int vm_pagelist_alloc(size_t n, vm_pagelist_t *pglist) {
  TAILQ_INIT(pglist);

  SCOPED_MTX_LOCK(&physmem_lock);

  /* Check if the request can be satisfied at all. */
  size_t sums[PM_NQUEUES + 1];
  size_t sum = 0;
  int fl;
  for (fl = 0; fl < (int)PM_NQUEUES; fl++) {
    sum += pagecount[fl] << fl;
    sums[fl] = sum;
    if (sum >= n)
      break;
  }

  if (sum < n)
    return ENOMEM;

  /* `fl` is the highest free list number we need to visit to collect enough
   * pages to satisfy the request. We scan the lists in descending order and
   * greedily take what we can. We may be forced to split pages since greedy
   * choice may not be optimal. */
  while (n > 0) {
    size_t prev_sum = fl > 0 ? sums[fl - 1] : 0;

    /* Can remaining part of the request be satisfied with smaller pages? */
    if (prev_sum >= n) {
      fl--;
      continue;
    }

    size_t pgsz = 1 << fl;

    /* Is page too large to satisfy remaining part of the request? */
    if (n < pgsz) {
      pm_split_page(fl);
      sums[--fl] += pgsz;
      continue;
    }

    vm_page_t *pg = pm_take_page(fl);
    TAILQ_INSERT_TAIL(pglist, pg, pageq);
    n -= pgsz;
  }

  return 0;
}

static void pm_free_from_seg(vm_physseg_t *seg, vm_page_t *page) {
  if (!(page->flags & PG_ALLOCATED))
    panic("page is already free: %p", (void *)page->paddr);

  vm_page_t *buddy;
  while ((buddy = pm_find_buddy(seg, page))) {
    TAILQ_REMOVE(FREELIST(buddy), buddy, freeq);
    PAGECOUNT(buddy)--;
    buddy->flags &= ~PG_MANAGED;
    page = pm_merge_buddies(page, buddy);
  }

  TAILQ_INSERT_HEAD(FREELIST(page), page, freeq);
  PAGECOUNT(page)++;
  page->flags |= PG_MANAGED;
  for (unsigned i = 0; i < page->size; i++) {
    assert(TAILQ_EMPTY(&page[i].pv_list));
    page[i].flags &= ~PG_ALLOCATED;
    page[i].flags &= ~(PG_REFERENCED | PG_MODIFIED);
  }
}

static void vm_page_free_nolock(vm_page_t *pg) {
  klog("%s: free %lx of size %ld", __func__, pg->paddr, pg->size);

  vm_physseg_t *seg = NULL;
  TAILQ_FOREACH (seg, &seglist, seglink) {
    if (PG_START(pg) >= seg->start && PG_END(pg) <= seg->end) {
      pm_free_from_seg(seg, pg);
      return;
    }
  }

  panic("page out of range: %p", (void *)pg->paddr);
}

void vm_page_free(vm_page_t *page) {
  SCOPED_MTX_LOCK(&physmem_lock);
  vm_page_free_nolock(page);
}

void vm_pagelist_free(vm_pagelist_t *pglist) {
  SCOPED_MTX_LOCK(&physmem_lock);

  vm_page_t *pg, *pg_next;
  TAILQ_FOREACH_SAFE (pg, pglist, pageq, pg_next) {
    TAILQ_REMOVE(pglist, pg, pageq);
    vm_page_free_nolock(pg);
  }
}

vm_page_t *vm_page_find(paddr_t pa) {
  SCOPED_MTX_LOCK(&physmem_lock);

  vm_physseg_t *seg_it;
  TAILQ_FOREACH (seg_it, &seglist, seglink) {
    if (seg_it->start <= pa && pa < seg_it->end) {
      intptr_t index = (pa - seg_it->start) / PAGESIZE;
      return &seg_it->pages[index];
    }
  }

  return NULL;
}
