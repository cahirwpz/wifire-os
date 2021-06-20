#define KL_LOG KL_VM
#include <sys/mimiker.h>
#include <sys/klog.h>
#include <sys/pool.h>
#include <sys/vm.h>
#include <sys/vm_physmem.h>
#include <sys/vm_anon.h>
#include <sys/pmap.h>

static POOL_DEFINE(P_ANON, "vm_anon", sizeof(vm_anon_t));

vm_anon_t *vm_anon_alloc(void) {
  vm_anon_t *anon = pool_alloc(P_ANON, M_ZERO);
  anon->an_page = vm_page_alloc(1);
  anon->an_ref = 1;
  mtx_init(&anon->an_lock, 0);
  return anon;
}

void vm_anon_lock(vm_anon_t *anon) {
  mtx_lock(&anon->an_lock);
}

void vm_anon_unlock(vm_anon_t *anon) {
  mtx_unlock(&anon->an_lock);
}

void vm_anon_hold(vm_anon_t *anon) {
  assert(mtx_owned(&anon->an_lock));
  anon->an_ref++;
}

static void anon_free(vm_anon_t *anon) {
  klog("Feeing anon with page %p", anon->an_page);
  if (anon->an_page)
    vm_page_free(anon->an_page);
  pool_free(P_ANON, anon);
}

void vm_anon_drop(vm_anon_t *anon) {
  assert(mtx_owned(&anon->an_lock));
  anon->an_ref--;

  vm_anon_unlock(anon);
  if (anon->an_ref == 0)
    anon_free(anon);
}

vm_anon_t *vm_anon_copy(vm_anon_t *anon) {
  assert(mtx_owned(&anon->an_lock));

  vm_anon_t *new = vm_anon_alloc();
  pmap_copy_page(anon->an_page, new->an_page);
  return new;
}

vm_anon_t *vm_anon_copy_page(vm_page_t *page) {
  vm_anon_t *anon = vm_anon_alloc();
  pmap_copy_page(page, anon->an_page);
  return anon;
}
