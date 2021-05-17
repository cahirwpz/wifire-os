#define KL_LOG KL_PMAP
#include <sys/klog.h>
#include <sys/pmap.h>
#include <sys/vm_physmem.h>

typedef struct pmap {
} pmap_t;

typedef struct pv_entry {
} pv_entry_t;

pmap_t *pmap_kernel(void) {
  panic("not implemented!");
}

pmap_t *pmap_user(void) {
  panic("not implemented!");
}

pmap_t *pmap_lookup(vaddr_t addr __unused) {
  panic("not implemented!");
}

void pmap_activate(pmap_t *umap __unused) {
  panic("not implemented!");
}

void pmap_kenter(vaddr_t va __unused, paddr_t pa __unused,
                 vm_prot_t prot __unused, unsigned flags __unused) {
  panic("not implemented!");
}

void pmap_kremove(vaddr_t va __unused, size_t size __unused) {
  panic("not implemented!");
}

bool pmap_kextract(vaddr_t va __unused, paddr_t *pap __unused) {
  panic("not implemented!");
}

void pmap_enter(pmap_t *pmap __unused, vaddr_t va __unused,
                vm_page_t *pg __unused, vm_prot_t prot __unused,
                unsigned flags __unused) {
  panic("not implemented!");
}

void pmap_remove(pmap_t *pmap __unused, vaddr_t start __unused,
                 vaddr_t end __unused) {
  panic("not implemented!");
}

void pmap_protect(pmap_t *pmap __unused, vaddr_t start __unused,
                  vaddr_t end __unused, vm_prot_t prot __unused) {
  panic("not implemented!");
}

bool pmap_extract(pmap_t *pmap __unused, vaddr_t va __unused,
                  paddr_t *pap __unused) {
  panic("not implemented!");
}

void pmap_page_remove(vm_page_t *pg __unused) {
  panic("not implemented!");
}

void pmap_zero_page(vm_page_t *pg __unused) {
  panic("not implemented!");
}

void pmap_copy_page(vm_page_t *src __unused, vm_page_t *dst __unused) {
  panic("not implemented!");
}

bool pmap_clear_referenced(vm_page_t *pg __unused) {
  panic("not implemented!");
}

bool pmap_clear_modified(vm_page_t *pg __unused) {
  panic("not implemented!");
}

bool pmap_is_referenced(vm_page_t *pg __unused) {
  panic("not implemented!");
}

bool pmap_is_modified(vm_page_t *pg __unused) {
  panic("not implemented!");
}

void pmap_set_referenced(vm_page_t *pg __unused) {
  panic("not implemented!");
}

void pmap_set_modified(vm_page_t *pg __unused) {
  panic("not implemented!");
}

int pmap_emulate_bits(pmap_t *pmap __unused, vaddr_t va __unused,
                      vm_prot_t prot __unused) {
  panic("not implemented!");
}

void init_pmap(void) {
  panic("not implemented!");
}

pmap_t *pmap_new(void) {
  panic("not implemented!");
}

void pmap_delete(pmap_t *pmap __unused) {
  panic("not implemented!");
}

void pmap_growkernel(vaddr_t maxkvaddr __unused) {
  panic("not implemented!");
}
