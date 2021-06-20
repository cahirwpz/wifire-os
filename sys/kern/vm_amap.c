#define KL_LOG KL_VM
#include <sys/klog.h>
#include <sys/mimiker.h>
#include <sys/libkern.h>
#include <sys/pool.h>
#include <sys/malloc.h>
#include <sys/vm_amap.h>
#include <sys/vm_anon.h>

#define AMAP_SLOT_EMPTY (-1)
#define AREF_EMPTY ((vm_aref_t){.ar_pageoff = 0, .ar_amap = 0})

static POOL_DEFINE(P_AMAP, "vm_amap", sizeof(vm_amap_t));

static long alloc_cnt, free_cnt;

static inline int vm_amap_slot(vm_aref_t *aref, vaddr_t offset) {
  assert(offset % PAGESIZE == 0);
  return (int)(aref->ar_pageoff + (offset >> PAGE_SHIFT));
}

vm_amap_t *vm_amap_alloc(void) {
  vm_amap_t *amap = pool_alloc(P_AMAP, M_ZERO);
  mtx_init(&amap->am_lock, 0);
  amap->am_ref = 1;
  klog("allocating %ld amap", ++alloc_cnt);
  return amap;
}

void vm_amap_lock(vm_amap_t *amap) {
  mtx_lock(&amap->am_lock);
}

void vm_amap_unlock(vm_amap_t *amap) {
  mtx_unlock(&amap->am_lock);
}

void vm_amap_hold(vm_amap_t *amap) {
  assert(mtx_owned(&amap->am_lock));
  amap->am_ref++;
}

static void vm_amap_free(vm_amap_t *amap) {
  for (int i = 0; i < amap->am_nused; ++i) {
    int slot = amap->am_slot[i];
    vm_anon_t *anon = amap->am_anon[slot];
    vm_anon_lock(anon);
    vm_anon_drop(anon);
  }

  kfree(M_TEMP, amap->am_slot);
  kfree(M_TEMP, amap->am_bckptr);
  kfree(M_TEMP, amap->am_anon);

  pool_free(P_AMAP, amap);
  klog("freeing %ld amap (%p)", ++free_cnt, amap);
}

void vm_amap_drop(vm_amap_t *amap) {
  assert(mtx_owned(&amap->am_lock));
  amap->am_ref--;

  vm_amap_unlock(amap);
  if (amap->am_ref == 0)
    vm_amap_free(amap);
}

static vm_anon_t *vm_amap_lookup_nolock(vm_amap_t *amap, int slot) {
  if (amap->am_nslot <= slot)
    return NULL;
  return amap->am_anon[slot];
}

vm_anon_t *vm_amap_lookup(vm_aref_t *aref, vaddr_t offset) {
  vm_amap_t *amap = aref->ar_amap;
  int slot = vm_amap_slot(aref, offset);

  SCOPED_MTX_LOCK(&amap->am_lock);
  return vm_amap_lookup_nolock(amap, slot);
}

static void __vm_amap_extend(vm_amap_t *amap, int size) {
  /* new entries in slot & bckptr will be set to AMAP_SLOT_EMPTY */
  int *slot = kmalloc(M_TEMP, sizeof(int) * size, 0);
  int *bckptr = kmalloc(M_TEMP, sizeof(int) * size, 0);
  /* new entries in anon will be set to NULL */
  vm_anon_t **anon = kmalloc(M_TEMP, sizeof(vm_anon_t *) * size, M_ZERO);

  /* old unused entires will be set to AMAP_SLOT_EMPTY */
  memcpy(slot, amap->am_slot, sizeof(int) * amap->am_nslot);
  memcpy(bckptr, amap->am_bckptr, sizeof(int) * amap->am_nslot);

  /* copy pointers only for allocated anons */
  for (int i = 0; i < amap->am_nused; ++i) {
    int slot = amap->am_slot[i];
    anon[slot] = amap->am_anon[slot];
  }

  /* set new entries to AMAP_SLOT_EMPTY */
  for (int i = amap->am_nslot; i < size; ++i) {
    slot[i] = AMAP_SLOT_EMPTY;
    bckptr[i] = AMAP_SLOT_EMPTY;
  }

  kfree(M_TEMP, amap->am_slot);
  kfree(M_TEMP, amap->am_bckptr);
  kfree(M_TEMP, amap->am_anon);

  amap->am_slot = slot;
  amap->am_bckptr = bckptr;
  amap->am_anon = anon;

  amap->am_nslot = size;
}

