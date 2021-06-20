#include <sys/klog.h>
#include <sys/ktest.h>
#include <sys/vm_anon.h>

static int test_anon_simple(void) {

  vm_anon_t *anon = vm_anon_alloc();
  assert(anon != NULL);

  vm_anon_lock(anon);
  vm_anon_hold(anon);
  assert(anon->an_ref == 2);

  vm_anon_drop(anon);

  vm_anon_lock(anon);
  assert(anon->an_ref == 1);
  vm_anon_drop(anon);

  return KTEST_SUCCESS;
}

KTEST_ADD(vm_anon_simple, test_anon_simple, 0);
