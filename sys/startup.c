#include <common.h>
#include <stdc.h>
#include <mips/clock.h>
#include <malloc.h>
#include <pci.h>
#include <pmap.h>
#include <callout.h>
#include <sched.h>
#include <sleepq.h>
#include <thread.h>
#include <vm_object.h>
#include <vm_map.h>

extern int main(int argc, char **argv);

int kernel_init(int argc, char **argv) {
  kprintf("Kernel arguments (%d): ", argc);
  for (int i = 0; i < argc; i++)
    kprintf("%s ", argv[i]);
  kprintf("\n");

  pci_init();
  callout_init();
  pmap_init();
  vm_object_init();
  vm_map_init();
  sched_init();
  sleepq_init();
  mips_clock_init();
  kprintf("[startup] kernel initialized\n");
  thread_init((void (*)())main, 2, argc, argv);
}
