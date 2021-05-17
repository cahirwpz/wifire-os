#define KL_LOG KL_VM
#include <sys/klog.h>
#include <sys/errno.h>
#include <sys/interrupt.h>
#include <sys/cpu.h>
#include <sys/context.h>
#include <mips/tlb.h>
#include <sys/pmap.h>
#include <sys/sysent.h>
#include <sys/thread.h>
#include <sys/vm_map.h>
#include <sys/vm_physmem.h>

__no_instrument_kgprof static inline unsigned exc_code(ctx_t *ctx) {
  return (_REG(ctx, CAUSE) & CR_X_MASK) >> CR_X_SHIFT;
}

static void syscall_handler(ctx_t *ctx, syscall_result_t *result) {
  /* TODO Eventually we should have a platform-independent syscall handler. */
  register_t args[SYS_MAXSYSARGS];
  register_t code = _REG(ctx, V0);
  const size_t nregs = min(SYS_MAXSYSARGS, FUNC_MAXREGARGS);
  int error = 0;

  /*
   * Copy the arguments passed via register from the
   * trapctx to our argument array
   */
  memcpy(args, &_REG(ctx, A0), nregs * sizeof(register_t));

  if (code > SYS_MAXSYSCALL) {
    args[0] = code;
    code = 0;
  }

  sysent_t *se = &sysent[code];
  size_t nargs = se->nargs;

  if (nargs > nregs) {
    /*
     * From ABI:
     * Despite the fact that some or all of the arguments to a function are
     * passed in registers, always allocate space on the stack for all
     * arguments.
     * For this reason, we read from the user stack with some offset.
     */
    vaddr_t usp = _REG(ctx, SP) + nregs * sizeof(register_t);
    error = copyin((register_t *)usp, &args[nregs],
                   (nargs - nregs) * sizeof(register_t));
  }

  /* Call the handler. */
  thread_t *td = thread_self();
  register_t retval = 0;

  assert(td->td_proc != NULL);

  if (!error)
    error = se->call(td->td_proc, (void *)args, &retval);

  result->retval = error ? -1 : retval;
  result->error = error;
}

/*
 * This is exception name table. For exact meaning of exception handlers
 * numbers please check 5.23 Table of MIPS32 4KEc User's Manual.
 */

/* clang-format off */
static const char *const exceptions[32] = {
  [EXC_INTR] = "Interrupt",
  [EXC_MOD] = "TLB modification exception",
  [EXC_TLBL] = "TLB exception (load or instruction fetch)",
  [EXC_TLBS] = "TLB exception (store)",
  [EXC_ADEL] = "Address error exception (load or instruction fetch)",
  [EXC_ADES] = "Address error exception (store)",
  [EXC_IBE] = "Bus error exception (instruction fetch)",
  [EXC_DBE] = "Bus error exception (data reference: load or store)",
  [EXC_SYS] = "System call",
  [EXC_BP] = "Breakpoint exception",
  [EXC_RI] = "Reserved instruction exception",
  [EXC_CPU] = "Coprocessor Unusable exception",
  [EXC_OVF] = "Arithmetic Overflow exception",
  [EXC_TRAP] = "Trap exception",
  [EXC_FPE] = "Floating point exception",
  [EXC_TLBRI] = "TLB read inhibit",
  [EXC_TLBXI] = "TLB execute inhibit",
  [EXC_WATCH] = "Reference to watchpoint address",
  [EXC_MCHECK] = "Machine checkcore",
};
/* clang-format on */

static __noreturn void kernel_oops(ctx_t *ctx) {
  unsigned code = exc_code(ctx);

  klog("%s at $%08lx!", exceptions[code], _REG(ctx, EPC));
  switch (code) {
    case EXC_ADEL:
    case EXC_ADES:
    case EXC_IBE:
    case EXC_DBE:
    case EXC_TLBL:
    case EXC_TLBS:
      klog("Caused by reference to $%08lx!", _REG(ctx, BADVADDR));
      break;
    case EXC_RI:
      klog("Reserved instruction exception in kernel mode!");
      break;
    case EXC_CPU:
      klog("Coprocessor unusable exception in kernel mode!");
      break;
    case EXC_FPE:
    case EXC_MSAFPE:
    case EXC_OVF:
      klog("Floating point exception or integer overflow in kernel mode!");
      break;
    default:
      break;
  }
  klog("HINT: Type 'info line *0x%08lx' into gdb to find faulty code line.",
       _REG(ctx, EPC));
  panic("KERNEL PANIC!!!");
}

