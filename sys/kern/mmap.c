#define KL_LOG KL_VM
#include <sys/klog.h>
#include <sys/mimiker.h>
#include <sys/mman.h>
#include <sys/thread.h>
#include <sys/errno.h>
#include <sys/vm_map.h>
#include <sys/vm_object.h>
#include <sys/mutex.h>
#include <sys/proc.h>

/* Ensure kernel vm_prot_t & vm_flags_t map directly to user-space constants. */
static_assert(VM_PROT_NONE == PROT_NONE, "VM_PROT_NONE != PROT_NONE");
static_assert(VM_PROT_READ == PROT_READ, "VM_PROT_READ != PROT_READ");
static_assert(VM_PROT_WRITE == PROT_WRITE, "VM_PROT_WRITE != PROT_WRITE");
static_assert(VM_PROT_EXEC == PROT_EXEC, "VM_PROT_EXEC != PROT_EXEC");

static_assert(VM_FILE == MAP_FILE, "VM_FILE != MAP_FILE");
static_assert(VM_ANON == MAP_ANON, "VM_ANON != MAP_ANON");
static_assert(VM_SHARED == MAP_SHARED, "VM_SHARED != MAP_SHARED");
static_assert(VM_PRIVATE == MAP_PRIVATE, "VM_PRIVATE != MAP_PRIVATE");
static_assert(VM_FIXED == MAP_FIXED, "VM_FIXED != MAP_FIXED");
static_assert(VM_STACK == MAP_STACK, "VM_STACK != MAP_STACK");

int do_mmap(vaddr_t *addr_p, size_t length, int u_prot, int u_flags) {
  thread_t *td = thread_self();
  assert(td->td_proc != NULL);
  vm_map_t *vmap = td->td_proc->p_uspace;
  assert(vmap != NULL);

  vm_prot_t prot = u_prot;
  vm_flags_t flags = u_flags;

  vaddr_t addr = *addr_p;
  *addr_p = (vaddr_t)MAP_FAILED;
  length = roundup(length, PAGESIZE);

  vm_flags_t sharing = flags & (VM_SHARED | VM_PRIVATE);
  if (sharing == (VM_SHARED | VM_PRIVATE))
    return EINVAL;
  if (sharing == 0)
    return EINVAL;

  int error;
  vm_segment_t *seg;
  if ((error = vm_map_alloc_segment(vmap, addr, length, prot, flags, &seg)))
    return error;

  vaddr_t start = vm_segment_start(seg);

  klog("Created segment at %p, length: %u", (void *)start, length);

  *addr_p = start;
  return 0;
}

int do_munmap(vaddr_t addr, size_t length) {
  thread_t *td = thread_self();
  assert(td && td->td_proc && td->td_proc->p_uspace);

  vm_map_t *uspace = proc_self()->p_uspace;

  if (length == 0)
    return EINVAL;

  if (!page_aligned_p(addr) || !page_aligned_p(length))
    return EINVAL;

  vaddr_t right_boundary = addr + length;

  WITH_VM_MAP_LOCK (uspace) {
    while (addr < right_boundary) {
      vm_segment_t *seg = vm_map_find_segment(uspace, addr);
      if (!seg)
        return EINVAL;

      vaddr_t end = vm_segment_end(seg);

      vm_segment_destroy_range(uspace, seg, addr, min(right_boundary, end));

      addr = end;
    }
  }
  return 0;
}

int do_mprotect(vaddr_t addr, size_t length, vm_prot_t prot) {
  thread_t *td = thread_self();
  assert(td && td->td_proc && td->td_proc->p_uspace);

  vm_map_t *uspace = td->td_proc->p_uspace;

  if (!page_aligned_p(addr) || !page_aligned_p(length))
    return EINVAL;

  vaddr_t right_boundary = addr + length;
  WITH_VM_MAP_LOCK (uspace) {
    /* addresses in range [addr, addr + length) are invalid for the address
     * space of the process */
    if (!vm_map_contains_p(uspace, addr, right_boundary))
      return ENOMEM;
  }

  vm_map_protect(uspace, addr, right_boundary, prot);

  return 0;
}