static void vm_amap_extend(vm_amap_t *amap, int size) {
  int capacity = max(amap->am_nslot, 1);
  while (capacity <= size)
    capacity *= 2;
  __vm_amap_extend(amap, capacity);
}

static void vm_amap_add_nolock(vm_amap_t *amap, vm_anon_t *anon, int slot) {
  if (slot >= amap->am_nslot)
    vm_amap_extend(amap, slot);

  assert(amap->am_anon[slot] == NULL);
  amap->am_anon[slot] = anon;
  amap->am_bckptr[slot] = amap->am_nused;
  amap->am_slot[amap->am_nused++] = slot;
}

void vm_amap_add(vm_aref_t *aref, vm_anon_t *anon, vaddr_t offset) {
  WITH_MTX_LOCK (&anon->an_lock)
    assert(anon->an_ref >= 1);
  vm_amap_t *amap = aref->ar_amap;
  int slot = vm_amap_slot(aref, offset);

  SCOPED_MTX_LOCK(&amap->am_lock);
  vm_amap_add_nolock(amap, anon, slot);
}

static void vm_amap_remove_nolock(vm_amap_t *amap, int slot) {
  assert(amap->am_nslot > slot);
  assert(amap->am_anon[slot] != NULL);

  vm_anon_t *anon = amap->am_anon[slot];
  vm_anon_lock(anon);
  vm_anon_drop(anon);
  amap->am_anon[slot] = NULL;

  int bslot = amap->am_bckptr[slot];
  amap->am_bckptr[slot] = AMAP_SLOT_EMPTY;
  amap->am_slot[bslot] = AMAP_SLOT_EMPTY;
  swap(amap->am_slot[bslot], amap->am_slot[amap->am_nused - 1]);
  amap->am_nused--;
}

void vm_amap_remove(vm_aref_t *aref, vaddr_t offset) {
  vm_amap_t *amap = aref->ar_amap;
  int slot = vm_amap_slot(aref, offset);

  SCOPED_MTX_LOCK(&amap->am_lock);
  vm_amap_remove_nolock(amap, slot);
}

vm_aref_t vm_amap_split(vm_aref_t *aref, vaddr_t offset) {
  vm_amap_t *amap = aref->ar_amap;

  vm_aref_t new_aref = {.ar_amap = amap,
                        .ar_pageoff = vm_amap_slot(aref, offset)};

  WITH_MTX_LOCK (&amap->am_lock)
    vm_amap_hold(amap);
  return new_aref;
}

vm_aref_t vm_amap_copy(vm_aref_t *aref, vaddr_t size) {
  vm_amap_t *amap = aref->ar_amap;

  if (amap == NULL) {
    /* Have to create new amap. */
    vm_amap_t *amap = vm_amap_alloc();
    return (vm_aref_t) {.ar_amap = amap, .ar_pageoff = 0};
  }

  WITH_MTX_LOCK(&amap->am_lock) {
    if (amap->am_ref == 1)
      return *aref;
  }

  int first_slot = aref->ar_pageoff;
  int last_slot = vm_amap_slot(aref, size);

  vm_amap_t *new = vm_amap_alloc();

  vm_amap_lock(amap);

  for (int i = 0; i < amap->am_nused; ++i) {
    int slot = amap->am_slot[i];
    if (first_slot <= slot && slot < last_slot) {
      vm_anon_t *anon = amap->am_anon[slot];
      WITH_MTX_LOCK (&anon->an_lock) {
        int ref = anon->an_ref;
        vm_anon_hold(anon);
        assert(anon->an_ref == ref + 1);
      }
      vm_amap_add_nolock(new, anon, slot - first_slot);
    }
  }

  /* Drop reference to amap and release lock */
  vm_amap_drop(amap);

  return (vm_aref_t) {.ar_amap = new, .ar_pageoff = 0};
}
