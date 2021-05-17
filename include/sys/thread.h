#ifndef _SYS_THREAD_H_
#define _SYS_THREAD_H_

#include <sys/cdefs.h>
#include <sys/queue.h>
#include <sys/context.h>
#include <sys/callout.h>
#include <sys/exception.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/priority.h>
#include <sys/time.h>
#include <sys/signal.h>
#include <sys/sigtypes.h>
#include <sys/kstack.h>
#include <sys/spinlock.h>
#include <sys/lockdep.h>

/*! \file thread.h */

struct turnstile;
typedef struct turnstile turnstile_t;
typedef struct sleepq sleepq_t;
typedef struct vm_page vm_page_t;
typedef struct vm_map vm_map_t;
typedef struct fdtab fdtab_t;
typedef struct proc proc_t;
typedef void (*entry_fn_t)(void *);

#define TD_NAME_MAX 32

/*! \brief Possible thread states.
 *
 * Possible state transitions look as follows:
 *  - INACTIVE -> READY (parent thread)
 *  - READY -> RUNNING (dispatcher)
 *  - RUNNING -> READY (dispatcher, self)
 *  - RUNNING -> SLEEPING (self)
 *  - RUNNING -> BLOCKED (self)
 *  - RUNNING -> STOPPED (self)
 *  - STOPPED -> READY (other threads)
 *  - SLEEPING -> READY (interrupts, other threads)
 *  - BLOCKED -> READY (other threads)
 *  - * -> DEAD (other threads or self)
 */
typedef enum {
  /*!< thread was created but it has not been run so far */
  TDS_INACTIVE,
  /*!< thread was put onto a run queue and it's ready to be dispatched */
  TDS_READY,
  /*!< thread was removed from a run queue and is being run at the moment */
  TDS_RUNNING,
  /*!< thread is waiting on a resource and it has been put on a sleep queue */
  TDS_SLEEPING,
  /*!< thread is waiting for a lock and it has been put on a turnstile */
  TDS_BLOCKED,
  /*!< thread stopped by a signal: not on a runqueue, not running */
  TDS_STOPPED,
  /*!< thread finished or was terminated by the kernel and awaits recycling */
  TDS_DEAD
} thread_state_t;

#define TDF_SLICEEND 0x00000001   /* run out of time slice */
#define TDF_NEEDSWITCH 0x00000002 /* must switch on next opportunity */
#define TDF_NEEDSIGCHK 0x00000004 /* signals were posted for delivery */
#define TDF_STOPPING 0x00000008   /* thread is about to stop */
#define TDF_BORROWING 0x00000010  /* priority propagation */
/* TDF_SLP* flags are used internally by sleep queue */
#define TDF_SLPINTR 0x00000040  /* sleep is interruptible */
#define TDF_SLPTIMED 0x00000080 /* sleep with timeout */

typedef enum {
  TDP_OLDSIGMASK = 0x01,  /* Pass td_oldsigmask as return mask to send_sig(). */
  TDP_FPUCTXSAVED = 0x02, /* FPU context was saved by `ctx_switch`. */
  TDP_FPUINUSE = 0x04     /* FPU is in use and its context should be saved &
                              restored on demand. */
} tdp_flags_t;

/*! \brief Thread structure
 *
 * UP = uniprocessor
 * MP = multiprocessor
 *
 * Field markings and the corresponding locks:
 *  - a: threads_lock
 *  - t: thread_t::td_lock
 *  - p: thread_t::td_proc::p_lock
 *  - @: read-only access
 *  - ~: always safe to access
 *  - #: UP & no preemption, only use from same thread
 *  - $: UP & no interrupts
 *  - *: only use from same thread, may read from any if thread is asleep
 *
 * Locking order:
 *  threads_lock >> thread_t::td_lock
 */
