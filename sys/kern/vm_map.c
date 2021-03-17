#define KL_LOG KL_VM
#include <sys/klog.h>
#include <sys/mimiker.h>
#include <sys/libkern.h>
#include <sys/pool.h>
#include <sys/pmap.h>
#include <sys/vm_pager.h>
#include <sys/vm_object.h>
#include <sys/vm_map.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/pcpu.h>
#include <machine/vm_param.h>

struct vm_segment {
  TAILQ_ENTRY(vm_segment) link;
  vm_object_t *object;
  vm_prot_t prot;
  vm_seg_flags_t flags;
  vaddr_t start;
  vaddr_t end;
};

struct vm_map {
  TAILQ_HEAD(vm_map_list, vm_segment) entries;
  size_t nentries;
  pmap_t *pmap;
  mtx_t mtx; /* Mutex guarding vm_map structure and all its entries. */
};

static POOL_DEFINE(P_VMMAP, "vm_map", sizeof(vm_map_t));
static POOL_DEFINE(P_VMSEG, "vm_segment", sizeof(vm_segment_t));

static vm_map_t *kspace = &(vm_map_t){};

static int vm_map_insert_nolock(vm_map_t *map, vm_segment_t *seg,
                                vm_flags_t flags);

void vm_map_activate(vm_map_t *map) {
  SCOPED_NO_PREEMPTION();

  PCPU_SET(uspace, map);
  pmap_activate(map ? map->pmap : NULL);
}

void vm_map_switch(thread_t *td) {
  proc_t *p = td->td_proc;
  if (p)
    vm_map_activate(p->p_uspace);
}

void vm_map_lock(vm_map_t *map) {
  mtx_lock(&map->mtx);
}

void vm_map_unlock(vm_map_t *map) {
  mtx_unlock(&map->mtx);
}

vm_map_t *vm_map_user(void) {
  return PCPU_GET(uspace);
}

vm_map_t *vm_map_kernel(void) {
  return kspace;
}

vaddr_t vm_map_start(vm_map_t *map) {
  return map->pmap == pmap_kernel() ? KERNEL_SPACE_BEGIN : USER_SPACE_BEGIN;
}

vaddr_t vm_map_end(vm_map_t *map) {
  return map->pmap == pmap_kernel() ? KERNEL_SPACE_END : USER_SPACE_END;
}

vaddr_t vm_segment_start(vm_segment_t *seg) {
  return seg->start;
}

vaddr_t vm_segment_end(vm_segment_t *seg) {
  return seg->end;
}

bool vm_map_address_p(vm_map_t *map, vaddr_t addr) {
  return map && vm_map_start(map) <= addr && addr < vm_map_end(map);
}

bool vm_map_contains_p(vm_map_t *map, vaddr_t start, vaddr_t end) {
  return map && vm_map_start(map) <= start && end <= vm_map_end(map);
}

vm_map_t *vm_map_lookup(vaddr_t addr) {
  if (vm_map_address_p(vm_map_user(), addr))
    return vm_map_user();
  if (vm_map_address_p(vm_map_kernel(), addr))
    return vm_map_kernel();
  return NULL;
}

static void vm_map_setup(vm_map_t *map) {
  TAILQ_INIT(&map->entries);
  mtx_init(&map->mtx, 0);
}

void init_vm_map(void) {
  vm_map_setup(kspace);
  kspace->pmap = pmap_kernel();
  vm_map_activate(kspace);
}

vm_map_t *vm_map_new(void) {
  vm_map_t *map = pool_alloc(P_VMMAP, M_ZERO);
  vm_map_setup(map);
  map->pmap = pmap_new();
  return map;
}

vm_segment_t *vm_segment_alloc(vm_object_t *obj, vaddr_t start, vaddr_t end,
                               vm_prot_t prot, vm_seg_flags_t flags) {
  assert(page_aligned_p(start) && page_aligned_p(end));

  vm_segment_t *seg = pool_alloc(P_VMSEG, M_ZERO);
  seg->object = obj;
  seg->start = start;
  seg->end = end;
  seg->prot = prot;
  seg->flags = flags;
  return seg;
}

static void vm_segment_free(vm_segment_t *seg) {
  /* we assume no other segment points to this object */
  if (seg->object)
    vm_object_free(seg->object);
  pool_free(P_VMSEG, seg);
}

vm_segment_t *vm_map_find_segment(vm_map_t *map, vaddr_t vaddr) {
  assert(mtx_owned(&map->mtx));

  vm_segment_t *it;
  TAILQ_FOREACH (it, &map->entries, link)
    if (it->start <= vaddr && vaddr < it->end)
      return it;
  return NULL;
}

