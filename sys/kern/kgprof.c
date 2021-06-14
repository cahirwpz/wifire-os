#include <sys/klog.h>
#include <sys/context.h>
#include <sys/gmon.h>
#include <sys/kmem.h>
#include <sys/timer.h>
#include <sys/thread.h>
#include <machine/vm_param.h>

gmonparam_t _gmonparam = {.state = GMON_PROF_NOT_INIT};
static gmonhdr_t _gmonhdr = {.profrate = CLK_TCK};

/* The macros description are provided in gmon.h */
void init_kgprof(void) {
  gmonparam_t *p = &_gmonparam;

  p->lowpc = rounddown((u_long)__text, INSTR_GRANULARITY);
  p->highpc = roundup((u_long)__etext, INSTR_GRANULARITY);
  p->textsize = p->highpc - p->lowpc;
  p->kcountsize = p->textsize / HISTFRACTION;
  p->hashfraction = HASHFRACTION;
  p->fromssize = p->textsize / HASHFRACTION;
  p->tolimit = (p->textsize * ARCDENSITY) / 100;
  p->tolimit = min(max(p->tolimit, MINARCS), MAXARCS);
  p->tossize = p->tolimit * sizeof(tostruct_t);

  size_t size = p->kcountsize + p->tossize + p->fromssize;
  void *profptr = kmem_alloc(align(size, PAGESIZE), M_NOWAIT | M_ZERO);
  assert(profptr != NULL);

  p->tos = (tostruct_t *)profptr;
  profptr += p->tossize;
  p->kcount = (u_short *)profptr;
  profptr += p->kcountsize;
  p->froms = (u_short *)profptr;

  assert(is_aligned(p->tos, alignof(tostruct_t)));
  assert(is_aligned(p->kcount, alignof(u_short)));
  assert(is_aligned(p->froms, alignof(u_short)));

  gmonhdr_t *hdr = &_gmonhdr;
  hdr->lpc = p->lowpc;
  hdr->hpc = p->highpc;
  hdr->ncnt = p->kcountsize + sizeof(gmonhdr_t);
  hdr->version = GMONVERSION;
  hdr->spare[0] = hdr->spare[1] = hdr->spare[2] = 0;

  p->state = GMON_PROF_ON;
}

void kgprof_tick(void) {
  assert(intr_disabled());

  uintptr_t pc, instr;
  gmonparam_t *g = &_gmonparam;
  thread_t *td = thread_self();

  if (td->td_kframe == NULL)
    return;

  pc = ctx_get_pc(td->td_kframe);
  if (g->state == GMON_PROF_ON && pc >= g->lowpc) {
    instr = pc - g->lowpc;
    if (instr < g->textsize) {
      instr /= INSTR_GRANULARITY;
      g->kcount[instr]++;
    }
  }
}

void set_kgprof_profrate(int profrate) {
  _gmonhdr.profrate = profrate;
}

timer_t *get_stat_timer(void) {
  return tm_reserve(NULL, TMF_PERIODIC);
}
