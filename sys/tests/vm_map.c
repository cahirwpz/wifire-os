#include <sys/klog.h>
#include <sys/mimiker.h>
#include <sys/libkern.h>
#include <sys/vm_pager.h>
#include <sys/vm_object.h>
#include <sys/vm_map.h>
#include <sys/vm_amap.h>
#include <sys/vm_anon.h>
#include <sys/errno.h>
#include <sys/thread.h>
#include <sys/ktest.h>
#include <sys/sched.h>
#include <machine/vm_param.h>

#ifdef __mips__
#define TOO_MUCH 0x40000000
#endif

#ifdef __aarch64__
#define TOO_MUCH 0x800000000000L
#endif

static int paging_on_demand_and_memory_protection_demo(void) {
  /* This test mustn't be preempted since PCPU's user-space vm_map will not be
   * restored while switching back. */
  SCOPED_NO_PREEMPTION();

  vm_map_t *orig = vm_map_user();
  vm_map_activate(vm_map_new());

  vm_map_t *kmap = vm_map_kernel();
  vm_map_t *umap = vm_map_user();

  klog("Kernel physical map : %08lx-%08lx", vm_map_start(kmap),
       vm_map_end(kmap));
  klog("User physical map   : %08lx-%08lx", vm_map_start(umap),
       vm_map_end(umap));

  vaddr_t pre_start = 0x1000000;
  vaddr_t start = 0x1001000;
  vaddr_t end = 0x1003000;
  vaddr_t post_end = 0x1004000;
  int n;

  /* preceding redzone segment */
  {
    vm_object_t *obj = vm_object_alloc(VM_DUMMY);
    vm_map_entry_t *ent =
      vm_map_entry_alloc(obj, pre_start, start, VM_PROT_NONE, VM_ENT_PRIVATE);
    n = vm_map_insert(umap, ent, VM_FIXED);
    assert(n == 0);
  }

  /* data segment */
  {
    vm_object_t *obj = vm_object_alloc(VM_ANONYMOUS);
    vm_map_entry_t *ent = vm_map_entry_alloc(
      obj, start, end, VM_PROT_READ | VM_PROT_WRITE, VM_ENT_PRIVATE);
    n = vm_map_insert(umap, ent, VM_FIXED);
    assert(n == 0);
  }

  /* succeeding redzone segment */
  {
    vm_object_t *obj = vm_object_alloc(VM_DUMMY);
    vm_map_entry_t *ent =
      vm_map_entry_alloc(obj, end, post_end, VM_PROT_NONE, VM_ENT_PRIVATE);
    n = vm_map_insert(umap, ent, VM_FIXED);
    assert(n == 0);
  }

  vm_map_dump(umap);
  vm_map_dump(kmap);

  /* Start in paged on demand range, but end outside, to cause fault */
  for (int *ptr = (int *)start; ptr != (int *)end; ptr += 256) {
    klog("%p", ptr);
    *ptr = 0xfeedbabe;
  }

  vm_map_dump(umap);
  vm_map_dump(kmap);

  vm_map_delete(umap);

  /* Restore original vm_map */
  vm_map_activate(orig);

  return KTEST_SUCCESS;
}

static int findspace_demo(void) {
  /* This test mustn't be preempted since PCPU's user-space vm_map will not be
   * restored while switching back. */
  SCOPED_NO_PREEMPTION();

  vm_map_t *orig = vm_map_user();

  vm_map_t *umap = vm_map_new();
  vm_map_activate(umap);

  const vaddr_t addr0 = 0x00400000;
  const vaddr_t addr1 = 0x10000000;
  const vaddr_t addr2 = 0x30000000;
  const vaddr_t addr3 = 0x30005000;
  const vaddr_t addr4 = 0x60000000;

  vm_map_entry_t *ent;
  vaddr_t t;
  int n;

  ent = vm_map_entry_alloc(NULL, addr1, addr2, VM_PROT_NONE, VM_ENT_PRIVATE);
  n = vm_map_insert(umap, ent, VM_FIXED);
  assert(n == 0);

  ent = vm_map_entry_alloc(NULL, addr3, addr4, VM_PROT_NONE, VM_ENT_PRIVATE);
  n = vm_map_insert(umap, ent, VM_FIXED);
  assert(n == 0);

  t = addr0;
  n = vm_map_findspace(umap, &t, PAGESIZE);
  assert(n == 0 && t == addr0);

  t = addr1;
  n = vm_map_findspace(umap, &t, PAGESIZE);
  assert(n == 0 && t == addr2);

  t = addr1 + 20 * PAGESIZE;
  n = vm_map_findspace(umap, &t, PAGESIZE);
  assert(n == 0 && t == addr2);

  t = addr1;
  n = vm_map_findspace(umap, &t, 0x6000);
  assert(n == 0 && t == addr4);

  t = addr1;
  n = vm_map_findspace(umap, &t, 0x5000);
  assert(n == 0 && t == addr2);

  /* Fill the gap exactly */
  ent = vm_map_entry_alloc(NULL, addr2, addr2 + 0x5000, VM_PROT_NONE,
                           VM_ENT_PRIVATE);
  n = vm_map_insert(umap, ent, VM_FIXED);
  assert(n == 0);

  t = addr1;
  n = vm_map_findspace(umap, &t, 0x5000);
  assert(n == 0 && t == addr4);

  t = addr4;
  n = vm_map_findspace(umap, &t, 0x6000);
  assert(n == 0 && t == addr4);

  t = 0;
  n = vm_map_findspace(umap, &t, TOO_MUCH);
  assert(n == ENOMEM);

  vm_map_delete(umap);

  /* Restore original vm_map */
  vm_map_activate(orig);

  return KTEST_SUCCESS;
}

