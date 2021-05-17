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

#ifdef __mips__
#define TOO_MUCH 0x40000000
#endif

#ifdef __aarch64__
#define TOO_MUCH 0x800000000000L
#endif

#ifdef __amd64__
#define TOO_MUCH 0x0 /* TODO(MichalBlk): fix me. */
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

KTEST_ADD(vm, paging_on_demand_and_memory_protection_demo, 0);
KTEST_ADD(findspace, findspace_demo, 0);
