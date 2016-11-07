#include <sys_mmap.h>
#include <thread.h>
#include <stdc.h>
#include <errno.h>
#include <vm_map.h>
#include <vm_pager.h>

vm_addr_t do_mmap(vm_addr_t addr, size_t length, vm_prot_t prot, int flags,
                  int *error) {
  thread_t *td = thread_self();
  vm_map_t *vmap = td->td_uspace;

  if (!vmap) {
    log("mmap called by a thread with no user space.");
    /* I have no idea what would be a good error code for this case. */
    *error = ENOENT;
    return MMAP_FAILED;
  }

  if (addr >= vmap->pmap->end) {
    /* mmap cannot callocate memory in kernel space! */
    *error = EINVAL;
    return MMAP_FAILED;
  }

  if (!flags & MMAP_FLAG_ANONYMOUS) {
    log("Non-anonymous memory mappings are not yet implemented.");
    *error = EINVAL;
    return MMAP_FAILED;
  }

  length = roundup(length, PAGESIZE);

  if (addr == 0x0) {
    /* We will choose an appropriate place in vm map. */
    if (vm_map_findspace(vmap, MMAP_LOW_ADDR, length, &addr) != 0) {
      /* No space in memory was found. */
      *error = ENOMEM;
      return MMAP_FAILED;
    }
  } else {
    if (addr < MMAP_LOW_ADDR)
      addr = MMAP_LOW_ADDR;
    addr = roundup(addr, PAGESIZE);
    /* Treat addr as a hint on where to place the new mapping. */
    if (vm_map_findspace(vmap, addr, length, &addr) != 0) {
      /* No memory was found following the hint. Search again entire address
         space. */
      if (vm_map_findspace(vmap, MMAP_LOW_ADDR, length, &addr) != 0) {
        /* Still no memory found. */
        *error = ENOMEM;
        return MMAP_FAILED;
      }
    }
  }

  /* TODO: Check if the range of addresses we are about to map is not already
     mapped. */

  /* Create new vm map entry for this allocation. Temporarily use permissive
   * protection, so that we may optionally initialize the entry. */
  vm_map_entry_t *entry =
    vm_map_add_entry(vmap, addr, addr + length, VM_PROT_READ | VM_PROT_WRITE);
  /* Assign a pager. */
  entry->object = default_pager->pgr_alloc();

  log("Created entry at %p, length: %ld", (void *)addr, length);

  if (flags & MMAP_FLAG_ANONYMOUS) {
    /* Calling bzero here defeats the purpose of paging on demand, but I do not
       know if I can assume that paged provides us with zeroed pages. */
    bzero((void *)addr, 1);
  }

  /* Apply requested protection. */
  vm_map_protect(vmap, addr, addr + length, prot);

  return addr;
}
