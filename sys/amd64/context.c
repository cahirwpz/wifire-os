#include <sys/klog.h>
#include <sys/context.h>
#include <sys/thread.h>

void ctx_init(ctx_t *ctx __unused, void *pc __unused, void *sp __unused) {
  panic("not implemented!");
}

void ctx_setup_call(ctx_t *ctx __unused, register_t retaddr __unused,
                    register_t arg __unused) {
  panic("not implemented!");
}

void ctx_set_retval(ctx_t *ctx __unused, long value __unused) {
  panic("not implemented!");
}

register_t ctx_get_pc(ctx_t *ctx __unused) {
  panic("not implemented!");
}

void mcontext_copy(mcontext_t *to __unused, mcontext_t *from __unused) {
  panic("not implemented!");
}

void mcontext_init(mcontext_t *ctx __unused, void *pc __unused,
                   void *sp __unused) {
  panic("not implemented!");
}

void mcontext_set_retval(mcontext_t *ctx __unused, register_t value __unused,
                         register_t error __unused) {
  panic("not implemented!");
}

void mcontext_restart_syscall(mcontext_t *ctx __unused) {
  panic("not implemented!");
}

bool user_mode_p(ctx_t *ctx __unused) {
  panic("not implemented!");
}

int do_setcontext(thread_t *td __unused, ucontext_t *uc __unused) {
  panic("not implemented!");
}
