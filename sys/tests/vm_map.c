#include <sys/klog.h>
#include <sys/mimiker.h>
#include <sys/libkern.h>
#include <sys/vm_pager.h>
#include <sys/vm_object.h>
#include <sys/vm_map.h>
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
    vm_segment_t *seg =
      vm_segment_alloc(obj, pre_start, start, VM_PROT_NONE, VM_SEG_PRIVATE);
    n = vm_map_insert(umap, seg, VM_FIXED);
    assert(n == 0);
  }

  /* data segment */
  {
    vm_object_t *obj = vm_object_alloc(VM_ANONYMOUS);
    vm_segment_t *seg = vm_segment_alloc(
      obj, start, end, VM_PROT_READ | VM_PROT_WRITE, VM_SEG_PRIVATE);
    n = vm_map_insert(umap, seg, VM_FIXED);
    assert(n == 0);
  }

  /* succeeding redzone segment */
  {
    vm_object_t *obj = vm_object_alloc(VM_DUMMY);
    vm_segment_t *seg =
      vm_segment_alloc(obj, end, post_end, VM_PROT_NONE, VM_SEG_PRIVATE);
    n = vm_map_insert(umap, seg, VM_FIXED);
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

static void vm_segment_alloc_and_insert(vm_map_t *map, vaddr_t start,
                                        vaddr_t end) {
  vm_segment_t *seg =
    vm_segment_alloc(NULL, start, end, VM_PROT_NONE, VM_SEG_PRIVATE);
  assert(vm_map_insert(map, seg, VM_FIXED) == 0);
}

static void vm_map_findspace_and_check(vm_map_t *map, vaddr_t start,
                                       vaddr_t len, vaddr_t expected) {
  assert(vm_map_findspace(map, &start, len) == 0 && start == expected);
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

  vaddr_t t;
  int n;

  vm_segment_alloc_and_insert(umap, addr1, addr2);
  vm_segment_alloc_and_insert(umap, addr3, addr4);

  vm_map_findspace_and_check(umap, addr0, PAGESIZE, addr0);
  vm_map_findspace_and_check(umap, addr1, PAGESIZE, addr2);
  vm_map_findspace_and_check(umap, addr1 + 20 * PAGESIZE, PAGESIZE, addr2);
  vm_map_findspace_and_check(umap, addr1, 0x6000, addr4);
  vm_map_findspace_and_check(umap, addr1, 0x5000, addr2);

  /* Fill the gap exactly */
  vm_segment_alloc_and_insert(umap, addr2, addr2 + 0x5000);

  vm_map_findspace_and_check(umap, addr1, 0x5000, addr4);
  vm_map_findspace_and_check(umap, addr4, 0x6000, addr4);

  t = 0;
  n = vm_map_findspace(umap, &t, TOO_MUCH);
  assert(n == ENOMEM);

  vm_map_delete(umap);

  /* Restore original vm_map */
  vm_map_activate(orig);

  return KTEST_SUCCESS;
}

static int vm_map_insert_fixed(void) {
  /* This test mustn't be preempted since PCPU's user-space vm_map will not be
   * restored while switching back. */
  SCOPED_NO_PREEMPTION();

  vm_map_t *orig = vm_map_user();

  vm_map_t *umap = vm_map_new();
  vm_map_activate(umap);

  const vaddr_t addr0 = 0x10000000;
  const vaddr_t addr1 = 0x10001000;
  const vaddr_t addr2 = 0x10005000;
  const vaddr_t addr3 = 0x10009000;

  vm_segment_alloc_and_insert(umap, addr0, addr1);
  vm_segment_alloc_and_insert(umap, addr2, addr3);

  vm_map_findspace_and_check(umap, addr1, 4 * PAGESIZE, addr1);
  vm_map_findspace_and_check(umap, addr1, 5 * PAGESIZE, addr3);

  /* Delete first and second mapping and replace them by new one */
  vm_segment_alloc_and_insert(umap, addr1, addr1 + 5 * PAGESIZE);

  vm_map_findspace_and_check(umap, addr1, 4 * PAGESIZE, addr1 + 5 * PAGESIZE);

  vm_map_delete(umap);

  /* Restore original vm_map */
  vm_map_activate(orig);
  return KTEST_SUCCESS;
}

static int vm_map_fixed_bad(void) {
  /* This test mustn't be preempted since PCPU's user-space vm_map will not be
   * restored while switching back. */
  SCOPED_NO_PREEMPTION();

  vm_map_t *orig = vm_map_user();

  vm_map_t *umap = vm_map_new();
  vm_map_activate(umap);

  const vaddr_t addr0 = USER_SPACE_BEGIN + PAGESIZE;
  const vaddr_t addr1 = USER_SPACE_BEGIN + 6 * PAGESIZE;
  const vaddr_t addr2 = USER_SPACE_BEGIN + 7 * PAGESIZE;
  const vaddr_t addr3 = USER_SPACE_BEGIN + 16 * PAGESIZE;

  vm_segment_t *seg;
  int n;

  vm_segment_alloc_and_insert(umap, addr0, addr1);
  vm_segment_alloc_and_insert(umap, addr2, addr3);

  vm_map_findspace_and_check(umap, addr1, PAGESIZE, addr1);
  vm_map_findspace_and_check(umap, addr1, 9 * PAGESIZE, addr3);

  seg = vm_segment_alloc(NULL, addr3, addr3 + USER_SPACE_END, VM_PROT_NONE,
                         VM_SEG_PRIVATE);
  n = vm_map_insert(umap, seg, VM_FIXED);
  assert(n == ENOMEM);

  vm_map_delete(umap);

  /* Restore original vm_map */
  vm_map_activate(orig);
  return KTEST_SUCCESS;
}

KTEST_ADD(vm, paging_on_demand_and_memory_protection_demo, 0);
KTEST_ADD(findspace, findspace_demo, 0);
KTEST_ADD(vm_map_insert_fixed, vm_map_insert_fixed, 0);
KTEST_ADD(vm_map_fixed_bad, vm_map_fixed_bad, 0);
