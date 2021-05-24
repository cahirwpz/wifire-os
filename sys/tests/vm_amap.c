#include <sys/klog.h>
#include <sys/mimiker.h>
#include <sys/libkern.h>
#include <sys/vm_amap.h>
#include <machine/vm_param.h>
#include <sys/ktest.h>

/* Test internal state of amap. */
static int test_amap_simple(void) {
  vm_amap_t *amap = vm_amap_alloc();
  assert(amap != NULL);

  vm_aref_t aref = {.ar_pageoff = 0, .ar_amap = amap};

  vm_amap_add(&aref, (vm_anon_t *)1, 3 * PAGESIZE);
  vm_amap_add(&aref, (vm_anon_t *)2, 7 * PAGESIZE);
  vm_amap_add(&aref, (vm_anon_t *)3, 2 * PAGESIZE);
  vm_amap_add(&aref, (vm_anon_t *)4, 14 * PAGESIZE);

  vm_amap_lock(amap);
  assert(amap->am_nslot == 16);
  assert(amap->am_nused == 4);

  assert(amap->am_anon[3] == (vm_anon_t *)1);
  assert(amap->am_anon[7] == (vm_anon_t *)2);
  assert(amap->am_anon[2] == (vm_anon_t *)3);
  assert(amap->am_anon[14] == (vm_anon_t *)4);

  assert(amap->am_bckptr[0] == 3);
  assert(amap->am_bckptr[1] == 7);
  assert(amap->am_bckptr[2] == 2);
  assert(amap->am_bckptr[3] == 14);

  assert(amap->am_slot[3] == 0);
  assert(amap->am_slot[7] == 1);
  assert(amap->am_slot[2] == 2);
  assert(amap->am_slot[14] == 3);
  vm_amap_unlock(amap);

  assert(vm_amap_lookup(&aref, 7 * PAGESIZE) == (vm_anon_t *)2);
  vm_amap_remove(&aref, 7 * PAGESIZE);
  assert(amap->am_nused == 3);
  assert(vm_amap_lookup(&aref, 7 * PAGESIZE) == NULL);

  vm_amap_lock(amap);
  assert(amap->am_slot[7] == -1);
  assert(amap->am_bckptr[1] == 14);
  assert(amap->am_bckptr[3] == -1);
  assert(amap->am_anon[7] == NULL);

  vm_amap_drop(amap);

  return KTEST_SUCCESS;
}

KTEST_ADD(vm_amap_simple, test_amap_simple, 0);