typedef struct thread {
  /* locking */
  spin_t *volatile td_lock; /*!< (~) used by dispatcher & scheduler */
  condvar_t td_waitcv;      /*!< (t) for thread_join */
  /* linked lists */
  TAILQ_ENTRY(thread) td_all;      /* (a) link on all threads list */
  TAILQ_ENTRY(thread) td_runq;     /* ($) link on run queue */
  TAILQ_ENTRY(thread) td_sleepq;   /* ($) link on sleep queue */
  TAILQ_ENTRY(thread) td_blockedq; /* (#) link on turnstile blocked queue */
  TAILQ_ENTRY(thread) td_zombieq;  /* (a) link on zombie queue */
  /* Properties */
  proc_t *td_proc; /*!< (t) parent process (NULL for kernel threads) */
  char *td_name;   /*!< (@) name of thread */
  tid_t td_tid;    /*!< (@) thread identifier */
  /* thread state */
  thread_state_t td_state;        /*!< (t) thread state */
  volatile uint32_t td_flags;     /*!< (t) TDF_* flags */
  volatile tdp_flags_t td_pflags; /*!< (*) TDP_* (private) flags */
  /* thread context */
  volatile unsigned td_idnest; /*!< (*) interrupt disable nest level */
  volatile unsigned td_pdnest; /*!< (*) preemption disable nest level */
  mcontext_t *td_uctx;         /*!< (*) user context (full exc. frame) */
  ctx_t *td_kframe;            /*!< (*) kernel context (last trap frame) */
  ctx_t *td_kctx;              /*!< (*) kernel context (switch) */
  intptr_t td_onfault;         /*!< (*) PC for copyin/copyout faults */
  kstack_t td_kstack;          /*!< (*) kernel stack structure */
  /* waiting channel */
  void *td_wchan;            /*!< (*) memory object on which thread awaits */
  const void *td_waitpt;     /*!< (*) PC where program waits */
  callout_t td_slpcallout;   /*!< (*) callout used to wakeup from sleep */
  sleepq_t *td_sleepqueue;   /*!< ($) thread's sleepqueue */
  turnstile_t *td_blocked;   /*!< (#) turnstile on which thread is blocked */
  turnstile_t *td_turnstile; /*!< (#) thread's turnstile */
  LIST_HEAD(, turnstile) td_contested; /* (#) turnstiles of locks that we own */
  /* scheduler part */
  prio_t td_base_prio; /*!< ($) base priority */
  prio_t td_prio;      /*!< ($) active priority */
  int td_slice;        /*!< ($) time slice length in system ticks */
  /* thread statistics */
  bintime_t td_rtime;        /*!< (*) time spent running */
  bintime_t td_last_rtime;   /*!< (*) time of last switch to running state */
  bintime_t td_slptime;      /*!< (*) time spent sleeping */
  bintime_t td_last_slptime; /*!< (*) time of last switch to sleep state */
  unsigned td_nctxsw;        /*!< (*) total number of context switches */
  /* signal handling */
  sigpend_t td_sigpend;   /*!< (p) Pending signals for this thread. */
  sigset_t td_sigmask;    /*!< (p) Signal mask */
  sigset_t td_oldsigmask; /*!< (*) Signal mask from before sigsuspend() */

  /* Both fields are protected by the lockdep lock */
#if LOCKDEP
  int td_lock_depth; /* The number of locks hold by the thread. */
  lock_class_t *td_held_locks[LOCKDEP_MAX_HELD_LOCKS];
#endif
} thread_t;

thread_t *thread_self(void) __no_instrument_kgprof;

/*! \brief Initialize first thread in the system. */
void init_thread0(void);

extern thread_t thread0;

/*! \brief Create a thread.
 *
 * Create a thread with given @name and priority @prio. The thread will execute
 * function @fn with an argument @arg.
 *
 * Try to follow this naming convention:
 * - use neither the `thread` word nor any synonyms like `td`,
 * - prefix kernel test thread's name with `test`,
 * - prefix user test thread's name with `utest`,
 * - use format [prefix-]module[-name[-number]].
 * Some good examples: `callout`, `test-malloc`, `test-sleepq-waiter-2`.
 */
thread_t *thread_create(const char *name, void (*fn)(void *), void *arg,
                        prio_t prio);

void thread_delete(thread_t *td);

/*! \brief Prepares thread to be launched.
 *
 * Initializes thread context so it can be resumed in such a way,
 * as if @target function was called with @arg argument.
 *
 * Such thread can be resumed either by switch or return from exception.
 */
void thread_entry_setup(thread_t *td, entry_fn_t target, void *arg);

/*! \brief Exit from a thread.
 *
 * Thread becomes zombie which resources will eventually be recycled.
 */
__noreturn void thread_exit(void);

/*! \brief Switch voluntarily to another thread. */
void thread_yield(void);

/*! \brief Find thread with matching \a id.
 *
 * \return Returned thread is locked.
 */
thread_t *thread_find(tid_t id);

/* Joins the specified thread, effectively waiting until it exits. */
void thread_join(thread_t *td);

/*! \brief Recycles dead threads.
 *
 * You do not need to call this function on your own, reaping will automatically
 * take place when convenient. The reason this function is exposed is because
 * some tests need to explicitly wait until threads are reaped before they can
 * verify test success. */
void thread_reap(void);

/*! \brief Continue stopped thread.
 *
 * Must be called with acquired td_lock. */
void thread_continue(thread_t *td);

/* Please use following functions to read state of a thread! */
static inline bool td_is_ready(thread_t *td) {
  return td->td_state == TDS_READY;
}

static inline bool td_is_dead(thread_t *td) {
  return td->td_state == TDS_DEAD;
}

static inline bool td_is_blocked(thread_t *td) {
  return td->td_state == TDS_BLOCKED;
}

static inline bool td_is_running(thread_t *td) {
  return td->td_state == TDS_RUNNING;
}

static inline bool td_is_inactive(thread_t *td) {
  return td->td_state == TDS_INACTIVE;
}

static inline bool td_is_sleeping(thread_t *td) {
  return td->td_state == TDS_SLEEPING;
}

static inline bool td_is_stopped(thread_t *td) {
  return td->td_state == TDS_STOPPED;
}

static inline bool td_is_interruptible(thread_t *td) {
  return (td->td_state == TDS_SLEEPING) && (td->td_flags & TDF_SLPINTR);
}

static inline bool td_is_borrowing(thread_t *td) {
  return td->td_flags & TDF_BORROWING;
}

#endif /* !_SYS_THREAD_H_ */
