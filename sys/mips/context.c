#include <sys/context.h>
#include <sys/libkern.h>
#include <sys/errno.h>
#include <sys/thread.h>
#include <mips/mips.h>
#include <mips/m32c0.h>
#include <mips/mcontext.h>
#include <sys/ucontext.h>
#include <sys/context.h>

void ctx_init(ctx_t *ctx, void *pc, void *sp) {
  bzero(ctx, sizeof(ctx_t));

  _REG(ctx, GP) = (register_t)_gp;
  _REG(ctx, EPC) = (register_t)pc;
  _REG(ctx, SP) = (register_t)sp;

  /* Take SR from caller's context and enable interrupts. */
  _REG(ctx, SR) = mips32_get_c0(C0_STATUS) | SR_IE;
}

void ctx_setup_call(ctx_t *ctx, register_t retaddr, register_t arg) {
  _REG(ctx, RA) = retaddr;
  _REG(ctx, A0) = arg;
}

void ctx_set_retval(ctx_t *ctx, long value) {
  _REG(ctx, V0) = (register_t)value;
}

register_t ctx_get_pc(ctx_t *ctx) {
  return _REG(ctx, EPC);
}

void mcontext_copy(mcontext_t *to, mcontext_t *from) {
  memcpy(to, from, sizeof(mcontext_t));
}

void mcontext_init(mcontext_t *ctx, void *pc, void *sp) {
  bzero(ctx, sizeof(mcontext_t));

  _REG(ctx, GP) = (register_t)0;
  _REG(ctx, EPC) = (register_t)pc;
  _REG(ctx, SP) = (register_t)sp;

  /* For user-mode context we must make sure that:
   * - user mode is active,
   * - interrupts are enabled.
   * The rest will be covered by usr_exc_leave. */
  _REG(ctx, SR) = mips32_get_c0(C0_STATUS) | SR_IE | SR_KSU_USER;
}

void mcontext_set_retval(mcontext_t *ctx, register_t value, register_t error) {
  _REG(ctx, V0) = (register_t)value;
  _REG(ctx, V1) = (register_t)error;
  _REG(ctx, EPC) += 4;
}

void mcontext_restart_syscall(mcontext_t *ctx) {
  /* Nothing needs to be done. */
}

__no_instrument_kgprof bool user_mode_p(ctx_t *ctx) {
  return (_REG(ctx, SR) & SR_KSU_MASK) == SR_KSU_USER;
}

int do_setcontext(thread_t *td, ucontext_t *uc) {
  mcontext_t *from = &uc->uc_mcontext;
  mcontext_t *to = td->td_uctx;

  /* registers AT-PC */
  if (uc->uc_flags & _UC_CPU)
    memcpy(&_REG(to, AT), &_REG(from, AT),
           sizeof(__greg_t) * (_REG_EPC - _REG_AT + 1));

  /* 32 FP registers + FP CSR */
  if (uc->uc_flags & _UC_FPU)
    memcpy(&to->__fpregs.__fp_r, &from->__fpregs.__fp_r,
           sizeof(from->__fpregs.__fp_r) + sizeof(from->__fpregs.__fp_csr));

  return EJUSTRETURN;
}