static void vm_map_insert_after(vm_map_t *map, vm_segment_t *after,
                                vm_segment_t *seg) {
  assert(mtx_owned(&map->mtx));
  if (after)
    TAILQ_INSERT_AFTER(&map->entries, after, seg, link);
  else
    TAILQ_INSERT_HEAD(&map->entries, seg, link);
  map->nentries++;
}

void vm_segment_destroy(vm_map_t *map, vm_segment_t *seg) {
  assert(mtx_owned(&map->mtx));

  TAILQ_REMOVE(&map->entries, seg, link);
  map->nentries--;
  vm_segment_free(seg);
}

void vm_segment_destroy_range(vm_map_t *map, vm_segment_t *seg, vaddr_t start,
                              vaddr_t end) {
  assert(mtx_owned(&map->mtx));
  assert(start >= seg->start && end <= seg->end);

  pmap_remove(map->pmap, start, end);
  if (seg->start == start && seg->end == end) {
    vm_segment_destroy(map, seg);
    return;
  }

  size_t length = end - start;
  vm_object_remove_pages(seg->object, start - seg->start, length);

  if (seg->start == start) {
    seg->start = end;
  } else if (seg->end == end) {
    seg->end = start;
  } else { /* a hole inside the segment */
    vm_object_t *obj = vm_object_clone(seg->object);
    vm_object_remove_pages(obj, 0, start - seg->start);
    vm_object_remove_pages(seg->object, end - seg->start, seg->end - end);
    vm_segment_t *new_seg =
      vm_segment_alloc(obj, end, seg->end, seg->prot, seg->flags);
    seg->end = start;
    vm_map_insert_after(map, new_seg, seg);
  }
}

void vm_map_delete(vm_map_t *map) {
  pmap_delete(map->pmap);
  WITH_MTX_LOCK (&map->mtx) {
    vm_segment_t *seg, *next;
    TAILQ_FOREACH_SAFE (seg, &map->entries, link, next)
      vm_segment_destroy(map, seg);
  }
  pool_free(P_VMMAP, map);
}

static bool vm_segment_split(vm_map_t *map, vm_segment_t *seg, vaddr_t start,
                             vaddr_t end) {
  if (seg->start == start && seg->end == end)
    return true;

  vm_object_t *obj = vm_object_alloc(seg->object->pager - &pagers[0]);

  if (obj == NULL) {
    return false;
  }

  vm_segment_t *new_seg =
    vm_segment_alloc(obj, start, end, seg->prot, seg->flags);

  if (new_seg == NULL) {
    return false;
  }

  off_t max_offset;
  off_t offset_base = 0;
  off_t wanted_offset;

  vm_flags_t flags = VM_FIXED;
  flags |= (new_seg->flags & VM_SEG_PRIVATE) ? VM_PRIVATE : VM_SHARED;

  if (seg->start == start && seg->end > end) {
    max_offset = (off_t)(end - seg->start);
    wanted_offset = (off_t)(start - seg->start);
    seg->start = end;
  } else if (seg->start < start && seg->end == end) {
    wanted_offset = (off_t)(start - seg->start);
    offset_base = (off_t)(seg->start - start);
    max_offset = (off_t)seg->end;
    seg->end = start;
  } else { /* seg->start < start && seg->end > end */
    vm_object_t *obj_before = vm_object_alloc(seg->object->pager - &pagers[0]);
    max_offset = (off_t)(end - seg->start);
    offset_base = (off_t)(seg->start - start);
    wanted_offset = (off_t)(start - seg->start);

    WITH_MTX_LOCK (&seg->object->mtx) {
      vm_page_t *pg, *next;
      TAILQ_FOREACH_SAFE (pg, &seg->object->list, obj.list, next) {
        if ((off_t)pg->offset >= wanted_offset)
          break;

        if ((off_t)pg->offset < wanted_offset) {
          TAILQ_REMOVE(&seg->object->list, pg, obj.list);
          seg->object->npages--;
          vm_object_add_page(obj_before, pg->offset, pg);
        }
      }
    }

    vm_segment_t *new_seg_before =
      vm_segment_alloc(obj_before, seg->start, start, seg->prot, seg->flags);

    if (new_seg_before == NULL) {
      /* TODO: handling errors, restoring pages etc. */
      return false;
    }

    seg->start = end;

    int error = vm_map_insert_nolock(map, new_seg_before, flags);

    if (error) {
      /* TODO: handling errors */
      return false;
    }
  }

  WITH_MTX_LOCK (&seg->object->mtx) {
    vm_page_t *pg, *next;
    TAILQ_FOREACH_SAFE (pg, &seg->object->list, obj.list, next) {
      if ((off_t)pg->offset >= max_offset)
        break;

      if ((off_t)pg->offset >= wanted_offset) {
        TAILQ_REMOVE(&seg->object->list, pg, obj.list);
        seg->object->npages--;
        vm_object_add_page(obj, offset_base + pg->offset, pg);
      }
    }
  }

  int error = vm_map_insert_nolock(map, new_seg, flags);

  if (error) {
    /* TODO: handling errors */
    return false;
  }

  return true;
}

