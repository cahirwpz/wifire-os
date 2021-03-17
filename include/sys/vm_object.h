#ifndef _SYS_VM_OBJECT_H_
#define _SYS_VM_OBJECT_H_

#include <sys/vm.h>
#include <sys/vm_pager.h>
#include <sys/mutex.h>
#include <sys/refcnt.h>

/*! \brief Virtual memory object
 *
 * Field marking and corresponding locks:
 * (a) atomic
 * (@) vm_object::mtx
 */

typedef struct vm_object {
  mtx_t mtx;
  vm_pagelist_t list;   /* (@) List of pages */
  size_t npages;        /* (@) Number of pages */
  vm_pager_t *pager;    /* Pager type and page fault function for object */
  refcnt_t ref_counter; /* (a) How many objects refer to this object? */
} vm_object_t;

vm_object_t *vm_object_alloc(vm_pgr_type_t type);
void vm_object_free(vm_object_t *obj);
void vm_object_add_page(vm_object_t *obj, vm_offset_t off, vm_page_t *pg);
void vm_object_remove_pages(vm_object_t *obj, vm_offset_t off, size_t len);
vm_page_t *vm_object_find_page(vm_object_t *obj, vm_offset_t off);
vm_object_t *vm_object_clone(vm_object_t *obj);
void vm_map_object_dump(vm_object_t *obj);
void vm_object_protect(vm_object_t *obj, vaddr_t start, vaddr_t end,
                       vm_prot_t prot);

#endif /* !_SYS_VM_OBJECT_H_ */
