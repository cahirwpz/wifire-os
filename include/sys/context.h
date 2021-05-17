#ifndef _SYS_CONTEXT_H_
#define _SYS_CONTEXT_H_

#include <sys/cdefs.h>
#include <sys/types.h>
#include <stdbool.h>

typedef struct thread thread_t;
typedef struct __ucontext ucontext_t;
typedef struct ctx ctx_t;
typedef struct mcontext mcontext_t;

/*! \brief Checks if saved context belongs to user space. */
bool user_mode_p(ctx_t *ctx) __no_instrument_kgprof;

/*! \brief Prepare ctx to jump into a kernel thread. */
void ctx_init(ctx_t *ctx, void *pc, void *sp);

/*! \brief Set args and return address for context that calls a procedure. */
void ctx_setup_call(ctx_t *ctx, register_t retaddr, register_t arg);

/*! \brief Sets a value that will be returned by ctx_switch. */
void ctx_set_retval(ctx_t *ctx, long value);

/*! \brief Gets program counter from context. */
register_t ctx_get_pc(ctx_t *ctx);

/*! \brief Copy user exception ctx. */
void mcontext_copy(mcontext_t *to, mcontext_t *from);

/*! \brief Prepare ctx to jump into a user-space program. */
void mcontext_init(mcontext_t *ctx, void *pc, void *sp);

/*! \brief Set a return value within the ctx and advance the program counter.
 *
 * Useful for returning values from syscalls. */
void mcontext_set_retval(mcontext_t *ctx, register_t value, register_t error);

/*! \brief Set up the user context to restart a syscall.
 *
 * `ctx` is assumed to be exactly the same as it was on syscall entry. */
void mcontext_restart_syscall(mcontext_t *ctx);

/* This function stores the current context to @from, and resumes the
 * context stored in @to. It does not return immediatelly, it returns
 * only when the @from context is resumed.
 *
 * \returns a value set by \a ctx_set_retval or 0 otherwise.
 * \note must be called with interrupts disabled!
 */
long ctx_switch(thread_t *from, thread_t *to);

/* Implementation of setcontext syscall. */
int do_setcontext(thread_t *td, ucontext_t *uc);

#endif /* !_SYS_CONTEXT_H_ */
