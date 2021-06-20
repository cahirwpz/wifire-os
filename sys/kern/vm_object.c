#define KL_LOG KL_VM
#include <sys/klog.h>
#include <sys/mimiker.h>
#include <sys/pool.h>
#include <sys/pmap.h>
#include <sys/vm_object.h>
#include <sys/vm_physmem.h>

static POOL_DEFINE(P_VMOBJ, "vm_object", sizeof(vm_object_t));

vm_object_t *vm_object_alloc(vm_pgr_type_t type) {
  vm_object_t *obj = pool_alloc(P_VMOBJ, M_ZERO);
  TAILQ_INIT(&obj->vo_pages);
  mtx_init(&obj->vo_lock, 0);
  obj->vo_pager = &pagers[type];
  obj->vo_refs = 1;
  return obj;
}

vm_page_t *vm_object_find_page(vm_object_t *obj, vm_offset_t offset) {
  SCOPED_MTX_LOCK(&obj->vo_lock);

  vm_page_t *pg;
  TAILQ_FOREACH (pg, &obj->vo_pages, objpages) {
    if (pg->offset == offset)
      return pg;
  }

  return NULL;
}

void vm_object_add_page(vm_object_t *obj, vm_offset_t offset, vm_page_t *pg) {
  assert(page_aligned_p(pg->offset));
  /* For simplicity of implementation let's insert pages of size 1 only */
  assert(pg->size == 1);

  pg->object = obj;
  pg->offset = offset;

  WITH_MTX_LOCK (&obj->vo_lock) {
    vm_page_t *it;
    TAILQ_FOREACH (it, &obj->vo_pages, objpages) {
      if (it->offset > pg->offset) {
        TAILQ_INSERT_BEFORE(it, pg, objpages);
        obj->vo_npages++;
        return;
      }
      /* there must be no page at the offset! */
      assert(it->offset != pg->offset);
    }

    /* offset of page is greater than the offset of any other page */
    TAILQ_INSERT_TAIL(&obj->vo_pages, pg, objpages);
    obj->vo_npages++;
  }
}

static void vm_object_remove_pages_nolock(vm_object_t *obj, vm_offset_t offset,
                                          size_t length) {
  assert(mtx_owned(&obj->vo_lock));
  assert(page_aligned_p(offset) && page_aligned_p(length));

  vm_page_t *pg, *next;
  TAILQ_FOREACH_SAFE (pg, &obj->vo_pages, objpages, next) {
    if (pg->offset >= offset + length)
      break;
    if (pg->offset < offset)
      continue;

    pg->offset = 0;
    pg->object = NULL;
    TAILQ_REMOVE(&obj->vo_pages, pg, objpages);
    vm_page_free(pg);
    obj->vo_npages--;
  }
}

void vm_object_remove_pages(vm_object_t *obj, vm_offset_t off, size_t len) {
  SCOPED_MTX_LOCK(&obj->vo_lock);
  vm_object_remove_pages_nolock(obj, off, len);
}

#define vm_object_remove_all_pages(obj)                                        \
  vm_object_remove_pages_nolock((obj), 0, (size_t)(-PAGESIZE))

void vm_object_hold(vm_object_t *obj) {
  refcnt_acquire(&obj->vo_refs);
}

void vm_object_drop(vm_object_t *obj) {
  if (!refcnt_release(&obj->vo_refs))
    return;

  WITH_MTX_LOCK (&obj->vo_lock)
    vm_object_remove_all_pages(obj);

  pool_free(P_VMOBJ, obj);
}

vm_object_t *vm_object_clone(vm_object_t *obj) {
  /* XXX: this function will not be used in UVM */
  vm_object_t *new_obj = vm_object_alloc(VM_DUMMY);
  new_obj->vo_pager = obj->vo_pager;

  WITH_MTX_LOCK (&obj->vo_lock) {
    vm_page_t *pg;
    TAILQ_FOREACH (pg, &obj->vo_pages, objpages) {
      vm_page_t *new_pg = vm_page_alloc(1);
      pmap_copy_page(pg, new_pg);
      vm_object_add_page(new_obj, pg->offset, new_pg);
    }
  }

  return new_obj;
}

void vm_object_dump(vm_object_t *obj) {
  SCOPED_MTX_LOCK(&obj->vo_lock);

  vm_page_t *pg;
  TAILQ_FOREACH (pg, &obj->vo_pages, objpages) {
    klog("(vm-obj) offset: 0x%08lx, size: %ld", pg->offset, pg->size);
  }
}
