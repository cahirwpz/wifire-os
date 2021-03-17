#ifndef _SYS_PCPU_H_
#define _SYS_PCPU_H_

#include <machine/types.h>
#include <machine/pcpu.h>
#include <stdbool.h>

typedef struct thread thread_t;
typedef struct pmap pmap_t;
typedef struct vm_map vm_map_t;

/*! \brief Private per-cpu structure. */
typedef struct pcpu {
  bool no_switch;        /*!< executing code that must not switch out */
  thread_t *curthread;   /*!< thread running on this CPU */
  thread_t *idle_thread; /*!< idle thread executed on this CPU */
  pmap_t *curpmap;       /*!< current page table */
  vm_map_t *uspace;      /*!< user space virtual memory map */
  void *panic_sp;        /*!< stack to handle kernel stack overflows */

  /* Machine-dependent part */
  PCPU_MD_FIELDS;
} pcpu_t;

extern pcpu_t _pcpu_data[1];

/* Read pcpu.h from FreeBSD for API reference */
#define PCPU_GET(member) (_pcpu_data->member)
#define PCPU_PTR(member) (&_pcpu_data->member)
#define PCPU_SET(member, value) (_pcpu_data->member = (value))

#endif /* !_SYS_PCPU_H_ */