static void vm_segment_protect(vm_map_t *map, vm_segment_t *seg, vaddr_t start,
                               vaddr_t end, vm_prot_t prot) {
  assert(seg != NULL);
  assert(seg->object != NULL);
  assert(seg->start >= start && seg->end >= end);

  if (seg->start == start && seg->end == end) {
    vm_object_protect(seg->object, start, end, prot);
  } else { /* we have to split our segment to at most 3 new */
    vm_segment_split(map, seg, start, end);

    seg = vm_map_find_segment(map, start);

    assert(seg != NULL);

    seg->prot = prot;
    vm_object_protect(seg->object, start, end, prot);
  }
}

void vm_map_protect(vm_map_t *map, vaddr_t start, vaddr_t end, vm_prot_t prot) {
  assert(map != NULL);

  WITH_MTX_LOCK (&map->mtx) {
    vaddr_t addr = start;

    while (addr < end) {
      vm_segment_t *seg = vm_map_find_segment(map, addr);
      assert(seg != NULL);
      vm_segment_protect(map, seg, max(addr, seg->start), min(end, seg->end),
                         prot);
      addr = seg->end;
    }
  }
}

static int vm_map_findspace_nolock(vm_map_t *map, vaddr_t /*inout*/ *start_p,
                                   size_t length, vm_segment_t **after_p) {
  vaddr_t start = *start_p;

  assert(page_aligned_p(start) && page_aligned_p(length));

  /* Bounds check */
  start = max(start, vm_map_start(map));
  if (start + length > vm_map_end(map))
    return ENOMEM;

  if (after_p)
    *after_p = NULL;

  /* Entire space free? */
  if (TAILQ_EMPTY(&map->entries))
    goto found;

  /* Is enought space before the first entry in the map? */
  vm_segment_t *first = TAILQ_FIRST(&map->entries);
  if (start + length <= first->start)
    goto found;

  /* Browse available gaps. */
  vm_segment_t *it;
  TAILQ_FOREACH (it, &map->entries, link) {
    vm_segment_t *next = TAILQ_NEXT(it, link);
    vaddr_t gap_start = it->end;
    vaddr_t gap_end = next ? next->start : vm_map_end(map);

    /* Move start address forward if it points inside allocated space. */
    if (start < gap_start)
      start = gap_start;

    /* Will we fit inside this gap? */
    if (start + length <= gap_end) {
      if (after_p)
        *after_p = it;
      goto found;
    }
  }

  /* Failed to find free space. */
  return ENOMEM;

found:
  *start_p = start;
  return 0;
}

int vm_map_findspace(vm_map_t *map, vaddr_t *start_p, size_t length) {
  SCOPED_MTX_LOCK(&map->mtx);
  return vm_map_findspace_nolock(map, start_p, length, NULL);
}

static int vm_map_insert_nolock(vm_map_t *map, vm_segment_t *seg,
                                vm_flags_t flags) {
  vm_segment_t *after;
  vaddr_t start = seg->start;
  size_t length = seg->end - seg->start;
  vm_seg_flags_t seg_flags = 0;

  int error = vm_map_findspace_nolock(map, &start, length, &after);
  if (error)
    return error;
  if ((flags & VM_FIXED) && (start != seg->start))
    return ENOMEM;

  assert((flags & (VM_SHARED | VM_PRIVATE)) != (VM_SHARED | VM_PRIVATE));

  seg_flags |= (flags & VM_SHARED) ? VM_SEG_SHARED : VM_SEG_PRIVATE;

  seg->start = start;
  seg->end = start + length;
  seg->flags = seg_flags;

  vm_map_insert_after(map, after, seg);
  return 0;
}

int vm_map_insert(vm_map_t *map, vm_segment_t *seg, vm_flags_t flags) {
  SCOPED_MTX_LOCK(&map->mtx);
  return vm_map_insert_nolock(map, seg, flags);
}

