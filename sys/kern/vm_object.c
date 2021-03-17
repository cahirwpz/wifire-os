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
  TAILQ_INIT(&obj->list);
  mtx_init(&obj->mtx, 0);
  obj->pager = &pagers[type];
  obj->ref_counter = 1;
  return obj;
}

vm_page_t *vm_object_find_page(vm_object_t *obj, vm_offset_t offset) {
  SCOPED_MTX_LOCK(&obj->mtx);

  vm_page_t *pg;
  TAILQ_FOREACH (pg, &obj->list, obj.list) {
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

  WITH_MTX_LOCK (&obj->mtx) {
    vm_page_t *it;
    TAILQ_FOREACH (it, &obj->list, obj.list) {
      if (it->offset > pg->offset) {
        TAILQ_INSERT_BEFORE(it, pg, obj.list);
        obj->npages++;
        return;
      }
      /* there must be no page at the offset! */
      assert(it->offset != pg->offset);
    }

    /* offset of page is greater than the offset of any other page */
    TAILQ_INSERT_TAIL(&obj->list, pg, obj.list);
    obj->npages++;
  }
}

static void vm_object_remove_pages_nolock(vm_object_t *obj, vm_offset_t offset,
                                          size_t length) {
  assert(mtx_owned(&obj->mtx));
  assert(page_aligned_p(offset) && page_aligned_p(length));

  vm_page_t *pg, *next;
  TAILQ_FOREACH_SAFE (pg, &obj->list, obj.list, next) {
    if (pg->offset >= offset + length)
      break;
    if (pg->offset < offset)
      continue;

    pg->offset = 0;
    pg->object = NULL;
    TAILQ_REMOVE(&obj->list, pg, obj.list);
    vm_page_free(pg);
    obj->npages--;
  }
}

void vm_object_remove_pages(vm_object_t *obj, vm_offset_t off, size_t len) {
  SCOPED_MTX_LOCK(&obj->mtx);
  vm_object_remove_pages_nolock(obj, off, len);
}

#define vm_object_remove_all_pages(obj)                                        \
  vm_object_remove_pages((obj), 0, (size_t)(-PAGESIZE))

void vm_object_free(vm_object_t *obj) {
  if (!refcnt_release(&obj->ref_counter))
    return;

  vm_object_remove_all_pages(obj);
  pool_free(P_VMOBJ, obj);
}

vm_object_t *vm_object_clone(vm_object_t *obj) {
  vm_object_t *new_obj = vm_object_alloc(VM_DUMMY);
  new_obj->pager = obj->pager;

  WITH_MTX_LOCK (&obj->mtx) {
    vm_page_t *pg;
    TAILQ_FOREACH (pg, &obj->list, obj.list) {
      vm_page_t *new_pg = vm_page_alloc(1);
      pmap_copy_page(pg, new_pg);
      vm_object_add_page(new_obj, pg->offset, new_pg);
    }
  }

  return new_obj;
}

void vm_map_object_dump(vm_object_t *obj) {
  SCOPED_MTX_LOCK(&obj->mtx);

  vm_page_t *pg;
  TAILQ_FOREACH (pg, &obj->list, obj.list) {
    klog("(vm-obj) offset: 0x%08lx, size: %ld", pg->offset, pg->size);
  }
}

void vm_object_protect(vm_object_t *obj, vaddr_t start, vaddr_t end,
                       vm_prot_t prot) {
  assert(obj != NULL);

  SCOPED_MTX_LOCK(&obj->mtx);

  vm_page_t *pg;
  TAILQ_FOREACH (pg, &obj->list, obj.list) {
    pmap_page_protect(pg, start, end, prot);
  }
}