static void tlb_exception_handler(ctx_t *ctx) {
  thread_t *td = thread_self();

  int code = exc_code(ctx);
  vaddr_t vaddr = _REG(ctx, BADVADDR);

  klog("%s at $%08x, caused by reference to $%08lx!", exceptions[code],
       _REG(ctx, EPC), vaddr);

  pmap_t *pmap = pmap_lookup(vaddr);
  if (!pmap) {
    klog("No physical map defined for %08lx address!", vaddr);
    goto fault;
  }

  vm_prot_t access = (code == EXC_TLBL) ? VM_PROT_READ : VM_PROT_WRITE;
  int error = pmap_emulate_bits(pmap, vaddr, access);
  if (error == 0)
    return;

  if (error == EACCES || error == EINVAL)
    goto fault;

  vm_map_t *vmap = vm_map_lookup(vaddr);
  if (!vmap) {
    klog("No virtual address space defined for %08lx!", vaddr);
    goto fault;
  }

  if (vm_page_fault(vmap, vaddr, access) == 0)
    return;

fault:
  if (td->td_onfault) {
    /* handle copyin / copyout faults */
    _REG(ctx, EPC) = td->td_onfault;
    td->td_onfault = 0;
  } else if (user_mode_p(ctx)) {
    /* Send a segmentation fault signal to the user program. */
    sig_trap(ctx, SIGSEGV);
  } else {
    /* Panic when kernel-mode thread uses wrong pointer. */
    kernel_oops(ctx);
  }
}

static void user_trap_handler(ctx_t *ctx) {
  /* We came here from user-space,
   * hence interrupts and preemption must have be enabled. */
  cpu_intr_enable();

  assert(!intr_disabled() && !preempt_disabled());

  int cp_id;
  syscall_result_t result;
  unsigned int code = exc_code(ctx);

  switch (code) {
    case EXC_MOD:
    case EXC_TLBL:
    case EXC_TLBS:
      tlb_exception_handler(ctx);
      break;

    /*
     * An address error exception occurs under the following circumstances:
     *  - instruction address is not aligned on a word boundary
     *  - load/store with an address is not aligned on a word/halfword boundary
     *  - reference to a kernel/supervisor address from user-space
     */
    case EXC_ADEL:
    case EXC_ADES:
      sig_trap(ctx, SIGBUS);
      break;

    case EXC_SYS:
      syscall_handler(ctx, &result);
      break;

    case EXC_FPE:
    case EXC_MSAFPE:
    case EXC_OVF:
      sig_trap(ctx, SIGFPE);
      break;

    case EXC_CPU:
      cp_id = (_REG(ctx, CAUSE) & CR_CEMASK) >> CR_CESHIFT;
      if (cp_id != 1) {
        sig_trap(ctx, SIGILL);
      } else {
        /* Enable FPU for interrupted context. */
        thread_self()->td_pflags |= TDP_FPUINUSE;
        _REG(ctx, SR) |= SR_CU1;
      }
      break;

    case EXC_RI:
      sig_trap(ctx, SIGILL);
      break;

    default:
      kernel_oops(ctx);
  }

  /* This is right moment to check if out time slice expired. */
  on_exc_leave();

  /* If we're about to return to user mode then check pending signals, etc. */
  on_user_exc_leave((mcontext_t *)ctx, code == EXC_SYS ? &result : NULL);
}

static void kern_trap_handler(ctx_t *ctx) {
  /* We came here from kernel-space. If interrupts were enabled before we
   * trapped, then turn them on here. */
  if (_REG(ctx, SR) & SR_IE)
    cpu_intr_enable();

  switch (exc_code(ctx)) {
    case EXC_MOD:
    case EXC_TLBL:
    case EXC_TLBS:
      tlb_exception_handler(ctx);
      break;

    default:
      kernel_oops(ctx);
  }
}

__no_instrument_kgprof void mips_exc_handler(ctx_t *ctx) {
  assert(cpu_intr_disabled());

  bool user_mode = user_mode_p(ctx);

  if (!user_mode) {
    /* If there's not enough space on the stack to store another exception
     * frame we consider situation to be critical and panic.
     * Hopefully sizeof(ctx_t) bytes of unallocated stack space will be enough
     * to display error message. */
    register_t sp = mips32_get_sp();
    if ((sp & (PAGESIZE - 1)) < sizeof(ctx_t))
      panic("Kernel stack overflow caught at $%08lx!", _REG(ctx, EPC));
  }

  if (exc_code(ctx)) {
    if (user_mode)
      user_trap_handler(ctx);
    else
      kern_trap_handler(ctx);
  } else {
    intr_root_handler(ctx);
  }
}