int vm_map_alloc_segment(vm_map_t *map, vaddr_t addr, size_t length,
                         vm_prot_t prot, vm_flags_t flags,
                         vm_segment_t **seg_p) {
  if (!(flags & VM_ANON)) {
    klog("Only anonymous memory mappings are supported!");
    return ENOTSUP;
  }

  if (!page_aligned_p(addr))
    return EINVAL;

  if (length == 0)
    return EINVAL;

  if (addr != 0 && !vm_map_contains_p(map, addr, addr + length))
    return EINVAL;

  /* Create object with a pager that supplies cleared pages on page fault. */
  vm_object_t *obj = vm_object_alloc(VM_ANONYMOUS);
  vm_segment_t *seg =
    vm_segment_alloc(obj, addr, addr + length, prot, VM_SEG_SHARED);

  /* Given the hint try to insert the segment at given position or after it. */
  if (vm_map_insert(map, seg, flags)) {
    vm_segment_free(seg);
    return ENOMEM;
  }

  *seg_p = seg;
  return 0;
}

int vm_segment_resize(vm_map_t *map, vm_segment_t *seg, vaddr_t new_end) {
  assert(page_aligned_p(new_end));
  assert(new_end >= seg->start);
  SCOPED_MTX_LOCK(&map->mtx);

  if (new_end >= seg->end) {
    /* Expanding entry */
    vm_segment_t *next = TAILQ_NEXT(seg, link);
    vaddr_t gap_end = next ? next->start : vm_map_end(map);
    if (new_end > gap_end)
      return ENOMEM;
  } else {
    /* Shrinking entry */
    off_t offset = new_end - seg->start;
    size_t length = seg->end - new_end;
    pmap_remove(map->pmap, new_end, seg->end);
    vm_object_remove_pages(seg->object, offset, length);
    /* TODO there's no reference to pmap in page, so we have to do it here */
  }

  seg->end = new_end;

  if (seg->start == seg->end)
    vm_segment_destroy(map, seg);

  return 0;
}

void vm_map_dump(vm_map_t *map) {
  SCOPED_MTX_LOCK(&map->mtx);

  klog("Virtual memory map (%08lx - %08lx):", vm_map_start(map),
       vm_map_end(map));

  vm_segment_t *it;
  TAILQ_FOREACH (it, &map->entries, link) {
    klog(" * %08lx - %08lx [%c%c%c]", it->start, it->end,
         (it->prot & VM_PROT_READ) ? 'r' : '-',
         (it->prot & VM_PROT_WRITE) ? 'w' : '-',
         (it->prot & VM_PROT_EXEC) ? 'x' : '-');
    vm_map_object_dump(it->object);
  }
}

vm_map_t *vm_map_clone(vm_map_t *map) {
  thread_t *td = thread_self();
  assert(td->td_proc);

  vm_map_t *new_map = vm_map_new();

  WITH_MTX_LOCK (&map->mtx) {
    vm_segment_t *it;
    TAILQ_FOREACH (it, &map->entries, link) {
      vm_object_t *obj;
      vm_segment_t *seg;

      if (it->flags & VM_SEG_SHARED) {
        refcnt_acquire(&it->object->ref_counter);
        obj = it->object;
      } else {
        /* vm_object_clone will clone the data from the vm_object_t
         * and will return the new object with ref_counter equal to one */
        obj = vm_object_clone(it->object);
      }
      seg = vm_segment_alloc(obj, it->start, it->end, it->prot, it->flags);
      TAILQ_INSERT_TAIL(&new_map->entries, seg, link);
      new_map->nentries++;
    }
  }

  return new_map;
}

int vm_page_fault(vm_map_t *map, vaddr_t fault_addr, vm_prot_t fault_type) {
  SCOPED_VM_MAP_LOCK(map);

  vm_segment_t *seg = vm_map_find_segment(map, fault_addr);

  if (!seg) {
    klog("Tried to access unmapped memory region: 0x%08lx!", fault_addr);
    return EFAULT;
  }

  if (seg->prot == VM_PROT_NONE) {
    klog("Cannot access to address: 0x%08lx", fault_addr);
    return EACCES;
  }

  if (!(seg->prot & VM_PROT_WRITE) && (fault_type == VM_PROT_WRITE)) {
    klog("Cannot write to address: 0x%08lx", fault_addr);
    return EACCES;
  }

  if (!(seg->prot & VM_PROT_READ) && (fault_type == VM_PROT_READ)) {
    klog("Cannot read from address: 0x%08lx", fault_addr);
    return EACCES;
  }

  assert(seg->start <= fault_addr && fault_addr < seg->end);

  vm_object_t *obj = seg->object;

  assert(obj != NULL);

  vaddr_t fault_page = fault_addr & -PAGESIZE;
  vaddr_t offset = fault_page - seg->start;
  vm_page_t *frame = vm_object_find_page(seg->object, offset);

  if (frame == NULL)
    frame = obj->pager->pgr_fault(obj, offset);

  if (frame == NULL)
    return EFAULT;

  pmap_enter(map->pmap, fault_page, frame, seg->prot, 0);

  return 0;
}