#define START_ADDR (1 * PAGESIZE + USER_SPACE_BEGIN)
#define END_ADDR (20 * PAGESIZE + USER_SPACE_BEGIN)

/* I wish we can make test without definition, but we need it to specify amap
 * for entry. */
typedef struct vm_map_entry {
  TAILQ_ENTRY(vm_map_entry) link;
  vm_object_t *object;
  vm_aref_t aref;
  vm_prot_t prot;
  vm_entry_flags_t flags;
  vaddr_t start;
  vaddr_t end;
} vm_map_entry_t;

static int test_entry_amap(void) {
  vm_map_t *map = vm_map_new();

  vm_map_entry_t *e1 = vm_map_entry_alloc(
    NULL, START_ADDR, END_ADDR, VM_PROT_READ | VM_PROT_WRITE, VM_ENT_PRIVATE);
  vm_map_insert(map, e1, VM_FIXED);

  vm_amap_t *amap1 = vm_amap_alloc();
  e1->aref = (vm_aref_t){.ar_pageoff = 0, .ar_amap = amap1};

  vm_anon_t *an1 = vm_anon_alloc();
  vm_anon_t *an2 = vm_anon_alloc();
  vm_anon_t *an3 = vm_anon_alloc();
  vm_anon_t *an4 = vm_anon_alloc();

  an1->an_page = (vm_page_t *)1;
  an2->an_page = (vm_page_t *)2;
  an3->an_page = (vm_page_t *)3;
  an4->an_page = (vm_page_t *)4;

  vm_amap_add(&e1->aref, an1, 3 * PAGESIZE);
  vm_amap_add(&e1->aref, an2, 7 * PAGESIZE);
  vm_amap_add(&e1->aref, an3, 13 * PAGESIZE);
  vm_amap_add(&e1->aref, an4, 15 * PAGESIZE);

  vm_map_entry_t *e2;
  WITH_VM_MAP_LOCK (map)
    e2 = vm_map_entry_split(map, e1, START_ADDR + 10 * PAGESIZE);

  assert(e1->aref.ar_amap == e2->aref.ar_amap);
  klog("E1: %d E2: %d", e1->aref.ar_pageoff, e2->aref.ar_pageoff);
  assert(e1->aref.ar_pageoff == 0 && e2->aref.ar_pageoff == 10);
  assert(amap1->am_ref == 2);

  klog("E1: %ld - %ld | E2: %ld - %ld", e1->start, e1->end, e2->start, e2->end);
  assert(e1->start == START_ADDR && e1->end == START_ADDR + 10 * PAGESIZE);
  assert(e2->start == START_ADDR + 10 * PAGESIZE && e2->end == END_ADDR);

  WITH_VM_MAP_LOCK (map) {
    klog("found1: %p e1: %p", vm_map_find_entry(map, 7 * PAGESIZE + START_ADDR),
         e1);
    klog("found2: %p e2: %p",
         vm_map_find_entry(map, 15 * PAGESIZE + START_ADDR), e2);
    assert(vm_map_find_entry(map, 7 * PAGESIZE + START_ADDR) == e1);
    assert(vm_map_find_entry(map, 15 * PAGESIZE + START_ADDR) == e2);
  }

  /* To safe removial of this artificial vm_space */
  an1->an_page = (vm_page_t *)0;
  an2->an_page = (vm_page_t *)0;
  an3->an_page = (vm_page_t *)0;
  an4->an_page = (vm_page_t *)0;

  vm_map_delete(map);
  return KTEST_SUCCESS;
}

KTEST_ADD(vm, paging_on_demand_and_memory_protection_demo, 0);
KTEST_ADD(findspace, findspace_demo, 0);
KTEST_ADD(vm_entry_amap, test_entry_amap, 0);
